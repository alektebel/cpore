#include "cpore/cpore.h"
#include "cpore/aqua.h"
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
static uint64_t rollout_hash(uint32_t seed, const CpGenome *g, int steps)
{
    CpWorld *w = (CpWorld *)malloc(sizeof(CpWorld));
    float obs[CP_OBS_DIM];
    uint64_t h = 1469598103934665603ull;

    cp_world_reset(w, seed, g);
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

static int count_of(const CpGenome *g, int type)
{
    int n = 0;
    for (int i = 0; i < CP_MAX_PARTS; i++) if (g->part[i].type == type) n++;
    return n;
}

static int has_mouth(const CpGenome *g)
{
    return count_of(g, CP_PART_FILTER) + count_of(g, CP_PART_JAW)
         + count_of(g, CP_PART_PROBOSCIS) > 0;
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

        for (int t = 0; t < 300; t++) {
            float a[CP_ACT_DIM];
            cp_policy_greedy(cp_env_world(e), a);
            cp_env_step(e, a, obs, &r, &term, &trunc);
        }

        void *snap = malloc(cp_env_state_size());
        cp_env_save(e, snap);

        float branch_a[CP_OBS_DIM];
        float ret_a = 0.0f;
        for (int t = 0; t < 200; t++) {
            float a[CP_ACT_DIM];
            cp_policy_greedy(cp_env_world(e), a);
            cp_env_step(e, a, branch_a, &r, &term, &trunc);
            ret_a += r;
        }

        cp_env_load(e, snap);
        float branch_b[CP_OBS_DIM];
        float ret_b = 0.0f;
        for (int t = 0; t < 200; t++) {
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

    /* --- observations stay finite and bounded under random genomes --- */
    {
        CpWorld *w = (CpWorld *)malloc(sizeof(CpWorld));
        float obs[CP_OBS_DIM];
        CpRng rng; cp_rng_seed(&rng, 5);
        int bad = 0;
        for (int ep = 0; ep < 8; ep++) {
            CpGenome g; cp_genome_random(&g, &rng, CP_GEN_BUDGET[0]);
            cp_world_reset(w, (uint32_t)ep, &g);
            for (int t = 0; t < 1500 && w->status == CP_RUN; t++) {
                float act[CP_ACT_DIM];
                for (int i = 0; i < CP_ACT_DIM; i++) act[i] = cp_rng_range(&rng, -1.2f, 1.2f);
                cp_world_step(w, act);
                cp_world_observe(w, obs);
                for (int i = 0; i < CP_OBS_DIM; i++)
                    if (!isfinite(obs[i]) || fabsf(obs[i]) > 3.0f) bad++;
            }
        }
        CHECK(bad == 0, "observations are finite and in range under random genomes");
        free(w);
    }

    /* --- the editor's DNA budget is actually enforced --- */
    {
        CpRng rng; cp_rng_seed(&rng, 3);
        int over = 0, toothless = 0, aliased = 0;
        for (int i = 0; i < 4000; i++) {
            int budget = CP_GEN_BUDGET[i % CP_GENERATIONS];
            CpGenome g;
            cp_genome_random(&g, &rng, budget);
            if (cp_genome_cost(&g) > budget) over++;
            if (!has_mouth(&g)) toothless++;
            for (int k = 0; k < CP_MAX_PARTS; k++)
                if (g.part[k].type >= CP_PART_COUNT) aliased++;
        }
        CHECK(over == 0, "no build ever exceeds its generation's DNA budget");
        CHECK(toothless == 0, "no build ends up unable to eat");
        CHECK(aliased == 0, "no genome slot holds an out-of-range part type");
    }

    /* --- the action-vector design head decodes to a legal genome --- */
    {
        CpRng rng; cp_rng_seed(&rng, 17);
        int over = 0, toothless = 0;
        float design[CP_MAX_PARTS * 2];
        for (int i = 0; i < 3000; i++) {
            /* deliberately include saturated and out-of-range outputs */
            for (int k = 0; k < CP_MAX_PARTS * 2; k++)
                design[k] = cp_rng_range(&rng, -2.0f, 2.0f);
            CpGenome g;
            cp_genome_from_action(&g, design, CP_GEN_BUDGET[2]);
            if (cp_genome_cost(&g) > CP_GEN_BUDGET[2]) over++;
            if (!has_mouth(&g)) toothless++;
        }
        CHECK(over == 0 && toothless == 0,
              "design head decodes to a legal genome for any policy output");
    }

    /* --- placement is load-bearing ---
     * A controlled probe rather than a statistical one: pin the player, hold
     * a target directly ahead, and hold a steady eastward action so heading
     * cannot drift. Only the mount angle varies. Full episodes are far too
     * chaotic to measure this - the between-seed variance buries it. */
    {
        float dealt[2] = { 0, 0 }, taken[2] = { 0, 0 };
        for (int rear = 0; rear < 2; rear++) {
            CpGenome g;
            cp_genome_clear(&g);
            g.part[0].type = CP_PART_FILTER; g.part[0].angle = 0;
            g.part[1].type = CP_PART_SPIKE;  g.part[1].angle = rear ? 128 : 0;
            cp_genome_normalise(&g, CP_GEN_BUDGET[0]);

            CpWorld *w = (CpWorld *)malloc(sizeof(CpWorld));
            cp_world_reset(w, 1, &g);
            for (int i = 1; i < CP_MAX_CELLS; i++) w->cells[i].alive = 0;
            CpCell *t = &w->cells[0];

            float a[CP_ACT_DIM];
            memset(a, 0, sizeof(a));
            a[0] = 1.0f;                 /* steer east: heading is pinned      */
            a[CP_ACT_CTRL] = 1e-9f;      /* non-null design head, but frozen   */

            for (int s = 0; s < 400; s++) {
                w->player.x = 1200.0f; w->player.y = 700.0f;
                w->player.vx = w->player.vy = 0.0f;
                w->player.hp = w->player.hp_max;
                t->alive = 1; t->hp = t->hp_max = 1e6f;
                t->r = 12.0f; t->armor = 0.0f; t->attack = 40.0f; t->poison = 0.0f;
                t->x = w->player.x + w->player.r + t->r - 2.0f;
                t->y = w->player.y;
                t->vx = t->vy = 0.0f;
                cp_world_step(w, a);
            }
            dealt[rear] = w->dmg_dealt;
            taken[rear] = w->dmg_taken;
            free(w);
        }
        printf("        spike forward: dealt %.0f taken %.0f | spike aft: dealt %.0f taken %.0f\n",
               (double)dealt[0], (double)taken[0], (double)dealt[1], (double)taken[1]);
        CHECK(dealt[0] > 0.0f && dealt[1] == 0.0f,
              "a spike damages only what it is pointing at");
        CHECK(taken[0] < taken[1] * 0.9f,
              "a spike facing the attacker also absorbs the hit");
    }

    /* --- a jet mounted forward is 16 DNA of dead weight --- */
    {
        CpGenome aft, fwd;
        CpStats sa, sf;
        cp_genome_clear(&aft);
        aft.part[0].type = CP_PART_FILTER; aft.part[0].angle = 0;
        aft.part[1].type = CP_PART_JET;    aft.part[1].angle = 128;
        fwd = aft;
        fwd.part[1].angle = 0;
        cp_genome_stats(&aft, &sa);
        cp_genome_stats(&fwd, &sf);
        printf("        jet aft thrust %.0f | jet forward thrust %.0f\n",
               (double)sa.jet_thrust, (double)sf.jet_thrust);
        CHECK(sa.jet_thrust > 0.0f && sf.jet_thrust == 0.0f,
              "only a jet's rearward component contributes thrust");
    }

    /* --- eyes buy information ---
     * Measured over a run, not at reset: cells spawn beyond even the best
     * perception radius, so comparing the first frame proves nothing. Food is
     * dense enough to fill the nearest-8 slots at any radius, so the signal
     * lives entirely in how many *cells* a build can account for. */
    {
        int seen[2] = { 0, 0 };
        const int cell_base = 13 + CP_OBS_FOOD_K * 4;
        for (int sighted = 0; sighted < 2; sighted++) {
            CpGenome g;
            cp_genome_clear(&g);
            g.part[0].type = CP_PART_FILTER; g.part[0].angle = 0;
            g.part[1].type = CP_PART_CILIA;  g.part[1].angle = 128;
            if (sighted) {
                g.part[2].type = CP_PART_EYE; g.part[2].angle = 32;
                g.part[3].type = CP_PART_EYE; g.part[3].angle = 224;
            }
            cp_genome_normalise(&g, CP_GEN_BUDGET[1]);

            CpWorld *w = (CpWorld *)malloc(sizeof(CpWorld));
            float obs[CP_OBS_DIM];
            cp_world_reset(w, 9, &g);
            for (int t = 0; t < 1200 && w->status == CP_RUN; t++) {
                float a[CP_ACT_DIM];
                cp_policy_greedy(w, a);
                memset(a + CP_ACT_CTRL, 0, sizeof(float) * CP_MAX_PARTS * 2);
                a[CP_ACT_CTRL] = 1e-9f;          /* freeze the build */
                cp_world_step(w, a);
                cp_world_observe(w, obs);
                for (int i = 0; i < CP_OBS_CELL_K; i++)
                    if (obs[cell_base + i * 6 + 3] != 0.0f || obs[cell_base + i * 6] != 0.0f)
                        seen[sighted]++;
            }
            free(w);
        }
        printf("        cell sightings over 1200 steps - blind: %d | two eyes: %d\n",
               seen[0], seen[1]);
        CHECK(seen[1] > seen[0] * 3 / 2,
              "two eyes see substantially more of the pool than none");
    }

    /* --- generations actually fire and change the build --- */
    {
        CpWorld *w = (CpWorld *)malloc(sizeof(CpWorld));
        cp_world_reset(w, 23, NULL);
        int parts0 = w->stats.n_parts;
        for (int t = 0; t < CP_MAX_STEPS && w->status == CP_RUN; t++) {
            float a[CP_ACT_DIM];
            cp_policy_greedy(w, a);
            cp_world_step(w, a);
        }
        printf("        gen 1 parts: %d -> gen %d parts: %d (%d design events)\n",
               parts0, w->generation + 1, w->stats.n_parts, w->design_events);
        CHECK(w->design_events > 0, "the editor opens at least once per run");
        CHECK(w->stats.n_parts > parts0, "later generations are more complex");
        CHECK(cp_genome_cost(&w->genome) <= CP_GEN_BUDGET[w->generation],
              "the live genome respects its generation's budget");
        free(w);
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

    /* ================= stage 2: aquatic ================= */
    printf("\ncpore aquatic-stage tests\n");

    /* --- determinism and snapshot in 3D --- */
    {
        Cp3World *a = (Cp3World *)malloc(sizeof(Cp3World));
        Cp3World *b = (Cp3World *)malloc(sizeof(Cp3World));
        cp3_world_reset(a, 5, NULL);
        cp3_world_reset(b, 5, NULL);
        int same = 1;
        for (int t = 0; t < 900; t++) {
            float aa[CP3_ACT_DIM], bb[CP3_ACT_DIM];
            cp3_policy_greedy(a, aa); cp3_world_step(a, aa);
            cp3_policy_greedy(b, bb); cp3_world_step(b, bb);
            if (a->reward != b->reward || a->biomass != b->biomass) { same = 0; break; }
        }
        CHECK(same, "aquatic: identical seeds produce identical trajectories");
        free(a); free(b);
    }
    {
        Cp3Env *e = cp3_env_create(9);
        float obs[CP3_OBS_DIM], r;
        int32_t te, tr;
        cp3_env_reset(e, 9, NULL, obs);
        for (int t = 0; t < 400; t++) {
            float a[CP3_ACT_DIM];
            cp3_policy_greedy(cp3_env_world(e), a);
            cp3_env_step(e, a, obs, &r, &te, &tr);
        }
        void *snap = malloc(cp3_env_state_size());
        cp3_env_save(e, snap);
        float o1[CP3_OBS_DIM], o2[CP3_OBS_DIM];
        float r1 = 0.0f, r2 = 0.0f;
        for (int t = 0; t < 250; t++) {
            float a[CP3_ACT_DIM];
            cp3_policy_greedy(cp3_env_world(e), a);
            cp3_env_step(e, a, o1, &r, &te, &tr); r1 += r;
        }
        cp3_env_load(e, snap);
        for (int t = 0; t < 250; t++) {
            float a[CP3_ACT_DIM];
            cp3_policy_greedy(cp3_env_world(e), a);
            cp3_env_step(e, a, o2, &r, &te, &tr); r2 += r;
        }
        CHECK(memcmp(o1, o2, sizeof(o1)) == 0 && r1 == r2,
              "aquatic: restore reproduces the branch exactly");
        free(snap);
        cp3_env_free(e);
    }

    /* --- observations stay bounded under random genomes and random actions --- */
    {
        Cp3World *w = (Cp3World *)malloc(sizeof(Cp3World));
        float obs[CP3_OBS_DIM];
        CpRng rng; cp_rng_seed(&rng, 21);
        int bad = 0;
        for (int ep = 0; ep < 4; ep++) {
            Cp3Genome g; cp3_genome_random(&g, &rng, CP3_GEN_BUDGET[0]);
            cp3_world_reset(w, (uint32_t)ep, &g);
            for (int t = 0; t < 900 && w->status == CP3_RUN; t++) {
                float a[CP3_ACT_DIM];
                for (int i = 0; i < CP3_ACT_DIM; i++) a[i] = cp_rng_range(&rng, -1.2f, 1.2f);
                cp3_world_step(w, a);
                cp3_world_observe(w, obs);
                for (int i = 0; i < CP3_OBS_DIM; i++)
                    if (!isfinite(obs[i]) || fabsf(obs[i]) > 3.0f) bad++;
            }
        }
        CHECK(bad == 0, "aquatic: observations are finite and in range");
        free(w);
    }

    /* --- the 3D editor's budget holds, including under mutation --- */
    {
        CpRng rng; cp_rng_seed(&rng, 4);
        int over = 0, mouthless = 0;
        Cp3Genome g;
        cp3_genome_random(&g, &rng, CP3_GEN_BUDGET[1]);
        for (int i = 0; i < 6000; i++) {
            cp3_genome_mutate(&g, &rng, CP3_GEN_BUDGET[1], 0.3f);
            if (cp3_genome_cost(&g) > CP3_GEN_BUDGET[1]) over++;
            int m = 0;
            for (int k = 0; k < CP3_MAX_PARTS; k++)
                if (g.part[k].type == CP3_FILTER || g.part[k].type == CP3_JAW) m++;
            if (!m) mouthless++;
        }
        CHECK(over == 0, "aquatic: 6000 rounds of mutation never break the budget");
        CHECK(mouthless == 0, "aquatic: mutation never produces an animal with no mouth");
    }

    /* --- placement is load-bearing in 3D too: a jaw only reaches through
     *     the cone it points down --- */
    {
        Cp3Genome fwdg, aftg;
        cp3_genome_clear(&fwdg);
        fwdg.nseg = 3;
        fwdg.part[0].type = CP3_JAW;  fwdg.part[0].seg = 0; fwdg.part[0].yaw = 0;
        fwdg.part[1].type = CP3_TAIL; fwdg.part[1].seg = 2; fwdg.part[1].yaw = 128;
        aftg = fwdg;
        aftg.part[0].yaw = 128;              /* same jaw, mounted facing astern */
        cp3_genome_normalise(&fwdg, CP3_GEN_BUDGET[0]);
        cp3_genome_normalise(&aftg, CP3_GEN_BUDGET[0]);

        float dealt[2] = { 0.0f, 0.0f };
        for (int aft = 0; aft < 2; aft++) {
            Cp3World *w = (Cp3World *)malloc(sizeof(Cp3World));
            cp3_world_reset(w, 2, aft ? &aftg : &fwdg);
            for (int i = 1; i < CP3_MAX_FISH; i++) w->fish[i].alive = 0;
            Cp3Fish *t = &w->fish[0];
            float a[CP3_ACT_DIM];
            memset(a, 0, sizeof(a));
            a[3] = 1.0f;                     /* bite, every step */
            a[CP3_ACT_CTRL] = 1e-9f;         /* non-null design head, frozen */
            for (int st = 0; st < 900; st++) {
                w->player.p.x = 700.0f; w->player.p.y = 300.0f; w->player.p.z = 700.0f;
                w->player.v.x = w->player.v.y = w->player.v.z = 0.0f;
                w->player.yaw = 0.0f; w->player.pitch = 0.0f;
                w->player.hp = w->player.hp_max;
                w->bite_cd = 0.0f;
                t->alive = 1; t->hp = t->hp_max = 1e6f;
                /* held dead ahead, on the +x axis the player is facing */
                t->p.x = w->player.p.x + w->player.s.radius + t->s.radius + 2.0f;
                t->p.y = w->player.p.y; t->p.z = w->player.p.z;
                t->v.x = t->v.y = t->v.z = 0.0f;
                cp3_world_step(w, a);
            }
            dealt[aft] = w->dmg_dealt;
            free(w);
        }
        printf("        jaw forward: dealt %.0f | jaw astern: dealt %.0f (%.1fx)\n",
               (double)dealt[0], (double)dealt[1],
               (double)(dealt[0] / (dealt[1] > 1.0f ? dealt[1] : 1.0f)));
        /* Not exactly zero: collision separation nudges the pair a little each
         * step, so the rear jaw clips the target on a handful of frames. The
         * claim being tested is that facing dominates, not that it is absolute. */
        CHECK(dealt[0] > dealt[1] * 25.0f,
              "aquatic: a jaw bites what it points at, and little else");
    }

    /* --- NATURAL SELECTION ---
     * Founders are drawn uniformly from the design space, so the starting
     * population is full of animals that cannot feed themselves. Nothing in
     * the simulation prefers a working body plan; the ones that eat simply
     * breed more. If selection is real, the population's mean mouth and tail
     * counts must rise on their own. */
    {
        int rose_mouth = 0, rose_tail = 0, trials = 0;
        float d_mouth = 0.0f, d_tail = 0.0f, gens = 0.0f;
        Cp3World *w = (Cp3World *)malloc(sizeof(Cp3World));
        for (int seed = 0; seed < 5; seed++) {
            cp3_world_reset(w, (uint32_t)(seed * 101 + 3), NULL);
            /* founder census before anything has bred */
            float m0 = 0.0f, t0 = 0.0f;
            for (int i = 0; i < CP3_MAX_FISH; i++) {
                m0 += w->fish[i].s.n[CP3_FILTER] + w->fish[i].s.n[CP3_JAW];
                t0 += w->fish[i].s.n[CP3_TAIL];
            }
            m0 /= CP3_MAX_FISH; t0 /= CP3_MAX_FISH;

            /* let the ocean run without a player in it - no policy, no reward,
             * just metabolism and breeding */
            float a[CP3_ACT_DIM];
            memset(a, 0, sizeof(a));
            for (int t = 0; t < 9000; t++) {
                w->player.hp = w->player.hp_max;     /* keep the episode alive */
                cp3_world_step(w, a);
            }
            d_mouth += w->mean_mouth - m0;
            d_tail  += w->mean_tail - t0;
            gens    += w->mean_gen;
            if (w->mean_mouth > m0) rose_mouth++;
            if (w->mean_tail > t0) rose_tail++;
            trials++;
        }
        printf("        over %d oceans: mean generation %.1f, mouths %+.2f, tails %+.2f\n",
               trials, (double)(gens / trials), (double)(d_mouth / trials),
               (double)(d_tail / trials));
        CHECK(gens / trials > 3.0f, "aquatic: several generations turn over per episode");
        CHECK(rose_mouth >= 4, "aquatic: selection raises the population's mouth count");
        CHECK(rose_tail >= 4, "aquatic: selection raises the population's tail count");
        free(w);
    }

    printf(fails ? "\n%d test(s) failed\n" : "\nall tests passed\n", fails);
    return fails ? 1 : 0;
}
