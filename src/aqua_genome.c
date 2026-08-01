#include "cpore/aqua.h"
#include <string.h>
#include <math.h>

/* Stage-2 body plans.
 *
 * The cell stage mounted parts at an angle on a circle. A swimming animal has
 * a body with length, so a part here is placed at a spine segment plus a
 * yaw/pitch on that segment - placement is a direction in three dimensions.
 * Everything else carries over: parts cost DNA out of a per-generation budget,
 * and every part trades something away. */

const int CP3_GEN_BUDGET[CP3_GENERATIONS] = { 40, 70, 105, 150 };

static const struct { const char *name; int cost; } PARTS[CP3_PART_COUNT] = {
    { "-",         0 },
    { "filter",    5 },
    { "jaw",      12 },
    { "fin",       6 },
    { "tail",      8 },
    { "spike",    10 },
    { "eye",       6 },
    { "lung",     10 },
    { "plate",    12 },
    { "light",    14 },
};

const char *cp3_part_name(int t)
{
    return (t >= 0 && t < CP3_PART_COUNT) ? PARTS[t].name : "?";
}

int cp3_part_cost(int t)
{
    return (t >= 0 && t < CP3_PART_COUNT) ? PARTS[t].cost : 0;
}

void cp3_genome_clear(Cp3Genome *g)
{
    memset(g, 0, sizeof(*g));
    g->nseg = 3;
    g->girth = 128;
}

int cp3_genome_cost(const Cp3Genome *g)
{
    int c = 0;
    for (int i = 0; i < CP3_MAX_PARTS; i++) c += cp3_part_cost(g->part[i].type);
    /* a longer body is itself an investment */
    c += (g->nseg > 2 ? (g->nseg - 2) * 4 : 0);
    return c;
}

static int count_of(const Cp3Genome *g, int t)
{
    int n = 0;
    for (int i = 0; i < CP3_MAX_PARTS; i++) if (g->part[i].type == t) n++;
    return n;
}

static int has_mouth(const Cp3Genome *g)
{
    return count_of(g, CP3_FILTER) + count_of(g, CP3_JAW) > 0;
}

void cp3_genome_starter(Cp3Genome *g)
{
    cp3_genome_clear(g);
    g->nseg = 3;
    g->girth = 120;
    g->part[0].type = CP3_FILTER; g->part[0].seg = 0; g->part[0].yaw = 0;   g->part[0].pitch = 0;
    g->part[1].type = CP3_TAIL;   g->part[1].seg = 2; g->part[1].yaw = 128; g->part[1].pitch = 0;
    g->part[2].type = CP3_FIN;    g->part[2].seg = 1; g->part[2].yaw = 64;  g->part[2].pitch = 0;
    g->part[3].type = CP3_FIN;    g->part[3].seg = 1; g->part[3].yaw = 192; g->part[3].pitch = 0;
}

void cp3_genome_normalise(Cp3Genome *g, int budget)
{
    if (g->nseg < 2) g->nseg = 2;
    if (g->nseg > CP3_MAX_SEG) g->nseg = CP3_MAX_SEG;

    /* compact live parts to the front and clamp segment indices into the body
     * that actually exists - shortening the spine must not orphan a part */
    Cp3Part tmp[CP3_MAX_PARTS];
    int n = 0;
    for (int i = 0; i < CP3_MAX_PARTS; i++) {
        int t = g->part[i].type;
        if (t <= CP3_NONE || t >= CP3_PART_COUNT) continue;
        tmp[n] = g->part[i];
        if (tmp[n].seg >= g->nseg) tmp[n].seg = (uint8_t)(g->nseg - 1);
        n++;
    }
    memset(g->part, 0, sizeof(g->part));
    for (int i = 0; i < n; i++) g->part[i] = tmp[i];

    /* an animal with no mouth starves; grant the cheapest one before budgeting */
    if (!has_mouth(g)) {
        int slot = (n < CP3_MAX_PARTS) ? n++ : CP3_MAX_PARTS - 1;
        g->part[slot].type = CP3_FILTER;
        g->part[slot].seg = 0;
        g->part[slot].yaw = 0;
        g->part[slot].pitch = 0;
    }

    /* spend down: drop the most expensive luxury first, then shorten the
     * spine, and never remove the last mouth */
    for (int guard = 0; guard < 64 && cp3_genome_cost(g) > budget; guard++) {
        int worst = -1;
        for (int i = 0; i < CP3_MAX_PARTS; i++) {
            int t = g->part[i].type;
            if (t == CP3_NONE) continue;
            if ((t == CP3_FILTER || t == CP3_JAW) &&
                count_of(g, CP3_FILTER) + count_of(g, CP3_JAW) <= 1) continue;
            if (worst < 0 || cp3_part_cost(t) > cp3_part_cost(g->part[worst].type)) worst = i;
        }
        if (worst >= 0) {
            g->part[worst].type = CP3_NONE;
            /* re-compact so slot index keeps meaning nothing */
            int k = 0;
            for (int i = 0; i < CP3_MAX_PARTS; i++)
                if (g->part[i].type != CP3_NONE) tmp[k++] = g->part[i];
            memset(g->part, 0, sizeof(g->part));
            for (int i = 0; i < k; i++) g->part[i] = tmp[i];
        } else if (g->nseg > 2) {
            g->nseg--;
            for (int i = 0; i < CP3_MAX_PARTS; i++)
                if (g->part[i].seg >= g->nseg) g->part[i].seg = (uint8_t)(g->nseg - 1);
        } else {
            break;
        }
    }
}

void cp3_genome_random(Cp3Genome *g, CpRng *r, int budget)
{
    cp3_genome_clear(g);
    g->nseg = (uint8_t)(2 + cp_rng_int(r, CP3_MAX_SEG - 1));
    g->girth = (uint8_t)(60 + cp_rng_int(r, 180));
    for (int i = 0; i < CP3_MAX_PARTS; i++) {
        if (cp_rng_f(r) < 0.30f) continue;
        g->part[i].type  = (uint8_t)(1 + cp_rng_int(r, CP3_PART_COUNT - 1));
        g->part[i].seg   = (uint8_t)cp_rng_int(r, g->nseg);
        g->part[i].yaw   = (uint8_t)cp_rng_int(r, 256);
        g->part[i].pitch = (int8_t)(cp_rng_int(r, 128) - 64);
    }
    cp3_genome_normalise(g, budget);
}

/* Reproduction copies the parent and perturbs it. Nothing here knows what a
 * good body plan is - selection is the only thing that decides. */
void cp3_genome_mutate(Cp3Genome *g, CpRng *r, int budget, float rate)
{
    if (cp_rng_f(r) < rate * 0.5f) {
        int d = cp_rng_int(r, 2) ? 1 : -1;
        int n = g->nseg + d;
        g->nseg = (uint8_t)(n < 2 ? 2 : (n > CP3_MAX_SEG ? CP3_MAX_SEG : n));
    }
    if (cp_rng_f(r) < rate)
        g->girth = (uint8_t)(g->girth + cp_rng_int(r, 41) - 20);

    for (int i = 0; i < CP3_MAX_PARTS; i++) {
        if (cp_rng_f(r) >= rate) continue;
        float roll = cp_rng_f(r);
        if (g->part[i].type == CP3_NONE) {
            if (roll < 0.55f) {              /* gain a part */
                g->part[i].type  = (uint8_t)(1 + cp_rng_int(r, CP3_PART_COUNT - 1));
                g->part[i].seg   = (uint8_t)cp_rng_int(r, g->nseg ? g->nseg : 1);
                g->part[i].yaw   = (uint8_t)cp_rng_int(r, 256);
                g->part[i].pitch = (int8_t)(cp_rng_int(r, 128) - 64);
            }
        } else if (roll < 0.18f) {           /* lose a part */
            g->part[i].type = CP3_NONE;
        } else if (roll < 0.42f) {           /* swap what it is */
            g->part[i].type = (uint8_t)(1 + cp_rng_int(r, CP3_PART_COUNT - 1));
        } else {                             /* move it */
            g->part[i].seg   = (uint8_t)cp_rng_int(r, g->nseg ? g->nseg : 1);
            g->part[i].yaw   = (uint8_t)(g->part[i].yaw + cp_rng_int(r, 65) - 32);
            g->part[i].pitch = (int8_t)(g->part[i].pitch + cp_rng_int(r, 33) - 16);
        }
    }
    cp3_genome_normalise(g, budget);
}

void cp3_genome_from_action(Cp3Genome *g, const float *d, int budget)
{
    cp3_genome_clear(g);
    for (int i = 0; i < CP3_MAX_PARTS; i++) {
        const float *q = d + i * 4;
        float tv = q[0] < -1.0f ? -1.0f : (q[0] > 1.0f ? 1.0f : q[0]);
        int t = (int)((tv + 1.0f) * 0.5f * (float)(CP3_PART_COUNT - 1) + 0.5f);
        if (t < 0) t = 0;
        if (t >= CP3_PART_COUNT) t = CP3_PART_COUNT - 1;
        g->part[i].type = (uint8_t)t;

        float sv = q[1] < -1.0f ? -1.0f : (q[1] > 1.0f ? 1.0f : q[1]);
        int sg = (int)((sv + 1.0f) * 0.5f * (float)(CP3_MAX_SEG - 1) + 0.5f);
        g->part[i].seg = (uint8_t)(sg < 0 ? 0 : (sg >= CP3_MAX_SEG ? CP3_MAX_SEG - 1 : sg));

        float yv = q[2] < -1.0f ? -1.0f : (q[2] > 1.0f ? 1.0f : q[2]);
        g->part[i].yaw = (uint8_t)((int)((yv + 1.0f) * 0.5f * 255.0f + 0.5f) & 0xFF);

        float pv = q[3] < -1.0f ? -1.0f : (q[3] > 1.0f ? 1.0f : q[3]);
        g->part[i].pitch = (int8_t)(pv * 63.0f);
    }
    float nv = d[CP3_MAX_PARTS * 4];
    float gv = d[CP3_MAX_PARTS * 4 + 1];
    if (nv < -1.0f) nv = -1.0f;
    if (nv > 1.0f) nv = 1.0f;
    if (gv < -1.0f) gv = -1.0f;
    if (gv > 1.0f) gv = 1.0f;
    g->nseg  = (uint8_t)(2 + (int)((nv + 1.0f) * 0.5f * (float)(CP3_MAX_SEG - 2) + 0.5f));
    g->girth = (uint8_t)(40 + (int)((gv + 1.0f) * 0.5f * 200.0f));
    cp3_genome_normalise(g, budget);
}

void cp3_genome_autodesign(Cp3Genome *g, CpRng *r, int budget, int style)
{
    cp3_genome_clear(g);
    int slot = 0, spent = 0;
    g->nseg = 3;
    g->girth = 120;
    spent += 4;                      /* the third segment */

    #define BUY(TYPE, SEG, YAW, PITCH)                                        \
        do {                                                                  \
            if (slot < CP3_MAX_PARTS && spent + cp3_part_cost(TYPE) <= budget) { \
                g->part[slot].type = (uint8_t)(TYPE);                         \
                g->part[slot].seg = (uint8_t)(SEG);                           \
                g->part[slot].yaw = (uint8_t)(YAW);                           \
                g->part[slot].pitch = (int8_t)(PITCH);                        \
                spent += cp3_part_cost(TYPE);                                 \
                slot++;                                                       \
            }                                                                 \
        } while (0)

    switch (style % CP3_STYLE_COUNT) {
    case CP3_STYLE_HUNTER:
        BUY(CP3_JAW, 0, 0, 0);
        BUY(CP3_TAIL, 2, 128, 0);
        BUY(CP3_FIN, 1, 64, 0);
        BUY(CP3_FIN, 1, 192, 0);
        BUY(CP3_EYE, 0, 40, 20);
        BUY(CP3_EYE, 0, 216, 20);
        BUY(CP3_SPIKE, 1, 0, -40);
        BUY(CP3_TAIL, 2, 128, 30);
        BUY(CP3_PLATE, 1, 128, 0);
        BUY(CP3_SPIKE, 2, 0, 40);
        break;
    case CP3_STYLE_DIVER:
        BUY(CP3_FILTER, 0, 0, 0);
        BUY(CP3_TAIL, 2, 128, 0);
        BUY(CP3_FIN, 1, 64, 0);
        BUY(CP3_FIN, 1, 192, 0);
        BUY(CP3_LUNG, 1, 128, 0);
        BUY(CP3_LIGHT, 0, 0, -50);   /* a lamp on the brow */
        BUY(CP3_EYE, 0, 40, 20);
        BUY(CP3_EYE, 0, 216, 20);
        BUY(CP3_LIGHT, 2, 128, 40);
        BUY(CP3_JAW, 0, 0, 0);
        break;
    default: /* grazer */
        BUY(CP3_FILTER, 0, 0, 0);
        BUY(CP3_TAIL, 2, 128, 0);
        BUY(CP3_FIN, 1, 64, 0);
        BUY(CP3_FIN, 1, 192, 0);
        BUY(CP3_FILTER, 0, 0, 30);
        BUY(CP3_EYE, 0, 40, 20);
        BUY(CP3_FIN, 2, 128, 60);
        BUY(CP3_TAIL, 2, 128, -30);
        BUY(CP3_LUNG, 1, 128, 0);
        BUY(CP3_EYE, 0, 216, 20);
        break;
    }
    #undef BUY

    if (r) {
        for (int i = 0; i < CP3_MAX_PARTS; i++)
            if (g->part[i].type != CP3_NONE)
                g->part[i].yaw = (uint8_t)((g->part[i].yaw + cp_rng_int(r, 9) - 4) & 0xFF);
    }
    cp3_genome_normalise(g, budget);
}

void cp3_genome_stats(const Cp3Genome *g, Cp3Stats *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < CP3_MAX_PARTS; i++) {
        int t = g->part[i].type;
        if (t > CP3_NONE && t < CP3_PART_COUNT) { o->n[t]++; o->n_parts++; }
    }
    o->cost = (int16_t)cp3_genome_cost(g);
    const uint8_t *n = o->n;

    int nseg = g->nseg < 2 ? 2 : g->nseg;
    o->radius = 5.0f + (float)g->girth / 255.0f * 9.0f;
    o->length = (float)nseg * o->radius * 1.45f;

    /* mass is real here: a long, thick, plated animal is a slow one */
    float mass = 1.0f + 0.05f * o->n_parts + 0.07f * n[CP3_PLATE]
                      + 0.10f * (float)(nseg - 2)
                      + 0.35f * ((float)g->girth / 255.0f);

    o->speed = (95.0f + 62.0f * n[CP3_TAIL]) / mass;
    o->accel = (250.0f + 130.0f * n[CP3_TAIL]) / mass;
    /* fins turn you; a long body resists turning */
    o->turn  = (2.10f + 0.95f * n[CP3_FIN]) / (0.80f + 0.11f * (float)nseg);
    o->drag  = 2.1f;

    o->hp_max = 120.0f + 20.0f * n[CP3_PLATE] + 10.0f * n[CP3_SPIKE]
                       + 14.0f * (float)(nseg - 2);
    o->armor  = 0.085f * n[CP3_PLATE] + 0.045f * n[CP3_SPIKE];
    if (o->armor > 0.62f) o->armor = 0.62f;

    o->bite      = n[CP3_JAW]   ? 34.0f : 0.0f;
    o->spike_dmg = n[CP3_SPIKE] ? 22.0f : 0.0f;

    o->filter_eff = 0.78f * n[CP3_FILTER];
    o->carn_eff   = 0.85f * n[CP3_JAW];

    /* Eyes give reach, but reach is multiplied by how much light there is.
     * Photophores are the only way to buy sight that the depth cannot take. */
    o->percep = 245.0f + 95.0f * n[CP3_EYE];
    if (o->percep > 640.0f) o->percep = 640.0f;
    o->light  = 0.30f * n[CP3_LIGHT];

    o->buoy   = 0.25f + 0.30f * n[CP3_LUNG];
    o->upkeep = 0.55f + 0.17f * o->n_parts + 0.14f * (float)(nseg - 2)
                      + 0.22f * ((float)g->girth / 255.0f);
}
