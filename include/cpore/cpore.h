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
#define CP_MAX_STEPS   9000

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

#define CP_DNA_GOAL    100.0f
#define CP_GENERATIONS 4          /* redesigns at 25/50/75 DNA, then evolve */

/* ------------------------------------------------------------------ *
 * Parts - the cell stage roster.
 *
 * Placement is part of the design, not decoration: a spike only hurts what
 * it is pointing at, a jet only helps if it is mounted behind you, and an
 * eye you did not buy is a cell you cannot see.
 * ------------------------------------------------------------------ */
enum {
    CP_PART_NONE = 0,
    CP_PART_FILTER,      /* filter mouth - plants                       */
    CP_PART_JAW,         /* jaw          - meat, and bites other cells  */
    CP_PART_PROBOSCIS,   /* proboscis    - both, less efficiently       */
    CP_PART_CILIA,       /* cilia        - sustained speed              */
    CP_PART_FLAGELLA,    /* flagella     - acceleration and burst       */
    CP_PART_JET,         /* jet          - top speed, direction matters */
    CP_PART_SPIKE,       /* spike        - directional contact damage   */
    CP_PART_ELECTRIC,    /* electric     - radial discharge, on trigger */
    CP_PART_POISON,      /* poison       - hurts whatever bites you     */
    CP_PART_EYE,         /* eye          - perception radius            */
    CP_PART_COUNT
};

#define CP_MAX_PARTS   12

/* DNA points available to spend, per generation */
extern const int CP_GEN_BUDGET[CP_GENERATIONS];

const char *cp_part_name(int type);
int         cp_part_cost(int type);

typedef struct {
    uint8_t type;        /* CP_PART_*                       */
    uint8_t angle;       /* 0..255 mapped onto 0..2pi, body-relative */
} CpPart;

typedef struct {
    CpPart part[CP_MAX_PARTS];
} CpGenome;

/* observation shape */
#define CP_OBS_FOOD_K  8
#define CP_OBS_CELL_K  6
#define CP_OBS_DIM     (13 + CP_OBS_FOOD_K * 4 + CP_OBS_CELL_K * 6 + (CP_PART_COUNT - 1) + 6)

/* action shape: steering x/y, flagella burst, electric discharge, then a
 * design head of (type, angle) per slot. The design head is read only at a
 * generation boundary and ignored on every other step, which keeps this a
 * plain Box action space that ordinary PPO can drive. */
#define CP_ACT_CTRL    4
#define CP_ACT_DIM     (CP_ACT_CTRL + CP_MAX_PARTS * 2)

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

/* ---- derived stats ---- */
typedef struct {
    float max_speed, accel, drag;
    float hp_max, armor;
    float herb_eff, carn_eff;
    float radius0, upkeep;

    float spike_dmg, jaw_dmg;      /* per part, applied through a facing arc */
    float elec_dmg, elec_radius, elec_cost, elec_cd;
    float poison_dmg;
    float jet_thrust;              /* bonus scale, only for rear-facing jets */
    float percep;                  /* how far the observation reaches        */

    uint8_t n[CP_PART_COUNT];      /* part counts by type */
    uint8_t n_parts;
    int16_t cost;
} CpStats;

void cp_genome_clear(CpGenome *g);
void cp_genome_starter(CpGenome *g);
int  cp_genome_cost(const CpGenome *g);
/* drop parts that overrun the budget, guarantee a mouth, compact slots */
void cp_genome_normalise(CpGenome *g, int budget);
void cp_genome_random(CpGenome *g, CpRng *r, int budget);
void cp_genome_stats(const CpGenome *g, CpStats *out);
/* decode the design head of an action vector (CP_MAX_PARTS*2 floats in [-1,1]) */
void cp_genome_from_action(CpGenome *g, const float *design, int budget);
/* scripted designer, used by the baseline and as the default at generation
 * boundaries when a policy leaves the design head at zero */
void cp_genome_autodesign(CpGenome *g, CpRng *r, int budget, int style);
#define CP_STYLE_GRAZER 0
#define CP_STYLE_HUNTER 1
#define CP_STYLE_TANK   2
#define CP_STYLE_SCOUT  3
#define CP_STYLE_COUNT  4

/* ---- entities ---- */
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
    float   poison;             /* retaliation damage this cell deals back */
    uint8_t alive, diet, spikes, cilia;
    uint8_t has_target, jaws, eyes, pad;
    float   hue;
} CpCell;

/* ---- world ---- */
typedef struct {
    CpRng    rng;
    uint32_t seed;
    int32_t  step;

    CpCell   player;
    CpGenome genome;
    CpStats  stats;
    float    dna;
    int32_t  generation;
    float    boost_cd, elec_cd, elec_flash;

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
    int32_t  ate_plant, ate_meat, kills, hits_taken, discharges;
    float    dmg_dealt, dmg_taken;   /* higher-resolution than kill counts */
    int32_t  design_events;     /* how many times the design head was applied */
    float    dist_travelled;
} CpWorld;

void  cp_world_reset(CpWorld *w, uint32_t seed, const CpGenome *genome);
void  cp_world_step(CpWorld *w, const float act[CP_ACT_DIM]);
void  cp_world_observe(const CpWorld *w, float *obs /* CP_OBS_DIM */);
/* Live redesign at the current generation's budget (the game editor's entry
 * point): rebuild toward a scripted style, or apply a decoded share-code
 * genome. Same refresh the generation boundary runs, minus the reward. */
void  cp_world_redesign(CpWorld *w, int style);
void  cp_world_apply_genome(CpWorld *w, const CpGenome *g);

/* scripted baseline: fills the whole action vector, design head included. */
void  cp_policy_greedy(const CpWorld *w, float act[CP_ACT_DIM]);

/* ---- RL environment C ABI (what the python/ctypes binding talks to) ---- */
typedef struct CpEnv CpEnv;

CpEnv *cp_env_create(uint32_t seed);
void   cp_env_free(CpEnv *e);
/* parts: CP_MAX_PARTS*2 int32 pairs (type, angle 0..255), or NULL for the
 * starter cell. */
void   cp_env_reset(CpEnv *e, uint32_t seed, const int32_t *parts, float *obs);
void   cp_env_step(CpEnv *e, const float *act, float *obs,
                   float *reward, int32_t *terminated, int32_t *truncated);
int32_t cp_env_obs_dim(void);
int32_t cp_env_act_dim(void);
int32_t cp_env_max_parts(void);
int32_t cp_env_part_count(void);

/* snapshot / restore - enables replay, mid-episode curricula, tree search */
size_t cp_env_state_size(void);
void   cp_env_save(const CpEnv *e, void *dst);
void   cp_env_load(CpEnv *e, const void *src);
const CpWorld *cp_env_world(const CpEnv *e);
/* read back the live genome as CP_MAX_PARTS pairs of (type, angle) */
void   cp_env_genome(const CpEnv *e, int32_t *out);
int32_t cp_env_generation(const CpEnv *e);
/* editor from any binding: rebuild toward a style, apply a pasted share
 * code (0 ok), or read the live genome as a share string (0 ok). */
void    cp_env_redesign(CpEnv *e, int style);
int32_t cp_env_apply_code(CpEnv *e, const char *code);
int32_t cp_env_share_code(const CpEnv *e, char *out, size_t cap);
int32_t cp_env_status(const CpEnv *e);

/* ---- rendering (optional; pure function of world state) ---- */

/* Visual styles. Each moves internal resolution, palette, camera scale, dither
 * strength and outline treatment together - they are not palette swaps. */
enum { CP_VIS_ABYSS = 0, CP_VIS_DMG, CP_VIS_NEON, CP_VIS_PETRI, CP_VIS_C64,
       CP_VIS_TERRA, CP_VIS_COUNT };
const char *cp_vis_name(int style);

void cp_render(const CpWorld *w, uint8_t *rgba, int width, int height);
void cp_render_styled(const CpWorld *w, uint8_t *rgba, int width, int height,
                      int style);

/* Shared pixel pipeline, so stage 2 draws through exactly the same palette,
 * dither and upscale rather than growing a second look. */
void cp_vis_dims(int style, int32_t *w, int32_t *h);
void cp_vis_quantise(uint8_t *fb, int32_t w, int32_t h, int32_t style);
/* The style's palette and dither spread, for a caller that has to do the same
 * quantisation somewhere this code cannot reach - a GPU shader, say. Copies
 * 3 bytes per entry and returns the count, so the two paths cannot drift onto
 * different colours. */
int32_t cp_vis_palette(int32_t style, uint8_t *rgb, int32_t max, float *dither);
void cp_vis_blit(const uint8_t *low, int32_t lw, int32_t lh,
                 uint8_t *out, int32_t W, int32_t H, int32_t style);
void cp_px_rect(uint8_t *fb, int32_t W, int32_t H, int32_t x, int32_t y,
                int32_t w, int32_t h, float r, float g, float b, float a);
void cp_px_text(uint8_t *fb, int32_t W, int32_t H, int32_t x, int32_t y,
                int32_t sc, const char *s, float r, float g, float b, float a);
int32_t cp_px_text_w(const char *s, int32_t sc);
int  cp_png_write(const char *path, const uint8_t *rgba, int width, int height);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_H */
