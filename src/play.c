/* Playing the cell stage, by hand.
 *
 * The same simulation the PufferLib environment steps and the same action
 * space it uses - nine directions, a boost and a discharge - because the point
 * of that action space was always that a key press and an action index are the
 * same number. A person driving this and a policy driving the environment are
 * playing one game, not two that resemble each other, and if they ever come
 * apart it will be visible here first.
 *
 * The ABI is flat on purpose: an opaque handle, ints, and one byte buffer. It
 * is the same shape as the creature editor's, so ctypes and WebAssembly can
 * both call it without a binding layer, and the browser build needs no runtime
 * beyond the two hundred lines in wasm/shim.c.
 */

#include "cpore/cpore.h"

#include <stdlib.h>
#include <string.h>

struct CpPlay {
    CpWorld  w;
    int      W, H;
    int      style;
    uint32_t seed;
    /* Held across steps so the HUD can report a run rather than a frame. */
    int32_t  best_dna;
};

typedef struct CpPlay CpPlay;

/* The nine directions, identical to the ones puffer/cell.h uses. Duplicated
 * rather than shared because the alternative is for the simulation to depend
 * on the environment, and the direction of that dependency is the one thing
 * this project has been careful about from the start. The test in
 * tests/test_core.c checks the two tables agree. */
static const float PLAY_MX[9] = { 0.0f,  0.0f,  0.7071f, 1.0f,  0.7071f,
                                  0.0f, -0.7071f, -1.0f, -0.7071f };
static const float PLAY_MY[9] = { 0.0f, -1.0f, -0.7071f, 0.0f,  0.7071f,
                                  1.0f,  0.7071f,  0.0f, -0.7071f };

CpPlay *cp_play_create(int32_t w, int32_t h, uint32_t seed)
{
    if (w < 64 || h < 64) return NULL;
    CpPlay *p = (CpPlay *)calloc(1, sizeof(CpPlay));
    if (!p) return NULL;
    p->W = w; p->H = h;
    p->seed = seed;
    p->style = CP_VIS_POND;
    cp_world_reset(&p->w, seed, NULL);
    return p;
}

void cp_play_free(CpPlay *p) { free(p); }

void cp_play_reset(CpPlay *p, uint32_t seed)
{
    if (!p) return;
    p->seed = seed;
    p->best_dna = 0;
    cp_world_reset(&p->w, seed, NULL);
}

void cp_play_style(CpPlay *p, int32_t style)
{
    if (!p) return;
    if (style < 0 || style >= CP_VIS_COUNT) style = CP_VIS_POND;
    p->style = style;
}

/* One frame. Returns the world status, so the page can tell "still going"
 * from "you evolved" without reading the stats block.
 *
 * Death respawns rather than ending, which is what the stage does when a
 * person is playing it: being eaten costs a chunk of the meter and puts you
 * back in the water. The environment makes the same choice for the same
 * reason. */
int32_t cp_play_step(CpPlay *p, int32_t move, int32_t boost, int32_t zap)
{
    if (!p) return CP_DEAD;
    if (move < 0 || move > 8) move = 0;

    float act[CP_ACT_DIM];
    memset(act, 0, sizeof(act));
    act[0] = PLAY_MX[move];
    act[1] = PLAY_MY[move];
    act[2] = boost ? 1.0f : 0.0f;
    act[3] = zap ? 1.0f : 0.0f;

    cp_world_step(&p->w, act);
    if ((int32_t)p->w.dna > p->best_dna) p->best_dna = (int32_t)p->w.dna;
    if (p->w.status == CP_DEAD) cp_world_respawn(&p->w);
    return p->w.status;
}

void cp_play_render(CpPlay *p, uint8_t *rgba)
{
    if (!p || !rgba) return;
    cp_render_styled(&p->w, rgba, p->W, p->H, p->style);
}

/* Everything a scoreboard could want, as one flat array so the caller needs no
 * struct layout knowledge. */
#define CP_PLAY_STATS 12
int32_t cp_play_stat_count(void) { return CP_PLAY_STATS; }

void cp_play_stats(const CpPlay *p, int32_t *out)
{
    if (!p || !out) return;
    const CpWorld *w = &p->w;
    out[0]  = (int32_t)w->dna;
    out[1]  = (int32_t)CP_DNA_GOAL;
    out[2]  = cp_world_tier(w);
    out[3]  = w->generation;
    out[4]  = (int32_t)(w->player.hp_max > 0.0f
                        ? w->player.hp / w->player.hp_max * 100.0f : 0.0f);
    out[5]  = w->stats.n_parts;
    out[6]  = w->ate_plant;
    out[7]  = w->ate_meat;
    out[8]  = w->kills;
    out[9]  = w->deaths;
    out[10] = w->step;
    out[11] = w->status;
}

const CpWorld *cp_play_world(const CpPlay *p) { return p ? &p->w : NULL; }
