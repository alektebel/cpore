/* cpore stage 4 - CIVILISATION.
 *
 * The scale changes but the planet does not. This stage runs on the same
 * heightfield stage 3 walked around on, from the same seed, so the mountain
 * the player's species evolved beside is the mountain its cities are now built
 * against. cp4_height() being a pure function is what makes that free.
 *
 * Three ways to take a city, exactly as Spore had it: break it, buy it, or
 * convert it. They are not three skins on one attack - military spends
 * defence, economic spends money, religious spends time - and a nation's
 * strengths carry forward from the body it evolved, so a species that won the
 * creature stage by singing is a missionary power here and a species that won
 * it by hunting is a military one.
 *
 * Same contract as every other stage: POD world, RNG inside the struct, no
 * allocation, no I/O, no globals. */
#ifndef CPORE_CIV_H
#define CPORE_CIV_H

#include <stdint.h>
#include <stddef.h>
#include "cpore/cpore.h"
#include "cpore/land.h"

#ifdef __cplusplus
extern "C" {
#endif

/* the same box stage 3 used, because it is the same planet */
#define CP5_W          2200.0f
#define CP5_D          2200.0f
#define CP5_SEA          CP4_SEA
#define CP5_DT         (1.0f / 10.0f)   /* a civilisation ticks slower */
#define CP5_MAX_STEPS   3600

#define CP5_MAX_CITIES    12
#define CP5_MAX_UNITS     96
/* the player plus one rival per approach, so every strategy in the stage is
 * represented on the board and there is room on the map to actually win */
#define CP5_MAX_NATIONS    4            /* index 0 is always the player */
#define CP5_PLAYER         0

/* the three approaches, and the order they appear in the action vector */
enum { CP5_MIL = 0, CP5_ECO, CP5_REL, CP5_APPROACH_COUNT };

const char *cp5_approach_name(int a);

typedef struct { float x, z; } Cp5Vec;

/* A city. allegiance is how firmly it belongs to its owner: military work
 * grinds down `defence`, economic and religious work grind down `allegiance`,
 * and a city whose allegiance reaches zero changes hands without a shot. */
typedef struct {
    Cp5Vec   p;
    int32_t  owner;
    float    pop, defence, allegiance, unrest;
    float    build_t;
    int32_t  spice;                 /* geysers inside the city's radius */
    int32_t  contest;               /* nation currently converting it, or -1 */
    float    convert;               /* how far that conversion has got, 0..1 */
    uint8_t  alive, pad[3];
} Cp5City;

typedef struct {
    Cp5Vec   p;
    float    hp, cargo;
    int32_t  owner, target;
    uint8_t  type, alive, pad[2];
} Cp5Unit;

typedef struct {
    float    money, income;
    int32_t  cities, built, lost, taken;
    float    bonus[CP5_APPROACH_COUNT];   /* what the species evolved into */
    uint8_t  alive, style, pad[2];
} Cp5Nation;

/* What stage 3 hands forward. Derived from the body that finished that stage,
 * so the civilisation inherits the creature's strengths rather than starting
 * from a blank slate. */
typedef struct { float bonus[CP5_APPROACH_COUNT]; } Cp5Legacy;

void cp5_legacy_from_creature(const Cp4Genome *g, Cp5Legacy *out);
/* The same thing straight off a finished stage-3 world, which is how a run is
 * actually chained: play the creature stage, read the body that survived it,
 * start the civilisation stage with what that body was good at. */
void cp5_legacy_from_world(const Cp4World *land, float *out3);
void cp5_legacy_default(Cp5Legacy *out);

#define CP5_OBS_CITY   9
#define CP5_OBS_DIM    (CP5_MAX_CITIES * CP5_OBS_CITY + 12 \
                          + CP5_MAX_NATIONS * 3)
/* mix over the three approaches, a priority per city, then tax and posture */
#define CP5_ACT_DIM    (CP5_APPROACH_COUNT + CP5_MAX_CITIES + 2)

enum { CP5_RUN = 0, CP5_LOST = 1, CP5_WON = 2, CP5_TIMEOUT = 3 };

typedef struct {
    CpRng     rng;
    uint32_t  seed;
    int32_t   step;

    Cp5City   city[CP5_MAX_CITIES];
    Cp5Unit   unit[CP5_MAX_UNITS];
    Cp5Nation nation[CP5_MAX_NATIONS];
    Cp5Vec    spice[CP5_MAX_CITIES];
    int32_t   n_cities, n_spice, n_nations;

    float     reward;
    int32_t   status;
    int32_t   captured, converted, bought, cities_lost;
    int32_t   units_built[CP5_APPROACH_COUNT];
    float     spent[CP5_APPROACH_COUNT];
} Cp5World;

void cp5_world_reset(Cp5World *w, uint32_t seed, const Cp5Legacy *legacy);
void cp5_world_step(Cp5World *w, const float act[CP5_ACT_DIM]);
void cp5_world_observe(const Cp5World *w, float *obs);
void cp5_policy_greedy(const Cp5World *w, float act[CP5_ACT_DIM]);

typedef struct Cp5Env Cp5Env;
Cp5Env *cp5_env_create(uint32_t seed);
void    cp5_env_free(Cp5Env *e);
void    cp5_env_reset(Cp5Env *e, uint32_t seed, const float *legacy, float *obs);
void    cp5_env_step(Cp5Env *e, const float *act, float *obs,
                     float *reward, int32_t *terminated, int32_t *truncated);
int32_t cp5_env_obs_dim(void);
int32_t cp5_env_act_dim(void);
size_t  cp5_env_state_size(void);
void    cp5_env_save(const Cp5Env *e, void *dst);
void    cp5_env_load(Cp5Env *e, const void *src);
const Cp5World *cp5_env_world(const Cp5Env *e);
/* counts: cities, captured, converted, bought, lost, units mil/eco/rel
 * vals:   money, income, bonus mil/eco/rel */
void    cp5_env_census(const Cp5Env *e, int32_t *counts, float *vals);

void cp5_render(const Cp5World *w, uint8_t *rgba, int width, int height);
void cp5_render_styled(const Cp5World *w, uint8_t *rgba, int width, int height,
                       int style);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_CIV_H */
