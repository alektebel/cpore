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
/* One pool now feeds four media, so it has to be bigger than it was when
 * every animal ate the same bushes: at 300 a specialist saw roughly a
 * third of the food a generalist used to. */
#define TARGET_FLORA  440
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
    /* Continental scale, and the reason the world has coastlines rather than a
     * puddle. One very low-frequency field decides land from sea; the hills
     * ride on top of it. Roughly a third of the world ends up flooded, in
     * bodies thousands of units across, which is what makes fins a way to live
     * somewhere rather than a way to cross a pond. */
    float cont = vnoise(seed ^ 0xC0DEu, x * 0.00042f, z * 0.00042f);

    float e = (cont - 0.42f) * 300.0f;    /* elevation, up-positive */
    e += (vnoise(seed,        x * 0.0018f, z * 0.0018f) - 0.5f) * 90.0f;
    e += (vnoise(seed ^ 0x9E, x * 0.0055f, z * 0.0055f) - 0.5f) * 34.0f;
    e += (vnoise(seed ^ 0x51, x * 0.0160f, z * 0.0160f) - 0.5f) * 11.0f;
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

/* ---------------- climate ----------------
 *
 * Temperature and moisture, both pure functions of position like the height
 * field, so biomes cost no storage and stream for free. Their frequency is
 * deliberately lower than the terrain's: a biome should be a region you walk
 * into and out of over minutes, not a patch you cross in one stride.
 *
 * Temperature also falls with altitude, which is what puts snow on a peak in
 * the middle of a savanna and is most of what makes the skyline interesting. */
void cp4_climate(uint32_t seed, float x, float z, float *temp, float *moist)
{
    float t = vnoise(seed ^ 0x7EA1u, x * 0.00026f, z * 0.00026f);
    float m = vnoise(seed ^ 0x3B0Du, x * 0.00034f, z * 0.00034f);
    /* a second octave so the boundaries wander instead of being ellipses */
    t = t * 0.78f + vnoise(seed ^ 0x11C7u, x * 0.0011f, z * 0.0011f) * 0.22f;
    m = m * 0.78f + vnoise(seed ^ 0x55A3u, x * 0.0013f, z * 0.0013f) * 0.22f;

    /* Value noise clusters hard around 0.5, so the raw field almost never
     * reaches the hot-and-dry corner: the first survey came out 30% frozen
     * and 0.1% desert. Expanding about the midpoint is what gives all eight
     * biomes a share of the map. */
    t = 0.5f + (t - 0.5f) * 1.55f;
    m = 0.5f + (m - 0.5f) * 1.70f;

    float elev = -cp4_height(seed, x, z);
    t -= clampf((elev - 60.0f) / 190.0f, 0.0f, 1.0f) * 0.38f;   /* lapse rate */
    /* the sea is the source of all the rain there is */
    m += clampf((CP4_SEA - elev) / 120.0f, 0.0f, 1.0f) * 0.16f;

    if (temp)  *temp  = clampf(t, 0.0f, 1.0f);
    if (moist) *moist = clampf(m, 0.0f, 1.0f);
}

int cp4_biome(uint32_t seed, float x, float z)
{
    float t, m;
    cp4_climate(seed, x, z, &t, &m);
    if (t < 0.11f) return CP4_BIOME_ICE;
    if (t < 0.34f) return m > 0.45f ? CP4_BIOME_TAIGA : CP4_BIOME_TUNDRA;
    if (t > 0.72f) {
        if (m < 0.30f) return CP4_BIOME_DESERT;
        if (m > 0.66f) return CP4_BIOME_JUNGLE;
        return CP4_BIOME_SAVANNA;
    }
    if (m < 0.34f) return CP4_BIOME_SAVANNA;
    if (m > 0.60f) return CP4_BIOME_FOREST;
    return CP4_BIOME_GRASS;
}

const char *cp4_biome_name(int b)
{
    static const char *B[CP4_BIOME_COUNT] = {
        "ice", "tundra", "taiga", "forest", "grass", "savanna", "desert", "jungle"
    };
    return (b >= 0 && b < CP4_BIOME_COUNT) ? B[b] : "?";
}

/* How much a biome grows. This is the whole reason biomes are a mechanic and
 * not a paint job: crossing a desert costs you, and a jungle is worth the
 * walk. */
float cp4_fertility(int b)
{
    switch (b) {
    case CP4_BIOME_ICE:     return 0.20f;
    case CP4_BIOME_TUNDRA:  return 0.32f;
    case CP4_BIOME_TAIGA:   return 0.72f;
    case CP4_BIOME_FOREST:  return 1.00f;
    case CP4_BIOME_GRASS:   return 0.85f;
    case CP4_BIOME_SAVANNA: return 0.55f;
    case CP4_BIOME_DESERT:  return 0.16f;
    case CP4_BIOME_JUNGLE:  return 1.15f;
    default:                return 0.7f;
    }
}

/* ---------------- spawning ----------------
 *
 * The world is unbounded. Terrain is a pure function of position, so there is
 * nothing to generate ahead of the player and nothing to store behind them -
 * the only finite thing is the resident set of flora, animals and nests, which
 * is recycled from behind the player to in front of them. The world struct
 * therefore stays exactly as POD and exactly as snapshot-able as it was when
 * it was a box. */

#define RESIDENT   1250.0f     /* how far out the world is populated */
#define RECYCLE    1650.0f     /* beyond this, move it round in front */

static int alloc_flora(Cp4World *w)
{
    for (int n = 0; n < CP4_MAX_FLORA; n++) {
        int i = w->flora_cursor;
        w->flora_cursor = (w->flora_cursor + 1) % CP4_MAX_FLORA;
        if (w->flora[i].type == CP4_FLORA_NONE) return i;
    }
    return -1;
}

/* Pick a point in the resident ring around `at`. Biased outward so that most
 * of what is spawned is somewhere the player has not already stripped. */
static Cp4Vec ring_point(Cp4World *w, Cp4Vec at, float rmin, float rmax)
{
    float a = cp_rng_range(&w->rng, -PI, PI);
    float u = cp_rng_f(&w->rng);
    float r = sqrtf(rmin * rmin + u * (rmax * rmax - rmin * rmin));
    return v4(at.x + cosf(a) * r, 0.0f, at.z + sinf(a) * r);
}

/* One food per medium. Which one grows here is decided by the ground itself:
 * kelp where the terrain is drowned, tubers buried in the dry soil, bushes on
 * the surface. That is what turns a medium into somewhere worth going. */
static void spawn_flora(Cp4World *w, int i, Cp4Vec at, float rmin, float rmax)
{
    for (int tries = 0; tries < 10; tries++) {
        Cp4Vec p = ring_point(w, at, rmin, rmax);
        float y = cp4_height(w->seed, p.x, p.z);
        Cp4Flora *f = &w->flora[i];
        int drowned = y > CP4_SEA;

        /* Fertility as a rejection test rather than a density parameter: a
         * spot in a desert usually fails and the attempt lands somewhere else
         * in the ring. That is what turns a biome from a colour into a reason
         * to be somewhere - crossing sand costs you, a jungle is worth the
         * walk, and none of it needs a second flora array. */
        if (!drowned && tries < 9) {
            float fert = cp4_fertility(cp4_biome(w->seed, p.x, p.z));
            if (cp_rng_f(&w->rng) > fert) continue;
        }

        if (drowned) {
            /* Deep enough to swim in and shallow enough to hold light. The
             * lower bound matters more than it looks: kelp growing in three
             * units of water lures a finned body into the shallows, where it
             * beaches and - having spent its budget on fins rather than legs -
             * crawls at a sixth of its swimming speed until it starves. */
            if ((y < CP4_SEA + 34.0f || y > CP4_SEA + 130.0f) && tries < 9) continue;
            f->p = v4(p.x, y, p.z);
            f->r = cp_rng_range(&w->rng, 8.0f, 15.0f);
            f->type = CP4_FLORA_KELP;
        } else if (cp_rng_f(&w->rng) < 0.38f) {
            /* buried: the y is inside the soil, so only a burrower reaches it */
            f->p = v4(p.x, y + cp_rng_range(&w->rng, 8.0f, 34.0f), p.z);
            /* found by touch rather than by sight, so the reach is generous */
            f->r = cp_rng_range(&w->rng, 11.0f, 19.0f);
            f->type = CP4_FLORA_TUBER;
        } else {
            f->p = v4(p.x, y, p.z);
            f->r = cp_rng_range(&w->rng, 7.0f, 14.0f);
            f->type = CP4_FLORA_BUSH;
        }
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

/* Ground clear of the waterline, spiralling outward from a hint. Used to place
 * anything that would otherwise start its life drowning. */
static Cp4Vec dry_spot(uint32_t seed, float x, float z, float margin)
{
    for (int i = 0; i < 64; i++) {
        float a = (float)i * 2.399963f;               /* golden angle */
        /* The basin is over 600 units across, so a search that only reaches a
         * couple of hundred never leaves the water and hands back a drowning
         * spawn - which is what killed whole seeds outright. */
        float r = 118.0f * sqrtf((float)i);
        float px = x + cosf(a) * r, pz = z + sinf(a) * r;
        float y = cp4_height(seed, px, pz);
        if (y < CP4_SEA - margin) return v4(px, y, pz);
    }
    return v4(x, cp4_height(seed, x, z), z);
}

/* The opposite: water deep enough to swim in, for a body with fins and no legs
 * that would otherwise be beached at birth. Returns 1 if it actually found
 * water - the caller has to know, because dropping a swimmer at the waterline
 * on a spot that turned out to be dry buries it seventy units inside a hill. */
static int wet_spot(uint32_t seed, float x, float z, float depth, Cp4Vec *out)
{
    /* Seas are continental now, so the search has to be too: a radius that
     * only reaches a few hundred units misses the coast entirely on any seed
     * that starts inland. */
    for (int i = 0; i < 220; i++) {
        float a = (float)i * 2.399963f;
        float r = 190.0f * sqrtf((float)i);
        float px = x + cosf(a) * r, pz = z + sinf(a) * r;
        float y = cp4_height(seed, px, pz);
        if (y > CP4_SEA + depth) { *out = v4(px, y, pz); return 1; }
    }
    *out = dry_spot(seed, x, z, 10.0f);
    return 0;
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

    /* Start the animal somewhere it can actually live. A finned, legless body
     * put on a hilltop is dead before it moves, and a legged one dropped in the
     * lake is dead just as fast - so the medium the body is built for decides
     * where the run begins. */
    int aquatic = p->s.swim > 0.0f && p->s.n[CP4_LEG] == 0;
    int in_water = 0;
    if (aquatic) in_water = wet_spot(seed, CP4_W * 0.5f, CP4_D * 0.5f, 25.0f, &p->p);
    else         p->p = dry_spot(seed, CP4_W * 0.5f, CP4_D * 0.5f, 30.0f);
    /* Only float the animal if the spot really is water. The first cut set the
     * height unconditionally, so on a seed with no sea within reach the
     * fallback handed back dry land and the swimmer began the episode buried
     * inside a hill - which the burrow clamp then held it in for the whole
     * run. */
    if (in_water) p->p.y = clampf(CP4_SEA + 20.0f, CP4_SEA + 6.0f, p->p.y - 6.0f);
    else          p->p.y -= p->s.stand;
    p->hp = p->hp_max;
    p->energy = 100.0f;
    p->stam = p->s.stamina;
    p->breath = p->s.breath;
    p->alive = 1;
    p->grounded = !aquatic;

    w->anchor = p->p;
    w->home.alive = 0;

    for (int i = 0; i < CP4_MAX_FLORA; i++) w->flora[i].type = CP4_FLORA_NONE;
    {
        /* never seed more than the array holds - the first cut of the density
         * bump walked straight off the end of it */
        int n0 = TARGET_FLORA < CP4_MAX_FLORA ? TARGET_FLORA : CP4_MAX_FLORA;
        for (int i = 0; i < n0; i++) spawn_flora(w, i, w->anchor, 90.0f, RESIDENT);
        w->n_flora = n0;
    }

    /* Rival species, each a nest with its own lineage. Founders are random:
     * nothing guarantees a neighbour is viable, and the ones that cannot feed
     * themselves die out on their own. */
    for (int k = 0; k < CP4_MAX_NESTS; k++) {
        Cp4Nest *nst = &w->nest[k];
        float a = (float)k / CP4_MAX_NESTS * 2.0f * PI + cp_rng_range(&w->rng, -0.3f, 0.3f);
        float rad = cp_rng_range(&w->rng, 380.0f, 1000.0f);
        nst->p = dry_spot(seed, w->anchor.x + cosf(a) * rad,
                                w->anchor.z + sinf(a) * rad, 20.0f);
        nst->style = (uint8_t)cp_rng_int(&w->rng, CP4_STYLE_COUNT);
        if (cp_rng_f(&w->rng) < 0.5f) cp4_genome_random(&nst->g, &w->rng, CP4_GEN_BUDGET[1]);
        else cp4_genome_autodesign(&nst->g, &w->rng, CP4_GEN_BUDGET[1], nst->style);
        /* Site the nest in the medium the lineage is built for. A shoal of
         * finned animals born on a hilltop is a shoal of corpses. */
        {
            Cp4Stats ns;
            cp4_genome_stats(&nst->g, &ns);
            if (ns.swim > 0.0f && ns.n[CP4_LEG] == 0) {
                Cp4Vec wet;
                if (wet_spot(seed, nst->p.x, nst->p.z, 25.0f, &wet)) nst->p = wet;
            }
        }
        nst->standing = 0.0f;
        nst->alive = 1;
    }

    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        int k = i % CP4_MAX_NESTS;
        Cp4Vec at = v4(w->nest[k].p.x + cp_rng_range(&w->rng, -140.0f, 140.0f), 0.0f,
                       w->nest[k].p.z + cp_rng_range(&w->rng, -140.0f, 140.0f));
        spawn_beast(w, i, at, &w->nest[k].g, (uint8_t)k, 0);
        w->nest[k].members++;
    }
    w->status = CP4_RUN;
}

/* ---------------- streaming ----------------
 *
 * What keeps an unbounded world finite. Anything that has fallen a long way
 * behind the player is not simulated in place, it is moved round in front -
 * flora respawns in the resident ring, a nest that has been left behind is
 * replaced by a new species ahead. The population size, the struct size and
 * the snapshot are all unchanged; only the contents move. */
static void stream_world(Cp4World *w)
{
    const Cp4Beast *p = &w->player;
    w->anchor = p->p;

    /* flora: a sixth of the array per tick, so this costs nothing per frame */
    {
        const int slice = CP4_MAX_FLORA / 6;
        const int base = (w->step % 6) * slice;
        for (int k = 0; k < slice; k++) {
            int i = base + k;
            if (i >= CP4_MAX_FLORA) break;
            Cp4Flora *f = &w->flora[i];
            if (f->type == CP4_FLORA_NONE) continue;
            if (f->type == CP4_FLORA_CARCASS) continue;   /* those rot instead */
            if (flat2(f->p, p->p) < RECYCLE * RECYCLE) continue;
            spawn_flora(w, i, w->anchor, RESIDENT * 0.55f, RESIDENT);
        }
    }

    /* animals: an abandoned herd is not worth simulating, but its species is */
    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        Cp4Beast *b = &w->beast[i];
        if (!b->alive || b->nest == CP4_OWN_NEST) continue;
        if (flat2(b->p, p->p) < RECYCLE * RECYCLE * 2.25f) continue;
        int k = b->nest % CP4_MAX_NESTS;
        Cp4Vec at = v4(w->nest[k].p.x + cp_rng_range(&w->rng, -140.0f, 140.0f), 0.0f,
                       w->nest[k].p.z + cp_rng_range(&w->rng, -140.0f, 140.0f));
        Cp4Genome ng = w->nest[k].g;
        spawn_beast(w, i, at, &ng, (uint8_t)k, b->gen);
    }

    /* Nests. A species left far enough behind is replaced by one the player
     * has never met, which is what makes walking in a straight line worth
     * doing: the far side of the map is not the same seven neighbours. */
    for (int k = 0; k < CP4_MAX_NESTS; k++) {
        Cp4Nest *nst = &w->nest[k];
        if (!nst->alive) continue;
        if (flat2(nst->p, p->p) < RECYCLE * RECYCLE * 2.25f) continue;
        float a = cp_rng_range(&w->rng, -PI, PI);
        float rad = cp_rng_range(&w->rng, RESIDENT * 0.6f, RESIDENT);
        nst->p = dry_spot(w->seed, p->p.x + cosf(a) * rad, p->p.z + sinf(a) * rad, 20.0f);
        nst->style = (uint8_t)cp_rng_int(&w->rng, CP4_STYLE_COUNT);
        if (cp_rng_f(&w->rng) < 0.5f) cp4_genome_random(&nst->g, &w->rng, CP4_GEN_BUDGET[1]);
        else cp4_genome_autodesign(&nst->g, &w->rng, CP4_GEN_BUDGET[1], nst->style);
        nst->standing = 0.0f;
        nst->befriended = 0;
        nst->seen = 0;
    }
}

/* ---------------- movement ---------------- */

/* Which medium the animal is in, decided by geometry alone.
 *
 * y grows downward, so: below the terrain surface is underground, between the
 * waterline and a drowned surface is water, well above the surface is air, and
 * everything else is the ground it is standing on. Nothing here consults the
 * genome - a body without fins can still be in the water, it simply cannot do
 * anything useful there, which is exactly the point. */
static int medium_at(const Cp4World *w, const Cp4Beast *b, float ground)
{
    /* Only a body that can dig is ever underground. Without this guard any
     * animal that ends up inside terrain - through a bad spawn, or by falling
     * into a hillside - is classified as burrowing and then held there by the
     * burrow floor, with no way out. Making the medium depend on the genome
     * rather than on geometry alone removes that whole class of failure. */
    if (b->s.dig > 0.0f && b->p.y > ground + 2.0f) return CP4_UNDER;
    if (ground > CP4_SEA && b->p.y > CP4_SEA + 1.0f) return CP4_IN_WATER;
    if (b->p.y < ground - b->s.stand - 8.0f) return CP4_IN_AIR;
    return CP4_ON_GROUND;
    (void)w;
}

/* One call, four sets of physics.
 *
 *   ground - legs, gravity, slopes that cost stamina
 *   water  - fins for thrust, buoyancy against gravity, a breath meter
 *   air    - wings for lift, and lift that needs airspeed to exist
 *   under  - no gravity at all, a dig rate, and a ceiling on how deep
 *
 * `climb` is the pitch control: it means look up/down on the ground, rise/dive
 * in water and air, and angle the tunnel underground. */
static void locomote(Cp4World *w, Cp4Beast *b, float turn, float move, float climb,
                     int ascend, int burrow, float dt)
{
    const Cp4Stats *s = &b->s;
    b->yaw = ang_wrap(b->yaw + turn * s->turn * dt);
    climb = clampf(climb, -1.0f, 1.0f);

    float fx = cosf(b->yaw), fz = sinf(b->yaw);
    float ground = cp4_height(w->seed, b->p.x, b->p.z);
    float drive = clampf(move, 0.0f, 1.0f);
    if (b->stam <= 0.0f) drive *= 0.35f;

    int med = medium_at(w, b, ground);

    /* Entering and leaving the soil is a decision, not a side effect: hold the
     * burrow control to dive under, release it to come back up. */
    if (med != CP4_UNDER && burrow && s->dig > 0.0f && b->grounded) {
        b->p.y = ground + 3.0f;
        b->v.x = b->v.y = b->v.z = 0.0f;
        med = CP4_UNDER;
    }

    switch (med) {

    case CP4_IN_WATER: {
        /* Thrust only if the body has something to push water with. A finless
         * animal here is not a slow swimmer, it is a drowning one. */
        float thrust = s->swim * drive;
        b->v.x += fx * thrust * dt * 3.0f;
        b->v.z += fz * thrust * dt * 3.0f;
        b->v.y += (-climb * thrust * 2.2f) * dt;
        /* buoyancy fights gravity; a fat body bobs, a lean one sinks */
        b->v.y += GRAVITY * (1.0f - s->buoy) * dt;
        if (ascend) b->v.y -= (30.0f + s->swim * 0.8f) * dt * 6.0f;
        /* water is thick: drag does the speed limiting rather than a cap */
        float wd = 2.4f;
        b->v.x -= b->v.x * wd * dt;
        b->v.y -= b->v.y * wd * dt;
        b->v.z -= b->v.z * wd * dt;
        b->stam -= (drive * 3.0f - 1.4f) * dt;
        break;
    }

    case CP4_IN_AIR: {
        if (s->fly > 0.0f) {
            /* Lift needs airspeed. That single dependency is what makes flight
             * a skill rather than a hover button: stall and you drop, and the
             * way out of a stall is to point down and get moving again. */
            float sp = sqrtf(b->v.x * b->v.x + b->v.z * b->v.z);
            float airspeed = clampf(sp / (s->speed + 1.0f), 0.0f, 1.6f);
            float lift = s->fly * (0.30f + 0.70f * airspeed) * (ascend ? 1.35f : 0.85f);
            b->v.y -= lift * dt;
            b->v.x += fx * s->fly * drive * 0.55f * dt;
            b->v.z += fz * s->fly * drive * 0.55f * dt;
            b->v.y += climb * s->fly * 0.55f * dt;
            /* Climbing is work; gliding is not. Charging for both meant a
             * winged animal could never stay up long enough for altitude to
             * pay for itself, and the sky went unused. */
            b->stam -= (ascend ? 7.0f : 0.6f) * dt;
        } else {
            /* wingless: this is a fall, and the only control is a little drift */
            b->v.x += fx * drive * s->accel * 0.10f * dt;
            b->v.z += fz * drive * s->accel * 0.10f * dt;
        }
        b->v.y += GRAVITY * dt;
        float ad = s->fly > 0.0f ? 0.55f : 0.25f;
        b->v.x -= b->v.x * ad * dt;
        b->v.z -= b->v.z * ad * dt;
        break;
    }

    case CP4_UNDER: {
        /* Soil is not a fluid. Position is driven directly by a dig rate, with
         * no momentum and no gravity, which is what makes a burrow feel like
         * tunnelling instead of like swimming through mud. */
        float rate = s->dig * drive;
        b->v.x = fx * rate * 0.75f;
        b->v.z = fz * rate * 0.75f;
        b->v.y = (burrow ? 0.30f : -0.55f) * s->dig - climb * s->dig * 0.85f;
        b->stam -= (1.6f + 3.4f * drive) * dt;
        break;
    }

    default: {   /* CP4_ON_GROUND */
        /* Slope. Climbing costs speed and stamina unless the build bought feet,
         * which is the whole reason grip is a stat rather than a constant. */
        float nx, ny, nz;
        cp4_normal(w->seed, b->p.x, b->p.z, &nx, &ny, &nz);
        float uphill = clampf(-(fx * nx + fz * nz), -1.0f, 1.0f);
        float slope_pen = 1.0f - clampf(uphill, 0.0f, 1.0f) * (1.0f - s->grip) * 0.85f;

        if (b->grounded) {
            b->v.x += fx * drive * s->accel * slope_pen * dt;
            b->v.z += fz * drive * s->accel * slope_pen * dt;
            /* A winged animal takes off with the same control a legged one
             * jumps with. One button, and what happens next is the genome's
             * business. */
            if (ascend) {
                float up = s->fly > 0.0f ? s->jump + s->fly * 0.55f : s->jump;
                b->v.y -= up;
                b->grounded = 0;
                b->stam -= 8.0f;
            }
        }
        b->stam -= (drive * 5.5f + (b->grounded ? 0.0f : 2.0f)) * dt;
        b->v.y += GRAVITY * dt;

        /* Throttle sets the speed the animal is allowed to reach, and friction
         * is gentle enough that it actually gets there. With a stiff drag and a
         * cap that ignored the throttle, terminal speed came out around a sixth
         * of the stat and every build crawled. */
        float sp = sqrtf(b->v.x * b->v.x + b->v.z * b->v.z);
        float cap = s->speed * slope_pen * (0.28f + 0.72f * drive);
        if (sp > cap && sp > 0.001f) { b->v.x = b->v.x / sp * cap; b->v.z = b->v.z / sp * cap; }
        float fric = b->grounded ? 2.6f : 0.5f;
        b->v.x -= b->v.x * fric * dt;
        b->v.z -= b->v.z * fric * dt;
        break;
    }
    }

    b->stam = clampf(b->stam + 3.2f * dt, 0.0f, s->stamina);

    /* The world has no walls. Terrain is a pure function of position, so there
     * is nothing to clamp against - the only bound is how much of the world is
     * resident near the player, which is handled by streaming rather than by
     * fencing the animal in. */
    b->p.x += b->v.x * dt;
    b->p.z += b->v.z * dt;
    b->p.y += b->v.y * dt;

    ground = cp4_height(w->seed, b->p.x, b->p.z);

    if (med == CP4_UNDER) {
        /* stay inside the soil: a ceiling just under the surface and a floor
         * you cannot dig past */
        float roof = ground + 2.5f, floor_y = ground + 55.0f;
        if (b->p.y < roof) {
            /* surfacing */
            b->p.y = ground - s->stand;
            b->v.y = 0.0f;
            b->grounded = 1;
            med = CP4_ON_GROUND;
        } else if (b->p.y > floor_y) {
            b->p.y = floor_y;
        }
    } else {
        float foot = ground - s->stand;
        if (b->p.y >= foot) {             /* landed: y grows downward */
            b->p.y = foot;
            b->v.y = 0.0f;
            b->grounded = 1;
        } else {
            b->grounded = 0;
        }
        if (b->p.y < -CP4_SKY) { b->p.y = -CP4_SKY; if (b->v.y < 0.0f) b->v.y = 0.0f; }
    }

    b->medium = (uint8_t)medium_at(w, b, cp4_height(w->seed, b->p.x, b->p.z));

    /* Breath. Gills set this to effectively infinite, so the meter simply
     * never moves for an animal built for the water. */
    if (b->medium == CP4_IN_WATER || b->medium == CP4_UNDER) {
        /* Soil has pockets; open water has none. A burrow is somewhere you can
         * stay a while, not somewhere you are holding your breath. */
        b->breath -= dt * (b->medium == CP4_UNDER ? 0.35f : 1.0f);
        if (b->breath < 0.0f) b->hp -= 9.0f * dt;
    } else {
        b->breath = clampf(b->breath + dt * 4.0f, 0.0f, s->breath);
    }

    b->phase += dt * (2.0f + 9.0f * drive);
}

/* What this animal can perceive from where it is.
 *
 * Height is the whole payoff of flight. The air has no food in it and costs
 * nearly twice the upkeep of walking, so without a reason to be up there a set
 * of wings is a tax - and in the first measured table the flyer spent 7% of
 * its life airborne and gained nothing by it. Seeing much further from
 * altitude is that reason: in an unbounded world, range is what finds the next
 * species to impress. */
static float perceive_at(const Cp4World *w, const Cp4Beast *b)
{
    /* Underground there is nothing to see, so an ear is the only sense left.
     * Without this a burrower never meets a neighbour and is cut off from
     * every source of DNA except the roots in front of its face. */
    if (b->medium == CP4_UNDER)
        return b->s.hearing > 60.0f ? b->s.hearing : 60.0f;
    if (b->medium != CP4_IN_AIR) return b->s.sight;
    float g = cp4_height(w->seed, b->p.x, b->p.z);
    float alt = clampf((g - b->p.y) / 180.0f, 0.0f, 1.6f);
    return b->s.sight * (1.0f + alt);
}

/* ---------------- npcs ---------------- */

static void beast_think(Cp4World *w, Cp4Beast *b, int idx)
{
    b->think_t -= CP4_DT;
    if (b->think_t > 0.0f) return;
    b->think_t = 0.22f + 0.02f * (float)(idx % 5);

    float see = perceive_at(w, b), see2 = see * see;
    float best = 1e18f;
    b->has_target = 0;
    b->want_med = CP4_ON_GROUND;

    uint8_t eats_plant = b->s.graze_eff > 0.0f;
    uint8_t eats_meat  = b->s.carn_eff > 0.0f;

    /* Food this body can actually get to. Without the medium filter an NPC
     * walks toward kelp it will drown reaching or a tuber it cannot dig for,
     * ignores the bush beside it, and starves - which is exactly what
     * happened to every rival population when the food split four ways. */
    for (int i = 0; i < CP4_MAX_FLORA; i++) {
        const Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        int meat = (f->type == CP4_FLORA_CARCASS);
        if (meat ? !eats_meat : !eats_plant) continue;
        int med = cp4_flora_medium(f->type);
        if (med == CP4_IN_WATER && b->s.swim <= 0.0f) continue;
        if (med == CP4_UNDER && b->s.dig <= 0.0f) continue;
        float d2 = flat2(f->p, b->p);
        if (d2 > see2 || d2 >= best) continue;
        best = d2; b->des = f->p; b->has_target = 1;
        b->want_med = (uint8_t)med;
    }

    /* Predators hunt, and a hostile nest hunts the player specifically. The
     * hunger gate matters: without it every meat-eater on the map converged on
     * the player at once and the stage was a mobbing simulator. */
    if (eats_meat && b->s.bite + b->s.claw_dmg > 8.0f && b->energy < 62.0f
        && b->nest != CP4_OWN_NEST) {
        const Cp4Beast *pl = &w->player;
        float host = -w->nest[b->nest % CP4_MAX_NESTS].standing;
        if (pl->alive && host > -0.2f) {
            float d2 = flat2(pl->p, b->p);
            if (d2 < see2 * (0.30f + 0.55f * (host + 1.0f) * 0.5f) && d2 < best * 4.0f) {
                best = d2; b->des = pl->p; b->has_target = 1;
            }
        }
    }

    /* A hatchling from the player's own nest keeps station on the player.
     * That is the whole point of having laid it: it is a lineage that travels
     * with you rather than a herd that stays put. */
    if (b->nest == CP4_OWN_NEST) {
        const Cp4Beast *pl = &w->player;
        if (pl->alive && flat2(pl->p, b->p) > 70.0f * 70.0f) {
            b->des = pl->p;
            b->has_target = 1;
        } else if (!b->has_target && w->home.alive) {
            b->des = w->home.p;
            b->has_target = 1;
        }
        return;
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
    if (b->nest != CP4_OWN_NEST && w->nest[b->nest % CP4_MAX_NESTS].members > 0)
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

    Cp4Vec was = p->p;
    locomote(w, p, clampf(act[0], -1, 1), clampf(act[2], 0, 1), clampf(act[1], -1, 1),
             act[3] > 0.5f, act[6] > 0.5f, dt);
    /* Upkeep is not the same in every medium. Flight is expensive, burrowing is
     * work, and floating costs almost nothing - which is a large part of why a
     * body chooses the medium it does. */
    static const float MED_COST[CP4_MEDIUM_COUNT] = { 1.00f, 0.80f, 1.75f, 1.30f };
    p->energy -= p->s.upkeep * dt * (0.6f + 0.7f * clampf(act[2], 0, 1))
                                 * MED_COST[p->medium % CP4_MEDIUM_COUNT];
    w->medium_steps[p->medium % CP4_MEDIUM_COUNT]++;
    {
        float dx = p->p.x - was.x, dz = p->p.z - was.z;
        w->travelled += sqrtf(dx * dx + dz * dz);
        float ox = p->p.x - CP4_W * 0.5f, oz = p->p.z - CP4_D * 0.5f;
        float d = sqrtf(ox * ox + oz * oz);
        if (d > w->far_from_start) w->far_from_start = d;
    }

    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        Cp4Beast *b = &w->beast[i];
        if (!b->alive) continue;
        beast_think(w, b, i);
        float turn = b->has_target ? steer_to(b, b->des) : 0.0f;
        /* An animal avoids water it cannot survive. One that swims does not:
         * for a finned body the lake is the target, not the obstacle. */
        if (b->s.swim <= 0.0f &&
            cp4_height(w->seed, b->p.x + cosf(b->yaw) * 55.0f,
                                b->p.z + sinf(b->yaw) * 55.0f) > CP4_SEA - 8.0f) {
            float nx, ny, nz;
            cp4_normal(w->seed, b->p.x, b->p.z, &nx, &ny, &nz);
            (void)ny;
            if (nx * nx + nz * nz > 1e-6f)
                turn = clampf(ang_wrap(atan2f(-nz, -nx) - b->yaw) * 2.4f, -1.0f, 1.0f);
        }
        /* An animal has to be able to use the medium its food lives in, or
         * the medium belongs to the player alone and every rival species is
         * quietly on a starvation diet. */
        float climb = 0.0f;
        int nb_dig = 0, nb_up = 0;
        int gasping = b->s.breath < 1.0e8f && b->breath < b->s.breath * 0.30f;
        if (b->has_target &&
            (b->medium == CP4_IN_WATER || b->medium == CP4_UNDER))
            climb = clampf((b->p.y - b->des.y) * 0.03f, -1.0f, 1.0f);
        if (gasping) climb = 1.0f;
        if (!gasping && b->s.dig > 0.0f && b->want_med == CP4_UNDER) nb_dig = 1;
        if (b->medium == CP4_IN_WATER && climb > 0.4f) nb_up = 1;
        if (b->s.fly > 0.0f && b->medium == CP4_IN_AIR && climb > 0.2f) nb_up = 1;
        locomote(w, b, turn, 0.55f + 0.35f * (1.0f - fabsf(turn)), climb, nb_up, nb_dig, dt);
        b->energy -= b->s.upkeep * dt;
        b->age += dt;
        if (b->atk_cd > 0.0f) b->atk_cd -= dt;
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

    /* ---- player feeding ----
     * Reach is gated by medium: kelp is only edible from the water and a tuber
     * only from inside the soil. That single check is what makes fins and
     * digging claws pay for themselves rather than being movement toys. */
    for (int i = 0; i < CP4_MAX_FLORA; i++) {
        Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        if (cp4_flora_medium(f->type) != p->medium) continue;
        float rr = p->s.radius + f->r + 10.0f;
        if (flat2(f->p, p->p) > rr * rr) continue;
        if (f->type == CP4_FLORA_TUBER && fabsf(f->p.y - p->p.y) > 26.0f) continue;
        if (f->type == CP4_FLORA_KELP && fabsf(f->p.y - p->p.y) > 70.0f) continue;

        if (f->type == CP4_FLORA_CARCASS) {
            if (p->s.carn_eff <= 0.0f) continue;
            w->dna += 2.10f * p->s.carn_eff;
            p->energy = clampf(p->energy + 17.0f, 0.0f, 170.0f);
            w->ate_meat++;
        } else {
            if (p->s.graze_eff <= 0.0f) continue;
            /* the harder a food is to reach, the better it pays */
            /* Every payout here is a third higher than it was when one food
             * pool fed one medium. Splitting the pool four ways left every
             * archetype topping out around half the meter - the specialists
             * were not failing at their niche, the niche simply did not pay
             * enough to finish a run. */
            float worth = f->type == CP4_FLORA_KELP  ? 1.80f
                        : f->type == CP4_FLORA_TUBER ? 3.10f : 1.00f;
            float feed  = f->type == CP4_FLORA_KELP  ? 13.0f
                        : f->type == CP4_FLORA_TUBER ? 16.0f : 9.0f;
            w->dna += worth * p->s.graze_eff;
            p->energy = clampf(p->energy + feed, 0.0f, 170.0f);
            if (f->type == CP4_FLORA_KELP)       w->ate_kelp++;
            else if (f->type == CP4_FLORA_TUBER) w->ate_tuber++;
            else                                 w->ate_plant++;
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
            if (cp4_flora_medium(f->type) != b->medium) continue;
            int meat = (f->type == CP4_FLORA_CARCASS);
            if (meat ? b->s.carn_eff <= 0.0f : b->s.graze_eff <= 0.0f) continue;
            float rr = b->s.radius + f->r + 8.0f;
            if (flat2(f->p, b->p) > rr * rr) continue;
            b->energy += (meat ? 40.0f : 26.0f) * (meat ? b->s.carn_eff : b->s.graze_eff);
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
                Cp4Vec at = v4(b->p.x + cp_rng_range(&w->rng, -40.0f, 40.0f), 0.0f,
                               b->p.z + cp_rng_range(&w->rng, -40.0f, 40.0f));
                spawn_beast(w, slot, at, &child, b->nest, (uint8_t)(b->gen < 250 ? b->gen + 1 : 250));
                w->beast[slot].energy = BREED_COST * 0.8f;
                if (b->nest != CP4_OWN_NEST) w->nest[b->nest % CP4_MAX_NESTS].members++;
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
            if (b->nest == CP4_OWN_NEST) continue;
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
        if (b->nest == CP4_OWN_NEST) continue;   /* your own do not fight you */
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

    /* ---- the nest the player builds ----
     *
     * The one piece of the world the agent makes rather than finds. Building
     * costs energy; standing at it afterwards banks food, heals, and once
     * enough is banked it hatches a follower carrying a mutated copy of the
     * player's own genome. Followers are a lineage, not a summon: they eat,
     * they can be killed, and they inherit whatever the player has become. */
    {
        int want_nest = act[7] > 0.5f;
        float dh2 = w->home.alive ? flat2(w->home.p, p->p) : 1e18f;
        float near_r = (p->s.radius + 34.0f) * (p->s.radius + 34.0f);

        if (want_nest && !w->home.alive && p->grounded && p->energy > 45.0f
            && p->medium == CP4_ON_GROUND) {
            w->home.p = v4(p->p.x, cp4_height(w->seed, p->p.x, p->p.z), p->p.z);
            w->home.store = 0.0f;
            w->home.build = 1.0f;
            w->home.eggs = w->home.hatched = 0;
            w->home.alive = 1;
            p->energy -= 32.0f;
            reward += 1.0f;
        } else if (w->home.alive && dh2 < near_r) {
            /* home ground: bank surplus food, and recover on it */
            if (want_nest && p->energy > 60.0f) {
                p->energy -= 26.0f * dt * 10.0f;
                w->home.store += 26.0f * dt * 10.0f;
            }
            p->hp = clampf(p->hp + 7.0f * dt, 0.0f, p->hp_max);
            p->stam = clampf(p->stam + 9.0f * dt, 0.0f, p->s.stamina);
            p->breath = p->s.breath;
        }

        if (w->home.alive && w->home.store >= 60.0f) {
            int slot = -1;
            for (int k = 0; k < CP4_MAX_BEASTS; k++)
                if (!w->beast[k].alive) { slot = k; break; }
            if (slot >= 0) {
                Cp4Genome child = p->g;
                cp4_genome_mutate(&child, &w->rng, CP4_GEN_BUDGET[w->generation], 0.14f);
                Cp4Vec at = v4(w->home.p.x + cp_rng_range(&w->rng, -30.0f, 30.0f), 0.0f,
                               w->home.p.z + cp_rng_range(&w->rng, -30.0f, 30.0f));
                spawn_beast(w, slot, at, &child, CP4_OWN_NEST, (uint8_t)w->generation);
                w->home.store -= 60.0f;
                w->home.eggs++;
                w->home.hatched++;
                w->hatchlings++;
                w->eggs_laid++;
                reward += 1.2f;
            }
        }
    }

    /* ---- discovery ----
     * The world is unbounded and the nests in it are recycled ahead of the
     * player, so meeting a species you have not met before is the thing that
     * makes walking somewhere new pay. */
    for (int k = 0; k < CP4_MAX_NESTS; k++) {
        Cp4Nest *nst = &w->nest[k];
        if (!nst->alive || nst->seen) continue;
        float reach = perceive_at(w, p);
        if (flat2(nst->p, p->p) > reach * reach) continue;
        nst->seen = 1;
        w->discovered++;
        w->dna += 4.5f;
        reward += 1.2f;
    }

    /* regrow the world, inside the resident ring rather than inside a box */
    for (int k = 0; k < 3 && w->n_flora < TARGET_FLORA; k++) {
        int slot = alloc_flora(w);
        if (slot < 0) break;
        spawn_flora(w, slot, w->anchor, 120.0f, RESIDENT);
        w->n_flora++;
    }
    stream_world(w);
    if ((w->step & 63) == 0) {
        census(w);
        if (w->pop < 8) {
            for (int i = 0; i < CP4_MAX_BEASTS && w->pop < 16; i++) {
                if (w->beast[i].alive) continue;
                int k = i % CP4_MAX_NESTS;
                Cp4Vec at = v4(w->nest[k].p.x + cp_rng_range(&w->rng, -120.0f, 120.0f), 0.0f,
                               w->nest[k].p.z + cp_rng_range(&w->rng, -120.0f, 120.0f));
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
                /* Redesign toward whatever the body has actually been doing.
                 * An animal that has spent the run in the water should come
                 * out of the editor with better fins, not with legs it has no
                 * use for. */
                int best_med = 0;
                for (int m = 1; m < CP4_MEDIUM_COUNT; m++)
                    if (w->medium_steps[m] > w->medium_steps[best_med]) best_med = m;
                int style;
                if (best_med == CP4_IN_WATER)      style = CP4_STYLE_SWIMMER;
                else if (best_med == CP4_IN_AIR)   style = CP4_STYLE_FLYER;
                else if (best_med == CP4_UNDER)    style = CP4_STYLE_BURROWER;
                else style = (p->s.carn_eff > p->s.graze_eff) ? CP4_STYLE_PREDATOR
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
    float see = perceive_at(w, p), see2 = see * see;
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
    {   /* the climate here, so "somewhere else" is a thing the agent can see */
        float tt, mm;
        cp4_climate(w->seed, p->p.x, p->p.z, &tt, &mm);
        o[k++] = tt;
        o[k++] = mm;
    }
    {   /* slope ahead: the terrain's own gradient, in the body frame */
        float nx, ny, nz;
        cp4_normal(w->seed, p->p.x + fx * 30.0f, p->p.z + fz * 30.0f, &nx, &ny, &nz);
        o[k++] = clampf(-(fx * nx + fz * nz) * 3.0f, -2.0f, 2.0f);
        o[k++] = clampf(-ny, 0.0f, 1.0f);                     /* 1 on the flat */
    }
    o[k++] = clampf((float)w->allies / (float)CP4_MAX_NESTS, 0.0f, 1.0f);
    o[k++] = clampf((float)w->enemies / (float)CP4_MAX_NESTS, 0.0f, 1.0f);

    /* ---- where the body is, and what it can do there ---- */
    for (int m = 0; m < CP4_MEDIUM_COUNT; m++)
        o[k++] = (p->medium == m) ? 1.0f : 0.0f;
    o[k++] = s->swim > 0.0f ? 1.0f : 0.0f;
    o[k++] = s->fly  > 0.0f ? 1.0f : 0.0f;
    o[k++] = s->dig  > 0.0f ? 1.0f : 0.0f;
    /* breath is what makes water a place with a clock on it */
    o[k++] = clampf(p->breath / (s->breath > 1.0f ? s->breath : 1.0f), 0.0f, 1.0f);
    /* signed height relative to the ground: positive in the air, negative in
     * the soil, which is one number the agent can steer on in every medium */
    o[k++] = clampf((ground - p->p.y) / 160.0f, -1.5f, 2.5f);

    /* ---- the nest the player built ---- */
    o[k++] = w->home.alive ? 1.0f : 0.0f;
    if (w->home.alive) {
        float dx = w->home.p.x - p->p.x, dz = w->home.p.z - p->p.z;
        o[k++] = clampf((dx * fx + dz * fz) / 500.0f, -2.5f, 2.5f);
        o[k++] = clampf((dx * rx + dz * rz) / 500.0f, -2.5f, 2.5f);
        o[k++] = clampf(w->home.store / 60.0f, 0.0f, 2.0f);
    } else {
        o[k++] = 0.0f; o[k++] = 0.0f; o[k++] = 0.0f;
    }
    {
        int mine = 0;
        for (int i = 0; i < CP4_MAX_BEASTS; i++)
            if (w->beast[i].alive && w->beast[i].nest == CP4_OWN_NEST) mine++;
        o[k++] = clampf((float)mine * 0.2f, 0.0f, 2.0f);
    }

    float fd[CP4_OBS_FLORA_K]; int fi_[CP4_OBS_FLORA_K];
    for (int i = 0; i < CP4_OBS_FLORA_K; i++) { fd[i] = 1e18f; fi_[i] = -1; }
    for (int i = 0; i < CP4_MAX_FLORA; i++) {
        const Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        int meat = (f->type == CP4_FLORA_CARCASS);
        if (meat ? s->carn_eff <= 0.0f : s->graze_eff <= 0.0f) continue;
        /* Food in a medium this body cannot enter is not food. Reporting kelp
         * to a legged animal that will drown reaching it is worse than
         * reporting nothing. */
        int med = cp4_flora_medium(f->type);
        if (med == CP4_IN_WATER && s->swim <= 0.0f) continue;
        if (med == CP4_UNDER && s->dig <= 0.0f) continue;
        float d2 = flat2(f->p, p->p);
        if (d2 > see2) continue;
        topk(fd, fi_, CP4_OBS_FLORA_K, d2, i);
    }
    for (int i = 0; i < CP4_OBS_FLORA_K; i++) {
        if (fi_[i] < 0) {
            for (int z = 0; z < CP4_OBS_FLORA; z++) o[k++] = 0.0f;
            continue;
        }
        const Cp4Flora *f = &w->flora[fi_[i]];
        float dx = f->p.x - p->p.x, dz = f->p.z - p->p.z;
        o[k++] = clampf((dx * fx + dz * fz) / 300.0f, -2.5f, 2.5f);
        o[k++] = clampf((dx * rx + dz * rz) / 300.0f, -2.5f, 2.5f);
        /* relative to the animal, not to the ground: in three media out of
         * four the ground is not where the animal is */
        o[k++] = clampf((f->p.y - p->p.y) / 80.0f, -2.5f, 2.5f);
        o[k++] = f->type == CP4_FLORA_BUSH ? 1.0f : 0.0f;
        o[k++] = f->type == CP4_FLORA_CARCASS ? 1.0f : 0.0f;
        o[k++] = f->type == CP4_FLORA_KELP ? 1.0f : 0.0f;
        o[k++] = f->type == CP4_FLORA_TUBER ? 1.0f : 0.0f;
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

    float see = perceive_at(w, p), see2 = see * see;
    float ground = cp4_height(w->seed, p->p.x, p->p.z);
    Cp4Vec target = p->p;
    int have = 0, attack = 0, sing = 0;
    float best = 1e18f;
    int want_medium = p->medium;

    /* one voice sac is all a starting budget can afford, so the threshold
     * has to sit below what one voice sac buys */
    int charmer = (s->charm > 0.35f);

    for (int i = 0; i < CP4_MAX_BEASTS; i++) {
        const Cp4Beast *b = &w->beast[i];
        if (!b->alive || b->nest == CP4_OWN_NEST) continue;
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

    /* Food, and the medium it lives in. This is the whole reason the baseline
     * needs to know about media at all: it has to be willing to go into the
     * water or under the ground to reach what it eats, and it must not pick a
     * target it has no way of reaching. */
    for (int i = 0; i < CP4_MAX_FLORA; i++) {
        const Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        int meat = (f->type == CP4_FLORA_CARCASS);
        if (meat ? s->carn_eff <= 0.0f : s->graze_eff <= 0.0f) continue;
        int med = cp4_flora_medium(f->type);
        if (med == CP4_IN_WATER && s->swim <= 0.0f) continue;
        if (med == CP4_UNDER && s->dig <= 0.0f) continue;
        /* a body on a breath clock does not start a dive it cannot finish */
        if (med == CP4_IN_WATER && s->breath < 1.0e8f && p->breath < s->breath * 0.35f) continue;
        float d2 = flat2(f->p, p->p);
        if (d2 > see2 || d2 >= best) continue;
        best = d2; target = f->p; have = 1; want_medium = med;
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
            if (!b->alive || b->nest == CP4_OWN_NEST) continue;
            const Cp4Nest *nst = &w->nest[b->nest % CP4_MAX_NESTS];
            float threat = b->s.bite + b->s.claw_dmg;
            if (threat < 9.0f) continue;                 /* harmless */
            if (nst->standing >= 0.5f) continue;         /* friends */
            if (own > threat * 1.15f) continue;          /* you are the danger */
            /* nothing on the surface can follow you into the soil */
            if (p->medium == CP4_UNDER) continue;
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
    /* A burrower's answer to being chased is to go down, not to run. */
    int hide = fleeing && s->dig > 0.0f && p->grounded;

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

    /* Shorelines, in both directions.
     *
     * A legged body steers uphill away from water it would drown in. A finned
     * one has the opposite problem and it is the more dangerous of the two: a
     * swimmer that wanders into the shallows is beached, and a build that spent
     * its budget on fins instead of legs crawls at a sixth of its swimming
     * speed. Either way the fix is the terrain gradient - uphill is minus the
     * normal's horizontal part, downhill is plus it. */
    float ahead_x = p->p.x + cosf(p->yaw) * 70.0f;
    float ahead_z = p->p.z + sinf(p->yaw) * 70.0f;
    float ahead_g = cp4_height(w->seed, ahead_x, ahead_z);
    int aquatic = s->swim > 0.0f && s->n[CP4_LEG] == 0;
    int wants_slope = 0, downhill = 0;

    if (s->swim <= 0.0f && ahead_g > CP4_SEA - 10.0f && !fleeing) {
        wants_slope = 1; downhill = 0;               /* get out of the water */
    } else if (aquatic && p->medium != CP4_IN_WATER) {
        wants_slope = 1; downhill = 1;               /* get back into it */
    } else if (aquatic && ground < CP4_SEA + 30.0f) {
        wants_slope = 1; downhill = 1;               /* too shallow to stay */
    }
    if (wants_slope) {
        float nx, ny, nz;
        cp4_normal(w->seed, p->p.x, p->p.z, &nx, &ny, &nz);
        (void)ny;
        if (nx * nx + nz * nz > 1e-6f) {
            float sx = downhill ? nx : -nx, sz = downhill ? nz : -nz;
            turn = clampf(ang_wrap(atan2f(sz, sx) - p->yaw) * 2.4f, -1.0f, 1.0f);
            have = 1;
        }
    }

    act[0] = turn;

    /* ---- vertical control ----
     * One number that means "up" in every medium: rise in water, climb in the
     * air, angle the tunnel underground, and nothing at all on foot. Steering
     * at the target's height rather than at a fixed depth is what lets one
     * baseline dive for kelp and surface for breath with the same rule. */
    float climb = 0.0f;
    if (p->medium == CP4_IN_WATER) {
        if (s->breath < 1.0e8f && p->breath < s->breath * 0.30f)
            climb = 1.0f;                      /* out of air: go up, now */
        else if (have) climb = clampf((p->p.y - target.y) * 0.03f, -1.0f, 1.0f);
    } else if (p->medium == CP4_IN_AIR) {
        /* hold a cruising height over whatever is below */
        float g = cp4_height(w->seed, p->p.x, p->p.z);
        float alt = g - p->p.y;
        climb = clampf((110.0f - alt) * 0.02f, -1.0f, 1.0f);
    } else if (p->medium == CP4_UNDER) {
        /* come up for air before the meter runs out, whatever is down there */
        if (s->breath < 1.0e8f && p->breath < s->breath * 0.30f) climb = 1.0f;
        else climb = have ? clampf((p->p.y - target.y) * 0.05f, -1.0f, 1.0f) : 0.0f;
    }
    act[1] = climb;

    /* still drive hard through a turn: steer_to saturates on anything past
     * 25 degrees, so tying throttle tightly to it means standing and
     * spinning instead of walking */
    act[2] = fleeing ? 1.0f : 0.60f + 0.40f * (1.0f - fabsf(turn));

    /* ascend: take off if the body flies and the food is somewhere else */
    int ascend = 0;
    if (p->medium == CP4_IN_AIR && climb > 0.4f) ascend = 1;
    if (p->medium == CP4_IN_WATER && climb > 0.4f) ascend = 1;
    /* A winged animal with nothing worth walking to goes up to look for
     * something, because altitude is the only thing the sky pays. */
    if (s->fly > 0.0f && p->grounded && (!have || best > 260.0f * 260.0f)
        && p->stam > s->stamina * 0.35f) ascend = 1;
    act[3] = ascend ? 1.0f : 0.0f;

    act[4] = attack ? 1.0f : 0.0f;
    act[5] = sing ? 1.0f : 0.0f;
    /* dig: to hide, or because what this build eats is buried */
    {
        int gasping = s->breath < 1.0e8f && p->breath < s->breath * 0.30f;
        act[6] = (!gasping && (hide || (s->dig > 0.0f && want_medium == CP4_UNDER)))
                 ? 1.0f : 0.0f;
    }

    /* ---- the nest ----
     * Build one once there is energy to spare, then come back to it to bank
     * the surplus. A nest is only worth having if something is put into it. */
    {
        int build = 0;
        if (!w->home.alive && p->energy > 95.0f && p->grounded
            && p->medium == CP4_ON_GROUND) build = 1;
        else if (w->home.alive && p->energy > 105.0f
                 && flat2(w->home.p, p->p) < 40.0f * 40.0f) build = 1;
        act[7] = build ? 1.0f : 0.0f;
        /* and walk home when there is a full belly to deposit */
        if (w->home.alive && p->energy > 120.0f && !fleeing && !attack) {
            act[0] = steer_to(p, w->home.p);
            act[2] = 0.85f;
        }
    }

    /* ---- the design head ----
     * Redesign toward the medium the body has actually been living in, so an
     * animal that took to the water comes out of the editor with better fins
     * rather than with legs it has stopped using. */
    int next = w->generation + 1;
    if (next > CP4_GENERATIONS - 1) next = CP4_GENERATIONS - 1;
    int best_med = 0;
    for (int m = 1; m < CP4_MEDIUM_COUNT; m++)
        if (w->medium_steps[m] > w->medium_steps[best_med]) best_med = m;
    int style;
    if (best_med == CP4_IN_WATER)     style = CP4_STYLE_SWIMMER;
    else if (best_med == CP4_IN_AIR)  style = CP4_STYLE_FLYER;
    else if (best_med == CP4_UNDER)   style = CP4_STYLE_BURROWER;
    else style = charmer ? CP4_STYLE_CHARMER
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
