#include "cpore/cpore.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "pixfont.h"

/* Software rasteriser. Reads world state and writes RGBA8; it never touches
 * the simulation, so training runs can skip this translation unit entirely. */

/* Pixel-art pipeline. Everything is drawn into a small fixed-size buffer with
 * hard edges, quantised to a 32-colour palette with ordered dithering, then
 * blown up with nearest-neighbour. No analytic antialiasing anywhere: a pixel
 * is either the shape's colour or it is not. */
#define PI 3.14159265358979f

#define PIX_W   320
#define PIX_H   180

/* A visual style is more than a palette swap: internal resolution, camera
 * scale, dither strength, background value structure and whether shapes are
 * keylined dark or rimmed bright all move together. */
typedef struct {
    const char    *name;
    const uint8_t (*pal)[3];
    int            pal_n;
    int            w, h;             /* internal resolution                  */
    float          wscale;           /* pixels per world unit                */
    float          dither;           /* ordered-dither spread                */
    int            rim_bright;       /* 0 = dark keyline, 1 = bright rim     */
    float          ink[3];
    float          sky0[3], sky1[3]; /* background ramp, top to bottom       */
    float          motes;
    float          wall[3];
    float          ui_bg[3], ui_fg[3], ui_dim[3];
    float          ui_deep[3];       /* recessed fills: bar tracks, minimap   */
} Vis;

typedef struct { uint8_t *fb; int W, H; const Vis *v; } Canvas;

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

/* ---------------- 5x7 bitmap font ---------------- */

static int text_w(const char *s, int sc) { return cp_font_width(s, sc); }

static void text(Canvas *c, int px_, int py, int sc, const char *s,
                 float r, float g, float b, float a)
{
    int cx = px_;
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        const uint8_t *gl = cp_font_glyph(ch);
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
    const Vis *v = c->v;
    if (v->rim_bright) {
        /* neon: the outline is the brightest thing on the shape, so the fill
         * can sit almost black against an almost-black background */
        disc(c, sx, sy, rad, acc.r, acc.g, acc.b, 1.0f);
        disc(c, sx, sy, rad - 1.0f, body.r * 0.28f, body.g * 0.28f, body.b * 0.28f, 1.0f);
        disc(c, sx - rad * 0.16f, sy - rad * 0.18f, rad * 0.70f,
             body.r * 0.60f, body.g * 0.60f, body.b * 0.60f, 1.0f);
    } else {
        disc(c, sx, sy, rad, v->ink[0], v->ink[1], v->ink[2], 1.0f);
        disc(c, sx, sy, rad - 1.0f, body.r * 0.62f, body.g * 0.62f, body.b * 0.62f, 1.0f);
        disc(c, sx - rad * 0.16f, sy - rad * 0.18f, rad * 0.76f, body.r, body.g, body.b, 1.0f);
    }

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
        float er = w->stats.elec_radius * c->v->wscale;
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
        const Vis *v = c->v;
        if (v->rim_bright) disc(c, sx, sy, rad, acc.r, acc.g, acc.b, 1.0f);
        else               disc(c, sx, sy, rad, v->ink[0], v->ink[1], v->ink[2], 1.0f);
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
    const Vis *v = c->v;
    rect_fill(c, x, y, w, h, v->ui_bg[0], v->ui_bg[1], v->ui_bg[2], 0.92f);
    rect_fill(c, x, y, w, 1, v->ui_fg[0], v->ui_fg[1], v->ui_fg[2], 0.55f);
    rect_fill(c, x, y + h - 1, w, 1, v->ui_dim[0], v->ui_dim[1], v->ui_dim[2], 0.7f);
}

static void bar(Canvas *c, int x, int y, int w, int h, float frac,
                float r, float g, float b)
{
    frac = clampf(frac, 0.0f, 1.0f);
    const float *dp = c->v->ui_deep;
    rect_fill(c, x, y, w, h, dp[0], dp[1], dp[2], 1.0f);
    int fw = (int)(w * frac + 0.5f);
    if (fw > 0) {
        rect_fill(c, x, y, fw, h, r, g, b, 1.0f);
        rect_fill(c, x, y, fw, 1, r * 1.35f + 0.15f, g * 1.35f + 0.15f, b * 1.35f + 0.15f, 1.0f);
    }
}

static void draw_minimap(Canvas *c, const CpWorld *w, float camx, float camy,
                         float viewW, float viewH)
{
    const int mw = c->W >= 300 ? 62 : 40, mh = c->W >= 300 ? 36 : 23;
    const int mx = c->W - mw - 3, my = 3;
    panel(c, mx - 1, my - 1, mw + 2, mh + 2);

    float sx = (float)mw / CP_WORLD_W, sy = (float)mh / CP_WORLD_H;
    rect_fill(c, mx, my, mw, mh, c->v->ui_deep[0], c->v->ui_deep[1], c->v->ui_deep[2], 1.0f);

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
    const float *uf = c->v->ui_fg;
    rect_fill(c, vx, vy, vw, 1, uf[0], uf[1], uf[2], 0.75f);
    rect_fill(c, vx, vy + vh, vw, 1, uf[0], uf[1], uf[2], 0.75f);
    rect_fill(c, vx, vy, 1, vh, uf[0], uf[1], uf[2], 0.75f);
    rect_fill(c, vx + vw, vy, 1, vh, uf[0], uf[1], uf[2], 0.75f);

    int px_ = mx + (int)(w->player.x * sx), py = my + (int)(w->player.y * sy);
    rect_fill(c, px_ - 1, py, 3, 1, uf[0], uf[1], uf[2], 1.0f);
    rect_fill(c, px_, py - 1, 1, 3, uf[0], uf[1], uf[2], 1.0f);
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
    const Vis *V = c->v;
    /* At 160x90 there is no room for the full readout, so the chunkier styles
     * get vitals and the placement dial only. */
    const int roomy = (c->W >= 300);

    /* --- vitals, top left --- */
    panel(c, 3, 3, roomy ? 104 : 62, 32);
    text(c, 6, 6, 1, roomy ? "CELL STAGE" : "CELL", V->ui_fg[0], V->ui_fg[1], V->ui_fg[2], 1.0f);
    if (roomy) snprintf(buf, sizeof(buf), "G%d/%d T%d", w->generation + 1, CP_GENERATIONS, w->step);
    else       snprintf(buf, sizeof(buf), "G%d T%d", w->generation + 1, w->step);
    text(c, 6, 15, 1, buf, V->ui_dim[0], V->ui_dim[1], V->ui_dim[2], 1.0f);
    int bw2 = roomy ? 46 : 25;
    bar(c, 6, 24, bw2, 3, p->hp / p->hp_max, 0.86f, 0.28f, 0.30f);
    bar(c, 9 + bw2, 24, bw2, 3, w->dna / CP_DNA_GOAL, 0.38f, 0.82f, 0.46f);

    /* --- body plan, bottom left: a swatch and a count per owned part --- */
    if (roomy) {
    panel(c, 3, c->H - 34, 132, 31);
    snprintf(buf, sizeof(buf), "%d DNA  %d PARTS", (int)w->stats.cost, w->stats.n_parts);
    text(c, 6, c->H - 31, 1, buf, V->ui_fg[0], V->ui_fg[1], V->ui_fg[2], 1.0f);

    int col = 0;
    for (int t = 1; t < CP_PART_COUNT; t++) {
        int n = w->stats.n[t];
        if (!n) continue;
        int x = 6 + (col % 5) * 19;
        int y = c->H - 21 + (col / 5) * 9;
        float r, g, b;
        part_colour(t, &r, &g, &b);
        rect_fill(c, x, y, 5, 5, r, g, b, 1.0f);
        rect_fill(c, x, y, 5, 1, r * 1.3f, g * 1.3f, b * 1.3f, 1.0f);
        snprintf(buf, sizeof(buf), "%d", n);
        text(c, x + 7, y - 1, 1, buf, V->ui_fg[0], V->ui_fg[1], V->ui_fg[2], 1.0f);
        col++;
    }

    }

    /* --- placement dial: where the parts actually sit. Front points right,
     *     matching the facing tick on the cell itself. --- */
    {
        float dx = roomy ? 118.0f : 20.0f, dy = (float)c->H - 16.0f;
        float dr = roomy ? 12.0f : 10.0f;
        if (!roomy) panel(c, 3, c->H - 30, 35, 27);
        ring(c, dx, dy, dr, 1.0f, V->ui_dim[0], V->ui_dim[1], V->ui_dim[2], 1.0f);
        rect_fill(c, (int)(dx + dr - 3), (int)dy, 5, 1,
                  V->ui_fg[0], V->ui_fg[1], V->ui_fg[2], 1.0f);
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
    if (roomy) {
        int x = c->W - 65, y = 43;
        panel(c, x - 1, y - 1, 64, 32);
        snprintf(buf, sizeof(buf), "EAT %d", w->ate_plant + w->ate_meat);
        text(c, x + 2, y + 2, 1, buf, V->ui_fg[0], V->ui_fg[1], V->ui_fg[2], 1.0f);
        snprintf(buf, sizeof(buf), "KILL %d", w->kills);
        text(c, x + 2, y + 10, 1, buf, V->ui_fg[0], V->ui_fg[1], V->ui_fg[2], 1.0f);
        snprintf(buf, sizeof(buf), "HIT %d", w->hits_taken);
        text(c, x + 2, y + 18, 1, buf, V->ui_fg[0], V->ui_fg[1], V->ui_fg[2], 1.0f);
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

/* ---------------- palettes ---------------- */

/* deep water, grouped as per-hue ramps so shading stays inside a hue */
static const uint8_t PAL_ABYSS[32][3] = {
    /* water ramp: seven steps from seabed black to lit shallows. The two
     * darkest are reserved for silhouette and outline, not for scenery. */
    {  4,  8, 14}, {  9, 19, 30}, { 14, 32, 46}, { 21, 48, 64},
    { 30, 66, 84}, { 42, 88,106}, { 60,116,132},
    /* greens */
    { 22, 54, 32}, { 40, 98, 52}, { 74,154, 72}, {126,202,102}, {182,232,140},
    /* warm: carrion, sand, and the orange half of the creature genome */
    { 74, 30, 22}, {126, 52, 34}, {180, 84, 46}, {224,134, 72}, {246,196,132},
    /* cyans: the player, photophores, highlights */
    { 24, 78,102}, { 40,124,158}, { 74,182,208}, {150,226,240},
    /* magenta / rose */
    { 90, 26, 58}, {148, 44, 92}, {204, 86,136}, {238,152,186},
    /* violet */
    { 54, 40,100}, { 96, 72,158}, {148,116,206},
    /* neutrals and the two brightest, kept for specular and bone */
    { 74, 80, 78}, {150,158,152}, {214,220,214}, {252,252,248},
};

/* original Game Boy DMG: four greens, and nothing else. Everything has to be
 * carried by silhouette and dither, which is the point of trying it. */
static const uint8_t PAL_DMG[4][3] = {
    { 15, 56, 15}, { 48, 98, 48}, {139,172, 15}, {155,188, 15},
};

/* near-black void with saturated arcade colour, outlines brighter than fills */
static const uint8_t PAL_NEON[16][3] = {
    {  4,  4, 10}, { 14, 10, 26}, { 26, 18, 44}, { 44, 30, 66},
    {255, 42,109}, {150, 20, 66}, {  5,255,161}, {  0,130, 86},
    { 41,173,255}, { 16, 84,140}, {255,236, 39}, {255,119,168},
    {170, 90,255}, {126, 37, 83}, {200,206,214}, {255,255,255},
};

/* lab illustration: cream paper, ink outlines, muted pigment. The only style
 * with an inverted value structure - light ground, dark subjects. */
static const uint8_t PAL_PETRI[16][3] = {
    {241,234,216}, {226,215,190}, {206,192,162}, {182,166,136},
    { 38, 32, 28}, { 78, 70, 60}, {120,110, 96}, {162,150,132},
    {126,148,110}, { 88,112, 74}, {170, 96, 66}, {206,140,100},
    { 92,116,146}, {146,172,196}, {138, 92,120}, {196,150,172},
};

/* the actual Commodore 64 hardware palette, at 160x90 for real chunk */
static const uint8_t PAL_C64[16][3] = {
    {  0,  0,  0}, {255,255,255}, {136, 57, 50}, {103,182,189},
    {139, 63,150}, { 85,160, 73}, { 64, 49,141}, {191,206,114},
    {139, 84, 41}, { 87, 66,  0}, {184,105, 98}, { 80, 80, 80},
    {120,120,120}, {148,224,137}, {120,105,196}, {159,159,159},
};

/* Daylight land. The abyss palette was mixed for a lit water column: it has
 * seven blues, five greens and no sky at all, so every land frame drawn with
 * it spent its sky on the cyan ramp and its distance on the two neutrals -
 * which is exactly why the first landscapes ended in a grey wall.
 *
 * This one is mixed the other way round. Sky gets six steps because the sky
 * is half the frame; foliage gets nine across two ramps because a forest that
 * is one green is a green wall; rock and earth get warm neutrals so slopes
 * read as ground rather than as shadow. The creature ramps at the end are the
 * abyss entries kept deliberately close, so a genome's colours survive the
 * move between stages and an animal does not change species when it walks
 * out of the sea. */
static const uint8_t PAL_TERRA[48][3] = {
    /* sky: zenith to horizon, the six steps aerial perspective walks along */
    { 38, 62,120}, { 54, 94,160}, { 82,134,196}, {124,176,224},
    {172,208,238}, {214,232,246},
    /* cloud and specular */
    {244,248,252}, {255,255,255},
    /* night: the sky at the other end of the day, plus a value below it for
     * silhouette and outline */
    {  8, 10, 24}, { 18, 24, 50}, { 34, 46, 84},
    /* water, from a shaded deep to a lit shallow */
    { 12, 38, 58}, { 20, 62, 92}, { 34, 98,134}, { 60,142,174}, {112,192,212},
    /* foliage: the shadowed half of every canopy */
    { 14, 34, 24}, { 24, 58, 36}, { 36, 84, 46}, { 54,116, 58}, { 82,152, 74},
    /* grass and lit leaves: the other half */
    {106,172, 78}, {140,198, 96}, {180,220,120}, {214,238,158},
    /* rock and earth, warm rather than neutral so a cliff is not a shadow */
    { 30, 26, 24}, { 54, 46, 40}, { 84, 72, 62}, {118,104, 90},
    {154,140,122}, {192,182,166},
    /* sand and dry ground */
    { 96, 62, 36}, {150,104, 60}, {202,160,104}, {238,212,158},
    /* creature warm */
    {132, 40, 28}, {198, 76, 40}, {240,140, 72},
    /* creature rose */
    {120, 34, 70}, {188, 62,112}, {234,132,170},
    /* creature cyan: the player reads as this against every biome */
    { 26, 90,100}, { 52,152,164}, {122,214,220},
    /* Creature violet. Kept deliberately low in green: written as a softer
     * blue-violet these two sat right on top of the sky ramp in luma, won a
     * band of horizon pixels off it, and drew a mauve seam along the join
     * between sea and sky. */
    { 66, 44,124}, {114, 76,186},
    /* snow and ice */
    {206,222,236}, {238,246,252},
};

static const Vis VIS[CP_VIS_COUNT] = {
    { "abyss", PAL_ABYSS, 32, 320, 180, 0.30f, 26.0f, 0,
      {0.02f,0.05f,0.08f},
      {0.055f,0.150f,0.200f}, {0.100f,0.235f,0.290f}, 0.28f, {0.30f,0.62f,0.64f},
      {0.04f,0.09f,0.13f}, {0.62f,0.92f,0.96f}, {0.42f,0.58f,0.64f},
      {0.012f,0.031f,0.043f} },

    /* Four tones and no hue at all, so the ground has to be perfectly flat -
     * dithering a ramp here just produces a checkerboard that swamps every
     * subject on screen. Silhouette carries the whole image. */
    { "dmg", PAL_DMG, 4, 160, 90, 0.16f, 10.0f, 0,
      {0.059f,0.220f,0.059f},
      {0.188f,0.384f,0.188f}, {0.188f,0.384f,0.188f}, 0.30f, {0.545f,0.675f,0.059f},
      {0.059f,0.220f,0.059f}, {0.545f,0.675f,0.059f}, {0.608f,0.737f,0.059f},
      {0.059f,0.220f,0.059f} },

    { "neon", PAL_NEON, 16, 320, 180, 0.30f, 8.0f, 1,
      {0.85f,0.95f,1.00f},
      {0.015f,0.015f,0.040f}, {0.055f,0.040f,0.105f}, 0.34f, {1.00f,0.16f,0.43f},
      {0.05f,0.04f,0.10f}, {0.02f,1.00f,0.63f}, {0.16f,0.68f,1.00f},
      {0.016f,0.016f,0.039f} },

    { "petri", PAL_PETRI, 16, 320, 180, 0.30f, 30.0f, 0,
      {0.15f,0.13f,0.11f},
      {0.945f,0.918f,0.847f}, {0.760f,0.706f,0.600f}, 0.30f, {0.31f,0.27f,0.24f},
      {0.886f,0.843f,0.745f}, {0.15f,0.13f,0.11f}, {0.40f,0.36f,0.31f},
      {0.714f,0.651f,0.533f} },

    { "c64", PAL_C64, 16, 160, 90, 0.16f, 18.0f, 0,
      {0.0f,0.0f,0.0f},
      {0.251f,0.192f,0.553f}, {0.251f,0.192f,0.553f}, 0.42f, {0.40f,0.71f,0.74f},
      {0.0f,0.0f,0.0f}, {0.58f,0.88f,0.54f}, {0.47f,0.47f,0.47f},
      {0.0f,0.0f,0.0f} },

    /* Twice the linear resolution of the others, which is a deliberate change
     * of subject rather than a change of mind about pixel art: the aquatic
     * stage is one animal against open water and reads fine at 320x180, but a
     * landscape is ridgelines and treelines and those are the first thing to
     * dissolve. Dither is lower to match - with 48 colours a ramp no longer
     * needs to be woven so hard to stay smooth. */
    { "terra", PAL_TERRA, 48, 640, 360, 0.30f, 16.0f, 0,
      {0.05f,0.06f,0.09f},
      {0.149f,0.243f,0.471f}, {0.675f,0.816f,0.933f}, 0.10f, {0.46f,0.41f,0.35f},
      {0.05f,0.06f,0.05f}, {0.84f,0.93f,0.62f}, {0.55f,0.62f,0.50f},
      {0.02f,0.03f,0.02f} },

    /* "drop" owns none of these fields. It is here so that the style enum
     * stays dense and every table lookup keyed on a style index - names,
     * dimensions, the fallback any other stage takes when handed a style it
     * cannot render - has something valid to find. The one number that is
     * real is the resolution pair, which is what a caller asking cp_vis_dims
     * gets told to allocate; the palette is the abyss ramp purely so that a
     * stage sharing the pixel pipeline degrades to a sensible look instead of
     * indexing off the end of an array. */
    { "drop", PAL_ABYSS, 32, 1280, 720, 1.42f, 0.0f, 0,
      {0.02f,0.05f,0.08f},
      {0.004f,0.012f,0.017f}, {0.010f,0.030f,0.038f}, 0.28f, {0.34f,0.86f,0.98f},
      {0.01f,0.03f,0.04f}, {0.34f,0.86f,0.92f}, {0.16f,0.40f,0.46f},
      {0.006f,0.016f,0.020f} },
};

/* Which styles go down the palette path and which do not. Kept as a function
 * rather than a flag on Vis because it is a property of the pipeline a style
 * selects, not of the style's own parameters. */
int cp_vis_continuous(int style) { return style == CP_VIS_DROP; }

const char *cp_vis_name(int style)
{
    return (style >= 0 && style < CP_VIS_COUNT) ? VIS[style].name : "?";
}

/* ---------------- dithering + quantisation ---------------- */

static const uint8_t BAYER[16] = { 0, 8, 2,10, 12, 4,14, 6, 3,11, 1, 9, 15, 7,13, 5 };

static void quantise(uint8_t *fb, int W, int H, const Vis *v)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t *px_ = fb + 4 * ((size_t)y * W + x);
            /* ordered dither: nudge each pixel before snapping, so a ramp
             * breaks into a woven texture instead of hard bands. The fewer
             * colours a style has, the harder it has to lean on this. */
            float d = ((float)BAYER[(y & 3) * 4 + (x & 3)] / 16.0f - 0.469f) * v->dither;
            float pr = px_[0] + d, pg = px_[1] + d, pb = px_[2] + d;

            /* Chroma of the source pixel, so a dim red can be told from a
             * mid grey of the same brightness. Without this term the neutral
             * entries win every dimly-lit saturated pixel and the whole
             * population renders grey however colourful its genome is. */
            float pmax = pr > pg ? (pr > pb ? pr : pb) : (pg > pb ? pg : pb);
            float pmin = pr < pg ? (pr < pb ? pr : pb) : (pg < pb ? pg : pb);
            float pc = pmax - pmin;

            int best = 0;
            float bestd = 1e18f;
            for (int i = 0; i < v->pal_n; i++) {
                float er = pr - v->pal[i][0], eg = pg - v->pal[i][1], eb = pb - v->pal[i][2];
                /* luma-weighted, so the match tracks perceived brightness */
                float e = er * er * 0.30f + eg * eg * 0.59f + eb * eb * 0.11f;
                int qmax = v->pal[i][0] > v->pal[i][1]
                         ? (v->pal[i][0] > v->pal[i][2] ? v->pal[i][0] : v->pal[i][2])
                         : (v->pal[i][1] > v->pal[i][2] ? v->pal[i][1] : v->pal[i][2]);
                int qmin = v->pal[i][0] < v->pal[i][1]
                         ? (v->pal[i][0] < v->pal[i][2] ? v->pal[i][0] : v->pal[i][2])
                         : (v->pal[i][1] < v->pal[i][2] ? v->pal[i][1] : v->pal[i][2]);
                float dc = pc - (float)(qmax - qmin);
                e += dc * dc * 0.30f;
                if (e < bestd) { bestd = e; best = i; }
            }
            px_[0] = v->pal[best][0];
            px_[1] = v->pal[best][1];
            px_[2] = v->pal[best][2];
        }
    }
}

/* ---------------- main entry ---------------- */

void cp_render_styled(const CpWorld *w, uint8_t *rgba, int W, int H, int style)
{
    if (style < 0 || style >= CP_VIS_COUNT) style = CP_VIS_DROP;
    /* The continuous-tone path shares nothing below this line - not the
     * canvas, not the primitives, not the HUD - so it forks here rather than
     * threading a second set of branches through a rasteriser built around a
     * palette. */
    if (cp_vis_continuous(style)) { cp_render_drop(w, rgba, W, H); return; }
    const Vis *v = &VIS[style];

    int scale = W / v->w;
    if (H / v->h < scale) scale = H / v->h;
    if (scale < 1) scale = 1;

    uint8_t *low = (uint8_t *)malloc((size_t)v->w * v->h * 4);
    if (!low) return;
    Canvas cv = { low, v->w, v->h, v };

    const float viewW = v->w / v->wscale, viewH = v->h / v->wscale;
    float camx = clampf(w->player.x - viewW * 0.5f, 0.0f, CP_WORLD_W - viewW);
    float camy = clampf(w->player.y - viewH * 0.5f, 0.0f, CP_WORLD_H - viewH);
    if (CP_WORLD_W < viewW) camx = (CP_WORLD_W - viewW) * 0.5f;
    if (CP_WORLD_H < viewH) camy = (CP_WORLD_H - viewH) * 0.5f;

    #define SXW(wx) (((wx) - camx) * v->wscale)
    #define SYW(wy) (((wy) - camy) * v->wscale)

    /* --- water: a vertical ramp, left to the dither to break up --- */
    for (int y = 0; y < v->h; y++) {
        float t = (float)y / (float)v->h;
        float r = mixf(v->sky0[0], v->sky1[0], t);
        float g = mixf(v->sky0[1], v->sky1[1], t);
        float b = mixf(v->sky0[2], v->sky1[2], t);
        uint8_t *row = low + 4 * (size_t)y * v->w;
        for (int x = 0; x < v->w; x++) {
            row[4 * x + 0] = (uint8_t)(clampf(r, 0, 1) * 255.0f);
            row[4 * x + 1] = (uint8_t)(clampf(g, 0, 1) * 255.0f);
            row[4 * x + 2] = (uint8_t)(clampf(b, 0, 1) * 255.0f);
            row[4 * x + 3] = 255;
        }
    }

    /* murk, fixed to the world so it parallaxes with the camera. On a light
     * ground it has to darken rather than add, or it does nothing at all. */
    CpRng mr; cp_rng_seed(&mr, w->seed ^ 0xA53Cu);
    int light_ground = (v->sky0[0] + v->sky0[1] + v->sky0[2]) > 1.5f;
    for (int i = 0; i < 12; i++) {
        float mx = cp_rng_range(&mr, -200.0f, CP_WORLD_W + 200.0f);
        float my = cp_rng_range(&mr, -200.0f, CP_WORLD_H + 200.0f);
        float mrad = cp_rng_range(&mr, 200.0f, 520.0f) * v->wscale;
        if (light_ground) {
            float sx = SXW(mx), sy = SYW(my);
            for (int k = 0; k < 3; k++)
                disc(&cv, sx, sy, mrad * (1.0f - 0.28f * k),
                     v->sky1[0], v->sky1[1], v->sky1[2], 0.16f);
        } else {
            glow(&cv, SXW(mx), SYW(my), mrad, 0.06f, 0.16f, 0.20f, 0.55f);
        }
    }

    /* pool walls */
    {
        int l = (int)SXW(0.0f), r = (int)SXW(CP_WORLD_W);
        int t = (int)SYW(0.0f), b = (int)SYW(CP_WORLD_H);
        int band = v->w >= 300 ? 14 : 7;
        for (int d = 0; d < band; d++) {
            float a = (1.0f - (float)d / band) * 0.55f;
            const float *ik = v->ink;
            if (l + d >= 0 && l + d < v->w) rect_fill(&cv, l + d, 0, 1, v->h, ik[0], ik[1], ik[2], a);
            if (r - d >= 0 && r - d < v->w) rect_fill(&cv, r - d, 0, 1, v->h, ik[0], ik[1], ik[2], a);
            if (t + d >= 0 && t + d < v->h) rect_fill(&cv, 0, t + d, v->w, 1, ik[0], ik[1], ik[2], a);
            if (b - d >= 0 && b - d < v->h) rect_fill(&cv, 0, b - d, v->w, 1, ik[0], ik[1], ik[2], a);
        }
        const float *wl = v->wall;
        if (l >= 0 && l < v->w) rect_fill(&cv, l, 0, 1, v->h, wl[0], wl[1], wl[2], 0.7f);
        if (r >= 0 && r < v->w) rect_fill(&cv, r, 0, 1, v->h, wl[0], wl[1], wl[2], 0.7f);
        if (t >= 0 && t < v->h) rect_fill(&cv, 0, t, v->w, 1, wl[0], wl[1], wl[2], 0.7f);
        if (b >= 0 && b < v->h) rect_fill(&cv, 0, b, v->w, 1, wl[0], wl[1], wl[2], 0.7f);
    }

    /* suspended detritus, parallaxed for depth - single pixels, no blur */
    CpRng dr; cp_rng_seed(&dr, w->seed ^ 0x1234u);
    for (int i = 0; i < 420; i++) {
        float wx = cp_rng_range(&dr, 0.0f, CP_WORLD_W);
        float wy = cp_rng_range(&dr, 0.0f, CP_WORLD_H);
        float depth = cp_rng_range(&dr, 0.30f, 0.85f);
        int sx = (int)((wx - camx * depth) * v->wscale);
        int sy = (int)((wy - camy * depth) * v->wscale);
        if (light_ground) px_blend(&cv, sx, sy, v->ink[0], v->ink[1], v->ink[2], v->motes * depth);
        else              px_blend(&cv, sx, sy, 0.55f, 0.78f, 0.82f, v->motes * depth);
    }

    /* --- food --- */
    for (int i = 0; i < CP_MAX_FOOD; i++) {
        const CpFood *f = &w->food[i];
        if (f->type == CP_FOOD_NONE) continue;
        float sx = SXW(f->x), sy = SYW(f->y);
        if (sx < -8 || sy < -8 || sx > v->w + 8 || sy > v->h + 8) continue;
        float rad = f->r * v->wscale + 0.6f;
        if (rad < 1.2f) rad = 1.2f;
        if (f->type == CP_FOOD_PLANT) {
            disc(&cv, sx, sy, rad + 1.0f, v->ink[0], v->ink[1], v->ink[2], 0.85f);
            disc(&cv, sx, sy, rad, 0.30f, 0.72f, 0.34f, 1.0f);
            px_blend(&cv, (int)sx, (int)(sy - 1), 0.66f, 0.92f, 0.55f, 1.0f);
        } else {
            disc(&cv, sx, sy, rad + 1.0f, v->ink[0], v->ink[1], v->ink[2], 0.85f);
            disc(&cv, sx, sy, rad, 0.78f, 0.32f, 0.20f, 1.0f);
            px_blend(&cv, (int)sx, (int)(sy - 1), 0.95f, 0.62f, 0.40f, 1.0f);
        }
    }

    /* --- npc cells --- */
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        const CpCell *c = &w->cells[i];
        if (!c->alive) continue;
        float sx = SXW(c->x), sy = SYW(c->y);
        float rad = c->r * v->wscale;
        if (rad < 1.5f) rad = 1.5f;
        if (sx < -40 || sy < -40 || sx > v->w + 40 || sy > v->h + 40) continue;
        draw_npc(&cv, c, sx, sy, rad);

        if (c->hp < c->hp_max * 0.995f && rad >= 4.0f) {
            int bw = (int)(rad * 2.0f);
            if (bw < 7) bw = 7;
            bar(&cv, (int)(sx - bw / 2), (int)(sy - rad - 4.0f), bw, 1,
                c->hp / c->hp_max, 0.86f, 0.30f, 0.26f);
        }
    }

    /* --- player --- */
    {
        float rad = w->player.r * v->wscale;
        if (rad < 3.0f) rad = 3.0f;
        draw_player(&cv, w, SXW(w->player.x), SYW(w->player.y), rad);
    }

    draw_hud(&cv, w, camx, camy, viewW, viewH);
    quantise(low, v->w, v->h, v);

    /* --- nearest-neighbour blow-up, centred --- */
    int outw = v->w * scale, outh = v->h * scale;
    int ox = (W - outw) / 2, oy = (H - outh) / 2;
    for (int y = 0; y < H; y++) {
        uint8_t *drow = rgba + 4 * (size_t)y * W;
        int syi = (y - oy) / scale;
        for (int x = 0; x < W; x++) {
            int sxi = (x - ox) / scale;
            const uint8_t *src;
            if (x < ox || y < oy || sxi >= v->w || syi >= v->h)
                src = v->pal[0];
            else
                src = low + 4 * ((size_t)syi * v->w + sxi);
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

/* Default style. One line to change, and every caller follows. */
void cp_render(const CpWorld *w, uint8_t *rgba, int W, int H)
{
    cp_render_styled(w, rgba, W, H, CP_VIS_DROP);
}


/* ---------------- shared pipeline exports ----------------
 * Stage 2 has its own rasteriser (spheres into a z-buffer) but must land in
 * the same palette, so the quantise/blit/text primitives are exported rather
 * than duplicated. */

void cp_vis_dims(int style, int32_t *w, int32_t *h)
{
    if (style < 0 || style >= CP_VIS_COUNT) style = CP_VIS_ABYSS;
    if (w) *w = VIS[style].w;
    if (h) *h = VIS[style].h;
}

void cp_vis_quantise(uint8_t *fb, int32_t w, int32_t h, int32_t style)
{
    if (style < 0 || style >= CP_VIS_COUNT) style = CP_VIS_ABYSS;
    quantise(fb, w, h, &VIS[style]);
}

void cp_vis_blit(const uint8_t *low, int32_t lw, int32_t lh,
                 uint8_t *out, int32_t W, int32_t H, int32_t style)
{
    if (style < 0 || style >= CP_VIS_COUNT) style = CP_VIS_ABYSS;
    const Vis *v = &VIS[style];
    int scale = W / lw;
    if (H / lh < scale) scale = H / lh;
    if (scale < 1) scale = 1;
    int ox = (W - lw * scale) / 2, oy = (H - lh * scale) / 2;
    for (int y = 0; y < H; y++) {
        uint8_t *drow = out + 4 * (size_t)y * W;
        int syi = (y - oy) / scale;
        for (int x = 0; x < W; x++) {
            int sxi = (x - ox) / scale;
            const uint8_t *src = (x < ox || y < oy || sxi >= lw || syi >= lh)
                               ? v->pal[0] : low + 4 * ((size_t)syi * lw + sxi);
            drow[4 * x + 0] = src[0];
            drow[4 * x + 1] = src[1];
            drow[4 * x + 2] = src[2];
            drow[4 * x + 3] = 255;
        }
    }
}

void cp_px_rect(uint8_t *fb, int32_t W, int32_t H, int32_t x, int32_t y,
                int32_t w, int32_t h, float r, float g, float b, float a)
{
    Canvas c = { fb, W, H, &VIS[CP_VIS_ABYSS] };
    rect_fill(&c, x, y, w, h, r, g, b, a);
}

void cp_px_text(uint8_t *fb, int32_t W, int32_t H, int32_t x, int32_t y,
                int32_t sc, const char *s, float r, float g, float b, float a)
{
    Canvas c = { fb, W, H, &VIS[CP_VIS_ABYSS] };
    text(&c, x, y, sc, s, r, g, b, a);
}

int32_t cp_px_text_w(const char *s, int32_t sc) { return text_w(s, sc); }
