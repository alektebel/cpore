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

static const char *STYLES[CP4_STYLE_COUNT] = { "grazer", "predator", "charmer" };

int main(int argc, char **argv)
{
    uint32_t seed = 5;
    int steps = 3000, W = 1280, H = 720, every = 0, vis = CP_VIS_ABYSS, gallery = 0;
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
            for (int k = 0; k < CP4_STYLE_COUNT; k++) if (!strcmp(nm, STYLES[k])) st = k;
            cp4_genome_autodesign(&g, NULL, CP4_GEN_BUDGET[0], st);
            have = 1;
        } else {
            printf("usage: cpore_land [--seed N] [--steps N] [--out F] [--size WxH]\n"
                   "                  [--every N] [--style grazer|predator|charmer]\n"
                   "                  [--gallery GEN] [--vis NAME] [--list-parts]\n");
            return 1;
        }
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
