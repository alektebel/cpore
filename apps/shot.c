#include "cpore/cpore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run an episode under the scripted baseline and write PNG stills.
 *
 *   ./build/cpore_shot --seed 23 --steps 900 --out shot.png
 *   ./build/cpore_shot --style hunter --steps 2000 --out build/hunter.png
 *   ./build/cpore_shot --parts 2:0,7:16,4:112,4:144 --out build/custom.png
 *
 * --parts takes type:angle pairs. Types are 1..10 in the order listed by
 * --list-parts; angle is 0..255 clockwise from the front of the cell.
 */

static const char *STYLES[CP_STYLE_COUNT] = { "grazer", "hunter", "tank", "scout" };

static void usage(void)
{
    printf("usage: cpore_shot [--seed N] [--steps N] [--out FILE] [--size WxH]\n"
           "                  [--every N] [--style NAME] [--parts t:a,t:a,...]\n"
           "                  [--vis NAME] [--vis-all] [--list-parts]\n");
}

static void list_parts(void)
{
    printf("parts (type  name  dna cost):\n");
    for (int t = 1; t < CP_PART_COUNT; t++)
        printf("  %2d  %-10s %3d\n", t, cp_part_name(t), cp_part_cost(t));
    printf("generation budgets:");
    for (int i = 0; i < CP_GENERATIONS; i++) printf(" %d", CP_GEN_BUDGET[i]);
    printf("\n");
}

static void print_genome(const CpGenome *g)
{
    printf("genome (%d dna):", cp_genome_cost(g));
    for (int i = 0; i < CP_MAX_PARTS; i++) {
        if (g->part[i].type == CP_PART_NONE) continue;
        printf(" %s@%d", cp_part_name(g->part[i].type), g->part[i].angle);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    uint32_t seed = 23;
    int steps = 900, W = 1280, H = 720, every = 0;
    const char *out = "shot.png";
    CpGenome g;
    cp_genome_starter(&g);
    int have_genome = 0;
    int vis = CP_VIS_DROP, vis_all = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc)       seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)   out = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)  sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--list-parts")) { list_parts(); return 0; }
        else if (!strcmp(argv[i], "--vis-all")) vis_all = 1;
        else if (!strcmp(argv[i], "--vis") && i + 1 < argc) {
            const char *nm = argv[++i];
            vis = -1;
            for (int k = 0; k < CP_VIS_COUNT; k++)
                if (!strcmp(nm, cp_vis_name(k))) vis = k;
            if (vis < 0) {
                fprintf(stderr, "unknown vis style '%s'; have:", nm);
                for (int k = 0; k < CP_VIS_COUNT; k++) fprintf(stderr, " %s", cp_vis_name(k));
                fprintf(stderr, "\n");
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--style") && i + 1 < argc) {
            const char *nm = argv[++i];
            int style = 0;
            for (int k = 0; k < CP_STYLE_COUNT; k++) if (!strcmp(nm, STYLES[k])) style = k;
            cp_genome_autodesign(&g, NULL, CP_GEN_BUDGET[0], style);
            have_genome = 1;
        } else if (!strcmp(argv[i], "--parts") && i + 1 < argc) {
            cp_genome_clear(&g);
            char *spec = argv[++i];
            int slot = 0;
            for (char *tok = strtok(spec, ","); tok && slot < CP_MAX_PARTS; tok = strtok(NULL, ",")) {
                int t = 0, a = 0;
                if (sscanf(tok, "%d:%d", &t, &a) < 1) continue;
                if (t <= 0 || t >= CP_PART_COUNT) continue;
                g.part[slot].type = (uint8_t)t;
                g.part[slot].angle = (uint8_t)(a & 0xFF);
                slot++;
            }
            cp_genome_normalise(&g, CP_GEN_BUDGET[0]);
            have_genome = 1;
        } else { usage(); return 1; }
    }

    CpWorld *w = (CpWorld *)malloc(sizeof(CpWorld));
    uint8_t *fb = (uint8_t *)malloc((size_t)W * H * 4);
    if (!w || !fb) { fprintf(stderr, "oom\n"); return 1; }

    cp_world_reset(w, seed, have_genome ? &g : NULL);

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
            cp_render_styled(w, fb, W, H, vis);
            cp_png_write(path, fb, W, H);
            printf("  wrote %s\n", path);
        }
        if (w->status != CP_RUN) break;
    }

    /* --vis-all renders the identical terminal state in every style, which is
     * the only fair way to compare them. */
    if (vis_all) {
        const char *dot = strrchr(out, '.');
        int stem = (int)(dot ? dot - out : (long)strlen(out));
        for (int k = 0; k < CP_VIS_COUNT; k++) {
            cp_render_styled(w, fb, W, H, k);
            snprintf(path, sizeof(path), "%.*s_%s.png", stem, out, cp_vis_name(k));
            if (cp_png_write(path, fb, W, H) != 0) {
                fprintf(stderr, "png write failed: %s\n", path);
                return 1;
            }
            printf("  wrote %s\n", path);
        }
    }

    cp_render_styled(w, fb, W, H, vis);
    if (cp_png_write(out, fb, W, H) != 0) { fprintf(stderr, "png write failed\n"); return 1; }

    static const char *status[] = { "RUNNING", "DEAD", "EVOLVED", "TIMEOUT" };
    printf("seed=%u  steps=%d  status=%s  dna=%.1f  hp=%.1f  gen=%d  return=%.2f\n",
           seed, w->step, status[w->status], (double)w->dna, (double)w->player.hp,
           w->generation + 1, (double)ret);
    printf("plants=%d meat=%d kills=%d hits=%d zaps=%d  pos=(%.0f,%.0f)  ->  %s (%dx%d)\n",
           w->ate_plant, w->ate_meat, w->kills, w->hits_taken, w->discharges,
           (double)w->player.x, (double)w->player.y, out, W, H);
    print_genome(&w->genome);

    free(w); free(fb);
    return 0;
}
