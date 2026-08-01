#include "cpore/cpore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Throughput matters more than anything else here: the reason nobody trains
 * RL on real Spore is that it runs at 1x. Measure honestly. */

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void run(const char *label, int use_policy, int observe, int n_env, int steps)
{
    CpWorld *ws = (CpWorld *)malloc(sizeof(CpWorld) * (size_t)n_env);
    float *obs = (float *)malloc(sizeof(float) * CP_OBS_DIM);
    CpRng rng; cp_rng_seed(&rng, 99);

    for (int i = 0; i < n_env; i++) cp_world_reset(&ws[i], (uint32_t)(1000 + i), NULL);

    double t0 = now_s();
    long total = 0;
    for (int t = 0; t < steps; t++) {
        for (int i = 0; i < n_env; i++) {
            float act[CP_ACT_DIM];
            if (use_policy) {
                cp_policy_greedy(&ws[i], act);
            } else {
                act[0] = cp_rng_range(&rng, -1.0f, 1.0f);
                act[1] = cp_rng_range(&rng, -1.0f, 1.0f);
                act[2] = 0.0f;
            }
            cp_world_step(&ws[i], act);
            if (observe) cp_world_observe(&ws[i], obs);
            if (ws[i].status != CP_RUN) cp_world_reset(&ws[i], (uint32_t)(t * 31 + i), NULL);
            total++;
        }
    }
    double dt = now_s() - t0;
    printf("  %-28s %10.0f steps/s   (%ld steps in %.2fs)\n",
           label, total / dt, total, dt);

    free(ws); free(obs);
}

int main(int argc, char **argv)
{
    int n_env = 64, steps = 4000;
    if (argc > 1) n_env = atoi(argv[1]);
    if (argc > 2) steps = atoi(argv[2]);

    printf("cpore benchmark  (%d envs x %d steps, single thread)\n", n_env, steps);
    printf("  world state: %zu bytes/env   obs dim: %d   act dim: %d\n\n",
           sizeof(CpWorld), CP_OBS_DIM, CP_ACT_DIM);

    run("step only (random)",        0, 0, n_env, steps);
    run("step + observe (random)",   0, 1, n_env, steps);
    run("step + observe + baseline", 1, 1, n_env, steps);
    return 0;
}
