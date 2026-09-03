/* Run the tribe stage under the scripted baseline and write PNG stills.
 *
 *   ./build/cpore_tribe --seed 5 --steps 3600 --out tribe.png
 *   ./build/cpore_tribe --table --seeds 12   (charm vs raid over seeds)
 */
#include "cpore/tribe.h"
#include "cpore/civ.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    uint32_t seed = 5;
    int steps = 3600, W = 1280, H = 720, table = 0, nseeds = 12;
    const char *out = "tribe.png";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)       seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   out = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)  sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--table"))                 table = 1;
        else if (!strcmp(argv[i], "--seeds") && i + 1 < argc) nseeds = atoi(argv[++i]);
        else {
            printf("usage: cpore_tribe [--seed N] [--steps N] [--out F] [--size WxH]\n"
                   "                   [--table] [--seeds N]\n");
            return 1;
        }
    }

    /* --table: charm-only vs raid-only founders, the fork in numbers */
    if (table) {
        int allied = 0, razed = 0, won_c = 0, won_r = 0;
        Cp6World *w = (Cp6World *)malloc(sizeof(Cp6World));
        for (int s = 0; s < nseeds; s++) {
            Cp4Genome devout, warlike;
            float act[CP6_ACT_DIM];
            cp4_genome_autodesign(&devout, NULL, CP4_GEN_BUDGET[0], CP4_STYLE_CHARMER);
            cp6_world_reset(w, (uint32_t)(s * 23 + 4), &devout, NULL, 0);
            memset(act, 0, sizeof(act));
            act[0] = 0.6f; act[1] = 0.2f; act[2] = 0.2f;
            for (int i = 1; i < CP6_MAX_TRIBES; i++) act[2 + i] = 0.8f;
            for (int t = 0; t < CP6_MAX_STEPS && w->status == CP6_RUN; t++)
                cp6_world_step(w, act);
            allied += w->allied;
            if (w->status == CP6_WON) won_c++;

            cp4_genome_autodesign(&warlike, NULL, CP4_GEN_BUDGET[0], CP4_STYLE_PREDATOR);
            cp6_world_reset(w, (uint32_t)(s * 23 + 4), &warlike, NULL, 0);
            memset(act, 0, sizeof(act));
            act[0] = 0.5f; act[1] = 0.35f; act[2] = 0.15f;
            for (int i = 1; i < CP6_MAX_TRIBES; i++) act[2 + i] = -0.8f;
            for (int t = 0; t < CP6_MAX_STEPS && w->status == CP6_RUN; t++)
                cp6_world_step(w, act);
            razed += w->razed;
            if (w->status == CP6_WON) won_r++;
        }
        printf("tribe fork over %d seeds:\n", nseeds);
        printf("  charm: %d allied, won %d/%d\n", allied, won_c, nseeds);
        printf("  raid:  %d razed,  won %d/%d\n", razed, won_r, nseeds);
        free(w);
        return 0;
    }

    {
        Cp6World *w = (Cp6World *)malloc(sizeof(Cp6World));
        uint8_t *fb = (uint8_t *)malloc((size_t)W * H * 4);
        if (!w || !fb) { printf("oom\n"); return 1; }
        cp6_world_reset(w, seed, NULL, NULL, 0);
        for (int t = 0; t < steps && w->status == CP6_RUN; t++) {
            float act[CP6_ACT_DIM];
            cp6_policy_greedy(w, act);
            cp6_world_step(w, act);
        }
        cp6_render(w, fb, W, H);
        if (cp_png_write(out, fb, W, H) != 0) { printf("write failed\n"); return 1; }
        printf("tribe: seed %u steps %d members %.0f allied %d razed %d -> %s\n",
               seed, w->step, (double)w->tribe[CP6_PLAYER].members,
               w->allied, w->razed, out);
        free(w); free(fb);
    }
    return 0;
}
