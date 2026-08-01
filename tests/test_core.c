#include "cpore/cpore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(cond, msg) do {                                    \
        if (!(cond)) { printf("  FAIL  %s\n", msg); fails++; }   \
        else         { printf("  ok    %s\n", msg); }            \
    } while (0)

/* fold a trajectory into one number so divergence anywhere shows up */
static uint64_t rollout_hash(uint32_t seed, const CpMorph *m, int steps)
{
    CpWorld *w = (CpWorld *)malloc(sizeof(CpWorld));
    float obs[CP_OBS_DIM];
    uint64_t h = 1469598103934665603ull;

    cp_world_reset(w, seed, m);
    for (int t = 0; t < steps && w->status == CP_RUN; t++) {
        float act[CP_ACT_DIM];
        cp_policy_greedy(w, act);
        cp_world_step(w, act);
        cp_world_observe(w, obs);
        for (int i = 0; i < CP_OBS_DIM; i++) {
            uint32_t bits;
            memcpy(&bits, &obs[i], 4);
            h = (h ^ bits) * 1099511628211ull;
        }
    }
    free(w);
    return h;
}

int main(void)
{
    printf("cpore core tests\n");

    /* --- determinism: same seed, same future --- */
    CHECK(rollout_hash(42, NULL, 700) == rollout_hash(42, NULL, 700),
          "identical seeds produce identical trajectories");
    CHECK(rollout_hash(42, NULL, 700) != rollout_hash(43, NULL, 700),
          "different seeds diverge");

    /* --- snapshot / restore round-trips the whole world --- */
    {
        CpEnv *e = cp_env_create(11);
        float obs[CP_OBS_DIM], r;
        int32_t term, trunc;
        cp_env_reset(e, 11, NULL, obs);

        for (int t = 0; t < 200; t++) {
            float a[CP_ACT_DIM];
            cp_policy_greedy(cp_env_world(e), a);
            cp_env_step(e, a, obs, &r, &term, &trunc);
        }

        void *snap = malloc(cp_env_state_size());
        cp_env_save(e, snap);

        float branch_a[CP_OBS_DIM];
        float ret_a = 0.0f;
        for (int t = 0; t < 150; t++) {
            float a[CP_ACT_DIM];
            cp_policy_greedy(cp_env_world(e), a);
            cp_env_step(e, a, branch_a, &r, &term, &trunc);
            ret_a += r;
        }

        cp_env_load(e, snap);
        float branch_b[CP_OBS_DIM];
        float ret_b = 0.0f;
        for (int t = 0; t < 150; t++) {
            float a[CP_ACT_DIM];
            cp_policy_greedy(cp_env_world(e), a);
            cp_env_step(e, a, branch_b, &r, &term, &trunc);
            ret_b += r;
        }

        CHECK(memcmp(branch_a, branch_b, sizeof(branch_a)) == 0 && ret_a == ret_b,
              "restore reproduces the branch exactly");
        free(snap);
        cp_env_free(e);
    }

    /* --- observations stay finite and bounded --- */
    {
        CpWorld *w = (CpWorld *)malloc(sizeof(CpWorld));
        float obs[CP_OBS_DIM];
        CpRng rng; cp_rng_seed(&rng, 5);
        int bad = 0;
        for (int ep = 0; ep < 6; ep++) {
            CpMorph m; cp_morph_random(&m, &rng);
            cp_world_reset(w, (uint32_t)ep, &m);
            for (int t = 0; t < 1200 && w->status == CP_RUN; t++) {
                float act[CP_ACT_DIM] = { cp_rng_range(&rng, -1, 1), cp_rng_range(&rng, -1, 1), 0 };
                cp_world_step(w, act);
                cp_world_observe(w, obs);
                for (int i = 0; i < CP_OBS_DIM; i++)
                    if (!isfinite(obs[i]) || fabsf(obs[i]) > 4.0f) bad++;
            }
        }
        CHECK(bad == 0, "observations are finite and in range under random morphologies");
        free(w);
    }

    /* --- the editor's part budget is actually enforced --- */
    {
        CpRng rng; cp_rng_seed(&rng, 3);
        int bad = 0, toothless = 0;
        for (int i = 0; i < 4000; i++) {
            CpMorph m;
            m.herb = (uint8_t)cp_rng_int(&rng, 9); m.carn = (uint8_t)cp_rng_int(&rng, 9);
            m.spike = (uint8_t)cp_rng_int(&rng, 9); m.cilia = (uint8_t)cp_rng_int(&rng, 9);
            m.flag = (uint8_t)cp_rng_int(&rng, 9); m.elec = (uint8_t)cp_rng_int(&rng, 9);
            cp_morph_clamp(&m);
            int tot = m.herb + m.carn + m.spike + m.cilia + m.flag + m.elec;
            if (tot > CP_MORPH_MAX_PARTS) bad++;
            if (m.herb == 0 && m.carn == 0) toothless++;
        }
        CHECK(bad == 0, "clamp keeps every build inside the part budget");
        CHECK(toothless == 0, "no build ends up unable to eat");
    }

    /* --- episodes actually terminate --- */
    {
        CpWorld *w = (CpWorld *)malloc(sizeof(CpWorld));
        int unfinished = 0;
        for (int s = 0; s < 8; s++) {
            cp_world_reset(w, (uint32_t)(s * 977), NULL);
            int t = 0;
            for (; t <= CP_MAX_STEPS && w->status == CP_RUN; t++) {
                float a[CP_ACT_DIM];
                cp_policy_greedy(w, a);
                cp_world_step(w, a);
            }
            if (w->status == CP_RUN) unfinished++;
        }
        CHECK(unfinished == 0, "every episode reaches a terminal state");
        free(w);
    }

    printf(fails ? "\n%d test(s) failed\n" : "\nall tests passed\n", fails);
    return fails ? 1 : 0;
}
