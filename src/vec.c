/* Batch stepper: N worlds, one call, flat buffers.
 *
 * Each lane is an independent world with its own seed stream. The reset
 * decoders mirror cp_env_reset / cp3_env_reset / cp4_env_reset /
 * cp5_env_reset exactly (clamp, then normalise into the generation-0
 * budget), so a batch lane and a single env given the same seed and parts
 * produce bit-identical trajectories - there is a test for that. */
#include "cpore/vec.h"
#include <stdlib.h>
#include <string.h>

/* ================= cell ================= */

struct CpVec {
    CpWorld *w;
    int n;
    uint32_t cursor;
};

CpVec *cp_vec_create(int n, uint32_t seed)
{
    CpVec *v;
    if (n < 1) n = 1;
    v = (CpVec *)calloc(1, sizeof(CpVec));
    if (!v) return NULL;
    v->w = (CpWorld *)calloc((size_t)n, sizeof(CpWorld));
    if (!v->w) { free(v); return NULL; }
    v->n = n;
    v->cursor = seed;
    cp_vec_reset_all(v, seed);
    return v;
}

void cp_vec_free(CpVec *v) { if (v) { free(v->w); free(v); } }
int cp_vec_count(const CpVec *v) { return v ? v->n : 0; }

void cp_vec_reset(CpVec *v, int idx, uint32_t seed, const int32_t *parts)
{
    CpGenome g;
    if (!v || idx < 0 || idx >= v->n) return;
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
    cp_world_reset(&v->w[idx], seed, &g);
}

void cp_vec_reset_all(CpVec *v, uint32_t seed)
{
    if (!v) return;
    v->cursor = seed;
    for (int i = 0; i < v->n; i++)
        cp_vec_reset(v, i, v->cursor++, NULL);
}

void cp_vec_step(CpVec *v, const float *acts, float *obs,
                 float *rew, int32_t *term, int32_t *trunc, int autoreset)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        CpWorld *w = &v->w[i];
        cp_world_step(w, acts + (size_t)i * CP_ACT_DIM);
        if (obs) cp_world_observe(w, obs + (size_t)i * CP_OBS_DIM);
        if (rew) rew[i] = w->reward;
        {
            int te = (w->status == CP_DEAD || w->status == CP_EVOLVED);
            int tr = (w->status == CP_TIMEOUT);
            if (term) term[i] = te;
            if (trunc) trunc[i] = tr;
            if (autoreset && (te || tr)) {
                /* mirror cp_env_reset's starter path; the fresh obs replaces
                 * the terminal one, puffer-style */
                CpGenome g;
                cp_genome_starter(&g);
                cp_world_reset(w, v->cursor++, &g);
                if (obs) cp_world_observe(w, obs + (size_t)i * CP_OBS_DIM);
            }
        }
    }
}

void cp_vec_greedy(CpVec *v, float *acts)
{
    if (!v || !acts) return;
    for (int i = 0; i < v->n; i++)
        cp_policy_greedy(&v->w[i], acts + (size_t)i * CP_ACT_DIM);
}

size_t cp_vec_state_size(int n) { return sizeof(CpVec) + sizeof(CpWorld) * (size_t)(n < 1 ? 1 : n); }

void cp_vec_save(const CpVec *v, void *dst)
{
    char *p;
    if (!v || !dst) return;
    p = (char *)dst;
    memcpy(p, v, sizeof(CpVec));
    p += sizeof(CpVec);
    memcpy(p, v->w, sizeof(CpWorld) * (size_t)v->n);
}

void cp_vec_load(CpVec *v, const void *src)
{
    const char *p;
    CpWorld *w;
    int n;
    if (!v || !src) return;
    p = (const char *)src;
    n = ((const CpVec *)p)->n;
    if (n != v->n) return;               /* refuse to load across shapes */
    v->cursor = ((const CpVec *)p)->cursor;
    p += sizeof(CpVec);
    w = v->w;
    memcpy(w, p, sizeof(CpWorld) * (size_t)v->n);
}

/* ================= aquatic ================= */

struct Cp3Batch {
    Cp3World *w;
    int n;
    uint32_t cursor;
};

Cp3Batch *cp3_vec_create(int n, uint32_t seed)
{
    Cp3Batch *v;
    if (n < 1) n = 1;
    v = (Cp3Batch *)calloc(1, sizeof(Cp3Batch));
    if (!v) return NULL;
    v->w = (Cp3World *)calloc((size_t)n, sizeof(Cp3World));
    if (!v->w) { free(v); return NULL; }
    v->n = n;
    v->cursor = seed;
    cp3_vec_reset_all(v, seed);
    return v;
}

void cp3_vec_free(Cp3Batch *v) { if (v) { free(v->w); free(v); } }
int cp3_vec_count(const Cp3Batch *v) { return v ? v->n : 0; }

void cp3_vec_reset(Cp3Batch *v, int idx, uint32_t seed, const int32_t *parts)
{
    Cp3Genome g;
    if (!v || idx < 0 || idx >= v->n) return;
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
        {
            int ns = parts[CP3_MAX_PARTS * 4];
            int gi = parts[CP3_MAX_PARTS * 4 + 1];
            g.nseg  = (uint8_t)(ns < 2 ? 2 : (ns > CP3_MAX_SEG ? CP3_MAX_SEG : ns));
            g.girth = (uint8_t)(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
        }
        cp3_genome_normalise(&g, CP3_GEN_BUDGET[0]);
    } else {
        cp3_genome_starter(&g);
    }
    cp3_world_reset(&v->w[idx], seed, &g);
}

void cp3_vec_reset_all(Cp3Batch *v, uint32_t seed)
{
    if (!v) return;
    v->cursor = seed;
    for (int i = 0; i < v->n; i++)
        cp3_vec_reset(v, i, v->cursor++, NULL);
}

void cp3_vec_step(Cp3Batch *v, const float *acts, float *obs,
                  float *rew, int32_t *term, int32_t *trunc, int autoreset)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        Cp3World *w = &v->w[i];
        cp3_world_step(w, acts + (size_t)i * CP3_ACT_DIM);
        if (obs) cp3_world_observe(w, obs + (size_t)i * CP3_OBS_DIM);
        if (rew) rew[i] = w->reward;
        {
            int te = (w->status == CP3_DEAD || w->status == CP3_EVOLVED);
            int tr = (w->status == CP3_TIMEOUT);
            if (term) term[i] = te;
            if (trunc) trunc[i] = tr;
            if (autoreset && (te || tr)) {
                Cp3Genome g;
                cp3_genome_starter(&g);
                cp3_world_reset(w, v->cursor++, &g);
                if (obs) cp3_world_observe(w, obs + (size_t)i * CP3_OBS_DIM);
            }
        }
    }
}

void cp3_vec_greedy(Cp3Batch *v, float *acts)
{
    if (!v || !acts) return;
    for (int i = 0; i < v->n; i++)
        cp3_policy_greedy(&v->w[i], acts + (size_t)i * CP3_ACT_DIM);
}

/* ================= land ================= */

struct Cp4VecBatch {
    Cp4World *w;
    int n;
    uint32_t cursor;
};

Cp4VecBatch *cp4_vec_create(int n, uint32_t seed)
{
    Cp4VecBatch *v;
    if (n < 1) n = 1;
    v = (Cp4VecBatch *)calloc(1, sizeof(Cp4VecBatch));
    if (!v) return NULL;
    v->w = (Cp4World *)calloc((size_t)n, sizeof(Cp4World));
    if (!v->w) { free(v); return NULL; }
    v->n = n;
    v->cursor = seed;
    cp4_vec_reset_all(v, seed);
    return v;
}

void cp4_vec_free(Cp4VecBatch *v) { if (v) { free(v->w); free(v); } }
int cp4_vec_count(const Cp4VecBatch *v) { return v ? v->n : 0; }

void cp4_vec_reset(Cp4VecBatch *v, int idx, uint32_t seed, const int32_t *parts)
{
    Cp4Genome g;
    if (!v || idx < 0 || idx >= v->n) return;
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
            g.part[i].len    = (uint8_t)(q[6] <= 0 ? 128 : (q[6] > 255 ? 255 : q[6]));
            g.part[i].bend   = (int8_t)(q[7] < -127 ? -127 : (q[7] > 127 ? 127 : q[7]));
        }
        {
            int ns = parts[CP4_MAX_PARTS * 8];
            int gi = parts[CP4_MAX_PARTS * 8 + 1];
            g.nseg  = (uint8_t)(ns < 2 ? 2 : (ns > CP4_MAX_SEG ? CP4_MAX_SEG : ns));
            g.girth = (uint8_t)(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
        }
        cp4_genome_normalise(&g, CP4_GEN_BUDGET[0]);
    } else {
        cp4_genome_starter(&g);
    }
    cp4_world_reset(&v->w[idx], seed, &g);
}

void cp4_vec_reset_all(Cp4VecBatch *v, uint32_t seed)
{
    if (!v) return;
    v->cursor = seed;
    for (int i = 0; i < v->n; i++)
        cp4_vec_reset(v, i, v->cursor++, NULL);
}

void cp4_vec_step(Cp4VecBatch *v, const float *acts, float *obs,
                  float *rew, int32_t *term, int32_t *trunc, int autoreset)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        Cp4World *w = &v->w[i];
        cp4_world_step(w, acts + (size_t)i * CP4_ACT_DIM);
        if (obs) cp4_world_observe(w, obs + (size_t)i * CP4_OBS_DIM);
        if (rew) rew[i] = w->reward;
        {
            int te = (w->status == CP4_DEAD || w->status == CP4_EVOLVED);
            int tr = (w->status == CP4_TIMEOUT);
            if (term) term[i] = te;
            if (trunc) trunc[i] = tr;
            if (autoreset && (te || tr)) {
                Cp4Genome g;
                cp4_genome_starter(&g);
                cp4_world_reset(w, v->cursor++, &g);
                if (obs) cp4_world_observe(w, obs + (size_t)i * CP4_OBS_DIM);
            }
        }
    }
}

void cp4_vec_greedy(Cp4VecBatch *v, float *acts)
{
    if (!v || !acts) return;
    for (int i = 0; i < v->n; i++)
        cp4_policy_greedy(&v->w[i], acts + (size_t)i * CP4_ACT_DIM);
}

void cp4_vec_census(const Cp4VecBatch *v, int32_t *counts, float *means)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        const Cp4World *w = &v->w[i];
        int32_t *c = counts ? counts + (size_t)i * 19 : NULL;
        float *m = means ? means + (size_t)i * 8 : NULL;
        if (c) {
            c[0] = w->births; c[1] = w->deaths; c[2] = w->pop;
            c[3] = w->allies; c[4] = w->enemies; c[5] = w->befriended;
            c[6] = w->kills; c[7] = w->discovered; c[8] = w->hatchlings;
            c[9] = w->home.alive; c[10] = w->player.medium;
            for (int k = 0; k < CP4_MEDIUM_COUNT; k++) c[11 + k] = w->medium_steps[k];
            c[15] = w->ate_plant; c[16] = w->ate_kelp;
            c[17] = w->ate_tuber; c[18] = w->ate_meat;
        }
        if (m) {
            m[0] = w->mean_gen; m[1] = w->mean_parts; m[2] = w->mean_legs;
            m[3] = w->mean_charm; m[4] = w->dna; m[5] = w->travelled;
            m[6] = w->far_from_start; m[7] = w->home.store;
        }
    }
}

/* ================= civ ================= */

struct Cp5Batch {
    Cp5World *w;
    int n;
    uint32_t cursor;
};

Cp5Batch *cp5_vec_create(int n, uint32_t seed)
{
    Cp5Batch *v;
    if (n < 1) n = 1;
    v = (Cp5Batch *)calloc(1, sizeof(Cp5Batch));
    if (!v) return NULL;
    v->w = (Cp5World *)calloc((size_t)n, sizeof(Cp5World));
    if (!v->w) { free(v); return NULL; }
    v->n = n;
    v->cursor = seed;
    cp5_vec_reset_all(v, seed);
    return v;
}

void cp5_vec_free(Cp5Batch *v) { if (v) { free(v->w); free(v); } }
int cp5_vec_count(const Cp5Batch *v) { return v ? v->n : 0; }

void cp5_vec_reset(Cp5Batch *v, int idx, uint32_t seed, const float *legacy)
{
    Cp5Legacy lg;
    if (!v || idx < 0 || idx >= v->n) return;
    if (legacy) {
        for (int a = 0; a < CP5_APPROACH_COUNT; a++) {
            float f = legacy[a];
            lg.bonus[a] = f < 0.60f ? 0.60f : (f > 1.80f ? 1.80f : f);
        }
    } else {
        cp5_legacy_default(&lg);
    }
    cp5_world_reset(&v->w[idx], seed, &lg);
}

void cp5_vec_reset_all(Cp5Batch *v, uint32_t seed)
{
    if (!v) return;
    v->cursor = seed;
    for (int i = 0; i < v->n; i++)
        cp5_vec_reset(v, i, v->cursor++, NULL);
}

void cp5_vec_step(Cp5Batch *v, const float *acts, float *obs,
                  float *rew, int32_t *term, int32_t *trunc, int autoreset)
{
    if (!v) return;
    for (int i = 0; i < v->n; i++) {
        Cp5World *w = &v->w[i];
        cp5_world_step(w, acts + (size_t)i * CP5_ACT_DIM);
        if (obs) cp5_world_observe(w, obs + (size_t)i * CP5_OBS_DIM);
        if (rew) rew[i] = w->reward;
        {
            int te = (w->status == CP5_LOST || w->status == CP5_WON);
            int tr = (w->status == CP5_TIMEOUT);
            if (term) term[i] = te;
            if (trunc) trunc[i] = tr;
            if (autoreset && (te || tr)) {
                cp5_world_reset(w, v->cursor++, NULL);
                if (obs) cp5_world_observe(w, obs + (size_t)i * CP5_OBS_DIM);
            }
        }
    }
}

void cp5_vec_greedy(Cp5Batch *v, float *acts)
{
    if (!v || !acts) return;
    for (int i = 0; i < v->n; i++)
        cp5_policy_greedy(&v->w[i], acts + (size_t)i * CP5_ACT_DIM);
}

/* ================= tribe ================= */

struct Cp6Batch {
    Cp6World *w;
    int n;
    uint32_t cursor;
};

Cp6Batch *cp6_vec_create(int n, uint32_t seed)
{
    Cp6Batch *v;
    if (n < 1) n = 1;
    v = (Cp6Batch *)calloc(1, sizeof(Cp6Batch));
    if (!v) return NULL;
    v->w = (Cp6World *)calloc((size_t)n, sizeof(Cp6World));
    if (!v->w) { free(v); return NULL; }
    v->n = n;
    v->cursor = seed;
    cp6_vec_reset_all(v, seed);
    return v;
}

void cp6_vec_free(Cp6Batch *v) { if (v) { free(v->w); free(v); } }
int cp6_vec_count(const Cp6Batch *v) { return v ? v->n : 0; }

void cp6_vec_reset(Cp6Batch *v, int idx, uint32_t seed, const int32_t *parts)
{
    /* Batch lanes start from the default founder; a designed founder is the
     * single-env path (cp6_env_reset / cp6_world_reset with imports). */
    (void)parts;
    if (!v || idx < 0 || idx >= v->n) return;
    cp6_world_reset(&v->w[idx], seed, NULL, NULL, 0);
}

void cp6_vec_reset_all(Cp6Batch *v, uint32_t seed)
{
    int i;
    if (!v) return;
    v->cursor = seed;
    for (i = 0; i < v->n; i++)
        cp6_vec_reset(v, i, v->cursor++, NULL);
}

void cp6_vec_step(Cp6Batch *v, const float *acts, float *obs,
                  float *rew, int32_t *term, int32_t *trunc, int autoreset)
{
    int i;
    if (!v) return;
    for (i = 0; i < v->n; i++) {
        Cp6World *w = &v->w[i];
        cp6_world_step(w, acts + (size_t)i * CP6_ACT_DIM);
        if (obs) cp6_world_observe(w, obs + (size_t)i * CP6_OBS_DIM);
        if (rew) rew[i] = w->reward;
        {
            int te = (w->status == CP6_LOST || w->status == CP6_WON);
            int tr = (w->status == CP6_TIMEOUT);
            if (term) term[i] = te;
            if (trunc) trunc[i] = tr;
            if (autoreset && (te || tr)) {
                cp6_world_reset(w, v->cursor++, NULL, NULL, 0);
                if (obs) cp6_world_observe(w, obs + (size_t)i * CP6_OBS_DIM);
            }
        }
    }
}

void cp6_vec_greedy(Cp6Batch *v, float *acts)
{
    int i;
    if (!v || !acts) return;
    for (i = 0; i < v->n; i++)
        cp6_policy_greedy(&v->w[i], acts + (size_t)i * CP6_ACT_DIM);
}

/* ---- observe-out (fills obs without stepping; used after reset_all) ---- */
void cp_vec_observe(const CpVec *v, float *obs)
{
    int i;
    if (!v || !obs) return;
    for (i = 0; i < v->n; i++)
        cp_world_observe(&v->w[i], obs + (size_t)i * CP_OBS_DIM);
}
void cp3_vec_observe(const Cp3Batch *v, float *obs)
{
    int i;
    if (!v || !obs) return;
    for (i = 0; i < v->n; i++)
        cp3_world_observe(&v->w[i], obs + (size_t)i * CP3_OBS_DIM);
}
void cp4_vec_observe(const Cp4VecBatch *v, float *obs)
{
    int i;
    if (!v || !obs) return;
    for (i = 0; i < v->n; i++)
        cp4_world_observe(&v->w[i], obs + (size_t)i * CP4_OBS_DIM);
}
void cp5_vec_observe(const Cp5Batch *v, float *obs)
{
    int i;
    if (!v || !obs) return;
    for (i = 0; i < v->n; i++)
        cp5_world_observe(&v->w[i], obs + (size_t)i * CP5_OBS_DIM);
}
void cp6_vec_observe(const Cp6Batch *v, float *obs)
{
    int i;
    if (!v || !obs) return;
    for (i = 0; i < v->n; i++)
        cp6_world_observe(&v->w[i], obs + (size_t)i * CP6_OBS_DIM);
}

/* ================= space ================= */

struct Cp7Batch {
    Cp7World *w;
    int n;
    uint32_t cursor;
};

Cp7Batch *cp7_vec_create(int n, uint32_t seed)
{
    Cp7Batch *v;
    if (n < 1) n = 1;
    v = (Cp7Batch *)calloc(1, sizeof(Cp7Batch));
    if (!v) return NULL;
    v->w = (Cp7World *)calloc((size_t)n, sizeof(Cp7World));
    if (!v->w) { free(v); return NULL; }
    v->n = n;
    v->cursor = seed;
    cp7_vec_reset_all(v, seed);
    return v;
}

void cp7_vec_free(Cp7Batch *v) { if (v) { free(v->w); free(v); } }
int cp7_vec_count(const Cp7Batch *v) { return v ? v->n : 0; }

void cp7_vec_reset(Cp7Batch *v, int idx, uint32_t seed, const float *legacy)
{
    /* Batch lanes start from an empire that inherited nothing; a legacy is
     * the single-env path (cp7_env_reset / cp7_world_reset with a legacy). */
    (void)legacy;
    if (!v || idx < 0 || idx >= v->n) return;
    cp7_world_reset(&v->w[idx], seed, NULL);
}

void cp7_vec_reset_all(Cp7Batch *v, uint32_t seed)
{
    int i;
    if (!v) return;
    v->cursor = seed;
    for (i = 0; i < v->n; i++)
        cp7_vec_reset(v, i, v->cursor++, NULL);
}

void cp7_vec_step(Cp7Batch *v, const float *acts, float *obs,
                  float *rew, int32_t *term, int32_t *trunc, int autoreset)
{
    int i;
    if (!v) return;
    for (i = 0; i < v->n; i++) {
        Cp7World *w = &v->w[i];
        cp7_world_step(w, acts + (size_t)i * CP7_ACT_DIM);
        if (obs) cp7_world_observe(w, obs + (size_t)i * CP7_OBS_DIM);
        if (rew) rew[i] = w->reward;
        {
            int te = (w->status == CP7_LOST || w->status == CP7_WON);
            int tr = (w->status == CP7_TIMEOUT);
            if (term) term[i] = te;
            if (trunc) trunc[i] = tr;
            if (autoreset && (te || tr)) {
                cp7_world_reset(w, v->cursor++, NULL);
                if (obs) cp7_world_observe(w, obs + (size_t)i * CP7_OBS_DIM);
            }
        }
    }
}

void cp7_vec_greedy(Cp7Batch *v, float *acts)
{
    int i;
    if (!v || !acts) return;
    for (i = 0; i < v->n; i++)
        cp7_policy_greedy(&v->w[i], acts + (size_t)i * CP7_ACT_DIM);
}

void cp7_vec_observe(const Cp7Batch *v, float *obs)
{
    int i;
    if (!v || !obs) return;
    for (i = 0; i < v->n; i++)
        cp7_world_observe(&v->w[i], obs + (size_t)i * CP7_OBS_DIM);
}
