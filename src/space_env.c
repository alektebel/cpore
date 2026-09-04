#include "cpore/space.h"
#include "cpore/civ.h"
#include <stdlib.h>
#include <string.h>

/* Same flat C ABI as every other stage, so one ctypes binding covers all of
 * them. */

struct Cp7Env { Cp7World w; };

Cp7Env *cp7_env_create(uint32_t seed)
{
    Cp7Env *e = (Cp7Env *)calloc(1, sizeof(Cp7Env));
    if (!e) return NULL;
    cp7_world_reset(&e->w, seed, NULL);
    return e;
}

void cp7_env_free(Cp7Env *e) { free(e); }

/* legacy: three multipliers (colonise, trade, attack), or NULL for an empire
 * that inherited nothing. This is the whole coupling between stage 4 and
 * stage 6 - deliberately three floats, same as the civ and tribe bridges. */
void cp7_env_reset(Cp7Env *e, uint32_t seed, const float *legacy, float *obs)
{
    Cp7Legacy lg;
    if (legacy) {
        for (int b = 0; b < CP7_BONUS_COUNT; b++) {
            float v = legacy[b];
            lg.bonus[b] = v < 0.60f ? 0.60f : (v > 1.80f ? 1.80f : v);
        }
    } else {
        cp7_legacy_default(&lg);
    }
    cp7_world_reset(&e->w, seed, &lg);
    if (obs) cp7_world_observe(&e->w, obs);
}

void cp7_env_step(Cp7Env *e, const float *act, float *obs,
                  float *reward, int32_t *terminated, int32_t *truncated)
{
    cp7_world_step(&e->w, act);
    if (obs)    cp7_world_observe(&e->w, obs);
    if (reward) *reward = e->w.reward;
    if (terminated) *terminated = (e->w.status == CP7_LOST || e->w.status == CP7_WON);
    if (truncated)  *truncated  = (e->w.status == CP7_TIMEOUT);
}

int32_t cp7_env_obs_dim(void) { return CP7_OBS_DIM; }
int32_t cp7_env_act_dim(void) { return CP7_ACT_DIM; }
size_t  cp7_env_state_size(void) { return sizeof(Cp7World); }
void    cp7_env_save(const Cp7Env *e, void *dst) { memcpy(dst, &e->w, sizeof(Cp7World)); }
void    cp7_env_load(Cp7Env *e, const void *src) { memcpy(&e->w, src, sizeof(Cp7World)); }
const Cp7World *cp7_env_world(const Cp7Env *e) { return &e->w; }

void cp7_env_census(const Cp7Env *e, int32_t *counts, float *vals)
{
    const Cp7World *w = &e->w;
    if (counts) {
        counts[0] = w->empire[CP7_PLAYER].stars;
        counts[1] = w->settled;
        counts[2] = w->flipped;
        counts[3] = w->captured;
        counts[4] = w->lost_stars;
        counts[5] = w->trade_runs;
        counts[6] = w->raids_fought;
    }
    if (vals) {
        vals[0] = w->empire[CP7_PLAYER].money;
        vals[1] = w->empire[CP7_PLAYER].income;
        vals[2] = w->tax_earned;
        vals[3] = w->trade_earned;
        vals[4] = w->upgrades;
        for (int b = 0; b < CP7_BONUS_COUNT; b++) vals[5 + b] = w->bonus[b];
    }
}

int32_t cp7_env_status(const Cp7Env *e) { return e ? e->w.status : CP7_RUN; }

/* Bridge from a finished stage-4 world. It lives here rather than in civ_env.c
 * so that stage 4 has no idea stage 6 exists - the dependency points one way
 * only, which is what lets either stage be trained on alone. A nation that
 * won by force is a conquering power; by trade, a trading power; by faith, a
 * settling power. */
void cp7_legacy_from_civ(const Cp5World *civ, float *out3)
{
    if (!civ || !out3) return;
    out3[CP7_COLONISE] = civ->nation[CP5_PLAYER].bonus[CP5_REL];
    out3[CP7_TRADE]    = civ->nation[CP5_PLAYER].bonus[CP5_ECO];
    out3[CP7_ATTACK]   = civ->nation[CP5_PLAYER].bonus[CP5_MIL];
}
