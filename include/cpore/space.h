/* cpore stage 6 - SPACE.
 *
 * The last stage Spore had, and the only one still missing here. The scale
 * changes again but the galaxy is not a new universe: every star system
 * carries its own seed, and that seed names the same procedural planet
 * stages 3-5 were played on - the homeworld is the world the creature
 * walked, addressed by the seed the whole campaign ran on.
 *
 * You captain one ship among a galaxy of systems and rival empires, and the
 * stage inherits the fork every scale above it has: there are three ways to
 * grow an empire, and they are three mechanics rather than one wearing three
 * hats -
 *
 *   colonise  - settle free systems outright. Cheap per system, but only
 *               works where nobody lives yet, and a colony is soft until it
 *               grows defence.
 *   trade     - haul spice between price spreads, and buy a rival system's
 *               loyalty run by run. Slow and expensive, but the system
 *               arrives intact and its people unspited - and trading with a
 *               power makes it like you, which war never did.
 *   conquer   - siege a system's defence down and take it. Fast, and the
 *               captured population is halved and every power resents it.
 *
 * Fuel makes distance real, pirates make defence real, and rivals expand on
 * their own, so an empire is something you run rather than a score you wait
 * out. What the civilisation stage hands forward arrives as three
 * multipliers - a military nation is a conquering power, an economic one a
 * trading power, a devout one a settling power.
 *
 * Same contract as every other stage: POD world, RNG inside the struct, no
 * allocation, no I/O, no globals. */
#ifndef CPORE_SPACE_H
#define CPORE_SPACE_H

#include <stdint.h>
#include <stddef.h>
#include "cpore/cpore.h"
#include "cpore/civ.h"     /* the legacy bridge reads a finished stage-4 world;
                              the dependency points one way: civ never sees space */

#ifdef __cplusplus
extern "C" {
#endif

/* the galactic plane, and a tick at civilisation pacing - a galaxy runs on
 * years, not seconds */
#define CP7_GW          2400.0f
#define CP7_GD          2400.0f
#define CP7_DT          (1.0f / 10.0f)
#define CP7_MAX_STEPS   3600

#define CP7_MAX_STARS    36
#define CP7_MAX_EMPIRES   8        /* index 0 is always the player */
#define CP7_PLAYER        0
#define CP7_SPICE_TYPES   4
#define CP7_UP_COUNT      4        /* engine, cargo, weapons, hull */

/* the three ways to grow, and the order they appear in the legacy */
enum { CP7_COLONISE = 0, CP7_TRADE, CP7_ATTACK, CP7_BONUS_COUNT };
const char *cp7_bonus_name(int b);

/* ship upgrades, and the order the upgrade dial cycles them */
enum { CP7_UP_ENGINE = 0, CP7_UP_CARGO, CP7_UP_WEAPONS, CP7_UP_HULL };

/* A star system. The seed that names its planet is hash(seed, index) - the
 * homeworld (index 0) uses the galaxy's own seed, so the arc's planet is
 * literally in the sky. pull is how far a commercial buyout of a rival
 * system has got, 0..1. */
typedef struct {
    float    x, y;
    int32_t  owner;                 /* -1 free, else empire index */
    float    pop, defence, pull;
    float    fertility;             /* grows pop and spice output */
    uint8_t  spice;                 /* what its planet exports */
    uint8_t  pirates;               /* raiders present, 0..255 */
    uint8_t  alive, pad;
    uint8_t  hold;                  /* consolidation window after a capture */
    uint8_t  pad2[3];
} Cp7Star;

typedef struct {
    float    money, income;
    int32_t  stars;
    float    rel;                   /* toward the player, -1 hostile .. 1 allied */
    float    pull;                  /* how far the player has bought its loyalty */
    uint8_t  style;                 /* CP7_COLONISE / TRADE / ATTACK */
    uint8_t  alive, pad[3];
} Cp7Empire;

/* what stage 4 hands forward: a nation that won by force arrives as a
 * conquering power, one that won by trade arrives as a trading power, one
 * that won by faith arrives as a settling power. */
typedef struct { float bonus[CP7_BONUS_COUNT]; } Cp7Legacy;

void cp7_legacy_default(Cp7Legacy *out);
/* bridge from a finished stage-4 world: the nation's own multipliers pass
 * through, doctrine for doctrine. Declared here, defined in space_env.c, so
 * stage 4 never has to know stage 6 exists. */
void cp7_legacy_from_civ(const Cp5World *civ, float *out3);

typedef struct {
    float    x, y, head;            /* head: last heading, for the render */
    float    fuel, hull;
    float    cargo[CP7_SPICE_TYPES];
    float    cargo_cap, speed, weapons, hull_max, fuel_max;
    uint8_t  up[CP7_UP_COUNT];
} Cp7Ship;

/* action vector: thrust x/y, then one dial per verb, then the upgrade dial */
enum { CP7_V_THRUST = 0, CP7_V_TRADE = 2, CP7_V_COLONISE = 3, CP7_V_ATTACK = 4,
       CP7_V_RESUPPLY = 5, CP7_V_UPGRADE = 6 };
#define CP7_ACT_DIM    7

#define CP7_OBS_STAR   10
#define CP7_OBS_STARS  10
#define CP7_OBS_DIM    (CP7_OBS_STARS * CP7_OBS_STAR + 12 \
                         + (CP7_MAX_EMPIRES - 1) * 3 + 4)

enum { CP7_RUN = 0, CP7_LOST = 1, CP7_WON = 2, CP7_TIMEOUT = 3 };

typedef struct {
    CpRng     rng;
    uint32_t  seed;
    int32_t   step;

    Cp7Star   star[CP7_MAX_STARS];
    Cp7Empire empire[CP7_MAX_EMPIRES];
    Cp7Ship   ship;
    int32_t   n_stars, n_empires;

    float     reward, score;
    float     bonus[CP7_BONUS_COUNT];  /* what the civilisation handed forward */
    int32_t   status;
    /* the ledger, so the fork can be audited after the fact */
    int32_t   settled, flipped, captured, lost_stars, trade_runs, raids_fought;
    float     tax_earned, trade_earned, fuel_spent, upgrades;
} Cp7World;

/* legacy may be NULL for an empire that inherited nothing */
void cp7_world_reset(Cp7World *w, uint32_t seed, const Cp7Legacy *legacy);
void cp7_world_step(Cp7World *w, const float act[CP7_ACT_DIM]);
void cp7_world_observe(const Cp7World *w, float *obs);
void cp7_policy_greedy(const Cp7World *w, float act[CP7_ACT_DIM]);

typedef struct Cp7Env Cp7Env;
Cp7Env *cp7_env_create(uint32_t seed);
void    cp7_env_free(Cp7Env *e);
void    cp7_env_reset(Cp7Env *e, uint32_t seed, const float *legacy, float *obs);
void    cp7_env_step(Cp7Env *e, const float *act, float *obs,
                     float *reward, int32_t *terminated, int32_t *truncated);
int32_t cp7_env_obs_dim(void);
int32_t cp7_env_act_dim(void);
size_t  cp7_env_state_size(void);
void    cp7_env_save(const Cp7Env *e, void *dst);
void    cp7_env_load(Cp7Env *e, const void *src);
const Cp7World *cp7_env_world(const Cp7Env *e);
/* counts: stars, settled, flipped, captured, lost, trade_runs, raids
 * vals:   money, income, bonus colonise/trade/attack */
void    cp7_env_census(const Cp7Env *e, int32_t *counts, float *vals);
int32_t cp7_env_status(const Cp7Env *e);

/* the planet seed a system's map would be drawn from - the homeworld is the
 * campaign seed itself, which is what makes the galaxy the same universe the
 * lower stages were played in */
uint32_t cp7_star_seed(uint32_t galaxy_seed, int star_index);

/* top-down map of the galaxy. styled() matches the other stages' signature;
 * v1 renders the map directly (no palette tier). */
void cp7_render(const Cp7World *w, uint8_t *rgba, int width, int height);
void cp7_render_styled(const Cp7World *w, uint8_t *rgba, int width, int height,
                       int style);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_SPACE_H */
