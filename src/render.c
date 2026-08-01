#include "cpore/cpore.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Software rasteriser. Reads world state and writes RGBA8; it never touches
 * the simulation, so training runs can skip this translation unit entirely. */

/* Pixel-art pipeline. Everything is drawn into a small fixed-size buffer with
 * hard edges, quantised to a 32-colour palette with ordered dithering, then
 * blown up with nearest-neighbour. No analytic antialiasing anywhere: a pixel
 * is either the shape's colour or it is not. */
#define PI 3.14159265358979f

#define PIX_W   320
#define PIX_H   180
#define WSCALE  0.30f          /* pixels per world unit */

typedef struct { uint8_t *fb; int W, H; } Canvas;

/* Coverage is thresholded at the pixel centre rather than blended, which is
 * the entire difference between a soft vector look and a pixel one. */
static inline float hard(float cov) { return cov >= 0.5f ? 1.0f : 0.0f; }

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

/* ---------------- pixel ops ---------------- */

static inline void px_blend(Canvas *c, int x, int y, float r, float g, float b, float a)
{
    if ((unsigned)x >= (unsigned)c->W || (unsigned)y >= (unsigned)c->H) return;
    if (a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    uint8_t *p = c->fb + 4 * ((size_t)y * c->W + x);
    p[0] = (uint8_t)(p[0] + (clampf(r, 0, 1) * 255.0f - p[0]) * a);
    p[1] = (uint8_t)(p[1] + (clampf(g, 0, 1) * 255.0f - p[1]) * a);
    p[2] = (uint8_t)(p[2] + (clampf(b, 0, 1) * 255.0f - p[2]) * a);
}

static inline void px_add(Canvas *c, int x, int y, float r, float g, float b, float a)
{
    if ((unsigned)x >= (unsigned)c->W || (unsigned)y >= (unsigned)c->H) return;
    if (a <= 0.0f) return;
    uint8_t *p = c->fb + 4 * ((size_t)y * c->W + x);
    float nr = p[0] + r * 255.0f * a;
    float ng = p[1] + g * 255.0f * a;
    float nb = p[2] + b * 255.0f * a;
    p[0] = (uint8_t)(nr > 255.0f ? 255.0f : nr);
    p[1] = (uint8_t)(ng > 255.0f ? 255.0f : ng);
    p[2] = (uint8_t)(nb > 255.0f ? 255.0f : nb);
}

/* ---------------- primitives (all analytically antialiased) ---------------- */

static void disc(Canvas *c, float cx, float cy, float rad, float r, float g, float b, float a)
{
    if (rad <= 0.0f) return;
    int x0 = (int)floorf(cx - rad - 1), x1 = (int)ceilf(cx + rad + 1);
    int y0 = (int)floorf(cy - rad - 1), y1 = (int)ceilf(cy + rad + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->W) x1 = c->W - 1;
    if (y1 >= c->H) y1 = c->H - 1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d = sqrtf(dx * dx + dy * dy);
            float cov = hard(clampf(rad + 0.5f - d, 0.0f, 1.0f));
            if (cov > 0.0f) px_blend(c, x, y, r, g, b, a);
        }
    }
}

static void ring(Canvas *c, float cx, float cy, float rad, float th,
                 float r, float g, float b, float a)
{
    float outer = rad + th * 0.5f + 1.0f;
    int x0 = (int)floorf(cx - outer), x1 = (int)ceilf(cx + outer);
    int y0 = (int)floorf(cy - outer), y1 = (int)ceilf(cy + outer);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->W) x1 = c->W - 1;
    if (y1 >= c->H) y1 = c->H - 1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float d = fabsf(sqrtf(dx * dx + dy * dy) - rad);
            float cov = hard(clampf(th * 0.5f + 0.5f - d, 0.0f, 1.0f));
            if (cov > 0.0f) px_blend(c, x, y, r, g, b, a);
        }
    }
}

/* A stepped halo rather than a falloff. Three hard bands read as deliberate
 * shading at this resolution; a smooth gradient just reads as blur. */
static void glow(Canvas *c, float cx, float cy, float rad, float r, float g, float b, float inten)
{
    if (rad < 1.0f || inten <= 0.0f) return;
    static const float band[3] = { 1.00f, 0.72f, 0.44f };
    static const float amt[3]  = { 0.16f, 0.30f, 0.52f };
    for (int k = 0; k < 3; k++) {
        float rr = rad * band[k];
        int x0 = (int)floorf(cx - rr), x1 = (int)ceilf(cx + rr);
        int y0 = (int)floorf(cy - rr), y1 = (int)ceilf(cy + rr);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= c->W) x1 = c->W - 1;
        if (y1 >= c->H) y1 = c->H - 1;
        float rr2 = rr * rr;
        for (int y = y0; y <= y1; y++) {
            float dy = (float)y + 0.5f - cy;
            for (int x = x0; x <= x1; x++) {
                float dx = (float)x + 0.5f - cx;
                if (dx * dx + dy * dy > rr2) continue;
                px_add(c, x, y, r, g, b, inten * amt[k] * 0.5f);
            }
        }
    }
}

/* one primitive for cilia, spikes and flagella: a tapered capsule */
static void capsule(Canvas *c, float ax, float ay, float bx, float by,
                    float r0, float r1, float r, float g, float b, float a)
{
    float rmax = (r0 > r1 ? r0 : r1) + 1.0f;
    int x0 = (int)floorf((ax < bx ? ax : bx) - rmax), x1 = (int)ceilf((ax > bx ? ax : bx) + rmax);
    int y0 = (int)floorf((ay < by ? ay : by) - rmax), y1 = (int)ceilf((ay > by ? ay : by) + rmax);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->W) x1 = c->W - 1;
    if (y1 >= c->H) y1 = c->H - 1;

    float vx = bx - ax, vy = by - ay;
    float vv = vx * vx + vy * vy;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float wx = (float)x + 0.5f - ax, wy = (float)y + 0.5f - ay;
            float t = vv > 1e-6f ? (wx * vx + wy * vy) / vv : 0.0f;
            t = clampf(t, 0.0f, 1.0f);
            float dx = wx - t * vx, dy = wy - t * vy;
            float d = sqrtf(dx * dx + dy * dy);
            float rr = r0 + (r1 - r0) * t;
            float cov = hard(clampf(rr + 0.5f - d, 0.0f, 1.0f));
            if (cov > 0.0f) px_blend(c, x, y, r, g, b, a);
        }
    }
}

static void rect_fill(Canvas *c, int x, int y, int w, int h, float r, float g, float b, float a)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            px_blend(c, i, j, r, g, b, a);
}

/* ---------------- 5x7 bitmap font (columns, bit0 = top row) ---------------- */

static const uint8_t FONT[96][5] = {
{0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},{0x00,0x07,0x00,0x07,0x00},
{0x14,0x7F,0x14,0x7F,0x14},{0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
{0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},{0x00,0x1C,0x22,0x41,0x00},
{0x00,0x41,0x22,0x1C,0x00},{0x14,0x08,0x3E,0x08,0x14},{0x08,0x08,0x3E,0x08,0x08},
{0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},{0x00,0x60,0x60,0x00,0x00},
{0x20,0x10,0x08,0x04,0x02},{0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},{0x18,0x14,0x12,0x7F,0x10},
{0x27,0x45,0x45,0x45,0x39},{0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
{0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},{0x00,0x36,0x36,0x00,0x00},
{0x00,0x56,0x36,0x00,0x00},{0x08,0x14,0x22,0x41,0x00},{0x14,0x14,0x14,0x14,0x14},
{0x00,0x41,0x22,0x14,0x08},{0x02,0x01,0x51,0x09,0x06},{0x32,0x49,0x79,0x41,0x3E},
{0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
{0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
{0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
{0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
{0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
{0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
{0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
{0x1F,0x20,0x40,0x20,0x1F},{0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
{0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},{0x00,0x7F,0x41,0x41,0x00},
{0x02,0x04,0x08,0x10,0x20},{0x00,0x41,0x41,0x7F,0x00},{0x04,0x02,0x01,0x02,0x04},
{0x40,0x40,0x40,0x40,0x40},{0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
{0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},{0x38,0x44,0x44,0x48,0x7F},
{0x38,0x54,0x54,0x54,0x18},{0x08,0x7E,0x09,0x01,0x02},{0x0C,0x52,0x52,0x52,0x3E},
{0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},{0x20,0x40,0x44,0x3D,0x00},
{0x7F,0x10,0x28,0x44,0x00},{0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
{0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},{0x7C,0x14,0x14,0x14,0x08},
{0x08,0x14,0x14,0x18,0x7C},{0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
{0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},{0x1C,0x20,0x40,0x20,0x1C},
{0x3C,0x40,0x30,0x40,0x3C},{0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
{0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},{0x00,0x00,0x7F,0x00,0x00},
{0x00,0x41,0x36,0x08,0x00},{0x08,0x08,0x2A,0x1C,0x08},{0x00,0x00,0x00,0x00,0x00}};

static int text_w(const char *s, int sc) { return (int)strlen(s) * 6 * sc; }

static void text(Canvas *c, int px_, int py, int sc, const char *s,
                 float r, float g, float b, float a)
{
    int cx = px_;
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7F) ch = '?';
        const uint8_t *gl = FONT[ch - 0x20];
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (gl[col] & (1u << row))
                    rect_fill(c, cx + col * sc, py + row * sc, sc, sc, r, g, b, a);
        cx += 6 * sc;
    }
}

/* ---------------- colour ---------------- */

static void hsv(float h, float s, float v, float *r, float *g, float *b)
{
    h = h - floorf(h);
    float i = floorf(h * 6.0f);
    float f = h * 6.0f - i;
    float p = v * (1.0f - s), q = v * (1.0f - f * s), t = v * (1.0f - (1.0f - f) * s);
    switch (((int)i) % 6) {
        case 0: *r = v; *g = t; *b = p; break;
        case 1: *r = q; *g = v; *b = p; break;
        case 2: *r = p; *g = v; *b = t; break;
        case 3: *r = p; *g = q; *b = v; break;
        case 4: *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

/* ---------------- creature drawing ---------------- */

typedef struct { float r, g, b; } Col;

/* ---------------- parts ----------------
 * Every part is drawn at its own mounting angle, because that angle is what
 * the simulation reads. If a spike looks like it is on the back, it is on the
 * back, and it will not hit anything in front of you. */

#define PART_UNDER 0   /* drawn beneath the membrane: protrusions   */
#define PART_OVER  1   /* drawn on top: surface organs              */

static int part_layer(int type)
{
    switch (type) {
    case CP_PART_EYE: case CP_PART_ELECTRIC: case CP_PART_POISON: return PART_OVER;
    default: return PART_UNDER;
    }
}

static void draw_part(Canvas *c, float cx, float cy, float rad, float wa,
                      int type, float phase, Col acc, float flash)
{
    float ux = cosf(wa), uy = sinf(wa);
    float bx = cx + ux * rad * 0.92f, by = cy + uy * rad * 0.92f;
    float wave = sinf(phase * 2.0f + wa * 3.0f);

    switch (type) {
    case CP_PART_FILTER: {                      /* a comb of feeding filaments */
        for (int i = -1; i <= 1; i++) {
            float a = wa + i * 0.34f;
            float ln = rad * (0.50f + 0.10f * sinf(phase * 3.0f + i));
            capsule(c, bx, by, bx + cosf(a) * ln, by + sinf(a) * ln,
                    rad * 0.055f + 0.6f, 0.4f, 0.80f, 0.98f, 0.80f, 0.85f);
        }
        disc(c, bx, by, rad * 0.16f, 0.70f, 0.95f, 0.78f, 0.70f);
        break;
    }
    case CP_PART_JAW: {                         /* two mandibles, chewing */
        float gap = 0.20f + 0.11f * (0.5f + 0.5f * sinf(phase * 5.0f));
        for (int i = -1; i <= 1; i += 2) {
            float a = wa + i * gap;
            capsule(c, bx - ux * rad * 0.10f, by - uy * rad * 0.10f,
                    bx + cosf(a) * rad * 0.72f, by + sinf(a) * rad * 0.72f,
                    rad * 0.17f, rad * 0.035f + 0.4f, 0.96f, 0.93f, 0.84f, 0.96f);
        }
        break;
    }
    case CP_PART_PROBOSCIS: {                   /* a straw with a soft tip */
        float ln = rad * (0.85f + 0.10f * wave);
        capsule(c, bx - ux * rad * 0.1f, by - uy * rad * 0.1f,
                bx + ux * ln, by + uy * ln,
                rad * 0.13f, rad * 0.09f, 0.86f, 0.72f, 0.95f, 0.92f);
        disc(c, bx + ux * ln, by + uy * ln, rad * 0.13f, 0.98f, 0.86f, 1.0f, 0.92f);
        break;
    }
    case CP_PART_CILIA: {                       /* a little oar */
        float ln = rad * 0.30f * (0.78f + 0.30f * wave);
        float bend = wave * 0.40f;
        capsule(c, bx, by, bx + cosf(wa + bend) * ln, by + sinf(wa + bend) * ln,
                0.9f, 0.5f, acc.r, acc.g, acc.b, 1.0f);
        break;
    }
    case CP_PART_FLAGELLA: {                    /* a long trailing whip */
        float x = bx, y = by, a = wa, seg = rad * 0.38f;
        for (int k = 0; k < 4; k++) {
            a += sinf(phase * 3.0f - k * 0.9f) * 0.40f;
            float nx = x + cosf(a) * seg, ny = y + sinf(a) * seg;
            float wdt = 1.4f - k * 0.25f;
            capsule(c, x, y, nx, ny, wdt, wdt * 0.75f, acc.r, acc.g, acc.b, 1.0f);
            x = nx; y = ny;
        }
        break;
    }
    case CP_PART_JET: {                         /* nozzle plus exhaust plume */
        float tx = bx + ux * rad * 0.42f, ty = by + uy * rad * 0.42f;
        capsule(c, bx - ux * rad * 0.08f, by - uy * rad * 0.08f, tx, ty,
                rad * 0.24f, rad * 0.17f, 0.72f, 0.78f, 0.88f, 0.95f);
        ring(c, tx, ty, rad * 0.16f, 1.6f, 0.90f, 0.95f, 1.0f, 0.75f);
        float plume = 0.55f + 0.45f * sinf(phase * 9.0f);
        glow(c, tx + ux * rad * 0.35f, ty + uy * rad * 0.35f, rad * 0.75f,
             0.35f, 0.72f, 1.0f, 0.55f * plume);
        break;
    }
    case CP_PART_SPIKE: {                       /* the reason facing matters */
        float t0x = cx + ux * rad * 0.70f, t0y = cy + uy * rad * 0.70f;
        float t1x = cx + ux * rad * 1.62f, t1y = cy + uy * rad * 1.62f;
        capsule(c, t0x, t0y, t1x, t1y, rad * 0.20f, 0.4f, 0.94f, 0.90f, 0.80f, 0.94f);
        capsule(c, t0x, t0y, t1x, t1y, rad * 0.11f, 0.3f, 1.0f, 1.0f, 1.0f, 0.55f);
        break;
    }
    case CP_PART_ELECTRIC: {                    /* bulb, and arcs when it fires */
        float ex = cx + ux * rad * 0.80f, ey = cy + uy * rad * 0.80f;
        glow(c, ex, ey, rad * (0.55f + flash * 2.4f), 0.35f, 0.75f, 1.0f, 0.55f + flash * 2.0f);
        disc(c, ex, ey, rad * 0.20f, 0.55f, 0.88f, 1.0f, 0.95f);
        disc(c, ex, ey, rad * 0.11f, 1.0f, 1.0f, 1.0f, 0.95f);
        if (flash > 0.0f) {
            for (int i = 0; i < 5; i++) {
                float a = wa + (i - 2) * 0.55f + sinf(phase * 20.0f + i) * 0.25f;
                float ln = rad * (0.9f + 0.6f * (float)((i * 7) % 3));
                capsule(c, ex, ey, ex + cosf(a) * ln, ey + sinf(a) * ln,
                        1.6f, 0.5f, 0.75f, 0.95f, 1.0f, 0.85f);
            }
        }
        break;
    }
    case CP_PART_POISON: {                      /* sacs that punish a biter */
        float ex = cx + ux * rad * 0.78f, ey = cy + uy * rad * 0.78f;
        glow(c, ex, ey, rad * 0.70f, 0.55f, 0.95f, 0.20f, 0.45f);
        disc(c, ex, ey, rad * 0.21f, 0.52f, 0.88f, 0.22f, 0.95f);
        disc(c, ex - ux * rad * 0.05f, ey - uy * rad * 0.05f, rad * 0.09f,
             0.85f, 1.0f, 0.55f, 0.85f);
        break;
    }
    case CP_PART_EYE: {                         /* perception, made visible */
        float ex = cx + ux * rad * 0.68f, ey = cy + uy * rad * 0.68f;
        disc(c, ex, ey, rad * 0.21f, 0.97f, 0.98f, 1.0f, 0.97f);
        disc(c, ex + ux * rad * 0.07f, ey + uy * rad * 0.07f, rad * 0.10f,
             0.06f, 0.09f, 0.14f, 0.97f);
        disc(c, ex - ux * rad * 0.04f, ey - uy * rad * 0.06f, rad * 0.045f,
             1.0f, 1.0f, 1.0f, 0.8f);
        break;
    }
    default: break;
    }
}

/* membrane, organelles and the player's marker ring */
static void draw_body(Canvas *c, float sx, float sy, float rad, float heading,
                      Col body, Col acc, int is_player)
{
    /* hard dark keyline, then the fill, then one highlight - the classic
     * three-step pixel read. Without the keyline everything dissolves into
     * the water at this resolution. */
    disc(c, sx, sy, rad, 0.02f, 0.05f, 0.08f, 1.0f);
    disc(c, sx, sy, rad - 1.0f, body.r * 0.62f, body.g * 0.62f, body.b * 0.62f, 1.0f);
    disc(c, sx - rad * 0.16f, sy - rad * 0.18f, rad * 0.76f, body.r, body.g, body.b, 1.0f);

    if (rad >= 5.0f) {
        disc(c, sx + rad * 0.20f, sy + rad * 0.16f, rad * 0.32f,
             acc.r * 0.60f, acc.g * 0.60f, acc.b * 0.72f, 1.0f);
        disc(c, sx - rad * 0.34f, sy - rad * 0.34f, rad * 0.17f, 1.0f, 1.0f, 1.0f, 0.85f);
    }

    if (is_player) {
        /* Four corner brackets rather than concentric rings: at 320x180 a ring
         * around a 10px cell is just noise on top of the cell. */
        int d = (int)(rad + 4.0f);
        int cx = (int)sx, cy = (int)sy;
        for (int qy = -1; qy <= 1; qy += 2) {
            for (int qx = -1; qx <= 1; qx += 2) {
                rect_fill(c, cx + qx * d - (qx < 0 ? 0 : 2), cy + qy * d, 3, 1,
                          0.78f, 1.0f, 0.98f, 1.0f);
                rect_fill(c, cx + qx * d, cy + qy * d - (qy < 0 ? 0 : 2), 1, 3,
                          0.78f, 1.0f, 0.98f, 1.0f);
            }
        }
        /* facing pip */
        rect_fill(c, (int)(sx + cosf(heading) * (rad + 2.0f)) - 1,
                  (int)(sy + sinf(heading) * (rad + 2.0f)) - 1, 2, 2,
                  1.0f, 1.0f, 1.0f, 1.0f);
    }
}

/* the player: parts come straight off the genome, at their real angles */
static void draw_player(Canvas *c, const CpWorld *w, float sx, float sy, float rad)
{
    const CpCell *p = &w->player;
    Col body = { 0.36f, 0.86f, 0.94f }, acc = { 0.82f, 0.98f, 1.0f };
    if (!p->alive) { body.r = 0.35f; body.g = 0.35f; body.b = 0.40f; }
    float flash = w->elec_flash > 0.0f ? w->elec_flash * 5.0f : 0.0f;

    glow(c, sx, sy, rad * 3.1f, body.r * 0.5f, body.g * 0.5f, body.b * 0.5f, 0.30f);

    for (int layer = 0; layer <= 1; layer++) {
        if (layer == 1) draw_body(c, sx, sy, rad, p->heading, body, acc, 1);
        for (int i = 0; i < CP_MAX_PARTS; i++) {
            int t = w->genome.part[i].type;
            if (t == CP_PART_NONE) continue;
            if (part_layer(t) != layer) continue;
            float wa = p->heading + (float)w->genome.part[i].angle * (2.0f * PI / 256.0f);
            draw_part(c, sx, sy, rad, wa, t, p->phase, acc, flash);
        }
    }

    /* discharge shockwave, at the radius the simulation actually used */
    if (w->elec_flash > 0.0f && w->stats.elec_radius > 0.0f) {
        float k = w->elec_flash / 0.20f;
        float er = w->stats.elec_radius * WSCALE;
        ring(c, sx, sy, er * (1.15f - 0.15f * k), 1.5f, 0.65f, 0.92f, 1.0f, 0.9f);
        ring(c, sx, sy, er * 0.72f, 1.0f, 0.85f, 0.96f, 1.0f, 0.6f * k);
    }
}

/* npc cells get the same part vocabulary, synthesised from their counts */
static void draw_npc(Canvas *c, const CpCell *n, float sx, float sy, float rad)
{
    Col body, acc;
    hsv(n->hue, 0.58f, 0.72f, &body.r, &body.g, &body.b);
    hsv(n->hue + 0.06f, 0.42f, 1.0f, &acc.r, &acc.g, &acc.b);

    float head = (n->vx * n->vx + n->vy * n->vy) > 1.0f
               ? atan2f(n->vy, n->vx) : n->wander_a;

    glow(c, sx, sy, rad * 3.1f, body.r * 0.5f, body.g * 0.5f, body.b * 0.5f, 0.16f);

    if (rad < 4.0f) {          /* too small to carry detail at this resolution */
        disc(c, sx, sy, rad, 0.02f, 0.05f, 0.08f, 1.0f);
        disc(c, sx, sy, rad - 1.0f, body.r, body.g, body.b, 1.0f);
        return;
    }

    int ncil = 6 + n->cilia * 2;
    if (ncil > 18) ncil = 18;
    for (int i = 0; i < ncil; i++)
        draw_part(c, sx, sy, rad, head + (float)i / ncil * 2.0f * PI,
                  CP_PART_CILIA, n->phase, acc, 0.0f);
    for (int i = 0; i < n->spikes; i++)
        draw_part(c, sx, sy, rad, head + (float)i / (n->spikes ? n->spikes : 1) * 2.0f * PI,
                  CP_PART_SPIKE, n->phase, acc, 0.0f);
    for (int i = 0; i < n->jaws; i++)
        draw_part(c, sx, sy, rad, head + (i ? PI : 0.0f),
                  n->diet == CP_DIET_HERB ? CP_PART_FILTER : CP_PART_JAW,
                  n->phase, acc, 0.0f);

    draw_body(c, sx, sy, rad, head, body, acc, 0);

    for (int i = 0; i < n->eyes; i++)
        draw_part(c, sx, sy, rad, head + (i - (n->eyes - 1) * 0.5f) * 0.55f,
                  CP_PART_EYE, n->phase, acc, 0.0f);
    if (n->poison > 0.0f)
        draw_part(c, sx, sy, rad, head + PI, CP_PART_POISON, n->phase, acc, 0.0f);
}

/* ---------------- HUD ---------------- */

static void panel(Canvas *c, int x, int y, int w, int h)
{
    rect_fill(c, x, y, w, h, 0.04f, 0.09f, 0.13f, 0.86f);
    rect_fill(c, x, y, w, 1, 0.32f, 0.62f, 0.66f, 0.55f);
    rect_fill(c, x, y + h - 1, w, 1, 0.10f, 0.24f, 0.30f, 0.9f);
}

static void bar(Canvas *c, int x, int y, int w, int h, float frac,
                float r, float g, float b)
{
    frac = clampf(frac, 0.0f, 1.0f);
    rect_fill(c, x, y, w, h, 0.06f, 0.12f, 0.15f, 1.0f);
    int fw = (int)(w * frac + 0.5f);
    if (fw > 0) {
        rect_fill(c, x, y, fw, h, r, g, b, 1.0f);
        rect_fill(c, x, y, fw, 1, r * 1.35f + 0.15f, g * 1.35f + 0.15f, b * 1.35f + 0.15f, 1.0f);
    }
}

static void draw_minimap(Canvas *c, const CpWorld *w, float camx, float camy,
                         float viewW, float viewH)
{
    const int mw = 62, mh = 36;
    const int mx = c->W - mw - 3, my = 3;
    panel(c, mx - 1, my - 1, mw + 2, mh + 2);

    float sx = (float)mw / CP_WORLD_W, sy = (float)mh / CP_WORLD_H;
    rect_fill(c, mx, my, mw, mh, 0.03f, 0.08f, 0.11f, 1.0f);

    for (int i = 0; i < CP_MAX_FOOD; i += 4) {
        const CpFood *f = &w->food[i];
        if (f->type == CP_FOOD_NONE) continue;
        int px_ = mx + (int)(f->x * sx), py = my + (int)(f->y * sy);
        if (f->type == CP_FOOD_PLANT) px_blend(c, px_, py, 0.30f, 0.62f, 0.34f, 1.0f);
        else                          px_blend(c, px_, py, 0.75f, 0.36f, 0.24f, 1.0f);
    }
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        const CpCell *cc = &w->cells[i];
        if (!cc->alive) continue;
        float r, g, b;
        hsv(cc->hue, 0.62f, 0.92f, &r, &g, &b);
        rect_fill(c, mx + (int)(cc->x * sx), my + (int)(cc->y * sy), 1, 1, r, g, b, 1.0f);
    }

    /* viewport box */
    int vx = mx + (int)(camx * sx), vy = my + (int)(camy * sy);
    int vw = (int)(viewW * sx), vh = (int)(viewH * sy);
    rect_fill(c, vx, vy, vw, 1, 0.70f, 0.90f, 0.92f, 0.55f);
    rect_fill(c, vx, vy + vh, vw, 1, 0.70f, 0.90f, 0.92f, 0.55f);
    rect_fill(c, vx, vy, 1, vh, 0.70f, 0.90f, 0.92f, 0.55f);
    rect_fill(c, vx + vw, vy, 1, vh, 0.70f, 0.90f, 0.92f, 0.55f);

    int px_ = mx + (int)(w->player.x * sx), py = my + (int)(w->player.y * sy);
    rect_fill(c, px_ - 1, py, 3, 1, 0.75f, 1.0f, 1.0f, 1.0f);
    rect_fill(c, px_, py - 1, 1, 3, 0.75f, 1.0f, 1.0f, 1.0f);
}

/* one colour per part type, shared by the strip and the placement dial so the
 * two readouts agree at a glance */
static void part_colour(int t, float *r, float *g, float *b)
{
    switch (t) {
    case CP_PART_FILTER:    *r=0.55f; *g=0.92f; *b=0.60f; break;
    case CP_PART_JAW:       *r=0.95f; *g=0.90f; *b=0.72f; break;
    case CP_PART_PROBOSCIS: *r=0.80f; *g=0.66f; *b=0.95f; break;
    case CP_PART_CILIA:     *r=0.45f; *g=0.82f; *b=0.90f; break;
    case CP_PART_FLAGELLA:  *r=0.35f; *g=0.66f; *b=0.92f; break;
    case CP_PART_JET:       *r=0.60f; *g=0.76f; *b=0.95f; break;
    case CP_PART_SPIKE:     *r=0.95f; *g=0.82f; *b=0.55f; break;
    case CP_PART_ELECTRIC:  *r=0.55f; *g=0.88f; *b=1.00f; break;
    case CP_PART_POISON:    *r=0.60f; *g=0.92f; *b=0.30f; break;
    case CP_PART_EYE:       *r=0.95f; *g=0.95f; *b=0.98f; break;
    default:                *r=0.30f; *g=0.36f; *b=0.40f; break;
    }
}

static void draw_hud(Canvas *c, const CpWorld *w, float camx, float camy,
                     float viewW, float viewH)
{
    char buf[64];
    const CpCell *p = &w->player;

    /* --- vitals, top left --- */
    panel(c, 3, 3, 104, 32);
    text(c, 6, 6, 1, "CELL STAGE", 0.62f, 0.92f, 0.96f, 1.0f);
    snprintf(buf, sizeof(buf), "G%d/%d T%d", w->generation + 1, CP_GENERATIONS, w->step);
    text(c, 6, 15, 1, buf, 0.42f, 0.58f, 0.64f, 1.0f);
    bar(c, 6, 24, 46, 3, p->hp / p->hp_max, 0.86f, 0.28f, 0.30f);
    bar(c, 55, 24, 46, 3, w->dna / CP_DNA_GOAL, 0.38f, 0.82f, 0.46f);

    /* --- body plan, bottom left: a swatch and a count per owned part --- */
    panel(c, 3, PIX_H - 34, 132, 31);
    snprintf(buf, sizeof(buf), "%d DNA  %d PARTS", (int)w->stats.cost, w->stats.n_parts);
    text(c, 6, PIX_H - 31, 1, buf, 0.50f, 0.74f, 0.78f, 1.0f);

    int col = 0;
    for (int t = 1; t < CP_PART_COUNT; t++) {
        int n = w->stats.n[t];
        if (!n) continue;
        int x = 6 + (col % 5) * 19;
        int y = PIX_H - 21 + (col / 5) * 9;
        float r, g, b;
        part_colour(t, &r, &g, &b);
        rect_fill(c, x, y, 5, 5, r, g, b, 1.0f);
        rect_fill(c, x, y, 5, 1, r * 1.3f, g * 1.3f, b * 1.3f, 1.0f);
        snprintf(buf, sizeof(buf), "%d", n);
        text(c, x + 7, y - 1, 1, buf, 0.78f, 0.86f, 0.88f, 1.0f);
        col++;
    }

    /* --- placement dial: where the parts actually sit. Front points right,
     *     matching the facing tick on the cell itself. --- */
    {
        float dx = 118.0f, dy = (float)PIX_H - 18.0f, dr = 12.0f;
        ring(c, dx, dy, dr, 1.0f, 0.24f, 0.44f, 0.50f, 1.0f);
        rect_fill(c, (int)(dx + dr - 3), (int)dy, 5, 1, 0.55f, 0.82f, 0.86f, 1.0f);
        for (int i = 0; i < CP_MAX_PARTS; i++) {
            int t = w->genome.part[i].type;
            if (t == CP_PART_NONE) continue;
            float a = (float)w->genome.part[i].angle * (2.0f * PI / 256.0f);
            float r, g, b;
            part_colour(t, &r, &g, &b);
            rect_fill(c, (int)(dx + cosf(a) * dr) - 1, (int)(dy + sinf(a) * dr) - 1,
                      3, 3, r, g, b, 1.0f);
        }
    }

    draw_minimap(c, w, camx, camy, viewW, viewH);

    /* --- episode counters, under the minimap --- */
    {
        int x = c->W - 65, y = 43;
        panel(c, x - 1, y - 1, 64, 32);
        snprintf(buf, sizeof(buf), "EAT %d", w->ate_plant + w->ate_meat);
        text(c, x + 2, y + 2, 1, buf, 0.66f, 0.76f, 0.78f, 1.0f);
        snprintf(buf, sizeof(buf), "KILL %d", w->kills);
        text(c, x + 2, y + 10, 1, buf, 0.66f, 0.76f, 0.78f, 1.0f);
        snprintf(buf, sizeof(buf), "HIT %d", w->hits_taken);
        text(c, x + 2, y + 18, 1, buf, 0.66f, 0.76f, 0.78f, 1.0f);
    }

    /* --- terminal banner --- */
    if (w->status != CP_RUN) {
        const char *msg = w->status == CP_EVOLVED ? "EVOLVE - CREATURE STAGE"
                        : w->status == CP_DEAD    ? "CONSUMED"
                                                  : "TIME UP";
        int tw = text_w(msg, 1);
        int bx = (c->W - tw) / 2, by = c->H / 2 - 7;
        panel(c, bx - 6, by - 4, tw + 12, 16);
        text(c, bx, by, 1, msg,
             w->status == CP_EVOLVED ? 0.52f : 1.0f,
             w->status == CP_EVOLVED ? 0.95f : 0.46f, 0.60f, 1.0f);
    }
}

/* ---------------- palette + dithering ---------------- */

/* 32 colours, grouped as ramps so shading stays inside a hue instead of
 * wandering across the palette when it is quantised. */
static const uint8_t PAL[32][3] = {
    {  6, 12, 20}, { 10, 22, 34}, { 14, 34, 48}, { 20, 48, 62},
    { 28, 62, 78}, { 38, 80, 94}, { 52,102,114}, { 74,128,138},
    { 26, 64, 38}, { 44,104, 56}, { 78,158, 74}, {132,206,104},
    {186,232,140}, { 92, 34, 30}, {148, 54, 40}, {198, 90, 56},
    {236,148, 96}, { 18, 62, 84}, { 34,104,134}, { 62,160,190},
    {126,214,232}, {198,244,250}, { 86, 28, 54}, {138, 44, 80},
    {192, 80,120}, {232,140,172}, { 56, 40, 96}, { 92, 68,148},
    {142,112,200}, {182,176,152}, {224,220,200}, {252,252,248},
};

static const uint8_t BAYER[16] = { 0, 8, 2,10, 12, 4,14, 6, 3,11, 1, 9, 15, 7,13, 5 };

static void quantise(uint8_t *fb, int W, int H)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *px_ = fb + 4 * ((size_t)y * W + x);
            /* ordered dither: nudge each pixel before snapping, so the water
             * gradient breaks into a woven texture instead of hard bands */
            float d = ((float)BAYER[(y & 3) * 4 + (x & 3)] / 16.0f - 0.469f) * 26.0f;
            float pr = px_[0] + d, pg = px_[1] + d, pb = px_[2] + d;

            int best = 0;
            float bestd = 1e18f;
            for (int i = 0; i < 32; i++) {
                float dr = pr - PAL[i][0], dg = pg - PAL[i][1], db = pb - PAL[i][2];
                /* luma-weighted, so the match tracks perceived brightness */
                float e = dr * dr * 0.30f + dg * dg * 0.59f + db * db * 0.11f;
                if (e < bestd) { bestd = e; best = i; }
            }
            px_[0] = PAL[best][0];
            px_[1] = PAL[best][1];
            px_[2] = PAL[best][2];
        }
    }
}

/* ---------------- main entry ---------------- */

void cp_render(const CpWorld *w, uint8_t *rgba, int W, int H)
{
    int scale = W / PIX_W;
    if (H / PIX_H < scale) scale = H / PIX_H;
    if (scale < 1) scale = 1;

    uint8_t *low = (uint8_t *)malloc((size_t)PIX_W * PIX_H * 4);
    if (!low) return;
    Canvas cv = { low, PIX_W, PIX_H };

    const float viewW = PIX_W / WSCALE, viewH = PIX_H / WSCALE;
    float camx = clampf(w->player.x - viewW * 0.5f, 0.0f, CP_WORLD_W - viewW);
    float camy = clampf(w->player.y - viewH * 0.5f, 0.0f, CP_WORLD_H - viewH);
    if (CP_WORLD_W < viewW) camx = (CP_WORLD_W - viewW) * 0.5f;
    if (CP_WORLD_H < viewH) camy = (CP_WORLD_H - viewH) * 0.5f;

    #define SXW(wx) (((wx) - camx) * WSCALE)
    #define SYW(wy) (((wy) - camy) * WSCALE)

    /* --- water: a vertical ramp, left to the dither to break up --- */
    for (int y = 0; y < PIX_H; y++) {
        float t = (float)y / (float)PIX_H;
        float r = mixf(0.055f, 0.100f, t);
        float g = mixf(0.150f, 0.235f, t);
        float b = mixf(0.200f, 0.290f, t);
        uint8_t *row = low + 4 * (size_t)y * PIX_W;
        for (int x = 0; x < PIX_W; x++) {
            row[4 * x + 0] = (uint8_t)(r * 255.0f);
            row[4 * x + 1] = (uint8_t)(g * 255.0f);
            row[4 * x + 2] = (uint8_t)(b * 255.0f);
            row[4 * x + 3] = 255;
        }
    }

    /* murk, fixed to the world so it parallaxes with the camera */
    CpRng mr; cp_rng_seed(&mr, w->seed ^ 0xA53Cu);
    for (int i = 0; i < 12; i++) {
        float mx = cp_rng_range(&mr, -200.0f, CP_WORLD_W + 200.0f);
        float my = cp_rng_range(&mr, -200.0f, CP_WORLD_H + 200.0f);
        float mrad = cp_rng_range(&mr, 200.0f, 520.0f) * WSCALE;
        glow(&cv, SXW(mx), SYW(my), mrad, 0.06f, 0.16f, 0.20f, 0.55f);
    }

    /* pool walls */
    {
        int l = (int)SXW(0.0f), r = (int)SXW(CP_WORLD_W);
        int t = (int)SYW(0.0f), b = (int)SYW(CP_WORLD_H);
        for (int d = 0; d < 14; d++) {
            float a = (1.0f - d / 14.0f) * 0.55f;
            if (l + d >= 0 && l + d < PIX_W) rect_fill(&cv, l + d, 0, 1, PIX_H, 0.02f, 0.05f, 0.08f, a);
            if (r - d >= 0 && r - d < PIX_W) rect_fill(&cv, r - d, 0, 1, PIX_H, 0.02f, 0.05f, 0.08f, a);
            if (t + d >= 0 && t + d < PIX_H) rect_fill(&cv, 0, t + d, PIX_W, 1, 0.02f, 0.05f, 0.08f, a);
            if (b - d >= 0 && b - d < PIX_H) rect_fill(&cv, 0, b - d, PIX_W, 1, 0.02f, 0.05f, 0.08f, a);
        }
        if (l >= 0 && l < PIX_W) rect_fill(&cv, l, 0, 1, PIX_H, 0.30f, 0.62f, 0.64f, 0.6f);
        if (r >= 0 && r < PIX_W) rect_fill(&cv, r, 0, 1, PIX_H, 0.30f, 0.62f, 0.64f, 0.6f);
        if (t >= 0 && t < PIX_H) rect_fill(&cv, 0, t, PIX_W, 1, 0.30f, 0.62f, 0.64f, 0.6f);
        if (b >= 0 && b < PIX_H) rect_fill(&cv, 0, b, PIX_W, 1, 0.30f, 0.62f, 0.64f, 0.6f);
    }

    /* suspended detritus, parallaxed for depth - single pixels, no blur */
    CpRng dr; cp_rng_seed(&dr, w->seed ^ 0x1234u);
    for (int i = 0; i < 420; i++) {
        float wx = cp_rng_range(&dr, 0.0f, CP_WORLD_W);
        float wy = cp_rng_range(&dr, 0.0f, CP_WORLD_H);
        float depth = cp_rng_range(&dr, 0.30f, 0.85f);
        int sx = (int)((wx - camx * depth) * WSCALE);
        int sy = (int)((wy - camy * depth) * WSCALE);
        px_blend(&cv, sx, sy, 0.55f, 0.78f, 0.82f, 0.10f + 0.22f * depth);
    }

    /* --- food --- */
    for (int i = 0; i < CP_MAX_FOOD; i++) {
        const CpFood *f = &w->food[i];
        if (f->type == CP_FOOD_NONE) continue;
        float sx = SXW(f->x), sy = SYW(f->y);
        if (sx < -8 || sy < -8 || sx > PIX_W + 8 || sy > PIX_H + 8) continue;
        float rad = f->r * WSCALE + 0.6f;
        if (f->type == CP_FOOD_PLANT) {
            disc(&cv, sx, sy, rad + 1.0f, 0.14f, 0.36f, 0.24f, 0.75f);
            disc(&cv, sx, sy, rad, 0.30f, 0.72f, 0.34f, 1.0f);
            px_blend(&cv, (int)sx, (int)(sy - 1), 0.66f, 0.92f, 0.55f, 1.0f);
        } else {
            disc(&cv, sx, sy, rad + 1.0f, 0.36f, 0.14f, 0.10f, 0.75f);
            disc(&cv, sx, sy, rad, 0.78f, 0.32f, 0.20f, 1.0f);
            px_blend(&cv, (int)sx, (int)(sy - 1), 0.95f, 0.62f, 0.40f, 1.0f);
        }
    }

    /* --- npc cells --- */
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        const CpCell *c = &w->cells[i];
        if (!c->alive) continue;
        float sx = SXW(c->x), sy = SYW(c->y);
        float rad = c->r * WSCALE;
        if (sx < -40 || sy < -40 || sx > PIX_W + 40 || sy > PIX_H + 40) continue;
        draw_npc(&cv, c, sx, sy, rad);

        if (c->hp < c->hp_max * 0.995f && rad >= 4.0f) {
            int bw = (int)(rad * 2.0f);
            if (bw < 7) bw = 7;
            bar(&cv, (int)(sx - bw / 2), (int)(sy - rad - 4.0f), bw, 1,
                c->hp / c->hp_max, 0.86f, 0.30f, 0.26f);
        }
    }

    /* --- player --- */
    draw_player(&cv, w, SXW(w->player.x), SYW(w->player.y), w->player.r * WSCALE);

    draw_hud(&cv, w, camx, camy, viewW, viewH);
    quantise(low, PIX_W, PIX_H);

    /* --- nearest-neighbour blow-up, centred --- */
    int outw = PIX_W * scale, outh = PIX_H * scale;
    int ox = (W - outw) / 2, oy = (H - outh) / 2;
    for (int y = 0; y < H; y++) {
        uint8_t *drow = rgba + 4 * (size_t)y * W;
        int syi = (y - oy) / scale;
        for (int x = 0; x < W; x++) {
            int sxi = (x - ox) / scale;
            const uint8_t *src;
            if (x < ox || y < oy || sxi >= PIX_W || syi >= PIX_H)
                src = PAL[0];
            else
                src = low + 4 * ((size_t)syi * PIX_W + sxi);
            drow[4 * x + 0] = src[0];
            drow[4 * x + 1] = src[1];
            drow[4 * x + 2] = src[2];
            drow[4 * x + 3] = 255;
        }
    }

    #undef SXW
    #undef SYW
    free(low);
}
