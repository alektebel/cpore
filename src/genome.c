#include "cpore/cpore.h"
#include <string.h>
#include <math.h>

/* The creature editor, as an action space.
 *
 * A genome is up to 12 parts, each with a type and a body-relative mounting
 * angle. Placement is load-bearing: spikes and jaws only reach through the
 * arc they point down, jets only push you forward if they are mounted behind
 * you. Parts are bought with DNA out of a per-generation budget, so a build
 * is a set of things given up as much as a set of things acquired. */

const int CP_GEN_BUDGET[CP_GENERATIONS] = { 30, 55, 85, 125 };

static const struct { const char *name; int cost; } PARTS[CP_PART_COUNT] = {
    { "-",          0 },
    { "filter",     5 },
    { "jaw",       10 },
    { "proboscis", 16 },
    { "cilia",      5 },
    { "flagella",  10 },
    { "jet",       16 },
    { "spike",      9 },
    { "electric",  20 },
    { "poison",    14 },
    { "eye",        6 },
};

const char *cp_part_name(int t)
{
    return (t >= 0 && t < CP_PART_COUNT) ? PARTS[t].name : "?";
}

int cp_part_cost(int t)
{
    return (t >= 0 && t < CP_PART_COUNT) ? PARTS[t].cost : 0;
}

void cp_genome_clear(CpGenome *g)
{
    memset(g, 0, sizeof(*g));
}

int cp_genome_cost(const CpGenome *g)
{
    int c = 0;
    for (int i = 0; i < CP_MAX_PARTS; i++) c += cp_part_cost(g->part[i].type);
    return c;
}

static int genome_count(const CpGenome *g, int type)
{
    int n = 0;
    for (int i = 0; i < CP_MAX_PARTS; i++) if (g->part[i].type == type) n++;
    return n;
}

/* the gen-0 cell: one mouth, two cilia, nothing else - same as Spore's */
void cp_genome_starter(CpGenome *g)
{
    cp_genome_clear(g);
    g->part[0].type = CP_PART_FILTER;   g->part[0].angle = 0;
    g->part[1].type = CP_PART_CILIA;    g->part[1].angle = 96;
    g->part[2].type = CP_PART_CILIA;    g->part[2].angle = 160;
}

void cp_genome_normalise(CpGenome *g, int budget)
{
    /* compact: push live parts to the front so slot index means nothing */
    CpPart tmp[CP_MAX_PARTS];
    int n = 0;
    for (int i = 0; i < CP_MAX_PARTS; i++)
        if (g->part[i].type > CP_PART_NONE && g->part[i].type < CP_PART_COUNT)
            tmp[n++] = g->part[i];
    memset(g, 0, sizeof(*g));
    for (int i = 0; i < n; i++) g->part[i] = tmp[i];

    /* a cell with no mouth cannot eat; grant the cheapest one before budgeting
     * so the trim below accounts for its cost */
    if (genome_count(g, CP_PART_FILTER) == 0 &&
        genome_count(g, CP_PART_JAW) == 0 &&
        genome_count(g, CP_PART_PROBOSCIS) == 0) {
        int slot = (n < CP_MAX_PARTS) ? n++ : CP_MAX_PARTS - 1;
        g->part[slot].type = CP_PART_FILTER;
    }

    /* spend down to budget, dropping the most expensive part first so a build
     * loses its luxuries rather than its whole identity - never the last mouth */
    while (cp_genome_cost(g) > budget) {
        int worst = -1;
        for (int i = 0; i < CP_MAX_PARTS; i++) {
            int t = g->part[i].type;
            if (t == CP_PART_NONE) continue;
            int is_mouth = (t == CP_PART_FILTER || t == CP_PART_JAW || t == CP_PART_PROBOSCIS);
            if (is_mouth) {
                int mouths = genome_count(g, CP_PART_FILTER) + genome_count(g, CP_PART_JAW)
                           + genome_count(g, CP_PART_PROBOSCIS);
                if (mouths <= 1) continue;
            }
            if (worst < 0 || cp_part_cost(t) > cp_part_cost(g->part[worst].type)) worst = i;
        }
        if (worst < 0) break;
        g->part[worst].type = CP_PART_NONE;
        cp_genome_normalise(g, budget);      /* re-compact, then re-check */
        return;
    }
}

void cp_genome_random(CpGenome *g, CpRng *r, int budget)
{
    cp_genome_clear(g);
    for (int i = 0; i < CP_MAX_PARTS; i++) {
        if (cp_rng_f(r) < 0.25f) continue;
        g->part[i].type = (uint8_t)(1 + cp_rng_int(r, CP_PART_COUNT - 1));
        g->part[i].angle = (uint8_t)cp_rng_int(r, 256);
    }
    cp_genome_normalise(g, budget);
}

/* action head -> genome. Each slot is two floats in [-1,1]: type index and
 * mounting angle. Values outside the valid range clamp rather than wrap, so a
 * saturated policy output means "empty slot" instead of an aliased part. */
void cp_genome_from_action(CpGenome *g, const float *design, int budget)
{
    cp_genome_clear(g);
    for (int i = 0; i < CP_MAX_PARTS; i++) {
        float tv = design[i * 2];
        float av = design[i * 2 + 1];
        if (tv < -1.0f) tv = -1.0f;
        if (tv > 1.0f) tv = 1.0f;
        int t = (int)((tv + 1.0f) * 0.5f * (float)(CP_PART_COUNT - 1) + 0.5f);
        if (t < 0) t = 0;
        if (t >= CP_PART_COUNT) t = CP_PART_COUNT - 1;
        g->part[i].type = (uint8_t)t;

        if (av < -1.0f) av = -1.0f;
        if (av > 1.0f) av = 1.0f;
        int a = (int)((av + 1.0f) * 0.5f * 255.0f + 0.5f);
        g->part[i].angle = (uint8_t)(a < 0 ? 0 : (a > 255 ? 255 : a));
    }
    cp_genome_normalise(g, budget);
}

/* ---- scripted designer ----
 * Buys a coherent build for the given budget and places parts where they
 * work: weapons forward, propulsion aft, eyes on the flanks. */
void cp_genome_autodesign(CpGenome *g, CpRng *r, int budget, int style)
{
    cp_genome_clear(g);
    int slot = 0, spent = 0;

    /* front is angle 0, rear is 128 */
    #define BUY(TYPE, ANGLE)                                                  \
        do {                                                                  \
            if (slot < CP_MAX_PARTS && spent + cp_part_cost(TYPE) <= budget) { \
                g->part[slot].type = (uint8_t)(TYPE);                         \
                g->part[slot].angle = (uint8_t)(ANGLE);                       \
                spent += cp_part_cost(TYPE);                                  \
                slot++;                                                       \
            }                                                                 \
        } while (0)

    switch (style % CP_STYLE_COUNT) {
    case CP_STYLE_HUNTER:
        BUY(CP_PART_JAW, 0);
        BUY(CP_PART_CILIA, 112);
        BUY(CP_PART_CILIA, 144);
        BUY(CP_PART_SPIKE, 16);
        BUY(CP_PART_SPIKE, 240);
        BUY(CP_PART_EYE, 32);
        BUY(CP_PART_JET, 128);
        BUY(CP_PART_JAW, 224);
        BUY(CP_PART_SPIKE, 48);
        BUY(CP_PART_FLAGELLA, 128);
        BUY(CP_PART_SPIKE, 208);
        BUY(CP_PART_POISON, 128);
        break;
    case CP_STYLE_TANK:
        /* Buy order is not cosmetic: the gen-0 budget is 30 DNA, so a style
         * that reaches for its signature part first ends up with no cilia and
         * dies to the first thing that chases it. Mobility comes first. */
        BUY(CP_PART_FILTER, 0);
        BUY(CP_PART_CILIA, 112);
        BUY(CP_PART_CILIA, 144);
        BUY(CP_PART_SPIKE, 128);        /* armour aft: it expects to be chased */
        BUY(CP_PART_SPIKE, 96);
        BUY(CP_PART_SPIKE, 160);
        BUY(CP_PART_POISON, 128);
        BUY(CP_PART_JAW, 0);
        BUY(CP_PART_ELECTRIC, 0);
        BUY(CP_PART_EYE, 32);
        BUY(CP_PART_SPIKE, 32);
        BUY(CP_PART_POISON, 96);
        break;
    case CP_STYLE_SCOUT:
        BUY(CP_PART_FILTER, 0);
        BUY(CP_PART_CILIA, 112);
        BUY(CP_PART_CILIA, 144);
        BUY(CP_PART_EYE, 32);
        BUY(CP_PART_EYE, 224);
        BUY(CP_PART_JET, 128);          /* jets only pay off mounted aft */
        BUY(CP_PART_PROBOSCIS, 8);
        BUY(CP_PART_FLAGELLA, 128);
        BUY(CP_PART_EYE, 128);
        BUY(CP_PART_SPIKE, 0);
        BUY(CP_PART_EYE, 64);
        BUY(CP_PART_JET, 144);
        break;
    default: /* grazer */
        BUY(CP_PART_FILTER, 0);
        BUY(CP_PART_CILIA, 104);
        BUY(CP_PART_CILIA, 152);
        BUY(CP_PART_FLAGELLA, 128);
        BUY(CP_PART_FILTER, 16);
        BUY(CP_PART_EYE, 32);
        BUY(CP_PART_CILIA, 80);
        BUY(CP_PART_SPIKE, 192);
        BUY(CP_PART_JET, 128);
        BUY(CP_PART_CILIA, 176);
        BUY(CP_PART_PROBOSCIS, 8);
        BUY(CP_PART_ELECTRIC, 240);
        break;
    }
    #undef BUY

    /* jitter placement slightly so generations are not carbon copies */
    if (r) {
        for (int i = 0; i < CP_MAX_PARTS; i++)
            if (g->part[i].type != CP_PART_NONE)
                g->part[i].angle = (uint8_t)((g->part[i].angle + cp_rng_int(r, 9) - 4) & 0xFF);
    }
    cp_genome_normalise(g, budget);
}

/* ---- genome -> stats ---- */

void cp_genome_stats(const CpGenome *g, CpStats *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < CP_MAX_PARTS; i++) {
        int t = g->part[i].type;
        if (t > CP_PART_NONE && t < CP_PART_COUNT) { o->n[t]++; o->n_parts++; }
    }
    o->cost = (int16_t)cp_genome_cost(g);

    const uint8_t *n = o->n;

    /* --- feeding --- */
    o->herb_eff = 0.70f * n[CP_PART_FILTER] + 0.48f * n[CP_PART_PROBOSCIS];
    o->carn_eff = 0.75f * n[CP_PART_JAW]    + 0.48f * n[CP_PART_PROBOSCIS];

    /* --- locomotion --- */
    o->max_speed = 150.0f + 40.0f * n[CP_PART_CILIA]
                          + 18.0f * n[CP_PART_FLAGELLA]
                          + 30.0f * n[CP_PART_JET];
    o->accel     = 600.0f + 80.0f * n[CP_PART_CILIA]
                          + 230.0f * n[CP_PART_FLAGELLA]
                          + 90.0f * n[CP_PART_JET];
    o->drag      = 2.6f;

    /* A jet pushes you along the axis it points down, so only its rearward
     * component does anything useful. Mounted at the front it is dead weight
     * you paid 16 DNA for. */
    o->jet_thrust = 0.0f;
    for (int i = 0; i < CP_MAX_PARTS; i++) {
        if (g->part[i].type != CP_PART_JET) continue;
        float a = (float)g->part[i].angle * (2.0f * 3.14159265f / 256.0f);
        float rearward = -cosf(a);
        if (rearward > 0.0f) o->jet_thrust += 165.0f * rearward;
    }

    /* mass costs mobility - a heavily built cell is a slow cell */
    float mass = 1.0f + 0.055f * o->n_parts
                      + 0.03f * n[CP_PART_SPIKE]
                      + 0.03f * n[CP_PART_JAW];
    o->max_speed /= mass;
    o->accel     /= mass;

    /* --- durability --- */
    o->hp_max = 100.0f + 12.0f * n[CP_PART_SPIKE]
                       + 8.0f * n[CP_PART_JAW]
                       + 14.0f * n[CP_PART_POISON];
    o->armor  = 0.075f * n[CP_PART_SPIKE] + 0.05f * n[CP_PART_POISON];
    if (o->armor > 0.60f) o->armor = 0.60f;

    /* --- weapons. Per-part, because the sim applies them through a facing
     *     arc: two spikes on the same side are not two spikes of coverage. --- */
    o->spike_dmg = n[CP_PART_SPIKE] ? 30.0f : 0.0f;
    o->jaw_dmg   = n[CP_PART_JAW]   ? 21.0f : 0.0f;
    o->poison_dmg = 9.0f * n[CP_PART_POISON];

    o->elec_dmg    = 22.0f * n[CP_PART_ELECTRIC];
    o->elec_radius = n[CP_PART_ELECTRIC] ? (58.0f + 16.0f * n[CP_PART_ELECTRIC]) : 0.0f;
    o->elec_cost   = 4.0f;
    o->elec_cd     = 1.6f;

    /* --- senses --- */
    o->percep = 210.0f + 95.0f * n[CP_PART_EYE];
    if (o->percep > 620.0f) o->percep = 620.0f;

    /* --- body --- */
    o->radius0 = 14.0f + 0.75f * o->n_parts;
    o->upkeep  = 0.26f + 0.115f * o->n_parts;
}
