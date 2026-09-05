/* Dump the creature parts as STL.
 *
 *   ./build/cpore_stl --out docs/stl            # every part, plus a creature
 *   ./build/cpore_stl --part horn --out .       # just one
 *
 * The meshes are generated from the same signed distance fields the game
 * draws, so they are not a parallel set of assets that can drift - there is
 * nothing to keep in sync, because there is only one description of a horn
 * and this reads it.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cpore/land.h"

int main(int argc, char **argv)
{
    const char *out = ".";
    const char *only = NULL;
    int res = 0, style = CP4_STYLE_PREDATOR;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)       out = argv[++i];
        else if (!strcmp(argv[i], "--part") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--res") && i + 1 < argc)  res = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--style") && i + 1 < argc) style = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: %s [--out DIR] [--part NAME] [--res N]\n", argv[0]);
            return 1;
        }
    }

    char path[512];
    int total = 0, files = 0;

    for (int t = CP4_NONE + 1; t < CP4_PART_COUNT; t++) {
        const char *name = cp4_part_name(t);
        if (only && strcmp(only, name)) continue;
        snprintf(path, sizeof(path), "%s/part_%s.stl", out, name);
        int n = cp4_stl_part(path, t, res);
        if (n > 0) { printf("  %-10s %7d triangles  %s\n", name, n, path); total += n; files++; }
        else       printf("  %-10s FAILED\n", name);
    }

    if (!only) {
        Cp4Genome g;
        CpRng r;
        cp_rng_seed(&r, 7u);
        cp4_genome_autodesign(&g, &r, CP4_GEN_BUDGET[CP4_GENERATIONS - 1], style);
        snprintf(path, sizeof(path), "%s/creature.stl", out);
        int n = cp4_stl_creature(path, &g, res ? res : 128);
        if (n > 0) { printf("  %-10s %7d triangles  %s\n", "creature", n, path); total += n; files++; }
    }

    printf("%d files, %d triangles\n", files, total);
    return files ? 0 : 1;
}
