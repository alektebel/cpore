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
#define CP4_MAX_FLORA   560   /* must exceed TARGET_FLORA, plus carcasses */
#define CP4_MAX_NESTS      7
#define CP4_MAX_PARTS     16
#define CP4_MAX_SEG        6
#define CP4_FCELL      150.0f   /* flora hash cell, a little over a sight step */
#define CP4_FGRID      2048     /* buckets; must be a power of two */

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
    CP4_WING,       /* wing           - lift, and the whole sky with it  */
    CP4_FIN,        /* fin/paddle     - thrust in water                  */
    CP4_GILL,       /* gill           - breathe water instead of drowning*/
    CP4_DIGGER,     /* digging claw   - burrow, and reach what is buried */
    CP4_ARM,        /* grasping arm   - reach, browse height, and provisioning*/
    CP4_TAIL,       /* tail           - a counterweight, and something to wave*/
    CP4_PART_COUNT
};

/* ---- media ----
 * The stage has four of them and a body plan decides which are open to it.
 * This is the axis the whole extension turns on: each medium has its own
 * physics, its own food, and its own part gating it, so wings and gills are
 * choices bought out of the DNA budget rather than decoration. */
enum { CP4_ON_GROUND = 0, CP4_IN_WATER, CP4_IN_AIR, CP4_UNDER, CP4_MEDIUM_COUNT };
const char *cp4_medium_name(int m);

extern const int CP4_GEN_BUDGET[CP4_GENERATIONS];
const char *cp4_part_name(int type);
int         cp4_part_cost(int type);

/* Where a part sits and what shape it is.
 *
 * type/seg/yaw/pitch/scale/mirror is where to stick it. `len` and `bend` are
 * what it is: a limb with a reach and a joint. Without them every leg on every
 * animal in the world was the same leg at a different scale, which is the
 * difference between a parts bin and a creature editor - in Spore the thing
 * you spend the most time on is dragging a limb out to the length you want and
 * setting how it folds. */
typedef struct {
    uint8_t type;
    uint8_t seg;
    uint8_t yaw;
    int8_t  pitch;
    uint8_t scale;
    uint8_t mirror;
    uint8_t len;     /* reach of a jointed limb, or elongation of a spike  */
    int8_t  bend;    /* how far the joint folds, and which way             */
} Cp4Part;

enum { CP4_PAT_PLAIN = 0, CP4_PAT_BANDS, CP4_PAT_SPOTS, CP4_PAT_COUNTER,
       CP4_PAT_STRIPES, CP4_PAT_MOTTLE, CP4_PAT_GRADIENT, CP4_PAT_RINGS,
       CP4_PAT_COUNT };

typedef struct {
    Cp4Part part[CP4_MAX_PARTS];
    uint8_t nseg, girth;
    uint8_t prof[4];
    int8_t  lump[CP4_MAX_SEG];
    /* Per-segment height, so the spine can rise and dip along its length
     * rather than only arching as one curve. A hump, a dropped neck and a
     * raised tail root are all the same three bytes. */
    int8_t  rise[CP4_MAX_SEG];
    int8_t  arch, sweep;
    /* Three coats, as in Spore: a base, a marking and a detail over the top,
     * each with its own pattern. Two colours and one pattern gave a space
     * where every animal was a body and one stripe. */
    uint8_t hue, hue2, hue3, sat, val;
    uint8_t pattern, pscale;
    uint8_t pattern2, pscale2;
} Cp4Genome;

typedef struct {
    float speed, accel, turn, jump, grip;
    float hp_max, armor, bite, claw_dmg;
    float graze_eff, carn_eff;
    float sight, hearing;
    float charm, social_reach;     /* the other half of the stage */
    /* What arms and tails buy. reach lengthens every contact the animal makes,
     * armed or social; carry is how fast it can provision a nest, which is the
     * only thing that turns food into descendants. */
    float reach, carry;
    float stamina, upkeep;
    float upkeep_leg;              /* what standing tall costs, kept separate */
    float radius, length, stand;   /* stand: how high the body rides */
    /* ---- the media ----
     * swim/fly/dig are zero for a body that cannot use that medium at all,
     * which is what makes them gates rather than modifiers. breath is how many
     * seconds underwater the animal has before it starts drowning; gills make
     * it effectively infinite. */
    float swim, buoy, fly, dig, breath;
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
void  cp4_genome_colour(const Cp4Genome *g, float *rgb, float *rgb2, float *rgb3);
/* Six archetypes, not three. The first three decide how you fill the DNA
 * meter; the last three decide which medium you live in. They are orthogonal
 * on purpose - a burrowing charmer is a legal and quite good animal. */
#define CP4_STYLE_GRAZER    0
#define CP4_STYLE_PREDATOR  1
#define CP4_STYLE_CHARMER   2
#define CP4_STYLE_SWIMMER   3
#define CP4_STYLE_FLYER     4
#define CP4_STYLE_BURROWER  5
#define CP4_STYLE_COUNT     6
const char *cp4_style_name(int style);

/* ---- time of day ----
 * A full cycle every CP4_DAY steps, so an episode covers four and a half of
 * them. Night is not decoration: sight falls away and hearing does not, which
 * is what finally makes an ear worth its DNA against an eye. */
#define CP4_DAY 2000

float cp4_daylight(int32_t step);      /* 0 at midnight, 1 at noon */
float cp4_sun_angle(int32_t step);     /* where the sun is, in radians */

/* ---- climate ----
 * Two more pure functions of position, on top of the height field. An
 * unbounded world is only worth walking across if what is over there differs
 * from what is here, and temperature and moisture are the cheapest pair that
 * produces regions rather than noise: they decide the ground colour, what
 * grows in it and how much of it grows.
 *
 * Both come back in 0..1. Temperature falls with altitude as well as varying
 * geographically, so a mountain is cold wherever it stands. */
enum { CP4_BIOME_ICE = 0, CP4_BIOME_TUNDRA, CP4_BIOME_TAIGA, CP4_BIOME_FOREST,
       CP4_BIOME_GRASS, CP4_BIOME_SAVANNA, CP4_BIOME_DESERT, CP4_BIOME_JUNGLE,
       CP4_BIOME_COUNT };

void  cp4_climate(uint32_t seed, float x, float z, float *temp, float *moist);
int   cp4_biome(uint32_t seed, float x, float z);
const char *cp4_biome_name(int b);
/* how much grows here, 0..1 - a desert is not merely a different colour */
float cp4_fertility(int biome);

/* ---- terrain ----
 * A deterministic function of world seed and position, so it needs no storage
 * and the same seed always grows the same hills. */
float cp4_height(uint32_t seed, float x, float z);
void  cp4_normal(uint32_t seed, float x, float z, float *nx, float *ny, float *nz);

/* Where the snow starts. Shared rather than repeated, because the renderer,
 * the map and the biome colours all have to agree about it or a peak is white
 * from one direction and green from another. */
#define CP4_SNOWLINE 150.0f

/* Ground, and the surface of whatever water is standing on it.
 *
 * The world has rivers as well as a sea, so "is this water" stopped being a
 * comparison against one global sea level. Both come out of the same
 * evaluation - the channel is cut by the height field itself, so the water
 * that fills it is known at the same moment the bed is - and the pair is what
 * every other test is written against: there is water here exactly when the
 * ground lies below the waterline, and the difference is how deep it is.
 *
 * y grows downward, so the waterline is never greater than the ground. */
float cp4_height_water(uint32_t seed, float x, float z, float *water_y);

/* The surface a ray actually meets: the waterline where there is water and
 * the ground where there is not. This is what the renderer marches. */
float cp4_surface(uint32_t seed, float x, float z);

typedef struct { float x, y, z; } Cp4Vec;

/* One food per medium, so a medium is somewhere worth going rather than
 * somewhere you merely can go. Air is the exception - what the sky pays is
 * speed, safety and the range to find nests, not a plant. */
enum { CP4_FLORA_NONE = 0, CP4_FLORA_BUSH = 1, CP4_FLORA_CARCASS = 2,
       CP4_FLORA_KELP = 3, CP4_FLORA_TUBER = 4, CP4_FLORA_COUNT };
int cp4_flora_medium(int type);

typedef struct {
    Cp4Vec  p;
    float   r, regrow;
    uint8_t type;
    uint8_t pad[3];
} Cp4Flora;

/* A rival species: a nest, a lineage, how it feels about the player, and what
 * it has learned about them.
 *
 * The memory is the point. Selection across generations is slow and already
 * works; what was missing was a species reacting inside a single episode. A
 * lineage you keep attacking gets warier and starts running sooner. One you
 * keep attacking from behind starts facing you. One you keep singing the same
 * song at stops being impressed. All three decay, so it is a mood rather than
 * a permanent state, and all three are visible in the observation so an agent
 * can actually respond to them. */
typedef struct {
    Cp4Vec    p;
    Cp4Genome g;
    float     standing;      /* -1 hostile .. +1 allied */
    float     wary;          /* 0..1 learned fear of the player          */
    float     fatigue;       /* 0..1 how tired they are of your display  */
    float     guard;         /* 0..1 how much they turn to face you      */
    int32_t   members, befriended, eaten;
    int32_t   attacks_seen, songs_heard;
    uint8_t   alive, style, seen, pad;
} Cp4Nest;

/* nest index CP4_OWN_NEST marks one of the player's own hatchlings rather than
 * a member of a rival species */
#define CP4_OWN_NEST 255

typedef struct {
    Cp4Vec    p, v;
    float     yaw, pitch, phase;
    float     hp, hp_max, energy, stam;
    Cp4Genome g;
    Cp4Stats  s;
    Cp4Vec    des;
    float     think_t, sing_t, atk_cd;
    float     breath;
    uint8_t   alive, nest, has_target, grounded;
    uint8_t   gen, medium, want_med, pad;
    float     age;
} Cp4Beast;

/* The player's own nest. Somewhere to bank food, heal, and hatch followers -
 * the one piece of the world the agent builds rather than finds. */
typedef struct {
    Cp4Vec   p;
    float    store, build;
    int32_t  eggs, hatched;
    uint8_t  alive, pad[3];
} Cp4Home;

#define CP4_OBS_FLORA_K 8
#define CP4_OBS_BEAST_K 6
#define CP4_OBS_FLORA   7     /* dx, dz, dy, and one flag per food type */
/* 18 body/world, then 14 for medium and home: four one-hot media, three
 * capability gates, breath, height over ground, and four for the nest. Then 3
 * for the climate and the hour, neighbours, own parts, own stats. Miscounting
 * overflows the caller's observation buffer, which is what the exact-count
 * test exists to catch. */
#define CP4_OBS_BEAST   9     /* ...and the last is what its species has learned */
#define CP4_OBS_DIM   (18 + 14 + 3 + CP4_OBS_FLORA_K * CP4_OBS_FLORA \
                          + CP4_OBS_BEAST_K * CP4_OBS_BEAST \
                          + (CP4_PART_COUNT - 1) + 7)
/* turn, pitch, move, ascend/jump, attack, sing, dig, nest */
#define CP4_ACT_CTRL   8
/* Six numbers a part: what, where on the spine, which way round, how far
 * over, how big, how long. The design head is only read at a generation
 * boundary, so its width costs the policy nothing per step. */
#define CP4_ACT_PART   6
#define CP4_ACT_DIM   (CP4_ACT_CTRL + CP4_MAX_PARTS * CP4_ACT_PART + 2)

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
    /* Spatial hash over the flora, as linked lists into flora[].
     *
     * Without it the NPC feeding pass is beasts x flora - sixty-four animals
     * against five hundred plants every step - which is what held the stage to
     * a tenth of stage 1's throughput. The world has no bounds, so the grid is
     * a hash of the cell coordinates rather than an array indexed by them. */
    int16_t   fgrid[CP4_FGRID];
    int16_t   fnext[CP4_MAX_FLORA];
    int32_t   last_tuber;    /* roots grow in patches, so they seed off each other */
    Cp4Beast  beast[CP4_MAX_BEASTS];
    Cp4Nest   nest[CP4_MAX_NESTS];
    Cp4Home   home;

    /* The world is unbounded: terrain is a pure function, so the only thing
     * that has to be finite is what is resident near the player. anchor is the
     * centre of the current resident window and travelled is how far the
     * player has actually gone, which is what "explore" is scored on. */
    Cp4Vec    anchor;
    float     travelled, far_from_start;
    int32_t   discovered;
    float     daylight;              /* cached from step, for renderer and obs */
    int32_t   night_steps;

    int32_t   births, deaths, pop;
    float     mean_parts, mean_legs, mean_charm, mean_gen;
    int32_t   allies, enemies;

    float     reward;
    int32_t   status;
    int32_t   ate_plant, ate_meat, kills, hits_taken, songs, befriended;
    int32_t   ate_kelp, ate_tuber, eggs_laid, hatchlings;
    int32_t   medium_steps[CP4_MEDIUM_COUNT];
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
/* counts[19]: births, deaths, pop, allies, enemies, befriended, kills,
 *             discovered, hatchlings, home, medium, steps in each of the four
 *             media, then bush/kelp/tuber/meat eaten
 * means[8]:   gen, parts, legs, charm, dna, travelled, furthest, nest store */
void cp4_env_census(const Cp4Env *e, int32_t *counts, float *means);

void cp4_render(const Cp4World *w, uint8_t *rgba, int width, int height);
/* Stage 3's continuous-tone renderer: a cached heightfield marched into a
 * linear HDR buffer at the output resolution, with the atmosphere doing the
 * drawing. Reachable through cp4_render_styled with CP_VIS_VISTA. */
void cp4_render_vista(const Cp4World *w, uint8_t *rgba, int width, int height);
void cp4_render_styled(const Cp4World *w, uint8_t *rgba, int width, int height,
                       int style);
void cp4_render_portrait(const Cp4Genome *g, uint8_t *fb, int lw, int lh,
                         int style, uint32_t seed);

/* ---- the creature editor's viewport ----
 *
 * One animal, a backdrop and a floor, rendered through the same continuous-
 * tone path the world uses - so what you design is what you get, which is the
 * one thing an editor viewport has to promise.
 *
 * A studio holds its buffers across frames because an editor redraws
 * constantly and an editor that reallocates a megabyte per frame is an editor
 * that stutters. `quality` picks an internal resolution and a marcher budget
 * together: draw at 0 or 1 while the mouse is moving and at 3 when it stops,
 * and the coarse frames are fast enough to drag against while the settled one
 * is supersampled.
 */
typedef struct Cp4Studio Cp4Studio;

typedef struct {
    float azimuth;   /* turntable, radians                            */
    float elev;      /* camera rise, radians                          */
    float zoom;      /* 1 frames the whole animal; larger moves in    */
    float phase;     /* gait phase, so the pose can breathe           */
    int   quality;   /* 0 coarse .. 3 settled and supersampled        */
} Cp4View;

Cp4Studio *cp4_studio_new(int width, int height);
void       cp4_studio_free(Cp4Studio *s);
void       cp4_studio_size(const Cp4Studio *s, int32_t *w, int32_t *h);
void       cp4_studio_render(Cp4Studio *s, const Cp4Genome *g,
                             const Cp4View *v, uint8_t *rgba);
/* Which genome slot is under a pixel, or -1 for the backdrop or the trunk.
 * The same march the renderer runs, so the answer can never disagree with
 * what is on screen - which is the usual failure of a separate picking
 * representation, and the one a user notices immediately. */
int        cp4_studio_pick(Cp4Studio *s, const Cp4Genome *g,
                           const Cp4View *v, int px, int py);
/* The same, from a chosen angle. azimuth turns around the animal, elev raises
 * the camera; both in radians. A creature editor that can only show you one
 * angle is not showing you the creature - the whole reason a part carries a
 * yaw gene is that it looks different from the front than from the side. */
void cp4_render_pose(const Cp4Genome *g, uint8_t *fb, int lw, int lh,
                     int style, uint32_t seed, float azimuth, float elev);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_LAND_H */
