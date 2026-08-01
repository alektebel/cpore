#include "cpore/cpore.h"
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ *
 * Cell stage simulation.
 *
 * No allocation, no I/O, no globals: cp_world_step is a pure function of
 * (world, action). That is what makes snapshot/restore and deterministic
 * replay fall out for free.
 * ------------------------------------------------------------------ */

#define TARGET_FOOD    470
#define MEAT_LIFETIME  14.0f

static inline float clampf(float v, float a, float b)
{
    return v < a ? a : (v > b ? b : v);
}

static inline float len2(float x, float y) { return sqrtf(x * x + y * y); }

/* ---------------- spatial grid over food ---------------- */

static inline int grid_index(float x, float y)
{
    int gx = (int)(x / CP_GRID_CS);
    int gy = (int)(y / CP_GRID_CS);
    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    if (gx >= CP_GRID_W) gx = CP_GRID_W - 1;
    if (gy >= CP_GRID_H) gy = CP_GRID_H - 1;
    return gy * CP_GRID_W + gx;
}

/* Food does not move once spawned, so the lookup grid is maintained
 * incrementally instead of rebuilt every step - that rebuild was touching all
 * 640 slots per step to serve roughly ten actual insertions and removals. */
static void grid_insert(CpWorld *w, int i)
{
    int g = grid_index(w->food[i].x, w->food[i].y);
    w->grid_next[i] = w->grid_head[g];
    w->grid_head[g] = i;
}

/* NOTE: deliberately leaves grid_next[i] intact. Removal happens while a
 * GRID_FOREACH is sitting on element i, and the traversal needs that link to
 * find the rest of the bucket. The stale pointer is overwritten on reuse. */
static void grid_remove(CpWorld *w, int i)
{
    int g = grid_index(w->food[i].x, w->food[i].y);
    int32_t *pp = &w->grid_head[g];
    while (*pp >= 0) {
        if (*pp == i) { *pp = w->grid_next[i]; return; }
        pp = &w->grid_next[*pp];
    }
}

/* consume food slot i: unlink, free, and keep the population count honest */
static void food_consume(CpWorld *w, int i)
{
    grid_remove(w, i);
    w->food[i].type = CP_FOOD_NONE;
    w->n_food--;
}

/* cells do move, so their grid is rebuilt - but it is only 48 entries */
static void cgrid_build(CpWorld *w)
{
    memset(w->cgrid_head, 0xFF, sizeof(w->cgrid_head));
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        if (!w->cells[i].alive) { w->cgrid_next[i] = -1; continue; }
        int gx = (int)(w->cells[i].x / CP_CGRID_CS);
        int gy = (int)(w->cells[i].y / CP_CGRID_CS);
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        if (gx >= CP_CGRID_W) gx = CP_CGRID_W - 1;
        if (gy >= CP_CGRID_H) gy = CP_CGRID_H - 1;
        int g = gy * CP_CGRID_W + gx;
        w->cgrid_next[i] = w->cgrid_head[g];
        w->cgrid_head[g] = i;
    }
}

/* Visit every food index within `rad` grid cells of (x,y).
 * Written as a macro so the hot loops stay allocation- and callback-free. */
#define GRID_FOREACH(w, x, y, rad, IDXVAR, ...)                               \
    do {                                                                      \
        int _cx = (int)((x) / CP_GRID_CS), _cy = (int)((y) / CP_GRID_CS);     \
        int _x0 = _cx - (rad), _x1 = _cx + (rad);                             \
        int _y0 = _cy - (rad), _y1 = _cy + (rad);                             \
        if (_x0 < 0) { _x0 = 0; }                                             \
        if (_y0 < 0) { _y0 = 0; }                                             \
        if (_x1 >= CP_GRID_W) { _x1 = CP_GRID_W - 1; }                        \
        if (_y1 >= CP_GRID_H) { _y1 = CP_GRID_H - 1; }                        \
        for (int _gy = _y0; _gy <= _y1; _gy++)                                \
            for (int _gx = _x0; _gx <= _x1; _gx++)                            \
                for (int IDXVAR = (w)->grid_head[_gy * CP_GRID_W + _gx];      \
                     IDXVAR >= 0;                                             \
                     IDXVAR = (w)->grid_next[IDXVAR]) { __VA_ARGS__ }         \
    } while (0)

/* ---------------- spawning ---------------- */

static void spawn_food(CpWorld *w, int i, uint8_t type, float x, float y)
{
    CpFood *f = &w->food[i];
    f->x = x; f->y = y;
    f->type = type;
    f->r = (type == CP_FOOD_PLANT) ? cp_rng_range(&w->rng, 4.0f, 6.5f)
                                   : cp_rng_range(&w->rng, 5.0f, 8.0f);
    f->phase = (type == CP_FOOD_MEAT) ? cp_rng_range(&w->rng, MEAT_LIFETIME, MEAT_LIFETIME * 3.0f)
                                      : cp_rng_range(&w->rng, 0.0f, 6.28f);
    grid_insert(w, i);
}

/* Round-robin free-slot search. The old code restarted at index 0 on every
 * spawn, which turned corpse drops into a repeated scan of the whole array. */
static int alloc_food_slot(CpWorld *w)
{
    for (int n = 0; n < CP_MAX_FOOD; n++) {
        int i = w->food_cursor;
        w->food_cursor = (w->food_cursor + 1) % CP_MAX_FOOD;
        if (w->food[i].type == CP_FOOD_NONE) return i;
    }
    return -1;
}

/* Ambient drift: mostly plants, plus a trickle of dead plankton. Without that
 * trickle a jaws-only build has nothing to eat until it has already killed
 * something, and the carnivore branch of the editor is dead on arrival. */
#define MEAT_SHARE 0.14f

static void spawn_food_random(CpWorld *w, int i)
{
    /* plants clump: pick a patch centre from the seed stream */
    float cx = cp_rng_range(&w->rng, 60.0f, CP_WORLD_W - 60.0f);
    float cy = cp_rng_range(&w->rng, 60.0f, CP_WORLD_H - 60.0f);
    float px = clampf(cx + cp_rng_range(&w->rng, -110.0f, 110.0f), 20.0f, CP_WORLD_W - 20.0f);
    float py = clampf(cy + cp_rng_range(&w->rng, -110.0f, 110.0f), 20.0f, CP_WORLD_H - 20.0f);
    uint8_t type = cp_rng_f(&w->rng) < MEAT_SHARE ? CP_FOOD_MEAT : CP_FOOD_PLANT;
    spawn_food(w, i, type, px, py);
}

static void spawn_cell(CpWorld *w, int i, float px, float py, float difficulty)
{
    CpCell *c = &w->cells[i];
    memset(c, 0, sizeof(*c));

    float roll = cp_rng_f(&w->rng);
    c->diet = (uint8_t)(roll < 0.45f ? CP_DIET_HERB : (roll < 0.80f ? CP_DIET_CARN : CP_DIET_OMNI));

    /* a spread of sizes: mostly prey, a few things that hunt you */
    float t = cp_rng_f(&w->rng);
    t = t * t;                                   /* bias small */
    float scale = 0.45f + 1.95f * t + 0.55f * difficulty * t;

    c->r      = 13.0f * scale;
    c->hp_max = 26.0f * scale * scale;
    c->hp     = c->hp_max;
    c->speed  = (c->diet == CP_DIET_HERB ? 88.0f : 122.0f) / (0.72f + 0.34f * scale);
    c->spikes = (uint8_t)(c->diet == CP_DIET_HERB ? cp_rng_int(&w->rng, 2)
                                                  : 1 + cp_rng_int(&w->rng, 4));
    c->cilia  = (uint8_t)(2 + cp_rng_int(&w->rng, 4));
    c->attack = (c->diet == CP_DIET_HERB ? 3.0f : 10.0f) * scale + 2.2f * c->spikes;
    c->armor  = clampf(0.05f * c->spikes, 0.0f, 0.45f);

    c->x = px; c->y = py;
    c->heading  = cp_rng_range(&w->rng, 0.0f, 6.2831853f);
    c->wander_a = c->heading;
    c->wander_t = cp_rng_range(&w->rng, 0.0f, 2.0f);
    c->wander_dx = cosf(c->wander_a);
    c->wander_dy = sinf(c->wander_a);
    c->phase    = cp_rng_range(&w->rng, 0.0f, 6.2831853f);
    c->hue      = c->diet == CP_DIET_HERB ? cp_rng_range(&w->rng, 0.24f, 0.38f)
                : c->diet == CP_DIET_CARN ? cp_rng_range(&w->rng, 0.90f, 1.02f)
                                          : cp_rng_range(&w->rng, 0.72f, 0.82f);
    c->alive = 1;
}

/* respawn away from the player so nothing materialises on top of them */
static void spawn_cell_offscreen(CpWorld *w, int i, float difficulty)
{
    for (int tries = 0; tries < 8; tries++) {
        float x = cp_rng_range(&w->rng, 40.0f, CP_WORLD_W - 40.0f);
        float y = cp_rng_range(&w->rng, 40.0f, CP_WORLD_H - 40.0f);
        if (len2(x - w->player.x, y - w->player.y) > 620.0f || tries == 7) {
            spawn_cell(w, i, x, y, difficulty);
            return;
        }
    }
}

/* ---------------- reset ---------------- */

void cp_world_reset(CpWorld *w, uint32_t seed, const CpMorph *morph)
{
    memset(w, 0, sizeof(*w));
    w->seed = seed;
    cp_rng_seed(&w->rng, seed);

    if (morph) w->morph = *morph;
    else       cp_morph_default(&w->morph);
    cp_morph_clamp(&w->morph);
    cp_morph_stats(&w->morph, &w->stats);

    CpCell *p = &w->player;
    p->x = CP_WORLD_W * 0.5f;
    p->y = CP_WORLD_H * 0.5f;
    p->r = w->stats.radius0;
    p->hp = p->hp_max = w->stats.hp_max;
    p->attack = w->stats.attack;
    p->armor  = w->stats.armor;
    p->speed  = w->stats.max_speed;
    p->heading = 0.0f;
    p->spikes = w->morph.spike;
    p->cilia  = w->morph.cilia;
    p->alive  = 1;
    p->diet   = (uint8_t)(w->morph.herb && w->morph.carn ? CP_DIET_OMNI
                          : (w->morph.carn ? CP_DIET_CARN : CP_DIET_HERB));

    memset(w->grid_head, 0xFF, sizeof(w->grid_head));
    for (int i = 0; i < CP_MAX_FOOD; i++) { w->food[i].type = CP_FOOD_NONE; w->grid_next[i] = -1; }
    for (int i = 0; i < TARGET_FOOD; i++) spawn_food_random(w, i);
    w->n_food = TARGET_FOOD;

    w->n_cells = CP_MAX_CELLS;
    for (int i = 0; i < CP_MAX_CELLS; i++) spawn_cell_offscreen(w, i, 0.0f);

    cgrid_build(w);
    w->status = CP_RUN;
}

/* ---------------- npc steering ---------------- */

static void cell_think(CpWorld *w, CpCell *c, int idx, float *out_ax, float *out_ay)
{
    float ax = 0.0f, ay = 0.0f;

    /* 1. flee anything meaningfully larger that can hurt us. Cheap, and it
     *    has to be responsive, so it runs every step. */
    float threat_x = 0.0f, threat_y = 0.0f, threat_w = 0.0f;
    const CpCell *pl = &w->player;
    if (pl->alive && pl->r > c->r * 1.12f && w->stats.attack > 0.0f) {
        float dx = c->x - pl->x, dy = c->y - pl->y;
        float d = len2(dx, dy);
        if (d < 260.0f && d > 0.01f) {
            float k = (260.0f - d) / 260.0f;
            threat_x += dx / d * k; threat_y += dy / d * k; threat_w += k;
        }
    }

    /* 2. Foraging target. Re-planned in a staggered burst rather than every
     *    step: scanning the food grid for all 48 cells every frame was ~80%
     *    of the step cost, and a plankton that re-aims 15 times a second is
     *    indistinguishable from one that re-aims 60 times a second. */
    if (((w->step + idx) & 3) == 0) {
        uint8_t eats_plant = (c->diet != CP_DIET_CARN);
        uint8_t eats_meat  = (c->diet != CP_DIET_HERB);
        float best_d2 = 1e18f;

        c->has_target = 0;
        GRID_FOREACH(w, c->x, c->y, 2, fi, {
            const CpFood *f = &w->food[fi];
            if (f->type == CP_FOOD_NONE) continue;
            if (f->type == CP_FOOD_PLANT ? !eats_plant : !eats_meat) continue;
            float dx = f->x - c->x, dy = f->y - c->y;
            float d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                c->des_x = f->x; c->des_y = f->y; c->has_target = 1;
            }
        });

        /* carnivores prefer live prey over pellets */
        if (eats_meat && pl->alive && c->r > pl->r * 1.15f) {
            float dx = pl->x - c->x, dy = pl->y - c->y;
            float d2 = dx * dx + dy * dy;
            if (d2 < 340.0f * 340.0f && d2 < best_d2 * 4.0f) {
                c->des_x = pl->x; c->des_y = pl->y; c->has_target = 1;
            }
        }
    }

    if (threat_w > 0.0f) {
        float n = len2(threat_x, threat_y);
        if (n > 0.001f) { ax += threat_x / n * 1.6f; ay += threat_y / n * 1.6f; }
    } else if (c->has_target) {
        float dx = c->des_x - c->x, dy = c->des_y - c->y;
        float n = len2(dx, dy);
        if (n > 0.001f) { ax += dx / n; ay += dy / n; }
    } else {
        /* lazy wander: re-roll a heading every couple of seconds */
        c->wander_t -= CP_DT;
        if (c->wander_t <= 0.0f) {
            c->wander_t = cp_rng_range(&w->rng, 1.2f, 3.4f);
            c->wander_a += cp_rng_range(&w->rng, -1.9f, 1.9f);
            /* cached - the heading only changes a few times a second */
            c->wander_dx = cosf(c->wander_a);
            c->wander_dy = sinf(c->wander_a);
        }
        ax += c->wander_dx * 0.55f;
        ay += c->wander_dy * 0.55f;
    }

    /* soft walls */
    const float m = 90.0f;
    if (c->x < m)                ax += (m - c->x) / m * 2.0f;
    if (c->x > CP_WORLD_W - m)   ax -= (c->x - (CP_WORLD_W - m)) / m * 2.0f;
    if (c->y < m)                ay += (m - c->y) / m * 2.0f;
    if (c->y > CP_WORLD_H - m)   ay -= (c->y - (CP_WORLD_H - m)) / m * 2.0f;

    *out_ax = ax; *out_ay = ay;
}

/* ---------------- step ---------------- */

static void kill_cell(CpWorld *w, CpCell *c)
{
    c->alive = 0;
    /* leave meat behind - a corpse is food for whatever comes next */
    int chunks = 2 + (int)(c->r / 14.0f);
    if (chunks > 5) chunks = 5;
    for (int k = 0; k < chunks; k++) {
        int slot = alloc_food_slot(w);
        if (slot < 0) break;
        spawn_food(w, slot, CP_FOOD_MEAT,
                   clampf(c->x + cp_rng_range(&w->rng, -18.0f, 18.0f), 10.0f, CP_WORLD_W - 10.0f),
                   clampf(c->y + cp_rng_range(&w->rng, -18.0f, 18.0f), 10.0f, CP_WORLD_H - 10.0f));
        w->n_food++;
    }
}

void cp_world_step(CpWorld *w, const float act[CP_ACT_DIM])
{
    if (w->status != CP_RUN) { w->reward = 0.0f; return; }

    const float dt = CP_DT;
    CpCell *p = &w->player;
    const CpStats *st = &w->stats;
    float reward = -0.0015f;                  /* time is not free */
    float dna_before = w->dna;

    /* ---- player locomotion ---- */
    float ax = clampf(act[0], -1.0f, 1.0f);
    float ay = clampf(act[1], -1.0f, 1.0f);
    float an = len2(ax, ay);
    if (an > 1.0f) { ax /= an; ay /= an; an = 1.0f; }

    float boost = 1.0f;
    w->boost_cd -= dt;
    if (act[2] > 0.5f && w->morph.flag > 0 && w->boost_cd <= 0.0f) {
        boost = 2.3f;
        w->boost_cd = 1.1f;
        p->hp -= 1.2f;                        /* burst costs energy */
    }

    p->vx += ax * st->accel * boost * dt;
    p->vy += ay * st->accel * boost * dt;

    float sp = len2(p->vx, p->vy);
    float cap = st->max_speed * (boost > 1.0f ? 1.9f : 1.0f);
    if (sp > cap && sp > 0.001f) { p->vx = p->vx / sp * cap; p->vy = p->vy / sp * cap; }

    p->vx -= p->vx * st->drag * dt;
    p->vy -= p->vy * st->drag * dt;
    p->x += p->vx * dt;
    p->y += p->vy * dt;
    w->dist_travelled += len2(p->vx, p->vy) * dt;
    if (an > 0.05f) p->heading = atan2f(ay, ax);
    p->phase += dt * (4.0f + 6.0f * an);

    p->x = clampf(p->x, p->r, CP_WORLD_W - p->r);
    p->y = clampf(p->y, p->r, CP_WORLD_H - p->r);

    /* ---- npcs ---- */
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        CpCell *c = &w->cells[i];
        if (!c->alive) continue;
        float cax, cay;
        cell_think(w, c, i, &cax, &cay);
        float n = len2(cax, cay);
        if (n > 1.0f) { cax /= n; cay /= n; }

        c->vx += cax * c->speed * 3.2f * dt;
        c->vy += cay * c->speed * 3.2f * dt;
        float cs = len2(c->vx, c->vy);
        if (cs > c->speed && cs > 0.001f) { c->vx = c->vx / cs * c->speed; c->vy = c->vy / cs * c->speed; }
        c->vx -= c->vx * 2.4f * dt;
        c->vy -= c->vy * 2.4f * dt;
        c->x = clampf(c->x + c->vx * dt, c->r, CP_WORLD_W - c->r);
        c->y = clampf(c->y + c->vy * dt, c->r, CP_WORLD_H - c->r);
        c->phase += dt * (3.0f + cs * 0.02f);   /* heading is derived at draw time */
    }

    /* ---- meat decay, amortised over eight steps ----
     * Food is static, so nothing here needs to run at full rate. Each step
     * ages one eighth of the array by eight times the timestep. */
    {
        const int slice = CP_MAX_FOOD / 8;
        const int base = (w->step % 8) * slice;
        for (int k = 0; k < slice; k++) {
            int i = base + k;
            if (w->food[i].type != CP_FOOD_MEAT) continue;
            w->food[i].phase -= dt * 8.0f;
            if (w->food[i].phase <= 0.0f) food_consume(w, i);
        }
    }

    cgrid_build(w);

    /* ---- player eats ---- */
    {
        float hp_gain = 0.0f;
        GRID_FOREACH(w, p->x, p->y, 2, fi, {
            CpFood *f = &w->food[fi];
            if (f->type == CP_FOOD_NONE) continue;
            float dx = f->x - p->x, dy = f->y - p->y;
            float rr = p->r + f->r;
            if (dx * dx + dy * dy > rr * rr) continue;
            if (f->type == CP_FOOD_PLANT) {
                if (st->herb_eff <= 0.0f) continue;
                w->dna += 1.20f * st->herb_eff;
                hp_gain += 3.0f;
                w->ate_plant++;
            } else {
                if (st->carn_eff <= 0.0f) continue;
                w->dna += 4.00f * st->carn_eff;
                hp_gain += 5.0f;
                w->ate_meat++;
            }
            food_consume(w, fi);
        });
        if (hp_gain > 0.0f) {
            float before = p->hp;
            p->hp = clampf(p->hp + hp_gain, 0.0f, p->hp_max);
            reward += 0.02f * (p->hp - before);
        }
    }

    /* growth: DNA makes you physically bigger, which changes who you can fight */
    p->r = st->radius0 * (1.0f + 0.55f * (w->dna / CP_DNA_GOAL));

    /* ---- player vs npc contact ---- */
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        CpCell *c = &w->cells[i];
        if (!c->alive) continue;
        float dx = c->x - p->x, dy = c->y - p->y;
        float d2 = dx * dx + dy * dy;
        float rr = p->r + c->r;
        if (d2 > rr * rr || d2 < 1e-8f) continue;
        float d = sqrtf(d2);

        float nx = dx / d, ny = dy / d;
        float push = (rr - d) * 0.5f;
        p->x -= nx * push; p->y -= ny * push;
        c->x += nx * push; c->y += ny * push;

        /* you can only hurt what you are not dwarfed by */
        if (st->attack > 0.0f && p->r >= c->r * 0.70f) {
            c->hp -= st->attack * (1.0f - c->armor) * dt * 3.0f;
            if (c->hp <= 0.0f) {
                kill_cell(w, c);
                w->kills++;
                reward += 1.0f;
                continue;
            }
        }
        if (c->attack > 0.0f && c->r >= p->r * 0.70f) {
            float dmg = c->attack * (1.0f - st->armor) * dt * 3.0f;
            if (c->r > p->r * 1.6f && c->diet != CP_DIET_HERB) dmg *= 2.4f;  /* swallowed */
            p->hp -= dmg;
            w->hits_taken++;
        }
    }

    /* ---- npc vs npc and npc feeding (keeps the pool churning) ---- */
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        CpCell *a = &w->cells[i];
        if (!a->alive) continue;

        GRID_FOREACH(w, a->x, a->y, 1, fi, {
            CpFood *f = &w->food[fi];
            if (f->type == CP_FOOD_NONE) continue;
            if (f->type == CP_FOOD_PLANT ? a->diet == CP_DIET_CARN
                                         : a->diet == CP_DIET_HERB) continue;
            float dx = f->x - a->x, dy = f->y - a->y;
            float rr = a->r + f->r;
            if (dx * dx + dy * dy > rr * rr) continue;
            food_consume(w, fi);
            a->hp = clampf(a->hp + 4.0f, 0.0f, a->hp_max);
        });

        /* 3x3 bucket sweep; j > i keeps each pair resolved exactly once */
        int agx = (int)(a->x / CP_CGRID_CS), agy = (int)(a->y / CP_CGRID_CS);
        int gx0 = agx - 1, gx1 = agx + 1, gy0 = agy - 1, gy1 = agy + 1;
        if (gx0 < 0) gx0 = 0;
        if (gy0 < 0) gy0 = 0;
        if (gx1 >= CP_CGRID_W) gx1 = CP_CGRID_W - 1;
        if (gy1 >= CP_CGRID_H) gy1 = CP_CGRID_H - 1;

        for (int gy = gy0; gy <= gy1 && a->alive; gy++)
        for (int gx = gx0; gx <= gx1 && a->alive; gx++)
        for (int j = w->cgrid_head[gy * CP_CGRID_W + gx]; j >= 0; j = w->cgrid_next[j]) {
            if (j <= i) continue;
            CpCell *b = &w->cells[j];
            if (!b->alive) continue;
            float dx = b->x - a->x, dy = b->y - a->y;
            float d2 = dx * dx + dy * dy;
            float rr = a->r + b->r;
            if (d2 > rr * rr || d2 < 1e-8f) continue;
            float d = sqrtf(d2);
            float nx = dx / d, ny = dy / d, push = (rr - d) * 0.5f;
            a->x -= nx * push; a->y -= ny * push;
            b->x += nx * push; b->y += ny * push;
            if (a->diet != CP_DIET_HERB && a->r > b->r * 1.2f)
                b->hp -= a->attack * (1.0f - b->armor) * dt * 2.0f;
            if (b->diet != CP_DIET_HERB && b->r > a->r * 1.2f)
                a->hp -= b->attack * (1.0f - a->armor) * dt * 2.0f;
            if (b->hp <= 0.0f) kill_cell(w, b);
            if (a->hp <= 0.0f) { kill_cell(w, a); break; }
        }
    }

    /* ---- upkeep, repopulation ---- */
    p->hp -= st->upkeep * dt;

    float difficulty = w->dna / CP_DNA_GOAL;
    for (int i = 0; i < CP_MAX_CELLS; i++)
        if (!w->cells[i].alive && cp_rng_f(&w->rng) < 0.012f)
            spawn_cell_offscreen(w, i, difficulty);

    for (int k = 0; k < 3 && w->n_food < TARGET_FOOD; k++) {
        int slot = alloc_food_slot(w);
        if (slot < 0) break;
        spawn_food_random(w, slot);
        w->n_food++;
    }

    reward += 0.5f * (w->dna - dna_before);

    /* ---- termination ---- */
    w->step++;
    if (p->hp <= 0.0f) {
        p->hp = 0.0f; p->alive = 0;
        w->status = CP_DEAD;
        reward -= 5.0f;
    } else if (w->dna >= CP_DNA_GOAL) {
        w->status = CP_EVOLVED;          /* -> creature stage */
        reward += 25.0f;
    } else if (w->step >= CP_MAX_STEPS) {
        w->status = CP_TIMEOUT;
    }

    w->reward = reward;
}

/* ---------------- observation ---------------- */

/* insert (d2, payload index) into a K-sized sorted-by-distance shortlist */
static void topk_insert(float *d2s, int *idx, int k, float d2, int i)
{
    if (d2 >= d2s[k - 1]) return;
    int j = k - 1;
    while (j > 0 && d2s[j - 1] > d2) { d2s[j] = d2s[j - 1]; idx[j] = idx[j - 1]; j--; }
    d2s[j] = d2; idx[j] = i;
}

void cp_world_observe(const CpWorld *w, float *o)
{
    const CpCell *p = &w->player;
    const CpStats *st = &w->stats;
    int k = 0;

    /* --- self --- */
    o[k++] = p->vx / st->max_speed;
    o[k++] = p->vy / st->max_speed;
    o[k++] = p->hp / p->hp_max;
    o[k++] = w->dna / CP_DNA_GOAL;
    o[k++] = p->r / 60.0f;
    o[k++] = clampf(1.0f - p->x / 400.0f, 0.0f, 1.0f);
    o[k++] = clampf(1.0f - (CP_WORLD_W - p->x) / 400.0f, 0.0f, 1.0f);
    o[k++] = clampf(1.0f - p->y / 400.0f, 0.0f, 1.0f);
    o[k++] = clampf(1.0f - (CP_WORLD_H - p->y) / 400.0f, 0.0f, 1.0f);
    o[k++] = (float)w->step / (float)CP_MAX_STEPS;

    /* --- nearest food (egocentric) --- */
    float fd[CP_OBS_FOOD_K];
    int   fi_[CP_OBS_FOOD_K];
    for (int i = 0; i < CP_OBS_FOOD_K; i++) { fd[i] = 1e18f; fi_[i] = -1; }

    GRID_FOREACH(w, p->x, p->y, 3, fi, {
        const CpFood *f = &w->food[fi];
        if (f->type == CP_FOOD_NONE) continue;
        if (f->type == CP_FOOD_PLANT ? st->herb_eff <= 0.0f : st->carn_eff <= 0.0f) continue;
        float dx = f->x - p->x, dy = f->y - p->y;
        topk_insert(fd, fi_, CP_OBS_FOOD_K, dx * dx + dy * dy, fi);
    });

    for (int i = 0; i < CP_OBS_FOOD_K; i++) {
        if (fi_[i] < 0) { o[k++] = 0; o[k++] = 0; o[k++] = 0; o[k++] = 0; continue; }
        const CpFood *f = &w->food[fi_[i]];
        o[k++] = clampf((f->x - p->x) / 300.0f, -1.5f, 1.5f);
        o[k++] = clampf((f->y - p->y) / 300.0f, -1.5f, 1.5f);
        o[k++] = f->type == CP_FOOD_PLANT ? 1.0f : 0.0f;
        o[k++] = f->type == CP_FOOD_MEAT  ? 1.0f : 0.0f;
    }

    /* --- nearest cells --- */
    float cd[CP_OBS_CELL_K];
    int   ci[CP_OBS_CELL_K];
    for (int i = 0; i < CP_OBS_CELL_K; i++) { cd[i] = 1e18f; ci[i] = -1; }
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        const CpCell *c = &w->cells[i];
        if (!c->alive) continue;
        float dx = c->x - p->x, dy = c->y - p->y;
        topk_insert(cd, ci, CP_OBS_CELL_K, dx * dx + dy * dy, i);
    }
    for (int i = 0; i < CP_OBS_CELL_K; i++) {
        if (ci[i] < 0) { for (int z = 0; z < 6; z++) o[k++] = 0; continue; }
        const CpCell *c = &w->cells[ci[i]];
        o[k++] = clampf((c->x - p->x) / 400.0f, -1.5f, 1.5f);
        o[k++] = clampf((c->y - p->y) / 400.0f, -1.5f, 1.5f);
        o[k++] = clampf(c->r / p->r - 1.0f, -1.5f, 1.5f);      /* >0 = bigger than me */
        o[k++] = clampf(c->attack / 24.0f, 0.0f, 1.5f);        /* how much it hurts  */
        o[k++] = clampf(c->vx / 200.0f, -1.5f, 1.5f);
        o[k++] = clampf(c->vy / 200.0f, -1.5f, 1.5f);
    }

    /* --- own body plan: the policy must know what it is driving --- */
    o[k++] = w->morph.herb  * 0.5f;
    o[k++] = w->morph.carn  * 0.5f;
    o[k++] = w->morph.spike * 0.25f;
    o[k++] = w->morph.cilia * 0.25f;
    o[k++] = w->morph.flag  * 0.5f;
    o[k++] = w->morph.elec  * 0.5f;
}
