#include "cpore/land.h"
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ *
 * Stage 3 - CREATURE.
 *
 * Terrain replaces depth as the axis that matters. Hills cost stamina, water
 * is a hazard rather than a home, and the seven rival nests are the point of
 * the stage: every encounter is impress-or-eat, both fill the same meter, and
 * they are bought out of the same DNA budget.
 *
 * Same contract as the earlier stages: no allocation, no I/O, no globals in
 * the world, RNG inside the struct.
 * ------------------------------------------------------------------ */

#define PI            3.14159265358979f
#define TARGET_FLORA  300
#define CARCASS_LIFE  30.0f
#define BREED_AT      90.0f
#define BREED_COST    50.0f
#define MUT_RATE      0.22f
#define GRAVITY       340.0f

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float ang_wrap(float a)
{
    while (a >  PI) a -= 2.0f * PI;
    while (a < -PI) a += 2.0f * PI;
    return a;
}
static inline Cp4Vec v4(float x, float y, float z) { Cp4Vec r = { x, y, z }; return r; }
static inline Cp4Vec v4sub(Cp4Vec a, Cp4Vec b) { return v4(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline float v4dot(Cp4Vec a, Cp4Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline float v4len(Cp4Vec a) { return sqrtf(v4dot(a, a)); }
/* horizontal distance: on land, "how far away" almost never means through rock */
static inline float flat2(Cp4Vec a, Cp4Vec b)
{
    float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

/* ---------------- terrain ----------------
 * A pure function of seed and position, so the world needs no storage and the
 * same seed always grows the same hills. The renderer calls it too. */

static float hash2(uint32_t seed, int x, int z)
{
    uint32_t h = seed ^ (uint32_t)(x * 374761393) ^ (uint32_t)(z * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFu) / 65535.0f;
}

static float vnoise(uint32_t seed, float x, float z)
{
    float fx = floorf(x), fz = floorf(z);
    int xi = (int)fx, zi = (int)fz;
    float xf = x - fx, zf = z - fz;
    float u = xf * xf * (3.0f - 2.0f * xf);
    float v = zf * zf * (3.0f - 2.0f * zf);
    float a = hash2(seed, xi, zi),       b = hash2(seed, xi + 1, zi);
    float c = hash2(seed, xi, zi + 1),   d = hash2(seed, xi + 1, zi + 1);
    return (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v;
}

/* Returns the ground's y coordinate, and y grows *downward* exactly as it did
 * in stage 2 where it meant depth. Keeping the axis pointing the same way in
 * both stages is what lets the stage-2 camera basis, lighting and sphere
 * impostors be reused unchanged; the cost is that "higher ground" reads as a
 * smaller number, which every comparison below has to respect. */
float cp4_height(uint32_t seed, float x, float z)
{
    /* The base offset lifts the whole map clear of the waterline, so the coast
     * is the basin's edge rather than a third of the world at random. */
    float e = 46.0f;                      /* elevation, up-positive */
    e += (vnoise(seed,        x * 0.0018f, z * 0.0018f) - 0.5f) * 110.0f;
    e += (vnoise(seed ^ 0x9E, x * 0.0055f, z * 0.0055f) - 0.5f) * 40.0f;
    e += (vnoise(seed ^ 0x51, x * 0.0160f, z * 0.0160f) - 0.5f) * 12.0f;
    /* a basin in the middle of the map, deep enough to flood, so there is
     * always somewhere low to find water and somewhere high to see from */
    float cx = (x - CP4_W * 0.5f) / (CP4_W * 0.5f);
    float cz = (z - CP4_D * 0.5f) / (CP4_D * 0.5f);
    e -= 96.0f * expf(-(cx * cx + cz * cz) * 3.1f);
    return -e;
}

/* The skyward normal, so ny is negative on flat ground. */
void cp4_normal(uint32_t seed, float x, float z, float *nx, float *ny, float *nz)
{
    const float e = 3.0f;
    float hl = cp4_height(seed, x - e, z), hr = cp4_height(seed, x + e, z);
    float hd = cp4_height(seed, x, z - e), hu = cp4_height(seed, x, z + e);
    float ax = (hr - hl), ay = -2.0f * e, az = (hu - hd);
    float l = sqrtf(ax * ax + ay * ay + az * az);
    if (l < 1e-5f) l = 1.0f;
    *nx = ax / l; *ny = ay / l; *nz = az / l;
}

/* ---------------- spawning ---------------- */

static int alloc_flora(Cp4World *w)
{
    for (int n = 0; n < CP4_MAX_FLORA; n++) {
        int i = w->flora_cursor;
        w->flora_cursor = (w->flora_cursor + 1) % CP4_MAX_FLORA;
        if (w->flora[i].type == CP4_FLORA_NONE) return i;
    }
    return -1;
}

static void spawn_bush(Cp4World *w, int i)
{
    for (int tries = 0; tries < 8; tries++) {
        float x = cp_rng_range(&w->rng, 40.0f, CP4_W - 40.0f);
        float z = cp_rng_range(&w->rng, 40.0f, CP4_D - 40.0f);
        float y = cp4_height(w->seed, x, z);
        if (y > CP4_SEA - 4.0f && tries < 7) continue;   /* nothing grows in the lake */
        Cp4Flora *f = &w->flora[i];
        f->p = v4(x, y, z);
        f->r = cp_rng_range(&w->rng, 7.0f, 14.0f);
        f->type = CP4_FLORA_BUSH;
        f->regrow = 0.0f;
        return;
    }
}

static void spawn_carcass(Cp4World *w, Cp4Vec at)
{
    int i = alloc_flora(w);
    if (i < 0) return;
    Cp4Flora *f = &w->flora[i];
    f->p = at;
    f->p.y = cp4_height(w->seed, at.x, at.z);
    f->r = cp_rng_range(&w->rng, 8.0f, 14.0f);
    f->type = CP4_FLORA_CARCASS;
    f->regrow = cp_rng_range(&w->rng, CARCASS_LIFE, CARCASS_LIFE * 2.0f);
    w->n_flora++;
}

/* The centre of the map is the bottom of the basin, which is now under water,
 * so nothing may simply spawn at 0.5,0.5. Spiral outward until the ground is
 * clear of the waterline. */
static Cp4Vec dry_spot(uint32_t seed, float x, float z, float margin)
{
    for (int i = 0; i < 64; i++) {
        float a = (float)i * 2.399963f;               /* golden angle */
        /* The basin is over 600 units across, so a search that only reaches a
         * couple of hundred never leaves the water and hands back a drowning
         * spawn - which is what killed whole seeds outright. */
        float r = 118.0f * sqrtf((float)i);
        float px = clampf(x + cosf(a) * r, 60.0f, CP4_W - 60.0f);
        float pz = clampf(z + sinf(a) * r, 60.0f, CP4_D - 60.0f);
        float y = cp4_height(seed, px, pz);
        if (y < CP4_SEA - margin) return v4(px, y, pz);
    }
    return v4(x, cp4_height(seed, x, z), z);
}

static void beast_from_genome(Cp4Beast *b)
{
    cp4_genome_stats(&b->g, &b->s);
    b->hp_max = b->s.hp_max;
    if (b->hp > b->hp_max) b->hp = b->hp_max;
}

static void spawn_beast(Cp4World *w, int i, Cp4Vec p, const Cp4Genome *g,
                        uint8_t nest, uint8_t gen)
{
    Cp4Beast *b = &w->beast[i];
    memset(b, 0, sizeof(*b));
    b->g = *g;
    beast_from_genome(b);
    b->p = p;
    b->p.y = cp4_height(w->seed, p.x, p.z) - b->s.stand;
    b->hp = b->hp_max;
    b->energy = 66.0f;
    b->stam = b->s.stamina;
    b->yaw = cp_rng_range(&w->rng, -PI, PI);
    b->phase = cp_rng_range(&w->rng, 0.0f, 6.28f);
    b->think_t = cp_rng_range(&w->rng, 0.0f, 0.5f);
    b->nest = nest;
    b->gen = gen;
    b->alive = 1;
    b->grounded = 1;
}

/* ---------------- reset ---------------- */

void cp4_world_reset(Cp4World *w, uint32_t seed, const Cp4Genome *genome)
{
    memset(w, 0, sizeof(*w));
    w->seed = seed;
    cp_rng_seed(&w->rng, seed);

    Cp4Genome g;
    if (genome) g = *genome;
    else        cp4_genome_starter(&g);
    cp4_genome_normalise(&g, CP4_GEN_BUDGET[0]);

    Cp4Beast *p = &w->player;
    memset(p, 0, sizeof(*p));
    p->g = g;
    beast_from_genome(p);
    p->p = dry_spot(seed, CP4_W * 0.5f, CP4_D * 0.5f, 30.0f);
    p->p.y -= p->s.stand;
    p->hp = p->hp_max;
    p->energy = 100.0f;
    p->stam = p->s.stamina;
    p->alive = 1;
    p->grounded = 1;

    for (int i = 0; i < CP4_MAX_FLORA; i++) w->flora[i].type = CP4_FLORA_NONE;
    for (int i = 0; i < TARGET_FLORA; i++) spawn_bush(w, i);
    w->n_flora = TARGET_FLORA;

    /* Seven rival species, each a nest with its own lineage. Founders are
     * random: nothing guarantees a neighbour is viable, and the ones that
     * cannot feed themselves die out on their own. */
    for (int k = 0; k < CP4_MAX_NESTS; k++) {
        Cp4Nest *nst = &w->nest[k];
        float a = (float)k / CP4_MAX_NESTS * 2.0f * PI + cp_rng_range(&w->rng, -0.3f, 0.3f);
        float rad = cp_rng_range(&w->rng, 380.0f, 900.0f);
        nst->p = dry_spot(seed, clampf(CP4_W * 0.5f + cosf(a) * rad, 90.0f, CP4_W - 90.0f),
                                clampf(CP4_D * 0.5f + sinf(a) * rad, 90.0f, CP4_D - 90.0f), 20.0f);
        nst->style = (uint8_t)cp_rng_int(&w->rng, CP4_STYLE_COUNT);
        if (cp_rng_f(&w->rng) < 0.5f) cp4_genome_random(&nst->g, &w->rng, CP4_GEN_BUDGET[1]);
        else cp4_genome_autodesign(&nst->g, &w->rng, CP4_GEN_BUDGET[1], nst->style);
        nst->standing = 0.0f;
        nst->alive = 1;
    }

    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        int k = i % CP4_MAX_NESTS;
        Cp4Vec at = v4(clampf(w->nest[k].p.x + cp_rng_range(&w->rng, -140.0f, 140.0f), 30.0f, CP4_W - 30.0f),
                       0.0f,
                       clampf(w->nest[k].p.z + cp_rng_range(&w->rng, -140.0f, 140.0f), 30.0f, CP4_D - 30.0f));
        spawn_beast(w, i, at, &w->nest[k].g, (uint8_t)k, 0);
        w->nest[k].members++;
    }
    w->status = CP4_RUN;
}

/* ---------------- movement ---------------- */

static void walk(Cp4World *w, Cp4Beast *b, float turn, float move, int jump, float dt)
{
    const Cp4Stats *s = &b->s;
    b->yaw = ang_wrap(b->yaw + turn * s->turn * dt);

    float fx = cosf(b->yaw), fz = sinf(b->yaw);
    float ground = cp4_height(w->seed, b->p.x, b->p.z);

    /* Slope. Climbing costs speed and stamina unless the build bought feet,
     * which is the whole reason grip is a stat rather than a constant. */
    float nx, ny, nz;
    cp4_normal(w->seed, b->p.x, b->p.z, &nx, &ny, &nz);
    float uphill = clampf(-(fx * nx + fz * nz), -1.0f, 1.0f);
    float slope_pen = 1.0f - clampf(uphill, 0.0f, 1.0f) * (1.0f - s->grip) * 0.85f;

    float drive = clampf(move, 0.0f, 1.0f);
    if (b->stam <= 0.0f) drive *= 0.35f;
    if (b->grounded) {
        b->v.x += fx * drive * s->accel * slope_pen * dt;
        b->v.z += fz * drive * s->accel * slope_pen * dt;
        if (jump) { b->v.y -= s->jump; b->grounded = 0; b->stam -= 8.0f; }
    }
    b->stam -= (drive * 5.5f + (b->grounded ? 0.0f : 2.0f)) * dt;
    b->stam = clampf(b->stam + 3.2f * dt, 0.0f, s->stamina);

    /* y grows downward, so gravity is positive and a jump is negative */
    b->v.y += GRAVITY * dt;

    /* Throttle sets the speed the animal is allowed to reach, and friction is
     * gentle enough that it actually gets there. With a stiff drag and a cap
     * that ignored the throttle, terminal speed came out around a sixth of the
     * stat and every build crawled - the speed gene may as well not have
     * existed. */
    float sp = sqrtf(b->v.x * b->v.x + b->v.z * b->v.z);
    float cap = s->speed * slope_pen * (0.28f + 0.72f * drive);
    if (sp > cap && sp > 0.001f) { b->v.x = b->v.x / sp * cap; b->v.z = b->v.z / sp * cap; }
    float fric = b->grounded ? 2.6f : 0.5f;
    b->v.x -= b->v.x * fric * dt;
    b->v.z -= b->v.z * fric * dt;

    b->p.x = clampf(b->p.x + b->v.x * dt, 12.0f, CP4_W - 12.0f);
    b->p.z = clampf(b->p.z + b->v.z * dt, 12.0f, CP4_D - 12.0f);
    b->p.y += b->v.y * dt;

    ground = cp4_height(w->seed, b->p.x, b->p.z);
    float foot = ground - s->stand;
    if (b->p.y >= foot) {                 /* landed: y grows downward */
        b->p.y = foot;
        b->v.y = 0.0f;
        b->grounded = 1;
    } else {
        b->grounded = 0;
    }
    if (b->p.y < -CP4_SKY) b->p.y = -CP4_SKY;

    b->phase += dt * (2.0f + 9.0f * drive);
}

/* what this animal can perceive here: sight, plus hearing which does not
 * care about the ridge in the way */
static float perceive(const Cp4Beast *b) { return b->s.sight; }

/* ---------------- npcs ---------------- */

static void beast_think(Cp4World *w, Cp4Beast *b, int idx)
{
    b->think_t -= CP4_DT;
    if (b->think_t > 0.0f) return;
    b->think_t = 0.22f + 0.02f * (float)(idx % 5);

    float see = perceive(b), see2 = see * see;
    float best = 1e18f;
    b->has_target = 0;

    uint8_t eats_plant = b->s.graze_eff > 0.0f;
    uint8_t eats_meat  = b->s.carn_eff > 0.0f;

    for (int i = 0; i < CP4_MAX_FLORA; i++) {
        const Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        if (f->type == CP4_FLORA_BUSH ? !eats_plant : !eats_meat) continue;
        float d2 = flat2(f->p, b->p);
        if (d2 > see2 || d2 >= best) continue;
        best = d2; b->des = f->p; b->has_target = 1;
    }

    /* Predators hunt, and a hostile nest hunts the player specifically. The
     * hunger gate matters: without it every meat-eater on the map converged on
     * the player at once and the stage was a mobbing simulator. */
    if (eats_meat && b->s.bite + b->s.claw_dmg > 8.0f && b->energy < 62.0f) {
        const Cp4Beast *pl = &w->player;
        float host = -w->nest[b->nest % CP4_MAX_NESTS].standing;
        if (pl->alive && host > -0.2f) {
            float d2 = flat2(pl->p, b->p);
            if (d2 < see2 * (0.30f + 0.55f * (host + 1.0f) * 0.5f) && d2 < best * 4.0f) {
                best = d2; b->des = pl->p; b->has_target = 1;
            }
        }
    }

    if (!b->has_target) {
        /* drift home: a nest is a place its members come back to */
        const Cp4Nest *nst = &w->nest[b->nest % CP4_MAX_NESTS];
        b->des = dry_spot(w->seed,
                          clampf(nst->p.x + cp_rng_range(&w->rng, -180.0f, 180.0f), 60.0f, CP4_W - 60.0f),
                          clampf(nst->p.z + cp_rng_range(&w->rng, -180.0f, 180.0f), 60.0f, CP4_D - 60.0f),
                          8.0f);
        b->has_target = 1;
    }
}

static float steer_to(const Cp4Beast *b, Cp4Vec target)
{
    float dx = target.x - b->p.x, dz = target.z - b->p.z;
    if (dx * dx + dz * dz < 1e-4f) return 0.0f;
    return clampf(ang_wrap(atan2f(dz, dx) - b->yaw) * 2.4f, -1.0f, 1.0f);
}

/* ---------------- combat ---------------- */

/* Weapons reach through the arc they point down, exactly as in the earlier
 * stages - a claw on the flank hits what is beside you. */
static float melee_damage(const Cp4Beast *b, float dir_yaw)
{
    float dmg = 0.0f;
    for (int i = 0; i < CP4_MAX_PARTS; i++) {
        int t = b->g.part[i].type;
        if (t != CP4_MOUTH_C && t != CP4_MOUTH_O && t != CP4_CLAW && t != CP4_HORN) continue;
        float scale = 0.45f + 1.45f * ((float)b->g.part[i].scale / 255.0f);
        int copies = b->g.part[i].mirror ? 2 : 1;
        for (int m = 0; m < copies; m++) {
            int yu = m ? ((256 - b->g.part[i].yaw) & 0xFF) : b->g.part[i].yaw;
            float pa = b->yaw + (float)yu * (2.0f * PI / 256.0f);
            float d = fabsf(ang_wrap(pa - dir_yaw));
            float arc = (t == CP4_CLAW) ? 0.85f : 0.70f;
            if (d > arc) continue;
            float fall = 0.55f + 0.45f * (1.0f - d / arc);
            float base = (t == CP4_CLAW) ? 24.0f
                       : (t == CP4_HORN) ? 20.0f
                       : (t == CP4_MOUTH_C ? 30.0f : 18.0f);
            dmg += base * fall * scale;
        }
    }
    return dmg;
}

static void kill_beast(Cp4World *w, Cp4Beast *b)
{
    b->alive = 0;
    w->deaths++;
    if (w->nest[b->nest % CP4_MAX_NESTS].members > 0)
        w->nest[b->nest % CP4_MAX_NESTS].members--;
    spawn_carcass(w, b->p);
}

/* ---------------- census ---------------- */

static void census(Cp4World *w)
{
    int n = 0;
    float parts = 0, legs = 0, charm = 0, gen = 0;
    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        const Cp4Beast *b = &w->beast[i];
        if (!b->alive) continue;
        n++;
        parts += b->s.n_parts;
        legs  += b->s.n[CP4_LEG];
        charm += b->s.charm;
        gen   += b->gen;
    }
    w->pop = n;
    int al = 0, en = 0;
    for (int k = 0; k < CP4_MAX_NESTS; k++) {
        if (!w->nest[k].alive) continue;
        if (w->nest[k].standing >= 0.65f) al++;
        else if (w->nest[k].standing <= -0.5f) en++;
    }
    w->allies = al;
    w->enemies = en;
    if (!n) return;
    w->mean_parts = parts / n;
    w->mean_legs  = legs / n;
    w->mean_charm = charm / n;
    w->mean_gen   = gen / n;
}

/* ---------------- step ---------------- */

static int design_is_null(const float *d)
{
    for (int i = 0; i < CP4_MAX_PARTS * 4 + 2; i++) if (d[i] != 0.0f) return 0;
    return 1;
}

void cp4_world_step(Cp4World *w, const float act[CP4_ACT_DIM])
{
    if (w->status != CP4_RUN) { w->reward = 0.0f; return; }
    const float dt = CP4_DT;
    Cp4Beast *p = &w->player;
    float reward = -0.0015f;
    float dna_before = w->dna;

    w->attack_cd -= dt;
    w->sing_cd -= dt;
    if (p->sing_t > 0.0f) p->sing_t -= dt;

    walk(w, p, clampf(act[0], -1, 1), clampf(act[2], 0, 1), act[3] > 0.5f && p->grounded, dt);
    p->energy -= p->s.upkeep * dt * (0.6f + 0.7f * clampf(act[2], 0, 1));
    /* drowning: the lake is a hazard, not a habitat */
    if (cp4_height(w->seed, p->p.x, p->p.z) > CP4_SEA) p->hp -= 7.0f * dt;

    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        Cp4Beast *b = &w->beast[i];
        if (!b->alive) continue;
        beast_think(w, b, i);
        float turn = b->has_target ? steer_to(b, b->des) : 0.0f;
        /* no animal here swims: if the ground ahead is under the waterline,
         * abandon the target and head uphill */
        if (cp4_height(w->seed, b->p.x + cosf(b->yaw) * 55.0f,
                                b->p.z + sinf(b->yaw) * 55.0f) > CP4_SEA - 8.0f) {
            float nx, ny, nz;
            cp4_normal(w->seed, b->p.x, b->p.z, &nx, &ny, &nz);
            (void)ny;
            if (nx * nx + nz * nz > 1e-6f)
                turn = clampf(ang_wrap(atan2f(-nz, -nx) - b->yaw) * 2.4f, -1.0f, 1.0f);
        }
        walk(w, b, turn, 0.55f + 0.35f * (1.0f - fabsf(turn)), 0, dt);
        b->energy -= b->s.upkeep * dt;
        b->age += dt;
        if (b->atk_cd > 0.0f) b->atk_cd -= dt;
        if (cp4_height(w->seed, b->p.x, b->p.z) > CP4_SEA) b->hp -= 5.0f * dt;
    }

    /* flora decay and regrowth */
    {
        const int slice = CP4_MAX_FLORA / 6;
        const int base = (w->step % 6) * slice;
        for (int k = 0; k < slice; k++) {
            int i = base + k;
            if (i >= CP4_MAX_FLORA) break;
            if (w->flora[i].type != CP4_FLORA_CARCASS) continue;
            w->flora[i].regrow -= dt * 6.0f;
            if (w->flora[i].regrow <= 0.0f) { w->flora[i].type = CP4_FLORA_NONE; w->n_flora--; }
        }
    }

    /* ---- player feeding ---- */
    for (int i = 0; i < CP4_MAX_FLORA; i++) {
        Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        float rr = p->s.radius + f->r + 10.0f;
        if (flat2(f->p, p->p) > rr * rr) continue;
        if (f->type == CP4_FLORA_BUSH) {
            if (p->s.graze_eff <= 0.0f) continue;
            w->dna += 0.75f * p->s.graze_eff;
            p->energy = clampf(p->energy + 9.0f, 0.0f, 170.0f);
            w->ate_plant++;
        } else {
            if (p->s.carn_eff <= 0.0f) continue;
            w->dna += 1.60f * p->s.carn_eff;
            p->energy = clampf(p->energy + 17.0f, 0.0f, 170.0f);
            w->ate_meat++;
        }
        p->hp = clampf(p->hp + 5.0f, 0.0f, p->hp_max);
        reward += 0.05f;
        f->type = CP4_FLORA_NONE;
        w->n_flora--;
    }

    /* ---- npc feeding, breeding, dying ---- */
    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        Cp4Beast *b = &w->beast[i];
        if (!b->alive) continue;
        for (int j = 0; j < CP4_MAX_FLORA; j++) {
            Cp4Flora *f = &w->flora[j];
            if (f->type == CP4_FLORA_NONE) continue;
            if (f->type == CP4_FLORA_BUSH ? b->s.graze_eff <= 0.0f
                                          : b->s.carn_eff <= 0.0f) continue;
            float rr = b->s.radius + f->r + 8.0f;
            if (flat2(f->p, b->p) > rr * rr) continue;
            b->energy += (f->type == CP4_FLORA_BUSH ? 24.0f : 40.0f)
                       * (f->type == CP4_FLORA_BUSH ? b->s.graze_eff : b->s.carn_eff);
            f->type = CP4_FLORA_NONE;
            w->n_flora--;
        }

        if (b->energy >= BREED_AT) {
            int slot = -1;
            for (int k = 0; k < CP4_MAX_BEASTS; k++)
                if (!w->beast[k].alive) { slot = k; break; }
            if (slot >= 0) {
                Cp4Genome child = b->g;
                cp4_genome_mutate(&child, &w->rng, CP4_GEN_BUDGET[1], MUT_RATE);
                Cp4Vec at = v4(clampf(b->p.x + cp_rng_range(&w->rng, -40.0f, 40.0f), 20.0f, CP4_W - 20.0f),
                               0.0f,
                               clampf(b->p.z + cp_rng_range(&w->rng, -40.0f, 40.0f), 20.0f, CP4_D - 20.0f));
                spawn_beast(w, slot, at, &child, b->nest, (uint8_t)(b->gen < 250 ? b->gen + 1 : 250));
                w->beast[slot].energy = BREED_COST * 0.8f;
                w->nest[b->nest % CP4_MAX_NESTS].members++;
                b->energy -= BREED_COST;
                w->births++;
            }
        }
        /* bounded lifespan, for the same reason as stage 2: without it
         * generations barely turn over inside an episode */
        if (b->age > 36.0f && cp_rng_f(&w->rng) < 0.02f) { kill_beast(w, b); continue; }
        if (b->energy <= 0.0f || b->hp <= 0.0f) kill_beast(w, b);
    }

    /* ---- the fork: impress or eat ---- */
    int attacking = (act[4] > 0.5f && w->attack_cd <= 0.0f);
    int singing   = (act[5] > 0.5f && w->sing_cd <= 0.0f && p->s.charm > 0.0f);

    if (singing) {
        w->sing_cd = 1.1f;
        p->sing_t = 0.6f;              /* the throat sac inflates on the call */
        w->songs++;
        p->stam -= 4.0f;
        float reach = p->s.social_reach;
        for (int i = 0; i < CP4_MAX_BEASTS; i++) {
            Cp4Beast *b = &w->beast[i];
            if (!b->alive) continue;
            float d2 = flat2(b->p, p->p);
            if (d2 > reach * reach) continue;
            Cp4Nest *nst = &w->nest[b->nest % CP4_MAX_NESTS];
            float before = nst->standing;
            /* charm is resisted by the audience's own display: impressing a
             * showy species is harder than impressing a drab one */
            float gain = 0.055f * p->s.charm / (1.0f + b->s.charm * 0.55f);
            nst->standing = clampf(nst->standing + gain, -1.0f, 1.0f);
            if (before < 0.65f && nst->standing >= 0.65f) {
                nst->befriended = 1;
                w->befriended++;
                w->dna += 18.0f;         /* a whole species won over */
                reward += 6.0f;
            } else {
                w->dna += gain * 6.0f;
            }
        }
    }

    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        Cp4Beast *b = &w->beast[i];
        if (!b->alive) continue;
        float rr = p->s.radius + b->s.radius + 14.0f;
        float d2 = flat2(b->p, p->p);
        if (d2 > rr * rr) continue;
        float dist = sqrtf(d2);
        if (dist < 0.001f) continue;
        float dir = atan2f(b->p.z - p->p.z, b->p.x - p->p.x);

        /* push apart */
        float sep = (rr - dist) * 0.5f;
        p->p.x -= cosf(dir) * sep; p->p.z -= sinf(dir) * sep;
        b->p.x += cosf(dir) * sep; b->p.z += sinf(dir) * sep;

        if (attacking) {
            float dmg = melee_damage(p, dir);
            if (dmg > 0.0f) {
                w->attack_cd = 0.45f;
                float applied = dmg * (1.0f - b->s.armor);
                b->hp -= applied;
                w->dmg_dealt += applied;
                /* violence has a diplomatic cost - the nest remembers */
                Cp4Nest *nst = &w->nest[b->nest % CP4_MAX_NESTS];
                nst->standing = clampf(nst->standing - 0.16f, -1.0f, 1.0f);
                if (b->hp <= 0.0f) {
                    nst->eaten++;
                    kill_beast(w, b);
                    w->kills++;
                    w->dna += 9.0f;
                    reward += 1.4f;
                    continue;
                }
            }
        }
        /* Who hits back. A species you have not wronged and that does not eat
         * meat has no reason to maul you for walking past, and while every
         * neutral nest did exactly that a peaceful build could not survive its
         * first encounter - the stage had only one viable answer, which is the
         * opposite of the point. Predators still hunt regardless. */
        Cp4Nest *nst = &w->nest[b->nest % CP4_MAX_NESTS];
        int wronged  = nst->standing < -0.15f;
        int predator = b->s.carn_eff > 0.0f && (b->s.bite + b->s.claw_dmg) > 8.0f;
        if ((wronged || predator) && b->atk_cd <= 0.0f) {
            float in = melee_damage(b, dir + PI);
            if (in > 0.0f) {
                /* Discrete blows on a cooldown, the same deal the player gets.
                 * Applying damage every frame of contact meant a two-second
                 * scuffle cost more health than a build could carry, and the
                 * only survivable tactic was never to meet anything. */
                b->atk_cd = 0.85f;
                float take = in * (1.0f - p->s.armor) * 0.30f;
                p->hp -= take;
                w->dmg_taken += take;
                w->hits_taken++;
            }
        }
    }

    /* regrow the world */
    for (int k = 0; k < 3 && w->n_flora < TARGET_FLORA; k++) {
        int slot = alloc_flora(w);
        if (slot < 0) break;
        spawn_bush(w, slot);
        w->n_flora++;
    }
    if ((w->step & 63) == 0) {
        census(w);
        if (w->pop < 8) {
            for (int i = 0; i < CP4_MAX_BEASTS && w->pop < 16; i++) {
                if (w->beast[i].alive) continue;
                int k = i % CP4_MAX_NESTS;
                Cp4Vec at = v4(clampf(w->nest[k].p.x + cp_rng_range(&w->rng, -120.0f, 120.0f), 30.0f, CP4_W - 30.0f),
                               0.0f,
                               clampf(w->nest[k].p.z + cp_rng_range(&w->rng, -120.0f, 120.0f), 30.0f, CP4_D - 30.0f));
                Cp4Genome fg = w->nest[k].g;
                cp4_genome_mutate(&fg, &w->rng, CP4_GEN_BUDGET[1], 0.3f);
                spawn_beast(w, i, at, &fg, (uint8_t)k, 0);
                w->nest[k].members++;
                w->pop++;
            }
        }
    }

    p->hp -= p->s.upkeep * 0.3f * dt;
    if (p->energy <= 0.0f) p->hp -= 6.0f * dt;
    reward += 0.5f * (w->dna - dna_before);

    /* ---- generations ---- */
    {
        float seg = CP4_DNA_GOAL / (float)CP4_GENERATIONS;
        int want = (int)(w->dna / seg);
        if (want > CP4_GENERATIONS - 1) want = CP4_GENERATIONS - 1;
        if (want > w->generation) {
            w->generation = want;
            const float *design = act + CP4_ACT_CTRL;
            Cp4Genome g;
            if (design_is_null(design)) {
                int style = (p->s.carn_eff > p->s.graze_eff) ? CP4_STYLE_PREDATOR
                          : (p->s.charm > 0.6f)              ? CP4_STYLE_CHARMER
                                                             : CP4_STYLE_GRAZER;
                cp4_genome_autodesign(&g, &w->rng, CP4_GEN_BUDGET[want], style);
            } else {
                cp4_genome_from_action(&g, design, CP4_GEN_BUDGET[want]);
            }
            p->g = g;
            beast_from_genome(p);
            p->hp = p->hp_max;
            p->stam = p->s.stamina;
            p->energy = clampf(p->energy + 40.0f, 0.0f, 170.0f);
            reward += 2.0f;
        }
    }

    w->step++;
    if ((w->step & 31) == 0) census(w);

    if (p->hp <= 0.0f) {
        p->hp = 0.0f; p->alive = 0;
        w->status = CP4_DEAD;
        reward -= 5.0f;
    } else if (w->dna >= CP4_DNA_GOAL) {
        w->status = CP4_EVOLVED;
        reward += 25.0f;
    } else if (w->step >= CP4_MAX_STEPS) {
        w->status = CP4_TIMEOUT;
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

void cp4_world_observe(const Cp4World *w, float *o)
{
    const Cp4Beast *p = &w->player;
    const Cp4Stats *s = &p->s;
    int k = 0;
    float fx = cosf(p->yaw), fz = sinf(p->yaw);
    float rx = -fz, rz = fx;
    float see = perceive(p), see2 = see * see;
    float ground = cp4_height(w->seed, p->p.x, p->p.z);

    o[k++] = clampf((p->v.x * fx + p->v.z * fz) / s->speed, -2.0f, 2.0f);
    o[k++] = clampf((p->v.x * rx + p->v.z * rz) / s->speed, -2.0f, 2.0f);
    o[k++] = clampf(-p->v.y / 300.0f, -2.0f, 2.0f);
    o[k++] = p->hp / p->hp_max;
    o[k++] = clampf(p->energy / 170.0f, 0.0f, 1.0f);
    o[k++] = clampf(p->stam / (s->stamina > 1.0f ? s->stamina : 1.0f), 0.0f, 1.0f);
    o[k++] = w->dna / CP4_DNA_GOAL;
    o[k++] = (float)w->step / (float)CP4_MAX_STEPS;
    o[k++] = (float)w->generation / (float)(CP4_GENERATIONS - 1);
    o[k++] = p->grounded ? 1.0f : 0.0f;
    o[k++] = w->attack_cd <= 0.0f ? 1.0f : 0.0f;
    o[k++] = w->sing_cd <= 0.0f ? 1.0f : 0.0f;
    o[k++] = clampf(-ground / 200.0f, -1.0f, 2.0f);          /* elevation */
    o[k++] = clampf((CP4_SEA - ground) / 60.0f, -2.0f, 2.0f); /* height over water */
    {   /* slope ahead: the terrain's own gradient, in the body frame */
        float nx, ny, nz;
        cp4_normal(w->seed, p->p.x + fx * 30.0f, p->p.z + fz * 30.0f, &nx, &ny, &nz);
        o[k++] = clampf(-(fx * nx + fz * nz) * 3.0f, -2.0f, 2.0f);
        o[k++] = clampf(-ny, 0.0f, 1.0f);                     /* 1 on the flat */
    }
    o[k++] = clampf((float)w->allies / (float)CP4_MAX_NESTS, 0.0f, 1.0f);
    o[k++] = clampf((float)w->enemies / (float)CP4_MAX_NESTS, 0.0f, 1.0f);

    float fd[CP4_OBS_FLORA_K]; int fi_[CP4_OBS_FLORA_K];
    for (int i = 0; i < CP4_OBS_FLORA_K; i++) { fd[i] = 1e18f; fi_[i] = -1; }
    for (int i = 0; i < CP4_MAX_FLORA; i++) {
        const Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        if (f->type == CP4_FLORA_BUSH ? s->graze_eff <= 0.0f : s->carn_eff <= 0.0f) continue;
        float d2 = flat2(f->p, p->p);
        if (d2 > see2) continue;
        topk(fd, fi_, CP4_OBS_FLORA_K, d2, i);
    }
    for (int i = 0; i < CP4_OBS_FLORA_K; i++) {
        if (fi_[i] < 0) { for (int z = 0; z < 5; z++) o[k++] = 0.0f; continue; }
        const Cp4Flora *f = &w->flora[fi_[i]];
        float dx = f->p.x - p->p.x, dz = f->p.z - p->p.z;
        o[k++] = clampf((dx * fx + dz * fz) / 300.0f, -2.5f, 2.5f);
        o[k++] = clampf((dx * rx + dz * rz) / 300.0f, -2.5f, 2.5f);
        o[k++] = clampf((f->p.y - ground) / 80.0f, -2.5f, 2.5f);
        o[k++] = f->type == CP4_FLORA_BUSH ? 1.0f : 0.0f;
        o[k++] = f->type == CP4_FLORA_CARCASS ? 1.0f : 0.0f;
    }

    float cd[CP4_OBS_BEAST_K]; int ci[CP4_OBS_BEAST_K];
    for (int i = 0; i < CP4_OBS_BEAST_K; i++) { cd[i] = 1e18f; ci[i] = -1; }
    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        const Cp4Beast *b = &w->beast[i];
        if (!b->alive) continue;
        float d2 = flat2(b->p, p->p);
        /* things you cannot see you may still hear */
        float reach = (d2 < s->hearing * s->hearing) ? s->hearing : see;
        if (d2 > reach * reach) continue;
        topk(cd, ci, CP4_OBS_BEAST_K, d2, i);
    }
    for (int i = 0; i < CP4_OBS_BEAST_K; i++) {
        if (ci[i] < 0) { for (int z = 0; z < 8; z++) o[k++] = 0.0f; continue; }
        const Cp4Beast *b = &w->beast[ci[i]];
        float dx = b->p.x - p->p.x, dz = b->p.z - p->p.z;
        o[k++] = clampf((dx * fx + dz * fz) / 400.0f, -2.5f, 2.5f);
        o[k++] = clampf((dx * rx + dz * rz) / 400.0f, -2.5f, 2.5f);
        o[k++] = clampf((b->p.y - p->p.y) / 100.0f, -2.5f, 2.5f);
        o[k++] = clampf(b->s.radius / s->radius - 1.0f, -2.5f, 2.5f);
        o[k++] = clampf((b->s.bite + b->s.claw_dmg) / 60.0f, 0.0f, 2.5f);
        o[k++] = clampf(b->s.charm / 3.0f, 0.0f, 2.5f);
        o[k++] = w->nest[b->nest % CP4_MAX_NESTS].standing;
        o[k++] = clampf(b->hp / b->hp_max, 0.0f, 1.0f);
    }

    for (int t = 1; t < CP4_PART_COUNT; t++) o[k++] = s->n[t] * 0.25f;

    o[k++] = clampf(s->graze_eff * 0.5f, 0.0f, 2.5f);
    o[k++] = clampf(s->carn_eff * 0.5f, 0.0f, 2.5f);
    o[k++] = clampf(s->speed / 250.0f, 0.0f, 2.5f);
    o[k++] = s->armor;
    o[k++] = clampf((s->bite + s->claw_dmg) / 60.0f, 0.0f, 2.5f);
    o[k++] = clampf(s->charm / 3.0f, 0.0f, 2.5f);
    o[k++] = clampf((float)s->cost / 170.0f, 0.0f, 2.5f);
}

/* ---------------- scripted baseline ---------------- */

void cp4_policy_greedy(const Cp4World *w, float act[CP4_ACT_DIM])
{
    const Cp4Beast *p = &w->player;
    const Cp4Stats *s = &p->s;
    memset(act, 0, sizeof(float) * CP4_ACT_DIM);

    float see = perceive(p), see2 = see * see;
    Cp4Vec target = p->p;
    int have = 0, attack = 0, sing = 0;
    float best = 1e18f;

    /* Which half of the stage this build is equipped for. A charmer that
     * tries to fight loses, and a predator that tries to sing is ignored. */
    /* one voice sac is all a starting budget can afford, so the threshold
     * has to sit below what one voice sac buys */
    int charmer = (s->charm > 0.35f);

    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        const Cp4Beast *b = &w->beast[i];
        if (!b->alive) continue;
        float d2 = flat2(b->p, p->p);
        if (d2 > see2) continue;
        float dist = sqrtf(d2);
        const Cp4Nest *nst = &w->nest[b->nest % CP4_MAX_NESTS];

        if (charmer) {
            /* work on whoever is closest to being won over but not there yet */
            if (nst->standing < 0.65f && d2 < best) { best = d2; target = b->p; have = 1; }
            if (dist < s->social_reach * 0.8f && nst->standing < 0.65f) sing = 1;
        } else if (s->carn_eff > 0.0f && s->bite + s->claw_dmg > 8.0f) {
            if (p->s.radius > b->s.radius * 0.85f && d2 < best) {
                best = d2; target = b->p; have = 1;
                /* Commit before contact. The separation push parks bodies at
                 * exactly radius+radius+14, so a trigger range inside that is
                 * never reached and the baseline landed literally zero blows
                 * in nine thousand steps. */
                if (dist < p->s.radius + b->s.radius + 26.0f) attack = 1;
            }
        }
    }

    for (int i = 0; i < CP4_MAX_FLORA; i++) {
        const Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        if (f->type == CP4_FLORA_BUSH ? s->graze_eff <= 0.0f : s->carn_eff <= 0.0f) continue;
        float d2 = flat2(f->p, p->p);
        if (d2 > see2 || d2 >= best) continue;
        best = d2; target = f->p; have = 1;
    }

    /* Run from what you cannot fight.
     *
     * Without this the baseline walked to the nearest bush while something ate
     * it, which is not a strategy so much as an absence of one - half the
     * herbivore runs ended in the first minute. Steering is by direction, not
     * at a waypoint, for the same reason it is in stage 2: a fixed flee point
     * lands outside the world and pins the animal against a boundary. */
    float flee_x = 0.0f, flee_z = 0.0f;
    {
        float own = s->bite + s->claw_dmg;
        for (int i = 0; i < CP4_MAX_BEASTS; i++) {
            const Cp4Beast *b = &w->beast[i];
            if (!b->alive) continue;
            const Cp4Nest *nst = &w->nest[b->nest % CP4_MAX_NESTS];
            float threat = b->s.bite + b->s.claw_dmg;
            if (threat < 9.0f) continue;                 /* harmless */
            if (nst->standing >= 0.5f) continue;         /* friends */
            if (own > threat * 1.15f) continue;          /* you are the danger */
            float dx = p->p.x - b->p.x, dz = p->p.z - b->p.z;
            float d2 = dx * dx + dz * dz;
            if (d2 > 210.0f * 210.0f || d2 < 1e-4f) continue;
            float wgt = (210.0f - sqrtf(d2)) / 210.0f;
            float inv = 1.0f / sqrtf(d2);
            flee_x += dx * inv * wgt;
            flee_z += dz * inv * wgt;
        }
    }
    int fleeing = (flee_x * flee_x + flee_z * flee_z) > 0.04f;

    float turn = 0.0f;
    if (fleeing) {
        turn = clampf(ang_wrap(atan2f(flee_z, flee_x) - p->yaw) * 2.4f, -1.0f, 1.0f);
        have = 1;
        attack = 0;
    } else if (have) turn = steer_to(p, target);
    else {
        /* nothing worth walking to: drift along the contour, which keeps the
         * baseline out of both the lake and the corners without a waypoint */
        turn = sinf((float)w->step * 0.004f + (float)w->seed * 0.001f) * 0.5f;
    }

    /* Keep out of the lake and off the map edge. Water sits at low elevation,
     * so the escape direction is simply uphill: minus the terrain normal's
     * horizontal part. Steering by direction rather than at a fixed waypoint
     * is what stopped the stage-2 baseline pinning itself in a corner. */
    float ahead_x = p->p.x + cosf(p->yaw) * 70.0f;
    float ahead_z = p->p.z + sinf(p->yaw) * 70.0f;
    int wet  = cp4_height(w->seed, ahead_x, ahead_z) > CP4_SEA - 10.0f;
    int edge = ahead_x < 70.0f || ahead_x > CP4_W - 70.0f ||
               ahead_z < 70.0f || ahead_z > CP4_D - 70.0f;
    if (wet || edge) {
        float ex, ez;
        if (wet) {
            float nx, ny, nz;
            cp4_normal(w->seed, p->p.x, p->p.z, &nx, &ny, &nz);
            (void)ny;
            ex = -nx; ez = -nz;
        } else {
            ex = CP4_W * 0.5f - p->p.x;
            ez = CP4_D * 0.5f - p->p.z;
        }
        if (ex * ex + ez * ez > 1e-6f)
            turn = clampf(ang_wrap(atan2f(ez, ex) - p->yaw) * 2.4f, -1.0f, 1.0f);
    }

    act[0] = turn;
    act[1] = 0.0f;
    /* still drive hard through a turn: steer_to saturates on anything past
     * 25 degrees, so tying throttle tightly to it means standing and
     * spinning instead of walking */
    act[2] = fleeing ? 1.0f : 0.60f + 0.40f * (1.0f - fabsf(turn));
    act[3] = 0.0f;
    act[4] = attack ? 1.0f : 0.0f;
    act[5] = sing ? 1.0f : 0.0f;

    int next = w->generation + 1;
    if (next > CP4_GENERATIONS - 1) next = CP4_GENERATIONS - 1;
    int style = charmer ? CP4_STYLE_CHARMER
              : (s->carn_eff > s->graze_eff ? CP4_STYLE_PREDATOR : CP4_STYLE_GRAZER);
    Cp4Genome g;
    cp4_genome_autodesign(&g, NULL, CP4_GEN_BUDGET[next], style);
    float *d = act + CP4_ACT_CTRL;
    for (int i = 0; i < CP4_MAX_PARTS; i++) {
        d[i * 4 + 0] = (float)g.part[i].type * (2.0f / (float)(CP4_PART_COUNT - 1)) - 1.0f;
        d[i * 4 + 1] = (float)g.part[i].seg * (2.0f / (float)(CP4_MAX_SEG - 1)) - 1.0f;
        d[i * 4 + 2] = (float)g.part[i].yaw * (2.0f / 255.0f) - 1.0f;
        d[i * 4 + 3] = (float)g.part[i].pitch / 63.0f;
    }
    d[CP4_MAX_PARTS * 4]     = (float)(g.nseg - 2) * (2.0f / (float)(CP4_MAX_SEG - 2)) - 1.0f;
    d[CP4_MAX_PARTS * 4 + 1] = ((float)g.girth - 50.0f) / 95.0f - 1.0f;
}
