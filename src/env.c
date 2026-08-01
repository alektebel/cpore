#include "cpore/cpore.h"
#include <stdlib.h>
#include <string.h>

/* Thin RL wrapper over CpWorld. Deliberately a flat C ABI with no callbacks
 * and no ownership subtleties, so ctypes / cffi / a Rust FFI shim can all
 * bind it without a build step. */

struct CpEnv {
    CpWorld  w;
    uint32_t next_seed;
};

CpEnv *cp_env_create(uint32_t seed)
{
    CpEnv *e = (CpEnv *)calloc(1, sizeof(CpEnv));
    if (!e) return NULL;
    e->next_seed = seed;
    cp_world_reset(&e->w, seed, NULL);
    return e;
}

void cp_env_free(CpEnv *e) { free(e); }

void cp_env_reset(CpEnv *e, uint32_t seed, const int32_t *morph6, float *obs)
{
    CpMorph m;
    if (morph6) {
        m.herb  = (uint8_t)(morph6[0] < 0 ? 0 : morph6[0]);
        m.carn  = (uint8_t)(morph6[1] < 0 ? 0 : morph6[1]);
        m.spike = (uint8_t)(morph6[2] < 0 ? 0 : morph6[2]);
        m.cilia = (uint8_t)(morph6[3] < 0 ? 0 : morph6[3]);
        m.flag  = (uint8_t)(morph6[4] < 0 ? 0 : morph6[4]);
        m.elec  = (uint8_t)(morph6[5] < 0 ? 0 : morph6[5]);
    } else {
        cp_morph_default(&m);
    }
    e->next_seed = seed;
    cp_world_reset(&e->w, seed, &m);
    if (obs) cp_world_observe(&e->w, obs);
}

void cp_env_step(CpEnv *e, const float *act, float *obs,
                 float *reward, int32_t *terminated, int32_t *truncated)
{
    cp_world_step(&e->w, act);
    if (obs)    cp_world_observe(&e->w, obs);
    if (reward) *reward = e->w.reward;
    if (terminated) *terminated = (e->w.status == CP_DEAD || e->w.status == CP_EVOLVED);
    if (truncated)  *truncated  = (e->w.status == CP_TIMEOUT);
}

int32_t cp_env_obs_dim(void) { return CP_OBS_DIM; }
int32_t cp_env_act_dim(void) { return CP_ACT_DIM; }

size_t cp_env_state_size(void) { return sizeof(CpWorld); }

void cp_env_save(const CpEnv *e, void *dst)      { memcpy(dst, &e->w, sizeof(CpWorld)); }
void cp_env_load(CpEnv *e, const void *src)      { memcpy(&e->w, src, sizeof(CpWorld)); }
const CpWorld *cp_env_world(const CpEnv *e)      { return &e->w; }
