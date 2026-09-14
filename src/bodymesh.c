/* Body mesh generation — ported from Daniel Lochner's Creature Creator
 * (MIT License, Copyright 2020 Daniel Lochner).
 * See CreatureController.Setup() in the original Unity project.
 */

#include "spore/bodymesh.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void bodymesh_defaults(BodyMeshSettings *s) {
    s->radius = 0.32f;
    s->length = 0.28f;
    s->segments = 12;
    s->rings_per_bone = 3;
}

typedef struct {
    int i0, i1, i2;
    float w0, w1, w2;
} BoneW;

static Vec3 xform_point(Vec3 p, Vec3 origin, Vec3 right, Vec3 up, Vec3 fwd) {
    return v3_add(origin,
           v3_add(v3_mul(right, p.x),
           v3_add(v3_mul(up, p.y), v3_mul(fwd, p.z))));
}

void bodymesh_add_weight(SpineColumn *s, int index, float delta, int dir) {
    if (!s || !s->active || index < 0 || index >= s->count) return;
    s->v[index].radius = clampf(s->v[index].radius + delta, 0.12f, 1.20f);
    if (index > 0 && (dir == -1 || dir == 0))
        bodymesh_add_weight(s, index - 1, delta * 0.5f, -1);
    if (index < s->count - 1 && (dir == 1 || dir == 0))
        bodymesh_add_weight(s, index + 1, delta * 0.5f, 1);
}

void bodymesh_build(BodyMesh *out, const SpineColumn *spine,
                    const BodyMeshSettings *settings,
                    Vec3 primary, Vec3 secondary) {
    memset(out, 0, sizeof(*out));
    if (!spine || !spine->active || spine->count < SPINE_MIN_VERTS) return;

    BodyMeshSettings st;
    if (settings) st = *settings;
    else bodymesh_defaults(&st);
    if (st.segments < 4) st.segments = 4;
    if (st.segments > BM_MAX_SEGMENTS) st.segments = BM_MAX_SEGMENTS;
    if (st.rings_per_bone < 2) st.rings_per_bone = 2;
    if (st.rings_per_bone > BM_MAX_RINGS_PER_BONE) st.rings_per_bone = BM_MAX_RINGS_PER_BONE;

    int nbones = spine->count;
    if (nbones > BM_MAX_BONES) nbones = BM_MAX_BONES;

    /* --- Rest-pose topology (Lochner: top hemi + cylinder + bottom hemi) --- */
    float rest_pos[BM_MAX_VERTS * 3];
    BoneW weights[BM_MAX_VERTS];
    int nv = 0;

    /* Pole (nose tip) */
    rest_pos[0] = 0; rest_pos[1] = 0; rest_pos[2] = 0;
    weights[0] = (BoneW){0, 0, 0, 1, 0, 0};
    nv = 1;

    int hemi_rings = st.segments / 2;
    if (hemi_rings < 2) hemi_rings = 2;

    /* Top hemisphere rings (excluding pole) */
    for (int ring = 1; ring < hemi_rings; ring++) {
        float percent = (float)ring / (float)hemi_rings;
        float ring_r = st.radius * sinf(0.5f * (float)M_PI * percent);
        float ring_z = st.radius * (-cosf(0.5f * (float)M_PI * percent) + 1.0f);
        for (int i = 0; i < st.segments; i++) {
            if (nv >= BM_MAX_VERTS) break;
            float ang = (float)i * 360.0f / (float)st.segments * ((float)M_PI / 180.0f);
            rest_pos[nv * 3 + 0] = ring_r * cosf(ang);
            rest_pos[nv * 3 + 1] = ring_r * sinf(ang);
            rest_pos[nv * 3 + 2] = ring_z;
            weights[nv] = (BoneW){0, 0, 0, 1, 0, 0};
            nv++;
        }
    }

    /* Middle cylinder — rings_per_bone * nbones */
    for (int ring = 0; ring < st.rings_per_bone * nbones; ring++) {
        float bone_f = (float)ring / (float)st.rings_per_bone;
        int bone_i = (int)floorf(bone_f);
        if (bone_i >= nbones) bone_i = nbones - 1;
        float bone_pct = bone_f - (float)bone_i;

        int b0 = (bone_i > 0) ? bone_i - 1 : 0;
        int b1 = bone_i;
        int b2 = (bone_i < nbones - 1) ? bone_i + 1 : nbones - 1;
        float w0 = (bone_i > 0) ? (1.0f - bone_pct) * 0.5f : 0.0f;
        float w2 = (bone_i < nbones - 1) ? bone_pct * 0.5f : 0.0f;
        float w1 = 1.0f - (w0 + w2);

        for (int i = 0; i < st.segments; i++) {
            if (nv >= BM_MAX_VERTS) break;
            float ang = (float)i * 360.0f / (float)st.segments * ((float)M_PI / 180.0f);
            rest_pos[nv * 3 + 0] = st.radius * cosf(ang);
            rest_pos[nv * 3 + 1] = st.radius * sinf(ang);
            rest_pos[nv * 3 + 2] = st.radius + (float)ring * st.length / (float)st.rings_per_bone;
            weights[nv] = (BoneW){b0, b1, b2, w0, w1, w2};
            nv++;
        }
    }

    /* Bottom hemisphere */
    for (int ring = 0; ring < hemi_rings; ring++) {
        float percent = (float)ring / (float)hemi_rings;
        float ring_r = st.radius * cosf(0.5f * (float)M_PI * percent);
        float ring_z = st.radius * sinf(0.5f * (float)M_PI * percent);
        for (int i = 0; i < st.segments; i++) {
            if (nv >= BM_MAX_VERTS) break;
            float ang = (float)i * 360.0f / (float)st.segments * ((float)M_PI / 180.0f);
            rest_pos[nv * 3 + 0] = ring_r * cosf(ang);
            rest_pos[nv * 3 + 1] = ring_r * sinf(ang);
            rest_pos[nv * 3 + 2] = st.radius + st.length * (float)nbones + ring_z;
            weights[nv] = (BoneW){nbones - 1, 0, 0, 1, 0, 0};
            nv++;
        }
    }
    /* Tail pole */
    if (nv < BM_MAX_VERTS) {
        rest_pos[nv * 3 + 0] = 0;
        rest_pos[nv * 3 + 1] = 0;
        rest_pos[nv * 3 + 2] = 2.0f * st.radius + st.length * (float)nbones;
        weights[nv] = (BoneW){nbones - 1, 0, 0, 1, 0, 0};
        nv++;
    }

    /* Triangles */
    unsigned idx[BM_MAX_IDX];
    int ni = 0;
    for (int i = 0; i < st.segments && ni + 3 <= BM_MAX_IDX; i++) {
        int seam = (i != st.segments - 1) ? 0 : st.segments;
        idx[ni++] = 0;
        idx[ni++] = (unsigned)(i + 2 - seam);
        idx[ni++] = (unsigned)(i + 1);
    }
    int rings = (st.rings_per_bone * nbones) + (2 * (hemi_rings - 1));
    for (int ring = 0; ring < rings; ring++) {
        int off = 1 + ring * st.segments;
        for (int i = 0; i < st.segments && ni + 6 <= BM_MAX_IDX; i++) {
            int seam = (i != st.segments - 1) ? 0 : st.segments;
            idx[ni++] = (unsigned)(off + i);
            idx[ni++] = (unsigned)(off + i + 1 - seam);
            idx[ni++] = (unsigned)(off + i + 1 - seam + st.segments);
            idx[ni++] = (unsigned)(off + i + 1 - seam + st.segments);
            idx[ni++] = (unsigned)(off + i + st.segments);
            idx[ni++] = (unsigned)(off + i);
        }
    }
    int top = 1 + (rings + 1) * st.segments;
    if (top >= nv) top = nv - 1;
    for (int i = 0; i < st.segments && ni + 3 <= BM_MAX_IDX; i++) {
        int seam = (i != st.segments - 1) ? 0 : st.segments;
        idx[ni++] = (unsigned)top;
        idx[ni++] = (unsigned)(top - i - 2 + seam);
        idx[ni++] = (unsigned)(top - i - 1);
    }

    /* Rest bone centres along Z (Lochner bind pose) */
    Vec3 rest_bone[BM_MAX_BONES];
    for (int b = 0; b < nbones; b++)
        rest_bone[b] = v3(0, 0, st.radius + st.length * (0.5f + (float)b));

    /* Blend-shape deltas (Lochner cosine inflate) — size maps radius→0..100 */
    float size[BM_MAX_BONES];
    for (int b = 0; b < nbones; b++) {
        /* Map vertebra radius ~0.12..1.20 → blend weight 0..100 */
        size[b] = clampf((spine->v[b].radius - 0.12f) / (1.20f - 0.12f) * 100.0f, 0.0f, 100.0f);
    }

    float inflated[BM_MAX_VERTS * 3];
    memcpy(inflated, rest_pos, (size_t)nv * 3 * sizeof(float));
    for (int b = 0; b < nbones; b++) {
        float w = size[b] / 100.0f;
        if (w < 1e-4f) continue;
        float max_along = st.length * 2.0f;
        float max_above = st.radius * 2.0f;
        for (int vi = 0; vi < nv; vi++) {
            float vx = rest_pos[vi * 3 + 0];
            float vy = rest_pos[vi * 3 + 1];
            float vz = rest_pos[vi * 3 + 2];
            float along = vz - rest_bone[b].z;
            float x = clampf(along / max_along, -1.0f, 1.0f);
            float a = max_above;
            float bb = 1.0f / a;
            float height = (cosf(x * (float)M_PI) / bb + a) * 0.5f;
            float rl = sqrtf(vx * vx + vy * vy);
            if (rl < 1e-5f) continue;
            inflated[vi * 3 + 0] += (vx / rl) * height * w;
            inflated[vi * 3 + 1] += (vy / rl) * height * w;
        }
    }

    /* World bone frames from spine vertebrae (nose=+Z local in spine) */
    Vec3 bone_origin[BM_MAX_BONES];
    Vec3 bone_right[BM_MAX_BONES], bone_up[BM_MAX_BONES], bone_fwd[BM_MAX_BONES];
    for (int b = 0; b < nbones; b++) {
        bone_origin[b] = v3(spine->v[b].x, spine->v[b].y, spine->v[b].z);
    }
    for (int b = 0; b < nbones; b++) {
        Vec3 fwd;
        if (b < nbones - 1)
            fwd = v3_sub(bone_origin[b + 1], bone_origin[b]);
        else if (b > 0)
            fwd = v3_sub(bone_origin[b], bone_origin[b - 1]);
        else
            fwd = v3(0, 0, -1); /* nose→tail is -Z in our spine, +Z in Lochner rest */
        /* Flip: our spine nose is +Z, rest mesh grows +Z from nose — map rest +Z along spine nose→tail (-Z) */
        fwd = v3_norm(v3(-fwd.x, -fwd.y, -fwd.z));
        if (v3_len(fwd) < 0.5f) fwd = v3(0, 0, -1);
        Vec3 up_hint = v3(0, 1, 0);
        Vec3 right = v3_norm(v3_cross(up_hint, fwd));
        if (v3_len(v3_cross(up_hint, fwd)) < 1e-4f)
            right = v3_norm(v3_cross(v3(1, 0, 0), fwd));
        Vec3 up = v3_cross(fwd, right);
        bone_fwd[b] = fwd;
        bone_right[b] = right;
        bone_up[b] = up;
    }

    /* Skin: rest→world with bone weights (CPU bake of Lochner skinned mesh) */
    for (int vi = 0; vi < nv; vi++) {
        Vec3 local = v3(inflated[vi * 3], inflated[vi * 3 + 1], inflated[vi * 3 + 2]);
        BoneW bw = weights[vi];
        Vec3 accum = v3(0, 0, 0);
        float tw = 0.0f;
        int bi[3] = {bw.i0, bw.i1, bw.i2};
        float ww[3] = {bw.w0, bw.w1, bw.w2};
        for (int k = 0; k < 3; k++) {
            if (ww[k] < 1e-5f) continue;
            int b = bi[k];
            if (b < 0 || b >= nbones) continue;
            /* Rest offset from bind bone */
            Vec3 off = v3_sub(local, rest_bone[b]);
            /* Lochner rest grows +Z; our bone_fwd is nose→tail. Remap rest Z along fwd. */
            Vec3 skinned = xform_point(off, bone_origin[b],
                                       bone_right[b], bone_up[b], bone_fwd[b]);
            accum = v3_add(accum, v3_mul(skinned, ww[k]));
            tw += ww[k];
        }
        if (tw > 1e-5f) accum = v3_mul(accum, 1.0f / tw);
        else accum = local;
        out->pos[vi * 3 + 0] = accum.x;
        out->pos[vi * 3 + 1] = accum.y;
        out->pos[vi * 3 + 2] = accum.z;
    }

    /* Normals from triangles */
    memset(out->nrm, 0, (size_t)nv * 3 * sizeof(float));
    for (int t = 0; t + 2 < ni; t += 3) {
        int ia = (int)idx[t], ib = (int)idx[t + 1], ic = (int)idx[t + 2];
        if (ia >= nv || ib >= nv || ic >= nv) continue;
        Vec3 a = v3(out->pos[ia * 3], out->pos[ia * 3 + 1], out->pos[ia * 3 + 2]);
        Vec3 b = v3(out->pos[ib * 3], out->pos[ib * 3 + 1], out->pos[ib * 3 + 2]);
        Vec3 c = v3(out->pos[ic * 3], out->pos[ic * 3 + 1], out->pos[ic * 3 + 2]);
        Vec3 n = v3_cross(v3_sub(b, a), v3_sub(c, a));
        out->nrm[ia * 3] += n.x; out->nrm[ia * 3 + 1] += n.y; out->nrm[ia * 3 + 2] += n.z;
        out->nrm[ib * 3] += n.x; out->nrm[ib * 3 + 1] += n.y; out->nrm[ib * 3 + 2] += n.z;
        out->nrm[ic * 3] += n.x; out->nrm[ic * 3 + 1] += n.y; out->nrm[ic * 3 + 2] += n.z;
    }
    for (int vi = 0; vi < nv; vi++) {
        Vec3 n = v3_norm(v3(out->nrm[vi * 3], out->nrm[vi * 3 + 1], out->nrm[vi * 3 + 2]));
        out->nrm[vi * 3] = n.x;
        out->nrm[vi * 3 + 1] = n.y;
        out->nrm[vi * 3 + 2] = n.z;
        (void)primary;
        (void)secondary;
    }

    out->count = nv;
    out->idx_count = ni;
    memcpy(out->idx, idx, (size_t)ni * sizeof(unsigned));
}
