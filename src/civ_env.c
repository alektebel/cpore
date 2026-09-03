#include "cpore/civ.h"
#include <stdlib.h>
#include <string.h>

/* Same flat C ABI as the other three stages, so one ctypes binding covers all
 * of them. */

struct Cp5Env { Cp5World w; };

Cp5Env *cp5_env_create(uint32_t seed)
{
    Cp5Env *e = (Cp5Env *)calloc(1, sizeof(Cp5Env));
    if (!e) return NULL;
    cp5_world_reset(&e->w, seed, NULL);
    return e;
}

void cp5_env_free(Cp5Env *e) { free(e); }

/* legacy: three multipliers (military, economic, religious), or NULL for a
 * civilisation that inherited nothing. This is the whole coupling between
 * stage 3 and stage 4 - deliberately three floats rather than a shared
 * struct, so neither stage can start reaching into the other. */
void cp5_env_reset(Cp5Env *e, uint32_t seed, const float *legacy, float *obs)
{
    Cp5Legacy lg;
    if (legacy) {
        for (int a = 0; a < CP5_APPROACH_COUNT; a++) {
            float v = legacy[a];
            lg.bonus[a] = v < 0.60f ? 0.60f : (v > 1.80f ? 1.80f : v);
        }
    } else {
        cp5_legacy_default(&lg);
    }
    cp5_world_reset(&e->w, seed, &lg);
    if (obs) cp5_world_observe(&e->w, obs);
}

void cp5_env_step(Cp5Env *e, const float *act, float *obs,
                  float *reward, int32_t *terminated, int32_t *truncated)
{
    cp5_world_step(&e->w, act);
    if (obs)    cp5_world_observe(&e->w, obs);
    if (reward) *reward = e->w.reward;
    if (terminated) *terminated = (e->w.status == CP5_LOST || e->w.status == CP5_WON);
    if (truncated)  *truncated  = (e->w.status == CP5_TIMEOUT);
}

int32_t cp5_env_obs_dim(void) { return CP5_OBS_DIM; }
int32_t cp5_env_act_dim(void) { return CP5_ACT_DIM; }
size_t  cp5_env_state_size(void) { return sizeof(Cp5World); }
void    cp5_env_save(const Cp5Env *e, void *dst) { memcpy(dst, &e->w, sizeof(Cp5World)); }
void    cp5_env_load(Cp5Env *e, const void *src) { memcpy(&e->w, src, sizeof(Cp5World)); }
const Cp5World *cp5_env_world(const Cp5Env *e) { return &e->w; }

void cp5_env_census(const Cp5Env *e, int32_t *counts, float *vals)
{
    const Cp5World *w = &e->w;
    if (counts) {
        counts[0] = w->nation[CP5_PLAYER].cities;
        counts[1] = w->captured;
        counts[2] = w->converted;
        counts[3] = w->bought;
        counts[4] = w->cities_lost;
        counts[5] = w->units_built[CP5_MIL];
        counts[6] = w->units_built[CP5_ECO];
        counts[7] = w->units_built[CP5_REL];
    }
    if (vals) {
        vals[0] = w->nation[CP5_PLAYER].money;
        vals[1] = w->nation[CP5_PLAYER].income;
        for (int a = 0; a < CP5_APPROACH_COUNT; a++)
            vals[2 + a] = w->nation[CP5_PLAYER].bonus[a];
    }
}

/* Bridge from a finished stage-3 world. It lives here rather than in
 * land_env.c so that stage 3 has no idea stage 4 exists - the dependency
 * points one way only, which is what lets either stage be trained on alone. */
void cp5_legacy_from_world(const Cp4World *land, float *out3)
{
    Cp5Legacy lg;
    if (!land || !out3) return;
    cp5_legacy_from_creature(&land->player.g, &lg);
    for (int a = 0; a < CP5_APPROACH_COUNT; a++) out3[a] = lg.bonus[a];
}

int32_t cp5_env_status(const Cp5Env *e) { return e ? e->w.status : CP5_RUN; }
