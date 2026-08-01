#include "cpore/aqua.h"
#include <stdlib.h>
#include <string.h>

/* Same flat C ABI as stage 1, so one ctypes binding covers both stages. */

struct Cp3Env { Cp3World w; };

Cp3Env *cp3_env_create(uint32_t seed)
{
    Cp3Env *e = (Cp3Env *)calloc(1, sizeof(Cp3Env));
    if (!e) return NULL;
    cp3_world_reset(&e->w, seed, NULL);
    return e;
}

void cp3_env_free(Cp3Env *e) { free(e); }

/* parts: CP3_MAX_PARTS quads of (type, seg, yaw, pitch), then nseg and girth */
void cp3_env_reset(Cp3Env *e, uint32_t seed, const int32_t *parts, float *obs)
{
    Cp3Genome g;
    if (parts) {
        cp3_genome_clear(&g);
        for (int i = 0; i < CP3_MAX_PARTS; i++) {
            const int32_t *q = parts + i * 4;
            int t = q[0];
            if (t < 0) t = 0;
            if (t >= CP3_PART_COUNT) t = CP3_PART_COUNT - 1;
            g.part[i].type  = (uint8_t)t;
            g.part[i].seg   = (uint8_t)(q[1] < 0 ? 0 : (q[1] >= CP3_MAX_SEG ? CP3_MAX_SEG - 1 : q[1]));
            g.part[i].yaw   = (uint8_t)(q[2] & 0xFF);
            g.part[i].pitch = (int8_t)(q[3] < -64 ? -64 : (q[3] > 63 ? 63 : q[3]));
        }
        int ns = parts[CP3_MAX_PARTS * 4];
        int gi = parts[CP3_MAX_PARTS * 4 + 1];
        g.nseg  = (uint8_t)(ns < 2 ? 2 : (ns > CP3_MAX_SEG ? CP3_MAX_SEG : ns));
        g.girth = (uint8_t)(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
        cp3_genome_normalise(&g, CP3_GEN_BUDGET[0]);
    } else {
        cp3_genome_starter(&g);
    }
    cp3_world_reset(&e->w, seed, &g);
    if (obs) cp3_world_observe(&e->w, obs);
}

void cp3_env_step(Cp3Env *e, const float *act, float *obs,
                  float *reward, int32_t *terminated, int32_t *truncated)
{
    cp3_world_step(&e->w, act);
    if (obs)    cp3_world_observe(&e->w, obs);
    if (reward) *reward = e->w.reward;
    if (terminated) *terminated = (e->w.status == CP3_DEAD || e->w.status == CP3_EVOLVED);
    if (truncated)  *truncated  = (e->w.status == CP3_TIMEOUT);
}

int32_t cp3_env_obs_dim(void) { return CP3_OBS_DIM; }
int32_t cp3_env_act_dim(void) { return CP3_ACT_DIM; }
size_t  cp3_env_state_size(void) { return sizeof(Cp3World); }
void    cp3_env_save(const Cp3Env *e, void *dst) { memcpy(dst, &e->w, sizeof(Cp3World)); }
void    cp3_env_load(Cp3Env *e, const void *src) { memcpy(&e->w, src, sizeof(Cp3World)); }
const Cp3World *cp3_env_world(const Cp3Env *e) { return &e->w; }

void cp3_env_census(const Cp3Env *e, int32_t *counts, float *means)
{
    if (counts) {
        counts[0] = e->w.births;
        counts[1] = e->w.deaths;
        counts[2] = e->w.pop;
    }
    if (means) {
        means[0] = e->w.mean_gen;
        means[1] = e->w.mean_parts;
        means[2] = e->w.mean_mouth;
        means[3] = e->w.mean_tail;
        means[4] = e->w.mean_light;
        means[5] = e->w.mean_depth;
    }
}
