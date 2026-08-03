#include "cpore/land.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run the creature stage under the scripted baseline and write PNG stills.
 *
 *   ./build/cpore_land --seed 5 --steps 3000 --out land.png
 *   ./build/cpore_land --style charmer --every 600 --out frames.png
 *   ./build/cpore_land --gallery 2 --out gallery.png
 */


int main(int argc, char **argv)
{
    uint32_t seed = 5;
    int steps = 3000, W = 1280, H = 720, every = 0, vis = CP_VIS_ABYSS, gallery = 0, table = 0,
        climate = 0;
    const char *out = "land.png";
    Cp4Genome g;
    cp4_genome_starter(&g);
    int have = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)       seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   out = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gallery") && i + 1 < argc) gallery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--table"))                 table = 1;
        else if (!strcmp(argv[i], "--climate"))               climate = 1;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)  sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--vis") && i + 1 < argc) {
            const char *nm = argv[++i];
            for (int k = 0; k < CP_VIS_COUNT; k++) if (!strcmp(nm, cp_vis_name(k))) vis = k;
        } else if (!strcmp(argv[i], "--list-parts")) {
            printf("land parts (type  name  dna):\n");
            for (int t = 1; t < CP4_PART_COUNT; t++)
                printf("  %2d  %-8s %3d\n", t, cp4_part_name(t), cp4_part_cost(t));
            return 0;
        } else if (!strcmp(argv[i], "--style") && i + 1 < argc) {
            const char *nm = argv[++i];
            int st = 0;
            for (int k = 0; k < CP4_STYLE_COUNT; k++) if (!strcmp(nm, cp4_style_name(k))) st = k;
            cp4_genome_autodesign(&g, NULL, CP4_GEN_BUDGET[0], st);
            have = 1;
        } else {
            printf("usage: cpore_land [--seed N] [--steps N] [--out F] [--size WxH]\n"
                   "                  [--every N] [--style NAME] [--gallery GEN]\n"
                   "                  [--vis NAME] [--list-parts] [--table]\n"
                   "  styles: grazer predator charmer swimmer flyer burrower\n");
            return 1;
        }
    }

    /* --climate: what the world is actually made of. A biome field that is
     * ninety percent one biome is a uniform world with extra code in it, so
     * this prints the mix and a coarse map to look at. */
    if (climate) {
        long hist[CP4_BIOME_COUNT];
        long sea = 0, total = 0;
        memset(hist, 0, sizeof(hist));
        const int N = 64;
        const float SPAN = 9000.0f;
        for (int gy = 0; gy < N; gy++) {
            for (int gx = 0; gx < N; gx++) {
                float x = ((float)gx / N - 0.5f) * SPAN + CP4_W * 0.5f;
                float z = ((float)gy / N - 0.5f) * SPAN + CP4_D * 0.5f;
                total++;
                if (cp4_height(seed, x, z) > CP4_SEA) { sea++; putchar('~'); continue; }
                int b = cp4_biome(seed, x, z);
                hist[b]++;
                putchar("IUTFGSDJ"[b]);
            }
            putchar('\n');
        }
        printf("\nseed %u over %.0f units square:\n", seed, (double)SPAN);
        printf("  water  %4.1f%%\n", 100.0 * (double)sea / (double)total);
        for (int b = 0; b < CP4_BIOME_COUNT; b++)
            printf("  %-8s %4.1f%%  fertility %.2f\n", cp4_biome_name(b),
                   100.0 * (double)hist[b] / (double)total, (double)cp4_fertility(b));
        return 0;
    }

    /* --table: every archetype against the same seeds. The question this
     * answers is whether the four media are four ways to live or one way plus
     * three decorations - if a medium never shows up in the step counts, or
     * shows up but feeds nobody, it is not pulling its weight. */
    if (table) {
        Cp4World *tw = (Cp4World *)malloc(sizeof(Cp4World));
        if (!tw) { fprintf(stderr, "oom\n"); return 1; }
        printf("  %-9s  %-5s %-4s  %-26s  %s\n", "style", "evolv", "died",
               "steps grnd/watr/air/undr", "ate bush/kelp/tuber/meat");
        for (int st = 0; st < CP4_STYLE_COUNT; st++) {
            int evolved = 0, died = 0, nests = 0, hatch = 0, disc = 0, songs = 0;
            long med[CP4_MEDIUM_COUNT] = { 0, 0, 0, 0 };
            long ate[4] = { 0, 0, 0, 0 };
            float far = 0.0f, dna = 0.0f;
            const int NS = 12;
            for (int sd = 0; sd < NS; sd++) {
                Cp4Genome sg;
                cp4_genome_autodesign(&sg, NULL, CP4_GEN_BUDGET[0], st);
                cp4_world_reset(tw, (uint32_t)(sd * 13 + 3), &sg);
                for (int t = 0; t < CP4_MAX_STEPS && tw->status == CP4_RUN; t++) {
                    float a[CP4_ACT_DIM];
                    cp4_policy_greedy(tw, a);
                    cp4_world_step(tw, a);
                }
                if (tw->status == CP4_EVOLVED) evolved++;
                if (tw->status == CP4_DEAD) died++;
                for (int m = 0; m < CP4_MEDIUM_COUNT; m++) med[m] += tw->medium_steps[m];
                ate[0] += tw->ate_plant; ate[1] += tw->ate_kelp;
                ate[2] += tw->ate_tuber; ate[3] += tw->ate_meat;
                nests += tw->home.alive ? 1 : 0;
                hatch += tw->hatchlings;
                disc  += tw->discovered;
                songs += tw->songs;
                far   += tw->far_from_start;
                dna   += tw->dna;
            }
            long tot = med[0] + med[1] + med[2] + med[3];
            if (tot < 1) tot = 1;
            printf("  %-9s  %2d/%-3d %-4d  %3ld%% %3ld%% %3ld%% %3ld%%%14s  %4ld %4ld %5ld %4ld"
                   "   dna %5.0f  nests %d hatch %-3d found %-3d sing %-4d roamed %.0f\n",
                   cp4_style_name(st), evolved, NS, died,
                   med[0] * 100 / tot, med[1] * 100 / tot,
                   med[2] * 100 / tot, med[3] * 100 / tot, "",
                   ate[0], ate[1], ate[2], ate[3],
                   (double)(dna / NS), nests, hatch, disc, songs, (double)(far / NS));
        }
        free(tw);
        return 0;
    }

    /* --gallery: a contact sheet of random genomes, so the variety the design
     * space actually produces can be judged rather than assumed. */
    if (gallery > 0) {
        const int cols = 4, rows = 2;
        const int tw = W / cols, th = H / rows;
        const int lw = tw / 4, lh = th / 4;
        uint8_t *tile = (uint8_t *)malloc((size_t)lw * lh * 4);
        uint8_t *fb2 = (uint8_t *)malloc((size_t)W * H * 4);
        if (!tile || !fb2) { fprintf(stderr, "oom\n"); return 1; }
        memset(fb2, 0, (size_t)W * H * 4);
        CpRng rng;
        cp_rng_seed(&rng, seed);
        int gi = gallery - 1;
        if (gi < 0) gi = 0;
        if (gi >= CP4_GENERATIONS) gi = CP4_GENERATIONS - 1;
        for (int i = 0; i < cols * rows; i++) {
            Cp4Genome rg;
            cp4_genome_random(&rg, &rng, CP4_GEN_BUDGET[gi]);
            cp4_render_portrait(&rg, tile, lw, lh, vis, (uint32_t)(seed + i * 7919));
            int ox = (i % cols) * tw, oy = (i / cols) * th;
            for (int y = 0; y < th; y++) {
                for (int x = 0; x < tw; x++) {
                    const uint8_t *sp = tile + 4 * ((size_t)(y / 4) * lw + (x / 4));
                    uint8_t *dp = fb2 + 4 * ((size_t)(oy + y) * W + (ox + x));
                    dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
                }
            }
            Cp4Stats st;
            cp4_genome_stats(&rg, &st);
            printf("  %d: %3d dna  %2d parts  %d seg  legs %d  charm %.2f  pattern %d\n",
                   i, cp4_genome_cost(&rg), st.n_parts, rg.nseg, st.n[CP4_LEG],
                   (double)st.charm, rg.pattern);
        }
        if (cp_png_write(out, fb2, W, H) != 0) { fprintf(stderr, "png failed\n"); return 1; }
        printf("gallery -> %s\n", out);
        free(tile); free(fb2);
        return 0;
    }

    Cp4World *w = (Cp4World *)malloc(sizeof(Cp4World));
    uint8_t *fb = (uint8_t *)malloc((size_t)W * H * 4);
    if (!w || !fb) { fprintf(stderr, "oom\n"); return 1; }

    cp4_world_reset(w, seed, have ? &g : NULL);

    float ret = 0.0f;
    char path[512];
    int shot = 0;
    for (int t = 0; t < steps; t++) {
        float act[CP4_ACT_DIM];
        cp4_policy_greedy(w, act);
        cp4_world_step(w, act);
        ret += w->reward;
        if (every > 0 && (t + 1) % every == 0) {
            const char *dot = strrchr(out, '.');
            snprintf(path, sizeof(path), "%.*s_%03d.png",
                     (int)(dot ? dot - out : (long)strlen(out)), out, shot++);
            cp4_render_styled(w, fb, W, H, vis);
            cp_png_write(path, fb, W, H);
            printf("  wrote %s\n", path);
        }
        if (w->status != CP4_RUN) break;
    }

    cp4_render_styled(w, fb, W, H, vis);
    if (cp_png_write(out, fb, W, H) != 0) { fprintf(stderr, "png write failed\n"); return 1; }

    static const char *st[] = { "RUNNING", "DEAD", "EVOLVED", "TIMEOUT" };
    printf("seed=%u steps=%d status=%s dna=%.1f hp=%.0f gen=%d return=%.1f\n",
           seed, w->step, st[w->status], (double)w->dna, (double)w->player.hp,
           w->generation + 1, (double)ret);
    printf("plants=%d meat=%d kills=%d songs=%d won=%d hits=%d -> %s\n",
           w->ate_plant, w->ate_meat, w->kills, w->songs, w->befriended,
           w->hits_taken, out);
    printf("population: n=%d births=%d deaths=%d meangen=%.1f legs=%.1f "
           "charm=%.2f parts=%.1f ally=%d foe=%d\n",
           w->pop, w->births, w->deaths, (double)w->mean_gen, (double)w->mean_legs,
           (double)w->mean_charm, (double)w->mean_parts, w->allies, w->enemies);
    free(w); free(fb);
    return 0;
}
