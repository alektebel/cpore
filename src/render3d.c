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

static void draw_creature(Ctx *c, const Cp3Fish *f, int is_player)
{
    V3 fwd, right, up;
    basis3(f->yaw, f->pitch, &fwd, &right, &up);

    int nseg = f->g.nseg < 2 ? 2 : f->g.nseg;
    float R = f->s.radius;
    float L = f->s.length;

    /* Lineage colour: descendants of a founder keep its tint, so a successful
     * family is visible as one colour spreading through the shoal. */
    /* Lineage colour: descendants keep their founder's tint, so a successful
     * family reads as one hue spreading through the shoal. */
    static const float LIN[6][3] = {
        { 0.86f, 0.52f, 0.30f }, { 0.42f, 0.82f, 0.44f }, { 0.72f, 0.44f, 0.86f },
        { 0.90f, 0.78f, 0.36f }, { 0.36f, 0.66f, 0.92f }, { 0.88f, 0.40f, 0.52f },
    };
    const float *lc = LIN[f->lineage % 6];
    V3 body = is_player ? v3(0.46f, 0.90f, 0.98f) : v3(lc[0], lc[1], lc[2]);

    /* spine, head at the front, tapering to the tail, with a swim wiggle */
    V3 segpos[CP3_MAX_SEG];
    float segrad[CP3_MAX_SEG];
    for (int i = 0; i < nseg; i++) {
        float t = (float)i / (float)(nseg - 1);            /* 0 head .. 1 tail */
        float along = (0.5f - t) * L;
        float wig = sinf(f->phase - t * 2.2f) * R * 0.55f * t;
        segpos[i] = add(add(cv(f->p), mul(fwd, along)), mul(right, wig));
        segrad[i] = R * (0.62f + 0.40f * sinf(PI * (0.18f + 0.72f * (1.0f - t))));
        sphere(c, segpos[i], segrad[i], body, 0.0f);
    }

    for (int i = 0; i < CP3_MAX_PARTS; i++) {
        int t = f->g.part[i].type;
        if (t == CP3_NONE) continue;
        int sg = f->g.part[i].seg;
        if (sg >= nseg) sg = nseg - 1;

        float py = (float)f->g.part[i].yaw * (2.0f * PI / 256.0f);
        float pp = (float)f->g.part[i].pitch * (PI / 128.0f);
        float cy = cosf(py), sy = sinf(py), cpp = cosf(pp), spp = sinf(pp);
        V3 ax = norm(add(add(mul(fwd, cy * cpp), mul(right, sy * cpp)), mul(up, spp)));

        float er;
        V3 col = part_albedo(t, &er);
        V3 base = add(segpos[sg], mul(ax, segrad[sg] * 0.85f));

        switch (t) {
        case CP3_FIN:
        case CP3_TAIL: {
            /* a blade suggested by three shrinking spheres along the axis */
            float span = (t == CP3_TAIL ? 1.5f : 1.1f) * R;
            float flap = sinf(f->phase * 1.6f + (float)i) * 0.35f;
            V3 dir = norm(add(ax, mul(up, flap)));
            for (int k = 0; k < 3; k++) {
                float u = 0.35f + 0.42f * k;
                sphere(c, add(base, mul(dir, span * u)), R * (0.34f - 0.07f * k), col, 0.0f);
            }
            break;
        }
        case CP3_SPIKE:
            for (int k = 0; k < 3; k++)
                sphere(c, add(base, mul(ax, R * 0.35f * k)), R * (0.28f - 0.08f * k), col, 0.0f);
            break;
        case CP3_JAW:
            sphere(c, add(base, mul(ax, R * 0.20f)), R * 0.42f, col, 0.0f);
            sphere(c, add(base, mul(ax, R * 0.55f)), R * 0.24f, col, 0.0f);
            break;
        case CP3_EYE:
            sphere(c, base, R * 0.30f, col, 0.0f);
            sphere(c, add(base, mul(ax, R * 0.20f)), R * 0.16f, v3(0.04f, 0.05f, 0.08f), 0.0f);
            break;
        case CP3_LIGHT:
            /* the only thing in the deep that is visible on its own terms */
            sphere(c, add(base, mul(ax, R * 0.35f)), R * 0.26f, col, er);
            break;
        default:
            sphere(c, base, R * 0.32f, col, er);
            break;
        }
    }
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
