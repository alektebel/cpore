/* The cell stage as a PufferLib environment.
 *
 * PufferLib's contract is four functions and a struct: c_reset, c_step,
 * c_render, c_close, over an Env whose first fields are a Log and pointers
 * into numpy arrays that Python owns. Everything below that line is cpore's
 * own simulation, unchanged and shared - the same CpWorld the ctypes
 * environment steps and the same one both renderers draw. There is one
 * simulation in this repository and this file is a second doorway into it,
 * not a second copy of it.
 *
 * Three things are decided here rather than in the simulation, because they
 * are about the shape of the learning problem rather than about the biology:
 *
 *  - The action space. cp_world_step takes a 28-float vector with a design
 *    head on the end; a policy learns far better from a small MultiDiscrete,
 *    and a human plays far better from one, so this translates. The important
 *    property is that the translation is the same in both directions: what a
 *    key press means and what an action index means are the same thing, so
 *    the agent and the person are playing the same game rather than two games
 *    that resemble each other.
 *
 *  - Death. Being eaten in a cell stage is a setback, not an ending. The
 *    episode runs until the meter fills or the clock runs out, and dying in
 *    between costs a chunk of the meter and puts you back in the water. That
 *    is both the faithful reading and, as it happens, the better learning
 *    signal: an episode that ends on the first mistake teaches an agent to
 *    hide.
 *
 *  - The editor. cp_world_step opens it whenever the meter crosses a segment
 *    and reads the design head at exactly that moment. Since the design head
 *    is not in the MultiDiscrete, it is left null and the simulation's own
 *    designer fills it - which is the behaviour a policy that has not been
 *    given a design head should get. Handing the editor to the policy is a
 *    separate action space and a separate decision.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "cpore/cpore.h"

/* Only floats, and n last: PufferLib averages a Log by walking it as an array
 * of floats and dividing by the final field. */
typedef struct {
    float score;         /* undiscounted return over the episode          */
    float dna;           /* how full the meter got, 0..1                  */
    float tier;          /* size ladder reached, 0..2                     */
    float generation;    /* how many times the editor opened              */
    float plants;
    float meat;
    float kills;
    float deaths;
    float evolved;       /* 1 if the stage was completed                  */
    float length;
    float n;             /* required last field                           */
} Log;

typedef struct {
    Log            log;              /* required first */
    float         *observations;     /* CP_OBS_DIM floats  */
    int           *actions;          /* 3 ints: move, boost, discharge */
    float         *rewards;
    unsigned char *terminals;

    CpWorld  world;
    uint32_t seed;
    int      episode_len;            /* steps before truncation */
    int      respawn;                /* 1: death costs the meter. 0: death ends it */
    float    ep_return;
} CellEnv;

/* The nine ways to push.
 *
 * Index 0 is "let go", which is a real choice in water - you keep drifting -
 * and the eight after it are the compass, so a key press and an action index
 * are the same thing. Diagonals are normalised here rather than in the
 * simulation, so that holding two keys is not secretly 41% faster.
 */
static const float MOVE_X[9] = { 0.0f,  0.0f,  0.7071f, 1.0f,  0.7071f,
                                 0.0f, -0.7071f, -1.0f, -0.7071f };
static const float MOVE_Y[9] = { 0.0f, -1.0f, -0.7071f, 0.0f,  0.7071f,
                                 1.0f,  0.7071f,  0.0f, -0.7071f };

void c_reset(CellEnv *env)
{
    if (env->episode_len <= 0) env->episode_len = CP_MAX_STEPS;
    cp_world_reset(&env->world, env->seed, NULL);
    env->seed = env->seed * 1664525u + 1013904223u;   /* a new pond next time */
    env->ep_return = 0.0f;
    cp_world_observe(&env->world, env->observations);
    env->terminals[0] = 0;
    env->rewards[0] = 0.0f;
}

static void c_finish(CellEnv *env, float evolved)
{
    CpWorld *w = &env->world;
    env->log.score      += env->ep_return;
    env->log.dna        += w->dna / CP_DNA_GOAL;
    env->log.tier       += (float)cp_world_tier(w);
    env->log.generation += (float)w->generation;
    env->log.plants     += (float)w->ate_plant;
    env->log.meat       += (float)w->ate_meat;
    env->log.kills      += (float)w->kills;
    env->log.deaths     += (float)w->deaths;
    env->log.evolved    += evolved;
    env->log.length     += (float)w->step;
    env->log.n          += 1.0f;
}

void c_step(CellEnv *env)
{
    CpWorld *w = &env->world;

    /* The design head stays zero, which is the signal cp_world_step reads as
     * "no policy is driving the editor" - it then designs the next body with
     * its own scripted designer. */
    float act[CP_ACT_DIM];
    memset(act, 0, sizeof(act));

    int mv = env->actions[0];
    if (mv < 0 || mv > 8) mv = 0;
    act[0] = MOVE_X[mv];
    act[1] = MOVE_Y[mv];
    act[2] = env->actions[1] ? 1.0f : 0.0f;
    act[3] = env->actions[2] ? 1.0f : 0.0f;

    cp_world_step(w, act);
    float reward = w->reward;

    int done = 0, evolved = 0;
    if (w->status == CP_DEAD) {
        if (env->respawn) {
            /* The -5 cp_world_step charges for dying stands; what does not is
             * the episode ending on it. Reward already carries the cost, so
             * the respawn adds nothing beyond putting the cell back. */
            cp_world_respawn(w);
        } else {
            done = 1;
        }
    } else if (w->status == CP_EVOLVED) {
        done = 1;
        evolved = 1;
    } else if (w->status == CP_TIMEOUT || w->step >= env->episode_len) {
        done = 1;
    }

    env->ep_return += reward;
    env->rewards[0] = reward;
    env->terminals[0] = (unsigned char)done;

    if (done) {
        c_finish(env, (float)evolved);
        c_reset(env);
    } else {
        cp_world_observe(w, env->observations);
    }
}

/* No window from here.
 *
 * cpore draws with its own renderer into a buffer of bytes and has done since
 * before this file existed, so the frame a caller wants already exists and
 * wants a surface to land on rather than a second renderer. Python asks for it
 * through the binding's `render` method and puts it wherever it likes - a
 * window, a PNG, a browser. Linking a windowing library in here would add a
 * dependency to the one part of the project that has never had one. */
void c_render(CellEnv *env) { (void)env; }
void c_close(CellEnv *env)  { (void)env; }
