#include "cpore/land.h"
#include <string.h>
#include <math.h>

/* Stage-3 body plans.
 *
 * The stage has two ways to win and one budget to buy them with. Claws, horns
 * and plates make you a predator; voice sacs and plumes make you charming.
 * Both fill the same meter, so the interesting genomes are the ones that pick
 * a side, and the interesting failures are the ones that try for both. */

/* Generation budgets. The first one has to buy a mouth and two leg pairs
 * before anything else - at 45 it could not, so every gen-0 build was the same
 * three parts and the styles were indistinguishable. */
const int CP4_GEN_BUDGET[CP4_GENERATIONS] = { 64, 105, 150, 205 };

static const struct { const char *name; int cost; } PARTS[CP4_PART_COUNT] = {
    { "-",        0 },
    { "graze",    6 },
    { "jaw",     14 },
    { "beak",    18 },
    { "leg",      9 },
    { "foot",     7 },
    { "claw",    12 },
    { "horn",    13 },
    { "plate",   13 },
    { "eye",      7 },
    { "ear",      8 },
    { "voice",   12 },
    { "plume",   11 },
    { "wing",    16 },
};

const char *cp4_part_name(int t)
{
    return (t >= 0 && t < CP4_PART_COUNT) ? PARTS[t].name : "?";
}

int cp4_part_cost(int t)
{
    return (t >= 0 && t < CP4_PART_COUNT) ? PARTS[t].cost : 0;
}

void cp4_genome_clear(Cp4Genome *g)
{
    memset(g, 0, sizeof(*g));
    g->nseg = 3;
    g->girth = 130;
    g->prof[0] = 130; g->prof[1] = 195; g->prof[2] = 160; g->prof[3] = 85;
    g->hue = 30; g->hue2 = 130; g->sat = 150; g->val = 175;
    g->pattern = CP4_PAT_PLAIN; g->pscale = 120;
}

float cp4_profile(const Cp4Genome *g, float t)
{
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    float x = t * 3.0f;
    int i = (int)x;
    if (i > 2) i = 2;
    float f = x - (float)i;
    float a = (float)g->prof[i] / 255.0f, b = (float)g->prof[i + 1] / 255.0f;
    float sm = f * f * (3.0f - 2.0f * f);
    /* Kept narrower than the aquatic profile. Water holds a blob up; on land
     * a body two and a half times its own nominal radius just drags. */
    return 0.34f + 1.06f * (a + (b - a) * sm);
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

void cp4_genome_colour(const Cp4Genome *g, float *rgb, float *rgb2)
{
    float s = 0.25f + 0.65f * ((float)g->sat / 255.0f);
    float v = 0.30f + 0.46f * ((float)g->val / 255.0f);
    if (rgb)  hsv2rgb((float)g->hue / 255.0f, s, v, rgb);
    if (rgb2) hsv2rgb((float)g->hue2 / 255.0f, s * 0.9f,
                      v > 0.55f ? v * 0.48f : v * 1.75f, rgb2);
}

int cp4_genome_cost(const Cp4Genome *g)
{
    int c = 0;
    for (int i = 0; i < CP4_MAX_PARTS; i++)
        c += cp4_part_cost(g->part[i].type) * (g->part[i].mirror ? 2 : 1);
    c += (g->nseg > 2 ? (g->nseg - 2) * 5 : 0);
    return c;
}

static int count_of(const Cp4Genome *g, int t)
{
    int n = 0;
    for (int i = 0; i < CP4_MAX_PARTS; i++)
        if (g->part[i].type == t) n += (g->part[i].mirror ? 2 : 1);
    return n;
}

static int mouths(const Cp4Genome *g)
{
    return count_of(g, CP4_MOUTH_G) + count_of(g, CP4_MOUTH_C) + count_of(g, CP4_MOUTH_O);
}

static int legs(const Cp4Genome *g) { return count_of(g, CP4_LEG); }

void cp4_genome_starter(Cp4Genome *g)
{
    cp4_genome_clear(g);
    g->part[0].type = CP4_MOUTH_G; g->part[0].seg = 0; g->part[0].yaw = 0; g->part[0].scale = 128;
    g->part[1].type = CP4_LEG; g->part[1].seg = 0; g->part[1].yaw = 60;
    g->part[1].pitch = -40; g->part[1].scale = 128; g->part[1].mirror = 1;
    g->part[2].type = CP4_LEG; g->part[2].seg = 2; g->part[2].yaw = 60;
    g->part[2].pitch = -40; g->part[2].scale = 128; g->part[2].mirror = 1;
    g->part[3].type = CP4_EYE; g->part[3].seg = 0; g->part[3].yaw = 34;
    g->part[3].pitch = 30; g->part[3].scale = 128; g->part[3].mirror = 1;
}

void cp4_genome_normalise(Cp4Genome *g, int budget)
{
    if (g->nseg < 2) g->nseg = 2;
    if (g->nseg > CP4_MAX_SEG) g->nseg = CP4_MAX_SEG;

    Cp4Part tmp[CP4_MAX_PARTS];
    int n = 0;
    for (int i = 0; i < CP4_MAX_PARTS; i++) {
        int t = g->part[i].type;
        if (t <= CP4_NONE || t >= CP4_PART_COUNT) continue;
        tmp[n] = g->part[i];
        if (tmp[n].seg >= g->nseg) tmp[n].seg = (uint8_t)(g->nseg - 1);
        n++;
    }
    memset(g->part, 0, sizeof(g->part));
    for (int i = 0; i < n; i++) g->part[i] = tmp[i];

    if (!mouths(g)) {
        int slot = (n < CP4_MAX_PARTS) ? n++ : CP4_MAX_PARTS - 1;
        g->part[slot].type = CP4_MOUTH_G;
        g->part[slot].seg = 0;
        g->part[slot].yaw = 0;
        g->part[slot].scale = 128;
        g->part[slot].mirror = 0;
    }
    /* A land animal with no legs cannot cross its own world, so one pair is
     * granted the same way a mouth is. It is the terrestrial equivalent of
     * needing something to eat with. */
    if (!legs(g)) {
        int slot = -1;
        for (int i = 0; i < CP4_MAX_PARTS; i++)
            if (g->part[i].type == CP4_NONE) { slot = i; break; }
        if (slot < 0) slot = CP4_MAX_PARTS - 1;
        g->part[slot].type = CP4_LEG;
        g->part[slot].seg = 0;
        g->part[slot].yaw = 60;
        g->part[slot].pitch = -40;
        g->part[slot].scale = 128;
        g->part[slot].mirror = 1;
    }

    for (int guard = 0; guard < 96 && cp4_genome_cost(g) > budget; guard++) {
        int worst = -1;
        for (int i = 0; i < CP4_MAX_PARTS; i++) {
            int t = g->part[i].type;
            if (t == CP4_NONE) continue;
            int is_mouth = (t == CP4_MOUTH_G || t == CP4_MOUTH_C || t == CP4_MOUTH_O);
            if (is_mouth && mouths(g) <= 1) continue;
            if (t == CP4_LEG && legs(g) <= 2) continue;
            if (worst < 0 || cp4_part_cost(t) > cp4_part_cost(g->part[worst].type)) worst = i;
        }
        if (worst >= 0) {
            /* dropping the mirror is half the saving for none of the loss */
            if (g->part[worst].mirror && cp4_part_cost(g->part[worst].type) * 2 > 12)
                g->part[worst].mirror = 0;
            else
                g->part[worst].type = CP4_NONE;
        } else if (g->nseg > 2) {
            g->nseg--;
            for (int i = 0; i < CP4_MAX_PARTS; i++)
                if (g->part[i].seg >= g->nseg) g->part[i].seg = (uint8_t)(g->nseg - 1);
        } else {
            break;
        }
    }
}

void cp4_genome_random(Cp4Genome *g, CpRng *r, int budget)
{
    cp4_genome_clear(g);
    g->nseg = (uint8_t)(2 + cp_rng_int(r, CP4_MAX_SEG - 1));
    g->girth = (uint8_t)(70 + cp_rng_int(r, 170));
    for (int i = 0; i < CP4_MAX_PARTS; i++) {
        if (cp_rng_f(r) < 0.28f) continue;
        g->part[i].type   = (uint8_t)(1 + cp_rng_int(r, CP4_PART_COUNT - 1));
        g->part[i].seg    = (uint8_t)cp_rng_int(r, g->nseg);
        g->part[i].yaw    = (uint8_t)cp_rng_int(r, 256);
        g->part[i].pitch  = (int8_t)(cp_rng_int(r, 128) - 64);
        g->part[i].scale  = (uint8_t)cp_rng_int(r, 256);
        g->part[i].mirror = (uint8_t)(cp_rng_f(r) < 0.55f);
    }
    for (int i = 0; i < 4; i++) g->prof[i] = (uint8_t)cp_rng_int(r, 256);
    for (int i = 0; i < CP4_MAX_SEG; i++) g->lump[i] = (int8_t)(cp_rng_int(r, 97) - 48);
    g->arch  = (int8_t)(cp_rng_int(r, 128) - 64);
    g->sweep = (int8_t)(cp_rng_int(r, 80) - 40);
    g->hue   = (uint8_t)cp_rng_int(r, 256);
    g->hue2  = (uint8_t)cp_rng_int(r, 256);
    g->sat   = (uint8_t)(60 + cp_rng_int(r, 196));
    g->val   = (uint8_t)(60 + cp_rng_int(r, 170));
    g->pattern = (uint8_t)cp_rng_int(r, CP4_PAT_COUNT);
    g->pscale  = (uint8_t)(40 + cp_rng_int(r, 216));
    cp4_genome_normalise(g, budget);
}

void cp4_genome_mutate(Cp4Genome *g, CpRng *r, int budget, float rate)
{
    if (cp_rng_f(r) < rate * 0.5f) {
        int d = cp_rng_int(r, 2) ? 1 : -1;
        int n = g->nseg + d;
        g->nseg = (uint8_t)(n < 2 ? 2 : (n > CP4_MAX_SEG ? CP4_MAX_SEG : n));
    }
    if (cp_rng_f(r) < rate) g->girth = (uint8_t)(g->girth + cp_rng_int(r, 41) - 20);
    for (int i = 0; i < 4; i++)
        if (cp_rng_f(r) < rate) g->prof[i] = (uint8_t)(g->prof[i] + cp_rng_int(r, 61) - 30);
    for (int i = 0; i < CP4_MAX_SEG; i++)
        if (cp_rng_f(r) < rate * 0.7f) g->lump[i] = (int8_t)(g->lump[i] + cp_rng_int(r, 41) - 20);
    if (cp_rng_f(r) < rate) g->arch  = (int8_t)(g->arch + cp_rng_int(r, 33) - 16);
    if (cp_rng_f(r) < rate) g->sweep = (int8_t)(g->sweep + cp_rng_int(r, 25) - 12);
    if (cp_rng_f(r) < rate) g->hue   = (uint8_t)(g->hue + cp_rng_int(r, 25) - 12);
    if (cp_rng_f(r) < rate * 0.6f) g->hue2 = (uint8_t)(g->hue2 + cp_rng_int(r, 41) - 20);
    if (cp_rng_f(r) < rate * 0.6f) g->sat  = (uint8_t)(g->sat + cp_rng_int(r, 41) - 20);
    if (cp_rng_f(r) < rate * 0.6f) g->val  = (uint8_t)(g->val + cp_rng_int(r, 41) - 20);
    if (cp_rng_f(r) < rate * 0.3f) g->pattern = (uint8_t)cp_rng_int(r, CP4_PAT_COUNT);
    if (cp_rng_f(r) < rate * 0.5f) g->pscale = (uint8_t)(g->pscale + cp_rng_int(r, 61) - 30);

    for (int i = 0; i < CP4_MAX_PARTS; i++) {
        if (cp_rng_f(r) >= rate) continue;
        float roll = cp_rng_f(r);
        if (g->part[i].type == CP4_NONE) {
            if (roll < 0.55f) {
                g->part[i].type   = (uint8_t)(1 + cp_rng_int(r, CP4_PART_COUNT - 1));
                g->part[i].seg    = (uint8_t)cp_rng_int(r, g->nseg ? g->nseg : 1);
                g->part[i].yaw    = (uint8_t)cp_rng_int(r, 256);
                g->part[i].pitch  = (int8_t)(cp_rng_int(r, 128) - 64);
                g->part[i].scale  = (uint8_t)cp_rng_int(r, 256);
                g->part[i].mirror = (uint8_t)(cp_rng_f(r) < 0.55f);
            }
        } else if (roll < 0.15f) {
            g->part[i].type = CP4_NONE;
        } else if (roll < 0.33f) {
            g->part[i].type = (uint8_t)(1 + cp_rng_int(r, CP4_PART_COUNT - 1));
        } else if (roll < 0.45f) {
            g->part[i].mirror = (uint8_t)(!g->part[i].mirror);
        } else if (roll < 0.67f) {
            g->part[i].scale = (uint8_t)(g->part[i].scale + cp_rng_int(r, 81) - 40);
        } else {
            g->part[i].seg   = (uint8_t)cp_rng_int(r, g->nseg ? g->nseg : 1);
            g->part[i].yaw   = (uint8_t)(g->part[i].yaw + cp_rng_int(r, 65) - 32);
            g->part[i].pitch = (int8_t)(g->part[i].pitch + cp_rng_int(r, 33) - 16);
        }
    }
    cp4_genome_normalise(g, budget);
}

void cp4_genome_from_action(Cp4Genome *g, const float *d, int budget)
{
    cp4_genome_clear(g);
    for (int i = 0; i < CP4_MAX_PARTS; i++) {
        const float *q = d + i * 4;
        float tv = q[0] < -1.0f ? -1.0f : (q[0] > 1.0f ? 1.0f : q[0]);
        int t = (int)((tv + 1.0f) * 0.5f * (float)(CP4_PART_COUNT - 1) + 0.5f);
        if (t < 0) t = 0;
        if (t >= CP4_PART_COUNT) t = CP4_PART_COUNT - 1;
        g->part[i].type = (uint8_t)t;
        float sv = q[1] < -1.0f ? -1.0f : (q[1] > 1.0f ? 1.0f : q[1]);
        int sg = (int)((sv + 1.0f) * 0.5f * (float)(CP4_MAX_SEG - 1) + 0.5f);
        g->part[i].seg = (uint8_t)(sg < 0 ? 0 : (sg >= CP4_MAX_SEG ? CP4_MAX_SEG - 1 : sg));
        float yv = q[2] < -1.0f ? -1.0f : (q[2] > 1.0f ? 1.0f : q[2]);
        g->part[i].yaw = (uint8_t)((int)((yv + 1.0f) * 0.5f * 255.0f + 0.5f) & 0xFF);
        float pv = q[3] < -1.0f ? -1.0f : (q[3] > 1.0f ? 1.0f : q[3]);
        g->part[i].pitch = (int8_t)(pv * 63.0f);
        g->part[i].scale = 128;
        g->part[i].mirror = (uint8_t)(q[2] > 0.0f);
    }
    float nv = d[CP4_MAX_PARTS * 4], gv = d[CP4_MAX_PARTS * 4 + 1];
    if (nv < -1.0f) nv = -1.0f;
    if (nv > 1.0f) nv = 1.0f;
    if (gv < -1.0f) gv = -1.0f;
    if (gv > 1.0f) gv = 1.0f;
    g->nseg  = (uint8_t)(2 + (int)((nv + 1.0f) * 0.5f * (float)(CP4_MAX_SEG - 2) + 0.5f));
    g->girth = (uint8_t)(50 + (int)((gv + 1.0f) * 0.5f * 190.0f));
    cp4_genome_normalise(g, budget);
}

void cp4_genome_autodesign(Cp4Genome *g, CpRng *r, int budget, int style)
{
    cp4_genome_clear(g);
    int slot = 0, spent = 5;      /* the third segment */
    g->nseg = 3;

    #define BUYM(TYPE, SEG, YAW, PITCH, MIR)                                  \
        do {                                                                  \
            int _c = cp4_part_cost(TYPE) * ((MIR) ? 2 : 1);                   \
            if (slot < CP4_MAX_PARTS && spent + _c <= budget) {               \
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

    switch (style % CP4_STYLE_COUNT) {
    case CP4_STYLE_PREDATOR:
        g->hue = 8; g->hue2 = 200; g->sat = 175; g->val = 150;
        g->pattern = CP4_PAT_STRIPES; g->pscale = 130;
        g->prof[0] = 105; g->prof[1] = 200; g->prof[2] = 140; g->prof[3] = 60;
        BUYM(CP4_MOUTH_C, 0, 0, 0, 0);
        BUYM(CP4_LEG, 0, 60, -40, 1);
        BUYM(CP4_LEG, 2, 60, -40, 1);
        BUYM(CP4_EYE, 0, 34, 30, 1);
        BUYM(CP4_CLAW, 0, 50, -20, 1);
        BUYM(CP4_FOOT, 2, 70, -55, 1);
        BUYM(CP4_HORN, 0, 0, 40, 0);
        BUYM(CP4_PLATE, 1, 128, 40, 0);
        break;
    case CP4_STYLE_CHARMER:
        g->hue = 190; g->hue2 = 40; g->sat = 200; g->val = 190;
        g->pattern = CP4_PAT_SPOTS; g->pscale = 160;
        g->prof[0] = 140; g->prof[1] = 190; g->prof[2] = 175; g->prof[3] = 90;
        BUYM(CP4_MOUTH_G, 0, 0, 0, 0);
        BUYM(CP4_LEG, 0, 60, -40, 1);
        BUYM(CP4_VOICE, 0, 0, 25, 0);
        BUYM(CP4_LEG, 2, 60, -40, 1);
        BUYM(CP4_PLUME, 1, 128, 55, 1);
        BUYM(CP4_EYE, 0, 34, 30, 1);
        BUYM(CP4_EAR, 0, 90, 45, 1);
        BUYM(CP4_PLUME, 2, 128, 45, 0);
        break;
    default: /* grazer */
        g->hue = 60; g->hue2 = 20; g->sat = 140; g->val = 165;
        g->pattern = CP4_PAT_MOTTLE; g->pscale = 140;
        g->prof[0] = 125; g->prof[1] = 210; g->prof[2] = 180; g->prof[3] = 75;
        BUYM(CP4_MOUTH_G, 0, 0, 0, 0);
        BUYM(CP4_LEG, 0, 60, -40, 1);
        BUYM(CP4_LEG, 2, 60, -40, 1);
        BUYM(CP4_FOOT, 2, 70, -55, 1);
        BUYM(CP4_EYE, 0, 34, 30, 1);
        BUYM(CP4_EAR, 0, 90, 45, 1);
        BUYM(CP4_VOICE, 0, 0, 25, 0);
        BUYM(CP4_PLATE, 1, 128, 40, 0);
        break;
    }
    #undef BUYM

    if (r) {
        for (int i = 0; i < CP4_MAX_PARTS; i++)
            if (g->part[i].type != CP4_NONE)
                g->part[i].yaw = (uint8_t)((g->part[i].yaw + cp_rng_int(r, 9) - 4) & 0xFF);
    }
    cp4_genome_normalise(g, budget);
}

void cp4_genome_stats(const Cp4Genome *g, Cp4Stats *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < CP4_MAX_PARTS; i++) {
        int t = g->part[i].type;
        if (t <= CP4_NONE || t >= CP4_PART_COUNT) continue;
        int mult = g->part[i].mirror ? 2 : 1;
        o->n[t] = (uint8_t)(o->n[t] + mult);
        o->n_parts = (uint8_t)(o->n_parts + mult);
    }
    o->cost = (int16_t)cp4_genome_cost(g);
    const uint8_t *n = o->n;

    int nseg = g->nseg < 2 ? 2 : g->nseg;
    o->radius = 6.0f + (float)g->girth / 255.0f * 10.0f;
    /* a walking animal is roughly three to five times as long as it is wide;
     * nseg * 1.4 gave a seven-to-one slug at six segments */
    o->length = (1.6f + 0.62f * (float)nseg) * o->radius;

    /* On land mass is carried, not floated, so it costs more than in water */
    float mass = 1.0f + 0.06f * o->n_parts + 0.09f * n[CP4_PLATE]
                      + 0.12f * (float)(nseg - 2) + 0.45f * ((float)g->girth / 255.0f);

    /* Legs both propel and support: too few for the body and everything is
     * slow, which is the whole reason a heavy build needs more of them. */
    float support = (float)n[CP4_LEG] / (mass * 1.6f);
    if (support > 1.0f) support = 1.0f;
    o->speed = (70.0f + 34.0f * n[CP4_LEG]) / mass * (0.45f + 0.55f * support);
    o->accel = (430.0f + 150.0f * n[CP4_LEG]) / mass;
    o->turn  = (2.0f + 0.5f * n[CP4_LEG]) / (0.85f + 0.10f * (float)nseg);
    o->jump  = 120.0f + 26.0f * n[CP4_LEG] + 55.0f * n[CP4_WING];
    o->grip  = 0.25f + 0.22f * n[CP4_FOOT] + 0.05f * n[CP4_LEG];
    if (o->grip > 1.0f) o->grip = 1.0f;
    /* How high the trunk rides.
     *
     * This has to clear the *widest* part of the body, not the nominal radius:
     * the profile genes scale each segment by up to 1.4 and the lump genes add
     * another 40%, so a stand computed off o->radius alone leaves a fat build
     * ploughing through the dirt with its legs invisible underneath it. More
     * legs stand you taller, which is also what makes a long-legged build read
     * as fast. */
    float widest = 0.0f;
    for (int i = 0; i < nseg; i++) {
        float t = nseg > 1 ? (float)i / (float)(nseg - 1) : 0.0f;
        int li = i < CP4_MAX_SEG ? i : CP4_MAX_SEG - 1;
        float r = cp4_profile(g, t) * (1.0f + (float)g->lump[li] / 127.0f * 0.40f);
        if (r > widest) widest = r;
    }
    if (widest < 0.35f) widest = 0.35f;
    o->stand = o->radius * widest * (1.02f + 0.68f * (float)n[CP4_LEG] / (3.0f + n[CP4_LEG]));

    o->hp_max = 130.0f + 22.0f * n[CP4_PLATE] + 11.0f * n[CP4_HORN]
                       + 16.0f * (float)(nseg - 2);
    o->armor  = 0.085f * n[CP4_PLATE] + 0.04f * n[CP4_HORN];
    if (o->armor > 0.62f) o->armor = 0.62f;

    o->bite     = n[CP4_MOUTH_C] ? 30.0f : (n[CP4_MOUTH_O] ? 18.0f : 0.0f);
    o->claw_dmg = 24.0f * (n[CP4_CLAW] ? 1.0f : 0.0f) + (n[CP4_HORN] ? 20.0f : 0.0f);

    o->graze_eff = 0.80f * n[CP4_MOUTH_G] + 0.50f * n[CP4_MOUTH_O];
    o->carn_eff  = 0.85f * n[CP4_MOUTH_C] + 0.50f * n[CP4_MOUTH_O];

    o->sight   = 210.0f + 90.0f * n[CP4_EYE];
    if (o->sight > 660.0f) o->sight = 660.0f;
    /* ears see through what eyes cannot: cover, and the far side of a ridge */
    o->hearing = 90.0f + 105.0f * n[CP4_EAR];

    /* the other half of the stage */
    o->charm         = 0.65f * n[CP4_VOICE] + 0.80f * n[CP4_PLUME];
    o->social_reach  = 110.0f + 85.0f * n[CP4_VOICE];

    o->stamina = 100.0f + 18.0f * n[CP4_LEG] + 12.0f * n[CP4_FOOT];
    o->upkeep  = 0.50f + 0.15f * o->n_parts + 0.16f * (float)(nseg - 2)
                       + 0.30f * ((float)g->girth / 255.0f);
}
