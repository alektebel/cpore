#include "cpore/cpore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run an episode under the scripted baseline and write PNG stills.
 *
 *   ./build/cpore_shot --seed 7 --steps 900 --out shot.png
 *   ./build/cpore_shot --morph 2,1,2,3,0,0 --steps 1500 --out build/hero.png
 */

static void usage(void)
{
    printf("usage: cpore_shot [--seed N] [--steps N] [--out FILE]\n"
           "                  [--morph h,c,s,ci,f,e] [--size WxH] [--every N]\n");
}

int main(int argc, char **argv)
{
    uint32_t seed = 7;
    int steps = 900, W = 1280, H = 720, every = 0;
    const char *out = "shot.png";
    CpMorph m;
    cp_morph_default(&m);
    int have_morph = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)      seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   out = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)  sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--morph") && i + 1 < argc) {
            int v[6] = { 0, 0, 0, 0, 0, 0 };
            sscanf(argv[++i], "%d,%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
            m.herb = (uint8_t)v[0]; m.carn = (uint8_t)v[1]; m.spike = (uint8_t)v[2];
            m.cilia = (uint8_t)v[3]; m.flag = (uint8_t)v[4]; m.elec = (uint8_t)v[5];
            have_morph = 1;
        } else { usage(); return 1; }
    }
    cp_morph_clamp(&m);

    CpWorld *w = (CpWorld *)malloc(sizeof(CpWorld));
    uint8_t *fb = (uint8_t *)malloc((size_t)W * H * 4);
    if (!w || !fb) { fprintf(stderr, "oom\n"); return 1; }

    cp_world_reset(w, seed, have_morph ? &m : NULL);

    float ret = 0.0f;
    char path[512];
    int shot_n = 0;

    for (int t = 0; t < steps; t++) {
        float act[CP_ACT_DIM];
        cp_policy_greedy(w, act);
        cp_world_step(w, act);
        ret += w->reward;

        if (every > 0 && (t + 1) % every == 0) {
            snprintf(path, sizeof(path), "%.*s_%03d.png",
                     (int)(strrchr(out, '.') ? strrchr(out, '.') - out : (long)strlen(out)),
                     out, shot_n++);
            cp_render(w, fb, W, H);
            cp_png_write(path, fb, W, H);
            printf("  wrote %s\n", path);
        }
        if (w->status != CP_RUN) break;
    }

    cp_render(w, fb, W, H);
    if (cp_png_write(out, fb, W, H) != 0) { fprintf(stderr, "png write failed\n"); return 1; }

    static const char *status[] = { "RUNNING", "DEAD", "EVOLVED", "TIMEOUT" };
    printf("seed=%u  steps=%d  status=%s  dna=%.1f  hp=%.1f  return=%.2f\n",
           seed, w->step, status[w->status], (double)w->dna, (double)w->player.hp, (double)ret);
    printf("plants=%d meat=%d kills=%d hits=%d  pos=(%.0f,%.0f)  ->  %s (%dx%d)\n",
           w->ate_plant, w->ate_meat, w->kills, w->hits_taken,
           (double)w->player.x, (double)w->player.y, out, W, H);

    free(w); free(fb);
    return 0;
}
