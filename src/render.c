#include "cpore/cpore.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* Software rasteriser. Reads world state and writes RGBA8; it never touches
 * the simulation, so training runs can skip this translation unit entirely. */

#define PI 3.14159265358979f

typedef struct { uint8_t *fb; int W, H; } Canvas;

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
            float cov = clampf(rad + 0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) px_blend(c, x, y, r, g, b, a * cov);
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
            float cov = clampf(th * 0.5f + 0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) px_blend(c, x, y, r, g, b, a * cov);
        }
    }
}

static void glow(Canvas *c, float cx, float cy, float rad, float r, float g, float b, float inten)
{
    int x0 = (int)floorf(cx - rad), x1 = (int)ceilf(cx + rad);
    int y0 = (int)floorf(cy - rad), y1 = (int)ceilf(cy + rad);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->W) x1 = c->W - 1;
    if (y1 >= c->H) y1 = c->H - 1;
    float inv = 1.0f / rad;
    for (int y = y0; y <= y1; y++) {
        float dy = ((float)y + 0.5f - cy) * inv;
        for (int x = x0; x <= x1; x++) {
            float dx = ((float)x + 0.5f - cx) * inv;
            float d2 = dx * dx + dy * dy;
            if (d2 >= 1.0f) continue;
            float f = 1.0f - d2;
            px_add(c, x, y, r, g, b, inten * f * f);
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
            float cov = clampf(rr + 0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) px_blend(c, x, y, r, g, b, a * cov);
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
        float ln = rad * 0.42f * (0.75f + 0.35f * wave);
        float bend = wave * 0.45f;
        capsule(c, bx, by, bx + cosf(wa + bend) * ln, by + sinf(wa + bend) * ln,
                rad * 0.055f + 0.5f, 0.35f, acc.r, acc.g, acc.b, 0.78f);
        break;
    }
    case CP_PART_FLAGELLA: {                    /* a long trailing whip */
        float x = bx, y = by, a = wa, seg = rad * 0.50f;
        for (int k = 0; k < 5; k++) {
            a += sinf(phase * 3.0f - k * 0.9f) * 0.42f;
            float nx = x + cosf(a) * seg, ny = y + sinf(a) * seg;
            float wdt = rad * 0.10f * (1.0f - k * 0.16f);
            capsule(c, x, y, nx, ny, wdt, wdt * 0.7f, acc.r, acc.g, acc.b, 0.70f);
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
    disc(c, sx, sy, rad, body.r * 0.55f, body.g * 0.55f, body.b * 0.55f, 0.92f);
    disc(c, sx - rad * 0.12f, sy - rad * 0.14f, rad * 0.88f, body.r, body.g, body.b, 0.80f);
    ring(c, sx, sy, rad - 0.6f, 2.0f, acc.r, acc.g, acc.b, 0.85f);
    ring(c, sx + rad * 0.10f, sy + rad * 0.12f, rad * 0.90f, 1.6f, 1.0f, 1.0f, 1.0f, 0.14f);

    disc(c, sx + rad * 0.18f, sy + rad * 0.10f, rad * 0.34f,
         acc.r * 0.75f, acc.g * 0.75f, acc.b * 0.85f, 0.62f);
    disc(c, sx - rad * 0.26f, sy + rad * 0.22f, rad * 0.15f, 1.0f, 1.0f, 1.0f, 0.22f);
    disc(c, sx - rad * 0.30f, sy - rad * 0.30f, rad * 0.20f, 1.0f, 1.0f, 1.0f, 0.30f);

    if (is_player) {
        /* the agent's cell has to be findable at a glance in a crowded pool */
        ring(c, sx, sy, rad + 7.0f, 1.8f, 0.60f, 1.0f, 0.95f, 0.70f);
        ring(c, sx, sy, rad + 13.0f, 1.0f, 0.60f, 1.0f, 0.95f, 0.28f);
        for (int i = 0; i < 4; i++) {
            float a = PI * 0.25f + i * PI * 0.5f;
            capsule(c, sx + cosf(a) * (rad + 10.0f), sy + sinf(a) * (rad + 10.0f),
                    sx + cosf(a) * (rad + 17.0f), sy + sinf(a) * (rad + 17.0f),
                    1.1f, 1.1f, 0.60f, 1.0f, 0.95f, 0.55f);
        }
        capsule(c, sx + cosf(heading) * (rad + 3.0f), sy + sinf(heading) * (rad + 3.0f),
                sx + cosf(heading) * (rad + 13.0f), sy + sinf(heading) * (rad + 13.0f),
                2.0f, 0.6f, 0.7f, 1.0f, 1.0f, 0.85f);
    }
}

/* the player: parts come straight off the genome, at their real angles */
static void draw_player(Canvas *c, const CpWorld *w, float sx, float sy)
{
    const CpCell *p = &w->player;
    Col body = { 0.16f, 0.72f, 0.88f }, acc = { 0.60f, 1.0f, 0.98f };
    if (!p->alive) { body.r = 0.35f; body.g = 0.35f; body.b = 0.40f; }
    float rad = p->r;
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
        ring(c, sx, sy, w->stats.elec_radius * (1.15f - 0.15f * k), 2.5f,
             0.65f, 0.92f, 1.0f, 0.85f * k);
        glow(c, sx, sy, w->stats.elec_radius * 1.2f, 0.30f, 0.65f, 1.0f, 0.55f * k);
    }
}

/* npc cells get the same part vocabulary, synthesised from their counts */
static void draw_npc(Canvas *c, const CpCell *n, float sx, float sy)
{
    Col body, acc;
    hsv(n->hue, 0.58f, 0.72f, &body.r, &body.g, &body.b);
    hsv(n->hue + 0.06f, 0.42f, 1.0f, &acc.r, &acc.g, &acc.b);

    float head = (n->vx * n->vx + n->vy * n->vy) > 1.0f
               ? atan2f(n->vy, n->vx) : n->wander_a;
    float rad = n->r;

    glow(c, sx, sy, rad * 3.1f, body.r * 0.5f, body.g * 0.5f, body.b * 0.5f, 0.16f);

    int ncil = 8 + n->cilia * 3;
    if (ncil > 26) ncil = 26;
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

static void panel(Canvas *c, int x, int y, int w, int h, float a)
{
    rect_fill(c, x, y, w, h, 0.02f, 0.06f, 0.09f, a);
    rect_fill(c, x, y, w, 1, 0.35f, 0.85f, 0.90f, 0.35f);
    rect_fill(c, x, y + h - 1, w, 1, 0.35f, 0.85f, 0.90f, 0.16f);
}

static void bar(Canvas *c, int x, int y, int w, int h, float frac,
                float r, float g, float b)
{
    frac = clampf(frac, 0.0f, 1.0f);
    rect_fill(c, x - 1, y - 1, w + 2, h + 2, 0.0f, 0.0f, 0.0f, 0.45f);
    rect_fill(c, x, y, w, h, 0.10f, 0.14f, 0.17f, 0.9f);
    int fw = (int)(w * frac);
    for (int i = 0; i < fw; i++) {
        float t = (float)i / (float)(w > 1 ? w - 1 : 1);
        rect_fill(c, x + i, y, 1, h, r * (0.72f + 0.38f * t), g * (0.72f + 0.38f * t),
                  b * (0.72f + 0.38f * t), 0.98f);
    }
    if (fw > 0) rect_fill(c, x, y, fw, 1, 1.0f, 1.0f, 1.0f, 0.22f);
}

static void draw_minimap(Canvas *c, const CpWorld *w, float camx, float camy)
{
    const int mw = 214, mh = 128;
    const int mx = c->W - mw - 18, my = c->H - mh - 18;
    panel(c, mx - 4, my - 16, mw + 8, mh + 24, 0.62f);
    text(c, mx, my - 12, 1, "POOL", 0.55f, 0.88f, 0.92f, 0.85f);

    float sx = (float)mw / CP_WORLD_W, sy = (float)mh / CP_WORLD_H;
    rect_fill(c, mx, my, mw, mh, 0.03f, 0.09f, 0.12f, 0.85f);

    for (int i = 0; i < CP_MAX_FOOD; i += 3) {
        const CpFood *f = &w->food[i];
        if (f->type == CP_FOOD_NONE) continue;
        int px_ = mx + (int)(f->x * sx), py_ = my + (int)(f->y * sy);
        if (f->type == CP_FOOD_PLANT) px_blend(c, px_, py_, 0.35f, 0.85f, 0.40f, 0.65f);
        else                          px_blend(c, px_, py_, 0.95f, 0.45f, 0.30f, 0.75f);
    }
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        const CpCell *cc = &w->cells[i];
        if (!cc->alive) continue;
        float r, g, b; hsv(cc->hue, 0.62f, 0.95f, &r, &g, &b);
        disc(c, mx + cc->x * sx, my + cc->y * sy, cc->r * sx * 1.6f + 0.8f, r, g, b, 0.85f);
    }
    disc(c, mx + w->player.x * sx, my + w->player.y * sy, 3.0f, 0.6f, 1.0f, 0.95f, 1.0f);
    ring(c, mx + w->player.x * sx, my + w->player.y * sy, 5.5f, 1.2f, 0.6f, 1.0f, 0.95f, 0.7f);

    /* current viewport */
    int vx = mx + (int)(camx * sx), vy = my + (int)(camy * sy);
    int vw = (int)(c->W * sx), vh = (int)(c->H * sy);
    rect_fill(c, vx, vy, vw, 1, 0.8f, 1.0f, 1.0f, 0.30f);
    rect_fill(c, vx, vy + vh, vw, 1, 0.8f, 1.0f, 1.0f, 0.30f);
    rect_fill(c, vx, vy, 1, vh, 0.8f, 1.0f, 1.0f, 0.30f);
    rect_fill(c, vx + vw, vy, 1, vh, 0.8f, 1.0f, 1.0f, 0.30f);
}

static void draw_hud(Canvas *c, const CpWorld *w, float camx, float camy)
{
    char buf[128];
    const CpCell *p = &w->player;

    /* --- stage / vitals --- */
    panel(c, 18, 18, 330, 104, 0.66f);
    text(c, 30, 28, 2, "CELL STAGE", 0.62f, 0.96f, 1.0f, 0.95f);
    snprintf(buf, sizeof(buf), "SEED %u   T %d   PARTS %d", w->seed, w->step, w->stats.n_parts);
    text(c, 30, 46, 1, buf, 0.45f, 0.62f, 0.70f, 0.9f);

    text(c, 30, 64, 1, "HEALTH", 0.75f, 0.80f, 0.82f, 0.9f);
    bar(c, 92, 63, 236, 9, p->hp / p->hp_max, 0.95f, 0.32f, 0.34f);

    text(c, 30, 84, 1, "DNA", 0.75f, 0.80f, 0.82f, 0.9f);
    bar(c, 92, 83, 236, 9, w->dna / CP_DNA_GOAL, 0.42f, 0.90f, 0.55f);

    snprintf(buf, sizeof(buf), "%d / %d", (int)w->dna, (int)CP_DNA_GOAL);
    text(c, 92, 100, 1, buf, 0.55f, 0.85f, 0.62f, 0.9f);
    snprintf(buf, sizeof(buf), "SIZE %.1f", (double)p->r);
    text(c, 240, 100, 1, buf, 0.50f, 0.70f, 0.78f, 0.9f);

    /* --- body plan: the editor's output, including where things are --- */
    panel(c, 18, 134, 330, 186, 0.62f);
    snprintf(buf, sizeof(buf), "BODY PLAN   GEN %d/%d   %d/%d DNA",
             w->generation + 1, CP_GENERATIONS,
             (int)w->stats.cost, CP_GEN_BUDGET[w->generation]);
    text(c, 30, 142, 1, buf, 0.62f, 0.96f, 1.0f, 0.9f);

    static const char *pn[CP_PART_COUNT] = { "", "FILTR", "JAW", "PROBO", "CILIA",
                                             "FLGLA", "JET", "SPIKE", "ELECT",
                                             "POISN", "EYE" };
    for (int t = 1; t < CP_PART_COUNT; t++) {
        int i = t - 1;
        int col = i / 5, row = i % 5;
        int x = 30 + col * 116, y = 160 + row * 15;
        int n = w->stats.n[t];
        float lum = n ? 1.0f : 0.34f;
        text(c, x, y, 1, pn[t], 0.70f * lum, 0.80f * lum, 0.84f * lum, 0.95f);
        for (int k = 0; k < 4; k++) {
            int on = k < n;
            rect_fill(c, x + 38 + k * 8, y, 6, 7,
                      on ? 0.40f : 0.14f, on ? 0.92f : 0.18f, on ? 0.86f : 0.22f, 0.95f);
        }
    }

    /* placement dial - where each part actually sits on the membrane.
     * Front of the cell is to the right, matching the facing tick in-world. */
    {
        float dx = 296.0f, dy = 218.0f, dr = 30.0f;
        ring(c, dx, dy, dr, 1.0f, 0.35f, 0.62f, 0.68f, 0.55f);
        capsule(c, dx + dr * 0.55f, dy, dx + dr * 0.95f, dy, 1.0f, 1.0f,
                0.55f, 0.85f, 0.90f, 0.75f);
        text(c, (int)(dx - 12), (int)(dy + dr + 5), 1, "FWD", 0.45f, 0.70f, 0.76f, 0.8f);
        for (int i = 0; i < CP_MAX_PARTS; i++) {
            int t = w->genome.part[i].type;
            if (t == CP_PART_NONE) continue;
            float a = (float)w->genome.part[i].angle * (2.0f * PI / 256.0f);
            float r, g, b;
            hsv(0.08f + 0.085f * (float)t, 0.70f, 1.0f, &r, &g, &b);
            disc(c, dx + cosf(a) * dr, dy + sinf(a) * dr, 3.2f, r, g, b, 0.95f);
        }
    }

    /* --- episode stats --- */
    panel(c, c->W - 232, 18, 214, 106, 0.62f);
    text(c, c->W - 220, 26, 1, "EPISODE", 0.62f, 0.96f, 1.0f, 0.9f);
    snprintf(buf, sizeof(buf), "PLANTS EATEN  %d", w->ate_plant);
    text(c, c->W - 220, 44, 1, buf, 0.72f, 0.80f, 0.82f, 0.9f);
    snprintf(buf, sizeof(buf), "MEAT EATEN    %d", w->ate_meat);
    text(c, c->W - 220, 58, 1, buf, 0.72f, 0.80f, 0.82f, 0.9f);
    snprintf(buf, sizeof(buf), "CELLS KILLED  %d", w->kills);
    text(c, c->W - 220, 72, 1, buf, 0.72f, 0.80f, 0.82f, 0.9f);
    snprintf(buf, sizeof(buf), "HITS TAKEN    %d", w->hits_taken);
    text(c, c->W - 220, 86, 1, buf, 0.72f, 0.80f, 0.82f, 0.9f);
    snprintf(buf, sizeof(buf), "DISCHARGES    %d", w->discharges);
    text(c, c->W - 220, 100, 1, buf, 0.72f, 0.80f, 0.82f, 0.9f);

    draw_minimap(c, w, camx, camy);

    /* --- terminal banner --- */
    if (w->status != CP_RUN) {
        const char *msg = w->status == CP_EVOLVED ? "EVOLVE - CREATURE STAGE"
                        : w->status == CP_DEAD    ? "CONSUMED"
                                                  : "TIME UP";
        int tw = text_w(msg, 3);
        int bx = (c->W - tw) / 2 - 24, by = c->H / 2 - 34;
        panel(c, bx, by, tw + 48, 62, 0.78f);
        text(c, (c->W - tw) / 2, by + 20, 3,
             msg, w->status == CP_EVOLVED ? 0.5f : 1.0f,
             w->status == CP_EVOLVED ? 1.0f : 0.45f, 0.6f, 0.97f);
    }

    text(c, 18, c->H - 22, 1, "CPORE  /  CELL STAGE  /  HEADLESS SIM + SOFTWARE RASTERISER",
         0.34f, 0.48f, 0.54f, 0.85f);
}

/* ---------------- main entry ---------------- */

void cp_render(const CpWorld *w, uint8_t *rgba, int W, int H)
{
    Canvas cv = { rgba, W, H };

    float camx = clampf(w->player.x - W * 0.5f, 0.0f, CP_WORLD_W - (float)W);
    float camy = clampf(w->player.y - H * 0.5f, 0.0f, CP_WORLD_H - (float)H);
    if (CP_WORLD_W < (float)W) camx = (CP_WORLD_W - (float)W) * 0.5f;
    if (CP_WORLD_H < (float)H) camy = (CP_WORLD_H - (float)H) * 0.5f;

    /* --- water --- */
    for (int y = 0; y < H; y++) {
        float t = (float)y / (float)H;
        float r = mixf(0.020f, 0.055f, t);
        float g = mixf(0.090f, 0.150f, t);
        float b = mixf(0.120f, 0.175f, t);
        uint8_t *row = rgba + 4 * (size_t)y * W;
        for (int x = 0; x < W; x++) {
            row[4 * x + 0] = (uint8_t)(r * 255.0f);
            row[4 * x + 1] = (uint8_t)(g * 255.0f);
            row[4 * x + 2] = (uint8_t)(b * 255.0f);
            row[4 * x + 3] = 255;
        }
    }

    /* murk: a few big soft blooms, fixed to the world so they parallax */
    CpRng mr; cp_rng_seed(&mr, w->seed ^ 0xA53Cu);
    for (int i = 0; i < 14; i++) {
        float mx = cp_rng_range(&mr, -200.0f, CP_WORLD_W + 200.0f);
        float my = cp_rng_range(&mr, -200.0f, CP_WORLD_H + 200.0f);
        float mrad = cp_rng_range(&mr, 260.0f, 620.0f);
        glow(&cv, mx - camx, my - camy, mrad,
             cp_rng_range(&mr, 0.02f, 0.07f), cp_rng_range(&mr, 0.10f, 0.20f),
             cp_rng_range(&mr, 0.12f, 0.22f), 0.55f);
    }

    /* pool walls: a soft rim so the bounded world reads as bounded */
    {
        struct { float pos; int vertical; int inward; } edges[4] = {
            { -camx, 1, +1 }, { CP_WORLD_W - camx, 1, -1 },
            { -camy, 0, +1 }, { CP_WORLD_H - camy, 0, -1 }
        };
        for (int e = 0; e < 4; e++) {
            float p0 = edges[e].pos;
            for (int d = 0; d < 70; d++) {
                float a = (1.0f - d / 70.0f);
                a = a * a * 0.5f;
                int q = (int)(p0 + edges[e].inward * d);
                if (edges[e].vertical) {
                    if (q < 0 || q >= W) continue;
                    rect_fill(&cv, q, 0, 1, H, 0.01f, 0.03f, 0.05f, a);
                } else {
                    if (q < 0 || q >= H) continue;
                    rect_fill(&cv, 0, q, W, 1, 0.01f, 0.03f, 0.05f, a);
                }
            }
            int q = (int)p0;
            if (edges[e].vertical) {
                if (q >= 0 && q < W) rect_fill(&cv, q, 0, 1, H, 0.30f, 0.70f, 0.72f, 0.22f);
            } else {
                if (q >= 0 && q < H) rect_fill(&cv, 0, q, W, 1, 0.30f, 0.70f, 0.72f, 0.22f);
            }
        }
    }

    /* suspended detritus, parallaxed for depth */
    CpRng dr; cp_rng_seed(&dr, w->seed ^ 0x1234u);
    for (int i = 0; i < 520; i++) {
        float wx = cp_rng_range(&dr, 0.0f, CP_WORLD_W);
        float wy = cp_rng_range(&dr, 0.0f, CP_WORLD_H);
        float depth = cp_rng_range(&dr, 0.25f, 0.85f);
        float rad = cp_rng_range(&dr, 0.6f, 2.2f) * depth;
        float sx = wx - camx * depth, sy = wy - camy * depth;
        if (sx < -8 || sy < -8 || sx > W + 8 || sy > H + 8) continue;
        disc(&cv, sx, sy, rad, 0.55f, 0.80f, 0.85f, 0.06f + 0.14f * depth);
    }

    /* --- food --- */
    for (int i = 0; i < CP_MAX_FOOD; i++) {
        const CpFood *f = &w->food[i];
        if (f->type == CP_FOOD_NONE) continue;
        float sx = f->x - camx, sy = f->y - camy;
        if (sx < -20 || sy < -20 || sx > W + 20 || sy > H + 20) continue;

        if (f->type == CP_FOOD_PLANT) {
            float pulse = 0.85f + 0.15f * sinf(f->phase + (float)w->step * CP_DT * 1.7f);
            glow(&cv, sx, sy, f->r * 4.2f, 0.10f, 0.55f, 0.22f, 0.30f * pulse);
            disc(&cv, sx, sy, f->r * pulse, 0.30f, 0.86f, 0.42f, 0.92f);
            disc(&cv, sx - f->r * 0.25f, sy - f->r * 0.28f, f->r * 0.42f,
                 0.80f, 1.0f, 0.85f, 0.55f);
        } else {
            float fade = clampf(f->phase / 4.0f, 0.25f, 1.0f);
            glow(&cv, sx, sy, f->r * 4.0f, 0.55f, 0.16f, 0.10f, 0.30f * fade);
            disc(&cv, sx, sy, f->r, 0.92f, 0.36f, 0.26f, 0.92f * fade);
            disc(&cv, sx - f->r * 0.22f, sy - f->r * 0.26f, f->r * 0.40f,
                 1.0f, 0.78f, 0.62f, 0.55f * fade);
        }
    }

    /* --- npc cells --- */
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        const CpCell *c = &w->cells[i];
        if (!c->alive) continue;
        float sx = c->x - camx, sy = c->y - camy;
        if (sx < -120 || sy < -120 || sx > W + 120 || sy > H + 120) continue;

        draw_npc(&cv, c, sx, sy);

        if (c->hp < c->hp_max * 0.995f) {
            int bw = (int)(c->r * 2.0f);
            if (bw < 16) bw = 16;
            bar(&cv, (int)(sx - bw / 2), (int)(sy - c->r - 12), bw, 3,
                c->hp / c->hp_max, 0.95f, 0.35f, 0.30f);
        }
    }

    /* --- player --- */
    draw_player(&cv, w, w->player.x - camx, w->player.y - camy);

    /* --- vignette --- */
    {
        float cx = W * 0.5f, cy = H * 0.5f;
        float inv = 1.0f / sqrtf(cx * cx + cy * cy);
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                float dx = ((float)x - cx), dy = ((float)y - cy);
                float d = sqrtf(dx * dx + dy * dy) * inv;
                float v = clampf((d - 0.55f) / 0.45f, 0.0f, 1.0f);
                if (v > 0.0f) px_blend(&cv, x, y, 0.0f, 0.015f, 0.03f, v * v * 0.72f);
            }
        }
    }

    draw_hud(&cv, w, camx, camy);
}
