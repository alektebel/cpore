#include "spore/desc.h"

#include <stdio.h>
#include <stdlib.h>
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
    spine_init_blob(&d->spine);
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
    int n = d->spine.active ? d->spine.count : 0;
    if (n < 0) n = 0;
    if (n > SPINE_MAX_VERTS) n = SPINE_MAX_VERTS;
    fprintf(f, "spine %d\n", n);
    for (int i = 0; i < n; i++)
        fprintf(f, "v %.4f %.4f %.4f %.4f\n", d->spine.v[i].x, d->spine.v[i].y,
                d->spine.v[i].z, d->spine.v[i].radius);
    fclose(f);
    return 1;
}

int desc_load(CreatureDesc *d, const char *path) {
    if (!d || !path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    CreatureDesc out;
    desc_defaults(&out);
    int have_spine = 0, spine_n = -1, vi = 0;
    int g[16] = { 0 }, g2 = 0;
    int nfields = 0;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "name ", 5) == 0) {
            char *src = line + 5;
            char *nl = strchr(src, '\n');
            size_t len = nl ? (size_t)(nl - src) : strlen(src);
            if (len >= sizeof(out.name)) len = sizeof(out.name) - 1;
            memcpy(out.name, src, len);
            out.name[len] = '\0';
        } else if (sscanf(line, "genome2 %d", &g2) == 1) {
            out.genome.dtl_slide = g2;
        } else if (strncmp(line, "genome ", 7) == 0) {
            nfields = sscanf(line + 7,
                             "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                             &g[0], &g[1], &g[2], &g[3], &g[4], &g[5], &g[6],
                             &g[7], &g[8], &g[9], &g[10], &g[11], &g[12],
                             &g[13], &g[14], &g[15]);
            if (nfields >= 16) {
                out.genome.mouth = g[0];   out.genome.legs = g[1];
                out.genome.weapon = g[2];  out.genome.ability = g[3];
                out.genome.eyes = g[4];    out.genome.grasper = g[5];
                out.genome.detail = g[6];  out.genome.spine = g[7];
                out.genome.neck = g[8];    out.genome.asym = g[9];
                out.genome.stance = g[10]; out.genome.wpn_sock = g[11];
                out.genome.dtl_sock = g[12]; out.genome.reach = g[13];
                out.genome.limbs = g[14];  out.genome.wpn_slide = g[15];
            }
        } else if (sscanf(line, "color %f %f %f", &out.color.x, &out.color.y,
                          &out.color.z) == 3) {
        } else if (sscanf(line, "color2 %f %f %f", &out.color2.x, &out.color2.y,
                          &out.color2.z) == 3) {
        } else if (sscanf(line, "pattern %d", &out.pattern) == 1) {
        } else if (sscanf(line, "scale %f", &out.scale) == 1) {
        } else if (sscanf(line, "spine %d", &spine_n) == 1) {
            if (spine_n >= 0 && spine_n <= SPINE_MAX_VERTS) {
                out.spine.active = spine_n > 0 ? 1 : 0;
                out.spine.count = spine_n;
                have_spine = 1;
            }
        } else if (line[0] == 'v' && line[1] == ' ') {
            float x, y, z, r;
            if (have_spine && vi < spine_n &&
                sscanf(line + 2, "%f %f %f %f", &x, &y, &z, &r) == 4) {
                out.spine.v[vi].x = x;
                out.spine.v[vi].y = y;
                out.spine.v[vi].z = z;
                out.spine.v[vi].radius = r;
                vi++;
            }
        }
    }
    fclose(f);

    genome_clamp(&out.genome);
    if (out.scale < 0.4f) out.scale = 0.4f;
    if (out.spine.count < SPINE_MIN_VERTS) {          /* keep a valid body */
        spine_init_blob(&out.spine);
    }
    *d = out;
    return 1;
}
