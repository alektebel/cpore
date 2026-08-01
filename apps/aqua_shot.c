#include "cpore/aqua.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run the aquatic stage under the scripted baseline and write PNG stills.
 *
 *   ./build/cpore_aqua --seed 3 --steps 2400 --out aqua.png
 *   ./build/cpore_aqua --style hunter --every 400 --out frames.png
 */

static const char *STYLES[CP3_STYLE_COUNT] = { "grazer", "hunter", "diver" };

int main(int argc, char **argv)
{
    uint32_t seed = 3;
    int steps = 2400, W = 1280, H = 720, every = 0, vis = CP_VIS_ABYSS;
    const char *out = "aqua.png";
    Cp3Genome g;
    cp3_genome_starter(&g);
    int have = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)       seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   out = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)  sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--vis") && i + 1 < argc) {
            const char *nm = argv[++i];
            for (int k = 0; k < CP_VIS_COUNT; k++) if (!strcmp(nm, cp_vis_name(k))) vis = k;
        } else if (!strcmp(argv[i], "--list-parts")) {
            printf("aquatic parts (type  name  dna):\n");
            for (int t = 1; t < CP3_PART_COUNT; t++)
                printf("  %2d  %-8s %3d\n", t, cp3_part_name(t), cp3_part_cost(t));
            return 0;
        } else if (!strcmp(argv[i], "--style") && i + 1 < argc) {
            const char *nm = argv[++i];
            int st = 0;
            for (int k = 0; k < CP3_STYLE_COUNT; k++) if (!strcmp(nm, STYLES[k])) st = k;
            cp3_genome_autodesign(&g, NULL, CP3_GEN_BUDGET[0], st);
            have = 1;
        } else {
            printf("usage: cpore_aqua [--seed N] [--steps N] [--out F] [--size WxH]\n"
                   "                  [--every N] [--style grazer|hunter|diver]\n"
                   "                  [--vis NAME] [--list-parts]\n");
            return 1;
        }
    }

    Cp3World *w = (Cp3World *)malloc(sizeof(Cp3World));
    uint8_t *fb = (uint8_t *)malloc((size_t)W * H * 4);
    if (!w || !fb) { fprintf(stderr, "oom\n"); return 1; }

    cp3_world_reset(w, seed, have ? &g : NULL);

    float ret = 0.0f;
    char path[512];
    int shot = 0;
    for (int t = 0; t < steps; t++) {
        float act[CP3_ACT_DIM];
        cp3_policy_greedy(w, act);
        cp3_world_step(w, act);
        ret += w->reward;
        if (every > 0 && (t + 1) % every == 0) {
            const char *dot = strrchr(out, '.');
            snprintf(path, sizeof(path), "%.*s_%03d.png",
                     (int)(dot ? dot - out : (long)strlen(out)), out, shot++);
            cp3_render_styled(w, fb, W, H, vis);
            cp_png_write(path, fb, W, H);
            printf("  wrote %s\n", path);
        }
        if (w->status != CP3_RUN) break;
    }

    cp3_render_styled(w, fb, W, H, vis);
    if (cp_png_write(out, fb, W, H) != 0) { fprintf(stderr, "png write failed\n"); return 1; }

    static const char *st[] = { "RUNNING", "DEAD", "EVOLVED", "TIMEOUT" };
    printf("seed=%u steps=%d status=%s biomass=%.1f hp=%.0f gen=%d return=%.1f\n",
           seed, w->step, st[w->status], (double)w->biomass, (double)w->player.hp,
           w->generation + 1, (double)ret);
    printf("plankton=%d carrion=%d kills=%d hits=%d depth=%.0f  -> %s\n",
           w->ate_plankton, w->ate_carrion, w->kills, w->hits_taken,
           (double)w->player.p.y, out);
    printf("population: n=%d births=%d deaths=%d meangen=%.1f "
           "mouth=%.2f tail=%.2f lamp=%.2f parts=%.1f depth=%.0f\n",
           w->pop, w->births, w->deaths, (double)w->mean_gen, (double)w->mean_mouth,
           (double)w->mean_tail, (double)w->mean_light, (double)w->mean_parts,
           (double)w->mean_depth);
    free(w); free(fb);
    return 0;
}
