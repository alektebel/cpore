#include "cpore/space.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run the space stage under the scripted baseline and write PNG stills.
 *
 *   ./build/cpore_space --seed 4 --steps 3600 --out space.png
 *   ./build/cpore_space --legacy 0.85,0.85,1.55 --every 600 --out frames.png
 *   ./build/cpore_space --table            (all three doctrines x seeds)
 */

static const char *ST[] = { "RUNNING", "LOST", "WON", "TIMEOUT" };

static void run(uint32_t seed, const Cp7Legacy *lg, int steps, Cp7World *w,
                float *ret)
{
    cp7_world_reset(w, seed, lg);
    *ret = 0.0f;
    for (int t = 0; t < steps; t++) {
        float act[CP7_ACT_DIM];
        cp7_policy_greedy(w, act);
        cp7_world_step(w, act);
        *ret += w->reward;
        if (w->status != CP7_RUN) break;
    }
}

int main(int argc, char **argv)
{
    uint32_t seed = 4;
    int steps = CP7_MAX_STEPS, W = 1280, H = 720, every = 0, table = 0;
    const char *out = "space.png";
    Cp7Legacy lg;
    cp7_legacy_default(&lg);

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
            lg.bonus[CP7_COLONISE] = (float)a;
            lg.bonus[CP7_TRADE]    = (float)b;
            lg.bonus[CP7_ATTACK]   = (float)c;
        } else {
            printf("usage: cpore_space [--seed N] [--steps N] [--out F] [--size WxH]\n"
                   "                  [--every N] [--legacy COL,TRD,ATK] [--table]\n");
            return 1;
        }
    }

    Cp7World *w = (Cp7World *)malloc(sizeof(Cp7World));
    if (!w) { fprintf(stderr, "oom\n"); return 1; }

    /* --table: does the doctrine you evolved actually change how the galaxy is
     * grown? Three lopsided legacies against the same seeds, and the ledger
     * says which verb did the growing. */
    if (table) {
        const char *nm[3] = { "settler", "trader", "warlord" };
        for (int k = 0; k < 3; k++) {
            Cp7Legacy l;
            for (int b = 0; b < CP7_BONUS_COUNT; b++) l.bonus[b] = 0.85f;
            l.bonus[k] = 1.55f;
            int won = 0, lost = 0;
            int32_t settled = 0, flipped = 0, captured = 0, lostst = 0, runs = 0;
            float stars = 0.0f, money = 0.0f, tax = 0.0f, trade = 0.0f;
            for (int s = 0; s < 30; s++) {
                float ret;
                run((uint32_t)(s * 17 + 3), &l, CP7_MAX_STEPS, w, &ret);
                if (w->status == CP7_WON) won++;
                if (w->status == CP7_LOST) lost++;
                settled += w->settled; flipped += w->flipped;
                captured += w->captured; lostst += w->lost_stars;
                runs += w->trade_runs;
                stars += (float)w->empire[CP7_PLAYER].stars;
                money += w->empire[CP7_PLAYER].money;
                tax += w->tax_earned; trade += w->trade_earned;
            }
            printf("  %-8s won %2d/30 lost %2d  mean stars %.1f  "
                   "settled %3d bought %3d taken %3d lost %2d  hauls %4d  "
                   "tax %6.0f trade %6.0f -> money %.0f\n",
                   nm[k], won, lost, (double)(stars / 30.0f),
                   settled, flipped, captured, lostst, runs,
                   (double)(tax / 30.0f), (double)(trade / 30.0f),
                   (double)(money / 30.0f));
        }
        free(w);
        return 0;
    }

    uint8_t *fb = (uint8_t *)malloc((size_t)W * H * 4);
    if (!fb) { fprintf(stderr, "oom\n"); return 1; }

    cp7_world_reset(w, seed, &lg);
    float ret = 0.0f;
    char path[512];
    int shot = 0;
    for (int t = 0; t < steps; t++) {
        float act[CP7_ACT_DIM];
        cp7_policy_greedy(w, act);
        cp7_world_step(w, act);
        ret += w->reward;
        if (every > 0 && (t + 1) % every == 0) {
            const char *dot = strrchr(out, '.');
            snprintf(path, sizeof(path), "%.*s_%03d.png",
                     (int)(dot ? dot - out : (long)strlen(out)), out, shot++);
            cp7_render_styled(w, fb, W, H, 0);
            cp_png_write(path, fb, W, H);
            printf("  wrote %s\n", path);
        }
        if (w->status != CP7_RUN) break;
    }

    cp7_render_styled(w, fb, W, H, 0);
    if (cp_png_write(out, fb, W, H) != 0) { fprintf(stderr, "png write failed\n"); return 1; }

    printf("seed=%u steps=%d status=%s stars=%d/%d money=%.0f return=%.1f -> %s\n",
           seed, w->step, ST[w->status], w->empire[CP7_PLAYER].stars,
           w->n_stars, (double)w->empire[CP7_PLAYER].money, (double)ret, out);
    printf("settled=%d bought=%d taken=%d lost=%d hauls=%d pirate-raids cleared=%d\n",
           w->settled, w->flipped, w->captured, w->lost_stars, w->trade_runs,
           w->raids_fought);
    for (int n = 1; n < CP7_MAX_EMPIRES; n++)
        printf("  empire %d %-8s stars=%-2d rel=%+.2f %s\n", n,
               cp7_bonus_name(w->empire[n].style), w->empire[n].stars,
               (double)w->empire[n].rel, w->empire[n].alive ? "" : "(fallen)");
    free(w); free(fb);
    return 0;
}
