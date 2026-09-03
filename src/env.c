#include "cpore/cpore.h"
#include "cpore/codec.h"
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

/* parts: CP_MAX_PARTS pairs of (type, angle). Anything out of range is
 * clamped rather than rejected, and cp_genome_normalise then enforces the
 * generation-0 budget - a caller cannot smuggle in a build it cannot pay for. */
void cp_env_reset(CpEnv *e, uint32_t seed, const int32_t *parts, float *obs)
{
    CpGenome g;
    if (parts) {
        cp_genome_clear(&g);
        for (int i = 0; i < CP_MAX_PARTS; i++) {
            int t = parts[i * 2], a = parts[i * 2 + 1];
            if (t < 0) t = 0;
            if (t >= CP_PART_COUNT) t = CP_PART_COUNT - 1;
            g.part[i].type = (uint8_t)t;
            g.part[i].angle = (uint8_t)(a & 0xFF);
        }
        cp_genome_normalise(&g, CP_GEN_BUDGET[0]);
    } else {
        cp_genome_starter(&g);
    }
    e->next_seed = seed;
    cp_world_reset(&e->w, seed, &g);
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

int32_t cp_env_obs_dim(void)    { return CP_OBS_DIM; }
int32_t cp_env_act_dim(void)    { return CP_ACT_DIM; }
int32_t cp_env_max_parts(void)  { return CP_MAX_PARTS; }
int32_t cp_env_part_count(void) { return CP_PART_COUNT; }

size_t cp_env_state_size(void) { return sizeof(CpWorld); }

void cp_env_save(const CpEnv *e, void *dst)      { memcpy(dst, &e->w, sizeof(CpWorld)); }
void cp_env_load(CpEnv *e, const void *src)      { memcpy(&e->w, src, sizeof(CpWorld)); }
const CpWorld *cp_env_world(const CpEnv *e)      { return &e->w; }

void cp_env_genome(const CpEnv *e, int32_t *out)
{
    for (int i = 0; i < CP_MAX_PARTS; i++) {
        out[i * 2]     = e->w.genome.part[i].type;
        out[i * 2 + 1] = e->w.genome.part[i].angle;
    }
}

int32_t cp_env_generation(const CpEnv *e) { return e->w.generation; }

/* Editor entry points at env level (game + share codes from any binding). */
void cp_env_redesign(CpEnv *e, int style)
{
    if (e) cp_world_redesign(&e->w, style);
}

int32_t cp_env_apply_code(CpEnv *e, const char *code)
{
    CpGenome g;
    if (!e || !code) return -1;
    if (cp_decode_cell(code, &g)) return -1;
    cp_world_apply_genome(&e->w, &g);
    return 0;
}

int32_t cp_env_share_code(const CpEnv *e, char *out, size_t cap)
{
    if (!e || !out) return -1;
    return cp_codec_cell(&e->w.genome, out, cap) > 0 ? 0 : -1;
}

int32_t cp_env_status(const CpEnv *e) { return e ? e->w.status : CP_RUN; }
