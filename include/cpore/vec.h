/* cpore vector core - the pufferlib path.
 *
 * The python/ctypes binding loops over envs in Python: one FFI call per env
 * per step. That is the slow path on purpose. This file is the fast path: N
 * worlds laid out contiguously in one allocation, stepped with one call that
 * writes flat obs/reward/terminal buffers the caller owns.
 *
 * The caller owns the float buffers (typically numpy arrays, so C writes
 * straight into them with no copy). Done envs auto-reset inside the step with
 * an incrementing seed, puffer-style, so the batch never stalls.
 *
 * Still dependency-free C99, still no I/O, no threads here - sharding across
 * cores is the caller's job (one vec per thread). */
#ifndef CPORE_VEC_H
#define CPORE_VEC_H

#include <stdint.h>
#include <stddef.h>
#include "cpore/cpore.h"
#include "cpore/aqua.h"
#include "cpore/land.h"
#include "cpore/civ.h"
#include "cpore/tribe.h"
#include "cpore/space.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- cell stage batch ---- */
typedef struct CpVec CpVec;

CpVec  *cp_vec_create(int n, uint32_t seed);
void    cp_vec_free(CpVec *v);
int     cp_vec_count(const CpVec *v);
/* reset one lane (parts: CP_MAX_PARTS pairs, or NULL for starter) */
void    cp_vec_reset(CpVec *v, int idx, uint32_t seed, const int32_t *parts);
void    cp_vec_reset_all(CpVec *v, uint32_t seed);
/* acts [n*CP_ACT_DIM] -> obs [n*CP_OBS_DIM], rew/term/trunc [n].
 * autoreset: a lane that terminates is immediately reset (seed cursor
 * advances) and its obs slot holds the FRESH observation. */
void    cp_vec_step(CpVec *v, const float *acts, float *obs,
                    float *rew, int32_t *term, int32_t *trunc, int autoreset);
void    cp_vec_observe(const CpVec *v, float *obs);
/* fill acts with the scripted baseline (throughput / sanity baseline) */
void    cp_vec_greedy(CpVec *v, float *acts);
size_t  cp_vec_state_size(int n);
void    cp_vec_save(const CpVec *v, void *dst);
void    cp_vec_load(CpVec *v, const void *src);

/* ---- aquatic batch ---- */
typedef struct Cp3Batch Cp3Batch;

Cp3Batch *cp3_vec_create(int n, uint32_t seed);
void    cp3_vec_free(Cp3Batch *v);
int     cp3_vec_count(const Cp3Batch *v);
void    cp3_vec_reset(Cp3Batch *v, int idx, uint32_t seed, const int32_t *parts);
void    cp3_vec_reset_all(Cp3Batch *v, uint32_t seed);
void    cp3_vec_step(Cp3Batch *v, const float *acts, float *obs,
                     float *rew, int32_t *term, int32_t *trunc, int autoreset);
void    cp3_vec_observe(const Cp3Batch *v, float *obs);
void    cp3_vec_greedy(Cp3Batch *v, float *acts);

/* ---- creature (land) batch - the main training stage ---- */
typedef struct Cp4VecBatch Cp4VecBatch;

Cp4VecBatch *cp4_vec_create(int n, uint32_t seed);
void         cp4_vec_free(Cp4VecBatch *v);
int          cp4_vec_count(const Cp4VecBatch *v);
void         cp4_vec_reset(Cp4VecBatch *v, int idx, uint32_t seed,
                           const int32_t *parts);
void         cp4_vec_reset_all(Cp4VecBatch *v, uint32_t seed);
void         cp4_vec_step(Cp4VecBatch *v, const float *acts, float *obs,
                          float *rew, int32_t *term, int32_t *trunc,
                          int autoreset);
void         cp4_vec_observe(const Cp4VecBatch *v, float *obs);
void         cp4_vec_greedy(Cp4VecBatch *v, float *acts);
/* per-lane census without breaking the batch: counts [n*19], means [n*8]
 * in the same layout as cp4_env_census. */
void         cp4_vec_census(const Cp4VecBatch *v, int32_t *counts, float *means);

/* ---- civilisation batch ---- */
typedef struct Cp5Batch Cp5Batch;

Cp5Batch *cp5_vec_create(int n, uint32_t seed);
void    cp5_vec_free(Cp5Batch *v);
int     cp5_vec_count(const Cp5Batch *v);
void    cp5_vec_reset(Cp5Batch *v, int idx, uint32_t seed, const float *legacy);
void    cp5_vec_reset_all(Cp5Batch *v, uint32_t seed);
void    cp5_vec_step(Cp5Batch *v, const float *acts, float *obs,
                     float *rew, int32_t *term, int32_t *trunc, int autoreset);
void    cp5_vec_observe(const Cp5Batch *v, float *obs);
void    cp5_vec_greedy(Cp5Batch *v, float *acts);

/* ---- tribe batch ---- */
typedef struct Cp6Batch Cp6Batch;

Cp6Batch *cp6_vec_create(int n, uint32_t seed);
void      cp6_vec_free(Cp6Batch *v);
int       cp6_vec_count(const Cp6Batch *v);
void      cp6_vec_reset(Cp6Batch *v, int idx, uint32_t seed,
                        const int32_t *parts);
void      cp6_vec_reset_all(Cp6Batch *v, uint32_t seed);
void      cp6_vec_step(Cp6Batch *v, const float *acts, float *obs,
                       float *rew, int32_t *term, int32_t *trunc,
                       int autoreset);
void      cp6_vec_observe(const Cp6Batch *v, float *obs);
void      cp6_vec_greedy(Cp6Batch *v, float *acts);

/* ---- space batch ---- */
typedef struct Cp7Batch Cp7Batch;

Cp7Batch *cp7_vec_create(int n, uint32_t seed);
void      cp7_vec_free(Cp7Batch *v);
int       cp7_vec_count(const Cp7Batch *v);
void      cp7_vec_reset(Cp7Batch *v, int idx, uint32_t seed,
                        const float *legacy);
void      cp7_vec_reset_all(Cp7Batch *v, uint32_t seed);
void      cp7_vec_step(Cp7Batch *v, const float *acts, float *obs,
                       float *rew, int32_t *term, int32_t *trunc,
                       int autoreset);
void      cp7_vec_observe(const Cp7Batch *v, float *obs);
void      cp7_vec_greedy(Cp7Batch *v, float *acts);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_VEC_H */
