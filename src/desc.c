#include "spore/desc.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

void desc_defaults(CreatureDesc *d) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "Blobert");
    d->genome = genome_starter();
    d->color = v3(0.92f, 0.78f, 0.55f);
    d->color2 = v3(0.95f, 0.88f, 0.72f);
    d->pattern = PATTERN_NONE;
    d->scale = 1.0f;
}

/* Create every parent directory of `path` (mkdir -p), so the default
 * data/creatures/ output lands even from a clean checkout. */
static void ensure_parent_dir(const char *path) {
    char buf[512];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            mkdir(buf, 0755);
            buf[i] = '/';
        }
    }
}

int desc_save(const CreatureDesc *d, const char *path) {
    if (!d || !path) return 0;
    ensure_parent_dir(path);
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    char name[24];
    snprintf(name, sizeof(name), "%s", d->name);
    fprintf(f, "cpore-creature 1\n");
    fprintf(f, "name %s\n", name);
    fprintf(f, "genome %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
            d->genome.mouth, d->genome.legs, d->genome.weapon,
            d->genome.ability, d->genome.eyes, d->genome.grasper,
            d->genome.detail, d->genome.spine, d->genome.neck,
            d->genome.asym, d->genome.stance, d->genome.wpn_sock,
            d->genome.dtl_sock, d->genome.reach, d->genome.limbs,
            d->genome.wpn_slide);
    fprintf(f, "genome2 %d\n", d->genome.dtl_slide);
    fprintf(f, "color %.4f %.4f %.4f\n", d->color.x, d->color.y, d->color.z);
    fprintf(f, "color2 %.4f %.4f %.4f\n", d->color2.x, d->color2.y, d->color2.z);
    fprintf(f, "pattern %d\n", d->pattern);
    fprintf(f, "scale %.4f\n", d->scale);
    fclose(f);
    return 1;
}
