#include "cpore/civ.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run the civilisation stage under the scripted baseline and write PNG stills.
 *
 *   ./build/cpore_civ --seed 4 --steps 2000 --out civ.png
 *   ./build/cpore_civ --legacy 1.5,0.9,0.9 --every 400 --out frames.png
 *   ./build/cpore_civ --table            (all three species biases x seeds)
 */

static const char *ST[] = { "RUNNING", "LOST", "WON", "TIMEOUT" };

static void run(uint32_t seed, const Cp5Legacy *lg, int steps, Cp5World *w, float *ret)
{
    cp5_world_reset(w, seed, lg);
    *ret = 0.0f;
    for (int t = 0; t < steps; t++) {
        float act[CP5_ACT_DIM];
        cp5_policy_greedy(w, act);
        cp5_world_step(w, act);
        *ret += w->reward;
        if (w->status != CP5_RUN) break;
    }
}

int main(int argc, char **argv)
{
    uint32_t seed = 4;
    int steps = CP5_MAX_STEPS, W = 1280, H = 720, every = 0, vis = CP_VIS_ABYSS, table = 0;
    const char *out = "civ.png";
    Cp5Legacy lg;
    cp5_legacy_default(&lg);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)       seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   out = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--table"))                 table = 1;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)  sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--legacy") && i + 1 < argc) {
            double a = 1.0, b = 1.0, c = 1.0;
            sscanf(argv[++i], "%lf,%lf,%lf", &a, &b, &c);
            lg.bonus[CP5_MIL] = (float)a; lg.bonus[CP5_ECO] = (float)b; lg.bonus[CP5_REL] = (float)c;
        } else if (!strcmp(argv[i], "--vis") && i + 1 < argc) {
            const char *nm = argv[++i];
            for (int k = 0; k < CP_VIS_COUNT; k++) if (!strcmp(nm, cp_vis_name(k))) vis = k;
        } else {
            printf("usage: cpore_civ [--seed N] [--steps N] [--out F] [--size WxH]\n"
                   "                 [--every N] [--legacy MIL,ECO,REL] [--vis NAME]\n"
                   "                 [--table]\n");
            return 1;
        }
    }

    Cp5World *w = (Cp5World *)malloc(sizeof(Cp5World));
    if (!w) { fprintf(stderr, "oom\n"); return 1; }

    /* --table: does the species you evolved actually change how the stage is
     * won? Three lopsided legacies against the same seeds. */
    if (table) {
        const char *nm[3] = { "warlike", "trading", "devout" };
        for (int k = 0; k < 3; k++) {
            Cp5Legacy l;
            for (int a = 0; a < CP5_APPROACH_COUNT; a++) l.bonus[a] = 0.85f;
            l.bonus[k] = 1.55f;
            int won = 0, lost = 0;
            int by[3] = { 0, 0, 0 }, un[3] = { 0, 0, 0 };
            float cities = 0.0f;
            for (int s = 0; s < 12; s++) {
                float ret;
                run((uint32_t)(s * 17 + 3), &l, CP5_MAX_STEPS, w, &ret);
                if (w->status == CP5_WON) won++;
                if (w->status == CP5_LOST) lost++;
                by[0] += w->captured; by[1] += w->bought; by[2] += w->converted;
                for (int a = 0; a < CP5_APPROACH_COUNT; a++) un[a] += w->units_built[a];
                cities += (float)w->nation[CP5_PLAYER].cities;
            }
            printf("  %-8s won %2d/12 lost %2d  mean cities %.1f  "
                   "cities taken by force/trade/faith %2d/%2d/%2d  units %2d/%2d/%2d\n",
                   nm[k], won, lost, (double)(cities / 12.0f), by[0], by[1], by[2],
                   un[0], un[1], un[2]);
        }
        free(w);
        return 0;
    }

    uint8_t *fb = (uint8_t *)malloc((size_t)W * H * 4);
    if (!fb) { fprintf(stderr, "oom\n"); return 1; }

    cp5_world_reset(w, seed, &lg);
    float ret = 0.0f;
    char path[512];
    int shot = 0;
    for (int t = 0; t < steps; t++) {
        float act[CP5_ACT_DIM];
        cp5_policy_greedy(w, act);
        cp5_world_step(w, act);
        ret += w->reward;
        if (every > 0 && (t + 1) % every == 0) {
            const char *dot = strrchr(out, '.');
            snprintf(path, sizeof(path), "%.*s_%03d.png",
                     (int)(dot ? dot - out : (long)strlen(out)), out, shot++);
            cp5_render_styled(w, fb, W, H, vis);
            cp_png_write(path, fb, W, H);
            printf("  wrote %s\n", path);
        }
        if (w->status != CP5_RUN) break;
    }

    cp5_render_styled(w, fb, W, H, vis);
    if (cp_png_write(out, fb, W, H) != 0) { fprintf(stderr, "png write failed\n"); return 1; }

    printf("seed=%u steps=%d status=%s cities=%d/%d gold=%d return=%.1f\n",
           seed, w->step, ST[w->status], w->nation[CP5_PLAYER].cities, w->n_cities,
           (int)w->nation[CP5_PLAYER].money, (double)ret);
    printf("taken by force=%d trade=%d faith=%d, lost=%d  units m/e/r=%d/%d/%d -> %s\n",
           w->captured, w->bought, w->converted, w->cities_lost,
           w->units_built[CP5_MIL], w->units_built[CP5_ECO], w->units_built[CP5_REL], out);
    for (int n = 0; n < CP5_MAX_NATIONS; n++)
        printf("  nation %d %-9s cities=%-2d gold=%-5d %s\n", n,
               cp5_approach_name(w->nation[n].style), w->nation[n].cities,
               (int)w->nation[n].money, w->nation[n].alive ? "" : "(fallen)");
    free(w); free(fb);
    return 0;
}
