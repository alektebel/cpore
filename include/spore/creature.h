#ifndef SPORE_CREATURE_H
#define SPORE_CREATURE_H

#include "spore/math3d.h"
#include "spore/terrain.h"

#define CREATURE_LEGS 6

/* Spore Creature Creator spine: metaball vertebrae along a column.
 * Start as a blob, then bend / inflate / extend — per the CC manual. */
#define SPINE_MAX_VERTS 24
#define SPINE_MIN_VERTS 3

typedef struct {
    float x, y, z;   /* body-local (Z = nose→tail axis) */
    float radius;    /* metaball size at this vertebra */
} Vertebra;

typedef struct {
    int active;      /* 1 = freeform column drives body silhouette */
    int count;
    Vertebra v[SPINE_MAX_VERTS];
} SpineColumn;

typedef enum {
    DIET_HERBIVORE = 0,
    DIET_CARNIVORE = 1,
    DIET_OMNIVORE  = 2
} Diet;

typedef struct {
    int mouth;
    int legs;
    int weapon;
    int ability;
    int eyes;
    int grasper;
    int detail;
    /* Spore-style freeform morphs (always editable, not unlock-gated) */
    int spine;   /* compact → serpentine body chain */
    int neck;    /* short → crane reach */
    int asym;    /* balanced / left-lead / right-lead hips */
    int stance;  /* quad / biped posture */
    int wpn_sock;/* weapon socket: flanks / head / tail */
    int dtl_sock;/* detail socket: back / crest / rump */
    int reach;   /* stubby / mid / lanky leg stretch */
    int limbs;   /* biped(2) / quad(4) / hex(6) leg pairs */
    int wpn_slide; /* 0..4 freeform slide along body (flank weapons) */
    int dtl_slide; /* 0..4 freeform slide along spine (back details) */
} Genome;

typedef struct {
    Vec3 hip, knee, foot, plant, swing_from, swing_to;
    float phase, swing_t;
    int swinging;
    float upper_len, lower_len;
} Leg;

typedef struct {
    Vec3  pos;
    float yaw, speed, gait_timer;
    Vec3  body, head, neck, tail;
    Leg   legs[CREATURE_LEGS];
    Vec3  hip_local[CREATURE_LEGS];

    Genome genome;
    Diet  diet;
    Vec3  color;
    float scale;
    float health, hunger, dna;
    float attack_cd, attack_power;
    float max_speed, stride_scale;
    float ability_cd;
    float charge_t;
    float sight_range;   /* from eyes */
    float gather_range;  /* from graspers */
    float armor;         /* from detail — damage reduction 0..0.5 */
    float vuln;          /* damage taken multiplier (juvenile frailty > 1) */
    float scent_range;   /* footprint tracking radius */
    float spawn_protect;
    float social;        /* 0..1 charm progress toward ally (herbivores) */
    float elev;          /* height above ground / water float line */
    float vel_y;
    float jump_cd;
    float stun_t;        /* >0 = cannot act (roar stun) */
    float poison_t;      /* >0 = DoT from poison weapon */
    float burrow_t;      /* >0 = dug in (hidden / armored) */
    float camo_t;        /* >0 = camouflage cloak (stealth) */
    float hit_flash;     /* >0 white flash after taking a hit */
    float jump_power;    /* set by legs via genome_apply */
    float threat_t;      /* >0 threat display / intimidate pose */
    float stamina;       /* 0..100 sprint meter */
    int   crouching;     /* stealth crouch — harder to spot, slower */
    int   sprinting;     /* actively burning stamina for speed */
    Vec3  home;          /* territory / spawn anchor */
    Vec3  knock;         /* XZ knockback impulse */
    Vec3  color2;        /* belly / secondary paint */
    int   pattern;       /* 0 none, 1 stripes, 2 spots, 3 bands */
    int   grounded;
    int   in_water;
    int   airborne;
    int   alive, player;
    int   generation;
    int   allied;
    int   herd;          /* 0=lone, >0 flock with same id */
    SpineColumn spine;   /* Spore CC column (blob → shaped body) */
} Creature;

#define PATTERN_NONE    0
#define PATTERN_STRIPES 1
#define PATTERN_SPOTS   2
#define PATTERN_BANDS   3
#define PATTERN_COUNT   4

void creature_init(Creature *c, const Terrain *t, Vec3 pos,
                   Genome genome, Vec3 color, float scale, int player);
void creature_update_locomotion(Creature *c, const Terrain *t, float dt,
                                float move_x, float move_z, int want_jump);
void creature_apply_damage(Creature *c, float amount);
int  creature_try_attack(Creature *attacker, Creature *target);
/* Freeform socket anchors for creator / combat FX */
Vec3 creature_weapon_anchor(const Creature *c);
Vec3 creature_detail_anchor(const Creature *c);
/* Spine vertebrae (Spore-style freeform body chain) */
int  creature_spine_count(const Creature *c);
Vec3 creature_spine_bead(const Creature *c, int index);
float creature_spine_radius(const Creature *c, int index);
/* Map freeform slide 0..4 onto a vertebra index */
int  creature_slide_vertebra(const Creature *c, int slide);

/* --- Spore Creature Creator: column editing --- */
void spine_init_blob(SpineColumn *s);           /* amorphous starter blob */
void spine_ensure_blob(Creature *c);            /* activate blob if inactive */
void spine_sync_anchors(Creature *c);           /* head/neck/tail from column */
int  spine_extend(SpineColumn *s, int front);   /* +1 vertebra at nose/tail */
int  spine_extend_dir(SpineColumn *s, int front, float dx, float dy, float dz);
int  spine_shorten(SpineColumn *s, int front);  /* -1 if past min */
void spine_bend(SpineColumn *s, int i, float dx, float dy, float dz);
/* Aim end vertebra toward a local-space target (Lochner stretch follow). */
void spine_aim_end(SpineColumn *s, int front, float tx, float ty, float tz, float seg_len);
/* Spore/Lochner bilateral symmetry: pin vertebrae to the sagittal plane (x=0). */
void spine_enforce_symmetry(SpineColumn *s);
void spine_inflate(SpineColumn *s, int i, float delta);
Vec3 spine_world(const Creature *c, int i);     /* vertebra world position */

#endif
