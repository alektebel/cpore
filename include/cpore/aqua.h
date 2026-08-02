/* cpore stage 2 - AQUATIC.
 *
 * A genuinely three-dimensional world: creatures swim in a volume of water
 * with a surface above and a seabed below, and depth is a real axis of the
 * problem rather than decoration. Light falls off with depth, plankton drifts
 * near the surface, carrion sinks, and a creature that cannot see in the dark
 * cannot hunt there.
 *
 * The NPC population is not scripted. Every fish carries a genome, spends
 * energy to live, and reproduces with mutation when it has banked enough.
 * Nothing enforces which body plans succeed - the ones that feed themselves
 * leave more copies, and the population drifts. */
#ifndef CPORE_AQUA_H
#define CPORE_AQUA_H

#include <stdint.h>
#include <stddef.h>
#include "cpore/cpore.h"      /* CpRng, CP_VIS_*, cp_png_write */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- world extents. y is depth: 0 is the surface, CP3_H is the seabed. ---- */
#define CP3_W          1400.0f
#define CP3_D          1400.0f
#define CP3_H           620.0f
#define CP3_DT         (1.0f / 60.0f)
#define CP3_MAX_STEPS   9000

#define CP3_MAX_FISH     56
#define CP3_MAX_FOOD    520
#define CP3_MAX_PARTS     10
#define CP3_MAX_SEG        6

#define CP3_BIOMASS_GOAL 100.0f
#define CP3_GENERATIONS    4

/* ---- parts ----
 * Mounted at a segment index plus a yaw/pitch on the body, so placement is a
 * direction in three dimensions rather than an angle on a circle. */
enum {
    CP3_NONE = 0,
    CP3_FILTER,     /* filter mouth  - plankton                        */
    CP3_JAW,        /* jaw           - meat, and bites through an arc  */
    CP3_FIN,        /* fin           - turn rate, and lift             */
    CP3_TAIL,       /* tail          - forward thrust                  */
    CP3_SPIKE,      /* spike         - directional damage and armour   */
    CP3_EYE,        /* eye           - perception, scaled by light     */
    CP3_LUNG,       /* air bladder   - stamina, and buoyancy control   */
    CP3_PLATE,      /* armour plate  - flat damage reduction, heavy    */
    CP3_LIGHT,      /* photophore    - carries its own light down deep */
    CP3_PART_COUNT
};

extern const int CP3_GEN_BUDGET[CP3_GENERATIONS];
const char *cp3_part_name(int type);
int         cp3_part_cost(int type);

typedef struct {
    uint8_t type;
    uint8_t seg;      /* which spine segment it sits on          */
    uint8_t yaw;      /* 0..255 around the body axis             */
    int8_t  pitch;    /* -64..63, up/down from the flank         */
    uint8_t scale;    /* 0..255 -> 0.45x .. 1.9x the type's size */
    uint8_t mirror;   /* nonzero: the part also exists at -yaw.
                       * Bilateral symmetry is the single thing that makes a
                       * generated body read as an animal rather than a lump,
                       * so it is genetic, and a mirrored part costs twice. */
} Cp3Part;

/* Pattern genome. Colour carries as much perceived variety as shape does, and
 * it is far cheaper, so it gets its own genes and its own inheritance. */
enum { CP3_PAT_PLAIN = 0, CP3_PAT_BANDS, CP3_PAT_SPOTS, CP3_PAT_COUNTER,
       CP3_PAT_COUNT };

typedef struct {
    Cp3Part part[CP3_MAX_PARTS];
    uint8_t nseg;     /* 2..CP3_MAX_SEG spine segments */
    uint8_t girth;    /* 0..255 -> body radius          */

    /* Body profile: radius multipliers at four stations from nose to tail.
     * One girth scalar could only make a fish fatter; four stations give
     * eels, torpedoes, discs, tadpoles and barrels out of the same code. */
    uint8_t prof[4];  /* 0..255 -> 0.30x .. 1.80x */
    int8_t  arch;     /* spine bow, up/down  */
    int8_t  sweep;    /* spine bow, sideways */

    uint8_t hue, hue2;  /* base and pattern colour */
    uint8_t sat, val;   /* saturation and lightness */
    uint8_t pattern;    /* CP3_PAT_*  */
    uint8_t pscale;     /* pattern frequency */
} Cp3Genome;

/* profile multiplier at t in [0,1] along the body, from the four stations */
float cp3_profile(const Cp3Genome *g, float t);
/* base and pattern colour as linear rgb */
void  cp3_genome_colour(const Cp3Genome *g, float *rgb, float *rgb2);

/* observation / action shape */
#define CP3_OBS_FOOD_K   8
#define CP3_OBS_FISH_K   6
#define CP3_OBS_DIM      (16 + CP3_OBS_FOOD_K * 5 + CP3_OBS_FISH_K * 7 + (CP3_PART_COUNT - 1) + 6)
#define CP3_ACT_CTRL     4        /* yaw rate, pitch rate, thrust, bite */
#define CP3_ACT_DIM      (CP3_ACT_CTRL + CP3_MAX_PARTS * 4 + 2)

enum { CP3_FOOD_NONE = 0, CP3_FOOD_PLANKTON = 1, CP3_FOOD_CARRION = 2 };
enum { CP3_RUN = 0, CP3_DEAD = 1, CP3_EVOLVED = 2, CP3_TIMEOUT = 3 };

/* ---- vec3 ---- */
typedef struct { float x, y, z; } Cp3Vec;

typedef struct {
    float speed, turn, accel, drag;
    float hp_max, armor, bite, spike_dmg;
    float filter_eff, carn_eff;
    float percep, light;         /* light: own photophores            */
    float buoy;                  /* neutral-depth control from lungs  */
    float upkeep, radius, length;
    uint8_t n[CP3_PART_COUNT];
    uint8_t n_parts;
    int16_t cost;
} Cp3Stats;

void  cp3_genome_clear(Cp3Genome *g);
void  cp3_genome_starter(Cp3Genome *g);
int   cp3_genome_cost(const Cp3Genome *g);
void  cp3_genome_normalise(Cp3Genome *g, int budget);
void  cp3_genome_random(Cp3Genome *g, CpRng *r, int budget);
void  cp3_genome_mutate(Cp3Genome *g, CpRng *r, int budget, float rate);
void  cp3_genome_stats(const Cp3Genome *g, Cp3Stats *out);
void  cp3_genome_from_action(Cp3Genome *g, const float *design, int budget);
void  cp3_genome_autodesign(Cp3Genome *g, CpRng *r, int budget, int style);
#define CP3_STYLE_GRAZER 0
#define CP3_STYLE_HUNTER 1
#define CP3_STYLE_DIVER  2
#define CP3_STYLE_COUNT  3

/* ---- entities ---- */
typedef struct {
    Cp3Vec  p;
    float   r, phase;
    uint8_t type;
    uint8_t pad[3];
} Cp3Food;

typedef struct {
    Cp3Vec    p, v;
    float     yaw, pitch;
    float     hp, hp_max, energy;
    float     phase;
    Cp3Genome g;
    Cp3Stats  s;                /* derived once at birth, not per step */
    float     think_t;
    Cp3Vec    des;              /* cached steering target */
    uint8_t   alive, has_target, lineage, gen;
    float     age;
} Cp3Fish;

/* ---- world ---- */
typedef struct {
    CpRng     rng;
    uint32_t  seed;
    int32_t   step;

    Cp3Fish   player;
    float     biomass;          /* the stage's progress meter */
    int32_t   generation;
    float     bite_cd;

    Cp3Food   food[CP3_MAX_FOOD];
    int32_t   n_food, food_cursor;
    Cp3Fish   fish[CP3_MAX_FISH];

    /* population telemetry - the whole point of letting them breed */
    int32_t   births, deaths, pop;
    float     mean_parts, mean_mouth, mean_tail, mean_light, mean_depth;
    float     mean_gen;

    float     reward;
    int32_t   status;
    int32_t   ate_plankton, ate_carrion, kills, hits_taken, design_events;
    float     dmg_dealt, dmg_taken;
} Cp3World;

void cp3_world_reset(Cp3World *w, uint32_t seed, const Cp3Genome *g);
void cp3_world_step(Cp3World *w, const float act[CP3_ACT_DIM]);
void cp3_world_observe(const Cp3World *w, float *obs);
void cp3_policy_greedy(const Cp3World *w, float act[CP3_ACT_DIM]);

/* ambient light reaching a given depth, 1 at the surface -> ~0 at the seabed */
float cp3_daylight(float depth);

/* ---- RL env ABI ---- */
typedef struct Cp3Env Cp3Env;
Cp3Env *cp3_env_create(uint32_t seed);
void    cp3_env_free(Cp3Env *e);
void    cp3_env_reset(Cp3Env *e, uint32_t seed, const int32_t *parts, float *obs);
void    cp3_env_step(Cp3Env *e, const float *act, float *obs,
                     float *reward, int32_t *terminated, int32_t *truncated);
int32_t cp3_env_obs_dim(void);
int32_t cp3_env_act_dim(void);
size_t  cp3_env_state_size(void);
void    cp3_env_save(const Cp3Env *e, void *dst);
void    cp3_env_load(Cp3Env *e, const void *src);
const Cp3World *cp3_env_world(const Cp3Env *e);
/* population telemetry: counts = {births, deaths, pop},
 * means = {gen, parts, mouth, tail, light, depth} */
void    cp3_env_census(const Cp3Env *e, int32_t *counts, float *means);

/* ---- rendering: sphere impostors into a z-buffer, then the same
 *      palette-quantised pixel pipeline as stage 1 ---- */
void cp3_render(const Cp3World *w, uint8_t *rgba, int width, int height);
void cp3_render_styled(const Cp3World *w, uint8_t *rgba, int width, int height,
                       int style);
/* Render one genome side-on against a plain ground, at the caller's
 * resolution and already palette-quantised. Used to lay out contact sheets so
 * the variety a genome space actually produces can be looked at directly. */
void cp3_render_portrait(const Cp3Genome *g, uint8_t *rgba_low, int lw, int lh,
                         int style, uint32_t seed);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_AQUA_H */
