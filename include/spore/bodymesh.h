#ifndef SPORE_BODYMESH_H
#define SPORE_BODYMESH_H

/* Procedural body mesh — algorithms ported from Daniel Lochner's
 * Creature Creator (MIT): skinned cylinder + hemisphere caps +
 * cosine blend-shape inflate along a bone/spine column.
 * Original: https://github.com/daniellochner/SPORE-Creature-Creator
 */

#include "spore/creature.h"
#include "spore/math3d.h"

#define BM_MAX_SEGMENTS 16
#define BM_MAX_RINGS_PER_BONE 6
#define BM_MAX_BONES SPINE_MAX_VERTS
#define BM_MAX_VERTS 4096
#define BM_MAX_IDX  12288

typedef struct {
    float radius;          /* rest cylinder radius */
    float length;          /* rest spacing between bones */
    int   segments;        /* radial subdivisions (4..16) */
    int   rings_per_bone;  /* rings along each bone segment */
} BodyMeshSettings;

typedef struct {
    int   count;
    float pos[BM_MAX_VERTS * 3];
    float nrm[BM_MAX_VERTS * 3];
    unsigned idx[BM_MAX_IDX];
    int   idx_count;
} BodyMesh;

void bodymesh_defaults(BodyMeshSettings *s);

/* Build + bake a mesh from a spine column (Lochner Setup()).
 * primary/secondary tint belly via Y-ish normal heuristic. */
void bodymesh_build(BodyMesh *out, const SpineColumn *spine,
                    const BodyMeshSettings *settings,
                    Vec3 primary, Vec3 secondary);

/* Lochner AddWeight / RemoveWeight: inflate selected bone and bleed
 * half to neighbours (dir: 0=both, -1=prev chain, +1=next chain). */
void bodymesh_add_weight(SpineColumn *s, int index, float delta, int dir);

#endif
