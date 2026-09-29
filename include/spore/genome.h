#ifndef SPORE_GENOME_H
#define SPORE_GENOME_H

/* The part catalogue and the morph tables a Genome indexes, plus the one
 * function that turns a Genome into the numbers a Creature moves by.
 * Everything here is data; creature.c owns the motion. */

#include "spore/creature.h"

/* --- morph axes (index into the tables below) --- */
enum { SPINE_COMPACT = 0, SPINE_STANDARD, SPINE_LONG, SPINE_SERPENTINE,
       SPINE_MORPH_COUNT };
enum { NECK_SHORT = 0, NECK_STANDARD, NECK_CRANE, NECK_MORPH_COUNT };
enum { REACH_STUBBY = 0, REACH_MID, REACH_LANKY, REACH_MORPH_COUNT };
enum { STANCE_QUAD = 0, STANCE_BIPED, STANCE_MORPH_COUNT };
enum { ASYM_BALANCED = 0, ASYM_LEFT, ASYM_RIGHT, ASYM_COUNT };
enum { WSOCK_FLANK = 0, WSOCK_HEAD, WSOCK_TAIL, WSOCK_COUNT };
enum { DSOCK_BACK = 0, DSOCK_CREST, DSOCK_RUMP, DSOCK_COUNT };
#define SLIDE_COUNT 5

/* --- creator part slots (Tab cycles these in order) --- */
enum { PART_SLOT_MOUTH = 0, PART_SLOT_LEGS, PART_SLOT_WEAPON, PART_SLOT_ABILITY,
       PART_SLOT_EYES, PART_SLOT_GRASPER, PART_SLOT_DETAIL, PART_SLOT_COUNT };

enum { MOUTH_NONE = 0, MOUTH_JAW, MOUTH_BEAK, MOUTH_PROBOSCIS, MOUTH_SUCKER,
       MOUTH_COUNT };
enum { LEGS_NONE = 0, LEGS_STUB, LEGS_SPRINT, LEGS_JUMP, LEGS_CLIMB,
       LEGS_FLIPPER, LEGS_COUNT };
enum { WEAPON_NONE = 0, WEAPON_HORN, WEAPON_CLUB, WEAPON_POISON,
       WEAPON_COUNT };
enum { ABILITY_NONE = 0, ABILITY_SING, ABILITY_CHARGE, ABILITY_ROAR,
       ABILITY_COUNT };
enum { EYES_NONE = 0, EYES_SMALL, EYES_LARGE, EYES_STALK, EYES_COUNT };
enum { GRASPER_NONE = 0, GRASPER_HANDS, GRASPER_CLAWS, GRASPER_COUNT };
enum { DETAIL_NONE = 0, DETAIL_FIN, DETAIL_WING, DETAIL_QUILLS, DETAIL_COUNT };

#define COMPLEXITY_MAX 100

typedef struct { const char *name; float length; int segs; } SpineMorph;
typedef struct { const char *name; float reach; } NeckMorph;
typedef struct { const char *name; float limb; float posture; } ReachMorph;
typedef struct { const char *name; float upright; } StanceMorph;

extern const SpineMorph SPINES[SPINE_MORPH_COUNT];
extern const NeckMorph NECKS[NECK_MORPH_COUNT];
extern const ReachMorph REACHES[REACH_MORPH_COUNT];
extern const StanceMorph STANCES[STANCE_MORPH_COUNT];

/* --- part unlocks (which catalogue entries a lineage has earned) --- */
typedef struct { unsigned mask[PART_SLOT_COUNT]; } PartUnlocks;

void        unlocks_grant(PartUnlocks *u, int slot, int part);
int         unlocks_cycle(const PartUnlocks *u, int slot, int cur);
const char *unlocks_part_name(int slot, int part);

/* --- genome --- */
Genome genome_starter(void);
void   genome_clamp(Genome *g);
int    genome_complexity(const Genome *g);
int    genome_limb_count(const Genome *g);
void   genome_apply(Creature *c);

#endif
