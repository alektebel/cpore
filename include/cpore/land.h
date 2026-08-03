/* cpore stage 3 - CREATURE.
 *
 * Out of the water and onto a heightfield. Depth stops being the axis and
 * terrain takes over: hills cost stamina to climb, ridges block sight, and
 * water is now a hazard rather than a home.
 *
 * The stage's real subject is other species. Each rival nest holds a lineage
 * with its own genome, and every encounter is the same fork Spore built the
 * creature stage around - impress it or eat it. Both fill the DNA meter, and
 * they pull the body plan in opposite directions, because charm and violence
 * are bought with the same budget.
 *
 * As in stage 2 the population is not scripted: nests breed, mutate, and are
 * selected by whether their occupants can feed themselves. */
#ifndef CPORE_LAND_H
#define CPORE_LAND_H

#include <stdint.h>
#include <stddef.h>
#include "cpore/cpore.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CP4_W          2200.0f      /* x extent */
#define CP4_D          2200.0f      /* z extent */
#define CP4_SKY         520.0f      /* how high the camera may go */
#define CP4_SEA          26.0f      /* terrain below this is water */
#define CP4_DT         (1.0f / 60.0f)
#define CP4_MAX_STEPS   9000

#define CP4_MAX_BEASTS   64
#define CP4_MAX_FLORA   380
#define CP4_MAX_NESTS      7
#define CP4_MAX_PARTS     12
#define CP4_MAX_SEG        6

#define CP4_DNA_GOAL   100.0f
#define CP4_GENERATIONS  4

/* ---- parts ---- */
enum {
    CP4_NONE = 0,
    CP4_MOUTH_G,    /* grazing mouth  - plants                          */
    CP4_MOUTH_C,    /* carnivore jaw  - meat, and bites                 */
    CP4_MOUTH_O,    /* omnivore beak  - both, less efficiently          */
    CP4_LEG,        /* leg            - speed, and carries body weight  */
    CP4_FOOT,       /* broad foot     - grip, so slopes stop costing    */
    CP4_CLAW,       /* claw           - directional damage              */
    CP4_HORN,       /* horn           - damage and armour, front-loaded */
    CP4_PLATE,      /* dermal plate   - armour, heavy                   */
    CP4_EYE,        /* eye            - sight range                     */
    CP4_EAR,        /* ear            - sight through cover and at night*/
    CP4_VOICE,      /* voice sac      - social reach                    */
    CP4_PLUME,      /* display plume  - social power                    */
    CP4_WING,       /* wing           - glide, and a jump that carries  */
    CP4_PART_COUNT
};

extern const int CP4_GEN_BUDGET[CP4_GENERATIONS];
const char *cp4_part_name(int type);
int         cp4_part_cost(int type);

typedef struct {
    uint8_t type;
    uint8_t seg;
    uint8_t yaw;
    int8_t  pitch;
    uint8_t scale;
    uint8_t mirror;
} Cp4Part;

enum { CP4_PAT_PLAIN = 0, CP4_PAT_BANDS, CP4_PAT_SPOTS, CP4_PAT_COUNTER,
       CP4_PAT_STRIPES, CP4_PAT_MOTTLE, CP4_PAT_GRADIENT, CP4_PAT_RINGS,
       CP4_PAT_COUNT };

typedef struct {
    Cp4Part part[CP4_MAX_PARTS];
    uint8_t nseg, girth;
    uint8_t prof[4];
    int8_t  lump[CP4_MAX_SEG];
    int8_t  arch, sweep;
    uint8_t hue, hue2, sat, val;
    uint8_t pattern, pscale;
} Cp4Genome;

typedef struct {
    float speed, accel, turn, jump, grip;
    float hp_max, armor, bite, claw_dmg;
    float graze_eff, carn_eff;
    float sight, hearing;
    float charm, social_reach;     /* the other half of the stage */
    float stamina, upkeep;
    float radius, length, stand;   /* stand: how high the body rides */
    uint8_t n[CP4_PART_COUNT];
    uint8_t n_parts;
    int16_t cost;
} Cp4Stats;

void  cp4_genome_clear(Cp4Genome *g);
void  cp4_genome_starter(Cp4Genome *g);
int   cp4_genome_cost(const Cp4Genome *g);
void  cp4_genome_normalise(Cp4Genome *g, int budget);
void  cp4_genome_random(Cp4Genome *g, CpRng *r, int budget);
void  cp4_genome_mutate(Cp4Genome *g, CpRng *r, int budget, float rate);
void  cp4_genome_stats(const Cp4Genome *g, Cp4Stats *out);
void  cp4_genome_from_action(Cp4Genome *g, const float *design, int budget);
void  cp4_genome_autodesign(Cp4Genome *g, CpRng *r, int budget, int style);
float cp4_profile(const Cp4Genome *g, float t);
void  cp4_genome_colour(const Cp4Genome *g, float *rgb, float *rgb2);
#define CP4_STYLE_GRAZER  0
#define CP4_STYLE_PREDATOR 1
#define CP4_STYLE_CHARMER 2
#define CP4_STYLE_COUNT   3

/* ---- terrain ----
 * A deterministic function of world seed and position, so it needs no storage
 * and the same seed always grows the same hills. */
float cp4_height(uint32_t seed, float x, float z);
void  cp4_normal(uint32_t seed, float x, float z, float *nx, float *ny, float *nz);

typedef struct { float x, y, z; } Cp4Vec;

enum { CP4_FLORA_NONE = 0, CP4_FLORA_BUSH = 1, CP4_FLORA_CARCASS = 2 };

typedef struct {
    Cp4Vec  p;
    float   r, regrow;
    uint8_t type;
    uint8_t pad[3];
} Cp4Flora;

/* a rival species: a nest, a lineage, and how it feels about the player */
typedef struct {
    Cp4Vec    p;
    Cp4Genome g;
    float     standing;      /* -1 hostile .. +1 allied */
    int32_t   members, befriended, eaten;
    uint8_t   alive, style, pad[2];
} Cp4Nest;

typedef struct {
    Cp4Vec    p, v;
    float     yaw, pitch, phase;
    float     hp, hp_max, energy, stam;
    Cp4Genome g;
    Cp4Stats  s;
    Cp4Vec    des;
    float     think_t, sing_t, atk_cd;
    uint8_t   alive, nest, has_target, grounded;
    uint8_t   gen, pad[3];
    float     age;
} Cp4Beast;

#define CP4_OBS_FLORA_K 8
#define CP4_OBS_BEAST_K 6
#define CP4_OBS_DIM   (18 + CP4_OBS_FLORA_K * 5 + CP4_OBS_BEAST_K * 8 \
                          + (CP4_PART_COUNT - 1) + 7)
#define CP4_ACT_CTRL   6      /* turn, pitch, move, jump, attack, sing */
#define CP4_ACT_DIM   (CP4_ACT_CTRL + CP4_MAX_PARTS * 4 + 2)

enum { CP4_RUN = 0, CP4_DEAD = 1, CP4_EVOLVED = 2, CP4_TIMEOUT = 3 };

typedef struct {
    CpRng     rng;
    uint32_t  seed;
    int32_t   step;

    Cp4Beast  player;
    float     dna;
    int32_t   generation;
    float     attack_cd, sing_cd;

    Cp4Flora  flora[CP4_MAX_FLORA];
    int32_t   n_flora, flora_cursor;
    Cp4Beast  beast[CP4_MAX_BEASTS];
    Cp4Nest   nest[CP4_MAX_NESTS];

    int32_t   births, deaths, pop;
    float     mean_parts, mean_legs, mean_charm, mean_gen;
    int32_t   allies, enemies;

    float     reward;
    int32_t   status;
    int32_t   ate_plant, ate_meat, kills, hits_taken, songs, befriended;
    float     dmg_dealt, dmg_taken;
} Cp4World;

void cp4_world_reset(Cp4World *w, uint32_t seed, const Cp4Genome *g);
void cp4_world_step(Cp4World *w, const float act[CP4_ACT_DIM]);
void cp4_world_observe(const Cp4World *w, float *obs);
void cp4_policy_greedy(const Cp4World *w, float act[CP4_ACT_DIM]);

typedef struct Cp4Env Cp4Env;
Cp4Env *cp4_env_create(uint32_t seed);
void    cp4_env_free(Cp4Env *e);
void    cp4_env_reset(Cp4Env *e, uint32_t seed, const int32_t *parts, float *obs);
void    cp4_env_step(Cp4Env *e, const float *act, float *obs,
                     float *reward, int32_t *terminated, int32_t *truncated);
int32_t cp4_env_obs_dim(void);
int32_t cp4_env_act_dim(void);
size_t  cp4_env_state_size(void);
void    cp4_env_save(const Cp4Env *e, void *dst);
void    cp4_env_load(Cp4Env *e, const void *src);
const Cp4World *cp4_env_world(const Cp4Env *e);
/* counts: births, deaths, pop, allies, enemies, befriended, kills
 * means:  gen, parts, legs, charm, dna */
void cp4_env_census(const Cp4Env *e, int32_t *counts, float *means);

void cp4_render(const Cp4World *w, uint8_t *rgba, int width, int height);
void cp4_render_styled(const Cp4World *w, uint8_t *rgba, int width, int height,
                       int style);
void cp4_render_portrait(const Cp4Genome *g, uint8_t *fb, int lw, int lh,
                         int style, uint32_t seed);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_LAND_H */
