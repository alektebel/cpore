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
 * three parts and the styles were indistinguishable.
 *
 * Raised again when the genome widened to sixteen slots and gained arms and
 * tails. The budget is what the design space costs to use, and leaving it at
 * 64 while the space grew by a third meant every rival nest could field a
 * bigger animal than the player could afford - measured over thirty seeds,
 * the four archetypes that fill their meter socially went from winning a
 * quarter of their runs to winning a twentieth, not because they had got
 * worse but because everything around them had got better. */
const int CP4_GEN_BUDGET[CP4_GENERATIONS] = { 82, 132, 186, 248 };

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
    { "fin",      9 },
    { "gill",    11 },
    { "digger",  12 },
    { "arm",     13 },
    { "tail",    10 },
};

static const char *STYLES[CP4_STYLE_COUNT] = {
    "grazer", "predator", "charmer", "swimmer", "flyer", "burrower"
};

const char *cp4_style_name(int st)
{
    return (st >= 0 && st < CP4_STYLE_COUNT) ? STYLES[st] : "?";
}

const char *cp4_medium_name(int m)
{
    static const char *M[CP4_MEDIUM_COUNT] = { "ground", "water", "air", "under" };
    return (m >= 0 && m < CP4_MEDIUM_COUNT) ? M[m] : "?";
}

int cp4_flora_medium(int type)
{
    switch (type) {
    case CP4_FLORA_KELP:  return CP4_IN_WATER;
    case CP4_FLORA_TUBER: return CP4_UNDER;
    default:              return CP4_ON_GROUND;
    }
}

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
    g->hue = 30; g->hue2 = 130; g->hue3 = 200; g->sat = 150; g->val = 175;
    g->pattern = CP4_PAT_PLAIN; g->pscale = 120;
    g->pattern2 = CP4_PAT_PLAIN; g->pscale2 = 170;
    for (int i = 0; i < CP4_MAX_PARTS; i++) { g->part[i].len = 128; g->part[i].bend = 0; }
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

static float clampf01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

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

void cp4_genome_colour(const Cp4Genome *g, float *rgb, float *rgb2, float *rgb3)
{
    float s = 0.25f + 0.65f * ((float)g->sat / 255.0f);
    float v = 0.30f + 0.46f * ((float)g->val / 255.0f);
    if (rgb)  hsv2rgb((float)g->hue / 255.0f, s, v, rgb);
    if (rgb2) hsv2rgb((float)g->hue2 / 255.0f, s * 0.9f,
                      v > 0.55f ? v * 0.48f : v * 1.75f, rgb2);
    /* The detail coat is the loud one. Spore's third slot is where the eye
     * markings and the warning flashes live, so it is deliberately more
     * saturated and further from the base value than the marking coat. */
    if (rgb3) hsv2rgb((float)g->hue3 / 255.0f, clampf01(s * 1.25f),
                      v > 0.5f ? 0.28f + v * 0.34f : 0.55f + v * 0.80f, rgb3);
}

int cp4_genome_cost(const Cp4Genome *g)
{
    int c = 0;
    for (int i = 0; i < CP4_MAX_PARTS; i++)
        c += cp4_part_cost(g->part[i].type) * (g->part[i].mirror ? 2 : 1);
    c += (g->nseg > 2 ? (g->nseg - 2) * 5 : 0);
    return c;
}

/* How many *slots* hold a part of this kind, as opposed to how many copies it
 * places. The trimmer has to guard on slots: a single mirrored mouth places
 * two copies, so a guard written against the copy count read "more than one
 * mouth" and cheerfully deleted the only one there was. */
static int slots_of(const Cp4Genome *g, int t)
{
    int n = 0;
    for (int i = 0; i < CP4_MAX_PARTS; i++) if (g->part[i].type == t) n++;
    return n;
}

static int mouth_slots(const Cp4Genome *g)
{
    return slots_of(g, CP4_MOUTH_G) + slots_of(g, CP4_MOUTH_C) + slots_of(g, CP4_MOUTH_O);
}

static int mover_slots(const Cp4Genome *g)
{
    return slots_of(g, CP4_LEG) + slots_of(g, CP4_FIN) + slots_of(g, CP4_WING);
}

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

    /* Grant the two things an animal cannot play without: something to eat
     * with, and something to move with.
     *
     * Both grants have to be able to displace an existing part, because a
     * genome can arrive with all twelve slots full and neither. Doing that
     * naively is how the two guarantees ended up fighting over the same slot -
     * the mouth grant overwrote the only leg, the mover grant then overwrote
     * the mouth back, and the genome came out with neither. Displacing the
     * least essential part instead, and never one the other guarantee is
     * relying on, makes the two independent. */
    {
        #define GRANT(WANT_TYPE, WANT_YAW, WANT_PITCH, WANT_MIRROR)               \
        do {                                                                      \
            int slot = -1;                                                        \
            for (int i = 0; i < CP4_MAX_PARTS; i++)                               \
                if (g->part[i].type == CP4_NONE) { slot = i; break; }              \
            if (slot < 0) {                                                       \
                /* full: displace the cheapest part that nothing depends on */    \
                int best_cost = 1 << 20;                                          \
                for (int i = 0; i < CP4_MAX_PARTS; i++) {                         \
                    int t = g->part[i].type;                                      \
                    int is_m = (t == CP4_MOUTH_G || t == CP4_MOUTH_C ||           \
                                t == CP4_MOUTH_O);                                \
                    int is_v = (t == CP4_LEG || t == CP4_FIN || t == CP4_WING);   \
                    if (is_m && mouth_slots(g) <= 1) continue;                    \
                    if (is_v && mover_slots(g) <= 1) continue;                    \
                    if (cp4_part_cost(t) < best_cost) {                           \
                        best_cost = cp4_part_cost(t); slot = i;                   \
                    }                                                             \
                }                                                                 \
            }                                                                     \
            if (slot >= 0) {                                                      \
                g->part[slot].type = (uint8_t)(WANT_TYPE);                        \
                g->part[slot].seg = 0;                                            \
                g->part[slot].yaw = (uint8_t)(WANT_YAW);                          \
                g->part[slot].pitch = (int8_t)(WANT_PITCH);                       \
                g->part[slot].scale = 128;                                        \
                g->part[slot].mirror = (uint8_t)(WANT_MIRROR);                    \
            }                                                                     \
        } while (0)

        if (!mouth_slots(g)) GRANT(CP4_MOUTH_G, 0, 0, 0);
        /* Fins or wings count as movers: a body that swims or flies is not
         * obliged to walk, and forcing legs on it was quietly taxing every
         * aquatic and aerial build for a part it never used. */
        if (!mover_slots(g)) GRANT(CP4_LEG, 60, -40, 1);
        #undef GRANT
    }

    /* Trim to budget along a ladder, cheapest loss first.
     *
     * Sorting purely by price strips a build of exactly the parts that define
     * it: a charmer authored at the top budget and normalised down to the
     * first one came out with no voice and no plume, because those were the
     * dearest things it owned. The ladder is
     *
     *   1. drop bilateral symmetry - halves a part's cost and keeps the
     *      capability entirely, so it is always the cheapest thing to give up
     *   2. delete a duplicate - the body still does that job, just less well
     *   3. delete a unique part - only now does the animal lose something it
     *      could do at all
     *   4. shorten the spine
     *
     * so a genome degrades into a smaller version of itself rather than into a
     * different animal. */
    for (int guard = 0; guard < 160 && cp4_genome_cost(g) > budget; guard++) {

        /* rung 1: symmetry */
        {
            int best = -1, best_cost = -1;
            for (int i = 0; i < CP4_MAX_PARTS; i++) {
                if (g->part[i].type == CP4_NONE || !g->part[i].mirror) continue;
                int t = g->part[i].type;
                /* dropping the mirror on the only leg pair would leave one leg,
                 * which movers() still counts, so no guard is needed here */
                int cost = cp4_part_cost(t);
                if (cost > best_cost) { best = i; best_cost = cost; }
            }
            if (best >= 0) { g->part[best].mirror = 0; continue; }
        }

        /* rungs 2 and 3: duplicates before uniques, dearest first within each */
        {
            int worst = -1, worst_unique = 2, worst_cost = -1;
            for (int i = 0; i < CP4_MAX_PARTS; i++) {
                int t = g->part[i].type;
                if (t == CP4_NONE) continue;
                int is_mouth = (t == CP4_MOUTH_G || t == CP4_MOUTH_C || t == CP4_MOUTH_O);
                if (is_mouth && mouth_slots(g) <= 1) continue;
                /* never trim the last thing that moves the animal */
                int is_mover = (t == CP4_LEG || t == CP4_FIN || t == CP4_WING);
                if (is_mover && mover_slots(g) <= 1) continue;
                /* nor the gill that is the only reason a swimmer is not drowning */
                if (t == CP4_GILL && slots_of(g, CP4_FIN) >= 1 && slots_of(g, CP4_GILL) <= 1)
                    continue;

                int slots = 0;
                for (int j = 0; j < CP4_MAX_PARTS; j++) if (g->part[j].type == t) slots++;
                int unique = slots > 1 ? 0 : 1;
                int cost = cp4_part_cost(t);
                if (worst < 0 || unique < worst_unique ||
                    (unique == worst_unique && cost > worst_cost)) {
                    worst = i; worst_unique = unique; worst_cost = cost;
                }
            }
            if (worst >= 0) { g->part[worst].type = CP4_NONE; continue; }
        }

        /* rung 4 */
        if (g->nseg > 2) {
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
        g->part[i].len    = (uint8_t)(40 + cp_rng_int(r, 216));
        g->part[i].bend   = (int8_t)(cp_rng_int(r, 128) - 64);
    }
    for (int i = 0; i < 4; i++) g->prof[i] = (uint8_t)cp_rng_int(r, 256);
    for (int i = 0; i < CP4_MAX_SEG; i++) g->lump[i] = (int8_t)(cp_rng_int(r, 97) - 48);
    for (int i = 0; i < CP4_MAX_SEG; i++) g->rise[i] = (int8_t)(cp_rng_int(r, 81) - 40);
    g->arch  = (int8_t)(cp_rng_int(r, 128) - 64);
    g->sweep = (int8_t)(cp_rng_int(r, 80) - 40);
    g->hue   = (uint8_t)cp_rng_int(r, 256);
    g->hue2  = (uint8_t)cp_rng_int(r, 256);
    g->hue3  = (uint8_t)cp_rng_int(r, 256);
    g->sat   = (uint8_t)(60 + cp_rng_int(r, 196));
    g->val   = (uint8_t)(60 + cp_rng_int(r, 170));
    g->pattern = (uint8_t)cp_rng_int(r, CP4_PAT_COUNT);
    g->pscale  = (uint8_t)(40 + cp_rng_int(r, 216));
    /* The detail coat is plain more often than not - three loud patterns on
     * one animal is camouflage against nothing and reads as static. */
    g->pattern2 = (uint8_t)(cp_rng_f(r) < 0.45f ? cp_rng_int(r, CP4_PAT_COUNT)
                                                : CP4_PAT_PLAIN);
    g->pscale2  = (uint8_t)(60 + cp_rng_int(r, 196));
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
    for (int i = 0; i < CP4_MAX_SEG; i++)
        if (cp_rng_f(r) < rate * 0.7f) g->rise[i] = (int8_t)(g->rise[i] + cp_rng_int(r, 33) - 16);
    if (cp_rng_f(r) < rate * 0.6f) g->hue2 = (uint8_t)(g->hue2 + cp_rng_int(r, 41) - 20);
    if (cp_rng_f(r) < rate * 0.5f) g->hue3 = (uint8_t)(g->hue3 + cp_rng_int(r, 49) - 24);
    if (cp_rng_f(r) < rate * 0.25f) g->pattern2 = (uint8_t)cp_rng_int(r, CP4_PAT_COUNT);
    if (cp_rng_f(r) < rate * 0.4f) g->pscale2 = (uint8_t)(g->pscale2 + cp_rng_int(r, 61) - 30);
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
                g->part[i].len    = (uint8_t)(40 + cp_rng_int(r, 216));
                g->part[i].bend   = (int8_t)(cp_rng_int(r, 128) - 64);
            }
        } else if (roll < 0.15f) {
            g->part[i].type = CP4_NONE;
        } else if (roll < 0.33f) {
            g->part[i].type = (uint8_t)(1 + cp_rng_int(r, CP4_PART_COUNT - 1));
        } else if (roll < 0.45f) {
            g->part[i].mirror = (uint8_t)(!g->part[i].mirror);
        } else if (roll < 0.60f) {
            g->part[i].scale = (uint8_t)(g->part[i].scale + cp_rng_int(r, 81) - 40);
        } else if (roll < 0.78f) {
            /* Limb proportions drift on their own. A lineage that only ever
             * swapped parts could never grow longer legs, which is the change
             * selection most obviously wants to be able to make. */
            g->part[i].len  = (uint8_t)(g->part[i].len + cp_rng_int(r, 71) - 35);
            g->part[i].bend = (int8_t)(g->part[i].bend + cp_rng_int(r, 49) - 24);
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
        const float *q = d + i * CP4_ACT_PART;
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
        /* Size and reach, which used to be constants. A design head that can
         * choose what a part *is* and where it goes but not how big it is or
         * how far it reaches is a parts bin, and the whole point of the two
         * extra numbers is that it stops being one. */
        float cv = q[4] < -1.0f ? -1.0f : (q[4] > 1.0f ? 1.0f : q[4]);
        g->part[i].scale = (uint8_t)(40 + (int)((cv + 1.0f) * 0.5f * 215.0f));
        float lv = q[5] < -1.0f ? -1.0f : (q[5] > 1.0f ? 1.0f : q[5]);
        g->part[i].len = (uint8_t)(40 + (int)((lv + 1.0f) * 0.5f * 215.0f));
        /* A long limb folds more; there is no separate control for it because
         * the two are not independent in anything that walks. */
        g->part[i].bend = (int8_t)((lv * 0.5f + 0.25f) * 90.0f);
        g->part[i].mirror = (uint8_t)(q[2] > 0.0f);
    }
    float nv = d[CP4_MAX_PARTS * CP4_ACT_PART], gv = d[CP4_MAX_PARTS * CP4_ACT_PART + 1];
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
                g->part[slot].len = 128;                                      \
                g->part[slot].bend = 40;                                      \
                spent += _c;                                                  \
                slot++;                                                       \
            }                                                                 \
        } while (0)
    /* Same, but with the limb's proportions spelled out. An archetype that
     * cannot say "long legs" or "a short heavy tail" is not an archetype, it
     * is a shopping list, and the six of them looked far more alike than their
     * stat blocks did. */
    #define BUYL(TYPE, SEG, YAW, PITCH, MIR, LEN, BEND)                       \
        do {                                                                  \
            int _s0 = slot;                                                   \
            BUYM(TYPE, SEG, YAW, PITCH, MIR);                                 \
            if (slot > _s0) {                                                 \
                g->part[_s0].len = (uint8_t)(LEN);                            \
                g->part[_s0].bend = (int8_t)(BEND);                           \
            }                                                                 \
        } while (0)

    /* Order is budget.
     *
     * The list is walked front to back and stops when the DNA runs out, so
     * where a part sits in it decides whether a gen-0 animal ever gets it.
     * Arms cost thirteen and are mirrored, which is twenty-six - two fifths of
     * the first budget - and slotting them in fifth quietly bought them
     * instead of the voice sac every archetype needs. Every archetype earns
     * its keep before it buys a limb it merely wants: tails and arms go last.
     */
    switch (style % CP4_STYLE_COUNT) {
    case CP4_STYLE_PREDATOR:
        g->hue = 8; g->hue2 = 200; g->sat = 175; g->val = 150;
        g->pattern = CP4_PAT_STRIPES; g->pscale = 130;
        g->prof[0] = 105; g->prof[1] = 200; g->prof[2] = 140; g->prof[3] = 60;
        g->rise[0] = 26; g->rise[2] = -14;      /* shoulders up, hips down */
        BUYM(CP4_MOUTH_C, 0, 0, 0, 0);
        BUYL(CP4_LEG, 0, 60, -40, 1, 190, 62);
        BUYL(CP4_LEG, 2, 60, -40, 1, 150, 48);
        BUYM(CP4_EYE, 0, 34, 30, 1);
        BUYM(CP4_CLAW, 0, 50, -20, 1);
        BUYM(CP4_FOOT, 2, 70, -55, 1);
        BUYM(CP4_HORN, 0, 0, 40, 0);
        BUYM(CP4_PLATE, 1, 128, 40, 0);
        BUYL(CP4_TAIL, 2, 128, -10, 0, 200, -30);
        BUYL(CP4_ARM, 0, 44, -8, 1, 170, 70);
        break;
    case CP4_STYLE_CHARMER:
        g->hue = 190; g->hue2 = 40; g->sat = 200; g->val = 190;
        g->pattern = CP4_PAT_SPOTS; g->pscale = 160;
        g->prof[0] = 140; g->prof[1] = 190; g->prof[2] = 175; g->prof[3] = 90;
        g->hue3 = 210; g->pattern2 = CP4_PAT_RINGS; g->pscale2 = 190;
        g->rise[1] = 30;
        BUYM(CP4_MOUTH_G, 0, 0, 0, 0);
        BUYL(CP4_LEG, 0, 60, -40, 1, 175, 55);
        BUYM(CP4_VOICE, 0, 0, 25, 0);
        BUYL(CP4_LEG, 2, 60, -40, 1, 175, 55);
        BUYM(CP4_PLUME, 1, 128, 55, 1);
        BUYM(CP4_EYE, 0, 34, 30, 1);
        BUYM(CP4_EAR, 0, 90, 45, 1);
        BUYM(CP4_PLUME, 2, 128, 45, 0);
        BUYL(CP4_TAIL, 2, 128, -34, 0, 235, -70);
        break;
    default: /* grazer */
        g->hue = 60; g->hue2 = 20; g->sat = 140; g->val = 165;
        g->pattern = CP4_PAT_MOTTLE; g->pscale = 140;
        g->prof[0] = 125; g->prof[1] = 210; g->prof[2] = 180; g->prof[3] = 75;
        /* Eyes before feet. In a world with no edges, range is the scarcest
         * resource a generalist has - a grazer that cannot see across the
         * resident ring spends the run walking past its own food. */
        g->rise[0] = 34;                        /* a raised neck to browse with */
        BUYM(CP4_MOUTH_G, 0, 0, 0, 0);
        BUYL(CP4_LEG, 0, 60, -40, 1, 200, 44);
        BUYM(CP4_EYE, 0, 34, 30, 1);
        BUYL(CP4_LEG, 2, 60, -40, 1, 200, 44);
        BUYM(CP4_FOOT, 2, 70, -55, 1);
        BUYM(CP4_EAR, 0, 90, 45, 1);
        BUYM(CP4_VOICE, 0, 0, 25, 0);
        BUYM(CP4_PLATE, 1, 128, 40, 0);
        BUYL(CP4_TAIL, 2, 128, -6, 0, 150, -20);
        BUYL(CP4_ARM, 0, 40, 14, 1, 200, 84);
        break;

    case CP4_STYLE_SWIMMER:
        /* Long, smooth and finned. It buys gills before anything ornamental,
         * because a swimmer without them is on a timer. */
        g->hue = 130; g->hue2 = 96; g->sat = 185; g->val = 175;
        g->pattern = CP4_PAT_COUNTER; g->pscale = 120;
        g->prof[0] = 120; g->prof[1] = 185; g->prof[2] = 165; g->prof[3] = 70;
        g->nseg = 4; spent += 5;
        BUYM(CP4_MOUTH_G, 0, 0, 0, 0);
        BUYL(CP4_FIN, 1, 64, 0, 1, 190, 20);
        BUYM(CP4_GILL, 0, 96, 10, 1);
        BUYL(CP4_FIN, 3, 64, 0, 1, 190, 20);
        BUYM(CP4_EYE, 0, 34, 30, 1);
        BUYL(CP4_LEG, 2, 60, -40, 1, 110, 40);
        BUYM(CP4_VOICE, 0, 0, 25, 0);
        BUYL(CP4_TAIL, 3, 128, 0, 0, 250, 0);
        break;

    case CP4_STYLE_FLYER:
        /* Light on purpose. Lift has to beat mass, so this build stays at
         * three segments and spends nothing on plate. */
        g->hue = 20; g->hue2 = 170; g->sat = 200; g->val = 200;
        g->pattern = CP4_PAT_BANDS; g->pscale = 150;
        g->prof[0] = 110; g->prof[1] = 165; g->prof[2] = 140; g->prof[3] = 60;
        g->girth = 95;
        BUYM(CP4_MOUTH_G, 0, 0, 0, 0);
        BUYL(CP4_WING, 1, 64, 20, 1, 230, 30);
        BUYL(CP4_LEG, 2, 60, -40, 1, 210, 70);
        BUYM(CP4_EYE, 0, 34, 30, 1);
        BUYL(CP4_WING, 2, 64, 10, 1, 210, 24);
        BUYM(CP4_VOICE, 0, 0, 25, 0);
        BUYL(CP4_TAIL, 2, 128, -18, 0, 175, -44);
        break;

    case CP4_STYLE_BURROWER:
        /* Squat, armoured and clawed. Slow above ground and safe below it. */
        g->hue = 25; g->hue2 = 210; g->sat = 130; g->val = 140;
        g->pattern = CP4_PAT_RINGS; g->pscale = 175;
        g->prof[0] = 150; g->prof[1] = 205; g->prof[2] = 180; g->prof[3] = 95;
        g->girth = 175;
        BUYM(CP4_MOUTH_G, 0, 0, 0, 0);
        BUYM(CP4_DIGGER, 0, 40, -25, 1);
        BUYL(CP4_LEG, 0, 60, -40, 1, 85, 88);
        BUYL(CP4_LEG, 2, 60, -40, 1, 85, 88);
        BUYM(CP4_PLATE, 1, 128, 40, 0);
        BUYM(CP4_EAR, 0, 90, 45, 1);
        BUYM(CP4_VOICE, 0, 0, 25, 0);
        BUYL(CP4_ARM, 0, 52, -22, 1, 120, 92);
        break;
    }
    #undef BUYL
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

    /* ---- the media ----
     *
     * Each one is gated by a part rather than scaled by it. A body with no
     * fins does not swim slowly, it does not swim: it flounders and drowns.
     * That is what makes buying a fin a decision instead of a stat bump, and
     * it is the same shape as the mouth gate on what an animal can eat. */
    o->swim = n[CP4_FIN] ? (58.0f + 26.0f * n[CP4_FIN]) / mass : 0.0f;
    /* every body floats a little; a fat one floats more */
    o->buoy = 0.35f + 0.30f * ((float)g->girth / 255.0f) + 0.06f * n[CP4_FIN];
    if (o->buoy > 0.95f) o->buoy = 0.95f;
    /* Lift has to beat mass, so wings on a heavy build are a waste of budget.
     * Two wings carry a light animal; a plated six-segment one needs four and
     * still climbs badly. */
    {
        float lift = 0.95f * n[CP4_WING];
        o->fly = lift > mass * 0.62f ? (lift - mass * 0.62f) * 150.0f : 0.0f;
    }
    /* Digging is a burrower's only mobility, so it has to be a real speed
     * rather than a crawl - at 26 the archetype covered a third of what a
     * walker did and starved politely. */
    o->dig = n[CP4_DIGGER] ? (44.0f + 22.0f * n[CP4_DIGGER]) / (0.7f + 0.4f * mass) : 0.0f;
    /* Gills make water home. Without them a big lung is worth a few seconds
     * and a small body drowns almost at once. */
    o->breath = n[CP4_GILL] ? 1.0e9f : (7.0f + 2.4f * (float)nseg + 6.0f * n[CP4_FIN]);
    /* A tail is a counterweight. It buys the two things a counterweight
     * actually buys - the ability to turn without falling over, and a foot
     * that stays planted while you do - which is why it is worth its DNA to a
     * runner rather than only to a display build. */
    o->turn += 0.34f * n[CP4_TAIL];
    o->jump += 18.0f * n[CP4_TAIL];

    o->grip  = 0.25f + 0.22f * n[CP4_FOOT] + 0.05f * n[CP4_LEG] + 0.07f * n[CP4_TAIL];
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
    /* How long the legs actually are.
     *
     * Until this, the `len` gene only decided where the knee sat: you could
     * design a leg that folded differently but not one that was longer, and
     * "drag the legs out until it is tall" is the first thing anyone does in a
     * creature editor. Leg length feeds stand, stand is the hip height the
     * rig reaches down from, so a long-legged build genuinely stands taller
     * and takes a longer stride - and pays for it in upkeep, because carrying
     * a body further off the ground is work. */
    float legspan = 0.5f;
    {
        int ln = 0; float sum = 0.0f;
        for (int i = 0; i < CP4_MAX_PARTS; i++)
            if (g->part[i].type == CP4_LEG) { sum += (float)g->part[i].len / 255.0f; ln++; }
        if (ln) legspan = sum / (float)ln;
    }
    /* Additive, not multiplicative. Scaling the whole thing by leg length let
     * a short-legged build stand *lower* than its own widest segment, and one
     * random body plan in ten went back to ploughing the dirt with its legs
     * invisible underneath it. The 1.02 is a clearance guarantee and nothing
     * downstream is allowed to eat into it; long legs add on top of it. */
    {
        float legfrac = (float)n[CP4_LEG] / (3.0f + n[CP4_LEG]);
        o->stand = o->radius * widest
                 * (1.02f + 0.68f * legfrac + 0.50f * legspan * legfrac);
    }
    /* Leg length changes what the animal looks like and nothing else.
     *
     * The first cut also paid it into speed and charged it in upkeep, on the
     * reasoning that a longer stride is a faster one. Measured over thirty
     * seeds that reasoning cost four of the six archetypes most of their
     * evolutions: the coupling is small per step and compounds over nine
     * thousand of them. A gene that makes a creature look the way you want is
     * worth having on its own, and this one is free. */
    o->upkeep_leg = 0.0f;

    o->hp_max = 130.0f + 22.0f * n[CP4_PLATE] + 11.0f * n[CP4_HORN]
                       + 16.0f * (float)(nseg - 2);
    o->armor  = 0.085f * n[CP4_PLATE] + 0.04f * n[CP4_HORN];
    if (o->armor > 0.62f) o->armor = 0.62f;

    o->bite     = n[CP4_MOUTH_C] ? 30.0f : (n[CP4_MOUTH_O] ? 18.0f : 0.0f);
    o->claw_dmg = 24.0f * (n[CP4_CLAW] ? 1.0f : 0.0f) + (n[CP4_HORN] ? 20.0f : 0.0f);

    /* Arms browse: they pull down what a mouth alone cannot reach, which is
     * worth something only to a body that has a mouth to put it in. */
    o->graze_eff = (0.80f * n[CP4_MOUTH_G] + 0.50f * n[CP4_MOUTH_O])
                 * (1.0f + 0.16f * n[CP4_ARM]);
    o->carn_eff  = 0.85f * n[CP4_MOUTH_C] + 0.50f * n[CP4_MOUTH_O];

    /* The world stopped having edges, so the useful range of an eye went up
     * with it: 210 units inside a 1250-unit resident ring is tunnel vision. */
    o->sight   = 260.0f + 95.0f * n[CP4_EYE];
    if (o->sight > 720.0f) o->sight = 720.0f;
    /* ears see through what eyes cannot: cover, and the far side of a ridge */
    o->hearing = 90.0f + 105.0f * n[CP4_EAR];

    /* the other half of the stage */
    /* No charm from a tail, however much a real one is a display organ. The
     * whole stage turns on charm and violence being bought with the same
     * budget, and a cheap part that quietly pays into both blunts the fork:
     * with it the predator archetype started winning encounters by impressing
     * them. A tail is a mobility part here, and it earns its DNA in turn,
     * grip, jump and stamina. */
    o->charm         = 0.65f * n[CP4_VOICE] + 0.80f * n[CP4_PLUME];
    o->social_reach  = 110.0f + 85.0f * n[CP4_VOICE];

    /* Reach. An arm lengthens every contact the animal makes - a blow lands
     * from further out, and so does a display. It is the one stat that helps
     * both halves of the stage, which is what stops the fork from being the
     * only decision in the game. */
    o->reach = 9.0f * n[CP4_ARM] + 0.30f * o->radius * (n[CP4_TAIL] ? 1.0f : 0.0f);
    /* Carry. Food only becomes descendants by being walked back to a nest, and
     * arms are what let you walk back with more of it. */
    o->carry = 1.0f + 0.42f * n[CP4_ARM];

    o->stamina = 100.0f + 18.0f * n[CP4_LEG] + 12.0f * n[CP4_FOOT] + 6.0f * n[CP4_TAIL]
                        + 10.0f * n[CP4_FIN] + 8.0f * n[CP4_WING];
    o->upkeep  = 0.50f + 0.15f * o->n_parts + 0.16f * (float)(nseg - 2)
                       + 0.30f * ((float)g->girth / 255.0f) + o->upkeep_leg;
}

/* ------------------------------------------------------------------ *
 * editing operations
 *
 * What an editor does to a genome, as opposed to what evolution does to one.
 * The difference that matters is failure: a mutation that overruns the budget
 * is normalised and whatever falls off, falls off, because nobody is watching.
 * A user who drags a part onto a body that cannot afford it has to be told
 * no, and told before anything moves - silently dropping some *other* part to
 * make room is the single most infuriating thing an editor can do.
 * ------------------------------------------------------------------ */

int cp4_genome_free_slot(const Cp4Genome *g)
{
    for (int i = 0; i < CP4_MAX_PARTS; i++)
        if (g->part[i].type == CP4_NONE) return i;
    return -1;
}

/* What this genome costs with one more part of `type` on it. Mirroring
 * doubles the price, which is the trade the whole bilateral-symmetry gene
 * exists to express. */
int cp4_genome_cost_with(const Cp4Genome *g, int type, int mirror)
{
    return cp4_genome_cost(g) + cp4_part_cost(type) * (mirror ? 2 : 1);
}

int cp4_genome_can_afford(const Cp4Genome *g, int type, int mirror, int budget)
{
    if (type <= CP4_NONE || type >= CP4_PART_COUNT) return 0;
    if (cp4_genome_free_slot(g) < 0) return 0;
    return cp4_genome_cost_with(g, type, mirror) <= budget;
}

/* Spore mirrors anything you do not place on the midline, and that is most of
 * why creatures built in it look like animals rather than like collections.
 * The midline here is the yaw axis: 0 is straight ahead and 128 straight
 * back, so the lateral component is what decides. */
int cp4_genome_should_mirror(int yaw)
{
    float a = (float)(yaw & 0xFF) * (2.0f * 3.14159265358979f / 256.0f);
    return fabsf(sinf(a)) > 0.30f;
}

int cp4_genome_place(Cp4Genome *g, int type, int seg, int yaw, int pitch,
                     int mirror, int budget)
{
    if (!g) return -1;
    if (type <= CP4_NONE || type >= CP4_PART_COUNT) return -1;
    if (mirror < 0) mirror = cp4_genome_should_mirror(yaw);
    if (!cp4_genome_can_afford(g, type, mirror, budget)) return -1;
    int slot = cp4_genome_free_slot(g);
    if (slot < 0) return -1;
    if (seg < 0) seg = 0;
    if (seg >= CP4_MAX_SEG) seg = CP4_MAX_SEG - 1;
    if (pitch > 127) pitch = 127;
    if (pitch < -127) pitch = -127;
    g->part[slot].type = (uint8_t)type;
    g->part[slot].seg = (uint8_t)seg;
    g->part[slot].yaw = (uint8_t)(yaw & 0xFF);
    g->part[slot].pitch = (int8_t)pitch;
    g->part[slot].mirror = (uint8_t)(mirror ? 1 : 0);
    g->part[slot].scale = 128;
    g->part[slot].len = 128;
    g->part[slot].bend = 0;
    return slot;
}

int cp4_genome_move(Cp4Genome *g, int slot, int seg, int yaw, int pitch)
{
    if (!g || slot < 0 || slot >= CP4_MAX_PARTS) return 0;
    if (g->part[slot].type == CP4_NONE) return 0;
    if (seg < 0) seg = 0;
    if (seg >= CP4_MAX_SEG) seg = CP4_MAX_SEG - 1;
    if (pitch > 127) pitch = 127;
    if (pitch < -127) pitch = -127;
    g->part[slot].seg = (uint8_t)seg;
    g->part[slot].yaw = (uint8_t)(yaw & 0xFF);
    g->part[slot].pitch = (int8_t)pitch;
    return 1;
}

/* Removing a part leaves its slot empty rather than compacting the array,
 * because an editor holds slot indices - in a selection, in an undo stack -
 * and compacting would silently repoint every one of them at a different
 * part. cp4_genome_normalise compacts when the genome goes back to the
 * simulation, which is the right moment for it. */
int cp4_genome_remove(Cp4Genome *g, int slot)
{
    if (!g || slot < 0 || slot >= CP4_MAX_PARTS) return 0;
    if (g->part[slot].type == CP4_NONE) return 0;
    memset(&g->part[slot], 0, sizeof(g->part[slot]));
    g->part[slot].len = 128;
    return 1;
}

/* Size, reach and fold, which in Spore are the handles you spend the most
 * time on: dragging a limb out to the length you want and setting how it
 * folds is most of what turns a parts bin into a creature. Each is clamped
 * rather than rejected, because a drag that runs past the end of a range
 * should stop there, not fail. */
int cp4_genome_shape(Cp4Genome *g, int slot, int scale, int len, int bend)
{
    if (!g || slot < 0 || slot >= CP4_MAX_PARTS) return 0;
    if (g->part[slot].type == CP4_NONE) return 0;
    if (scale >= 0) g->part[slot].scale = (uint8_t)(scale > 255 ? 255 : (scale < 20 ? 20 : scale));
    if (len   >= 0) g->part[slot].len   = (uint8_t)(len   > 255 ? 255 : (len   < 20 ? 20 : len));
    if (bend > -1000) {
        if (bend > 127) bend = 127;
        if (bend < -127) bend = -127;
        g->part[slot].bend = (int8_t)bend;
    }
    return 1;
}

int cp4_genome_mirror(Cp4Genome *g, int slot, int on, int budget)
{
    if (!g || slot < 0 || slot >= CP4_MAX_PARTS) return 0;
    int t = g->part[slot].type;
    if (t == CP4_NONE) return 0;
    if (on && !g->part[slot].mirror) {
        /* turning symmetry on costs another copy of the part */
        if (cp4_genome_cost(g) + cp4_part_cost(t) > budget) return 0;
    }
    g->part[slot].mirror = (uint8_t)(on ? 1 : 0);
    return 1;
}

/* ---- paint ----
 *
 * The three coats have been in the genome since the stage was written and
 * have never been reachable from outside it: cp4_genome_from_action does not
 * touch them and neither did the bindings, so every animal anything other
 * than the random generator produced came out the same beige with both coats
 * plain. Colour is the cheapest variety there is - shape is expensive and a
 * hue is one byte - so an editor that cannot paint is missing most of the
 * space it is supposed to be exploring.
 */
void cp4_genome_paint(Cp4Genome *g, int hue, int hue2, int hue3,
                      int sat, int val)
{
    if (!g) return;
    if (hue  >= 0) g->hue  = (uint8_t)(hue  & 0xFF);
    if (hue2 >= 0) g->hue2 = (uint8_t)(hue2 & 0xFF);
    if (hue3 >= 0) g->hue3 = (uint8_t)(hue3 & 0xFF);
    if (sat  >= 0) g->sat  = (uint8_t)(sat > 255 ? 255 : sat);
    if (val  >= 0) g->val  = (uint8_t)(val > 255 ? 255 : val);
}

void cp4_genome_coats(Cp4Genome *g, int pattern, int pscale,
                      int pattern2, int pscale2)
{
    if (!g) return;
    if (pattern  >= 0) g->pattern  = (uint8_t)(pattern  % CP4_PAT_COUNT);
    if (pscale   >= 0) g->pscale   = (uint8_t)(pscale > 255 ? 255 : pscale);
    if (pattern2 >= 0) g->pattern2 = (uint8_t)(pattern2 % CP4_PAT_COUNT);
    if (pscale2  >= 0) g->pscale2  = (uint8_t)(pscale2 > 255 ? 255 : pscale2);
}

/* ---- the spine, as numbers ----
 * The same three genes cp4_studio_spine_drag writes, for callers that have a
 * value rather than a gesture. */
void cp4_genome_spine(Cp4Genome *g, int nseg, int girth, int arch, int sweep)
{
    if (!g) return;
    if (nseg >= 0) {
        if (nseg < 2) nseg = 2;
        if (nseg > CP4_MAX_SEG) nseg = CP4_MAX_SEG;
        g->nseg = (uint8_t)nseg;
    }
    if (girth >= 0) g->girth = (uint8_t)(girth > 255 ? 255 : girth);
    if (arch  > -1000) g->arch  = (int8_t)(arch  > 127 ? 127 : (arch  < -127 ? -127 : arch));
    if (sweep > -1000) g->sweep = (int8_t)(sweep > 127 ? 127 : (sweep < -127 ? -127 : sweep));
}

void cp4_genome_vertebra(Cp4Genome *g, int i, int rise, int lump)
{
    if (!g || i < 0 || i >= CP4_MAX_SEG) return;
    if (rise > -1000) g->rise[i] = (int8_t)(rise > 127 ? 127 : (rise < -127 ? -127 : rise));
    if (lump > -1000) g->lump[i] = (int8_t)(lump > 127 ? 127 : (lump < -127 ? -127 : lump));
}
