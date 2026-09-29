#include "spore/genome.h"

#include <math.h>
#include <string.h>

/* --- morph tables ------------------------------------------------------ */

const SpineMorph SPINES[SPINE_MORPH_COUNT] = {
    { "compact",     0.70f, 4 },
    { "standard",    1.00f, 6 },
    { "long",        1.35f, 9 },
    { "serpentine",  1.80f, 16 },
};

const NeckMorph NECKS[NECK_MORPH_COUNT] = {
    { "short",    0.55f },
    { "standard", 0.80f },
    { "crane",    1.25f },
};

const ReachMorph REACHES[REACH_MORPH_COUNT] = {
    { "stubby", 0.70f, 0.85f },
    { "mid",    1.00f, 1.00f },
    { "lanky",  1.30f, 1.18f },
};

const StanceMorph STANCES[STANCE_MORPH_COUNT] = {
    { "quad",  1.00f },
    { "biped", 1.22f },
};

/* --- part catalogue ---------------------------------------------------- */

static const char *const MOUTH_NAMES[MOUTH_COUNT] =
    { "none", "jaw", "beak", "proboscis", "sucker" };
static const char *const LEGS_NAMES[LEGS_COUNT] =
    { "none", "stub", "sprint", "jump", "climb", "flipper" };
static const char *const WEAPON_NAMES[WEAPON_COUNT] =
    { "none", "horn", "club", "poison" };
static const char *const ABILITY_NAMES[ABILITY_COUNT] =
    { "none", "sing", "charge", "roar" };
static const char *const EYES_NAMES[EYES_COUNT] =
    { "none", "small", "large", "stalk" };
static const char *const GRASPER_NAMES[GRASPER_COUNT] =
    { "none", "hands", "claws" };
static const char *const DETAIL_NAMES[DETAIL_COUNT] =
    { "none", "fin", "wing", "quills" };

static const char *const *slot_names(int slot, int *count) {
    switch (slot) {
    case PART_SLOT_MOUTH:   *count = MOUTH_COUNT;   return MOUTH_NAMES;
    case PART_SLOT_LEGS:    *count = LEGS_COUNT;    return LEGS_NAMES;
    case PART_SLOT_WEAPON:  *count = WEAPON_COUNT;  return WEAPON_NAMES;
    case PART_SLOT_ABILITY: *count = ABILITY_COUNT; return ABILITY_NAMES;
    case PART_SLOT_EYES:    *count = EYES_COUNT;    return EYES_NAMES;
    case PART_SLOT_GRASPER: *count = GRASPER_COUNT; return GRASPER_NAMES;
    case PART_SLOT_DETAIL:  *count = DETAIL_COUNT;  return DETAIL_NAMES;
    default: *count = 0; return NULL;
    }
}

void unlocks_grant(PartUnlocks *u, int slot, int part) {
    if (!u || slot < 0 || slot >= PART_SLOT_COUNT) return;
    int count = 0;
    slot_names(slot, &count);
    if (part < 0 || part >= count || part >= 32) return;
    u->mask[slot] |= (1u << part);
}

int unlocks_cycle(const PartUnlocks *u, int slot, int cur) {
    int count = 0;
    slot_names(slot, &count);
    if (!u || count <= 1) return cur;
    for (int step = 1; step <= count; step++) {
        int p = (cur + step) % count;
        if (p < 32 && (u->mask[slot] & (1u << p)))
            return p;
    }
    return cur;
}

const char *unlocks_part_name(int slot, int part) {
    int count = 0;
    const char *const *names = slot_names(slot, &count);
    if (!names || count == 0) return "?";
    if (part < 0) part = 0;
    if (part >= count) part = count - 1;
    return names[part];
}

/* --- genome ------------------------------------------------------------ */

Genome genome_starter(void) {
    Genome g;
    memset(&g, 0, sizeof(g));
    g.mouth = MOUTH_JAW;
    g.legs = LEGS_STUB;
    g.weapon = WEAPON_NONE;
    g.ability = ABILITY_NONE;
    g.eyes = EYES_SMALL;
    g.grasper = GRASPER_NONE;
    g.detail = DETAIL_NONE;
    g.spine = SPINE_STANDARD;
    g.neck = NECK_STANDARD;
    g.asym = ASYM_BALANCED;
    g.stance = STANCE_QUAD;
    g.wpn_sock = WSOCK_FLANK;
    g.dtl_sock = DSOCK_BACK;
    g.reach = REACH_MID;
    g.limbs = 4;
    g.wpn_slide = SLIDE_COUNT / 2;
    g.dtl_slide = SLIDE_COUNT / 2;
    return g;
}

static int clamp_index(int v, int count) {
    if (v < 0) return 0;
    if (v >= count) return count - 1;
    return v;
}

void genome_clamp(Genome *g) {
    if (!g) return;
    g->mouth = clamp_index(g->mouth, MOUTH_COUNT);
    g->legs = clamp_index(g->legs, LEGS_COUNT);
    g->weapon = clamp_index(g->weapon, WEAPON_COUNT);
    g->ability = clamp_index(g->ability, ABILITY_COUNT);
    g->eyes = clamp_index(g->eyes, EYES_COUNT);
    g->grasper = clamp_index(g->grasper, GRASPER_COUNT);
    g->detail = clamp_index(g->detail, DETAIL_COUNT);
    g->spine = clamp_index(g->spine, SPINE_MORPH_COUNT);
    g->neck = clamp_index(g->neck, NECK_MORPH_COUNT);
    g->asym = clamp_index(g->asym, ASYM_COUNT);
    g->stance = clamp_index(g->stance, STANCE_MORPH_COUNT);
    g->wpn_sock = clamp_index(g->wpn_sock, WSOCK_COUNT);
    g->dtl_sock = clamp_index(g->dtl_sock, DSOCK_COUNT);
    g->reach = clamp_index(g->reach, REACH_MORPH_COUNT);
    if (g->limbs < 0) g->limbs = 0;
    if (g->limbs > CREATURE_LEGS) g->limbs = CREATURE_LEGS;
    g->limbs &= ~1; /* even: limbs come in pairs */
    g->wpn_slide = clamp_index(g->wpn_slide, SLIDE_COUNT);
    g->dtl_slide = clamp_index(g->dtl_slide, SLIDE_COUNT);
}

int genome_complexity(const Genome *g) {
    if (!g) return 0;
    int cx = 0;
    if (g->mouth != MOUTH_NONE) cx++;
    if (g->legs != LEGS_NONE) cx++;
    if (g->weapon != WEAPON_NONE) cx += 2;
    if (g->ability != ABILITY_NONE) cx += 2;
    if (g->eyes != EYES_NONE) cx++;
    if (g->grasper != GRASPER_NONE) cx++;
    if (g->detail != DETAIL_NONE) cx++;
    if (g->spine != SPINE_STANDARD) cx += 3;
    if (g->neck != NECK_STANDARD) cx += 2;
    if (g->reach != REACH_MID) cx += 2;
    if (g->stance != STANCE_QUAD) cx += 3;
    if (g->asym != ASYM_BALANCED) cx += 2;
    cx += g->limbs;
    if (cx > COMPLEXITY_MAX) cx = COMPLEXITY_MAX;
    return cx;
}

int genome_limb_count(const Genome *g) {
    if (!g || g->legs == LEGS_NONE) return 0;
    int n = g->limbs;
    if (n < 2) n = 2;
    if (n > CREATURE_LEGS) n = CREATURE_LEGS;
    return n & ~1;
}

void genome_apply(Creature *c) {
    if (!c) return;
    Genome g = c->genome;
    genome_clamp(&g);
    c->genome = g;

    const SpineMorph *sp = &SPINES[g.spine];
    const ReachMorph *rc = &REACHES[g.reach];

    float leg;
    switch (g.legs) {
    case LEGS_SPRINT:  leg = 1.45f; break;
    case LEGS_JUMP:    leg = 1.15f; break;
    case LEGS_CLIMB:   leg = 1.05f; break;
    case LEGS_FLIPPER: leg = 0.85f; break;
    case LEGS_STUB:    leg = 0.72f; break;
    default:           leg = 0.50f; break;
    }
    c->max_speed = 2.60f * leg * rc->limb * (0.85f + 0.15f * sp->length);

    c->jump_power = (g.legs == LEGS_JUMP) ? 8.5f
                  : (g.legs == LEGS_CLIMB) ? 6.0f
                  : (g.legs == LEGS_SPRINT) ? 5.4f : 4.8f;

    c->attack_power = (g.weapon == WEAPON_CLUB) ? 19.0f
                    : (g.weapon == WEAPON_HORN) ? 16.0f
                    : (g.weapon == WEAPON_POISON) ? 12.0f : 6.0f;

    c->armor = (g.detail == DETAIL_QUILLS) ? 0.28f
             : (g.detail == DETAIL_FIN) ? 0.10f : 0.0f;

    c->sight_range = (g.eyes == EYES_STALK) ? 22.0f
                   : (g.eyes == EYES_LARGE) ? 18.0f
                   : (g.eyes == EYES_SMALL) ? 10.0f : 6.0f;

    c->gather_range = (g.grasper != GRASPER_NONE) ? 3.0f : 1.2f;
    c->scent_range = 7.0f;

    /* Hip sockets, body-local; body_to_world applies scale plus yaw. */
    for (int i = 0; i < CREATURE_LEGS; i++)
        c->hip_local[i] = v3(0.0f, 0.0f, 0.0f);

    int nlegs = genome_limb_count(&g);
    int pairs = nlegs / 2;
    if (pairs < 1) pairs = 1;
    for (int p = 0; p < pairs && (p * 2 + 1) < CREATURE_LEGS; p++) {
        float u = (pairs <= 1) ? 0.5f : (float)p / (float)(pairs - 1);
        float z = 0.35f - 0.70f * u; /* front (+Z) to rear (-Z) */
        float x = 0.30f * (g.asym == ASYM_LEFT ? 1.08f
                         : g.asym == ASYM_RIGHT ? 0.92f : 1.0f);
        c->hip_local[p * 2]     = v3(x, -0.10f, z);
        c->hip_local[p * 2 + 1] = v3(-x, -0.10f, z);
    }
}
