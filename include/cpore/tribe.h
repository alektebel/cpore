/* cpore stage 5 - TRIBE.
 *
 * The gap Spore filled between creature and civilisation: the species
 * settles. The unit of play stops being one animal and becomes a tribe -
 * members, food stores, tools, huts - on the same planet, from the same
 * seed, so the valley the creature walked is the valley its tribe farms.
 *
 * Charm and violence stay the two currencies: a neighbouring tribe is
 * befriended with gifts or removed with raids, and the body the species
 * evolved decides which is cheap - fangs raid well, songs befriend well,
 * legs gather further. Imported share-code genomes found rival tribes, so
 * Phase-3 invasions ride the whole arc: a friend's creature becomes a
 * neighbouring tribe, then a nation.
 *
 * Same contract as every other stage: POD world, RNG inside the struct, no
 * allocation, no I/O, no globals. */
#ifndef CPORE_TRIBE_H
#define CPORE_TRIBE_H

#include <stdint.h>
#include <stddef.h>
#include "cpore/land.h"   /* Cp4Genome, cp4_height, biomes */

#ifdef __cplusplus
extern "C" {
#endif

#define CP6_W          2200.0f
#define CP6_D          2200.0f
#define CP6_SEA        CP4_SEA
#define CP6_DT         (1.0f / 10.0f)   /* a tribe ticks slower than a beast */
#define CP6_MAX_STEPS  3600

#define CP6_MAX_TRIBES   6   /* index 0 is always the player */
#define CP6_PLAYER       0
#define CP6_MAX_IMPORTS  3   /* share-code lineages riding along */

enum { CP6_RAID = 0, CP6_GATHER, CP6_CHARM, CP6_BONUS_COUNT };

/* A tribe: where it lives, what it has, how it feels about the player.
 * standing -1 hostile .. +1 allied; allied at +1, dead at members 0. */
typedef struct {
    float    x, z;
    float    members, stores, tools, huts;
    float    standing;
    float    bonus[CP6_BONUS_COUNT];  /* raid / gather / charm multipliers */
    uint8_t  style;                   /* rival specialty: CP6_RAID..CHARM */
    uint8_t  alive, allied, pad;
    Cp4Genome genome;                 /* the body this tribe grew out of */
} Cp6Tribe;

#define CP6_OBS_TRIBE   6
#define CP6_OBS_DIM     (8 + (CP6_MAX_TRIBES - 1) * CP6_OBS_TRIBE + 3)
/* workforce mix (gather/build/raise), per-rival stance (-raid..+befriend),
 * hut investment */
#define CP6_ACT_DIM     (3 + (CP6_MAX_TRIBES - 1) + 1)

enum { CP6_RUN = 0, CP6_LOST = 1, CP6_WON = 2, CP6_TIMEOUT = 3 };

typedef struct {
    CpRng     rng;
    uint32_t  seed;
    int32_t   step;

    Cp6Tribe  tribe[CP6_MAX_TRIBES];
    int32_t   n_tribes;

    float     reward, score;
    int32_t   status;
    int32_t   allied, razed, gifts, raids, loot;
    float     gathered, lost_members;
} Cp6World;

/* founder: the species that settled (NULL: a default grazer). imports: up to
 * CP6_MAX_IMPORTS rival genomes (share-code invaders); null/0 for none. */
void cp6_world_reset(Cp6World *w, uint32_t seed, const Cp4Genome *founder,
                     const Cp4Genome *imports, int n_imports);
void cp6_world_step(Cp6World *w, const float act[CP6_ACT_DIM]);
void cp6_world_observe(const Cp6World *w, float *obs);
void cp6_policy_greedy(const Cp6World *w, float act[CP6_ACT_DIM]);

typedef struct Cp6Env Cp6Env;
Cp6Env *cp6_env_create(uint32_t seed);
void    cp6_env_free(Cp6Env *e);
void    cp6_env_reset(Cp6Env *e, uint32_t seed, const int32_t *parts, float *obs);
void    cp6_env_step(Cp6Env *e, const float *act, float *obs,
                     float *reward, int32_t *terminated, int32_t *truncated);
int32_t cp6_env_obs_dim(void);
int32_t cp6_env_act_dim(void);
size_t  cp6_env_state_size(void);
void    cp6_env_save(const Cp6Env *e, void *dst);
void    cp6_env_load(Cp6Env *e, const void *src);
const Cp6World *cp6_env_world(const Cp6Env *e);
/* counts: members, allied, razed, gifts, raids, loot(int) | vals: stores,
 * gathered, standing_sum, score */
void    cp6_env_census(const Cp6Env *e, int32_t *counts, float *vals);
int32_t cp6_env_status(const Cp6Env *e);

/* top-down map of the same planet the creature walked. styled() matches the
 * other stages' signature; v1 renders the map directly (no palette tier). */
void cp6_render(const Cp6World *w, uint8_t *rgba, int width, int height);
void cp6_render_styled(const Cp6World *w, uint8_t *rgba, int width, int height,
                       int style);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_TRIBE_H */
