/* The lineage: one creature carried across five games.
 *
 * See include/cpore/lineage.h for why this exists at all. The short version is
 * that converting genome-to-genome would be twenty conversions that all have
 * to agree; this is ten that never have to know about each other.
 *
 * Two conventions run through the whole file.
 *
 * Reading a stage writes back only what that stage can express. A cell has no
 * colour genes, so cp_lineage_from_cell does not touch colour and your species
 * comes out of the pond the colour it went in. This is what makes the record
 * trustworthy: every field in it was last written by a stage that had an
 * opinion about it.
 *
 * Writing a stage goes through that stage's own designer rather than around
 * it. Each stage already knows how to spend a budget on a coherent animal, and
 * a second allocator here would be a second thing to keep balanced and the
 * first to fall behind. So the lineage picks the nearest style, lets the
 * designer work, imposes the identity on top, and spends whatever is left on
 * the traits the style under-served.
 */

#include "cpore/lineage.h"

#include <string.h>
#include <math.h>

static uint8_t u8f(float v)
{
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

static int imaxi(int a, int b) { return a > b ? a : b; }

static int clampi8(int v) { return v < -127 ? -127 : (v > 127 ? 127 : v); }

/* ------------------------------------------------------------------ *
 * starting points
 * ------------------------------------------------------------------ */

void cp_lineage_default(CpLineage *l)
{
    if (!l) return;
    memset(l, 0, sizeof(*l));
    l->herb = 120; l->carn = 60;
    l->speed = 90; l->armour = 60; l->weapon = 60; l->sense = 90; l->social = 60;
    /* A colour, not the absence of one. A lineage that started at sat 0 would
     * still be grey five stages later, because nothing downstream invents
     * saturation for you. */
    l->hue = 96; l->hue2 = 30; l->hue3 = 200;
    l->sat = 170; l->val = 195;
    l->pattern = 2; l->pscale = 140;
    l->nseg = 3; l->girth = 140;
    l->arch = 10; l->sweep = 0;
    l->stages = 0;
}

void cp_lineage_random(CpLineage *l, CpRng *r)
{
    if (!l) return;
    cp_lineage_default(l);
    if (!r) return;
    l->herb   = (uint8_t)cp_rng_int(r, 256);
    l->carn   = (uint8_t)cp_rng_int(r, 256);
    l->speed  = (uint8_t)cp_rng_int(r, 256);
    l->armour = (uint8_t)cp_rng_int(r, 256);
    l->weapon = (uint8_t)cp_rng_int(r, 256);
    l->sense  = (uint8_t)cp_rng_int(r, 256);
    l->social = (uint8_t)cp_rng_int(r, 256);
    l->hue    = (uint8_t)cp_rng_int(r, 256);
    l->hue2   = (uint8_t)cp_rng_int(r, 256);
    l->hue3   = (uint8_t)cp_rng_int(r, 256);
    /* Saturation and value are floored rather than uniform: the bottom of
     * those ranges is not a different-looking animal, it is an unlit one. */
    l->sat    = (uint8_t)(120 + cp_rng_int(r, 136));
    l->val    = (uint8_t)(140 + cp_rng_int(r, 116));
    l->pattern = (uint8_t)cp_rng_int(r, 8);
    l->pscale  = (uint8_t)(80 + cp_rng_int(r, 176));
    l->nseg    = (uint8_t)(2 + cp_rng_int(r, 4));
    l->girth   = (uint8_t)(90 + cp_rng_int(r, 140));
    l->arch    = (int8_t)(cp_rng_int(r, 127) - 63);
    l->sweep   = (int8_t)(cp_rng_int(r, 127) - 63);
    return;
}

/* ------------------------------------------------------------------ *
 * reading each stage back into the lineage
 * ------------------------------------------------------------------ */

void cp_lineage_from_cell(CpLineage *l, const CpGenome *g)
{
    if (!l || !g) return;
    CpStats s;
    cp_genome_stats(g, &s);
    const uint8_t *n = s.n;

    /* A proboscis is half a filter and half a jaw, which is exactly what
     * omnivory is and exactly why diet is two numbers here. */
    l->herb = u8f(n[CP_PART_FILTER] * 96.0f + n[CP_PART_PROBOSCIS] * 56.0f);
    l->carn = u8f(n[CP_PART_JAW]    * 96.0f + n[CP_PART_PROBOSCIS] * 56.0f);

    l->speed  = u8f((n[CP_PART_CILIA] + n[CP_PART_FLAGELLA] + n[CP_PART_JET]) * 54.0f);
    /* Stage 1 has no plate. What stands in for armour is a spike facing
     * outward and a poison sac, both of which make you costly to bite. */
    l->armour = u8f(n[CP_PART_SPIKE] * 30.0f + n[CP_PART_POISON] * 50.0f);
    l->weapon = u8f(n[CP_PART_JAW] * 50.0f + n[CP_PART_SPIKE] * 44.0f
                    + n[CP_PART_ELECTRIC] * 60.0f);
    l->sense  = u8f(n[CP_PART_EYE] * 80.0f);
    /* No social trait, no colour, no proportions: a cell has no opinion about
     * any of them, so it does not get a vote. */
    l->stages = (uint8_t)imaxi(l->stages, 1);
}

void cp_lineage_from_aqua(CpLineage *l, const Cp3Genome *g)
{
    if (!l || !g) return;
    Cp3Stats s;
    cp3_genome_stats(g, &s);
    const uint8_t *n = s.n;

    l->herb = u8f(n[CP3_FILTER] * 110.0f);
    l->carn = u8f(n[CP3_JAW]    * 110.0f);

    l->speed  = u8f((n[CP3_FIN] + n[CP3_TAIL]) * 52.0f);
    l->armour = u8f(n[CP3_PLATE] * 70.0f + n[CP3_SPIKE] * 24.0f);
    l->weapon = u8f(n[CP3_JAW] * 54.0f + n[CP3_SPIKE] * 48.0f);
    /* A photophore is a sense organ here: it is the only way to see at depth,
     * so it buys perception rather than decoration. */
    l->sense  = u8f(n[CP3_EYE] * 70.0f + n[CP3_LIGHT] * 50.0f);

    /* A fish has colour and proportion, so it gets to change both. hue3 has no
     * counterpart underwater and survives untouched. */
    l->hue = g->hue; l->hue2 = g->hue2;
    l->sat = g->sat; l->val = g->val;
    l->pattern = g->pattern; l->pscale = g->pscale;
    l->nseg = g->nseg; l->girth = g->girth;
    l->arch = g->arch; l->sweep = g->sweep;
    l->stages = (uint8_t)imaxi(l->stages, 2);
}

void cp_lineage_from_land(CpLineage *l, const Cp4Genome *g)
{
    if (!l || !g) return;
    Cp4Stats s;
    cp4_genome_stats(g, &s);
    const uint8_t *n = s.n;

    l->herb = u8f(n[CP4_MOUTH_G] * 110.0f + n[CP4_MOUTH_O] * 60.0f);
    l->carn = u8f(n[CP4_MOUTH_C] * 110.0f + n[CP4_MOUTH_O] * 60.0f);

    l->speed  = u8f(n[CP4_LEG] * 40.0f + n[CP4_FOOT] * 24.0f
                    + n[CP4_WING] * 30.0f + n[CP4_FIN] * 20.0f);
    l->armour = u8f(n[CP4_PLATE] * 70.0f + n[CP4_HORN] * 26.0f);
    l->weapon = u8f(n[CP4_CLAW] * 52.0f + n[CP4_HORN] * 46.0f
                    + n[CP4_MOUTH_C] * 40.0f);
    l->sense  = u8f(n[CP4_EYE] * 60.0f + n[CP4_EAR] * 60.0f);
    /* The first stage that can be charming, and therefore the first that can
     * say anything about how a civilisation will behave. */
    l->social = u8f(n[CP4_VOICE] * 70.0f + n[CP4_PLUME] * 70.0f);

    l->hue = g->hue; l->hue2 = g->hue2; l->hue3 = g->hue3;
    l->sat = g->sat; l->val = g->val;
    l->pattern = g->pattern; l->pscale = g->pscale;
    l->nseg = g->nseg; l->girth = g->girth;
    /* Stage 3's spine is a set of points rather than an arch and a sweep, so
     * the lineage's two shape genes are measured off it instead of copied:
     * how far the middle of the body rides above the nose-to-tail line, and
     * how far to one side. That is what arch and sweep meant, and it is the
     * part of a dragged shape the other stages can still express - a hump and
     * a dropped neck on the same animal average out to neither, which is the
     * honest answer when the receiving stage has one number for both. */
    {
        int n = g->nseg < 2 ? 2 : (g->nseg > CP4_MAX_SEG ? CP4_MAX_SEG : g->nseg);
        float up = 0.0f, side = 0.0f, w = 0.0f;
        for (int i = 0; i < n; i++) {
            float t = (float)i / (float)(n - 1);
            float bend = sinf(3.14159265f * t);
            up   += (float)g->spine[i].up   * bend;
            side += (float)g->spine[i].side * bend;
            w    += bend * bend;
        }
        if (w < 1e-3f) w = 1e-3f;
        /* Least-squares fit of a single sine, then into the aquatic scales:
         * stage 3 stores 1.6 body radii per 127, stage 2's arch is 1.5 and
         * its sweep 1.2. */
        l->arch  = (int8_t)clampi8((int)(up   / w * (1.6f / 1.5f)));
        l->sweep = (int8_t)clampi8((int)(side / w * (1.6f / 1.2f)));
    }
    l->stages = (uint8_t)imaxi(l->stages, 3);
}

/* ------------------------------------------------------------------ *
 * which style a lineage most resembles
 *
 * Scored rather than switched, because the interesting lineages are the ones
 * that are two things at once and a switch would have to pick a winner by
 * argument order.
 * ------------------------------------------------------------------ */

static int argmax(const float *v, int n)
{
    int best = 0;
    for (int i = 1; i < n; i++) if (v[i] > v[best]) best = i;
    return best;
}

int cp_lineage_style_cell(const CpLineage *l)
{
    if (!l) return CP_STYLE_GRAZER;
    float s[CP_STYLE_COUNT];
    s[CP_STYLE_GRAZER] = (float)l->herb;
    s[CP_STYLE_HUNTER] = (float)l->carn + 0.5f * (float)l->weapon;
    s[CP_STYLE_TANK]   = (float)l->armour;
    s[CP_STYLE_SCOUT]  = (float)l->sense + 0.5f * (float)l->speed;
    return argmax(s, CP_STYLE_COUNT);
}

int cp_lineage_style_aqua(const CpLineage *l)
{
    if (!l) return CP3_STYLE_GRAZER;
    float s[CP3_STYLE_COUNT];
    s[CP3_STYLE_GRAZER] = (float)l->herb;
    s[CP3_STYLE_HUNTER] = (float)l->carn + 0.5f * (float)l->weapon;
    s[CP3_STYLE_DIVER]  = (float)l->sense;
    return argmax(s, CP3_STYLE_COUNT);
}

int cp_lineage_style_land(const CpLineage *l)
{
    if (!l) return CP4_STYLE_GRAZER;
    /* Swimmer, flyer and burrower are choices of *medium*, and nothing
     * upstream of the land stage has a medium to express - a cell and a fish
     * are both simply in water. So they are unreachable from a lineage and
     * score zero here rather than being faked from speed. A creature becomes
     * a flier by growing wings in the stage that has a sky, and the lineage
     * carries that back out afterwards through `speed`. */
    float s[CP4_STYLE_COUNT];
    for (int i = 0; i < CP4_STYLE_COUNT; i++) s[i] = 0.0f;
    s[CP4_STYLE_GRAZER]   = (float)l->herb;
    s[CP4_STYLE_PREDATOR] = (float)l->carn + 0.5f * (float)l->weapon;
    s[CP4_STYLE_CHARMER]  = (float)l->social;
    return argmax(s, CP4_STYLE_COUNT);
}

/* ------------------------------------------------------------------ *
 * writing the lineage into each stage
 * ------------------------------------------------------------------ */

/* The part each stage uses to express each trait, strongest first. Two entries
 * per trait so that a budget which cannot afford the obvious answer still buys
 * something in the right direction. */
typedef struct { int trait_of[2]; } TopUp;

/* trait indices, purely local */
enum { T_HERB = 0, T_CARN, T_SPEED, T_ARMOUR, T_WEAPON, T_SENSE, T_SOCIAL, T_N };

static void traits_of(const CpLineage *l, float *t)
{
    t[T_HERB]   = (float)l->herb;
    t[T_CARN]   = (float)l->carn;
    t[T_SPEED]  = (float)l->speed;
    t[T_ARMOUR] = (float)l->armour;
    t[T_WEAPON] = (float)l->weapon;
    t[T_SENSE]  = (float)l->sense;
    t[T_SOCIAL] = (float)l->social;
}

/* A trait below this is something the lineage *has a little of*, not something
 * it is, and topping one up manufactures an ancestry that never existed.
 *
 * Without the floor, a committed herbivore came out of the aquatic stage an
 * omnivore: carnivory sat around 56 out of 255, which is nothing, but it was
 * still the third-ranked trait, so the leftover budget bought it a jaw. Rank
 * alone cannot tell "third strongest" from "third weakest" - on a specialist
 * they are the same position. */
#define TRAIT_FLOOR 96.0f

/* Order the traits strongest first, so a top-up spends on what the lineage
 * most is before what it merely also is. Traits under the floor are dropped
 * from the list entirely; the count comes back so a caller stops there. */
static int trait_order(const CpLineage *l, int *order)
{
    float t[T_N];
    traits_of(l, t);
    int n = 0;
    for (int i = 0; i < T_N; i++) if (t[i] >= TRAIT_FLOOR) order[n++] = i;
    for (int i = 1; i < n; i++) {
        int k = order[i];
        int j = i - 1;
        while (j >= 0 && t[order[j]] < t[k]) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    return n;
}

/* Diet belongs with the identity, not with the role.
 *
 * A style is a job - grazer, hunter, diver, charmer - and a designer picks
 * parts to do that job, mouth included. But a diver can be a herbivore or a
 * carnivore, and when a lineage that had spent two stages filter-feeding came
 * out of the aquatic stage with a jaw, nothing had gone wrong in the designer:
 * `sense` was the lineage's strongest trait, DIVER won the style vote, and the
 * diver build has teeth. The style had quietly voted on diet.
 *
 * So the mouths are rewritten afterwards, the same way colour is. The comment
 * further down applies here too: the designer is entitled to pick parts and is
 * not entitled to pick who you are.
 *
 * 1.6 is the width of the omnivore band. Below it the two diets are close
 * enough that "eats both" is the honest reading rather than a tie broken by
 * rounding. */
static int diet_of(const CpLineage *l)
{
    if ((float)l->herb > (float)l->carn * 1.6f) return 0;   /* herbivore */
    if ((float)l->carn > (float)l->herb * 1.6f) return 1;   /* carnivore */
    return 2;                                                /* omnivore  */
}

void cp_lineage_to_cell(const CpLineage *l, CpGenome *g, int budget, CpRng *r)
{
    if (!l || !g) return;
    cp_genome_autodesign(g, r, budget, cp_lineage_style_cell(l));

    /* Top up. The designer builds a coherent animal for its style; what it
     * cannot know is which of the lineage's other traits mattered enough to
     * survive four stages, so those get whatever budget is left. */
    static const int expr[T_N][2] = {
        { CP_PART_FILTER,   CP_PART_PROBOSCIS },
        { CP_PART_JAW,      CP_PART_PROBOSCIS },
        { CP_PART_CILIA,    CP_PART_FLAGELLA  },
        { CP_PART_SPIKE,    CP_PART_POISON    },
        { CP_PART_SPIKE,    CP_PART_ELECTRIC  },
        { CP_PART_EYE,      CP_PART_EYE       },
        { CP_PART_EYE,      CP_PART_EYE       },   /* no social organ here */
    };
    int order[T_N];
    int nord = trait_order(l, order);
    for (int k = 0; k < nord; k++) {
        for (int e = 0; e < 2; e++) {
            int type = expr[order[k]][e];
            if (cp_genome_cost(g) + cp_part_cost(type) > budget) continue;
            int slot = -1;
            for (int i = 0; i < CP_MAX_PARTS; i++)
                if (g->part[i].type == CP_PART_NONE) { slot = i; break; }
            if (slot < 0) { k = nord; break; }
            g->part[slot].type = (uint8_t)type;
            /* Spread the additions around the rim rather than stacking them
             * on one bearing, since in stage 1 an angle is what a part is for. */
            g->part[slot].angle = (uint8_t)((slot * 47 + k * 23) & 0xFF);
        }
    }

    /* Impose the diet the lineage arrived with. */
    {
        int d = diet_of(l);
        int want = d == 0 ? CP_PART_FILTER : (d == 1 ? CP_PART_JAW : CP_PART_PROBOSCIS);
        for (int i = 0; i < CP_MAX_PARTS; i++) {
            int t = g->part[i].type;
            if (t == CP_PART_FILTER || t == CP_PART_JAW || t == CP_PART_PROBOSCIS)
                g->part[i].type = (uint8_t)want;
        }
    }
    cp_genome_normalise(g, budget);
}

void cp_lineage_to_aqua(const CpLineage *l, Cp3Genome *g, int budget, CpRng *r)
{
    if (!l || !g) return;
    cp3_genome_autodesign(g, r, budget, cp_lineage_style_aqua(l));

    static const int expr[T_N][2] = {
        { CP3_FILTER, CP3_FILTER },
        { CP3_JAW,    CP3_JAW    },
        { CP3_TAIL,   CP3_FIN    },
        { CP3_PLATE,  CP3_SPIKE  },
        { CP3_SPIKE,  CP3_JAW    },
        { CP3_EYE,    CP3_LIGHT  },
        { CP3_EYE,    CP3_LIGHT  },   /* no social organ underwater either */
    };
    int order[T_N];
    int nord = trait_order(l, order);
    for (int k = 0; k < nord; k++) {
        for (int e = 0; e < 2; e++) {
            int type = expr[order[k]][e];
            if (cp3_genome_cost(g) + cp3_part_cost(type) > budget) continue;
            int slot = -1;
            for (int i = 0; i < CP3_MAX_PARTS; i++)
                if (g->part[i].type == CP3_NONE) { slot = i; break; }
            if (slot < 0) { k = nord; break; }
            g->part[slot].type   = (uint8_t)type;
            g->part[slot].seg    = (uint8_t)(1 + ((slot + k) % (g->nseg ? g->nseg : 3)));
            g->part[slot].yaw    = (uint8_t)((slot * 53 + k * 29) & 0xFF);
            g->part[slot].pitch  = (int8_t)(((slot * 17) % 64) - 32);
            g->part[slot].scale  = 150;
            g->part[slot].mirror = (uint8_t)((slot & 1) ? 1 : 0);
        }
    }

    /* The identity, imposed after the designer rather than before, because the
     * designer is entitled to pick parts and is not entitled to pick who you
     * are. */
    g->hue = l->hue; g->hue2 = l->hue2;
    g->sat = l->sat; g->val = l->val;
    g->pattern = (uint8_t)(l->pattern % CP3_PAT_COUNT);
    g->pscale = l->pscale;
    if (l->nseg >= 2) g->nseg = (uint8_t)(l->nseg > CP3_MAX_SEG ? CP3_MAX_SEG : l->nseg);
    g->girth = l->girth;
    g->arch = l->arch; g->sweep = l->sweep;

    /* No omnivore mouth exists underwater, so an omnivore keeps whatever mix
     * the designer produced; a specialist gets every mouth converted. */
    {
        int d = diet_of(l);
        if (d != 2) {
            int want = d == 0 ? CP3_FILTER : CP3_JAW;
            for (int i = 0; i < CP3_MAX_PARTS; i++) {
                int t = g->part[i].type;
                if (t == CP3_FILTER || t == CP3_JAW) g->part[i].type = (uint8_t)want;
            }
        }
    }

    cp3_genome_normalise(g, budget);
}

void cp_lineage_to_land(const CpLineage *l, Cp4Genome *g, int budget, CpRng *r)
{
    if (!l || !g) return;
    cp4_genome_autodesign(g, r, budget, cp_lineage_style_land(l));

    static const int expr[T_N][2] = {
        { CP4_MOUTH_G, CP4_MOUTH_O },
        { CP4_MOUTH_C, CP4_MOUTH_O },
        { CP4_LEG,     CP4_FOOT    },
        { CP4_PLATE,   CP4_HORN    },
        { CP4_CLAW,    CP4_HORN    },
        { CP4_EYE,     CP4_EAR     },
        { CP4_VOICE,   CP4_PLUME   },
    };
    int order[T_N];
    int nord = trait_order(l, order);
    for (int k = 0; k < nord; k++) {
        for (int e = 0; e < 2; e++) {
            int type = expr[order[k]][e];
            if (cp4_genome_cost(g) + cp4_part_cost(type) > budget) continue;
            /* The land stage has a placement function that knows where a part
             * of each type belongs, which is worth using rather than
             * reproducing - a leg placed on the spine like a horn is not a leg. */
            int seg = 1 + ((k + e) % (g->nseg ? g->nseg : 3));
            int yaw = (k * 37 + e * 91) & 0xFF;
            if (cp4_genome_place(g, type, seg, yaw, 0, -1, budget) < 0) { k = nord; break; }
        }
    }

    g->hue = l->hue; g->hue2 = l->hue2; g->hue3 = l->hue3;
    g->sat = l->sat; g->val = l->val;
    g->pattern = (uint8_t)(l->pattern % CP4_PAT_COUNT);
    g->pscale = l->pscale;
    /* Through cp4_genome_spine, which relays the designer's curve onto the
     * lineage's point count rather than truncating it - the thickness the
     * archetype chose is the shape of the animal and there is no reason for a
     * longer body to lose it. */
    cp4_genome_spine(g, l->nseg >= 2 ? l->nseg : -1, l->girth);
    /* Then bow it. Stage 3 has no arch gene to assign; a bow is what you get
     * by pushing every control point off the axis by a sine, which is what
     * arch and sweep were computing at draw time before the points existed. */
    {
        int n = g->nseg < 2 ? 2 : (g->nseg > CP4_MAX_SEG ? CP4_MAX_SEG : g->nseg);
        for (int i = 0; i < n; i++) {
            float t = (float)i / (float)(n - 1);
            float bend = sinf(3.14159265f * t);
            int up   = (int)((float)l->arch  * bend * (1.5f / 1.6f));
            int side = (int)((float)l->sweep * bend * (1.2f / 1.6f));
            g->spine[i].up   = (int8_t)clampi8(g->spine[i].up   + up);
            g->spine[i].side = (int8_t)clampi8(g->spine[i].side + side);
        }
    }

    {
        int d = diet_of(l);
        int want = d == 0 ? CP4_MOUTH_G : (d == 1 ? CP4_MOUTH_C : CP4_MOUTH_O);
        for (int i = 0; i < CP4_MAX_PARTS; i++) {
            int t = g->part[i].type;
            if (t == CP4_MOUTH_G || t == CP4_MOUTH_C || t == CP4_MOUTH_O)
                g->part[i].type = (uint8_t)want;
        }
    }

    cp4_genome_normalise(g, budget);
}

/* ------------------------------------------------------------------ *
 * and out the other end, as doctrine
 * ------------------------------------------------------------------ */

void cp_lineage_to_legacy(const CpLineage *l, Cp5Legacy *out)
{
    if (!out) return;
    if (!l) { cp5_legacy_default(out); return; }

    /* The same 0.80..1.60 window cp5_legacy_from_creature produces, so a
     * campaign can be joined at either end and the civilisation stage cannot
     * tell which of the two seeded it.
     *
     * The three readings are the argument of the whole chain. A body that
     * spent its life biting things is a nation that reaches for force; one
     * that spent it eating well and seeing far is one that reaches for trade;
     * one that spent it displaying is one that reaches for faith. */
    float mil = (l->weapon + 0.6f * l->armour + 0.4f * l->carn) / 255.0f;
    float eco = (l->sense + 0.6f * l->speed + 0.4f * l->herb) / 255.0f;
    float rel = (l->social * 1.6f) / 255.0f;

    float v[CP5_APPROACH_COUNT];
    v[CP5_MIL] = mil; v[CP5_ECO] = eco; v[CP5_REL] = rel;
    for (int i = 0; i < CP5_APPROACH_COUNT; i++) {
        float b = 0.80f + v[i] * 0.45f;
        out->bonus[i] = b < 0.80f ? 0.80f : (b > 1.60f ? 1.60f : b);
    }
}
