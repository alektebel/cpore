/* "Drop" - the cell stage as a darkfield microscope plate.
 *
 * This is a second renderer for stage 1, not a restyling of the first one.
 * The pixel path in render.c draws hard-edged shapes into a 320x180 buffer,
 * snaps them to 32 colours through an ordered dither and blows the result up
 * with nearest-neighbour. Every decision in it follows from a bandwidth
 * budget: spend everything on silhouette and value, nothing on gradient or
 * material. This file takes the opposite bet, and so shares none of that
 * machinery.
 *
 * The organising idea is darkfield illumination. Light a specimen obliquely
 * and the only light that reaches the objective is the light the specimen
 * scattered, so the field goes black and anything transparent blazes along
 * its edges. It is how almost every beautiful photograph of pond water is
 * taken, and it happens to suit a cheap 2D renderer perfectly:
 *
 *   - the background is nearly black, so contrast is free and there is
 *     nothing to quantise;
 *   - a body reads by its rim rather than its fill, and a rim is an analytic
 *     function of the distance to a circle's edge, which costs one sqrt;
 *   - interiors are translucent, so organelles are worth drawing and the
 *     thing that carries variety is internal structure rather than outline;
 *   - anything that emits - a discharge, a poison sac, a jet - lands in an
 *     HDR buffer and blooms, which is most of what makes bioluminescence
 *     read as light rather than as paint.
 *
 * Nothing in here is thresholded. Coverage is a real number, the accumulation
 * buffer is linear float with no ceiling, and the palette is whatever the
 * tonemapper hands back. The pixel styles all still work; this is one more
 * entry in the style enum, and the only one that goes down a different path.
 */

#include "cpore/cpore.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "hdrcanvas.h"

#define DPI HC_PI

/* ------------------------------------------------------------------ *
 * the plate
 *
 * The canvas, plus where the microscope is pointed. Everything that is not
 * specific to a drop of water - the buffer, the primitives, the film chain -
 * lives in hdrcanvas.h and is shared with the landscape renderer.
 * ------------------------------------------------------------------ */

typedef struct {
    Hdr      h;
    float    camx, camy;          /* world coords at the top-left pixel   */
    float    scale;               /* pixels per world unit                */
    float    time;
    uint32_t seed;
} Plate;

static inline float sxw(const Plate *p, float wx) { return (wx - p->camx) * p->scale; }
static inline float syw(const Plate *p, float wy) { return (wy - p->camy) * p->scale; }

/* ------------------------------------------------------------------ *
 * the field
 * ------------------------------------------------------------------ */

/* ---- levels ----
 *
 * Every intensity in this file is scene-referred, and the exposure that maps
 * the set of them onto the tonemapping curve lives in one place at the bottom.
 * The six reference levels below are what the numbers mean; a term written as
 * 0.5 is "half of a lit surface", not "half of white". Keeping the two apart
 * is what makes the exposure adjustable at all - mixed together, changing how
 * bright the frame is means retuning forty constants, which is exactly the
 * hole the first pass of this renderer fell into.
 *
 *   L_FIELD  the dark water, and the floor of the whole image
 *   L_DIM    present, but not something the eye should go to
 *   L_MID    an ordinary lit surface
 *   L_SKIN   the membrane rim: the brightest thing that is still a colour
 *   L_HOT    clips to white, and blooms
 *   L_FLARE  a discharge core, several stops past clipping
 */
#define L_FIELD 0.012f
#define L_DIM   0.055f
#define L_MID   0.200f
#define L_SKIN  0.480f
#define L_HOT   1.600f
#define L_FLARE 9.000f

/* Not black. A field that is exactly zero has no colour for the bloom to
 * pick up and no floor for the tonemapper to sit on, and the frame comes out
 * looking like a cutout on a void rather than like water. */
static const C3 FIELD  = { 0.0028f, 0.0092f, 0.0132f };
static const C3 COND   = { 0.0038f, 0.0118f, 0.0150f };   /* condenser cone   */
static const C3 CAUS   = { 0.0070f, 0.0195f, 0.0242f };   /* surface caustics */

/* Where the illumination comes from, in screen space. Up and to the left,
 * and slightly toward the viewer, which is the convention every reader of a
 * lit image already has. */
static const float LX = -0.480f, LY = -0.545f, LZ = 0.688f;

static void draw_field(Plate *p, const CpWorld *w)
{
    float cx = p->h.W * 0.5f, cy = p->h.H * 0.5f;
    float inv = 1.0f / (float)(p->h.W < p->h.H ? p->h.W : p->h.H);

    /* Base gradient, condenser hotspot and caustics in one pass. The caustics
     * are two interfering ripples squared: light through a moving surface
     * concentrates along the lines where the two agree, and squaring is what
     * turns a smooth sum into those thin bright filaments. */
    for (int y = 0; y < p->h.H; y++) {
        float wy = p->camy + (float)y / p->scale;
        for (int x = 0; x < p->h.W; x++) {
            float wx = p->camx + (float)x / p->scale;
            float dx = ((float)x - cx) * inv, dy = ((float)y - cy) * inv;
            float r2 = dx * dx + dy * dy;

            float cone = expf(-r2 * 3.4f);
            C3 col = cadd(FIELD, cscl(COND, cone));

            float a = sinf(wx * 0.0135f + wy * 0.0072f + p->time * 0.42f);
            float b = sinf(wx * 0.0061f - wy * 0.0148f - p->time * 0.31f);
            float k = (a + b) * 0.5f + 0.5f;
            k *= k; k *= k;
            col = cadd(col, cscl(CAUS, k * (0.35f + 0.65f * cone)));

            float *t = p->h.px + 3 * ((size_t)y * p->h.W + x);
            t[0] = col.r; t[1] = col.g; t[2] = col.b;
        }
    }

    /* Junk a long way off the focal plane. Enormous, almost invisible, and
     * the single strongest cue that the frame is looking through an
     * objective rather than at a diagram: real bokeh discs are flat with a
     * brighter edge, so that is what these are. */
    CpRng r;
    cp_rng_seed(&r, w->seed ^ 0x51A3u);
    for (int i = 0; i < 22; i++) {
        float bx = cp_rng_range(&r, -400.0f, CP_WORLD_W + 400.0f);
        float by = cp_rng_range(&r, -400.0f, CP_WORLD_H + 400.0f);
        float depth = cp_rng_range(&r, 0.10f, 0.34f);      /* parallax factor */
        float rad = cp_rng_range(&r, 46.0f, 165.0f);
        float hue = cp_rng_range(&r, 0.42f, 0.60f);
        C3 col = dhsv(hue, 0.55f, 1.0f);
        float sx = (bx - p->camx * depth) * p->scale;
        float sy = (by - p->camy * depth) * p->scale;
        if (sx < -rad * 2 || sy < -rad * 2 || sx > p->h.W + rad * 2 || sy > p->h.H + rad * 2) continue;
        d_disc(&p->h, sx, sy, rad, col, L_FIELD * 0.11f, rad * 0.16f);
        d_arc(&p->h, sx, sy, rad * 0.94f, rad * 0.11f, 0.0f, 2.0f * DPI, col, L_FIELD * 0.18f);
    }

    /* Suspended matter, at every depth. Near particles are big and soft, ones
     * on the plane are single sharp points, far ones are dim - which is
     * exactly the read the pixel path could not have, because at 320x180 a
     * mote is one pixel and one pixel has no depth. */
    CpRng m;
    cp_rng_seed(&m, w->seed ^ 0x1234u);
    for (int i = 0; i < 1500; i++) {
        float mx = cp_rng_range(&m, -200.0f, CP_WORLD_W + 200.0f);
        float my = cp_rng_range(&m, -200.0f, CP_WORLD_H + 200.0f);
        float z  = cp_rng_range(&m, -1.0f, 1.0f);          /* -1 near, +1 far */
        float ph = cp_rng_range(&m, 0.0f, 6.28f);
        float par = 1.0f - z * 0.42f;
        /* a slow brownian drift, so a still frame still looks alive */
        float drift = 2.6f + 2.0f * z;
        mx += sinf(p->time * 0.21f + ph) * drift;
        my += cosf(p->time * 0.17f + ph * 1.7f) * drift * 0.7f;
        float sx = (mx - p->camx * par) * p->scale;
        float sy = (my - p->camy * par) * p->scale;
        if (sx < -20 || sy < -20 || sx > p->h.W + 20 || sy > p->h.H + 20) continue;
        float blur = fabsf(z) * (z < 0.0f ? 5.5f : 1.6f);
        float rad = 0.55f + (z < 0.0f ? -z * 2.2f : 0.0f);
        float dim = z > 0.0f ? 1.0f - z * 0.72f : 1.0f;
        C3 col = clerp(c3(0.70f, 0.92f, 1.00f), c3(0.42f, 0.78f, 0.74f),
                       dhash2(w->seed, i, 7));
        d_disc(&p->h, sx, sy, rad, col, L_MID * 1.35f * dim, blur);
    }
}

/* The edge of the drop.
 *
 * The world is bounded, and the pixel path drew that boundary as a dark band
 * with a hard line on it - a wall. Under an objective the same edge is a
 * meniscus: water curving up to meet glass, which concentrates the light into
 * a bright rim with darkness beyond it. Same information, and it is the one
 * place in the frame where the whole illumination model is visible at once. */
static void draw_meniscus(Plate *p)
{
    float l = sxw(p, 0.0f), rt = sxw(p, CP_WORLD_W);
    float t = syw(p, 0.0f), b = syw(p, CP_WORLD_H);

    for (int y = 0; y < p->h.H; y++) {
        float fy = (float)y + 0.5f;
        for (int x = 0; x < p->h.W; x++) {
            float fx = (float)x + 0.5f;
            /* signed distance to the inside of the box, in pixels */
            float dl = fx - l, drr = rt - fx, dt = fy - t, db = b - fy;
            float d = dl < drr ? dl : drr;
            if (dt < d) d = dt;
            if (db < d) d = db;

            if (d < 0.0f) {
                /* outside the drop: glass, and nothing in it */
                hdr_mul(&p->h, x, y, sat(0.06f + 0.10f * expf(d * 0.06f)));
                continue;
            }
            float band = 46.0f;
            if (d > band) continue;
            float f = d / band;
            /* absorption climbing toward the edge, then the rim on top */
            hdr_mul(&p->h, x, y, dmixf(0.20f, 1.0f, sstep(0.0f, 1.0f, f)));
            float rim = expf(-powf((f - 0.10f) / 0.085f, 2.0f));
            hdr_add(&p->h, x, y, c3(0.34f, 0.86f, 0.98f), rim * L_SKIN * 0.62f);
        }
    }
}

/* ------------------------------------------------------------------ *
 * cell material
 *
 * A cell is a translucent bag. The three passes below are the three things
 * light does to one: it is absorbed on the way through, it scatters off what
 * is suspended inside, and it grazes the membrane on the way past. Splitting
 * them lets the organelles be drawn between the second and the third, so they
 * sit inside the body rather than on it, which is the entire reason the
 * interior is worth drawing at all.
 * ------------------------------------------------------------------ */

typedef struct {
    C3    body;         /* cytoplasm tint                    */
    C3    rim;          /* what the membrane scatters        */
    float blur;         /* pixels off the focal plane        */
    float gain;         /* depth dimming                     */
    float wobble;       /* how much the outline breathes     */
    float phase;
    uint32_t id;
} Skin;

/* Radius of the outline at a given bearing. A perfect circle reads as a
 * bubble; a cell is a bag under tension and its outline is never quite
 * closed. Two low harmonics is enough and keeps the shape stable frame to
 * frame, which a noise field would not. */
static inline float memb_r(const Skin *sk, float rad, float a)
{
    return rad * (1.0f + sk->wobble * (0.62f * sinf(a * 3.0f + sk->phase * 0.7f)
                                     + 0.38f * sinf(a * 5.0f - sk->phase * 1.1f + 1.7f)));
}

/* Pass one: absorption and the interior scatter. */
static void memb_body(Plate *p, float cx, float cy, float rad, const Skin *sk)
{
    float w = 1.0f + 2.0f * sk->blur;
    float ext = rad * (1.0f + sk->wobble) + w + 1.0f;
    int x0 = (int)floorf(cx - ext), x1 = (int)ceilf(cx + ext);
    int y0 = (int)floorf(cy - ext), y1 = (int)ceilf(cy + ext);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->h.W) x1 = p->h.W - 1;
    if (y1 >= p->h.H) y1 = p->h.H - 1;

    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);
            float R = memb_r(sk, rad, atan2f(dy, dx));
            float c = cov_disc(d, R, w);
            if (c <= 0.0f) continue;
            float f = R > 0.0f ? d / R : 0.0f;
            if (f > 1.0f) f = 1.0f;
            float nz = sqrtf(1.0f - f * f);

            /* Absorption. A darkfield specimen is bright at the edge and
             * dim through the middle - a jellyfish, not a marble - so the
             * thick part of the bag is where the background goes away. */
            float T = 1.0f - 0.74f * powf(nz, 0.55f);
            hdr_mul(&p->h, x, y, dmixf(1.0f, T, c));

            /* Cytoplasm: what little the interior does scatter, granulated so
             * a flat fill does not read as moulded plastic. */
            float g = dfbm(sk->id, (cx + dx) * 0.16f, (cy + dy) * 0.16f);
            C3 inner = cscl(sk->body, (L_DIM * 0.42f + L_DIM * 1.35f * powf(nz, 0.85f))
                                      * (0.66f + 0.68f * g) * sk->gain);
            hdr_add(&p->h, x, y, inner, c);
        }
    }
}

/* Pass three: everything that happens at the surface. Run after the
 * organelles so the membrane always closes over its own contents. */
static void memb_skin(Plate *p, float cx, float cy, float rad, const Skin *sk)
{
    float w = 1.0f + 2.0f * sk->blur;
    float ext = rad * (1.0f + sk->wobble) + w + 1.0f;
    int x0 = (int)floorf(cx - ext), x1 = (int)ceilf(cx + ext);
    int y0 = (int)floorf(cy - ext), y1 = (int)ceilf(cy + ext);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->h.W) x1 = p->h.W - 1;
    if (y1 >= p->h.H) y1 = p->h.H - 1;

    /* Defocus takes the peak off every thin feature, and the rim is the
     * thinnest feature on the frame. Without this a blurred cell keeps a
     * razor edge and reads as sharp with a soft interior. */
    float sharp = 1.0f / (1.0f + sk->blur * 0.85f);

    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);
            float R = memb_r(sk, rad, atan2f(dy, dx));
            float c = cov_disc(d, R, w);
            if (c <= 0.0f) continue;
            float f = R > 0.0f ? d / R : 0.0f;
            if (f > 1.0f) f = 1.0f;
            float nz = sqrtf(1.0f - f * f);
            float nx = d > 1e-4f ? dx / d * f : 0.0f;
            float ny = d > 1e-4f ? dy / d * f : 0.0f;

            C3 out = c3(0, 0, 0);

            /* Fresnel scatter. This is the whole look: at a grazing angle the
             * path through the membrane is long, so the edge of a transparent
             * body is the brightest thing on it.
             *
             * The exponent is the load-bearing number in this file. Written
             * at the 3.4 a 3D fresnel wants, essentially all of the term
             * lands in the outermost two percent of the radius - which on a
             * twenty-pixel cell is less than one pixel, so the single effect
             * the whole look is built on renders as nothing at all. On a
             * projected disc the falloff has to be broad enough to occupy a
             * band the eye can see. */
            float fres = powf(1.0f - nz, 1.75f);
            out = cadd(out, cscl(sk->rim, fres * L_SKIN * 1.12f));

            /* The membrane itself: a bright line just inside the outline. Its
             * width is held to at least a pixel and a half in screen space
             * for the same reason - a shell defined as a fixed fraction of
             * the radius disappears on anything small. */
            float lw = rad > 0.0f ? (1.6f / rad) : 0.08f;
            if (lw < 0.055f) lw = 0.055f;
            float line = expf(-powf((f - (1.0f - lw * 1.1f)) / lw, 2.0f));
            out = cadd(out, cscl(sk->rim, line * L_SKIN * 1.25f * sharp));

            /* Specular. Small, tight and white - the one term that says the
             * surface is wet rather than merely bright. */
            float ndl = nx * LX + ny * LY + nz * LZ;
            if (ndl > 0.0f) {
                float s = powf(ndl, 88.0f);
                out = cadd(out, cscl(c3(0.86f, 0.97f, 1.00f), s * L_HOT * 0.62f * sharp));
            }

            /* Inner caustic: light that entered the lit side, crossed the
             * body and piled up against the far inner wall. It is what
             * separates a bag of fluid from a disc with a highlight on it. */
            float away = -(nx * LX + ny * LY) / (f > 1e-3f ? f : 1.0f);
            float caus = sstep(0.30f, 1.0f, away) * sstep(0.40f, 0.94f, f)
                       * sstep(1.02f, 0.86f, f);
            out = cadd(out, cscl(sk->rim, caus * L_MID * 1.55f));

            hdr_add(&p->h, x, y, cscl(out, sk->gain), c);
        }
    }
}

/* Pass two: what is suspended in the bag.
 *
 * Placement is hashed off the cell's identity so it is stable for the life of
 * the animal, and the whole arrangement rotates with the heading, because an
 * interior that stays put while the body turns reads as a decal. */
static void memb_organelles(Plate *p, float cx, float cy, float rad,
                            float head, const Skin *sk)
{
    if (rad < 4.0f) return;
    float ca = cosf(head), sa = sinf(head);
    C3 org = clerp(sk->body, sk->rim, 0.45f);

    /* Nucleus: off centre, toward the back, with a dense core. */
    {
        float ox = -rad * 0.16f, oy = rad * 0.10f;
        float nx = cx + ox * ca - oy * sa, ny = cy + ox * sa + oy * ca;
        float nr = rad * 0.30f;
        d_occ(&p->h, nx, ny, nr, 0.72f, sk->blur);
        d_disc(&p->h, nx, ny, nr, org, L_DIM * 0.75f, sk->blur);
        d_arc(&p->h, nx, ny, nr * 0.96f, 1.5f, 0.0f, 2.0f * DPI,
              sk->rim, L_MID * 1.35f * sk->gain);
        d_disc(&p->h, nx - nr * 0.20f, ny - nr * 0.22f, nr * 0.34f,
               sk->rim, L_MID * 1.9f * sk->gain, sk->blur);
        d_glow(&p->h, nx, ny, nr * 2.2f, org, L_DIM * 0.65f * sk->gain);
    }

    /* Vacuoles: small bubbles, bright-rimmed and hollow, which is what makes
     * the interior read as a volume with things floating in it. */
    int n = 3 + (int)(dhash2(sk->id, 3, 9) * 3.0f);
    for (int i = 0; i < n; i++) {
        float a = dhash2(sk->id, i, 17) * 6.2831f;
        float t = 0.34f + 0.44f * dhash2(sk->id, i, 23);
        float vr = rad * (0.055f + 0.075f * dhash2(sk->id, i, 31));
        float ox = cosf(a) * rad * t, oy = sinf(a) * rad * t;
        float vx = cx + ox * ca - oy * sa, vy = cy + ox * sa + oy * ca;
        /* breathing, on its own phase */
        vr *= 0.86f + 0.20f * sinf(sk->phase * 1.6f + (float)i * 2.1f);
        d_occ(&p->h, vx, vy, vr, 0.80f, sk->blur);
        d_arc(&p->h, vx, vy, vr, 1.3f, 0.0f, 2.0f * DPI,
              sk->rim, L_MID * 1.75f * sk->gain);
        d_disc(&p->h, vx - vr * 0.28f, vy - vr * 0.30f, vr * 0.30f,
               c3(0.80f, 0.95f, 1.00f), L_MID * 1.5f * sk->gain, sk->blur);
    }
}

/* ------------------------------------------------------------------ *
 * parts
 *
 * The same ten types and the same mounting angles the simulation reads - if
 * a spike looks like it is on the back it is on the back, and it will not hit
 * anything in front of you. What changes is the material vocabulary. Soft
 * things scatter and glow; the two hard things in the roster, the jaw and the
 * spike, are the only parts in the stage allowed a specular edge, so "this
 * one is armed" is legible before any of the numbers are.
 * ------------------------------------------------------------------ */

#define LAYER_UNDER 0
#define LAYER_OVER  1

static int part_layer(int type)
{
    switch (type) {
    case CP_PART_EYE: case CP_PART_ELECTRIC: case CP_PART_POISON: return LAYER_OVER;
    default: return LAYER_UNDER;
    }
}

/* A whip that carries a travelling wave rather than a wobble. Real flagella
 * push a bend from base to tip; sampling one sine along the arc with the
 * phase running backwards is the whole trick, and it is the difference
 * between a tail that swims and a tail that vibrates. */
static void whip(Plate *p, float bx, float by, float ang, float len,
                 float amp, float freq, float phase, int nseg,
                 float r0, float r1, C3 col, float inten, float blur)
{
    float ux = cosf(ang), uy = sinf(ang);
    float px_ = -uy, py = ux;
    float lx = bx, ly = by;
    for (int k = 1; k <= nseg; k++) {
        float t = (float)k / (float)nseg;
        float off = sinf(t * freq - phase) * amp * t * t;
        float nx = bx + ux * len * t + px_ * off;
        float ny = by + uy * len * t + py * off;
        d_fil(&p->h, lx, ly, nx, ny, dmixf(r0, r1, t - 1.0f / nseg), dmixf(r0, r1, t),
              col, inten * (1.0f - 0.35f * t), blur);
        lx = nx; ly = ny;
    }
}

/* Branching discharge. Two levels of fork is plenty: the eye reads "lightning"
 * off the fact that a branch exists at all, not off its depth. */
static void bolt(Plate *p, float x, float y, float ang, float len,
                 uint32_t s, int depth, C3 col, float inten)
{
    int n = 4;
    float lx = x, ly = y, a = ang;
    for (int k = 0; k < n; k++) {
        a += (dhash2(s, k, depth) - 0.5f) * 1.15f;
        float seg = len / n;
        float nx = lx + cosf(a) * seg, ny = ly + sinf(a) * seg;
        float wdt = dmixf(1.9f, 0.5f, (float)k / n) * (depth ? 0.55f : 1.0f);
        /* Sheath first, then the core stroke on top. A halo hung off each
         * node instead reads as a puff of smoke strung along the bolt - the
         * glow has to follow the line, because that is what the ionised
         * channel around a real arc does. */
        d_fil(&p->h, lx, ly, nx, ny, wdt * 4.0f, wdt * 3.2f, col, inten * 0.055f, 1.6f);
        d_fil(&p->h, lx, ly, nx, ny, wdt, wdt * 0.8f, col, inten, 0.0f);
        if (depth == 0 && k == 1 + (int)(dhash2(s, 9, 1) * 2.0f))
            bolt(p, nx, ny, a + (dhash2(s, k, 5) - 0.5f) * 1.9f, len * 0.5f,
                 s ^ 0x9E37u, 1, col, inten * 0.65f);
        lx = nx; ly = ny;
    }
}

static void draw_part(Plate *p, float cx, float cy, float rad, float wa,
                      int type, float phase, const Skin *sk, float flash, int idx)
{
    float ux = cosf(wa), uy = sinf(wa);
    float bx = cx + ux * rad * 0.94f, by = cy + uy * rad * 0.94f;
    float bl = sk->blur, gn = sk->gain;
    C3 acc = sk->rim;

    switch (type) {

    case CP_PART_FILTER: {
        /* A feeding comb: fine filaments fanned out, with the catch glowing
         * green where it collects at the base. */
        C3 fc = c3(0.42f, 1.00f, 0.62f);
        for (int i = -2; i <= 2; i++) {
            float a = wa + (float)i * 0.235f;
            float ln = rad * (0.62f + 0.10f * sinf(phase * 3.0f + (float)i * 0.9f));
            whip(p, bx, by, a, ln, rad * 0.07f, 3.4f, phase * 2.6f - (float)i * 0.5f,
                 3, rad * 0.035f, rad * 0.012f, fc, L_MID * 1.25f * gn, bl);
        }
        d_glow(&p->h, bx, by, rad * 0.55f, fc, L_DIM * 1.5f * gn);
        d_disc(&p->h, bx, by, rad * 0.13f, fc, L_HOT * 0.55f * gn, bl);
        break;
    }

    case CP_PART_JAW: {
        /* Chitin: the one hard material in the stage. Drawn as a dark body
         * with a bright specular edge along its outer curve rather than as a
         * bright shape - a mandible that glows looks soft, and this part is
         * the whole reason a carnivore is frightening. */
        float gap = 0.19f + 0.13f * (0.5f + 0.5f * sinf(phase * 5.0f));
        for (int i = -1; i <= 1; i += 2) {
            float a = wa + (float)i * gap;
            float tx = bx + cosf(a) * rad * 0.80f, ty = by + sinf(a) * rad * 0.80f;
            float rx = bx - ux * rad * 0.14f, ry = by - uy * rad * 0.14f;
            d_fil_occ(&p->h, rx, ry, tx, ty, rad * 0.19f, rad * 0.030f, 0.22f, bl);
            d_fil(&p->h, rx, ry, tx, ty, rad * 0.19f, rad * 0.030f,
                  c3(0.42f, 0.36f, 0.28f), L_DIM * 0.9f * gn, bl);
            /* the lit edge, offset toward the light */
            float ox = LX * rad * 0.055f, oy = LY * rad * 0.055f;
            d_fil(&p->h, rx + ox, ry + oy, tx + ox, ty + oy, rad * 0.115f, rad * 0.016f,
                  c3(1.00f, 0.94f, 0.76f), L_SKIN * 1.05f * gn, bl);
            d_disc(&p->h, tx, ty, rad * 0.035f, c3(1.0f, 1.0f, 0.92f), L_HOT * 0.75f * gn, bl);
        }
        break;
    }

    case CP_PART_PROBOSCIS: {
        /* A glass straw with something moving up it. */
        float ln = rad * (0.95f + 0.10f * sinf(phase * 2.0f + wa * 3.0f));
        float tx = bx + ux * ln, ty = by + uy * ln;
        C3 pc = c3(0.78f, 0.58f, 1.00f);
        d_fil(&p->h, bx - ux * rad * 0.1f, by - uy * rad * 0.1f, tx, ty,
              rad * 0.135f, rad * 0.095f, pc, L_DIM * 0.85f, bl);
        d_fil(&p->h, bx - ux * rad * 0.1f, by - uy * rad * 0.1f, tx, ty,
              rad * 0.055f, rad * 0.035f, pc, L_MID * 1.6f * gn, bl);
        /* a bolus travelling toward the mouth */
        float t = fmodf(phase * 0.34f, 1.0f);
        d_disc(&p->h, dmixf(tx, bx, t), dmixf(ty, by, t), rad * 0.055f,
               c3(1.0f, 0.86f, 1.0f), L_HOT * 0.60f * gn, bl);
        d_disc(&p->h, tx, ty, rad * 0.115f, pc, L_SKIN * 1.15f * gn, bl);
        d_glow(&p->h, tx, ty, rad * 0.5f, pc, L_DIM * 1.2f * gn);
        break;
    }

    case CP_PART_CILIA: {
        /* One oar of a metachronal wave. The phase offset comes in through
         * `idx` so that a ring of these beats as a travelling wave around the
         * body instead of all together, which is both what real cilia do and
         * the single most hypnotic thing in the frame. */
        float beat = sinf(phase * 5.5f - (float)idx * 0.85f);
        float ln = rad * 0.42f * (0.72f + 0.36f * beat);
        float bend = beat * 0.55f;
        C3 cc = clerp(acc, c3(0.85f, 1.0f, 1.0f), 0.35f);
        whip(p, bx, by, wa + bend * 0.4f, ln, rad * 0.09f, 2.2f, -bend * 2.0f,
             3, rad * 0.038f, rad * 0.010f, cc, L_MID * 1.15f * gn, bl);
        /* the tip catches the light on the power stroke */
        if (beat > 0.45f)
            d_disc(&p->h, bx + cosf(wa + bend) * ln, by + sinf(wa + bend) * ln,
                   rad * 0.028f, c3(0.9f, 1.0f, 1.0f), L_HOT * 0.42f * gn * beat, bl);
        break;
    }

    case CP_PART_FLAGELLA: {
        C3 fc = clerp(acc, c3(0.45f, 0.72f, 1.00f), 0.5f);
        whip(p, bx, by, wa, rad * 2.15f, rad * 0.42f, 7.5f, phase * 4.2f,
             9, rad * 0.055f, rad * 0.012f, fc, L_MID * 1.05f * gn, bl);
        break;
    }

    case CP_PART_JET: {
        /* Nozzle, throat and plume. The throat is the brightest single point
         * on an unarmed cell, which is what makes thrust read as thrust. */
        float tx = bx + ux * rad * 0.44f, ty = by + uy * rad * 0.44f;
        C3 jc = c3(0.42f, 0.72f, 1.00f);
        d_fil_occ(&p->h, bx - ux * rad * 0.10f, by - uy * rad * 0.10f, tx, ty,
                  rad * 0.25f, rad * 0.19f, 0.30f, bl);
        d_fil(&p->h, bx - ux * rad * 0.10f, by - uy * rad * 0.10f, tx, ty,
              rad * 0.25f, rad * 0.19f, c3(0.42f, 0.56f, 0.72f), L_DIM * 0.8f, bl);
        d_arc(&p->h, tx, ty, rad * 0.17f, 2.0f, 0.0f, 2.0f * DPI, jc, L_SKIN * 1.1f * gn);
        float pulse = 0.55f + 0.45f * sinf(phase * 9.0f);
        d_disc(&p->h, tx, ty, rad * 0.10f, c3(0.85f, 0.96f, 1.0f), L_FLARE * 0.30f * gn * pulse, bl);
        for (int k = 1; k <= 7; k++) {
            float t = (float)k / 7.0f;
            float jx = tx + ux * rad * 1.5f * t;
            float jy = ty + uy * rad * 1.5f * t;
            float wob = sinf(phase * 8.0f - (float)k * 1.1f) * rad * 0.10f * t;
            d_glow(&p->h, jx - uy * wob, jy + ux * wob, rad * (0.22f + 0.42f * t),
                   jc, L_DIM * 2.4f * pulse * (1.0f - t) * (1.0f - t) * gn);
        }
        break;
    }

    case CP_PART_SPIKE: {
        /* Glassy rather than metallic: a translucent barb with the light
         * piped down it to a hot tip. */
        float t0x = cx + ux * rad * 0.72f, t0y = cy + uy * rad * 0.72f;
        float t1x = cx + ux * rad * 1.78f, t1y = cy + uy * rad * 1.78f;
        C3 sc = c3(1.00f, 0.93f, 0.74f);
        d_fil(&p->h, t0x, t0y, t1x, t1y, rad * 0.22f, rad * 0.012f, sc, L_DIM * 0.9f, bl);
        d_fil(&p->h, t0x, t0y, t1x, t1y, rad * 0.085f, rad * 0.008f, sc, L_SKIN * 1.25f * gn, bl);
        d_disc(&p->h, t1x, t1y, rad * 0.045f, c3(1.0f, 1.0f, 0.94f), L_HOT * 1.0f * gn, bl);
        d_glow(&p->h, t1x, t1y, rad * 0.42f, sc, L_DIM * 1.4f * gn);
        break;
    }

    case CP_PART_ELECTRIC: {
        float ex = cx + ux * rad * 0.82f, ey = cy + uy * rad * 0.82f;
        C3 ec = c3(0.42f, 0.80f, 1.00f);
        float charge = 0.55f + 0.45f * sinf(phase * 3.1f);
        /* The halo has to stay smaller than the animal it is on. Grown to
         * three and a half radii at full intensity - and drawn once per
         * electric part - a discharge stopped being a discharge and became a
         * white disc with the cell somewhere inside it. What should carry the
         * moment is the branching and the ring, both of which are structure,
         * and structure is the first thing a blown highlight destroys. */
        d_glow(&p->h, ex, ey, rad * (0.70f + flash * 1.15f), ec,
               L_DIM * 1.6f + flash * L_MID * 1.15f);
        d_disc(&p->h, ex, ey, rad * 0.20f, ec, L_DIM * 1.1f, bl);
        d_arc(&p->h, ex, ey, rad * 0.20f, 1.6f, 0.0f, 2.0f * DPI, ec, L_SKIN * 1.5f * gn);
        d_disc(&p->h, ex, ey, rad * 0.085f, c3(0.90f, 0.98f, 1.0f),
               (L_HOT * 1.0f + L_FLARE * 0.75f * flash) * charge, bl);
        if (flash > 0.0f)
            for (int i = 0; i < 3; i++)
                bolt(p, ex, ey, wa + ((float)i - 1.0f) * 0.95f, rad * (2.2f + 2.6f * flash),
                     (uint32_t)(sk->id + (uint32_t)i * 977u), 0,
                     c3(0.72f, 0.92f, 1.0f), L_HOT * (0.42f * flash + 0.20f));
        break;
    }

    case CP_PART_POISON: {
        /* Sacs that punish a biter, so they have to look toxic: an acid green
         * that appears nowhere else in the palette, pulsing, and leaking. */
        float ex = cx + ux * rad * 0.80f, ey = cy + uy * rad * 0.80f;
        C3 pc = c3(0.58f, 1.00f, 0.16f);
        float pulse = 0.60f + 0.40f * sinf(phase * 2.2f);
        d_glow(&p->h, ex, ey, rad * 1.05f, pc, L_DIM * 2.1f * pulse * gn);
        d_disc(&p->h, ex, ey, rad * 0.21f, pc, L_DIM * 0.85f, bl);
        d_arc(&p->h, ex, ey, rad * 0.21f, 1.5f, 0.0f, 2.0f * DPI, pc, L_SKIN * 1.3f * gn);
        d_disc(&p->h, ex - ux * rad * 0.05f, ey - uy * rad * 0.05f, rad * 0.085f,
               c3(0.86f, 1.0f, 0.55f), L_HOT * 0.85f * pulse * gn, bl);
        for (int k = 0; k < 3; k++) {
            float t = fmodf(phase * 0.22f + (float)k * 0.33f, 1.0f);
            float a = wa + sinf((float)k * 2.4f + phase * 0.4f) * 0.8f;
            d_disc(&p->h, ex + cosf(a) * rad * t * 0.9f, ey + sinf(a) * rad * t * 0.9f,
                   rad * 0.030f, pc, L_MID * 1.9f * (1.0f - t) * gn, bl + t * 1.5f);
        }
        break;
    }

    case CP_PART_EYE: {
        /* The focal point. Spore's creatures are legible almost entirely
         * because of their eyes, and the reason is contrast: a small dark
         * pupil with a hard white catchlight on it is the highest-frequency,
         * highest-contrast thing on an otherwise soft body, so the eye is
         * where a reader's attention lands and the rest of the animal is
         * read outward from there. */
        float ex = cx + ux * rad * 0.66f, ey = cy + uy * rad * 0.66f;
        float er = rad * 0.25f;
        C3 iris = clerp(acc, c3(0.30f, 0.95f, 1.00f), 0.45f);
        d_glow(&p->h, ex, ey, er * 3.0f, iris, L_DIM * 1.2f * gn);
        d_occ(&p->h, ex, ey, er, 0.55f, bl);
        d_disc(&p->h, ex, ey, er, c3(0.90f, 0.96f, 1.00f), L_MID * 1.15f * gn, bl); /* sclera */
        d_disc(&p->h, ex + ux * er * 0.20f, ey + uy * er * 0.20f, er * 0.70f,
               iris, L_MID * 1.7f * gn, bl);                                    /* iris   */
        d_arc(&p->h, ex + ux * er * 0.20f, ey + uy * er * 0.20f, er * 0.70f, 1.4f,
              0.0f, 2.0f * DPI, iris, L_SKIN * 1.25f * gn);
        /* Pupil: the darkest thing on the animal, and the reason the eye has
         * any contrast at all. It has to remove light, not add it. */
        d_occ(&p->h, ex + ux * er * 0.34f, ey + uy * er * 0.34f, er * 0.34f, 0.06f, bl);
        d_disc(&p->h, ex + LX * er * 0.42f, ey + LY * er * 0.42f, er * 0.20f,
               c3(1.0f, 1.0f, 1.0f), L_FLARE * 0.22f * gn, bl * 0.5f);          /* catch  */
        break;
    }

    default: break;
    }
    (void)idx;
}

/* ------------------------------------------------------------------ *
 * cells
 * ------------------------------------------------------------------ */

/* Where in the drop this cell is swimming, hashed off its slot so it is
 * stable for the whole episode. Depth costs it sharpness, brightness and a
 * little saturation - the three things distance takes from anything seen
 * through a medium, and the reason a crowded frame still has a foreground. */
static float cell_depth(uint32_t seed, int idx)
{
    return dhash2(seed ^ 0x2B1Du, idx, 5) * 2.0f - 1.0f;
}

static void draw_npc(Plate *p, const CpWorld *w, const CpCell *n, int idx,
                     float sx, float sy, float rad)
{
    float z = cell_depth(w->seed, idx);
    float az = fabsf(z);

    Skin sk;
    sk.id     = (uint32_t)(w->seed + (uint32_t)idx * 7919u);
    sk.blur   = az * az * 3.4f;
    sk.gain   = 1.0f - 0.45f * az;
    sk.phase  = n->phase;
    sk.wobble = 0.045f + 0.030f * dhash2(sk.id, 1, 2);

    /* Diet is a mechanic, so it gets to be a colour: a hunter runs hot and
     * saturated, a grazer cool and soft. Hue still comes from the genome, so
     * two hunters are still two different animals. */
    float hot = n->diet == CP_DIET_CARN ? 1.0f : (n->diet == CP_DIET_OMNI ? 0.5f : 0.0f);
    sk.body = dhsv(n->hue, 0.92f - 0.10f * hot, 1.00f);
    sk.rim  = dhsv(n->hue + 0.02f * hot, 0.66f - 0.20f * hot, 1.0f);
    sk.rim  = clerp(sk.rim, c3(1.00f, 0.72f, 0.42f), hot * 0.35f);
    /* far cells drift toward the colour of the water they are seen through */
    if (z > 0.0f) {
        sk.body = clerp(sk.body, c3(0.10f, 0.34f, 0.40f), z * 0.45f);
        sk.rim  = clerp(sk.rim,  c3(0.26f, 0.62f, 0.72f), z * 0.45f);
    }

    float head = (n->vx * n->vx + n->vy * n->vy) > 1.0f
               ? atan2f(n->vy, n->vx) : n->wander_a;

    /* the scatter halo every suspended body has in a lit medium */
    d_glow(&p->h, sx, sy, rad * 2.1f, sk.rim, L_DIM * 0.16f * sk.gain);

    if (rad < 3.2f) {
        d_disc(&p->h, sx, sy, rad, sk.body, L_DIM * 1.1f, sk.blur);
        d_arc(&p->h, sx, sy, rad, 1.4f, 0.0f, 2.0f * DPI, sk.rim, L_SKIN * 1.15f * sk.gain);
        return;
    }

    int ncil = 6 + n->cilia * 2;
    if (ncil > 18) ncil = 18;
    for (int i = 0; i < ncil; i++)
        draw_part(p, sx, sy, rad, head + (float)i / ncil * 2.0f * DPI,
                  CP_PART_CILIA, n->phase, &sk, 0.0f, i);
    for (int i = 0; i < n->spikes; i++)
        draw_part(p, sx, sy, rad, head + (float)i / (n->spikes ? n->spikes : 1) * 2.0f * DPI,
                  CP_PART_SPIKE, n->phase, &sk, 0.0f, i);
    for (int i = 0; i < n->jaws; i++)
        draw_part(p, sx, sy, rad, head + (i ? DPI : 0.0f),
                  n->diet == CP_DIET_HERB ? CP_PART_FILTER : CP_PART_JAW,
                  n->phase, &sk, 0.0f, i);

    memb_body(p, sx, sy, rad, &sk);
    memb_organelles(p, sx, sy, rad, head, &sk);
    memb_skin(p, sx, sy, rad, &sk);

    for (int i = 0; i < n->eyes; i++)
        draw_part(p, sx, sy, rad, head + ((float)i - (n->eyes - 1) * 0.5f) * 0.52f,
                  CP_PART_EYE, n->phase, &sk, 0.0f, i);
    if (n->poison > 0.0f)
        draw_part(p, sx, sy, rad, head + DPI, CP_PART_POISON, n->phase, &sk, 0.0f, 0);

    /* Wounds, as an arc over the animal rather than a bar above it. */
    if (n->hp < n->hp_max * 0.995f && rad >= 4.0f) {
        float f = sat(n->hp / n->hp_max);
        float r0 = rad * 1.28f + 3.0f;
        d_arc(&p->h, sx, sy, r0, 1.4f, -DPI * 0.86f, -DPI * 0.14f,
              c3(1.00f, 0.30f, 0.28f), L_DIM * 0.8f);
        if (f > 0.0f)
            d_arc(&p->h, sx, sy, r0, 1.8f, -DPI * 0.86f, -DPI * (0.86f - 0.72f * f),
                  c3(1.00f, 0.28f, 0.22f), L_SKIN * 1.2f);
    }
}

static void draw_player(Plate *p, const CpWorld *w, float sx, float sy, float rad)
{
    const CpCell *pl = &w->player;

    Skin sk;
    sk.id     = w->seed ^ 0xB1A5u;
    sk.blur   = 0.0f;                /* the player is always on the plane */
    sk.gain   = 1.0f;
    sk.phase  = pl->phase;
    sk.wobble = 0.040f;
    sk.body   = c3(0.04f, 0.60f, 0.86f);
    sk.rim    = c3(0.20f, 0.86f, 1.00f);
    if (!pl->alive) {
        sk.body = c3(0.22f, 0.24f, 0.28f);
        sk.rim  = c3(0.42f, 0.46f, 0.52f);
    }

    float flash = w->elec_flash > 0.0f ? w->elec_flash * 5.0f : 0.0f;

    d_glow(&p->h, sx, sy, rad * 2.4f, sk.rim, L_DIM * 0.34f);

    for (int layer = 0; layer <= 1; layer++) {
        if (layer == 1) {
            memb_body(p, sx, sy, rad, &sk);
            memb_organelles(p, sx, sy, rad, pl->heading, &sk);
            memb_skin(p, sx, sy, rad, &sk);
        }
        int ci = 0;
        for (int i = 0; i < CP_MAX_PARTS; i++) {
            int t = w->genome.part[i].type;
            if (t == CP_PART_NONE) continue;
            if (part_layer(t) != layer) continue;
            float wa = pl->heading + (float)w->genome.part[i].angle * (2.0f * DPI / 256.0f);
            draw_part(p, sx, sy, rad, wa, t, pl->phase, &sk, flash, ci++);
        }
    }

    /* Discharge shockwave, at the radius the simulation actually used - the
     * ring has to be the truth about reach, not a flourish. */
    if (w->elec_flash > 0.0f && w->stats.elec_radius > 0.0f) {
        float k = sat(w->elec_flash / 0.20f);
        float er = w->stats.elec_radius * p->scale;
        float g = 1.15f - 0.15f * k;
        d_arc(&p->h, sx, sy, er * g, 2.4f, 0.0f, 2.0f * DPI,
              c3(0.62f, 0.90f, 1.0f), L_HOT * 1.15f * k);
        d_arc(&p->h, sx, sy, er * g * 0.80f, 1.4f, 0.0f, 2.0f * DPI,
              c3(0.40f, 0.72f, 1.0f), L_SKIN * 1.1f * k);
        d_glow(&p->h, sx, sy, er * 1.3f, c3(0.30f, 0.62f, 1.0f), L_DIM * 2.2f * k);
    }
}

/* ------------------------------------------------------------------ *
 * food
 * ------------------------------------------------------------------ */

static void draw_food(Plate *p, const CpWorld *w)
{
    for (int i = 0; i < CP_MAX_FOOD; i++) {
        const CpFood *f = &w->food[i];
        if (f->type == CP_FOOD_NONE) continue;
        float sx = sxw(p, f->x), sy = syw(p, f->y);
        float rad = f->r * p->scale;
        if (sx < -30 || sy < -30 || sx > p->h.W + 30 || sy > p->h.H + 30) continue;

        if (f->type == CP_FOOD_PLANT) {
            /* An algal colony: a few cells stuck together, each with its own
             * chloroplast lit from inside. Green is the only strongly
             * saturated hue in the field, so food is findable at a glance. */
            C3 gc = c3(0.22f, 1.00f, 0.42f);
            float bob = sinf(p->time * 1.4f + f->phase) * 0.5f + 0.5f;
            d_glow(&p->h, sx, sy, rad * 2.4f, gc, L_DIM * (0.42f + 0.16f * bob));
            for (int k = 0; k < 3; k++) {
                float a = f->phase + (float)k * 2.094f;
                float ox = cosf(a) * rad * 0.42f, oy = sinf(a) * rad * 0.42f;
                d_disc(&p->h, sx + ox, sy + oy, rad * 0.58f, gc, L_DIM * 0.75f, 0.0f);
                d_arc(&p->h, sx + ox, sy + oy, rad * 0.58f, 1.3f, 0.0f, 2.0f * DPI,
                      gc, L_SKIN * 0.78f);
                d_disc(&p->h, sx + ox * 0.4f, sy + oy * 0.4f, rad * 0.20f,
                       c3(0.62f, 1.0f, 0.66f), L_HOT * (0.30f + 0.09f * bob), 0.0f);
            }
        } else {
            /* Carrion. Dim, warm and decaying: the phase field is seconds of
             * life left, so it can actually fade as it rots. */
            float life = sat(f->phase / 12.0f);
            C3 mc = c3(1.00f, 0.34f, 0.16f);
            d_glow(&p->h, sx, sy, rad * 2.6f, mc, L_DIM * 1.1f * (0.4f + 0.6f * life));
            d_disc(&p->h, sx, sy, rad * 0.95f, mc, L_DIM * 0.65f, 0.6f);
            d_arc(&p->h, sx, sy, rad * 0.95f, 1.5f, 0.0f, 2.0f * DPI,
                  mc, L_MID * 1.5f * (0.35f + 0.65f * life));
            /* shreds coming off it */
            for (int k = 0; k < 3; k++) {
                float a = (float)i * 1.7f + (float)k * 2.1f;
                d_fil(&p->h, sx, sy, sx + cosf(a) * rad * 1.5f, sy + sinf(a) * rad * 1.5f,
                      rad * 0.16f, 0.4f, mc, L_DIM * 1.1f * life, 0.8f);
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * HUD
 *
 * Hairlines and dot matrix, nothing filled. The roadmap's complaint about the
 * land stage applies twice as hard here: two solid panels in the corners of a
 * frame this dark read as holes punched in it. The vitals go around the
 * player instead, which is where the eye already is, and everything else is
 * pushed to the very edge as thin rules.
 * ------------------------------------------------------------------ */

static const C3 UI     = { 0.34f, 0.86f, 0.92f };
static const C3 UI_DIM = { 0.16f, 0.40f, 0.46f };

/* Frame corners: four thin brackets, which is the cheapest way to say
 * "instrument" and the only decoration in here that is not data. */
static void hud_frame(Plate *p)
{
    float m = 14.0f * p->h.ui, l = 26.0f * p->h.ui;
    float W = (float)p->h.W, H = (float)p->h.H;
    struct { float x, y, dx, dy; } k[4] = {
        { m, m, 1, 1 }, { W - m, m, -1, 1 }, { m, H - m, 1, -1 }, { W - m, H - m, -1, -1 }
    };
    for (int i = 0; i < 4; i++) {
        float x = k[i].x, y = k[i].y;
        hud_rule(&p->h, k[i].dx > 0 ? x : x - l, y, l, p->h.ui, UI_DIM, L_MID * 1.0f);
        hud_rule(&p->h, x - (k[i].dx > 0 ? 0.0f : p->h.ui), k[i].dy > 0 ? y : y - l,
                 p->h.ui, l, UI_DIM, L_MID * 1.0f);
    }
}

static void hud_vitals(Plate *p, const CpWorld *w, float sx, float sy, float rad)
{
    const CpCell *pl = &w->player;
    float r0 = rad * 1.34f + 7.0f * p->h.ui;

    /* Health on the left half, DNA on the right, both reading outward from
     * the top - so a glance at the animal is a glance at both. */
    float hp = sat(pl->hp / pl->hp_max);
    float dna = sat(w->dna / CP_DNA_GOAL);

    d_arc(&p->h, sx, sy, r0, 1.2f * p->h.ui, -DPI * 0.97f, -DPI * 0.53f, UI_DIM, L_MID * 1.2f);
    d_arc(&p->h, sx, sy, r0, 1.2f * p->h.ui, -DPI * 0.47f, -DPI * 0.03f, UI_DIM, L_MID * 1.2f);
    if (hp > 0.0f)
        d_arc(&p->h, sx, sy, r0, 2.2f * p->h.ui, -DPI * (0.53f + 0.44f * hp), -DPI * 0.53f,
              clerp(c3(1.0f, 0.22f, 0.20f), c3(0.42f, 1.0f, 0.62f), hp), L_SKIN * 1.6f);
    if (dna > 0.0f)
        d_arc(&p->h, sx, sy, r0, 2.2f * p->h.ui, -DPI * 0.47f, -DPI * (0.47f - 0.44f * dna),
              c3(0.40f, 1.00f, 0.72f), L_SKIN * 1.6f);

    /* facing chevron */
    float a = pl->heading;
    float tx = sx + cosf(a) * (r0 + 5.0f * p->h.ui), ty = sy + sinf(a) * (r0 + 5.0f * p->h.ui);
    for (int i = -1; i <= 1; i += 2) {
        float b = a + (float)i * 2.42f;
        d_fil(&p->h, tx, ty, tx + cosf(b) * 5.0f * p->h.ui, ty + sinf(b) * 5.0f * p->h.ui,
              1.1f * p->h.ui, 0.4f, UI, L_SKIN * 1.4f, 0.0f);
    }
}

static void hud_minimap(Plate *p, const CpWorld *w, float viewW, float viewH)
{
    float mw = 132.0f * p->h.ui, mh = mw * (CP_WORLD_H / CP_WORLD_W);
    float mx = (float)p->h.W - mw - 26.0f * p->h.ui, my = 26.0f * p->h.ui;
    float sx = mw / CP_WORLD_W, sy = mh / CP_WORLD_H;

    hud_rule(&p->h, mx, my, mw, p->h.ui, UI_DIM, L_MID * 1.0f);
    hud_rule(&p->h, mx, my + mh, mw, p->h.ui, UI_DIM, L_MID * 1.0f);
    hud_rule(&p->h, mx, my, p->h.ui, mh, UI_DIM, L_MID * 1.0f);
    hud_rule(&p->h, mx + mw, my, p->h.ui, mh, UI_DIM, L_MID * 1.0f);

    for (int i = 0; i < CP_MAX_FOOD; i += 3) {
        const CpFood *f = &w->food[i];
        if (f->type == CP_FOOD_NONE) continue;
        d_disc(&p->h, mx + f->x * sx, my + f->y * sy, 0.7f * p->h.ui,
               f->type == CP_FOOD_PLANT ? c3(0.30f, 1.0f, 0.45f) : c3(1.0f, 0.40f, 0.22f),
               L_SKIN * 0.80f, 0.0f);
    }
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        const CpCell *c = &w->cells[i];
        if (!c->alive) continue;
        d_disc(&p->h, mx + c->x * sx, my + c->y * sy, 1.0f * p->h.ui, dhsv(c->hue, 0.55f, 1.0f),
               L_SKIN * 1.05f, 0.0f);
    }

    /* viewport */
    float vx = mx + p->camx * sx, vy = my + p->camy * sy;
    float vw = viewW * sx, vh = viewH * sy;
    hud_rule(&p->h, vx, vy, vw, p->h.ui, UI, L_MID * 1.6f);
    hud_rule(&p->h, vx, vy + vh, vw, p->h.ui, UI, L_MID * 1.6f);
    hud_rule(&p->h, vx, vy, p->h.ui, vh, UI, L_MID * 1.6f);
    hud_rule(&p->h, vx + vw, vy, p->h.ui, vh, UI, L_MID * 1.6f);

    d_disc(&p->h, mx + w->player.x * sx, my + w->player.y * sy, 1.8f * p->h.ui,
           c3(0.60f, 1.0f, 1.0f), L_HOT * 0.75f, 0.0f);
}

static C3 part_hue(int t)
{
    switch (t) {
    case CP_PART_FILTER:    return c3(0.42f, 1.00f, 0.62f);
    case CP_PART_JAW:       return c3(1.00f, 0.94f, 0.76f);
    case CP_PART_PROBOSCIS: return c3(0.78f, 0.58f, 1.00f);
    case CP_PART_CILIA:     return c3(0.45f, 0.90f, 1.00f);
    case CP_PART_FLAGELLA:  return c3(0.45f, 0.72f, 1.00f);
    case CP_PART_JET:       return c3(0.42f, 0.72f, 1.00f);
    case CP_PART_SPIKE:     return c3(1.00f, 0.93f, 0.74f);
    case CP_PART_ELECTRIC:  return c3(0.42f, 0.80f, 1.00f);
    case CP_PART_POISON:    return c3(0.58f, 1.00f, 0.16f);
    case CP_PART_EYE:       return c3(0.90f, 0.96f, 1.00f);
    default:                return UI_DIM;
    }
}

/* A scrim, so a readout stays readable over a bright cell.
 *
 * The HUD is drawn last and still lost against the animal: it is thin
 * additive dot matrix, and adding light to something already near the top of
 * the tonemapping curve moves it almost nowhere. Drawing it brighter is not
 * the fix - there is no headroom left up there, which is exactly the problem -
 * so what the text needs is somewhere darker to sit. This multiplies the
 * frame down along the line before the glyphs are added on top of it, which
 * costs nothing when the background is already dark and rescues the case
 * where it is not. */
static void hud_scrim(Hdr *h, float x, float y, float w, float glyph)
{
    float r = glyph * 0.95f;
    d_fil_occ(h, x - r * 0.6f, y + glyph * 0.5f, x + w, y + glyph * 0.5f,
              r, r, 0.20f, r * 0.9f);
}

static void draw_hud(Plate *p, const CpWorld *w, float viewW, float viewH,
                     float psx, float psy, float prad)
{
    char buf[80];
    /* Everything below is laid out in HUD units rather than pixels, so the
     * readout keeps its proportion of the frame instead of doubling in
     * apparent size the moment someone renders at 640 wide. */
    float u = p->h.ui, M = 26.0f * u;

    hud_frame(p);
    hud_vitals(p, w, psx, psy, prad);
    hud_minimap(p, w, viewW, viewH);

    /* top left: where we are in the run */
    hud_scrim(&p->h, M, M, 132.0f * u, 3.0f * u * 5.0f);
    hud_scrim(&p->h, M, M + 33.0f * u, 132.0f * u, 2.0f * u * 5.0f);
    d_text(&p->h, M, M, 3.0f * u, "CELL STAGE", UI, L_SKIN * 0.95f);
    hud_rule(&p->h, M, M + 25.0f * u, 118.0f * u, u, UI_DIM, L_MID * 1.5f);
    snprintf(buf, sizeof(buf), "GEN %d/%d   T%d", w->generation + 1, CP_GENERATIONS, w->step);
    d_text(&p->h, M, M + 33.0f * u, 2.0f * u, buf, UI_DIM, L_SKIN * 1.35f);

    /* bottom left: the body plan, as swatches */
    {
        float y = (float)p->h.H - M - 34.0f * u;
        snprintf(buf, sizeof(buf), "%d DNA   %d PARTS", (int)w->stats.cost, w->stats.n_parts);
        hud_scrim(&p->h, M, y, 142.0f * u, 2.0f * u * 5.0f);
        hud_scrim(&p->h, M, y + 25.0f * u, 142.0f * u, 2.0f * u * 5.0f);
        d_text(&p->h, M, y, 2.0f * u, buf, UI, L_SKIN * 0.85f);
        hud_rule(&p->h, M, y + 19.0f * u, 128.0f * u, u, UI_DIM, L_MID * 1.5f);
        float x = M;
        for (int t = 1; t < CP_PART_COUNT; t++) {
            int n = w->stats.n[t];
            if (!n) continue;
            C3 pc = part_hue(t);
            d_disc(&p->h, x + 3.0f * u, y + 31.0f * u, 3.2f * u, pc, L_SKIN * 1.1f, 0.0f);
            d_glow(&p->h, x + 3.0f * u, y + 31.0f * u, 9.0f * u, pc, L_DIM * 1.4f);
            snprintf(buf, sizeof(buf), "%d", n);
            d_text(&p->h, x + 9.0f * u, y + 25.0f * u, 2.0f * u, buf, UI_DIM, L_SKIN * 1.25f);
            x += 26.0f * u;
        }
    }

    /* under the minimap: what the episode has actually done */
    {
        float mh = 132.0f * u * (CP_WORLD_H / CP_WORLD_W);
        float x = (float)p->h.W - 132.0f * u - M, y = M + mh + 12.0f * u;
        snprintf(buf, sizeof(buf), "EAT %-4d KILL %d", w->ate_plant + w->ate_meat, w->kills);
        d_text(&p->h, x, y, 2.0f * u, buf, UI_DIM, L_SKIN * 1.25f);
        snprintf(buf, sizeof(buf), "HIT %-4d ZAP  %d", w->hits_taken, w->discharges);
        d_text(&p->h, x, y + 14.0f * u, 2.0f * u, buf, UI_DIM, L_SKIN * 1.25f);
    }

    /* terminal card */
    if (w->status != CP_RUN) {
        const char *msg = w->status == CP_EVOLVED ? "EVOLVE - CREATURE STAGE"
                        : w->status == CP_DEAD    ? "CONSUMED"
                                                  : "TIME UP";
        C3 col = w->status == CP_EVOLVED ? c3(0.44f, 1.00f, 0.72f)
               : w->status == CP_DEAD    ? c3(1.00f, 0.34f, 0.30f)
                                         : c3(0.90f, 0.86f, 0.52f);
        float sc = 5.0f * u;
        float tw = d_textw(msg, sc);
        float x = ((float)p->h.W - tw) * 0.5f, y = (float)p->h.H * 0.5f - 3.5f * sc;
        for (int j = 0; j < p->h.H; j++) {
            float fy = fabsf((float)j - (float)p->h.H * 0.5f) / ((float)p->h.H * 0.5f);
            float k = dmixf(0.34f, 1.0f, sstep(0.10f, 0.68f, fy));
            for (int i = 0; i < p->h.W; i++) hdr_mul(&p->h, i, j, k);
        }
        hud_rule(&p->h, x - 20.0f * u, y - 16.0f * u, tw + 40.0f * u, u, col, L_SKIN * 1.1f);
        hud_rule(&p->h, x - 20.0f * u, y + 7.0f * sc + 15.0f * u, tw + 40.0f * u, u,
                 col, L_SKIN * 1.1f);
        d_text(&p->h, x, y, sc, msg, col, L_HOT * 0.75f);
    }
}

/* ------------------------------------------------------------------ *
 * entry
 * ------------------------------------------------------------------ */

/* Pixels per world unit. The pixel path showed about 1070 world units across
 * a 1280-wide frame; this shows about 900, because the whole argument for a
 * continuous-tone renderer is that a cell has an interior worth looking at,
 * and at the old scale the interior is nine pixels across. */
#define DROP_SCALE_REF 1.85f

/* Exposure.
 *
 * Held as one number in one place because everything else in the file is
 * written in scene-referred units - a cilium is "a bit brighter than the
 * cytoplasm", a discharge core is "twenty times anything else" - and those
 * ratios are the art direction. How far up the tonemapping curve the whole
 * set lands is a separate decision, and mixing the two means every retune of
 * the exposure turns into a retune of forty constants. */
#define DROP_EXPOSURE 6.6f

void cp_render_drop(const CpWorld *w, uint8_t *rgba, int W, int H)
{
    if (!w || !rgba || W < 16 || H < 16) return;

    Plate p;
    p.h.px = (float *)malloc(sizeof(float) * (size_t)W * H * 3);
    if (!p.h.px) return;
    p.h.W = W; p.h.H = H;
    /* The camera pulls back as the cell grows.
     *
     * A fixed scale hides the one thing this stage is about. You spend an
     * hour becoming four times the size you started at, and at a fixed scale
     * the only evidence is that the sprite got fatter - the pond looks the
     * same, so the growth reads as a number on the meter rather than as
     * something that happened to you.
     *
     * The exponent is the whole decision. At 1.0 the player holds a constant
     * fraction of the frame, which is correct and feels like nothing at all:
     * you never appear to grow, the world merely shrinks, and the two cancel.
     * Below 1.0 the cancellation is deliberately incomplete - you gain on
     * screen while the water opens up around you - and 0.72 is where being
     * bigger is legible without the far edge of the pond arriving too soon. */
    float grow = w->stats.radius0 > 0.01f ? w->player.r / w->stats.radius0 : 1.0f;
    if (grow < 1.0f) grow = 1.0f;
    p.scale = DROP_SCALE_REF * (float)W / 1280.0f / powf(grow, 0.72f);
    p.h.expo = DROP_EXPOSURE;
    p.h.ui = (float)W / 1280.0f;
    if (p.h.ui < 0.5f) p.h.ui = 0.5f;      /* below this the font stops resolving */
    p.time = (float)w->step * CP_DT;
    p.seed = w->seed;
    p.h.step = w->step;

    float viewW = (float)W / p.scale, viewH = (float)H / p.scale;
    p.camx = dclampf(w->player.x - viewW * 0.5f, 0.0f, CP_WORLD_W - viewW);
    p.camy = dclampf(w->player.y - viewH * 0.5f, 0.0f, CP_WORLD_H - viewH);
    if (CP_WORLD_W < viewW) p.camx = (CP_WORLD_W - viewW) * 0.5f;
    if (CP_WORLD_H < viewH) p.camy = (CP_WORLD_H - viewH) * 0.5f;

    draw_field(&p, w);
    draw_meniscus(&p);
    draw_food(&p, w);

    /* Back to front, so the near cells occlude the far ones and the depth
     * the defocus implies is the depth the draw order gives. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < CP_MAX_CELLS; i++) {
            const CpCell *c = &w->cells[i];
            if (!c->alive) continue;
            if ((cell_depth(w->seed, i) > 0.0f) != (pass == 0)) continue;
            float sx = sxw(&p, c->x), sy = syw(&p, c->y);
            float rad = c->r * p.scale;
            if (sx < -90 || sy < -90 || sx > W + 90 || sy > H + 90) continue;
            draw_npc(&p, w, c, i, sx, sy, rad);
        }
    }

    float psx = sxw(&p, w->player.x), psy = syw(&p, w->player.y);
    float prad = w->player.r * p.scale;
    draw_player(&p, w, psx, psy, prad);

    bloom(&p.h, 1.55f, 0.46f);
    draw_hud(&p, w, viewW, viewH, psx, psy, prad);
    resolve(&p.h, rgba);

    free(p.h.px);
}
