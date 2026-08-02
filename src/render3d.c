#include "cpore/aqua.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ *
 * Stage-2 renderer.
 *
 * Everything solid in this world is a sphere, so the whole scene is drawn as
 * ray-traced sphere impostors into a z-buffer: project the centre, rasterise
 * the screen-space circle, and solve for depth and normal per pixel. That
 * gives correct occlusion and real lighting for about the cost of drawing
 * circles, with no triangles, no matrices and no clipper.
 *
 * The water itself is drawn by ray-marching nothing at all: for each
 * background pixel we intersect the view ray with the seabed plane and the
 * surface plane analytically. Fog then does the rest of the work - visibility
 * falls off with depth, which is the mechanic the stage is built around.
 *
 * Output lands in the shared palette pipeline, so stage 2 looks like stage 1.
 * ------------------------------------------------------------------ */

#define PI 3.14159265358979f
#define MAXW 512
#define MAXH 320

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
/* the sim's vector type and the renderer's are the same three floats, but
 * keeping them distinct stops render maths leaking back into the sim */
static inline V3 cv(Cp3Vec v) { return v3(v.x, v.y, v.z); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

typedef struct {
    uint8_t *fb;
    float   *zb;
    int      W, H;
    V3       eye, fwd, right, up;
    float    focal;
} Ctx;

/* Rendering light is deliberately a gentler curve than cp3_daylight().
 * The simulation's falloff is aggressive because it gates perception, but
 * shading the picture with it produces a black rectangle at 300m. This is the
 * one place the renderer is allowed to disagree with the sim. */
static float lit(float depth)
{
    return expf(-clampf(depth, 0.0f, CP3_H) / 330.0f);
}

/* the colour of open water at a given depth - this is also the fog colour */
static V3 water_col(float depth)
{
    float l = lit(depth);
    return v3(0.030f + 0.105f * l, 0.135f + 0.330f * l, 0.200f + 0.370f * l);
}

static float fog_dist(float depth)
{
    return 420.0f + 700.0f * lit(depth);
}

static inline void put(Ctx *c, int x, int y, V3 col)
{
    uint8_t *p = c->fb + 4 * ((size_t)y * c->W + x);
    p[0] = (uint8_t)(clampf(col.x, 0, 1) * 255.0f);
    p[1] = (uint8_t)(clampf(col.y, 0, 1) * 255.0f);
    p[2] = (uint8_t)(clampf(col.z, 0, 1) * 255.0f);
    p[3] = 255;
}

/* apply distance fog toward the water colour at the shaded point's depth */
static V3 fogged(V3 col, float dist, float depth)
{
    float f = 1.0f - expf(-dist / fog_dist(depth));
    V3 w = water_col(depth);
    return v3(mixf(col.x, w.x, f), mixf(col.y, w.y, f), mixf(col.z, w.z, f));
}

/* ---------------- sphere impostor ---------------- */

static const V3 SUN = { 0.32f, -1.0f, 0.22f };   /* down from the surface */

static void sphere(Ctx *c, V3 wp, float rad, V3 albedo, float emissive)
{
    V3 d = sub(wp, c->eye);
    float vz = dot(d, c->fwd);
    /* Near plane. Without it a plankton mote drifting past the lens fills a
     * third of the frame and occludes the whole scene. */
    if (vz < 55.0f) return;

    float vx = dot(d, c->right), vy = dot(d, c->up);
    float sx = c->W * 0.5f + c->focal * vx / vz;
    float sy = c->H * 0.5f - c->focal * vy / vz;
    float pr = c->focal * rad / vz;
    if (pr < 0.35f) pr = 0.35f;
    if (pr > (float)c->W) return;

    int x0 = (int)floorf(sx - pr), x1 = (int)ceilf(sx + pr);
    int y0 = (int)floorf(sy - pr), y1 = (int)ceilf(sy + pr);
    if (x1 < 0 || y1 < 0 || x0 >= c->W || y0 >= c->H) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->W) x1 = c->W - 1;
    if (y1 >= c->H) y1 = c->H - 1;

    V3 sun = norm(SUN);
    float inv = 1.0f / pr;

    for (int y = y0; y <= y1; y++) {
        float ny = ((float)y + 0.5f - sy) * inv;
        for (int x = x0; x <= x1; x++) {
            float nx = ((float)x + 0.5f - sx) * inv;
            float d2 = nx * nx + ny * ny;
            if (d2 > 1.0f) continue;
            float nz = sqrtf(1.0f - d2);

            /* depth at this pixel: the front of the sphere along the view ray */
            float z = vz - rad * nz;
            float *zp = &c->zb[(size_t)y * c->W + x];
            if (z >= *zp) continue;
            *zp = z;

            /* surface normal in world space, from the impostor offset */
            V3 n = add(add(mul(c->right, nx), mul(c->up, -ny)), mul(c->fwd, -nz));
            float lam = clampf(-dot(n, sun), 0.0f, 1.0f);
            float rim = powf(1.0f - nz, 2.5f);

            /* light in the water falls off with depth; deep things are only
             * visible because they glow or because we brought a lamp */
            float here = wp.y;
            float ll = lit(here);
            float amb = 0.30f + 0.40f * ll;
            float k = amb + 0.95f * lam * (0.45f + 0.55f * ll);

            V3 col = v3(albedo.x * k, albedo.y * k, albedo.z * k);
            /* rim light does most of the silhouette work once the water gets
             * dark, so it is worth more than it looks */
            col = add(col, mul(v3(0.50f, 0.86f, 1.0f), rim * 0.42f));
            if (emissive > 0.0f) col = add(col, mul(albedo, emissive));

            put(c, x, y, fogged(col, z, wp.y));
        }
    }
}

/* ---------------- background: surface, seabed, open water ---------------- */

static void draw_water(Ctx *c, const Cp3World *w, uint32_t seed)
{
    (void)w;
    for (int y = 0; y < c->H; y++) {
        for (int x = 0; x < c->W; x++) {
            float px = ((float)x + 0.5f - c->W * 0.5f) / c->focal;
            float py = -((float)y + 0.5f - c->H * 0.5f) / c->focal;
            V3 ray = norm(add(add(mul(c->right, px), mul(c->up, py)), c->fwd));

            V3 col;
            float dist = 4000.0f;
            float depth = clampf(c->eye.y, 0.0f, CP3_H);

            if (ray.y > 0.001f) {                    /* looking down: seabed */
                float t = (CP3_H - c->eye.y) / ray.y;
                if (t > 0.0f && t < 6000.0f) {
                    V3 hit = add(c->eye, mul(ray, t));
                    /* a cheap sand texture: two out-of-phase ripples */
                    float g = 0.5f + 0.5f * sinf(hit.x * 0.035f + sinf(hit.z * 0.021f) * 2.0f);
                    float m = 0.5f + 0.5f * sinf(hit.z * 0.028f);
                    float sand = 0.46f + 0.20f * g + 0.13f * m;
                    col = v3(sand * 0.72f, sand * 0.66f, sand * 0.48f);
                    dist = t;
                    depth = CP3_H;
                } else {
                    col = water_col(CP3_H);
                    dist = 4000.0f;
                    depth = CP3_H;
                }
            } else if (ray.y < -0.001f) {            /* looking up: the surface */
                float t = (0.0f - c->eye.y) / ray.y;
                if (t > 0.0f && t < 6000.0f) {
                    V3 hit = add(c->eye, mul(ray, t));
                    float ca = sinf(hit.x * 0.02f + (float)seed * 0.001f)
                             * sinf(hit.z * 0.017f);
                    float shim = 0.55f + 0.45f * ca;
                    /* brightest looking straight up, where the sun comes in */
                    float grz = powf(clampf(-ray.y, 0.0f, 1.0f), 0.6f);
                    /* the surface is a ceiling, not a light source - too
                     * bright and it washes out the entire frame */
                    col = v3(0.14f + 0.34f * shim * grz,
                             0.38f + 0.30f * shim * grz,
                             0.52f + 0.24f * shim * grz);
                    dist = t;
                    depth = 0.0f;
                } else {
                    col = water_col(0.0f);
                    dist = 4000.0f;
                    depth = 0.0f;
                }
            } else {
                col = water_col(c->eye.y);
                dist = 4000.0f;
            }

            put(c, x, y, fogged(col, dist, depth));
            c->zb[(size_t)y * c->W + x] = 1e30f;
        }
    }
}

/* ---------------- creatures ---------------- */

/* A creature is a list of round cones (tapered capsules) unioned with a
 * SMOOTH minimum rather than a plain one. That single change is what turns a
 * body from a string of beads into one animal: smin() fillets every junction
 * instead of letting the parts intersect as separate lobes.
 *
 * Ray marching this field also sidesteps the entire mesh pipeline - no
 * triangles, no UVs, no rigging - which is exactly why it suits bodies that
 * are generated rather than authored. */

#define MAX_PRIM 44

typedef struct {
    V3    a, b;        /* axis endpoints; a == b makes it a sphere */
    float ra, rb;      /* radius at each end                       */
    float k;           /* blend radius when merged into the body   */
    V3    col;
    float em;
    float body;        /* 1 for trunk, 0 for appendages - only the trunk
                        * carries the pattern genes */
} Prim;

/* everything the shader needs about a creature that is not geometry */
typedef struct {
    V3    base, mark;  /* body colour and pattern colour */
    int   pattern;
    float freq;
    V3    origin, fwd, right, up;
} Skin;

static V3 part_albedo(int t, float *emissive)
{
    *emissive = 0.0f;
    switch (t) {
    case CP3_FILTER: return v3(0.55f, 0.88f, 0.62f);
    case CP3_JAW:    return v3(0.92f, 0.88f, 0.76f);
    case CP3_FIN:    return v3(0.42f, 0.72f, 0.86f);
    case CP3_TAIL:   return v3(0.34f, 0.62f, 0.84f);
    case CP3_SPIKE:  return v3(0.94f, 0.86f, 0.60f);
    case CP3_EYE:    return v3(0.96f, 0.97f, 1.00f);
    case CP3_LUNG:   return v3(0.72f, 0.66f, 0.86f);
    case CP3_PLATE:  return v3(0.60f, 0.64f, 0.68f);
    case CP3_LIGHT:  *emissive = 0.85f; return v3(0.55f, 0.95f, 1.00f);
    default:         return v3(0.6f, 0.6f, 0.6f);
    }
}

static void basis3(float yaw, float pitch, V3 *fwd, V3 *right, V3 *up)
{
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    *fwd   = v3(cp * cy, sp, cp * sy);
    *right = v3(-sy, 0.0f, cy);
    *up    = v3(cy * sp, -cp, sy * sp);
}

static void push(Prim *out, int *n, V3 a, V3 b, float ra, float rb,
                 float k, V3 col, float em, float body)
{
    if (*n >= MAX_PRIM) return;
    Prim *p = &out[(*n)++];
    p->a = a; p->b = b; p->ra = ra; p->rb = rb; p->k = k;
    p->col = col; p->em = em; p->body = body;
}

/* Build the body in world space. Shared by both the ray-marched and the
 * impostor path, so near and far creatures are the same animal. */
static int build_prims(const Cp3Fish *f, int is_player, Prim *out,
                       V3 *centre, float *bound, Skin *skin)
{
    V3 fwd, right, up;
    basis3(f->yaw, f->pitch, &fwd, &right, &up);

    int nseg = f->g.nseg < 2 ? 2 : f->g.nseg;
    float R = f->s.radius;
    float L = f->s.length;

    float base[3], mark[3];
    cp3_genome_colour(&f->g, base, mark);
    V3 body = v3(base[0], base[1], base[2]);
    if (is_player) {
        /* the agent's own animal keeps its genome's shape but is tinted toward
         * a fixed hue, because you must be able to find yourself in a shoal */
        body = v3(0.30f + body.x * 0.25f, 0.66f + body.y * 0.28f, 0.82f + body.z * 0.16f);
    }
    skin->base = body;
    skin->mark = v3(mark[0], mark[1], mark[2]);
    skin->pattern = f->g.pattern;
    skin->freq = 0.02f + 0.16f * ((float)f->g.pscale / 255.0f);
    skin->origin = cv(f->p);
    skin->fwd = fwd; skin->right = right; skin->up = up;

    /* Spine. The profile genes reshape the silhouette; arch and sweep bow it.
     * These three are what turn one fish into eels, discs and tadpoles. */
    float arch  = (float)f->g.arch  / 127.0f * R * 1.5f;
    float sweep = (float)f->g.sweep / 127.0f * R * 1.2f;

    V3 segpos[CP3_MAX_SEG];
    float segrad[CP3_MAX_SEG];
    for (int i = 0; i < nseg; i++) {
        float t = (float)i / (float)(nseg - 1);
        float along = (0.5f - t) * L;
        float bend = sinf(PI * t);
        float wig = sinf(f->phase - t * 2.2f) * R * 0.55f * t;
        segpos[i] = add(add(add(cv(f->p), mul(fwd, along)),
                            mul(right, wig + sweep * bend)),
                        mul(up, arch * bend));
        segrad[i] = R * cp3_profile(&f->g, t);
    }

    int n = 0;
    float bk = R * 0.34f;
    for (int i = 0; i + 1 < nseg; i++)
        push(out, &n, segpos[i], segpos[i + 1], segrad[i], segrad[i + 1],
             bk, body, 0.0f, 1.0f);
    {
        V3 tip = add(segpos[nseg - 1], mul(fwd, -L * 0.22f));
        push(out, &n, segpos[nseg - 1], tip, segrad[nseg - 1],
             R * 0.13f, bk, body, 0.0f, 1.0f);
    }

    for (int i = 0; i < CP3_MAX_PARTS; i++) {
        int t = f->g.part[i].type;
        if (t == CP3_NONE) continue;
        int sg = f->g.part[i].seg;
        if (sg >= nseg) sg = nseg - 1;

        float er;
        V3 col = part_albedo(t, &er);
        float sc = 0.45f + 1.45f * ((float)f->g.part[i].scale / 255.0f);
        int copies = f->g.part[i].mirror ? 2 : 1;

        for (int m = 0; m < copies; m++) {
            int yaw_u = m ? ((256 - f->g.part[i].yaw) & 0xFF) : f->g.part[i].yaw;
            float py = (float)yaw_u * (2.0f * PI / 256.0f);
            float pp = (float)f->g.part[i].pitch * (PI / 128.0f);
            float cy = cosf(py), sy = sinf(py), cpp = cosf(pp), spp = sinf(pp);
            V3 ax = norm(add(add(mul(fwd, cy * cpp), mul(right, sy * cpp)), mul(up, spp)));
            V3 bs = add(segpos[sg], mul(ax, segrad[sg] * 0.55f));

            switch (t) {
            case CP3_FIN:
            case CP3_TAIL: {
                float span = (t == CP3_TAIL ? 1.7f : 1.25f) * R * sc;
                float flap = sinf(f->phase * 1.6f + (float)i + (float)m * 3.1f) * 0.35f;
                V3 dir = norm(add(ax, mul(up, flap)));
                push(out, &n, bs, add(bs, mul(dir, span)),
                     R * 0.40f * sc, R * 0.10f * sc, R * 0.26f, col, 0.0f, 0.0f);
                break;
            }
            case CP3_SPIKE:
                push(out, &n, bs, add(bs, mul(ax, R * 1.15f * sc)),
                     R * 0.30f * sc, R * 0.03f, R * 0.16f, col, 0.0f, 0.0f);
                break;
            case CP3_JAW:
                push(out, &n, bs, add(bs, mul(ax, R * 0.75f * sc)),
                     R * 0.46f * sc, R * 0.20f * sc, R * 0.22f, col, 0.0f, 0.0f);
                break;
            case CP3_EYE: {
                V3 e = add(bs, mul(ax, R * 0.22f));
                float er2 = R * 0.28f * (0.7f + 0.5f * sc);
                push(out, &n, e, e, er2, er2, R * 0.05f, col, 0.0f, 0.0f);
                V3 pu = add(e, mul(ax, er2 * 0.68f));
                push(out, &n, pu, pu, er2 * 0.46f, er2 * 0.46f, R * 0.04f,
                     v3(0.05f, 0.06f, 0.10f), 0.0f, 0.0f);
                break;
            }
            case CP3_LIGHT: {
                V3 e = add(bs, mul(ax, R * 0.42f));
                float lr = R * 0.24f * sc;
                push(out, &n, e, e, lr, lr, R * 0.10f, col, er, 0.0f);
                break;
            }
            default:
                push(out, &n, bs, add(bs, mul(ax, R * 0.28f * sc)),
                     R * 0.34f * sc, R * 0.30f * sc, R * 0.20f, col, er, 0.0f);
                break;
            }
        }
    }

    V3 mid = cv(f->p);
    float rad = 0.0f;
    for (int i = 0; i < n; i++) {
        float da = sqrtf(dot(sub(out[i].a, mid), sub(out[i].a, mid))) + out[i].ra + out[i].k;
        float db = sqrtf(dot(sub(out[i].b, mid), sub(out[i].b, mid))) + out[i].rb + out[i].k;
        if (da > rad) rad = da;
        if (db > rad) rad = db;
    }
    *centre = mid;
    *bound = rad + 1.0f;
    return n;
}

/* ---------------- the distance field ---------------- */

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
    float d = 1e9f, dmin = 1e9f, kmax = 0.0f;
    V3 c = v3(0, 0, 0);
    float e = 0.0f, bw = 0.0f, wsum = 1e-6f;
    for (int i = 0; i < n; i++) {
        float di = sd_cone(q, &pr[i]);
        if (di < dmin) dmin = di;
        if (pr[i].k > kmax) kmax = pr[i].k;
        d = smin(d, di, pr[i].k);
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

/* Markings, applied only where the surface is trunk rather than appendage.
 * Colour carries as much perceived variety as shape, and quantising to 32
 * colours punishes gradients, so the patterns are deliberately crisp. */
static V3 apply_pattern(const Skin *sk, V3 q, V3 albedo, float bodyw)
{
    if (bodyw <= 0.02f || sk->pattern == CP3_PAT_PLAIN) return albedo;
    V3 d = sub(q, sk->origin);
    float along = dot(d, sk->fwd), side = dot(d, sk->right), vert = dot(d, sk->up);
    float m = 0.0f;
    switch (sk->pattern) {
    case CP3_PAT_BANDS:
        m = sinf(along * sk->freq * 6.0f) > 0.15f ? 1.0f : 0.0f;
        break;
    case CP3_PAT_SPOTS:
        m = (sinf(along * sk->freq * 5.0f) * sinf(side * sk->freq * 5.0f)
           * sinf(vert * sk->freq * 5.0f)) > 0.30f ? 1.0f : 0.0f;
        break;
    case CP3_PAT_COUNTER:
        /* dark back, pale belly - the near-universal fish pattern */
        m = clampf(0.5f + vert * 0.16f, 0.0f, 1.0f);
        break;
    default: break;
    }
    m *= bodyw;
    return v3(mixf(albedo.x, sk->mark.x, m),
              mixf(albedo.y, sk->mark.y, m),
              mixf(albedo.z, sk->mark.z, m));
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

static void shade_hit(Ctx *c, int x, int y, V3 q, V3 nrm, V3 albedo, float em, float vz)
{
    V3 sun = norm(SUN);
    float lam = clampf(-dot(nrm, sun), 0.0f, 1.0f);
    float ndotv = clampf(-dot(nrm, c->fwd), 0.0f, 1.0f);
    float rim = powf(1.0f - ndotv, 2.5f);
    float ll = lit(q.y);
    float amb = 0.30f + 0.40f * ll;
    float k = amb + 0.95f * lam * (0.45f + 0.55f * ll);

    V3 col = v3(albedo.x * k, albedo.y * k, albedo.z * k);
    col = add(col, mul(v3(0.50f, 0.86f, 1.0f), rim * 0.42f));
    if (em > 0.0f) col = add(col, mul(albedo, em));
    put(c, x, y, fogged(col, vz, q.y));
}

/* ---------------- ray-marched path ---------------- */

static void march_creature(Ctx *c, const Prim *pr, int n, V3 centre, float bound,
                           const Skin *sk)
{
    /* project the bounding sphere to a screen rect */
    V3 d = sub(centre, c->eye);
    float vz = dot(d, c->fwd);
    if (vz < 8.0f) return;
    float vx = dot(d, c->right), vy = dot(d, c->up);
    float sx = c->W * 0.5f + c->focal * vx / vz;
    float sy = c->H * 0.5f - c->focal * vy / vz;
    float pr_px = c->focal * bound / vz;

    int x0 = (int)floorf(sx - pr_px), x1 = (int)ceilf(sx + pr_px);
    int y0 = (int)floorf(sy - pr_px), y1 = (int)ceilf(sy + pr_px);
    if (x1 < 0 || y1 < 0 || x0 >= c->W || y0 >= c->H) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->W) x1 = c->W - 1;
    if (y1 >= c->H) y1 = c->H - 1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = ((float)x + 0.5f - c->W * 0.5f) / c->focal;
            float py = -((float)y + 0.5f - c->H * 0.5f) / c->focal;
            V3 ray = norm(add(add(mul(c->right, px), mul(c->up, py)), c->fwd));

            /* enter at the bounding sphere rather than at the camera */
            V3 oc = sub(c->eye, centre);
            float b = dot(oc, ray);
            float cc = dot(oc, oc) - bound * bound;
            float disc = b * b - cc;
            if (disc <= 0.0f) continue;
            float sq = sqrtf(disc);
            float t = -b - sq, tmax = -b + sq;
            if (tmax < 0.0f) continue;
            if (t < 0.0f) t = 0.0f;

            float *zp = &c->zb[(size_t)y * c->W + x];
            int hit = 0;
            V3 q = v3(0, 0, 0);
            for (int i = 0; i < 64 && t < tmax; i++) {
                q = add(c->eye, mul(ray, t));
                float dist = creature_sdf(pr, n, q, NULL, NULL, NULL);
                if (dist < 0.12f) { hit = 1; break; }
                t += dist * 0.8f;      /* the cone SDF is approximate */
            }
            if (!hit) continue;

            float hz = dot(sub(q, c->eye), c->fwd);
            if (hz >= *zp) continue;
            *zp = hz;

            V3 albedo; float em, bw;
            creature_sdf(pr, n, q, &albedo, &em, &bw);
            albedo = apply_pattern(sk, q, albedo, bw);
            shade_hit(c, x, y, q, sdf_normal(pr, n, q), albedo, em, hz);
        }
    }
}

/* ---------------- impostor path (distant creatures) ---------------- */

static void impostor_creature(Ctx *c, const Prim *pr, int n)
{
    for (int i = 0; i < n; i++) {
        sphere(c, pr[i].a, pr[i].ra, pr[i].col, pr[i].em);
        if (pr[i].ra != pr[i].rb || pr[i].a.x != pr[i].b.x ||
            pr[i].a.y != pr[i].b.y || pr[i].a.z != pr[i].b.z) {
            V3 mid = mul(add(pr[i].a, pr[i].b), 0.5f);
            sphere(c, mid, (pr[i].ra + pr[i].rb) * 0.5f, pr[i].col, pr[i].em);
            sphere(c, pr[i].b, pr[i].rb, pr[i].col, pr[i].em);
        }
    }
}

/* Ray marching a body is far more expensive than splatting spheres, so only
 * creatures big enough on screen for the difference to be visible pay for it.
 * Everything else keeps the impostors it always had. */
#define MARCH_MIN_PX 9.0f

static void draw_creature(Ctx *c, const Cp3Fish *f, int is_player)
{
    Prim pr[MAX_PRIM];
    Skin sk;
    V3 centre;
    float bound;
    int n = build_prims(f, is_player, pr, &centre, &bound, &sk);
    if (n <= 0) return;

    V3 d = sub(centre, c->eye);
    float vz = dot(d, c->fwd);
    if (vz < 8.0f) return;
    float px = c->focal * bound / vz;

    if (px >= MARCH_MIN_PX) march_creature(c, pr, n, centre, bound, &sk);
    else                    impostor_creature(c, pr, n);
}

/* ---------------- HUD ---------------- */

static void draw_hud3(uint8_t *fb, int W, int H, const Cp3World *w)
{
    char buf[80];
    const Cp3Fish *p = &w->player;

    #define PANEL(X,Y,PW,PH) do {                                             \
        cp_px_rect(fb, W, H, (X), (Y), (PW), (PH), 0.03f, 0.07f, 0.10f, 0.86f);\
        cp_px_rect(fb, W, H, (X), (Y), (PW), 1, 0.32f, 0.62f, 0.66f, 0.55f);  \
    } while (0)
    #define BAR(X,Y,BW,F,R,G,B) do {                                          \
        cp_px_rect(fb, W, H, (X), (Y), (BW), 3, 0.02f, 0.05f, 0.07f, 1.0f);   \
        int _f = (int)((BW) * clampf((F), 0.0f, 1.0f));                       \
        if (_f > 0) cp_px_rect(fb, W, H, (X), (Y), _f, 3, (R), (G), (B), 1.0f); \
    } while (0)

    /* vitals */
    PANEL(3, 3, 104, 32);
    cp_px_text(fb, W, H, 6, 6, 1, "AQUATIC STAGE", 0.62f, 0.92f, 0.96f, 1.0f);
    snprintf(buf, sizeof(buf), "G%d/%d T%d", w->generation + 1, CP3_GENERATIONS, w->step);
    cp_px_text(fb, W, H, 6, 15, 1, buf, 0.42f, 0.58f, 0.64f, 1.0f);
    BAR(6, 24, 46, p->hp / p->hp_max, 0.86f, 0.28f, 0.30f);
    BAR(55, 24, 46, w->biomass / CP3_BIOMASS_GOAL, 0.38f, 0.82f, 0.46f);

    /* depth gauge: the axis the whole stage turns on */
    {
        int gx = W - 14, gy = 8, gh = H - 42;
        PANEL(gx - 3, gy - 5, 12, gh + 12);
        cp_px_text(fb, W, H, gx - 2, gy - 4, 1, "D", 0.5f, 0.8f, 0.84f, 1.0f);
        for (int i = 0; i < gh; i++) {
            float d = (float)i / gh * CP3_H;
            float l = cp3_daylight(d);
            cp_px_rect(fb, W, H, gx, gy + 4 + i, 5, 1,
                       0.04f + 0.10f * l, 0.10f + 0.34f * l, 0.16f + 0.36f * l, 1.0f);
        }
        int py = gy + 4 + (int)(p->p.y / CP3_H * gh);
        cp_px_rect(fb, W, H, gx - 2, py, 9, 1, 0.75f, 1.0f, 0.98f, 1.0f);
        snprintf(buf, sizeof(buf), "%dM", (int)p->p.y);
        cp_px_text(fb, W, H, gx - 20, py - 3, 1, buf, 0.70f, 0.92f, 0.95f, 1.0f);
    }

    /* Population panel. This is the interesting readout: nothing scripts these
     * numbers, they are whatever survived. */
    PANEL(3, H - 54, 118, 51);
    cp_px_text(fb, W, H, 6, H - 51, 1, "POPULATION", 0.62f, 0.92f, 0.96f, 1.0f);
    snprintf(buf, sizeof(buf), "N %-3d GEN %.1f", w->pop, (double)w->mean_gen);
    cp_px_text(fb, W, H, 6, H - 42, 1, buf, 0.72f, 0.82f, 0.84f, 1.0f);
    snprintf(buf, sizeof(buf), "BIRTH %-4d DIE %d", w->births, w->deaths);
    cp_px_text(fb, W, H, 6, H - 34, 1, buf, 0.62f, 0.72f, 0.74f, 1.0f);
    snprintf(buf, sizeof(buf), "MOUTH %.2f TAIL %.2f", (double)w->mean_mouth, (double)w->mean_tail);
    cp_px_text(fb, W, H, 6, H - 26, 1, buf, 0.55f, 0.85f, 0.62f, 1.0f);
    snprintf(buf, sizeof(buf), "LAMP %.2f  DEPTH %d", (double)w->mean_light, (int)w->mean_depth);
    cp_px_text(fb, W, H, 6, H - 18, 1, buf, 0.55f, 0.78f, 0.90f, 1.0f);
    snprintf(buf, sizeof(buf), "PARTS %.1f", (double)w->mean_parts);
    cp_px_text(fb, W, H, 6, H - 10, 1, buf, 0.60f, 0.70f, 0.72f, 1.0f);

    /* own build */
    PANEL(W - 128, H - 30, 108, 27);
    snprintf(buf, sizeof(buf), "%dDNA %dP %dSEG",
             (int)p->s.cost, p->s.n_parts, p->g.nseg);
    cp_px_text(fb, W, H, W - 125, H - 27, 1, buf, 0.50f, 0.74f, 0.78f, 1.0f);
    int col = 0;
    for (int t = 1; t < CP3_PART_COUNT; t++) {
        int n = p->s.n[t];
        if (!n) continue;
        float er;
        V3 cl = part_albedo(t, &er);
        int x = W - 125 + (col % 6) * 17, y = H - 17 + (col / 6) * 8;
        cp_px_rect(fb, W, H, x, y, 5, 5, cl.x, cl.y, cl.z, 1.0f);
        snprintf(buf, sizeof(buf), "%d", n);
        cp_px_text(fb, W, H, x + 7, y - 1, 1, buf, 0.78f, 0.86f, 0.88f, 1.0f);
        col++;
    }

    if (w->status != CP3_RUN) {
        const char *msg = w->status == CP3_EVOLVED ? "EVOLVE - LAND"
                        : w->status == CP3_DEAD    ? "EATEN" : "TIME UP";
        int tw = cp_px_text_w(msg, 1);
        int bx = (W - tw) / 2, by = H / 2 - 7;
        PANEL(bx - 6, by - 4, tw + 12, 16);
        cp_px_text(fb, W, H, bx, by, 1, msg,
                   w->status == CP3_EVOLVED ? 0.52f : 1.0f,
                   w->status == CP3_EVOLVED ? 0.95f : 0.46f, 0.60f, 1.0f);
    }
    #undef PANEL
    #undef BAR
}

/* ---------------- entry ---------------- */

void cp3_render_styled(const Cp3World *w, uint8_t *rgba, int OW, int OH, int style)
{
    int32_t lw = 320, lh = 180;
    cp_vis_dims(style, &lw, &lh);
    if (lw > MAXW) lw = MAXW;
    if (lh > MAXH) lh = MAXH;

    uint8_t *fb = (uint8_t *)malloc((size_t)lw * lh * 4);
    float   *zb = (float *)malloc(sizeof(float) * (size_t)lw * lh);
    if (!fb || !zb) { free(fb); free(zb); return; }

    const Cp3Fish *p = &w->player;
    V3 pf, pr, pu;
    basis3(p->yaw, p->pitch, &pf, &pr, &pu);

    Ctx c;
    c.fb = fb; c.zb = zb; c.W = lw; c.H = lh;
    c.focal = (float)lw * 0.85f;

    /* chase camera: behind and a little above, looking slightly down the body */
    float back = p->s.length * 2.4f + p->s.radius * 6.5f + 44.0f;
    c.eye = add(add(cv(p->p), mul(pf, -back)), mul(pu, p->s.radius * 2.2f + 9.0f));
    c.eye.y = clampf(c.eye.y, 4.0f, CP3_H - 4.0f);
    V3 look = norm(sub(add(cv(p->p), mul(pf, 34.0f)), c.eye));
    c.fwd = look;
    c.right = norm(v3(-look.z, 0.0f, look.x));
    c.up = norm(v3(c.right.z * look.y - c.right.y * look.z,
                   c.right.x * look.z - c.right.z * look.x,
                   c.right.y * look.x - c.right.x * look.y));

    draw_water(&c, w, w->seed);

    /* plankton and carrion */
    for (int i = 0; i < CP3_MAX_FOOD; i++) {
        const Cp3Food *f = &w->food[i];
        if (f->type == CP3_FOOD_NONE) continue;
        V3 wp = v3(f->p.x, f->p.y, f->p.z);
        if (f->type == CP3_FOOD_PLANKTON)
            sphere(&c, wp, f->r, v3(0.40f, 0.80f, 0.46f), 0.08f);
        else
            sphere(&c, wp, f->r, v3(0.80f, 0.36f, 0.24f), 0.05f);
    }

    for (int i = 0; i < CP3_MAX_FISH; i++)
        if (w->fish[i].alive) draw_creature(&c, &w->fish[i], 0);

    draw_creature(&c, p, 1);

    draw_hud3(fb, lw, lh, w);
    cp_vis_quantise(fb, lw, lh, style);
    cp_vis_blit(fb, lw, lh, rgba, OW, OH, style);

    free(fb);
    free(zb);
}

void cp3_render(const Cp3World *w, uint8_t *rgba, int W, int H)
{
    cp3_render_styled(w, rgba, W, H, CP_VIS_ABYSS);
}


/* ---------------- portrait ----------------
 * A genome space is only as good as the variety you can see in it, so this
 * renders one creature side-on with nothing else in frame. */
void cp3_render_portrait(const Cp3Genome *g, uint8_t *fb, int lw, int lh,
                         int style, uint32_t seed)
{
    float *zb = (float *)malloc(sizeof(float) * (size_t)lw * lh);
    if (!zb) return;

    Cp3Fish f;
    memset(&f, 0, sizeof(f));
    f.g = *g;
    cp3_genome_stats(&f.g, &f.s);
    f.hp = f.hp_max = f.s.hp_max;
    f.alive = 1;
    f.p.x = 0.0f; f.p.y = 120.0f; f.p.z = 0.0f;
    f.yaw = 0.0f;
    f.pitch = -0.06f;
    f.phase = (float)(seed % 128) * 0.05f;
    f.lineage = (uint8_t)(seed % 6);

    Prim pr[MAX_PRIM];
    Skin sk;
    V3 centre;
    float bound;
    int n = build_prims(&f, 0, pr, &centre, &bound, &sk);

    Ctx c;
    c.fb = fb; c.zb = zb; c.W = lw; c.H = lh;
    c.focal = (float)lw * 1.15f;
    /* three-quarter view: straight side-on hides everything mounted fore
     * and aft, which is most of what the genome varies */
    V3 dir = norm(v3(-0.62f, -0.30f, 0.72f));
    float dist = bound * 2.15f + 6.0f;
    c.eye = add(centre, mul(dir, dist));
    V3 look = norm(sub(centre, c.eye));
    c.fwd = look;
    c.right = norm(v3(-look.z, 0.0f, look.x));
    c.up = norm(v3(c.right.z * look.y - c.right.y * look.z,
                   c.right.x * look.z - c.right.z * look.x,
                   c.right.y * look.x - c.right.x * look.y));

    for (int y = 0; y < lh; y++) {
        float t = (float)y / (float)lh;
        V3 bg = v3(mixf(0.030f, 0.075f, t), mixf(0.100f, 0.170f, t),
                   mixf(0.150f, 0.215f, t));
        for (int x = 0; x < lw; x++) {
            put(&c, x, y, bg);
            zb[(size_t)y * lw + x] = 1e30f;
        }
    }

    march_creature(&c, pr, n, centre, bound, &sk);
    cp_vis_quantise(fb, lw, lh, style);
    free(zb);
}
