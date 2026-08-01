/* cpore - a Spore-like life simulation built as a reinforcement learning
 * environment. Headless-first: the simulation core has no I/O and no
 * rendering dependency. The renderer reads world state, never the reverse.
 *
 * Stage 1 of 5: CELL. */
#ifndef CPORE_H
#define CPORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- world limits (fixed, so CpWorld stays a memcpy-able POD) ---- */
#define CP_MAX_FOOD    640
#define CP_MAX_CELLS   48

#define CP_WORLD_W     2400.0f
#define CP_WORLD_H     1400.0f
#define CP_DT          (1.0f / 60.0f)
#define CP_MAX_STEPS   6000

/* food lookup grid */
#define CP_GRID_CS     80.0f
#define CP_GRID_W      30
#define CP_GRID_H      18
#define CP_GRID_N      (CP_GRID_W * CP_GRID_H)

/* coarser grid for live cells - they move, so this one is rebuilt each step */
#define CP_CGRID_CS    160.0f
#define CP_CGRID_W     15
#define CP_CGRID_H     9
#define CP_CGRID_N     (CP_CGRID_W * CP_CGRID_H)

/* observation shape */
#define CP_OBS_FOOD_K  8
#define CP_OBS_CELL_K  6
#define CP_OBS_DIM     (10 + CP_OBS_FOOD_K * 4 + CP_OBS_CELL_K * 6 + 6)
#define CP_ACT_DIM     3

#define CP_DNA_GOAL    100.0f

enum { CP_FOOD_NONE = 0, CP_FOOD_PLANT = 1, CP_FOOD_MEAT = 2 };
enum { CP_DIET_HERB = 0, CP_DIET_CARN = 1, CP_DIET_OMNI = 2 };
enum { CP_RUN = 0, CP_DEAD = 1, CP_EVOLVED = 2, CP_TIMEOUT = 3 };

/* ---- rng: xoshiro128**, lives inside the world so runs are reproducible ---- */
typedef struct { uint32_t s[4]; } CpRng;

void     cp_rng_seed(CpRng *r, uint32_t seed);
uint32_t cp_rng_u32(CpRng *r);
float    cp_rng_f(CpRng *r);                        /* [0,1)  */
float    cp_rng_range(CpRng *r, float a, float b);
int      cp_rng_int(CpRng *r, int n);               /* [0,n)  */

/* ---- morphology: the "creature editor" as an action space ----
 * The agent picks a body plan before the episode, then has to live with it.
 * Every part trades something away. */
typedef struct {
    uint8_t herb;   /* 0..2  filter mouths  - eat plants           */
    uint8_t carn;   /* 0..2  jaws           - eat meat, bite cells */
    uint8_t spike;  /* 0..4  spikes         - damage, no eating    */
    uint8_t cilia;  /* 0..4  cilia          - speed                */
    uint8_t flag;   /* 0..2  flagella       - burst speed          */
    uint8_t elec;   /* 0..2  electric       - ranged stun/damage   */
} CpMorph;

#define CP_MORPH_MAX_PARTS 8   /* budget: sum of all part counts */

typedef struct {
    float max_speed, accel, drag;
    float hp_max, attack, armor;
    float herb_eff, carn_eff;
    float radius0;
    float upkeep;               /* hp/sec cost of carrying the body plan */
} CpStats;

void cp_morph_default(CpMorph *m);
void cp_morph_random(CpMorph *m, CpRng *r);
void cp_morph_clamp(CpMorph *m);                     /* enforce part budget */
void cp_morph_stats(const CpMorph *m, CpStats *out);

/* ---- entities ---- */
/* Food is static once spawned: no velocity, no per-step integration. Keeping
 * this struct small matters - 640 of them are streamed every step. */
typedef struct {
    float   x, y, r;
    float   phase;              /* plant: anim offset. meat: seconds of life left */
    uint8_t type;               /* CP_FOOD_* */
    uint8_t pad[3];
} CpFood;

typedef struct {
    float   x, y, vx, vy;
    float   r, hp, hp_max;
    float   attack, armor, speed;
    float   heading, phase;
    float   wander_t, wander_a, wander_dx, wander_dy;
    float   des_x, des_y;       /* cached steering target, re-planned in bursts */
    uint8_t alive, diet, spikes, cilia;
    uint8_t has_target, pad[3];
    float   hue;
} CpCell;

/* ---- world ---- */
typedef struct {
    CpRng    rng;
    uint32_t seed;
    int32_t  step;

    CpCell   player;
    CpMorph  morph;
    CpStats  stats;
    float    dna;
    float    boost_cd;

    CpFood   food[CP_MAX_FOOD];
    int32_t  n_food;
    CpCell   cells[CP_MAX_CELLS];
    int32_t  n_cells;

    int32_t  food_cursor;
    int32_t  grid_head[CP_GRID_N];
    int32_t  grid_next[CP_MAX_FOOD];
    int32_t  cgrid_head[CP_CGRID_N];
    int32_t  cgrid_next[CP_MAX_CELLS];

    /* episode bookkeeping */
    float    reward;
    int32_t  status;            /* CP_RUN / CP_DEAD / ... */
    int32_t  ate_plant, ate_meat, kills, hits_taken;
    float    dist_travelled;
} CpWorld;

void  cp_world_reset(CpWorld *w, uint32_t seed, const CpMorph *morph);
void  cp_world_step(CpWorld *w, const float act[CP_ACT_DIM]);
void  cp_world_observe(const CpWorld *w, float *obs /* CP_OBS_DIM */);

/* scripted baseline: steer to the best nearby food, flee anything bigger. */
void  cp_policy_greedy(const CpWorld *w, float act[CP_ACT_DIM]);

/* ---- RL environment C ABI (what the python/ctypes binding talks to) ---- */
typedef struct CpEnv CpEnv;

CpEnv *cp_env_create(uint32_t seed);
void   cp_env_free(CpEnv *e);
void   cp_env_reset(CpEnv *e, uint32_t seed, const int32_t *morph6, float *obs);
void   cp_env_step(CpEnv *e, const float *act, float *obs,
                   float *reward, int32_t *terminated, int32_t *truncated);
int32_t cp_env_obs_dim(void);
int32_t cp_env_act_dim(void);

/* snapshot / restore - enables replay, mid-episode curricula, tree search */
size_t cp_env_state_size(void);
void   cp_env_save(const CpEnv *e, void *dst);
void   cp_env_load(CpEnv *e, const void *src);
const CpWorld *cp_env_world(const CpEnv *e);

/* ---- rendering (optional; pure function of world state) ---- */
void cp_render(const CpWorld *w, uint8_t *rgba, int width, int height);
int  cp_png_write(const char *path, const uint8_t *rgba, int width, int height);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_H */
