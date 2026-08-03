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
    g->prof[0] = 110; g->prof[1] = 190; g->prof[2] = 150; g->prof[3] = 70;
    g->hue = 130; g->hue2 = 30; g->sat = 150; g->val = 190;
    g->pattern = CP3_PAT_PLAIN; g->pscale = 120;
    memset(g->lump, 0, sizeof(g->lump));
}

/* Catmull-Rom-ish blend across the four profile stations. Smooth, so a
 * mutation to one station bulges the body rather than creating a step. */
float cp3_profile(const Cp3Genome *g, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float x = t * 3.0f;
    int i = (int)x;
    if (i > 2) i = 2;
    float f = x - (float)i;
    float a = (float)g->prof[i] / 255.0f, b = (float)g->prof[i + 1] / 255.0f;
    float sm = f * f * (3.0f - 2.0f * f);          /* smoothstep the segment */
    return 0.30f + 1.50f * (a + (b - a) * sm);
}

static void hsv2rgb(float h, float s, float v, float *out)
{
    h = h - floorf(h);
    float i = floorf(h * 6.0f), f = h * 6.0f - i;
    float p = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
    switch (((int)i) % 6) {
    case 0: out[0]=v; out[1]=t; out[2]=p; break;
    case 1: out[0]=q; out[1]=v; out[2]=p; break;
    case 2: out[0]=p; out[1]=v; out[2]=t; break;
    case 3: out[0]=p; out[1]=q; out[2]=v; break;
    case 4: out[0]=t; out[1]=p; out[2]=v; break;
    default: out[0]=v; out[1]=p; out[2]=q; break;
    }
}

void cp3_genome_colour(const Cp3Genome *g, float *rgb, float *rgb2)
{
    float s = 0.25f + 0.65f * ((float)g->sat / 255.0f);
    /* capped below 1.0: the renderer tonemaps, so an albedo near white
     * plus three light terms rolls off to a flat pale blob */
    float v = 0.30f + 0.46f * ((float)g->val / 255.0f);
    if (rgb)  hsv2rgb((float)g->hue / 255.0f, s, v, rgb);
    /* the pattern colour is deliberately pushed apart in value as well as hue,
     * or the markings vanish the moment the palette quantises */
    if (rgb2) hsv2rgb((float)g->hue2 / 255.0f, s * 0.9f,
                      v > 0.62f ? v * 0.48f : v * 1.75f, rgb2);
}

int cp3_genome_cost(const Cp3Genome *g)
{
    int c = 0;
    for (int i = 0; i < CP3_MAX_PARTS; i++)
        c += cp3_part_cost(g->part[i].type) * (g->part[i].mirror ? 2 : 1);
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
    g->part[0].type = CP3_FILTER; g->part[0].seg = 0; g->part[0].yaw = 0;   g->part[0].scale = 128;
    g->part[1].type = CP3_TAIL;   g->part[1].seg = 2; g->part[1].yaw = 128; g->part[1].scale = 128;
    g->part[2].type = CP3_FIN;    g->part[2].seg = 1; g->part[2].yaw = 64;  g->part[2].scale = 128;
    g->part[2].mirror = 1;                          /* one gene, a pair of fins */
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
        g->part[i].type   = (uint8_t)(1 + cp_rng_int(r, CP3_PART_COUNT - 1));
        g->part[i].seg    = (uint8_t)cp_rng_int(r, g->nseg);
        g->part[i].yaw    = (uint8_t)cp_rng_int(r, 256);
        g->part[i].pitch  = (int8_t)(cp_rng_int(r, 128) - 64);
        g->part[i].scale  = (uint8_t)cp_rng_int(r, 256);
        g->part[i].mirror = (uint8_t)(cp_rng_f(r) < 0.45f);
    }
    for (int i = 0; i < 4; i++) g->prof[i] = (uint8_t)cp_rng_int(r, 256);
    g->arch  = (int8_t)(cp_rng_int(r, 128) - 64);
    g->sweep = (int8_t)(cp_rng_int(r, 96) - 48);
    g->hue   = (uint8_t)cp_rng_int(r, 256);
    g->hue2  = (uint8_t)cp_rng_int(r, 256);
    g->sat   = (uint8_t)(60 + cp_rng_int(r, 196));
    g->val   = (uint8_t)(70 + cp_rng_int(r, 186));
    g->pattern = (uint8_t)cp_rng_int(r, CP3_PAT_COUNT);
    g->pscale  = (uint8_t)(40 + cp_rng_int(r, 216));
    for (int i = 0; i < CP3_MAX_SEG; i++)
        g->lump[i] = (int8_t)(cp_rng_int(r, 97) - 48);
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

    /* Shape and colour drift every generation. These are what make a lineage
     * recognisable and a population varied; without them every fish is the
     * same silhouette in one of six tints. */
    for (int i = 0; i < 4; i++)
        if (cp_rng_f(r) < rate)
            g->prof[i] = (uint8_t)(g->prof[i] + cp_rng_int(r, 61) - 30);
    if (cp_rng_f(r) < rate) g->arch  = (int8_t)(g->arch + cp_rng_int(r, 33) - 16);
    if (cp_rng_f(r) < rate) g->sweep = (int8_t)(g->sweep + cp_rng_int(r, 25) - 12);
    if (cp_rng_f(r) < rate) g->hue   = (uint8_t)(g->hue + cp_rng_int(r, 25) - 12);
    if (cp_rng_f(r) < rate * 0.6f) g->hue2 = (uint8_t)(g->hue2 + cp_rng_int(r, 41) - 20);
    if (cp_rng_f(r) < rate * 0.6f) g->sat  = (uint8_t)(g->sat + cp_rng_int(r, 41) - 20);
    if (cp_rng_f(r) < rate * 0.6f) g->val  = (uint8_t)(g->val + cp_rng_int(r, 41) - 20);
    if (cp_rng_f(r) < rate * 0.3f) g->pattern = (uint8_t)cp_rng_int(r, CP3_PAT_COUNT);
    if (cp_rng_f(r) < rate * 0.5f) g->pscale = (uint8_t)(g->pscale + cp_rng_int(r, 61) - 30);
    for (int i = 0; i < CP3_MAX_SEG; i++)
        if (cp_rng_f(r) < rate * 0.7f)
            g->lump[i] = (int8_t)(g->lump[i] + cp_rng_int(r, 41) - 20);

    for (int i = 0; i < CP3_MAX_PARTS; i++) {
        if (cp_rng_f(r) >= rate) continue;
        float roll = cp_rng_f(r);
        if (g->part[i].type == CP3_NONE) {
            if (roll < 0.55f) {              /* gain a part */
                g->part[i].type   = (uint8_t)(1 + cp_rng_int(r, CP3_PART_COUNT - 1));
                g->part[i].seg    = (uint8_t)cp_rng_int(r, g->nseg ? g->nseg : 1);
                g->part[i].yaw    = (uint8_t)cp_rng_int(r, 256);
                g->part[i].pitch  = (int8_t)(cp_rng_int(r, 128) - 64);
                g->part[i].scale  = (uint8_t)cp_rng_int(r, 256);
                g->part[i].mirror = (uint8_t)(cp_rng_f(r) < 0.45f);
            }
        } else if (roll < 0.16f) {           /* lose a part */
            g->part[i].type = CP3_NONE;
        } else if (roll < 0.34f) {           /* swap what it is */
            g->part[i].type = (uint8_t)(1 + cp_rng_int(r, CP3_PART_COUNT - 1));
        } else if (roll < 0.46f) {           /* gain or lose the mirrored twin */
            g->part[i].mirror = (uint8_t)(!g->part[i].mirror);
        } else if (roll < 0.68f) {           /* resize it */
            g->part[i].scale = (uint8_t)(g->part[i].scale + cp_rng_int(r, 81) - 40);
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

    /* paired placements are bought as one mirrored gene, at double cost */
    #define BUYM(TYPE, SEG, YAW, PITCH, MIR)                                  \
        do {                                                                  \
            int _c = cp3_part_cost(TYPE) * ((MIR) ? 2 : 1);                   \
            if (slot < CP3_MAX_PARTS && spent + _c <= budget) {               \
                g->part[slot].type = (uint8_t)(TYPE);                         \
                g->part[slot].seg = (uint8_t)(SEG);                           \
                g->part[slot].yaw = (uint8_t)(YAW);                           \
                g->part[slot].pitch = (int8_t)(PITCH);                        \
                g->part[slot].scale = 128;                                    \
                g->part[slot].mirror = (uint8_t)(MIR);                        \
                spent += _c;                                                  \
                slot++;                                                       \
            }                                                                 \
        } while (0)
    #define BUY(TYPE, SEG, YAW, PITCH) BUYM(TYPE, SEG, YAW, PITCH, 0)

    switch (style % CP3_STYLE_COUNT) {
    case CP3_STYLE_HUNTER:
        g->prof[0] = 95; g->prof[1] = 205; g->prof[2] = 130; g->prof[3] = 45;
        g->hue = 12; g->hue2 = 200; g->sat = 170; g->val = 175;
        g->pattern = CP3_PAT_COUNTER; g->pscale = 110;
        BUY(CP3_JAW, 0, 0, 0);
        BUY(CP3_TAIL, 2, 128, 0);
        BUYM(CP3_FIN, 1, 64, 0, 1);
        BUYM(CP3_EYE, 0, 40, 20, 1);
        BUY(CP3_SPIKE, 1, 0, -40);
        BUY(CP3_TAIL, 2, 128, 30);
        BUY(CP3_PLATE, 1, 128, 0);
        BUY(CP3_SPIKE, 2, 0, 40);
        break;
    case CP3_STYLE_DIVER:
        g->prof[0] = 150; g->prof[1] = 175; g->prof[2] = 165; g->prof[3] = 60;
        g->hue = 175; g->hue2 = 130; g->sat = 120; g->val = 120;
        g->pattern = CP3_PAT_SPOTS; g->pscale = 190;
        BUY(CP3_FILTER, 0, 0, 0);
        BUY(CP3_TAIL, 2, 128, 0);
        BUYM(CP3_FIN, 1, 64, 0, 1);
        BUY(CP3_LUNG, 1, 128, 0);
        BUY(CP3_LIGHT, 0, 0, -50);   /* a lamp on the brow */
        BUY(CP3_EYE, 0, 40, 20);
        BUY(CP3_EYE, 0, 216, 20);
        BUY(CP3_LIGHT, 2, 128, 40);
        BUY(CP3_JAW, 0, 0, 0);
        break;
    default: /* grazer */
        g->prof[0] = 120; g->prof[1] = 215; g->prof[2] = 175; g->prof[3] = 55;
        g->hue = 95; g->hue2 = 35; g->sat = 165; g->val = 205;
        g->pattern = CP3_PAT_BANDS; g->pscale = 130;
        BUY(CP3_FILTER, 0, 0, 0);
        BUY(CP3_TAIL, 2, 128, 0);
        BUYM(CP3_FIN, 1, 64, 0, 1);
        BUY(CP3_FILTER, 0, 0, 30);
        BUY(CP3_EYE, 0, 40, 20);
        BUY(CP3_FIN, 2, 128, 60);
        BUY(CP3_TAIL, 2, 128, -30);
        BUY(CP3_LUNG, 1, 128, 0);
        BUY(CP3_EYE, 0, 216, 20);
        break;
    }
    #undef BUY
    #undef BUYM

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
        if (t <= CP3_NONE || t >= CP3_PART_COUNT) continue;
        int mult = g->part[i].mirror ? 2 : 1;      /* a pair of fins is two fins */
        o->n[t] = (uint8_t)(o->n[t] + mult);
        o->n_parts = (uint8_t)(o->n_parts + mult);
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
