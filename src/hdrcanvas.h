/* Shared HDR canvas - internal to src/, not part of the public API.
 *
 * Two renderers now write continuous tone instead of palette indices, and
 * they need exactly the same three things: a linear float buffer with no
 * ceiling, primitives that resolve coverage as a real number rather than a
 * threshold, and a film chain that decides where the accumulated light lands
 * on a curve. What they do in between - a darkfield plate of pond water, a
 * ray-marched landscape - has nothing in common at all, which is why this is
 * a canvas and a film stock rather than a renderer.
 *
 * Everything is static, so any number of translation units may include it.
 *
 * ---- levels ----
 *
 * Intensities written against this canvas are scene-referred, and the
 * exposure that maps them onto the tonemapping curve is one number held by
 * the caller. A term written as 0.5 means "half of a lit surface", not "half
 * of white". Keeping the two apart is what makes exposure adjustable at all:
 * mixed together, changing how bright a frame is means retuning forty
 * constants.
 */
#ifndef CPORE_HDRCANVAS_H
#define CPORE_HDRCANVAS_H

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pixfont.h"

#ifndef HC_PI
#define HC_PI 3.14159265358979f
#endif

/* ------------------------------------------------------------------ *
 * scalars and colour
 * ------------------------------------------------------------------ */

typedef struct { float r, g, b; } C3;

static inline C3 c3(float r, float g, float b) { C3 c = { r, g, b }; return c; }
static inline C3 cadd(C3 a, C3 b) { return c3(a.r + b.r, a.g + b.g, a.b + b.b); }
static inline C3 cmul(C3 a, C3 b) { return c3(a.r * b.r, a.g * b.g, a.b * b.b); }
static inline C3 cscl(C3 a, float k) { return c3(a.r * k, a.g * k, a.b * k); }

static inline float dclampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float dmixf(float a, float b, float t) { return a + (b - a) * t; }
static inline float sat(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

static inline C3 clerp(C3 a, C3 b, float t)
{
    return c3(dmixf(a.r, b.r, t), dmixf(a.g, b.g, t), dmixf(a.b, b.b, t));
}

/* Hermite between two edges. Used everywhere a term has to arrive without a
 * visible seam - which, once there is no dither to hide one, is everywhere.
 *
 * e1 < e0 is legal and means a descending ramp, which several callers here
 * want: a vignette falls off as the radius grows, and a caustic fades back
 * out past the far wall. Guarding against it as though it were an error - and
 * returning a constant - silently multiplied the entire frame by the far end
 * of the vignette and cost the image every stop above about 15%. */
static inline float sstep(float e0, float e1, float x)
{
    if (e0 == e1) return x < e0 ? 0.0f : 1.0f;
    float t = sat((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

static inline C3 dhsv(float h, float s, float v)
{
    h = h - floorf(h);
    float i = floorf(h * 6.0f), f = h * 6.0f - i;
    float p = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
    switch (((int)i) % 6) {
    case 0:  return c3(v, t, p);
    case 1:  return c3(q, v, p);
    case 2:  return c3(p, v, t);
    case 3:  return c3(p, q, v);
    case 4:  return c3(t, p, v);
    default: return c3(v, p, q);
    }
}

/* ------------------------------------------------------------------ *
 * noise
 * ------------------------------------------------------------------ */

static inline float dhash2(uint32_t s, int x, int y)
{
    uint32_t h = (uint32_t)x * 0x27D4EB2Du ^ (uint32_t)y * 0x9E3779B1u ^ s;
    h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / 16777216.0f;
}

/* Bilinear value noise. Two octaves of it is all the cytoplasm needs: the
 * grain only has to stop an interior reading as moulded plastic. */
static inline float dnoise(uint32_t s, float x, float y)
{
    float fx = floorf(x), fy = floorf(y);
    int ix = (int)fx, iy = (int)fy;
    float tx = x - fx, ty = y - fy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    float a = dhash2(s, ix, iy),     b = dhash2(s, ix + 1, iy);
    float c = dhash2(s, ix, iy + 1), d = dhash2(s, ix + 1, iy + 1);
    return dmixf(dmixf(a, b, tx), dmixf(c, d, tx), ty);
}

static inline float dfbm(uint32_t s, float x, float y)
{
    return dnoise(s, x, y) * 0.62f + dnoise(s ^ 0x51u, x * 2.7f, y * 2.7f) * 0.38f;
}

/* ------------------------------------------------------------------ *
 * the canvas
 *
 * One linear float buffer at the output resolution. No z, no depth sort
 * beyond draw order, and no ceiling: a discharge is allowed to write 40.0
 * into a channel, and the bloom and the tonemapper between them decide what
 * that means.
 * ------------------------------------------------------------------ */

typedef struct {
    float   *px;                  /* W*H*3, linear                          */
    int      W, H;
    float    expo;                /* stops applied at resolve, not at write */
    float    ui;                  /* HUD unit: 1.0 at 1280 wide             */
    int32_t  step;                /* animates the grain                     */
} Hdr;

static inline void hdr_add(Hdr *p, int x, int y, C3 c, float k)
{
    if ((unsigned)x >= (unsigned)p->W || (unsigned)y >= (unsigned)p->H) return;
    float *t = p->px + 3 * ((size_t)y * p->W + x);
    t[0] += c.r * k; t[1] += c.g * k; t[2] += c.b * k;
}

static inline void hdr_mul(Hdr *p, int x, int y, float k)
{
    if ((unsigned)x >= (unsigned)p->W || (unsigned)y >= (unsigned)p->H) return;
    float *t = p->px + 3 * ((size_t)y * p->W + x);
    t[0] *= k; t[1] *= k; t[2] *= k;
}

static inline void hdr_set(Hdr *p, int x, int y, C3 c)
{
    float *t = p->px + 3 * ((size_t)y * p->W + x);
    t[0] = c.r; t[1] = c.g; t[2] = c.b;
}

/* ------------------------------------------------------------------ *
 * primitives
 *
 * Every one of these is analytically antialiased, because the moment the
 * palette and the nearest-neighbour blow-up are gone, a thresholded edge
 * stops reading as a deliberate pixel and starts reading as a mistake.
 *
 * "soft" is the defocus width in pixels. Depth of field is not a post pass
 * here: each sprite knows how far it sits from the focal plane and widens its
 * own coverage falloff, which for round shapes is both cheaper and closer to
 * a real bokeh disc than a screen-space gather would be.
 * ------------------------------------------------------------------ */

/* Coverage of a disc of radius rad at distance d, with an edge `w` wide. */
static inline float cov_disc(float d, float rad, float w)
{
    return sat((rad - d) / w + 0.5f);
}

/* Additive disc. Defocus spreads the same energy over a larger area, so the
 * intensity comes down as it widens - without that, blurring something makes
 * it brighter, and every out-of-focus mote turns into a headlight. */
static inline void d_disc(Hdr *p, float cx, float cy, float rad, C3 col, float inten, float soft)
{
    if (rad <= 0.0f || inten <= 0.0f) return;
    float w = 1.0f + 2.0f * soft;
    float ext = rad + w;
    if (soft > 0.0f) {
        float k = rad / (rad + soft);
        inten *= k * k;
    }
    int x0 = (int)floorf(cx - ext), x1 = (int)ceilf(cx + ext);
    int y0 = (int)floorf(cy - ext), y1 = (int)ceilf(cy + ext);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->W) x1 = p->W - 1;
    if (y1 >= p->H) y1 = p->H - 1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float c = cov_disc(sqrtf(dx * dx + dy * dy), rad, w);
            if (c > 0.0f) hdr_add(p, x, y, col, c * inten);
        }
    }
}

/* ---- opaque compositing ----
 *
 * The additive primitives above draw light: a thing that emits, or scatters,
 * against a dark field. That is the whole grammar of a darkfield plate and it
 * is the wrong grammar for water with the lamp behind it, where a creature is
 * an opaque painted object that *blocks* the light behind it and is lit from
 * one side. Adding a colour can never darken, and multiplying can never
 * change hue, so neither of them can put a matte orange body over pale blue
 * water. That needs a third operator, and this is it.
 *
 * `a` is opacity, so a value below 1 is genuine translucency - a jelly, a
 * bubble wall, a fin thin enough to see the water through. Nothing here is
 * energy-conserving because nothing here is emitting energy; that is the
 * point of the distinction.
 */
static inline void hdr_over(Hdr *p, int x, int y, C3 c, float a)
{
    if ((unsigned)x >= (unsigned)p->W || (unsigned)y >= (unsigned)p->H) return;
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    float *t = p->px + 3 * ((size_t)y * p->W + x);
    t[0] += (c.r - t[0]) * a;
    t[1] += (c.g - t[1]) * a;
    t[2] += (c.b - t[2]) * a;
}

/* An opaque disc, shaded.
 *
 * `lit` is the direction the key comes from, as a unit vector in screen space,
 * and the shading is a hemisphere term rather than a flat fill: a body drawn
 * as one colour reads as a sticker, and the single cheapest thing that makes
 * it read as a rounded object is knowing which side the light is on. The
 * terminator is deliberately soft and the shadow side is tinted toward the
 * water rather than toward black, because in a lit medium the dark side of
 * anything is lit by everything around it.
 */
static inline void d_ball(Hdr *p, float cx, float cy, float rad, C3 col,
                          C3 shade, float lx, float ly, float alpha, float soft)
{
    if (rad <= 0.0f || alpha <= 0.0f) return;
    float w = 1.0f + 2.0f * soft;
    float ext = rad + w;
    int x0 = (int)floorf(cx - ext), x1 = (int)ceilf(cx + ext);
    int y0 = (int)floorf(cy - ext), y1 = (int)ceilf(cy + ext);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->W) x1 = p->W - 1;
    if (y1 >= p->H) y1 = p->H - 1;
    float inv = rad > 0.001f ? 1.0f / rad : 0.0f;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);
            float c = cov_disc(d, rad, w);
            if (c <= 0.0f) continue;
            /* Surface normal of a sphere, read straight off the disc. */
            float nx = dx * inv, ny = dy * inv;
            float nz2 = 1.0f - nx * nx - ny * ny;
            float nz = nz2 > 0.0f ? sqrtf(nz2) : 0.0f;
            float lam = sat(nx * lx + ny * ly + nz * 0.72f);
            float k = 0.42f + 0.58f * lam * lam;
            hdr_over(p, x, y, clerp(shade, col, k), c * alpha);
        }
    }
}

/* An opaque tapered stroke: limbs, spikes, flagella, algae stalks. Radius
 * runs r0 to r1 along the segment, so one call draws a taper rather than a
 * chain of discs that pulses where they overlap. */
static inline void d_fil_over(Hdr *p, float ax, float ay, float bx, float by,
                              float r0, float r1, C3 col, float alpha, float soft)
{
    if (alpha <= 0.0f) return;
    float w = 1.0f + 2.0f * soft;
    float rmax = (r0 > r1 ? r0 : r1) + w;
    float x0f = (ax < bx ? ax : bx) - rmax, x1f = (ax > bx ? ax : bx) + rmax;
    float y0f = (ay < by ? ay : by) - rmax, y1f = (ay > by ? ay : by) + rmax;
    int x0 = (int)floorf(x0f), x1 = (int)ceilf(x1f);
    int y0 = (int)floorf(y0f), y1 = (int)ceilf(y1f);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->W) x1 = p->W - 1;
    if (y1 >= p->H) y1 = p->H - 1;
    float ex = bx - ax, ey = by - ay;
    float el2 = ex * ex + ey * ey;
    if (el2 < 1e-6f) el2 = 1e-6f;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px2 = (float)x + 0.5f - ax, py2 = (float)y + 0.5f - ay;
            float t = (px2 * ex + py2 * ey) / el2;
            t = sat(t);
            float qx = px2 - ex * t, qy = py2 - ey * t;
            float d = sqrtf(qx * qx + qy * qy);
            float r = r0 + (r1 - r0) * t;
            float c = cov_disc(d, r, w);
            if (c > 0.0f) hdr_over(p, x, y, col, c * alpha);
        }
    }
}

/* Occlusion.
 *
 * An additive buffer can only ever make the frame brighter, so a shape that
 * is meant to be dark - a pupil, a mandible, the throat of a jet - cannot be
 * drawn by adding a dark colour: it comes out as a slightly brighter patch
 * of whatever was already there. Those shapes multiply instead, and the light
 * ones that sit on top of them are added afterwards. It is the same two-step
 * the membrane uses, for the same reason. */
static inline void d_occ(Hdr *p, float cx, float cy, float rad, float k, float soft)
{
    if (rad <= 0.0f) return;
    float w = 1.0f + 2.0f * soft;
    float ext = rad + w;
    int x0 = (int)floorf(cx - ext), x1 = (int)ceilf(cx + ext);
    int y0 = (int)floorf(cy - ext), y1 = (int)ceilf(cy + ext);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->W) x1 = p->W - 1;
    if (y1 >= p->H) y1 = p->H - 1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float c = cov_disc(sqrtf(dx * dx + dy * dy), rad, w);
            if (c > 0.0f) hdr_mul(p, x, y, dmixf(1.0f, k, c));
        }
    }
}

/* The same, along a tapered segment: a mandible is an occluder with a lit
 * edge, not a bright stroke. */
static inline void d_fil_occ(Hdr *p, float ax, float ay, float bx, float by,
                      float r0, float r1, float k, float soft)
{
    float w = 1.0f + 2.0f * soft;
    float rmax = (r0 > r1 ? r0 : r1) + w;
    int x0 = (int)floorf((ax < bx ? ax : bx) - rmax), x1 = (int)ceilf((ax > bx ? ax : bx) + rmax);
    int y0 = (int)floorf((ay < by ? ay : by) - rmax), y1 = (int)ceilf((ay > by ? ay : by) + rmax);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->W) x1 = p->W - 1;
    if (y1 >= p->H) y1 = p->H - 1;
    float vx = bx - ax, vy = by - ay;
    float vv = vx * vx + vy * vy;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float wx = (float)x + 0.5f - ax, wy = (float)y + 0.5f - ay;
            float t = vv > 1e-6f ? sat((wx * vx + wy * vy) / vv) : 0.0f;
            float dx = wx - t * vx, dy = wy - t * vy;
            float c = cov_disc(sqrtf(dx * dx + dy * dy), dmixf(r0, r1, t), w);
            if (c > 0.0f) hdr_mul(p, x, y, dmixf(1.0f, k, c));
        }
    }
}

/* A falloff rather than a shape: the halo around anything that emits. Squared
 * so the core stays tight and the skirt stays faint, which is what stops a
 * scene full of glows turning into fog. */
static inline void d_glow(Hdr *p, float cx, float cy, float rad, C3 col, float inten)
{
    if (rad <= 0.5f || inten <= 0.0f) return;
    int x0 = (int)floorf(cx - rad), x1 = (int)ceilf(cx + rad);
    int y0 = (int)floorf(cy - rad), y1 = (int)ceilf(cy + rad);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->W) x1 = p->W - 1;
    if (y1 >= p->H) y1 = p->H - 1;
    float inv = 1.0f / rad;
    for (int y = y0; y <= y1; y++) {
        float dy = ((float)y + 0.5f - cy) * inv;
        for (int x = x0; x <= x1; x++) {
            float dx = ((float)x + 0.5f - cx) * inv;
            float r2 = dx * dx + dy * dy;
            if (r2 >= 1.0f) continue;
            float f = 1.0f - r2;
            hdr_add(p, x, y, col, f * f * inten);
        }
    }
}

/* A tapered capsule, which is every filament in the stage: a cilium, a
 * flagellum, a mandible, a bough of a feeding comb. */
static inline void d_fil(Hdr *p, float ax, float ay, float bx, float by,
                  float r0, float r1, C3 col, float inten, float soft)
{
    float w = 1.0f + 2.0f * soft;
    float rmax = (r0 > r1 ? r0 : r1) + w;
    int x0 = (int)floorf((ax < bx ? ax : bx) - rmax), x1 = (int)ceilf((ax > bx ? ax : bx) + rmax);
    int y0 = (int)floorf((ay < by ? ay : by) - rmax), y1 = (int)ceilf((ay > by ? ay : by) + rmax);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->W) x1 = p->W - 1;
    if (y1 >= p->H) y1 = p->H - 1;
    float vx = bx - ax, vy = by - ay;
    float vv = vx * vx + vy * vy;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float wx = (float)x + 0.5f - ax, wy = (float)y + 0.5f - ay;
            float t = vv > 1e-6f ? sat((wx * vx + wy * vy) / vv) : 0.0f;
            float dx = wx - t * vx, dy = wy - t * vy;
            float rr = dmixf(r0, r1, t);
            float c = cov_disc(sqrtf(dx * dx + dy * dy), rr, w);
            if (c > 0.0f) hdr_add(p, x, y, col, c * inten);
        }
    }
}

/* An arc of a ring, with soft ends. The HUD is built almost entirely out of
 * these: a vitals bar bent around the animal it belongs to reads faster than
 * one parked in a corner, and it costs the frame nothing. */
static inline void d_arc(Hdr *p, float cx, float cy, float rad, float th,
                  float a0, float a1, C3 col, float inten)
{
    float ext = rad + th + 2.0f;
    int x0 = (int)floorf(cx - ext), x1 = (int)ceilf(cx + ext);
    int y0 = (int)floorf(cy - ext), y1 = (int)ceilf(cy + ext);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->W) x1 = p->W - 1;
    if (y1 >= p->H) y1 = p->H - 1;
    int full = (a1 - a0) >= 2.0f * HC_PI - 1e-3f;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);
            float c = sat(th * 0.5f + 0.5f - fabsf(d - rad));
            if (c <= 0.0f) continue;
            if (!full) {
                float a = atan2f(dy, dx);
                while (a < a0) a += 2.0f * HC_PI;
                if (a > a1) {
                    /* soften the cap over roughly a pixel of arc */
                    float over = (a - a1) * (d > 1.0f ? d : 1.0f);
                    if (over > 1.2f) continue;
                    c *= 1.0f - over / 1.2f;
                }
            }
            hdr_add(p, x, y, col, c * inten);
        }
    }
}

/* Dot-matrix text: the same 5x7 table the pixel path uses, with each set bit
 * stamped as a soft dot instead of a hard square. That one substitution is
 * the difference between a retro sprite font and an instrument readout, and
 * it means the HUD belongs to the same optical world as everything else -
 * the dots bloom, so a bright readout glows the way a lit display does. */
static inline void d_text(Hdr *p, float x, float y, float sc, const char *s, C3 col, float inten)
{
    float cx = x;
    for (; *s; s++) {
        const uint8_t *gl = cp_font_glyph((unsigned char)*s);
        for (int col_i = 0; col_i < 5; col_i++)
            for (int row = 0; row < 7; row++)
                if (gl[col_i] & (1u << row))
                    d_disc(p, cx + ((float)col_i + 0.5f) * sc,
                           y + ((float)row + 0.5f) * sc,
                           sc * 0.44f, col, inten, 0.0f);
        cx += 6.0f * sc;
    }
}

static inline float d_textw(const char *s, float sc) { return (float)strlen(s) * 6.0f * sc; }

/* The same text, composited instead of added.
 *
 * A readout over a lit field has the opposite problem to one over a dark
 * field: the additive version cannot draw dark ink at all - d_disc refuses a
 * non-positive intensity, so asking for it by passing a negative one silently
 * draws nothing - and even a bright glyph has no dark surround to read
 * against. On pale water the legible thing is dark type, and dark type is an
 * `over`, not an `add`. */
static inline void d_dot_over(Hdr *p, float cx, float cy, float rad, C3 col, float alpha)
{
    if (rad <= 0.0f || alpha <= 0.0f) return;
    float w = 1.0f;
    float ext = rad + w;
    int x0 = (int)floorf(cx - ext), x1 = (int)ceilf(cx + ext);
    int y0 = (int)floorf(cy - ext), y1 = (int)ceilf(cy + ext);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= p->W) x1 = p->W - 1;
    if (y1 >= p->H) y1 = p->H - 1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float c = cov_disc(sqrtf(dx * dx + dy * dy), rad, w);
            if (c > 0.0f) hdr_over(p, x, y, col, c * alpha);
        }
    }
}

static inline void d_text_over(Hdr *p, float x, float y, float sc, const char *s,
                               C3 col, float alpha)
{
    float cx = x;
    for (; *s; s++) {
        const uint8_t *gl = cp_font_glyph((unsigned char)*s);
        for (int col_i = 0; col_i < 5; col_i++)
            for (int row = 0; row < 7; row++)
                if (gl[col_i] & (1u << row))
                    d_dot_over(p, cx + ((float)col_i + 0.5f) * sc,
                               y + ((float)row + 0.5f) * sc,
                               sc * 0.46f, col, alpha);
        cx += 6.0f * sc;
    }
}

/* A plain filled rule. The only HUD primitive that is not a shape - used for
 * hairlines, frame brackets and minimap borders. */
static inline void hud_rule(Hdr *p, float x, float y, float w, float h, C3 col, float k)
{
    int x0 = (int)x, x1 = (int)(x + w), y0 = (int)y, y1 = (int)(y + h);
    for (int j = y0; j < y1; j++)
        for (int i = x0; i < x1; i++)
            hdr_add(p, i, j, col, k);
}

/* ------------------------------------------------------------------ *
 * post
 * ------------------------------------------------------------------ */

static inline void box_blur(float *src, float *dst, int w, int h, int r)
{
    float *tmp = (float *)malloc(sizeof(float) * (size_t)w * h * 3);
    if (!tmp) { memcpy(dst, src, sizeof(float) * (size_t)w * h * 3); return; }
    float inv = 1.0f / (float)(2 * r + 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float a = 0, b = 0, c = 0;
            for (int k = -r; k <= r; k++) {
                int sx = x + k;
                if (sx < 0) sx = 0;
                if (sx >= w) sx = w - 1;
                const float *s = src + 3 * ((size_t)y * w + sx);
                a += s[0]; b += s[1]; c += s[2];
            }
            float *d = tmp + 3 * ((size_t)y * w + x);
            d[0] = a * inv; d[1] = b * inv; d[2] = c * inv;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float a = 0, b = 0, c = 0;
            for (int k = -r; k <= r; k++) {
                int sy = y + k;
                if (sy < 0) sy = 0;
                if (sy >= h) sy = h - 1;
                const float *s = tmp + 3 * ((size_t)sy * w + x);
                a += s[0]; b += s[1]; c += s[2];
            }
            float *d = dst + 3 * ((size_t)y * w + x);
            d[0] = a * inv; d[1] = b * inv; d[2] = c * inv;
        }
    free(tmp);
}

static inline void halve(const float *src, int sw, int sh, float *dst)
{
    int dw = sw / 2, dh = sh / 2;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            const float *a = src + 3 * ((size_t)(2 * y) * sw + 2 * x);
            const float *b = a + 3;
            const float *c = src + 3 * ((size_t)(2 * y + 1) * sw + 2 * x);
            const float *d = c + 3;
            float *o = dst + 3 * ((size_t)y * dw + x);
            for (int k = 0; k < 3; k++) o[k] = (a[k] + b[k] + c[k] + d[k]) * 0.25f;
        }
}

static inline void upsample_add(const float *src, int sw, int sh,
                         float *dst, int dw, int dh, float k)
{
    for (int y = 0; y < dh; y++) {
        float fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
        int y0 = (int)floorf(fy);
        float ty = fy - (float)y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 < 0) y1 = 0;
        if (y0 >= sh) y0 = sh - 1;
        if (y1 >= sh) y1 = sh - 1;
        for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f;
            int x0 = (int)floorf(fx);
            float tx = fx - (float)x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 < 0) x1 = 0;
            if (x0 >= sw) x0 = sw - 1;
            if (x1 >= sw) x1 = sw - 1;
            const float *a = src + 3 * ((size_t)y0 * sw + x0);
            const float *b = src + 3 * ((size_t)y0 * sw + x1);
            const float *c = src + 3 * ((size_t)y1 * sw + x0);
            const float *d = src + 3 * ((size_t)y1 * sw + x1);
            float *o = dst + 3 * ((size_t)y * dw + x);
            for (int i = 0; i < 3; i++)
                o[i] += k * dmixf(dmixf(a[i], b[i], tx), dmixf(c[i], d[i], tx), ty);
        }
    }
}

/* Bloom, over four octaves.
 *
 * One blur radius gives one halo size, and one halo size reads as a filter.
 * A discharge wants a tight core flare and a wash across a quarter of the
 * frame at the same time, which is four octaves summed - and doing it before
 * the tonemap is the whole point, because after it there is nothing above
 * white left to bleed. */
static inline void bloom(Hdr *p, float thresh, float inten)
{
    /* The threshold is given in resolved units - "brighter than white" - so
     * it has to be divided back through the exposure to become a number the
     * scene-referred buffer can be compared against. Writing it the other way
     * round means every exposure tweak silently retunes the bloom. */
    thresh /= p->expo;
    int W = p->W, H = p->H;
    int w0 = W / 2, h0 = H / 2;
    if (w0 < 8 || h0 < 8) return;

    float *lv[4], *tm[4];
    int lw[4], lh[4];
    lw[0] = w0; lh[0] = h0;
    for (int i = 1; i < 4; i++) { lw[i] = lw[i - 1] / 2; lh[i] = lh[i - 1] / 2; }
    for (int i = 0; i < 4; i++) {
        if (lw[i] < 2 || lh[i] < 2) { lw[i] = 2; lh[i] = 2; }
        lv[i] = (float *)malloc(sizeof(float) * (size_t)lw[i] * lh[i] * 3);
        tm[i] = (float *)malloc(sizeof(float) * (size_t)lw[i] * lh[i] * 3);
        if (!lv[i] || !tm[i]) {
            for (int k = 0; k <= i; k++) { free(lv[k]); free(tm[k]); }
            return;
        }
    }

    /* Soft knee rather than a hard cut: a hard threshold makes anything
     * hovering around it pop in and out between frames. */
    float *full = (float *)malloc(sizeof(float) * (size_t)W * H * 3);
    if (!full) {
        for (int i = 0; i < 4; i++) { free(lv[i]); free(tm[i]); }
        return;
    }
    for (size_t i = 0; i < (size_t)W * H; i++) {
        const float *s = p->px + 3 * i;
        float lum = s[0] * 0.30f + s[1] * 0.59f + s[2] * 0.11f;
        float k = sstep(thresh * 0.55f, thresh * 1.7f, lum);
        for (int c = 0; c < 3; c++) full[3 * i + c] = s[c] * k;
    }

    halve(full, W, H, lv[0]);
    free(full);
    for (int i = 1; i < 4; i++) halve(lv[i - 1], lw[i - 1], lh[i - 1], lv[i]);
    for (int i = 0; i < 4; i++) box_blur(lv[i], tm[i], lw[i], lh[i], 2);

    static const float WGT[4] = { 0.42f, 0.30f, 0.20f, 0.14f };
    for (int i = 0; i < 4; i++) {
        upsample_add(tm[i], lw[i], lh[i], p->px, W, H, WGT[i] * inten);
        free(lv[i]); free(tm[i]);
    }
}

static inline float filmic(float x)
{
    if (x < 0.0f) x = 0.0f;
    float a = x * (2.51f * x + 0.03f);
    float b = x * (2.43f * x + 0.59f) + 0.14f;
    return sat(a / b);
}

/* Resolve.
 *
 * Tonemap, grade, then the three things that say the image came through a
 * lens rather than out of an array: a radial colour split, a falloff toward
 * the corners, and enough grain to keep the near-black field from banding.
 * The grain is not optional at this exposure - the darkest quarter of the
 * frame covers about six 8-bit codes, and without a dither of some kind the
 * condenser cone comes out as contour lines. */
static inline void resolve(Hdr *p, uint8_t *out)
{
    int W = p->W, H = p->H;
    float *ldr = (float *)malloc(sizeof(float) * (size_t)W * H * 3);
    if (!ldr) return;

    float e = p->expo;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        const float *s = p->px + 3 * i;
        float r = filmic(s[0] * e), g = filmic(s[1] * e), b = filmic(s[2] * e);

        /* Grade: shadows toward deep teal, highlights kept cool but allowed
         * to run slightly warm at the very top so a discharge core reads as
         * hot rather than as blue paper. */
        float lum = r * 0.30f + g * 0.59f + b * 0.11f;
        float sh = (1.0f - lum) * (1.0f - lum) * (1.0f - lum);
        r += -0.010f * sh; g += 0.022f * sh; b += 0.040f * sh;
        float hi = lum * lum * lum;
        r += 0.035f * hi; g += 0.012f * hi;

        /* a touch of saturation, since the tonemapper takes some out */
        float l2 = r * 0.30f + g * 0.59f + b * 0.11f;
        r = l2 + (r - l2) * 1.34f;
        g = l2 + (g - l2) * 1.34f;
        b = l2 + (b - l2) * 1.34f;

        ldr[3 * i + 0] = r; ldr[3 * i + 1] = g; ldr[3 * i + 2] = b;
    }

    float cx = W * 0.5f, cy = H * 0.5f;
    float inv = 1.0f / (float)(W < H ? W : H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            float dx = ((float)x - cx) * inv, dy = ((float)y - cy) * inv;
            float r2 = dx * dx + dy * dy;

            /* Lateral chromatic aberration: red and blue focus on slightly
             * different planes, so they land at slightly different radii.
             * Scaled by r^2 because it is nothing on axis and everything at
             * the corner, which is exactly where a cheap objective fails. */
            float k = 0.0026f * r2;
            float rx = cx + ((float)x - cx) * (1.0f + k);
            float ry = cy + ((float)y - cy) * (1.0f + k);
            float bx = cx + ((float)x - cx) * (1.0f - k);
            float by = cy + ((float)y - cy) * (1.0f - k);
            int rxi = (int)dclampf(rx, 0.0f, (float)W - 1.0f);
            int ryi = (int)dclampf(ry, 0.0f, (float)H - 1.0f);
            int bxi = (int)dclampf(bx, 0.0f, (float)W - 1.0f);
            int byi = (int)dclampf(by, 0.0f, (float)H - 1.0f);

            float cr = ldr[3 * ((size_t)ryi * W + rxi) + 0];
            float cg = ldr[3 * ((size_t)y * W + x) + 1];
            float cb = ldr[3 * ((size_t)byi * W + bxi) + 2];

            float vig = sstep(1.28f, 0.34f, sqrtf(r2));
            vig = 0.14f + 0.86f * vig;
            cr *= vig; cg *= vig; cb *= vig;

            float gr = (dhash2((uint32_t)p->step * 2654435761u, x, y) - 0.5f) * 0.016f;
            cr += gr; cg += gr; cb += gr;

            uint8_t *o = out + 4 * ((size_t)y * W + x);
            o[0] = (uint8_t)(sat(cr) * 255.0f + 0.5f);
            o[1] = (uint8_t)(sat(cg) * 255.0f + 0.5f);
            o[2] = (uint8_t)(sat(cb) * 255.0f + 0.5f);
            o[3] = 255;
        }
    }
    free(ldr);
}


#endif /* CPORE_HDRCANVAS_H */
