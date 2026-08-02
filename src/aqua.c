#include "cpore/aqua.h"
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ *
 * Stage 2 - AQUATIC.
 *
 * Same contract as stage 1: no allocation, no I/O, no globals, RNG inside the
 * world struct, so snapshot/restore and deterministic replay come for free.
 *
 * What is new is that the other animals are not props. Each carries a genome,
 * burns energy to stay alive, and breeds with mutation when it has banked
 * enough. Nothing in this file knows which body plans are good. The ones that
 * feed themselves leave more copies; that is the whole mechanism.
 * ------------------------------------------------------------------ */

#define PI            3.14159265358979f
#define TARGET_FOOD   400
#define CARRION_LIFE  26.0f
#define BREED_COST    48.0f
#define BREED_AT       85.0f
#define MUT_RATE      0.22f

/* food lookup grid over the volume */
#define GW 10
#define GH 4
#define GD 10
#define GN (GW * GH * GD)
static int32_t grid_head[GN];          /* scratch, rebuilt per call - see note */

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float ang_wrap(float a)
{
    while (a >  PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}
static inline float v3len(Cp3Vec v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }
static inline Cp3Vec v3(float x, float y, float z) { Cp3Vec r = { x, y, z }; return r; }
static inline Cp3Vec v3sub(Cp3Vec a, Cp3Vec b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline float v3dot(Cp3Vec a, Cp3Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

/* Sunlight through water. Depth is the stage's real axis: at the seabed there
 * is essentially nothing to see by unless you brought your own light. */
float cp3_daylight(float depth)
{
    return expf(-depth / 210.0f);
}

/* body frame from a yaw/pitch heading. y is down, so "up" is -y at level. */
static void basis(float yaw, float pitch, Cp3Vec *fwd, Cp3Vec *right, Cp3Vec *up)
{
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    *fwd   = v3(cp * cy, sp, cp * sy);
    *right = v3(-sy, 0.0f, cy);
    *up    = v3(cy * sp, -cp, sy * sp);
}

/* ---------------- food grid ---------------- */

static inline int gidx(Cp3Vec p)
{
    int gx = (int)(p.x / (CP3_W / GW)), gy = (int)(p.y / (CP3_H / GH)), gz = (int)(p.z / (CP3_D / GD));
    if (gx < 0) gx = 0;
    if (gy < 0) gy = 0;
    if (gz < 0) gz = 0;
    if (gx >= GW) gx = GW - 1;
    if (gy >= GH) gy = GH - 1;
    if (gz >= GD) gz = GD - 1;
    return (gy * GD + gz) * GW + gx;
}

/* The grid is rebuilt once per step into file-scope scratch rather than stored
 * in the world, which keeps CpWorld a clean POD for snapshot/restore. It is
 * 400 ints and 300 inserts - cheaper than carrying it in every saved state. */
static int32_t grid_next[CP3_MAX_FOOD];

static void grid_build(const Cp3World *w)
{
    memset(grid_head, 0xFF, sizeof(grid_head));
    for (int i = 0; i < CP3_MAX_FOOD; i++) {
        if (w->food[i].type == CP3_FOOD_NONE) { grid_next[i] = -1; continue; }
        int g = gidx(w->food[i].p);
        grid_next[i] = grid_head[g];
        grid_head[g] = i;
    }
}

#define FOOD_NEAR(w, P, RAD, IDX, ...)                                        \
    do {                                                                      \
        int _cx = (int)((P).x / (CP3_W / GW)), _cy = (int)((P).y / (CP3_H / GH)); \
        int _cz = (int)((P).z / (CP3_D / GD));                                \
        for (int _y = _cy - (RAD); _y <= _cy + (RAD); _y++) {                 \
            if (_y < 0 || _y >= GH) continue;                                 \
            for (int _z = _cz - (RAD); _z <= _cz + (RAD); _z++) {             \
                if (_z < 0 || _z >= GD) continue;                             \
                for (int _x = _cx - (RAD); _x <= _cx + (RAD); _x++) {         \
                    if (_x < 0 || _x >= GW) continue;                         \
                    for (int IDX = grid_head[(_y * GD + _z) * GW + _x];       \
                         IDX >= 0; IDX = grid_next[IDX]) { __VA_ARGS__ }      \
                }                                                             \
            }                                                                 \
        }                                                                     \
    } while (0)

/* ---------------- spawning ---------------- */

static int alloc_food(Cp3World *w)
{
    for (int n = 0; n < CP3_MAX_FOOD; n++) {
        int i = w->food_cursor;
        w->food_cursor = (w->food_cursor + 1) % CP3_MAX_FOOD;
        if (w->food[i].type == CP3_FOOD_NONE) return i;
    }
    return -1;
}

/* Plankton blooms near the light; carrion sinks. That single fact is what
 * makes depth a decision rather than a coordinate. */
static void spawn_food(Cp3World *w, int i, uint8_t type, Cp3Vec p)
{
    Cp3Food *f = &w->food[i];
    f->p = p;
    f->type = type;
    f->r = (type == CP3_FOOD_PLANKTON) ? cp_rng_range(&w->rng, 3.0f, 5.5f)
                                       : cp_rng_range(&w->rng, 5.0f, 9.0f);
    f->phase = (type == CP3_FOOD_CARRION) ? cp_rng_range(&w->rng, CARRION_LIFE, CARRION_LIFE * 2.0f)
                                          : cp_rng_range(&w->rng, 0.0f, 6.28f);
}

static void spawn_food_random(Cp3World *w, int i)
{
    float u = cp_rng_f(&w->rng);
    uint8_t type = (cp_rng_f(&w->rng) < 0.18f) ? CP3_FOOD_CARRION : CP3_FOOD_PLANKTON;
    float depth;
    if (type == CP3_FOOD_PLANKTON) depth = CP3_H * (u * u) * 0.60f;        /* shallow */
    else                           depth = CP3_H * (0.45f + 0.55f * u);    /* deep    */
    spawn_food(w, i, type, v3(cp_rng_range(&w->rng, 30.0f, CP3_W - 30.0f),
                              clampf(depth, 12.0f, CP3_H - 12.0f),
                              cp_rng_range(&w->rng, 30.0f, CP3_D - 30.0f)));
}

static void fish_from_genome(Cp3Fish *f)
{
    cp3_genome_stats(&f->g, &f->s);
    f->hp_max = f->s.hp_max;
    if (f->hp > f->hp_max) f->hp = f->hp_max;
}

static void spawn_fish(Cp3World *w, int i, Cp3Vec p, const Cp3Genome *g, uint8_t gen)
{
    Cp3Fish *f = &w->fish[i];
    memset(f, 0, sizeof(*f));
    f->g = *g;
    fish_from_genome(f);
    f->p = p;
    f->hp = f->hp_max;
    f->energy = 62.0f;
    f->yaw = cp_rng_range(&w->rng, -PI, PI);
    f->pitch = cp_rng_range(&w->rng, -0.3f, 0.3f);
    f->phase = cp_rng_range(&w->rng, 0.0f, 6.28f);
    f->think_t = cp_rng_range(&w->rng, 0.0f, 0.5f);
    f->lineage = (uint8_t)cp_rng_int(&w->rng, 6);
    f->gen = gen;
    f->alive = 1;
}

/* ---------------- reset ---------------- */

void cp3_world_reset(Cp3World *w, uint32_t seed, const Cp3Genome *genome)
{
    memset(w, 0, sizeof(*w));
    w->seed = seed;
    cp_rng_seed(&w->rng, seed);

    Cp3Genome g;
    if (genome) g = *genome;
    else        cp3_genome_starter(&g);
    cp3_genome_normalise(&g, CP3_GEN_BUDGET[0]);

    Cp3Fish *p = &w->player;
    memset(p, 0, sizeof(*p));
    p->g = g;
    fish_from_genome(p);
    p->p = v3(CP3_W * 0.5f, CP3_H * 0.30f, CP3_D * 0.5f);
    p->hp = p->hp_max;
    p->energy = 100.0f;
    p->alive = 1;

    for (int i = 0; i < CP3_MAX_FOOD; i++) w->food[i].type = CP3_FOOD_NONE;
    for (int i = 0; i < TARGET_FOOD; i++) spawn_food_random(w, i);
    w->n_food = TARGET_FOOD;

    /* Founders are drawn at random from the whole design space, including
     * body plans that cannot feed themselves. Selection is what removes
     * them - nothing here filters the starting population. */
    for (int i = 0; i < CP3_MAX_FISH; i++) {
        Cp3Genome fg;
        cp3_genome_random(&fg, &w->rng, CP3_GEN_BUDGET[1]);
        Cp3Vec pos = v3(cp_rng_range(&w->rng, 40.0f, CP3_W - 40.0f),
                        cp_rng_range(&w->rng, 30.0f, CP3_H - 30.0f),
                        cp_rng_range(&w->rng, 40.0f, CP3_D - 40.0f));
        spawn_fish(w, i, pos, &fg, 0);
    }
    w->status = CP3_RUN;
    grid_build(w);
}

/* ---------------- movement ---------------- */

static void swim(Cp3Fish *f, float turn_y, float turn_p, float thrust, float dt)
{
    const Cp3Stats *s = &f->s;
    f->yaw   = ang_wrap(f->yaw + turn_y * s->turn * dt);
    f->pitch = clampf(f->pitch + turn_p * s->turn * 0.7f * dt, -1.25f, 1.25f);

    Cp3Vec fwd, right, up;
    basis(f->yaw, f->pitch, &fwd, &right, &up);

    float th = clampf(thrust, 0.0f, 1.0f) * s->accel;
    f->v.x += fwd.x * th * dt;
    f->v.y += fwd.y * th * dt;
    f->v.z += fwd.z * th * dt;

    /* Buoyancy: lungs let an animal hold a depth cheaply. Without them it
     * sinks, and swimming back up costs thrust it could have spent hunting. */
    float neutral = CP3_H * 0.35f;
    f->v.y += (14.0f - s->buoy * 26.0f) * dt;
    f->v.y += (neutral - f->p.y) * 0.0015f * s->buoy * dt * 60.0f;

    float sp = v3len(f->v);
    if (sp > s->speed && sp > 0.001f) {
        float k = s->speed / sp;
        f->v.x *= k; f->v.y *= k; f->v.z *= k;
    }
    f->v.x -= f->v.x * s->drag * dt;
    f->v.y -= f->v.y * s->drag * dt;
    f->v.z -= f->v.z * s->drag * dt;

    f->p.x += f->v.x * dt;
    f->p.y += f->v.y * dt;
    f->p.z += f->v.z * dt;

    float m = s->radius;
    if (f->p.x < m)          { f->p.x = m;          f->v.x = fabsf(f->v.x) * 0.3f; }
    if (f->p.x > CP3_W - m)  { f->p.x = CP3_W - m;  f->v.x = -fabsf(f->v.x) * 0.3f; }
    if (f->p.z < m)          { f->p.z = m;          f->v.z = fabsf(f->v.z) * 0.3f; }
    if (f->p.z > CP3_D - m)  { f->p.z = CP3_D - m;  f->v.z = -fabsf(f->v.z) * 0.3f; }
    if (f->p.y < m)          { f->p.y = m;          f->v.y = fabsf(f->v.y) * 0.3f; }
    if (f->p.y > CP3_H - m)  { f->p.y = CP3_H - m;  f->v.y = -fabsf(f->v.y) * 0.3f; }

    f->phase += dt * (2.0f + 7.0f * clampf(thrust, 0.0f, 1.0f));
}

/* how far this animal can actually perceive, here, right now */
static float sight(const Cp3Fish *f)
{
    float lit = cp3_daylight(f->p.y) + f->s.light;
    if (lit > 1.0f) lit = 1.0f;
    return f->s.percep * (0.22f + 0.78f * lit);
}

/* ---------------- npc behaviour ---------------- */

static void fish_think(Cp3World *w, Cp3Fish *f, int idx)
{
    f->think_t -= CP3_DT;
    if (f->think_t > 0.0f) return;
    f->think_t = 0.20f + 0.02f * (float)(idx % 5);

    float see = sight(f);
    float see2 = see * see;
    float best = 1e18f;
    f->has_target = 0;

    uint8_t eats_p = f->s.filter_eff > 0.0f;
    uint8_t eats_c = f->s.carn_eff > 0.0f;

    FOOD_NEAR(w, f->p, 1, fi, {
        const Cp3Food *fd = &w->food[fi];
        if (fd->type == CP3_FOOD_NONE) continue;
        if (fd->type == CP3_FOOD_PLANKTON ? !eats_p : !eats_c) continue;
        Cp3Vec d = v3sub(fd->p, f->p);
        float d2 = v3dot(d, d);
        if (d2 > see2 || d2 >= best) continue;
        best = d2; f->des = fd->p; f->has_target = 1;
    });

    /* predators prefer something alive, including the player */
    if (eats_c && f->s.bite > 0.0f) {
        const Cp3Fish *pl = &w->player;
        if (pl->alive) {
            Cp3Vec d = v3sub(pl->p, f->p);
            float d2 = v3dot(d, d);
            if (d2 < see2 && f->s.radius > pl->s.radius * 0.85f && d2 < best * 3.0f) {
                best = d2; f->des = pl->p; f->has_target = 1;
            }
        }
        for (int j = 0; j < CP3_MAX_FISH; j += 3) {
            const Cp3Fish *o = &w->fish[(idx + j) % CP3_MAX_FISH];
            if (!o->alive || o == f) continue;
            if (o->s.radius > f->s.radius * 0.8f) continue;
            Cp3Vec d = v3sub(o->p, f->p);
            float d2 = v3dot(d, d);
            if (d2 < see2 && d2 < best) { best = d2; f->des = o->p; f->has_target = 1; }
        }
    }

    if (!f->has_target) {
        /* drift toward the depth band its diet lives in */
        float want = eats_p ? CP3_H * 0.22f : CP3_H * 0.72f;
        f->des = v3(cp_rng_range(&w->rng, 60.0f, CP3_W - 60.0f),
                    clampf(want + cp_rng_range(&w->rng, -90.0f, 90.0f), 20.0f, CP3_H - 20.0f),
                    cp_rng_range(&w->rng, 60.0f, CP3_D - 60.0f));
        f->has_target = 1;
    }
}

/* turn to face a direction, expressed as yaw/pitch rates */
static void steer_dir(const Cp3Fish *f, Cp3Vec dir, float *ty, float *tp)
{
    float len = sqrtf(v3dot(dir, dir));
    if (len < 1e-4f) { *ty = 0.0f; *tp = 0.0f; return; }
    float want_yaw = atan2f(dir.z, dir.x);
    float want_pitch = asinf(clampf(dir.y / len, -1.0f, 1.0f));
    *ty = clampf(ang_wrap(want_yaw - f->yaw) * 2.2f, -1.0f, 1.0f);
    *tp = clampf((want_pitch - f->pitch) * 2.2f, -1.0f, 1.0f);
}

/* ---------------- combat ---------------- */

/* Does this animal's mouth reach a target lying in world direction `dir`?
 * The jaw's mounted yaw/pitch rotate the bite cone off the nose, so a jaw
 * mounted on the flank hits what is beside you and nothing in front. */
static float bite_damage(const Cp3Fish *f, Cp3Vec dir)
{
    float dmg = 0.0f;
    Cp3Vec fwd, right, up;
    basis(f->yaw, f->pitch, &fwd, &right, &up);
    for (int i = 0; i < CP3_MAX_PARTS; i++) {
        int t = f->g.part[i].type;
        if (t != CP3_JAW && t != CP3_SPIKE) continue;
        float scale = 0.45f + 1.45f * ((float)f->g.part[i].scale / 255.0f);
        /* a mirrored gene is two real weapons, one on each flank */
        int copies = f->g.part[i].mirror ? 2 : 1;
        for (int m = 0; m < copies; m++) {
            int yaw_u = m ? ((256 - f->g.part[i].yaw) & 0xFF) : f->g.part[i].yaw;
            float py = (float)yaw_u * (2.0f * PI / 256.0f);
            float pp = (float)f->g.part[i].pitch * (PI / 128.0f);
            float cy = cosf(py), sy = sinf(py), cp = cosf(pp), sp = sinf(pp);
            Cp3Vec ax = v3(fwd.x * cy * cp + right.x * sy * cp + up.x * sp,
                           fwd.y * cy * cp + right.y * sy * cp + up.y * sp,
                           fwd.z * cy * cp + right.z * sy * cp + up.z * sp);
            float c = v3dot(ax, dir);
            float arc = (t == CP3_JAW) ? 0.62f : 0.75f;   /* cos threshold */
            if (c < arc) continue;
            float fall = 0.55f + 0.45f * (c - arc) / (1.0f - arc);
            dmg += (t == CP3_JAW ? f->s.bite : f->s.spike_dmg) * fall * scale;
        }
    }
    return dmg;
}

static void kill_fish(Cp3World *w, Cp3Fish *f)
{
    f->alive = 0;
    w->deaths++;
    int chunks = 1 + (int)(f->s.radius / 6.0f);
    if (chunks > 4) chunks = 4;
    for (int k = 0; k < chunks; k++) {
        int slot = alloc_food(w);
        if (slot < 0) break;
        spawn_food(w, slot, CP3_FOOD_CARRION,
                   v3(clampf(f->p.x + cp_rng_range(&w->rng, -14.0f, 14.0f), 8.0f, CP3_W - 8.0f),
                      clampf(f->p.y + cp_rng_range(&w->rng, -6.0f, 22.0f), 8.0f, CP3_H - 8.0f),
                      clampf(f->p.z + cp_rng_range(&w->rng, -14.0f, 14.0f), 8.0f, CP3_D - 8.0f)));
        w->n_food++;
    }
}

/* ---------------- population telemetry ---------------- */

static void census(Cp3World *w)
{
    int n = 0;
    float parts = 0, mouth = 0, tail = 0, light = 0, depth = 0, gen = 0;
    for (int i = 0; i < CP3_MAX_FISH; i++) {
        const Cp3Fish *f = &w->fish[i];
        if (!f->alive) continue;
        n++;
        parts += f->s.n_parts;
        mouth += f->s.n[CP3_FILTER] + f->s.n[CP3_JAW];
        tail  += f->s.n[CP3_TAIL];
        light += f->s.n[CP3_LIGHT];
        depth += f->p.y;
        gen   += f->gen;
    }
    w->pop = n;
    if (!n) return;
    w->mean_parts = parts / n;
    w->mean_mouth = mouth / n;
    w->mean_tail  = tail / n;
    w->mean_light = light / n;
    w->mean_depth = depth / n;
    w->mean_gen   = gen / n;
}

/* ---------------- step ---------------- */

static int design_is_null(const float *d)
{
    for (int i = 0; i < CP3_MAX_PARTS * 4 + 2; i++) if (d[i] != 0.0f) return 0;
    return 1;
}

void cp3_world_step(Cp3World *w, const float act[CP3_ACT_DIM])
{
    if (w->status != CP3_RUN) { w->reward = 0.0f; return; }
    const float dt = CP3_DT;
    Cp3Fish *p = &w->player;
    float reward = -0.0015f;
    float bio_before = w->biomass;

    grid_build(w);

    /* ---- player ---- */
    w->bite_cd -= dt;
    swim(p, clampf(act[0], -1, 1), clampf(act[1], -1, 1), clampf(act[2], 0, 1), dt);
    p->energy -= p->s.upkeep * dt * (0.6f + 0.8f * clampf(act[2], 0, 1));

    /* ---- npcs ---- */
    for (int i = 0; i < CP3_MAX_FISH; i++) {
        Cp3Fish *f = &w->fish[i];
        if (!f->alive) continue;
        fish_think(w, f, i);
        float ty = 0.0f, tp = 0.0f;
        if (f->has_target) {
            Cp3Vec dir = v3sub(f->des, f->p);
            float dn = sqrtf(v3dot(dir, dir));
            if (dn > 1e-4f) { dir.x /= dn; dir.y /= dn; dir.z /= dn; }
            const float M = 200.0f;
            if (f->p.x < M)         dir.x += (1.0f - f->p.x / M) * 1.4f;
            if (f->p.x > CP3_W - M) dir.x -= (1.0f - (CP3_W - f->p.x) / M) * 1.4f;
            if (f->p.z < M)         dir.z += (1.0f - f->p.z / M) * 1.4f;
            if (f->p.z > CP3_D - M) dir.z -= (1.0f - (CP3_D - f->p.z) / M) * 1.4f;
            if (f->p.y < 50.0f)         dir.y += (1.0f - f->p.y / 50.0f) * 1.4f;
            if (f->p.y > CP3_H - 50.0f) dir.y -= (1.0f - (CP3_H - f->p.y) / 50.0f) * 1.4f;
            steer_dir(f, dir, &ty, &tp);
        }
        /* Ease off the throttle through a hard turn. At full thrust the
         * turning circle is wider than the distance to the target and the
         * animal orbits its lunch forever without ever reaching it. */
        float thr = 0.30f + 0.60f * (1.0f - fabsf(ty));
        swim(f, ty, tp, thr, dt);
        f->energy -= f->s.upkeep * dt;
        f->age += dt;
    }

    /* ---- carrion decay, amortised ---- */
    {
        const int slice = CP3_MAX_FOOD / 6;
        const int base = (w->step % 6) * slice;
        for (int k = 0; k < slice; k++) {
            int i = base + k;
            if (i >= CP3_MAX_FOOD) break;
            if (w->food[i].type != CP3_FOOD_CARRION) continue;
            w->food[i].phase -= dt * 6.0f;
            if (w->food[i].phase <= 0.0f) { w->food[i].type = CP3_FOOD_NONE; w->n_food--; }
        }
    }

    /* ---- feeding: player ---- */
    {
        float gain = 0.0f;
        FOOD_NEAR(w, p->p, 1, fi, {
            Cp3Food *f = &w->food[fi];
            if (f->type == CP3_FOOD_NONE) continue;
            Cp3Vec d = v3sub(f->p, p->p);
            float rr = p->s.radius + f->r + 8.0f;
            if (v3dot(d, d) > rr * rr) continue;
            if (f->type == CP3_FOOD_PLANKTON) {
                if (p->s.filter_eff <= 0.0f) continue;
                w->biomass += 1.70f * p->s.filter_eff;
                gain += 5.0f;
                w->ate_plankton++;
            } else {
                if (p->s.carn_eff <= 0.0f) continue;
                w->biomass += 5.0f * p->s.carn_eff;
                gain += 11.0f;
                w->ate_carrion++;
            }
            f->type = CP3_FOOD_NONE;
            w->n_food--;
        });
        if (gain > 0.0f) {
            p->energy = clampf(p->energy + gain, 0.0f, 160.0f);
            p->hp = clampf(p->hp + gain * 0.4f, 0.0f, p->hp_max);
            reward += 0.01f * gain;
        }
    }

    /* ---- feeding + breeding + dying: npcs ---- */
    for (int i = 0; i < CP3_MAX_FISH; i++) {
        Cp3Fish *f = &w->fish[i];
        if (!f->alive) continue;

        FOOD_NEAR(w, f->p, 1, fi, {
            Cp3Food *fd = &w->food[fi];
            if (fd->type == CP3_FOOD_NONE) continue;
            if (fd->type == CP3_FOOD_PLANKTON ? f->s.filter_eff <= 0.0f
                                              : f->s.carn_eff <= 0.0f) continue;
            Cp3Vec d = v3sub(fd->p, f->p);
            float rr = f->s.radius + fd->r + 7.0f;
            if (v3dot(d, d) > rr * rr) continue;
            f->energy += (fd->type == CP3_FOOD_PLANKTON ? 23.0f : 42.0f)
                       * (fd->type == CP3_FOOD_PLANKTON ? f->s.filter_eff : f->s.carn_eff);
            fd->type = CP3_FOOD_NONE;
            w->n_food--;
        });

        /* Breeding is the only way a genome persists, and it is paid for out
         * of the same energy pool that feeding fills. */
        if (f->energy >= BREED_AT) {
            int slot = -1;
            for (int k = 0; k < CP3_MAX_FISH; k++)
                if (!w->fish[k].alive) { slot = k; break; }
            if (slot >= 0) {
                Cp3Genome child = f->g;
                cp3_genome_mutate(&child, &w->rng, CP3_GEN_BUDGET[1], MUT_RATE);
                Cp3Vec pos = v3(clampf(f->p.x + cp_rng_range(&w->rng, -25.0f, 25.0f), 20.0f, CP3_W - 20.0f),
                                clampf(f->p.y + cp_rng_range(&w->rng, -25.0f, 25.0f), 20.0f, CP3_H - 20.0f),
                                clampf(f->p.z + cp_rng_range(&w->rng, -25.0f, 25.0f), 20.0f, CP3_D - 20.0f));
                uint8_t gen = (uint8_t)(f->gen < 250 ? f->gen + 1 : 250);
                spawn_fish(w, slot, pos, &child, gen);
                w->fish[slot].lineage = f->lineage;
                w->fish[slot].energy = BREED_COST * 0.8f;
                f->energy -= BREED_COST;
                w->births++;
            }
        }

        /* Old age. Without it the population sits at carrying capacity and
         * every birth has to wait for a violent death, so generations barely
         * turn over inside an episode and there is nothing to see. A bounded
         * lifespan makes reproduction rate - not survival alone - the thing
         * selection acts on. */
        if (f->age > 32.0f && cp_rng_f(&w->rng) < 0.02f) { kill_fish(w, f); continue; }
        if (f->energy <= 0.0f || f->hp <= 0.0f) kill_fish(w, f);
    }

    /* ---- player vs fish ---- */
    int biting = (act[3] > 0.5f);
    for (int i = 0; i < CP3_MAX_FISH; i++) {
        Cp3Fish *f = &w->fish[i];
        if (!f->alive) continue;
        Cp3Vec d = v3sub(f->p, p->p);
        float dist = v3len(d);
        float rr = p->s.radius + f->s.radius + 6.0f;
        if (dist > rr || dist < 0.001f) continue;
        Cp3Vec dir = v3(d.x / dist, d.y / dist, d.z / dist);

        float sep = (rr - dist) * 0.5f;
        p->p.x -= dir.x * sep; p->p.y -= dir.y * sep; p->p.z -= dir.z * sep;
        f->p.x += dir.x * sep; f->p.y += dir.y * sep; f->p.z += dir.z * sep;

        if (biting && w->bite_cd <= 0.0f) {
            float dmg = bite_damage(p, dir);
            if (dmg > 0.0f) {
                w->bite_cd = 0.35f;
                float applied = dmg * (1.0f - f->s.armor);
                f->hp -= applied;
                w->dmg_dealt += applied;
                if (f->hp <= 0.0f) {
                    kill_fish(w, f);
                    w->kills++;
                    reward += 1.2f;
                    continue;
                }
            }
        }
        /* their teeth work the same way */
        Cp3Vec back = v3(-dir.x, -dir.y, -dir.z);
        float in = bite_damage(f, back);
        if (in > 0.0f) {
            float take = in * (1.0f - p->s.armor) * dt * 2.2f;
            p->hp -= take;
            w->dmg_taken += take;
            w->hits_taken++;
        }
    }

    /* ---- repopulation ---- */
    for (int k = 0; k < 3 && w->n_food < TARGET_FOOD; k++) {
        int slot = alloc_food(w);
        if (slot < 0) break;
        spawn_food_random(w, slot);
        w->n_food++;
    }
    /* if the population collapses entirely, reseed from random founders so an
     * episode cannot silently become an empty ocean */
    if ((w->step & 63) == 0) {
        census(w);
        if (w->pop < 6) {
            for (int i = 0; i < CP3_MAX_FISH && w->pop < 12; i++) {
                if (w->fish[i].alive) continue;
                Cp3Genome fg;
                cp3_genome_random(&fg, &w->rng, CP3_GEN_BUDGET[1]);
                spawn_fish(w, i, v3(cp_rng_range(&w->rng, 40.0f, CP3_W - 40.0f),
                                    cp_rng_range(&w->rng, 30.0f, CP3_H - 30.0f),
                                    cp_rng_range(&w->rng, 40.0f, CP3_D - 40.0f)), &fg, 0);
                w->pop++;
            }
        }
    }

    p->hp -= p->s.upkeep * 0.35f * dt;
    if (p->energy <= 0.0f) p->hp -= 6.0f * dt;      /* starving */

    reward += 0.5f * (w->biomass - bio_before);

    /* ---- generations ---- */
    {
        float seg = CP3_BIOMASS_GOAL / (float)CP3_GENERATIONS;
        int want = (int)(w->biomass / seg);
        if (want > CP3_GENERATIONS - 1) want = CP3_GENERATIONS - 1;
        if (want > w->generation) {
            w->generation = want;
            const float *design = act + CP3_ACT_CTRL;
            Cp3Genome g;
            if (design_is_null(design)) {
                int style = (p->s.carn_eff > p->s.filter_eff) ? CP3_STYLE_HUNTER
                          : (p->s.n[CP3_LIGHT] > 0)           ? CP3_STYLE_DIVER
                                                              : CP3_STYLE_GRAZER;
                cp3_genome_autodesign(&g, &w->rng, CP3_GEN_BUDGET[want], style);
            } else {
                cp3_genome_from_action(&g, design, CP3_GEN_BUDGET[want]);
            }
            p->g = g;
            fish_from_genome(p);
            p->hp = p->hp_max;
            p->energy = clampf(p->energy + 40.0f, 0.0f, 160.0f);
            w->design_events++;
            reward += 2.0f;
        }
    }

    w->step++;
    if ((w->step & 31) == 0) census(w);

    if (p->hp <= 0.0f) {
        p->hp = 0.0f; p->alive = 0;
        w->status = CP3_DEAD;
        reward -= 5.0f;
    } else if (w->biomass >= CP3_BIOMASS_GOAL) {
        w->status = CP3_EVOLVED;
        reward += 25.0f;
    } else if (w->step >= CP3_MAX_STEPS) {
        w->status = CP3_TIMEOUT;
    }
    w->reward = reward;
}

/* ---------------- observation ---------------- */

static void topk(float *d2s, int *idx, int k, float d2, int i)
{
    if (d2 >= d2s[k - 1]) return;
    int j = k - 1;
    while (j > 0 && d2s[j - 1] > d2) { d2s[j] = d2s[j - 1]; idx[j] = idx[j - 1]; j--; }
    d2s[j] = d2; idx[j] = i;
}

void cp3_world_observe(const Cp3World *w, float *o)
{
    const Cp3Fish *p = &w->player;
    const Cp3Stats *s = &p->s;
    Cp3Vec fwd, right, up;
    basis(p->yaw, p->pitch, &fwd, &right, &up);
    int k = 0;

    float see = sight(p), see2 = see * see;

    o[k++] = clampf(v3dot(p->v, fwd) / s->speed, -2.0f, 2.0f);
    o[k++] = clampf(p->v.y / s->speed, -2.0f, 2.0f);
    o[k++] = p->hp / p->hp_max;
    o[k++] = clampf(p->energy / 160.0f, 0.0f, 1.0f);
    o[k++] = w->biomass / CP3_BIOMASS_GOAL;
    o[k++] = p->p.y / CP3_H;
    o[k++] = cp3_daylight(p->p.y);
    o[k++] = see / 640.0f;
    o[k++] = sinf(p->pitch);
    o[k++] = (float)w->step / (float)CP3_MAX_STEPS;
    o[k++] = (float)w->generation / (float)(CP3_GENERATIONS - 1);
    o[k++] = w->bite_cd <= 0.0f ? 1.0f : 0.0f;
    o[k++] = clampf(1.0f - p->p.y / 160.0f, 0.0f, 1.0f);
    o[k++] = clampf(1.0f - (CP3_H - p->p.y) / 160.0f, 0.0f, 1.0f);
    {
        float dx = p->p.x < CP3_W - p->p.x ? p->p.x : CP3_W - p->p.x;
        float dz = p->p.z < CP3_D - p->p.z ? p->p.z : CP3_D - p->p.z;
        o[k++] = clampf(1.0f - (dx < dz ? dx : dz) / 200.0f, 0.0f, 1.0f);
    }
    o[k++] = clampf(s->light, 0.0f, 1.0f);

    /* --- nearest food, in the body frame, clipped to what we can see --- */
    float fd[CP3_OBS_FOOD_K]; int fi_[CP3_OBS_FOOD_K];
    for (int i = 0; i < CP3_OBS_FOOD_K; i++) { fd[i] = 1e18f; fi_[i] = -1; }
    for (int i = 0; i < CP3_MAX_FOOD; i++) {
        const Cp3Food *f = &w->food[i];
        if (f->type == CP3_FOOD_NONE) continue;
        if (f->type == CP3_FOOD_PLANKTON ? s->filter_eff <= 0.0f : s->carn_eff <= 0.0f) continue;
        Cp3Vec d = v3sub(f->p, p->p);
        float d2 = v3dot(d, d);
        if (d2 > see2) continue;
        topk(fd, fi_, CP3_OBS_FOOD_K, d2, i);
    }
    for (int i = 0; i < CP3_OBS_FOOD_K; i++) {
        if (fi_[i] < 0) { for (int z = 0; z < 5; z++) o[k++] = 0.0f; continue; }
        Cp3Vec d = v3sub(w->food[fi_[i]].p, p->p);
        o[k++] = clampf(v3dot(d, fwd) / 300.0f, -2.5f, 2.5f);
        o[k++] = clampf(v3dot(d, right) / 300.0f, -2.5f, 2.5f);
        o[k++] = clampf(v3dot(d, up) / 300.0f, -2.5f, 2.5f);
        o[k++] = w->food[fi_[i]].type == CP3_FOOD_PLANKTON ? 1.0f : 0.0f;
        o[k++] = w->food[fi_[i]].type == CP3_FOOD_CARRION ? 1.0f : 0.0f;
    }

    /* --- nearest fish --- */
    float cd[CP3_OBS_FISH_K]; int ci[CP3_OBS_FISH_K];
    for (int i = 0; i < CP3_OBS_FISH_K; i++) { cd[i] = 1e18f; ci[i] = -1; }
    for (int i = 0; i < CP3_MAX_FISH; i++) {
        const Cp3Fish *f = &w->fish[i];
        if (!f->alive) continue;
        Cp3Vec d = v3sub(f->p, p->p);
        float d2 = v3dot(d, d);
        if (d2 > see2) continue;
        topk(cd, ci, CP3_OBS_FISH_K, d2, i);
    }
    for (int i = 0; i < CP3_OBS_FISH_K; i++) {
        if (ci[i] < 0) { for (int z = 0; z < 7; z++) o[k++] = 0.0f; continue; }
        const Cp3Fish *f = &w->fish[ci[i]];
        Cp3Vec d = v3sub(f->p, p->p);
        o[k++] = clampf(v3dot(d, fwd) / 400.0f, -2.5f, 2.5f);
        o[k++] = clampf(v3dot(d, right) / 400.0f, -2.5f, 2.5f);
        o[k++] = clampf(v3dot(d, up) / 400.0f, -2.5f, 2.5f);
        o[k++] = clampf(f->s.radius / s->radius - 1.0f, -2.5f, 2.5f);
        o[k++] = clampf(f->s.bite / 40.0f, 0.0f, 2.5f);
        o[k++] = clampf(v3len(f->v) / 200.0f, 0.0f, 2.5f);
        o[k++] = f->s.carn_eff > 0.0f ? 1.0f : 0.0f;
    }

    for (int t = 1; t < CP3_PART_COUNT; t++) o[k++] = s->n[t] * 0.25f;

    o[k++] = clampf(s->filter_eff * 0.5f, 0.0f, 2.5f);
    o[k++] = clampf(s->carn_eff * 0.5f, 0.0f, 2.5f);
    o[k++] = clampf(s->speed / 300.0f, 0.0f, 2.5f);
    o[k++] = s->armor;
    o[k++] = clampf((s->bite + s->spike_dmg) / 60.0f, 0.0f, 2.5f);
    o[k++] = clampf((float)s->cost / 150.0f, 0.0f, 2.5f);
}

/* ---------------- scripted baseline ---------------- */

void cp3_policy_greedy(const Cp3World *w, float act[CP3_ACT_DIM])
{
    const Cp3Fish *p = &w->player;
    const Cp3Stats *s = &p->s;
    memset(act, 0, sizeof(float) * CP3_ACT_DIM);

    float see = sight(p), see2 = see * see;
    Cp3Vec target = p->p;
    int have = 0;
    float best = 1e18f;
    int bite = 0;

    /* something bigger and armed nearby outweighs any meal */
    Cp3Vec flee = v3(0, 0, 0);
    float flee_w = 0.0f;
    for (int i = 0; i < CP3_MAX_FISH; i++) {
        const Cp3Fish *f = &w->fish[i];
        if (!f->alive) continue;
        Cp3Vec d = v3sub(f->p, p->p);
        float d2 = v3dot(d, d);
        if (d2 > see2) continue;
        float dist = sqrtf(d2);
        int outclassed = (s->bite > 0.0f && p->s.radius > f->s.radius * 1.15f);
        /* Fleeing anything your own size that happens to own a jaw means
         * fleeing most of the ocean, and the animal never eats. Only run from
         * something meaningfully bigger, and only once it is close. */
        if (f->s.bite > 0.0f && f->s.radius >= p->s.radius * 1.12f && !outclassed) {
            float reach = 80.0f + f->s.radius * 3.0f;
            if (dist < reach) {
                float kk = (reach - dist) / reach;
                flee.x -= d.x / dist * kk; flee.y -= d.y / dist * kk; flee.z -= d.z / dist * kk;
                flee_w += kk;
            }
        }
        if (outclassed && s->carn_eff > 0.0f && d2 < best) {
            best = d2; target = f->p; have = 1;
            if (dist < p->s.radius + f->s.radius + 8.0f) bite = 1;
        }
    }

    if (flee_w < 0.4f) {
        for (int i = 0; i < CP3_MAX_FOOD; i++) {
            const Cp3Food *f = &w->food[i];
            if (f->type == CP3_FOOD_NONE) continue;
            if (f->type == CP3_FOOD_PLANKTON ? s->filter_eff <= 0.0f
                                             : s->carn_eff <= 0.0f) continue;
            Cp3Vec d = v3sub(f->p, p->p);
            float d2 = v3dot(d, d);
            if (d2 > see2 || d2 >= best) continue;
            best = d2; target = f->p; have = 1;
        }
    }

    /* Work in directions, not target points. Aiming at "here plus the flee
     * vector times 300" happily picks a point outside the tank, and then the
     * animal swims into the corner and starves with food 200 units away. */
    Cp3Vec dir;
    if (flee_w > 0.4f) {
        dir = flee;
    } else if (have) {
        dir = v3sub(target, p->p);
    } else {
        float want = s->filter_eff > 0.0f ? CP3_H * 0.20f : CP3_H * 0.70f;
        dir = v3sub(v3(CP3_W * 0.5f, want, CP3_D * 0.5f), p->p);
    }
    {
        float n = sqrtf(v3dot(dir, dir));
        if (n > 1e-4f) { dir.x /= n; dir.y /= n; dir.z /= n; }
    }

    /* Continuous wall repulsion, zero at the trigger distance. A hard switch
     * exactly cancels a unit-length attraction and parks you on the line. */
    const float M = 240.0f;
    if (p->p.x < M)          dir.x += (1.0f - p->p.x / M) * 1.5f;
    if (p->p.x > CP3_W - M)  dir.x -= (1.0f - (CP3_W - p->p.x) / M) * 1.5f;
    if (p->p.z < M)          dir.z += (1.0f - p->p.z / M) * 1.5f;
    if (p->p.z > CP3_D - M)  dir.z -= (1.0f - (CP3_D - p->p.z) / M) * 1.5f;
    if (p->p.y < 60.0f)          dir.y += (1.0f - p->p.y / 60.0f) * 1.5f;
    if (p->p.y > CP3_H - 60.0f)  dir.y -= (1.0f - (CP3_H - p->p.y) / 60.0f) * 1.5f;

    float ty = 0.0f, tp = 0.0f;
    steer_dir(p, dir, &ty, &tp);

    act[0] = ty;
    act[1] = tp;
    /* same rule as the npcs: throttle back to turn, full ahead to flee */
    act[2] = (flee_w > 0.4f) ? 1.0f : (0.32f + 0.60f * (1.0f - fabsf(ty)));
    act[3] = bite ? 1.0f : 0.0f;

    int next = w->generation + 1;
    if (next > CP3_GENERATIONS - 1) next = CP3_GENERATIONS - 1;
    int style = (s->carn_eff > s->filter_eff) ? CP3_STYLE_HUNTER
              : (s->n[CP3_LIGHT] > 0)         ? CP3_STYLE_DIVER : CP3_STYLE_GRAZER;
    Cp3Genome g;
    cp3_genome_autodesign(&g, NULL, CP3_GEN_BUDGET[next], style);
    float *d = act + CP3_ACT_CTRL;
    for (int i = 0; i < CP3_MAX_PARTS; i++) {
        d[i * 4 + 0] = (float)g.part[i].type * (2.0f / (float)(CP3_PART_COUNT - 1)) - 1.0f;
        d[i * 4 + 1] = (float)g.part[i].seg * (2.0f / (float)(CP3_MAX_SEG - 1)) - 1.0f;
        d[i * 4 + 2] = (float)g.part[i].yaw * (2.0f / 255.0f) - 1.0f;
        d[i * 4 + 3] = (float)g.part[i].pitch / 63.0f;
    }
    d[CP3_MAX_PARTS * 4]     = (float)(g.nseg - 2) * (2.0f / (float)(CP3_MAX_SEG - 2)) - 1.0f;
    d[CP3_MAX_PARTS * 4 + 1] = ((float)g.girth - 40.0f) / 100.0f - 1.0f;
}
