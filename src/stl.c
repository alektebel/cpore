/* Meshing the creature, for anything that cannot ray march.
 *
 * There is no mesh anywhere in this project and that is deliberate: bodies are
 * signed distance fields, which is what lets them be *generated* rather than
 * authored, and which sidesteps meshing, UVs, rigging and skinning entirely.
 * The roadmap lists a mesh pipeline under things not to do.
 *
 * This is not that. Nothing here feeds back into the simulation or the
 * renderer, and the field stays the source of truth. What this does is emit a
 * derived artifact for the tools that can only speak triangles - a slicer, a
 * DCC package, a viewer someone already has - which is the one direction the
 * dependency can safely run. Change a part's code and the STL regenerates;
 * edit the STL and nothing in cpore notices, because nothing in cpore reads it.
 *
 * ---- why tetrahedra ----
 *
 * Marching cubes needs a 256-entry case table and an edge table beside it,
 * which is a few hundred lines of data that has to be transcribed exactly and
 * cannot be checked by reading. Marching tetrahedra splits each cell into six
 * tets, and a tet has four corners and therefore three cases: nothing, one
 * corner cut off, or a quad through the middle. That is a page of code with no
 * table in it. It emits perhaps twice the triangles for the same surface,
 * which for an export path nobody is rendering in real time is a trade worth
 * making every time.
 *
 * Winding is decided by measurement rather than by case analysis: each
 * triangle's normal is compared against the field's own gradient at its
 * centroid and flipped if they disagree. That is one dot product per triangle
 * and it is right by construction, where a hand-derived winding table is right
 * only if every one of its entries is.
 */

#include "cpore/land.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sdfbody.h"
#include "landbody.h"

/* ------------------------------------------------------------------ *
 * triangle sink
 * ------------------------------------------------------------------ */

typedef struct {
    float *v;        /* 9 floats per triangle */
    int    n, cap;
} TriBuf;

static int tri_push(TriBuf *t, V3 a, V3 b, V3 c)
{
    if (t->n == t->cap) {
        int cap = t->cap ? t->cap * 2 : 4096;
        float *v = (float *)realloc(t->v, sizeof(float) * 9 * (size_t)cap);
        if (!v) return 0;
        t->v = v; t->cap = cap;
    }
    float *p = t->v + 9 * (size_t)t->n;
    p[0] = a.x; p[1] = a.y; p[2] = a.z;
    p[3] = b.x; p[4] = b.y; p[5] = b.z;
    p[6] = c.x; p[7] = c.y; p[8] = c.z;
    t->n++;
    return 1;
}

/* ------------------------------------------------------------------ *
 * marching tetrahedra
 * ------------------------------------------------------------------ */

/* Where the surface crosses an edge, by linear interpolation on the field.
 * The field is a distance, so linear is the right interpolant here rather than
 * merely a convenient one - halfway between +1 and -1 really is the surface. */
static V3 edge_cut(V3 pa, float da, V3 pb, float db)
{
    float t = (da - db) != 0.0f ? da / (da - db) : 0.5f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return add(pa, mul(sub(pb, pa), t));
}

/* Emit for one tetrahedron. Corners are ordered so that `in` counts how many
 * are inside; the three cases are nothing, a corner cut off, and a quad. */
static void march_tet(TriBuf *out, const Prim *pr, int n,
                      V3 p[4], float d[4])
{
    int idx = 0;
    for (int i = 0; i < 4; i++) if (d[i] < 0.0f) idx |= 1 << i;
    if (idx == 0 || idx == 15) return;

    /* Sort corners so the inside ones come first. Doing this once removes the
     * sixteen-case switch that would otherwise be needed. */
    int in[4], outv[4], ni = 0, no = 0;
    for (int i = 0; i < 4; i++) {
        if (d[i] < 0.0f) in[ni++] = i;
        else             outv[no++] = i;
    }

    V3 tri[6];
    int ntri = 0;
    if (ni == 1 || ni == 3) {
        /* One corner on its own side: a single triangle across the three
         * edges that leave it. */
        int a = (ni == 1) ? in[0] : outv[0];
        const int *other = (ni == 1) ? outv : in;
        tri[0] = edge_cut(p[a], d[a], p[other[0]], d[other[0]]);
        tri[1] = edge_cut(p[a], d[a], p[other[1]], d[other[1]]);
        tri[2] = edge_cut(p[a], d[a], p[other[2]], d[other[2]]);
        ntri = 1;
    } else {
        /* Two and two: a quad through the four edges that join them, split
         * into two triangles. The ordering here keeps the quad convex. */
        V3 q0 = edge_cut(p[in[0]], d[in[0]], p[outv[0]], d[outv[0]]);
        V3 q1 = edge_cut(p[in[0]], d[in[0]], p[outv[1]], d[outv[1]]);
        V3 q2 = edge_cut(p[in[1]], d[in[1]], p[outv[1]], d[outv[1]]);
        V3 q3 = edge_cut(p[in[1]], d[in[1]], p[outv[0]], d[outv[0]]);
        tri[0] = q0; tri[1] = q1; tri[2] = q2;
        tri[3] = q0; tri[4] = q2; tri[5] = q3;
        ntri = 2;
    }

    for (int t = 0; t < ntri; t++) {
        V3 a = tri[t * 3], b = tri[t * 3 + 1], c = tri[t * 3 + 2];
        V3 ab = sub(b, a), ac = sub(c, a);
        V3 nr = v3(ab.y * ac.z - ab.z * ac.y,
                   ab.z * ac.x - ab.x * ac.z,
                   ab.x * ac.y - ab.y * ac.x);
        if (dot(nr, nr) < 1e-16f) continue;      /* degenerate, drop it */

        /* Outward is whichever way the field increases. Measured, not
         * derived: one gradient beats a winding table that is only correct if
         * every entry of it is. */
        V3 mid = mul(add(add(a, b), c), 1.0f / 3.0f);
        V3 g = sdf_normal(pr, n, mid);
        if (dot(nr, g) < 0.0f) { V3 tmp = b; b = c; c = tmp; }
        tri_push(out, a, b, c);
    }
}

/* The six tetrahedra of a cube, all sharing the 0-7 body diagonal. Corner i
 * is at (i&1, (i>>1)&1, (i>>2)&1), so 0 and 7 are opposite. */
static const int TETS[6][4] = {
    { 0, 7, 1, 3 }, { 0, 7, 3, 2 }, { 0, 7, 2, 6 },
    { 0, 7, 6, 4 }, { 0, 7, 4, 5 }, { 0, 7, 5, 1 },
};

/* Mesh whatever the given primitives describe, over their own bounds.
 *
 * `res` is cells along the longest axis. The grid is padded by two cells so
 * the surface never touches the boundary - a field clipped at the edge of its
 * own box produces a mesh with a hole exactly where the box was, which is the
 * classic way to get an unprintable model out of a correct field. */
static int mesh_prims(TriBuf *out, const Prim *pr, int n, int res)
{
    if (n <= 0 || res < 8) return 0;

    V3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for (int i = 0; i < n; i++)
        for (int e = 0; e < 2; e++) {
            V3 p = e ? pr[i].b : pr[i].a;
            float r = (e ? pr[i].rb : pr[i].ra) + pr[i].k;
            if (p.x - r < lo.x) lo.x = p.x - r;
            if (p.y - r < lo.y) lo.y = p.y - r;
            if (p.z - r < lo.z) lo.z = p.z - r;
            if (p.x + r > hi.x) hi.x = p.x + r;
            if (p.y + r > hi.y) hi.y = p.y + r;
            if (p.z + r > hi.z) hi.z = p.z + r;
        }

    float span = hi.x - lo.x;
    if (hi.y - lo.y > span) span = hi.y - lo.y;
    if (hi.z - lo.z > span) span = hi.z - lo.z;
    if (span <= 1e-4f) return 0;
    float h = span / (float)res;

    lo = sub(lo, v3(h * 2.0f, h * 2.0f, h * 2.0f));
    hi = add(hi, v3(h * 2.0f, h * 2.0f, h * 2.0f));
    int nx = (int)((hi.x - lo.x) / h) + 1;
    int ny = (int)((hi.y - lo.y) / h) + 1;
    int nz = (int)((hi.z - lo.z) / h) + 1;
    if (nx < 2 || ny < 2 || nz < 2) return 0;

    /* Two slabs of samples, not the whole volume: the marcher only ever needs
     * z and z+1, and a 256-cube grid held whole is 67 million floats. */
    size_t slab = (size_t)nx * ny;
    float *d0 = (float *)malloc(sizeof(float) * slab);
    float *d1 = (float *)malloc(sizeof(float) * slab);
    if (!d0 || !d1) { free(d0); free(d1); return 0; }

    for (size_t i = 0; i < slab; i++) {
        int ix = (int)(i % (size_t)nx), iy = (int)(i / (size_t)nx);
        d0[i] = creature_sdf(pr, n, v3(lo.x + ix * h, lo.y + iy * h, lo.z),
                             NULL, NULL, NULL);
    }

    for (int iz = 0; iz + 1 < nz; iz++) {
        float z1 = lo.z + (iz + 1) * h;
        for (size_t i = 0; i < slab; i++) {
            int ix = (int)(i % (size_t)nx), iy = (int)(i / (size_t)nx);
            d1[i] = creature_sdf(pr, n, v3(lo.x + ix * h, lo.y + iy * h, z1),
                                 NULL, NULL, NULL);
        }
        for (int iy = 0; iy + 1 < ny; iy++)
            for (int ix = 0; ix + 1 < nx; ix++) {
                V3 cp[8];
                float cd[8];
                for (int c = 0; c < 8; c++) {
                    int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
                    cp[c] = v3(lo.x + (ix + dx) * h,
                               lo.y + (iy + dy) * h,
                               lo.z + (iz + dz) * h);
                    cd[c] = (dz ? d1 : d0)[(size_t)(iy + dy) * nx + (ix + dx)];
                }
                for (int t = 0; t < 6; t++) {
                    V3 tp[4];
                    float td[4];
                    for (int k = 0; k < 4; k++) {
                        tp[k] = cp[TETS[t][k]];
                        td[k] = cd[TETS[t][k]];
                    }
                    march_tet(out, pr, n, tp, td);
                }
            }
        float *sw = d0; d0 = d1; d1 = sw;
    }

    free(d0);
    free(d1);
    return out->n;
}

/* ------------------------------------------------------------------ *
 * writing it out
 * ------------------------------------------------------------------ */

static void put_u32(FILE *f, uint32_t v)
{
    unsigned char b[4] = { (unsigned char)(v), (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fwrite(b, 1, 4, f);
}

/* Binary STL. The format has no colour, no units and no materials - it is
 * triangles and nothing else - so anything a part knows about its own hue is
 * lost here by the format's own design, not by this code being lazy. A caller
 * that wants colour wants OBJ+MTL or glTF, which is a different exporter. */
static int stl_write(const char *path, const TriBuf *t, const char *note)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;

    char head[80];
    memset(head, 0, sizeof(head));
    snprintf(head, sizeof(head), "cpore %s", note ? note : "");
    fwrite(head, 1, 80, f);
    put_u32(f, (uint32_t)t->n);

    for (int i = 0; i < t->n; i++) {
        const float *p = t->v + 9 * (size_t)i;
        V3 a = v3(p[0], p[1], p[2]), b = v3(p[3], p[4], p[5]), c = v3(p[6], p[7], p[8]);
        V3 ab = sub(b, a), ac = sub(c, a);
        V3 nr = norm(v3(ab.y * ac.z - ab.z * ac.y,
                        ab.z * ac.x - ab.x * ac.z,
                        ab.x * ac.y - ab.y * ac.x));
        float rec[12] = { nr.x, nr.y, nr.z,
                          a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z };
        fwrite(rec, sizeof(float), 12, f);
        unsigned char attr[2] = { 0, 0 };
        fwrite(attr, 1, 2, f);
    }
    fclose(f);
    return 1;
}

/* Move a set of primitives so their bounds are centred on the origin and the
 * body sits on z = 0. A parts library whose entries each arrive at whatever
 * offset they were mounted at is a library nobody can use. */
static void recentre(Prim *pr, int n)
{
    V3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for (int i = 0; i < n; i++)
        for (int e = 0; e < 2; e++) {
            V3 p = e ? pr[i].b : pr[i].a;
            float r = (e ? pr[i].rb : pr[i].ra) + pr[i].k;
            if (p.x - r < lo.x) lo.x = p.x - r;
            if (p.y - r < lo.y) lo.y = p.y - r;
            if (p.z - r < lo.z) lo.z = p.z - r;
            if (p.x + r > hi.x) hi.x = p.x + r;
            if (p.y + r > hi.y) hi.y = p.y + r;
            if (p.z + r > hi.z) hi.z = p.z + r;
        }
    V3 c = mul(add(lo, hi), 0.5f);
    for (int i = 0; i < n; i++) {
        pr[i].a = sub(pr[i].a, c);
        pr[i].b = sub(pr[i].b, c);
    }
}

/* ------------------------------------------------------------------ *
 * public
 * ------------------------------------------------------------------ */

int cp4_stl_creature(const char *path, const Cp4Genome *g, int res)
{
    if (!path || !g) return 0;
    static Prim pr[MAX_PRIM];
    Cp4Beast b;
    memset(&b, 0, sizeof(b));
    b.g = *g;
    cp4_genome_stats(&b.g, &b.s);
    b.hp = b.hp_max = b.s.hp_max;
    b.alive = 1;
    b.p.x = 0.0f; b.p.y = -b.s.stand; b.p.z = 0.0f;

    V3 centre;
    float bound;
    Skin sk;
    int n = build_prims4(&b, 0, pr, &centre, &bound, &sk);
    if (n <= 0) return 0;
    recentre(pr, n);

    TriBuf t;
    memset(&t, 0, sizeof(t));
    int ntri = mesh_prims(&t, pr, n, res > 0 ? res : 80);
    int ok = ntri > 0 && stl_write(path, &t, "creature");
    free(t.v);
    return ok ? ntri : 0;
}

/* One part, on its own, at the size and shape the builder gives it.
 *
 * The part is mounted on a minimal body and then the trunk is discarded,
 * because a part's geometry is decided by where it sits: a leg reaches the
 * ground from the flank it sprouts on, and asking the builder for a leg in
 * isolation would either be a second code path or a lie. Building the whole
 * animal and keeping one slot's primitives means the exported part is exactly
 * the part the game draws.
 */
int cp4_stl_part(const char *path, int part_type, int res)
{
    if (!path || part_type <= CP4_NONE || part_type >= CP4_PART_COUNT) return 0;

    Cp4Genome g;
    cp4_genome_clear(&g);
    cp4_genome_spine(&g, 4, 150);
    int slot = cp4_genome_place(&g, part_type, 2, 64, 0, 0,
                                CP4_GEN_BUDGET[CP4_GENERATIONS - 1]);
    if (slot < 0) return 0;

    static Prim pr[MAX_PRIM];
    Cp4Beast b;
    memset(&b, 0, sizeof(b));
    b.g = g;
    cp4_genome_stats(&b.g, &b.s);
    b.hp = b.hp_max = b.s.hp_max;
    b.alive = 1;
    b.p.x = 0.0f; b.p.y = -b.s.stand; b.p.z = 0.0f;

    V3 centre;
    float bound;
    Skin sk;
    int n = build_prims4(&b, 0, pr, &centre, &bound, &sk);
    if (n <= 0) return 0;

    /* Keep only what this slot pushed. */
    static Prim mine[MAX_PRIM];
    int m = 0;
    for (int i = 0; i < n; i++)
        if (pr[i].part == slot) mine[m++] = pr[i];
    if (m <= 0) return 0;
    recentre(mine, m);

    TriBuf t;
    memset(&t, 0, sizeof(t));
    /* 44 cells across the longest axis. The default was 72, which put a
     * hundred thousand triangles on an eyeball - a sphere - and made the parts
     * library forty megabytes. Grid resolution costs triangles quadratically
     * and buys detail linearly, so the useful setting is the coarsest one that
     * still resolves the smallest feature the part has. --res overrides it for
     * anyone who is actually printing. */
    int ntri = mesh_prims(&t, mine, m, res > 0 ? res : 44);
    int ok = ntri > 0 && stl_write(path, &t, cp4_part_name(part_type));
    free(t.v);
    return ok ? ntri : 0;
}
