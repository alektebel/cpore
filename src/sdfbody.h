/* Shared SDF body core - internal to src/, not part of the public API.
 *
 * A creature in this project is a list of round cones (tapered capsules)
 * unioned with a SMOOTH minimum rather than a plain one. That single change is
 * what turns a body from a string of beads into one animal: smin() fillets
 * every junction instead of letting the parts intersect as separate lobes.
 *
 * Ray marching this field also sidesteps the entire mesh pipeline - no
 * triangles, no UVs, no rigging - which is exactly why it suits bodies that
 * are generated rather than authored.
 *
 * Everything here is geometry and nothing here knows about water, sky or a
 * camera, which is why the aquatic and land renderers can share it while
 * shading their worlds completely differently. Functions are static: two
 * translation units may include this without colliding at link time.
 */
#ifndef CPORE_SDFBODY_H
#define CPORE_SDFBODY_H

#include <math.h>
#include <stddef.h>

#ifndef CP_PI
#define CP_PI 3.14159265358979f
#endif

/* A stage-3 animal can now carry sixteen parts, some of which are
 * multi-link chains drawn twice for mirroring - a tail alone is five.
 * At 44 the array filled up before the tail and the legs were reached,
 * and push() dropped them silently, which reads as a body plan the
 * genome does not have. */
#define MAX_PRIM 176

typedef struct { float x, y, z; } V3;

static inline V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static inline V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 mul(V3 a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 norm(V3 a)
{
    float l = sqrtf(dot(a, a));
    return l > 1e-6f ? mul(a, 1.0f / l) : v3(0, 0, 1);
}
static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

/* Filmic roll-off instead of a hard clamp. Three light terms plus emissive
 * routinely push past 1.0, and clipping turns every bright surface into a
 * flat plate of one colour - which then quantises to a single palette entry
 * and throws away the shape underneath it. */
static inline float tonemap(float x)
{
    if (x < 0.0f) x = 0.0f;
    float a = x * (2.51f * x + 0.03f);
    float b = x * (2.43f * x + 0.59f) + 0.14f;
    return clampf(a / b, 0.0f, 1.0f);
}

/* Body-local frame from a yaw/pitch pair. Both stages keep y pointing down,
 * which is why "up" comes out negated. */
static void basis3(float yaw, float pitch, V3 *fwd, V3 *right, V3 *up)
{
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    *fwd   = v3(cp * cy, sp, cp * sy);
    *right = v3(-sy, 0.0f, cy);
    *up    = v3(cy * sp, -cp, sy * sp);
}

typedef struct {
    V3    a, b;        /* axis endpoints; a == b makes it a sphere */
    float ra, rb;      /* radius at each end                       */
    float k;           /* blend radius when merged into the body   */
    V3    col;
    float em;
    float body;        /* 1 for trunk, 0 for appendages - only the trunk
                        * carries the pattern genes */
    /* Which genome slot put this here, or -1 for the trunk. Geometry does not
     * need it and neither renderer reads it while shading; an editor does,
     * because "which part did I just click on" has no other honest answer
     * than asking the same field the picture was made from. Stamped by the
     * builder after the fact rather than passed through push(), which would
     * mean touching every call site in two stages for a field most of them
     * have nothing to say about. */
    int   part;
} Prim;

static void push(Prim *out, int *n, V3 a, V3 b, float ra, float rb,
                 float k, V3 col, float em, float body)
{
    if (*n >= MAX_PRIM) return;
    Prim *p = &out[(*n)++];
    p->a = a; p->b = b; p->ra = ra; p->rb = rb; p->k = k;
    p->col = col; p->em = em; p->body = body;
    p->part = -1;
}

/* round cone: a segment with a radius that varies along it. Approximate (the
 * exact form needs the tangent correction) so the marcher takes 0.8 steps. */
static float sd_cone(V3 p, const Prim *pr)
{
    V3 ba = sub(pr->b, pr->a);
    float l2 = dot(ba, ba);
    V3 pa = sub(p, pr->a);
    float t = l2 > 1e-6f ? clampf(dot(pa, ba) / l2, 0.0f, 1.0f) : 0.0f;
    V3 d = sub(pa, mul(ba, t));
    return sqrtf(dot(d, d)) - mixf(pr->ra, pr->rb, t);
}

/* polynomial smooth minimum - the whole point of this file */
static float smin(float a, float b, float k)
{
    if (k <= 1e-4f) return a < b ? a : b;
    float h = clampf(0.5f + 0.5f * (a - b) / k, 0.0f, 1.0f);
    return mixf(a, b, h) - k * h * (1.0f - h);
}

/* Distance plus a softmin-weighted colour, so the albedo blends across a
 * junction at the same rate the geometry does.
 *
 * The clamp at the end is load-bearing. Chaining smin() over sixteen
 * primitives compounds its correction term: every union can subtract up to
 * k/4, and with a generous k the field collapses far outside the body, so the
 * marcher hits the bounding sphere and every animal renders as one enormous
 * ball. Tracking the true minimum alongside and refusing to blend more than
 * one fillet's worth below it keeps the field honest no matter how many parts
 * a genome piles on. */
static float creature_sdf(const Prim *pr, int n, V3 q, V3 *col, float *em, float *bodyw)
{
    /* The accumulator starts at the first primitive's distance, not at a large
     * sentinel, and that is a correctness fix rather than a tidy-up.
     *
     * smin computes `a + (b - a) * h`. Seeded with 1e9f, the first iteration
     * evaluates that with a = 1e9 in float32, where the ulp is about 64 - so
     * `b - a` rounds to exactly -1e9 and the sum is exactly zero, whatever the
     * real distance was. The union of one primitive came back as 0 everywhere:
     * a body with a single lobe had no interior at all.
     *
     * It survived because nothing asked. Every rendered creature has several
     * primitives, and from the second iteration onward the accumulator holds a
     * normal-magnitude number and behaves; the floor clamp below then pulls the
     * result back to within a fillet of the true minimum, so the surface looked
     * right. It took meshing a single horn - where n really is 1 - for the
     * field to return 0.0000 over the whole of its own bounding box.
     *
     * The smooth minimum of one thing is that thing. */
    float d = 0.0f, dmin = 1e9f, kmax = 0.0f;
    V3 c = v3(0, 0, 0);
    float e = 0.0f, bw = 0.0f, wsum = 1e-6f;
    for (int i = 0; i < n; i++) {
        float di = sd_cone(q, &pr[i]);
        if (di < dmin) dmin = di;
        if (pr[i].k > kmax) kmax = pr[i].k;
        d = (i == 0) ? di : smin(d, di, pr[i].k);
        if (col) {
            float w = expf(-di * 0.30f);
            c = add(c, mul(pr[i].col, w));
            e += pr[i].em * w;
            bw += pr[i].body * w;
            wsum += w;
        }
    }
    if (col) { *col = mul(c, 1.0f / wsum); *em = e / wsum; }
    if (bodyw) *bodyw = bw / wsum;
    float floor_d = dmin - kmax * 0.55f;
    return d < floor_d ? floor_d : d;
}

/* Ambient occlusion straight out of the distance field: step along the normal
 * and see how much closer the surface is than it should be. Creases and the
 * gaps between fins darken, which is most of what makes a form look solid. */
static float sdf_ao(const Prim *pr, int n, V3 q, V3 nrm)
{
    float occ = 0.0f, sca = 1.0f;
    for (int i = 1; i <= 4; i++) {
        float h = 0.55f * (float)i;
        float d = creature_sdf(pr, n, add(q, mul(nrm, h)), NULL, NULL, NULL);
        occ += (h - d) * sca;
        sca *= 0.70f;
    }
    return clampf(1.0f - 1.6f * occ, 0.20f, 1.0f);
}

/* Soft shadow by cone tracing: march toward the light and track the closest
 * approach, which gives a penumbra for free. */
static float sdf_shadow(const Prim *pr, int n, V3 q, V3 ldir)
{
    float res = 1.0f, t = 0.9f;
    for (int i = 0; i < 20 && t < 26.0f; i++) {
        float d = creature_sdf(pr, n, add(q, mul(ldir, t)), NULL, NULL, NULL);
        if (d < 0.05f) return 0.15f;
        float k = 7.0f * d / t;
        if (k < res) res = k;
        t += clampf(d, 0.4f, 3.0f);
    }
    return clampf(res, 0.15f, 1.0f);
}

static V3 sdf_normal(const Prim *pr, int n, V3 q)
{
    const float h = 0.35f;
    float dx = creature_sdf(pr, n, v3(q.x + h, q.y, q.z), NULL, NULL, NULL)
             - creature_sdf(pr, n, v3(q.x - h, q.y, q.z), NULL, NULL, NULL);
    float dy = creature_sdf(pr, n, v3(q.x, q.y + h, q.z), NULL, NULL, NULL)
             - creature_sdf(pr, n, v3(q.x, q.y - h, q.z), NULL, NULL, NULL);
    float dz = creature_sdf(pr, n, v3(q.x, q.y, q.z + h), NULL, NULL, NULL)
             - creature_sdf(pr, n, v3(q.x, q.y, q.z - h), NULL, NULL, NULL);
    return norm(v3(dx, dy, dz));
}

/* Bounding sphere around a built body, so the marcher can enter at the shell
 * rather than at the camera. */
static void prim_bounds(const Prim *pr, int n, V3 mid, V3 *centre, float *bound)
{
    float rad = 0.0f;
    for (int i = 0; i < n; i++) {
        float da = sqrtf(dot(sub(pr[i].a, mid), sub(pr[i].a, mid))) + pr[i].ra + pr[i].k;
        float db = sqrtf(dot(sub(pr[i].b, mid), sub(pr[i].b, mid))) + pr[i].rb + pr[i].k;
        if (da > rad) rad = da;
        if (db > rad) rad = db;
    }
    *centre = mid;
    *bound = rad + 1.0f;
}

#endif /* CPORE_SDFBODY_H */
