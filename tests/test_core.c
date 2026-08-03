#include "cpore/cpore.h"
#include "cpore/aqua.h"
#include "cpore/land.h"
#include "cpore/civ.h"
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

    /* ================= STAGE 3: CREATURE ================= */
    printf("\n-- land --\n");

    /* Terrain is a pure function of seed and position. That is the property
     * that lets the world carry no heightmap, lets the renderer evaluate the
     * ground wherever a ray happens to land, and keeps snapshots small. */
    {
        int stable = 1, varied = 0, differs = 0;
        float lo = 1e9f, hi = -1e9f;
        for (int i = 0; i < 400; i++) {
            float x = (float)(i * 37 % 2100) + 30.0f;
            float z = (float)(i * 91 % 2100) + 30.0f;
            float a = cp4_height(7, x, z), b = cp4_height(7, x, z);
            if (a != b) stable = 0;
            if (cp4_height(8, x, z) != a) differs++;
            if (a < lo) lo = a;
            if (a > hi) hi = a;
        }
        if (hi - lo > 80.0f) varied = 1;
        printf("        terrain y range %.0f .. %.0f over 400 probes, %d/400 differ by seed\n",
               (double)lo, (double)hi, differs);
        CHECK(stable, "land: terrain is a pure function of position");
        CHECK(differs > 380, "land: a different seed grows a different world");
        CHECK(varied, "land: the world has real relief, not a plain");
    }

    /* The normal has to point at the sky. It got this backwards once, which
     * silently inverted every slope penalty in the movement code. */
    {
        int skyward = 1;
        for (int i = 0; i < 200; i++) {
            float x = (float)(i * 53 % 2100) + 30.0f;
            float z = (float)(i * 71 % 2100) + 30.0f;
            float nx, ny, nz;
            cp4_normal(11, x, z, &nx, &ny, &nz);
            if (ny > -0.15f) skyward = 0;    /* y is down, so up is negative */
            float l = nx * nx + ny * ny + nz * nz;
            if (l < 0.98f || l > 1.02f) skyward = 0;
        }
        CHECK(skyward, "land: terrain normals are unit length and point at the sky");
    }

    /* Same contract as the earlier stages: identical seed, identical episode. */
    {
        Cp4World *a = (Cp4World *)malloc(sizeof(Cp4World));
        Cp4World *b = (Cp4World *)malloc(sizeof(Cp4World));
        cp4_world_reset(a, 19, NULL);
        cp4_world_reset(b, 19, NULL);
        int same = 1;
        for (int t = 0; t < 1500; t++) {
            float act[CP4_ACT_DIM];
            cp4_policy_greedy(a, act);
            cp4_world_step(a, act);
            cp4_policy_greedy(b, act);
            cp4_world_step(b, act);
        }
        if (memcmp(a, b, sizeof(Cp4World)) != 0) same = 0;
        CHECK(same, "land: the same seed replays exactly");
        free(a); free(b);
    }

    /* Snapshot and restore, which is what the whole POD-world discipline buys. */
    {
        Cp4Env *e = cp4_env_create(23);
        float obs[CP4_OBS_DIM], act[CP4_ACT_DIM], r;
        int32_t term, trunc;
        void *snap = malloc(cp4_env_state_size());
        memset(act, 0, sizeof(act));
        act[0] = 0.3f; act[2] = 0.8f;
        for (int t = 0; t < 400; t++) cp4_env_step(e, act, obs, &r, &term, &trunc);
        cp4_env_save(e, snap);
        float after[CP4_OBS_DIM];
        for (int t = 0; t < 200; t++) cp4_env_step(e, act, after, &r, &term, &trunc);
        cp4_env_load(e, snap);
        float redo[CP4_OBS_DIM];
        for (int t = 0; t < 200; t++) cp4_env_step(e, act, redo, &r, &term, &trunc);
        CHECK(memcmp(after, redo, sizeof(after)) == 0,
              "land: restore reproduces the future exactly");
        free(snap);
        cp4_env_free(e);
    }

    /* A body plan must clear its own belly. When stand was computed from the
     * nominal radius rather than the widest segment, fat builds were buried to
     * the waist and their legs rendered underground. */
    {
        CpRng rng;
        cp_rng_seed(&rng, 5);
        int clears = 0, trials = 200;
        for (int i = 0; i < trials; i++) {
            Cp4Genome g;
            Cp4Stats st;
            cp4_genome_random(&g, &rng, CP4_GEN_BUDGET[3]);
            cp4_genome_normalise(&g, CP4_GEN_BUDGET[3]);
            cp4_genome_stats(&g, &st);
            float widest = 0.0f;
            int nseg = g.nseg < 2 ? 2 : g.nseg;
            for (int k = 0; k < nseg; k++) {
                float t = nseg > 1 ? (float)k / (float)(nseg - 1) : 0.0f;
                int li = k < CP4_MAX_SEG ? k : CP4_MAX_SEG - 1;
                float rr = cp4_profile(&g, t) * (1.0f + (float)g.lump[li] / 127.0f * 0.40f);
                if (rr > widest) widest = rr;
            }
            if (st.stand > st.radius * widest) clears++;
        }
        printf("        %d/%d random builds stand clear of their own widest segment\n",
               clears, trials);
        CHECK(clears == trials, "land: every body plan stands clear of the ground");
    }

    /* Budgets, a guaranteed mouth and a guaranteed leg pair. A land animal
     * with nothing to eat with or nothing to walk on cannot play the stage. */
    {
        CpRng rng;
        cp_rng_seed(&rng, 77);
        int ok_budget = 1, ok_mouth = 1, ok_legs = 1;
        for (int gen = 0; gen < CP4_GENERATIONS; gen++) {
            for (int i = 0; i < 300; i++) {
                Cp4Genome g;
                Cp4Stats st;
                cp4_genome_random(&g, &rng, CP4_GEN_BUDGET[gen]);
                cp4_genome_normalise(&g, CP4_GEN_BUDGET[gen]);
                cp4_genome_stats(&g, &st);
                if (cp4_genome_cost(&g) > CP4_GEN_BUDGET[gen]) ok_budget = 0;
                if (st.n[CP4_MOUTH_G] + st.n[CP4_MOUTH_C] + st.n[CP4_MOUTH_O] < 1) ok_mouth = 0;
                /* legs are no longer the only way to get around: a finned or
                 * winged body is allowed to have none */
                if (st.n[CP4_LEG] + st.n[CP4_FIN] + st.n[CP4_WING] < 1) ok_legs = 0;
            }
        }
        CHECK(ok_budget, "land: normalise never exceeds the generation budget");
        CHECK(ok_mouth, "land: every normalised genome keeps a mouth");
        CHECK(ok_legs, "land: every normalised genome keeps a way to move");
    }

    /* The observation must write exactly as many floats as it advertises.
     * Getting this wrong overruns whatever buffer the caller supplied, and it
     * is silent until something further up the stack is corrupted - which is
     * exactly how the medium block shipped one value over budget. */
    {
        static float buf[CP4_OBS_DIM + 16];
        Cp4World *w = (Cp4World *)malloc(sizeof(Cp4World));
        int exact = 1;
        for (int seed = 0; seed < 3; seed++) {
            for (int i = 0; i < CP4_OBS_DIM + 16; i++) buf[i] = -999.0f;
            cp4_world_reset(w, (uint32_t)(seed * 7 + 1), NULL);
            for (int t = 0; t < 60; t++) {
                float act[CP4_ACT_DIM];
                cp4_policy_greedy(w, act);
                cp4_world_step(w, act);
            }
            cp4_world_observe(w, buf);
            for (int i = CP4_OBS_DIM; i < CP4_OBS_DIM + 16; i++)
                if (buf[i] != -999.0f) exact = 0;
            /* and it must fill every slot it claims */
            int untouched = 0;
            for (int i = 0; i < CP4_OBS_DIM; i++) if (buf[i] == -999.0f) untouched++;
            if (untouched) exact = 0;
        }
        CHECK(exact, "land: the observation writes exactly CP4_OBS_DIM floats");
        free(w);
    }

    /* --- THE FOUR MEDIA ---
     * Ground, water, air and soil have to be four ways to live, not one way
     * plus three decorations. Each archetype is run from its own starting
     * build and has to spend real time in its own medium and eat the food that
     * grows there - a medium nobody enters, or enters but cannot feed in, is
     * not pulling its weight. */
    {
        long med[CP4_MEDIUM_COUNT] = { 0, 0, 0, 0 };
        int kelp = 0, tuber = 0, air_style_air = 0, dug = 0, swam = 0;
        int nests = 0, hatched = 0, found = 0;
        Cp4World *w = (Cp4World *)malloc(sizeof(Cp4World));
        for (int st = 0; st < CP4_STYLE_COUNT; st++) {
            long m[CP4_MEDIUM_COUNT] = { 0, 0, 0, 0 };
            for (int s = 0; s < 6; s++) {
                Cp4Genome g;
                cp4_genome_autodesign(&g, NULL, CP4_GEN_BUDGET[0], st);
                cp4_world_reset(w, (uint32_t)(s * 13 + 3), &g);
                for (int t = 0; t < CP4_MAX_STEPS && w->status == CP4_RUN; t++) {
                    float act[CP4_ACT_DIM];
                    cp4_policy_greedy(w, act);
                    cp4_world_step(w, act);
                }
                for (int k = 0; k < CP4_MEDIUM_COUNT; k++) {
                    med[k] += w->medium_steps[k];
                    m[k] += w->medium_steps[k];
                }
                kelp += w->ate_kelp;
                tuber += w->ate_tuber;
                nests += w->home.alive ? 1 : 0;
                hatched += w->hatchlings;
                found += w->discovered;
            }
            long tot = m[0] + m[1] + m[2] + m[3];
            if (tot < 1) tot = 1;
            printf("        %-9s ground %2ld%%  water %2ld%%  air %2ld%%  under %2ld%%\n",
                   cp4_style_name(st), m[0] * 100 / tot, m[1] * 100 / tot,
                   m[2] * 100 / tot, m[3] * 100 / tot);
            if (st == CP4_STYLE_SWIMMER  && m[CP4_IN_WATER] * 4 > tot) swam = 1;
            if (st == CP4_STYLE_FLYER    && m[CP4_IN_AIR] * 20 > tot) air_style_air = 1;
            if (st == CP4_STYLE_BURROWER && m[CP4_UNDER] * 4 > tot) dug = 1;
        }
        printf("        ate %d kelp and %d tubers, built %d nests, hatched %d, found %d\n",
               kelp, tuber, nests, hatched, found);
        free(w);
        CHECK(swam, "land: a finned build spends its life in the water");
        CHECK(air_style_air, "land: a winged build actually uses the sky");
        CHECK(dug, "land: a clawed build actually lives underground");
        CHECK(kelp > 0, "land: the water feeds what lives in it");
        CHECK(tuber > 0, "land: the soil feeds what lives in it");
        CHECK(nests > 0, "land: the player can build a nest");
        CHECK(hatched > 0, "land: a nest hatches followers");
        CHECK(found > 0, "land: new species are met by going somewhere");
    }

    /* --- MEDIA ARE GATED BY THE BODY ---
     * Fins, wings and digging claws are gates, not modifiers. A body without
     * them should not be able to make a living in that medium at all, which is
     * what makes buying one a decision. */
    {
        CpRng rng;
        cp_rng_seed(&rng, 909);
        int ok = 1;
        for (int i = 0; i < 400; i++) {
            Cp4Genome g;
            Cp4Stats st;
            cp4_genome_random(&g, &rng, CP4_GEN_BUDGET[3]);
            cp4_genome_normalise(&g, CP4_GEN_BUDGET[3]);
            cp4_genome_stats(&g, &st);
            if (st.n[CP4_FIN] == 0 && st.swim != 0.0f) ok = 0;
            if (st.n[CP4_WING] == 0 && st.fly != 0.0f) ok = 0;
            if (st.n[CP4_DIGGER] == 0 && st.dig != 0.0f) ok = 0;
            if (st.n[CP4_GILL] > 0 && st.breath < 1.0e8f) ok = 0;
        }
        CHECK(ok, "land: no part, no medium - swim, fly and dig are gates");
    }

    /* --- AN UNBOUNDED WORLD ---
     * The terrain is a pure function of position, so there is nothing to fence
     * the animal in with. What has to stay true is that walking a long way
     * neither breaks the observation nor empties the world. */
    {
        Cp4World *w = (Cp4World *)malloc(sizeof(Cp4World));
        float obs[CP4_OBS_DIM];
        int finite = 1, populated = 1;
        float furthest = 0.0f;
        for (int seed = 0; seed < 3; seed++) {
            cp4_world_reset(w, (uint32_t)(seed * 31 + 2), NULL);
            for (int t = 0; t < CP4_MAX_STEPS; t++) {
                float act[CP4_ACT_DIM];
                memset(act, 0, sizeof(act));
                /* walk in one direction and never turn round */
                act[0] = 0.0f;
                act[2] = 1.0f;
                w->player.hp = w->player.hp_max;
                w->player.energy = 120.0f;
                cp4_world_step(w, act);
                cp4_world_observe(w, obs);
                for (int i = 0; i < CP4_OBS_DIM; i++)
                    if (!(obs[i] == obs[i]) || obs[i] < -4.0f || obs[i] > 4.0f) finite = 0;
            }
            if (w->far_from_start > furthest) furthest = w->far_from_start;
            /* the world must still be around the animal, not behind it */
            int near_food = 0;
            for (int i = 0; i < CP4_MAX_FLORA; i++) {
                if (w->flora[i].type == CP4_FLORA_NONE) continue;
                float dx = w->flora[i].p.x - w->player.p.x;
                float dz = w->flora[i].p.z - w->player.p.z;
                if (dx * dx + dz * dz < 900.0f * 900.0f) near_food++;
            }
            if (near_food < 30) populated = 0;
        }
        printf("        walked %.0f units from the start and the world came with it\n",
               (double)furthest);
        CHECK(furthest > 2000.0f, "land: the animal can leave the starting region");
        CHECK(finite, "land: observations stay finite however far you go");
        CHECK(populated, "land: the world streams along with the player");
        free(w);
    }

    /* --- THE FORK ---
     * The stage is meant to have two answers, not one. Charm and violence are
     * bought out of the same budget, so if only one of them can finish an
     * episode the design space has collapsed to a single strategy. This runs
     * the scripted baseline from each starting build and checks that each one
     * makes progress by its own mechanic. */
    {
        int reached[CP4_STYLE_COUNT];
        int songs[CP4_STYLE_COUNT], kills[CP4_STYLE_COUNT];
        memset(reached, 0, sizeof(reached));
        memset(songs, 0, sizeof(songs));
        memset(kills, 0, sizeof(kills));
        Cp4World *w = (Cp4World *)malloc(sizeof(Cp4World));
        for (int st = 0; st < 3; st++) {
            for (int s = 0; s < 6; s++) {
                Cp4Genome g;
                cp4_genome_autodesign(&g, NULL, CP4_GEN_BUDGET[0], st);
                cp4_world_reset(w, (uint32_t)(s * 13 + 3), &g);
                for (int t = 0; t < CP4_MAX_STEPS && w->status == CP4_RUN; t++) {
                    float act[CP4_ACT_DIM];
                    cp4_policy_greedy(w, act);
                    cp4_world_step(w, act);
                }
                if (w->dna >= CP4_DNA_GOAL * 0.5f) reached[st]++;
                songs[st] += w->songs;
                kills[st] += w->kills;
            }
            printf("        %-8s reached half the meter %d/6, songs %3d, kills %2d\n",
                   cp4_style_name(st), reached[st], songs[st], kills[st]);
        }
        free(w);
        CHECK(reached[CP4_STYLE_GRAZER] >= 2, "land: the grazing build is viable");
        CHECK(reached[CP4_STYLE_PREDATOR] >= 2, "land: the predator build is viable");
        CHECK(reached[CP4_STYLE_CHARMER] >= 2, "land: the charmer build is viable");
        CHECK(songs[CP4_STYLE_CHARMER] > 0 && kills[CP4_STYLE_CHARMER] == 0,
              "land: the charmer wins by impressing, not by eating");
        CHECK(kills[CP4_STYLE_PREDATOR] > 0 && songs[CP4_STYLE_PREDATOR] == 0,
              "land: the predator wins by eating, not by impressing");
    }

    /* --- NATURAL SELECTION, ON LAND ---
     * Same claim as the ocean: founders are random, nothing prefers a working
     * body, and the ones that can feed themselves simply breed more. */
    {
        int rose_legs = 0, trials = 0;
        float d_legs = 0.0f, gens = 0.0f;
        Cp4World *w = (Cp4World *)malloc(sizeof(Cp4World));
        for (int seed = 0; seed < 5; seed++) {
            cp4_world_reset(w, (uint32_t)(seed * 71 + 5), NULL);
            /* Legs are no longer the only way to get around, so the claim is
             * about whatever moves an animal - a population that drifts toward
             * fins in a flooded world is selection working, not failing. */
            float l0 = 0.0f;
            for (int i = 0; i < CP4_MAX_BEASTS; i++)
                l0 += w->beast[i].s.n[CP4_LEG] + w->beast[i].s.n[CP4_FIN]
                    + w->beast[i].s.n[CP4_WING];
            l0 /= CP4_MAX_BEASTS;

            float a[CP4_ACT_DIM];
            memset(a, 0, sizeof(a));
            for (int t = 0; t < CP4_MAX_STEPS; t++) {
                w->player.hp = w->player.hp_max;   /* keep the episode alive */
                cp4_world_step(w, a);
            }
            float l1 = 0.0f;
            int alive = 0;
            for (int i = 0; i < CP4_MAX_BEASTS; i++) {
                if (!w->beast[i].alive) continue;
                l1 += w->beast[i].s.n[CP4_LEG] + w->beast[i].s.n[CP4_FIN]
                    + w->beast[i].s.n[CP4_WING];
                alive++;
            }
            if (alive) l1 /= alive;
            d_legs += l1 - l0;
            gens += w->mean_gen;
            if (l1 > l0) rose_legs++;
            trials++;
        }
        printf("        over %d worlds: mean generation %.1f, movers %+.2f\n",
               trials, (double)(gens / trials), (double)(d_legs / trials));
        CHECK(gens / trials > 1.5f, "land: generations turn over inside an episode");
        CHECK(rose_legs >= 4, "land: selection raises the population's mobility");
        free(w);
    }

    /* ================= STAGE 4: CIVILISATION ================= */
    printf("\n-- civilisation --\n");

    /* Same seed, same planet. The cities are laid out on the stage-3
     * heightfield, which is what makes the two stages the same world rather
     * than two worlds that happen to share a number. */
    {
        Cp5World *w = (Cp5World *)malloc(sizeof(Cp5World));
        int dry = 1, spread = 1;
        cp5_world_reset(w, 12, NULL);
        for (int i = 0; i < w->n_cities; i++) {
            if (cp4_height(12, w->city[i].p.x, w->city[i].p.z) > CP5_SEA) dry = 0;
            for (int k = i + 1; k < w->n_cities; k++) {
                float dx = w->city[i].p.x - w->city[k].p.x;
                float dz = w->city[i].p.z - w->city[k].p.z;
                if (dx * dx + dz * dz < 200.0f * 200.0f) spread = 0;
            }
        }
        printf("        %d cities and %d spice on the stage-3 heightfield\n",
               w->n_cities, w->n_spice);
        CHECK(w->n_cities >= 8, "civ: the map fills with cities");
        CHECK(dry, "civ: every city stands on the same world's dry land");
        CHECK(spread, "civ: cities are places, not one blob");
        free(w);
    }

    /* Determinism and snapshot, the contract every stage keeps. */
    {
        Cp5World *a = (Cp5World *)malloc(sizeof(Cp5World));
        Cp5World *b = (Cp5World *)malloc(sizeof(Cp5World));
        cp5_world_reset(a, 31, NULL);
        cp5_world_reset(b, 31, NULL);
        for (int t = 0; t < 900; t++) {
            float act[CP5_ACT_DIM];
            cp5_policy_greedy(a, act);
            cp5_world_step(a, act);
            cp5_policy_greedy(b, act);
            cp5_world_step(b, act);
        }
        CHECK(memcmp(a, b, sizeof(Cp5World)) == 0, "civ: the same seed replays exactly");
        free(a); free(b);
    }
    {
        Cp5Env *e = cp5_env_create(37);
        float obs[CP5_OBS_DIM], act[CP5_ACT_DIM], r;
        int32_t term, trunc;
        void *snap = malloc(cp5_env_state_size());
        memset(act, 0, sizeof(act));
        act[CP5_MIL] = 1.0f;
        for (int t = 0; t < 300; t++) cp5_env_step(e, act, obs, &r, &term, &trunc);
        cp5_env_save(e, snap);
        float after[CP5_OBS_DIM], redo[CP5_OBS_DIM];
        for (int t = 0; t < 300; t++) cp5_env_step(e, act, after, &r, &term, &trunc);
        cp5_env_load(e, snap);
        for (int t = 0; t < 300; t++) cp5_env_step(e, act, redo, &r, &term, &trunc);
        CHECK(memcmp(after, redo, sizeof(after)) == 0,
              "civ: restore reproduces the future exactly");
        free(snap);
        cp5_env_free(e);
    }

    /* Observations have to stay finite and bounded whatever the board does. */
    {
        Cp5World *w = (Cp5World *)malloc(sizeof(Cp5World));
        float obs[CP5_OBS_DIM];
        int ok = 1;
        for (int seed = 0; seed < 4; seed++) {
            cp5_world_reset(w, (uint32_t)(seed * 29 + 1), NULL);
            for (int t = 0; t < 1200; t++) {
                float act[CP5_ACT_DIM];
                cp5_policy_greedy(w, act);
                cp5_world_step(w, act);
                cp5_world_observe(w, obs);
                for (int i = 0; i < CP5_OBS_DIM; i++)
                    if (!(obs[i] == obs[i]) || obs[i] < -4.0f || obs[i] > 4.0f) ok = 0;
                if (w->status != CP5_RUN) break;
            }
        }
        CHECK(ok, "civ: observations are finite and in range");
        free(w);
    }

    /* --- THE THREE APPROACHES ---
     * Force, trade and faith have to be three mechanics, not one mechanic with
     * three labels. Each is forced in turn and has to take cities on its own,
     * and each has to leave a different mark: conquest destroys population,
     * trade pays for itself, conversion does neither. */
    {
        const char *nm[CP5_APPROACH_COUNT] = { "force", "trade", "faith" };
        int took[CP5_APPROACH_COUNT] = { 0, 0, 0 };
        float pop_after[CP5_APPROACH_COUNT] = { 0.0f, 0.0f, 0.0f };
        Cp5World *w = (Cp5World *)malloc(sizeof(Cp5World));
        for (int a = 0; a < CP5_APPROACH_COUNT; a++) {
            for (int s = 0; s < 6; s++) {
                Cp5Legacy l;
                for (int q = 0; q < CP5_APPROACH_COUNT; q++) l.bonus[q] = 0.85f;
                l.bonus[a] = 1.55f;
                cp5_world_reset(w, (uint32_t)(s * 17 + 3), &l);
                for (int t = 0; t < CP5_MAX_STEPS && w->status == CP5_RUN; t++) {
                    float act[CP5_ACT_DIM];
                    cp5_policy_greedy(w, act);
                    cp5_world_step(w, act);
                }
                took[a] += (a == CP5_MIL) ? w->captured
                         : (a == CP5_ECO) ? w->bought : w->converted;
                float p = 0.0f;
                int n = 0;
                for (int i = 0; i < w->n_cities; i++)
                    if (w->city[i].alive && w->city[i].owner == CP5_PLAYER) {
                        p += w->city[i].pop; n++;
                    }
                if (n) pop_after[a] += p / n;
            }
            printf("        %-6s took %3d cities over 6 seeds, mean pop held %.0f\n",
                   nm[a], took[a], (double)(pop_after[a] / 6.0f));
        }
        free(w);
        CHECK(took[CP5_MIL] > 0, "civ: cities can be taken by force");
        CHECK(took[CP5_ECO] > 0, "civ: cities can be bought");
        CHECK(took[CP5_REL] > 0, "civ: cities can be converted");
    }

    /* A species carries its body forward. A creature that won stage 3 by
     * singing has to arrive here as a missionary power, not as a generic one -
     * this is the only coupling between the two stages and it has to bite. */
    {
        Cp4Genome warlike, devout;
        Cp5Legacy lw, ld;
        cp4_genome_autodesign(&warlike, NULL, CP4_GEN_BUDGET[3], CP4_STYLE_PREDATOR);
        cp4_genome_autodesign(&devout, NULL, CP4_GEN_BUDGET[3], CP4_STYLE_CHARMER);
        cp5_legacy_from_creature(&warlike, &lw);
        cp5_legacy_from_creature(&devout, &ld);
        printf("        predator -> M%.2f E%.2f R%.2f | charmer -> M%.2f E%.2f R%.2f\n",
               (double)lw.bonus[CP5_MIL], (double)lw.bonus[CP5_ECO], (double)lw.bonus[CP5_REL],
               (double)ld.bonus[CP5_MIL], (double)ld.bonus[CP5_ECO], (double)ld.bonus[CP5_REL]);
        CHECK(lw.bonus[CP5_MIL] > lw.bonus[CP5_REL],
              "civ: a predator arrives as a military power");
        CHECK(ld.bonus[CP5_REL] > ld.bonus[CP5_MIL],
              "civ: a charmer arrives as a missionary power");
    }

    /* Every episode has to end, and a nation that loses its last city has to
     * actually be out - a stage that can run forever is not an environment. */
    {
        Cp5World *w = (Cp5World *)malloc(sizeof(Cp5World));
        int ended = 0, trials = 0, consistent = 1;
        for (int seed = 0; seed < 10; seed++) {
            cp5_world_reset(w, (uint32_t)(seed * 41 + 7), NULL);
            for (int t = 0; t < CP5_MAX_STEPS + 4; t++) {
                float act[CP5_ACT_DIM];
                cp5_policy_greedy(w, act);
                cp5_world_step(w, act);
                if (w->status != CP5_RUN) break;
            }
            if (w->status != CP5_RUN) ended++;
            trials++;
            /* the city ledger must agree with the cities themselves */
            int tally[CP5_MAX_NATIONS];
            memset(tally, 0, sizeof(tally));
            for (int i = 0; i < w->n_cities; i++)
                if (w->city[i].alive && w->city[i].owner >= 0) tally[w->city[i].owner]++;
            for (int n = 0; n < CP5_MAX_NATIONS; n++)
                if (tally[n] != w->nation[n].cities) consistent = 0;
        }
        CHECK(ended == trials, "civ: every episode reaches a terminal state");
        CHECK(consistent, "civ: the city ledger matches the cities on the map");
        free(w);
    }

    printf(fails ? "\n%d test(s) failed\n" : "\nall tests passed\n", fails);
    return fails ? 1 : 0;
}
