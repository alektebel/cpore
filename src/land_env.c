#include "cpore/land.h"
#include <stdlib.h>
#include <string.h>

/* Same flat C ABI as stages 1 and 2, so one ctypes binding covers all three. */

struct Cp4Env { Cp4World w; };

Cp4Env *cp4_env_create(uint32_t seed)
{
    Cp4Env *e = (Cp4Env *)calloc(1, sizeof(Cp4Env));
    if (!e) return NULL;
    cp4_world_reset(&e->w, seed, NULL);
    return e;
}

void cp4_env_free(Cp4Env *e) { free(e); }

/* parts: CP4_MAX_PARTS sextuples of (type, seg, yaw, pitch, scale, mirror),
 * then nseg and girth. Wider than stage 2's quads because scale and bilateral
 * symmetry are genes here rather than constants. */
void cp4_env_reset(Cp4Env *e, uint32_t seed, const int32_t *parts, float *obs)
{
    Cp4Genome g;
    if (parts) {
        cp4_genome_clear(&g);
        for (int i = 0; i < CP4_MAX_PARTS; i++) {
            const int32_t *q = parts + i * 8;
            int t = q[0];
            if (t < 0) t = 0;
            if (t >= CP4_PART_COUNT) t = CP4_PART_COUNT - 1;
            g.part[i].type   = (uint8_t)t;
            g.part[i].seg    = (uint8_t)(q[1] < 0 ? 0 : (q[1] >= CP4_MAX_SEG ? CP4_MAX_SEG - 1 : q[1]));
            g.part[i].yaw    = (uint8_t)(q[2] & 0xFF);
            g.part[i].pitch  = (int8_t)(q[3] < -64 ? -64 : (q[3] > 63 ? 63 : q[3]));
            g.part[i].scale  = (uint8_t)(q[4] < 0 ? 0 : (q[4] > 255 ? 255 : q[4]));
            g.part[i].mirror = (uint8_t)(q[5] ? 1 : 0);
            /* Limb proportions, defaulted so a caller that only knows about
             * the first six fields still builds the animal it used to. */
            g.part[i].len    = (uint8_t)(q[6] <= 0 ? 128 : (q[6] > 255 ? 255 : q[6]));
            g.part[i].bend   = (int8_t)(q[7] < -127 ? -127 : (q[7] > 127 ? 127 : q[7]));
        }
        int ns = parts[CP4_MAX_PARTS * 8];
        int gi = parts[CP4_MAX_PARTS * 8 + 1];
        g.nseg  = (uint8_t)(ns < 2 ? 2 : (ns > CP4_MAX_SEG ? CP4_MAX_SEG : ns));
        g.girth = (uint8_t)(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
        cp4_genome_normalise(&g, CP4_GEN_BUDGET[0]);
    } else {
        cp4_genome_starter(&g);
    }
    cp4_world_reset(&e->w, seed, &g);
    if (obs) cp4_world_observe(&e->w, obs);
}

void cp4_env_step(Cp4Env *e, const float *act, float *obs,
                  float *reward, int32_t *terminated, int32_t *truncated)
{
    cp4_world_step(&e->w, act);
    if (obs)    cp4_world_observe(&e->w, obs);
    if (reward) *reward = e->w.reward;
    if (terminated) *terminated = (e->w.status == CP4_DEAD || e->w.status == CP4_EVOLVED);
    if (truncated)  *truncated  = (e->w.status == CP4_TIMEOUT);
}

int32_t cp4_env_obs_dim(void) { return CP4_OBS_DIM; }
int32_t cp4_env_act_dim(void) { return CP4_ACT_DIM; }
size_t  cp4_env_state_size(void) { return sizeof(Cp4World); }
void    cp4_env_save(const Cp4Env *e, void *dst) { memcpy(dst, &e->w, sizeof(Cp4World)); }
void    cp4_env_load(Cp4Env *e, const void *src) { memcpy(&e->w, src, sizeof(Cp4World)); }
const Cp4World *cp4_env_world(const Cp4Env *e) { return &e->w; }

/* Read the population out through C rather than letting Python guess at the
 * struct layout, which is how the stage-2 census came to report nonsense. */
void cp4_env_census(const Cp4Env *e, int32_t *counts, float *means)
{
    const Cp4World *w = &e->w;
    if (counts) {
        counts[0] = w->births;
        counts[1] = w->deaths;
        counts[2] = w->pop;
        counts[3] = w->allies;
        counts[4] = w->enemies;
        counts[5] = w->befriended;
        counts[6] = w->kills;
        counts[7] = w->discovered;
        counts[8] = w->hatchlings;
        counts[9] = w->home.alive;
        counts[10] = w->player.medium;
        for (int m = 0; m < CP4_MEDIUM_COUNT; m++) counts[11 + m] = w->medium_steps[m];
        counts[15] = w->ate_plant;
        counts[16] = w->ate_kelp;
        counts[17] = w->ate_tuber;
        counts[18] = w->ate_meat;
    }
    if (means) {
        means[0] = w->mean_gen;
        means[1] = w->mean_parts;
        means[2] = w->mean_legs;
        means[3] = w->mean_charm;
        means[4] = w->dna;
        means[5] = w->travelled;
        means[6] = w->far_from_start;
        means[7] = w->home.store;
    }
}
