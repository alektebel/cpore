/* `pond` - the cell stage with the lamp behind the water.
 *
 * This is the second continuous-tone renderer stage 1 has, and it is the
 * opposite of the first in the one way that decides everything else. `drop` is
 * a darkfield plate: the field is near black, nothing is lit directly, and
 * every organism is visible only by the light it scatters at its own edge.
 * That is a real microscopy technique and it makes a beautiful, cold, clinical
 * picture in which a cell is a glowing outline.
 *
 * `pond` is brightfield. The lamp is behind the water, the water is full of
 * light, and an organism is an opaque painted object that blocks some of that
 * light and is lit from above by the rest. Everything follows from that
 * inversion:
 *
 *  - Bodies composite over the water instead of adding to it, which is why
 *    hdrcanvas grew d_ball and d_fil_over. You cannot make a matte orange
 *    body over pale blue water by adding light to it.
 *
 *  - A body is shaded by a hemisphere term from a fixed key, and its dark side
 *    is tinted toward the water rather than toward black, because in a lit
 *    medium the shadow side of anything is lit by everything around it.
 *
 *  - Depth reads as *loss of contrast* rather than loss of brightness. A far
 *    layer is washed toward the water colour and blurred; a near one is sharp
 *    and saturated. In darkfield the same cue runs the other way.
 *
 *  - The bubbles are the giveaway of the whole look. In darkfield a bubble is
 *    a ring of scattered light; here it is a lens - it takes the water behind
 *    it, brightens the rim where the wall is edge-on, and puts one small hard
 *    specular where the key hits it.
 *
 * The three water palettes are tied to the growth tier, so the pond visibly
 * changes as you outgrow it. That is not decoration: the tier is already the
 * mechanic that decides what can eat you, and giving it a colour means the
 * player can see which pond they are in without reading the meter.
 */

#include "cpore/cpore.h"
#include "hdrcanvas.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ *
 * the plate
 * ------------------------------------------------------------------ */

typedef struct {
    Hdr      h;
    float    camx, camy;      /* world coords at the top-left pixel */
    float    scale;           /* pixels per world unit              */
    float    time;
    uint32_t seed;
    int      tier;
    C3       wtop, wbot;      /* water, near the surface and below  */
    C3       silt;            /* the substrate this pond sits on    */
    C3       haze;            /* what distance washes things toward */
} Pond;

static inline float psx(const Pond *p, float wx) { return (wx - p->camx) * p->scale; }
static inline float psy(const Pond *p, float wy) { return (wy - p->camy) * p->scale; }

/* The key. Up and slightly left, fixed for the whole stage, because a moving
 * key in a top-down scene reads as the world tilting rather than as time
 * passing. */
#define KEY_X (-0.44f)
#define KEY_Y (-0.66f)

/* Pixels per world unit at 1280 wide, before the growth pullback. */
#define POND_SCALE_REF 1.95f
#define POND_EXPOSURE  1.28f

/* Three ponds, one per size tier.
 *
 * Warm and shallow to start - a puddle with a silt floor close enough to see -
 * then open blue-green water, then something colder and deeper. Each is a
 * complete palette rather than a tint of the last, because a tinted copy reads
 * as the same place under a filter and the point is that you have moved. */
static void pond_palette(Pond *p, int tier)
{
    switch (tier) {
    case 0:
        p->wtop = c3(0.55f, 0.72f, 0.72f);
        p->wbot = c3(0.30f, 0.45f, 0.44f);
        p->silt = c3(0.62f, 0.52f, 0.33f);
        p->haze = c3(0.50f, 0.66f, 0.66f);
        break;
    case 1:
        p->wtop = c3(0.32f, 0.60f, 0.66f);
        p->wbot = c3(0.13f, 0.34f, 0.44f);
        p->silt = c3(0.20f, 0.40f, 0.42f);
        p->haze = c3(0.30f, 0.55f, 0.62f);
        break;
    default:
        p->wtop = c3(0.26f, 0.34f, 0.60f);
        p->wbot = c3(0.09f, 0.13f, 0.31f);
        p->silt = c3(0.14f, 0.18f, 0.34f);
        p->haze = c3(0.24f, 0.31f, 0.55f);
        break;
    }
}

/* Place a drifting decoration so that it is actually on screen.
 *
 * The first version scattered bubbles uniformly over the world, which is the
 * obvious thing and the wrong one: the pond is 2400 units across and the view
 * at this zoom is under 700, so thirty-nine of every forty bubbles were
 * somewhere else. Instrumenting it was the only way to see that - the frame
 * just looked like water.
 *
 * Instead each one has a home position in a tile the size of the view, and is
 * drawn at whichever copy of that tile is nearest the camera. The field is
 * effectively infinite, the density on screen is constant at every zoom, and
 * nothing has to be spawned or freed. The tile is larger than the view so that
 * the moment a decoration wraps to the far side happens off screen.
 */
static float tile_near(float home, float anchor, float period)
{
    return home + period * floorf((anchor - home) / period + 0.5f);
}

/* How much a thing at world depth `k` (0 near, 1 far) is washed out. Contrast
 * is the depth cue here, so this is applied to colour rather than to alpha. */
static C3 fade(const Pond *p, C3 col, float k)
{
    return clerp(col, p->haze, sat(k) * 0.82f);
}

/* ------------------------------------------------------------------ *
 * the water
 * ------------------------------------------------------------------ */

/* The water's cloudiness, on a coarse grid.
 *
 * It is two octaves of noise at a world scale of 0.0026, which is to say a
 * feature every four hundred world units - a signal with nothing in it above a
 * few cycles per screen. Evaluating that per pixel is half a million noise
 * lookups to reconstruct something a 16-pixel grid captures exactly, which is
 * the same mistake the contact shadow made in the studio and the terrain
 * marcher made before that. Sampled coarse and bilinear-filtered it is
 * indistinguishable and costs a four-hundredth as much. */
#define WGRID 16

static void draw_water(Pond *p)
{
    const int W = p->h.W, H = p->h.H;
    const int gw = W / WGRID + 2, gh = H / WGRID + 2;
    float *ng = (float *)malloc(sizeof(float) * (size_t)gw * gh);
    if (!ng) return;
    for (int j = 0; j < gh; j++)
        for (int i = 0; i < gw; i++) {
            float wx = (p->camx + (float)(i * WGRID) / p->scale) * 0.0026f;
            float wy = (p->camy + (float)(j * WGRID) / p->scale) * 0.0026f;
            ng[j * gw + i] = dfbm(p->seed ^ 0x51EDu, wx, wy);
        }
    /* World-space y of the substrate, so the floor stays put when the camera
     * moves rather than sliding with it. */
    float floor_y = psy(p, CP_WORLD_H + 40.0f);

    for (int y = 0; y < H; y++) {
        float v = (float)y / (float)(H - 1);
        C3 base = clerp(p->wtop, p->wbot, v * v * 0.85f + v * 0.15f);
        for (int x = 0; x < W; x++) {
            int gi = x / WGRID, gj = y / WGRID;
            float fx = (float)(x - gi * WGRID) / WGRID;
            float fy = (float)(y - gj * WGRID) / WGRID;
            float n00 = ng[gj * gw + gi],       n10 = ng[gj * gw + gi + 1];
            float n01 = ng[(gj + 1) * gw + gi], n11 = ng[(gj + 1) * gw + gi + 1];
            float m = dmixf(dmixf(n00, n10, fx), dmixf(n01, n11, fx), fy);
            C3 c = cscl(base, 0.88f + 0.30f * m);

            /* The substrate, when it is in frame. Horizontal striations
             * because silt settles in layers, and they are the one thing that
             * gives the floor a sense of being a surface rather than a colour. */
            if ((float)y > floor_y - 90.0f) {
                float t = sat(((float)y - (floor_y - 90.0f)) / 110.0f);
                float wxx = p->camx + (float)x / p->scale;
                float wyy = p->camy + (float)y / p->scale;
                float band = dnoise(p->seed ^ 0x2A11u, wxx * 0.010f, wyy * 0.075f);
                C3 s = cscl(p->silt, 0.82f + 0.36f * band);
                c = clerp(c, s, t * 0.92f);
            }
            hdr_set(&p->h, x, y, c);
        }
    }
    free(ng);
}

/* Out-of-focus bodies drifting behind everything.
 *
 * Pure parallax decoration, and the cheapest depth cue in the file: a frame
 * with nothing behind the action reads as a diagram, and one with a couple of
 * blurred shapes a long way back reads as a volume. They move at a fraction of
 * the camera's rate, which is what sells the distance. */
static void draw_far_layer(Pond *p)
{
    for (int i = 0; i < 22; i++) {
        float par = 0.34f + 0.22f * dhash2(p->seed ^ 0x77u, i, 3);
        float tw = (float)p->h.W / p->scale * 1.7f;
        float th = (float)p->h.H / p->scale * 1.7f;
        float hx = dhash2(p->seed ^ 0x11u, i, 1) * tw + 26.0f * sinf(p->time * 0.11f + (float)i);
        float hy = dhash2(p->seed ^ 0x12u, i, 2) * th + 18.0f * cosf(p->time * 0.09f + (float)i * 1.7f);
        float ax = p->camx * par + (float)p->h.W / p->scale * 0.5f;
        float ay = p->camy * par + (float)p->h.H / p->scale * 0.5f;
        float bx = tile_near(hx, ax, tw);
        float by = tile_near(hy, ay, th);

        float sx = (bx - p->camx * par) * p->scale;
        float sy = (by - p->camy * par) * p->scale;
        if (sx < -180.0f || sy < -180.0f
            || sx > p->h.W + 180.0f || sy > p->h.H + 180.0f) continue;

        float rad = (13.0f + 26.0f * dhash2(p->seed ^ 0x13u, i, 4)) * p->scale;
        float hue = dhash2(p->seed ^ 0x14u, i, 5);
        C3 col = dhsv(hue, 0.34f, 0.62f);
        /* Barely there: far things are almost the water. */
        d_ball(&p->h, sx, sy, rad, fade(p, col, 0.86f), fade(p, cscl(col, 0.7f), 0.9f),
               KEY_X, KEY_Y, 0.30f, rad * 0.55f);
    }
}

/* ------------------------------------------------------------------ *
 * bubbles
 *
 * The single most characteristic object in a lit water scene, and the one that
 * would look wrong drawn as anything else. A bubble is not a light source and
 * it is not a solid: it is a thin curved wall around nothing, so almost all of
 * it is the water behind it, its rim is bright because there the wall is
 * edge-on and you are looking through the most of it, and it carries exactly
 * one small hard specular where the key hits the sphere.
 * ------------------------------------------------------------------ */

static void draw_bubbles(Pond *p)
{
    /* Eighteen, not forty. Once the tiling fix put them all on screen the
     * frame became a picture of bubbles with some organisms behind it; they
     * are atmosphere and the animals are the subject. */
    for (int i = 0; i < 18; i++) {
        float s0 = dhash2(p->seed ^ 0x31u, i, 1);
        float rad = (7.0f + 26.0f * s0 * s0) * p->scale;
        float par = 0.72f + 0.30f * dhash2(p->seed ^ 0x32u, i, 2);

        float tw = (float)p->h.W / p->scale * 1.6f;
        float th = (float)p->h.H / p->scale * 1.6f;
        float hx = dhash2(p->seed ^ 0x33u, i, 3) * tw;
        float hy = dhash2(p->seed ^ 0x35u, i, 5) * th;
        /* Rising, and wobbling on the way up. */
        hy -= p->time * (16.0f + 26.0f * dhash2(p->seed ^ 0x34u, i, 4));
        hx += 9.0f * sinf(p->time * 0.7f + (float)i * 2.1f);

        float ax = p->camx * par + (float)p->h.W / p->scale * 0.5f;
        float ay = p->camy * par + (float)p->h.H / p->scale * 0.5f;
        float bx = tile_near(hx, ax, tw);
        float by = tile_near(hy, ay, th);

        float sx = (bx - p->camx * par) * p->scale;
        float sy = (by - p->camy * par) * p->scale;
        if (sx < -80.0f || sy < -80.0f
            || sx > p->h.W + 80.0f || sy > p->h.H + 80.0f) continue;
        if (rad < 2.0f) continue;

        /* Composited, not added.
         *
         * The first version drew the rim additively and it was invisible: on
         * water this bright there is almost no headroom left above the
         * background, so adding light to it moves it nowhere. Every part of a
         * bubble here is an `over`, and the interior is lifted slightly
         * brighter and bluer than the water rather than darkened - a bubble is
         * air, and air in water carries more light than the water does. */
        C3 inner = clerp(p->wtop, c3(0.92f, 0.97f, 1.0f), 0.28f);
        d_ball(&p->h, sx, sy, rad, inner, clerp(inner, p->wbot, 0.45f),
               KEY_X, KEY_Y, 0.30f, rad * 0.10f);

        /* The wall, as a ring: bright where it is edge-on, which is the entire
         * reason a bubble reads as a sphere rather than as a disc. Drawn as two
         * concentric strokes so the outer edge can be brighter than the inner. */
        d_ring_over(&p->h, sx, sy, rad * 0.88f, rad * 0.22f,
                    c3(0.95f, 0.99f, 1.0f), 0.46f, KEY_X, KEY_Y, 0.55f);

        /* A thin darker line just outside it, which is what separates a pale
         * bubble from pale water. Without it a big one dissolves. */
        d_occ(&p->h, sx, sy, rad * 1.02f, 0.95f, rad * 0.06f);
        d_occ(&p->h, sx, sy, rad * 0.80f, 1.08f, rad * 0.30f);

        /* One specular, up-left with the key, small and hard. */
        d_ball(&p->h, sx + rad * KEY_X * 0.50f, sy + rad * KEY_Y * 0.50f,
               rad * 0.20f, c3(1.0f, 1.0f, 1.0f), c3(0.9f, 0.95f, 1.0f),
               KEY_X, KEY_Y, 0.55f, rad * 0.05f);
    }
}

/* ------------------------------------------------------------------ *
 * food
 * ------------------------------------------------------------------ */

/* Plant food as a frond rather than a dot.
 *
 * The simulation stores one position and one radius; drawn literally that is a
 * green circle, and a pond full of green circles reads as a menu rather than
 * as somewhere anything lives. The stalks are derived from a hash of the
 * item's own slot, so a given clump keeps its shape for as long as it exists
 * while costing nothing to store.
 */
static void draw_algae(Pond *p, const CpFood *f, int idx, float sx, float sy, float rad)
{
    uint32_t s = p->seed ^ 0x9E37u;
    int n = 3 + (int)(dhash2(s, idx, 1) * 3.0f);
    C3 stalk = c3(0.24f, 0.46f, 0.16f);
    C3 leaf  = c3(0.55f, 0.86f, 0.22f);
    C3 tip   = c3(0.78f, 0.97f, 0.38f);

    for (int k = 0; k < n; k++) {
        float a = dhash2(s, idx, 10 + k) * 6.2832f;
        float len = rad * (1.5f + 1.5f * dhash2(s, idx, 20 + k));
        /* Each frond sways on its own phase, so a clump moves like weed
         * rather than like a rigid object being rotated. */
        float sway = 0.30f * sinf(p->time * 1.5f + f->phase + (float)k * 1.3f);
        float ex = sx + cosf(a + sway) * len;
        float ey = sy + sinf(a + sway) * len;
        float mx = sx + cosf(a + sway * 0.4f) * len * 0.55f;
        float my = sy + sinf(a + sway * 0.4f) * len * 0.55f;

        d_fil_over(&p->h, sx, sy, mx, my, rad * 0.30f, rad * 0.20f, stalk, 0.92f, 0.4f);
        d_fil_over(&p->h, mx, my, ex, ey, rad * 0.20f, rad * 0.13f, stalk, 0.92f, 0.4f);
        /* The berry at the tip is what the eye actually tracks, and what the
         * player is aiming at, so it is the brightest thing in the clump. */
        d_ball(&p->h, ex, ey, rad * 0.44f, leaf, cscl(leaf, 0.45f),
               KEY_X, KEY_Y, 0.96f, 0.35f);
        d_disc(&p->h, ex + rad * 0.12f * KEY_X, ey + rad * 0.12f * KEY_Y,
               rad * 0.16f, tip, 0.30f, 0.3f);
    }
    d_ball(&p->h, sx, sy, rad * 0.40f, stalk, cscl(stalk, 0.5f), KEY_X, KEY_Y, 0.9f, 0.4f);
}

static void draw_food(Pond *p, const CpWorld *w)
{
    for (int i = 0; i < CP_MAX_FOOD; i++) {
        const CpFood *f = &w->food[i];
        if (f->type == CP_FOOD_NONE) continue;
        float sx = psx(p, f->x), sy = psy(p, f->y);
        float rad = f->r * p->scale;
        if (sx < -70.0f || sy < -70.0f
            || sx > p->h.W + 70.0f || sy > p->h.H + 70.0f) continue;

        if (f->type == CP_FOOD_PLANT) {
            draw_algae(p, f, i, sx, sy, rad);
        } else {
            /* Meat: a soft irregular lump, warm against everything else in
             * frame. Two offset balls rather than one, because a perfect
             * circle reads as a berry and this is supposed to read as a piece
             * of something. */
            C3 flesh = c3(0.86f, 0.34f, 0.44f);
            C3 dark  = c3(0.42f, 0.12f, 0.20f);
            float wob = 0.30f * sinf(p->time * 2.0f + f->phase);
            d_ball(&p->h, sx, sy, rad * 1.05f, flesh, dark, KEY_X, KEY_Y, 0.95f, 0.4f);
            d_ball(&p->h, sx + rad * (0.42f + wob * 0.1f), sy - rad * 0.30f,
                   rad * 0.62f, flesh, dark, KEY_X, KEY_Y, 0.95f, 0.4f);
            d_disc(&p->h, sx - rad * 0.3f, sy - rad * 0.35f, rad * 0.30f,
                   c3(1.0f, 0.72f, 0.70f), 0.22f, rad * 0.4f);
        }
    }
}

/* ------------------------------------------------------------------ *
 * bodies
 * ------------------------------------------------------------------ */

/* Cilia: a fringe of short hairs around the rim, all leaning the same way and
 * beating out of phase with each other. Drawn under the body so they read as
 * attached to it rather than laid over it. */
static void draw_cilia(Pond *p, float sx, float sy, float rad, float phase, int n, C3 col)
{
    if (n <= 0) return;
    int m = 14 + n * 5;
    if (m > 42) m = 42;
    for (int i = 0; i < m; i++) {
        float a = (float)i / (float)m * 6.2832f;
        float beat = sinf(phase * 2.2f + (float)i * 0.85f);
        float len = rad * (0.30f + 0.13f * beat);
        float ax = sx + cosf(a) * rad * 0.92f;
        float ay = sy + sinf(a) * rad * 0.92f;
        float bend = 0.42f * beat;
        float ex = sx + cosf(a + bend) * (rad * 0.92f + len);
        float ey = sy + sinf(a + bend) * (rad * 0.92f + len);
        d_fil_over(&p->h, ax, ay, ex, ey, rad * 0.055f, rad * 0.018f, col, 0.85f, 0.35f);
    }
}

/* A flagellum: one long trailing whip with a travelling wave along it. */
static void draw_flagellum(Pond *p, float sx, float sy, float rad, float heading,
                           float phase, C3 col)
{
    const int SEG = 9;
    float px2 = sx - cosf(heading) * rad * 0.9f;
    float py2 = sy - sinf(heading) * rad * 0.9f;
    for (int i = 1; i <= SEG; i++) {
        float t = (float)i / SEG;
        float amp = rad * 0.42f * t;
        float wave = sinf(phase * 3.0f - t * 7.0f) * amp;
        float back = rad * (0.9f + t * 2.3f);
        float nx2 = sx - cosf(heading) * back - sinf(heading) * wave;
        float ny2 = sy - sinf(heading) * back + cosf(heading) * wave;
        float r0 = rad * 0.075f * (1.0f - t * 0.7f);
        d_fil_over(&p->h, px2, py2, nx2, ny2, r0, r0 * 0.8f, col, 0.9f, 0.35f);
        px2 = nx2; py2 = ny2;
    }
}

/* An eye: a white ball, a dark iris that looks the way the body is going, and
 * one specular. Eyes are what make a shape read as an animal rather than as a
 * blob, so they are drawn at full contrast even on distant cells. */
static void draw_eye(Pond *p, float cx, float cy, float r, float look)
{
    d_ball(&p->h, cx, cy, r, c3(0.98f, 0.98f, 0.95f), c3(0.60f, 0.62f, 0.66f),
           KEY_X, KEY_Y, 1.0f, 0.3f);
    d_ball(&p->h, cx + cosf(look) * r * 0.34f, cy + sinf(look) * r * 0.34f,
           r * 0.48f, c3(0.06f, 0.05f, 0.09f), c3(0.02f, 0.02f, 0.04f),
           KEY_X, KEY_Y, 1.0f, 0.25f);
    d_disc(&p->h, cx + KEY_X * r * 0.42f, cy + KEY_Y * r * 0.42f, r * 0.20f,
           c3(1.0f, 1.0f, 1.0f), 0.55f, 0.25f);
}

/* One organism, opaque, lit from the key, with its parts.
 *
 * `hue` and `sat` give the species its colour; `spikes/cilia/jaws/eyes` are the
 * counts the simulation actually tracks, so what is drawn is what the thing
 * can do rather than a decorative approximation of it.
 */
static void draw_body(Pond *p, float sx, float sy, float rad, float hue, float satu,
                      float heading, float phase, int spikes, int cilia, int jaws,
                      int eyes, int flag, float depth, float hurt)
{
    C3 skin  = dhsv(hue, satu, 0.86f);
    C3 shade = dhsv(hue + 0.02f, satu * 0.85f, 0.34f);
    skin  = fade(p, skin, depth);
    shade = fade(p, shade, depth);
    if (hurt > 0.0f) skin = clerp(skin, c3(1.0f, 0.86f, 0.86f), sat(hurt));

    C3 pale = clerp(skin, c3(0.98f, 0.96f, 0.90f), 0.55f);

    /* Under the body, so they attach to it. */
    draw_cilia(p, sx, sy, rad, phase, cilia, pale);
    if (flag > 0) draw_flagellum(p, sx, sy, rad, heading, phase, pale);

    /* Spikes radiate from the rim and point outward. They are drawn before the
     * body so the body's edge covers their roots. */
    for (int i = 0; i < spikes; i++) {
        float a = heading + (float)i / (float)(spikes > 0 ? spikes : 1) * 6.2832f;
        float base = rad * 0.86f;
        float len = rad * 0.95f;
        d_fil_over(&p->h, sx + cosf(a) * base, sy + sinf(a) * base,
                   sx + cosf(a) * (base + len), sy + sinf(a) * (base + len),
                   rad * 0.15f, rad * 0.012f, pale, 0.96f, 0.35f);
    }

    /* The body. A second, smaller, brighter ball offset toward the key gives
     * the sheen of something wet without a specular highlight, which on a
     * matte organism would read as plastic. */
    d_ball(&p->h, sx, sy, rad, skin, shade, KEY_X, KEY_Y, 0.97f, 0.4f);
    d_ball(&p->h, sx + rad * KEY_X * 0.22f, sy + rad * KEY_Y * 0.22f, rad * 0.62f,
           clerp(skin, c3(1.0f, 1.0f, 0.96f), 0.22f), skin, KEY_X, KEY_Y, 0.35f, rad * 0.28f);

    /* A darker rim all the way round, which is what separates one body from
     * another when two overlap. Without it a crowd turns into one shape. */
    d_arc(&p->h, sx, sy, rad * 0.95f, rad * 0.10f, 0.0f, 6.2832f,
          cscl(shade, 0.5f), 0.0f);
    d_occ(&p->h, sx, sy, rad * 1.0f, 0.86f, rad * 0.12f);
    d_ball(&p->h, sx, sy, rad * 0.90f, skin, shade, KEY_X, KEY_Y, 0.92f, 0.4f);

    /* Internal texture: a few darker vacuoles, fixed to the body so they turn
     * with it. Cheap, and it stops a large cell reading as a balloon. */
    for (int i = 0; i < 3; i++) {
        float a = heading * 0.4f + (float)i * 2.4f;
        float rr = rad * (0.20f + 0.16f * (float)i);
        d_ball(&p->h, sx + cosf(a) * rr, sy + sinf(a) * rr, rad * 0.16f,
               cscl(shade, 1.25f), cscl(shade, 0.8f), KEY_X, KEY_Y, 0.45f, rad * 0.1f);
    }

    /* A jaw, as a wedge of two dark mandibles at the front. */
    for (int i = 0; i < jaws && i < 2; i++) {
        float open = 0.22f + 0.14f * sinf(phase * 2.6f);
        for (int s = -1; s <= 1; s += 2) {
            float a = heading + (float)s * open;
            d_fil_over(&p->h, sx + cosf(heading) * rad * 0.7f,
                       sy + sinf(heading) * rad * 0.7f,
                       sx + cosf(a) * rad * 1.30f, sy + sinf(a) * rad * 1.30f,
                       rad * 0.13f, rad * 0.04f, c3(0.20f, 0.16f, 0.14f), 0.95f, 0.35f);
        }
    }

    /* Eyes on stalks, out front, splayed. */
    for (int i = 0; i < eyes && i < 4; i++) {
        float s = (i & 1) ? 1.0f : -1.0f;
        float tier = 0.34f + 0.30f * (float)(i / 2);
        float a = heading + s * tier;
        float ex = sx + cosf(a) * rad * 0.98f;
        float ey = sy + sinf(a) * rad * 0.98f;
        d_fil_over(&p->h, sx + cosf(a) * rad * 0.5f, sy + sinf(a) * rad * 0.5f,
                   ex, ey, rad * 0.11f, rad * 0.09f, pale, 0.95f, 0.35f);
        draw_eye(p, ex, ey, rad * 0.26f, heading);
    }
}

static void draw_npcs(Pond *p, const CpWorld *w)
{
    for (int i = 0; i < CP_MAX_CELLS; i++) {
        const CpCell *c = &w->cells[i];
        if (!c->alive) continue;
        float sx = psx(p, c->x), sy = psy(p, c->y);
        float rad = c->r * p->scale;
        if (sx < -rad * 3.0f || sy < -rad * 3.0f
            || sx > p->h.W + rad * 3.0f || sy > p->h.H + rad * 3.0f) continue;

        float heading = atan2f(c->vy, c->vx);
        float hurt = 1.0f - (c->hp_max > 0.0f ? c->hp / c->hp_max : 1.0f);
        draw_body(p, sx, sy, rad, c->hue, 0.62f, heading, c->phase,
                  c->spikes, c->cilia, c->jaws, c->eyes,
                  c->diet == CP_DIET_HERB ? 0 : 1, 0.10f, hurt * 0.5f);
    }
}

/* The player, drawn from the genome rather than from counts, because here the
 * angles are real: a spike at 90 degrees defends the flank and nothing else,
 * and the picture has to agree with the rule. */
static void draw_player(Pond *p, const CpWorld *w)
{
    const CpCell *pl = &w->player;
    float sx = psx(p, pl->x), sy = psy(p, pl->y);
    float rad = pl->r * p->scale;
    const CpStats *st = &w->stats;

    float hurt = 1.0f - (pl->hp_max > 0.0f ? pl->hp / pl->hp_max : 1.0f);
    C3 skin  = dhsv(0.075f, 0.70f, 0.92f);
    C3 shade = dhsv(0.045f, 0.72f, 0.36f);
    if (hurt > 0.55f) skin = clerp(skin, c3(1.0f, 0.80f, 0.78f), (hurt - 0.55f) * 1.6f);
    C3 pale = clerp(skin, c3(1.0f, 0.97f, 0.90f), 0.5f);

    draw_cilia(p, sx, sy, rad, pl->phase, st->n[CP_PART_CILIA], pale);
    if (st->n[CP_PART_FLAGELLA] > 0)
        draw_flagellum(p, sx, sy, rad, pl->heading, pl->phase, pale);

    /* Every part, at the bearing the genome says. */
    for (int i = 0; i < CP_MAX_PARTS; i++) {
        int t = w->genome.part[i].type;
        if (t == CP_PART_NONE) continue;
        float a = pl->heading + (float)w->genome.part[i].angle * (6.2832f / 256.0f);
        float bx = sx + cosf(a) * rad * 0.84f, by = sy + sinf(a) * rad * 0.84f;
        switch (t) {
        case CP_PART_SPIKE:
            d_fil_over(&p->h, bx, by, sx + cosf(a) * rad * 1.85f, sy + sinf(a) * rad * 1.85f,
                       rad * 0.16f, rad * 0.012f, pale, 0.97f, 0.35f);
            break;
        case CP_PART_JAW:
            for (int s = -1; s <= 1; s += 2) {
                float o = a + (float)s * (0.20f + 0.13f * sinf(pl->phase * 2.6f));
                d_fil_over(&p->h, sx + cosf(a) * rad * 0.6f, sy + sinf(a) * rad * 0.6f,
                           sx + cosf(o) * rad * 1.42f, sy + sinf(o) * rad * 1.42f,
                           rad * 0.15f, rad * 0.045f, c3(0.22f, 0.17f, 0.15f), 0.96f, 0.35f);
            }
            break;
        case CP_PART_FILTER:
            d_ball(&p->h, sx + cosf(a) * rad * 1.02f, sy + sinf(a) * rad * 1.02f,
                   rad * 0.34f, c3(0.72f, 0.86f, 0.55f), c3(0.28f, 0.40f, 0.20f),
                   KEY_X, KEY_Y, 0.95f, 0.35f);
            break;
        case CP_PART_PROBOSCIS:
            d_fil_over(&p->h, bx, by, sx + cosf(a) * rad * 1.6f, sy + sinf(a) * rad * 1.6f,
                       rad * 0.19f, rad * 0.11f, clerp(pale, c3(0.9f, 0.7f, 0.5f), 0.5f),
                       0.95f, 0.35f);
            break;
        case CP_PART_JET:
            d_fil_over(&p->h, bx, by, sx + cosf(a) * rad * 1.35f, sy + sinf(a) * rad * 1.35f,
                       rad * 0.24f, rad * 0.17f, cscl(shade, 1.3f), 0.95f, 0.35f);
            d_occ(&p->h, sx + cosf(a) * rad * 1.35f, sy + sinf(a) * rad * 1.35f,
                  rad * 0.14f, 0.55f, rad * 0.08f);
            break;
        case CP_PART_POISON:
            d_ball(&p->h, sx + cosf(a) * rad * 0.95f, sy + sinf(a) * rad * 0.95f,
                   rad * 0.26f, c3(0.62f, 0.34f, 0.80f), c3(0.24f, 0.10f, 0.34f),
                   KEY_X, KEY_Y, 0.95f, 0.35f);
            break;
        case CP_PART_ELECTRIC:
            d_ball(&p->h, sx + cosf(a) * rad * 0.95f, sy + sinf(a) * rad * 0.95f,
                   rad * 0.24f, c3(0.55f, 0.86f, 1.0f), c3(0.16f, 0.34f, 0.5f),
                   KEY_X, KEY_Y, 0.95f, 0.35f);
            break;
        default: break;
        }
    }

    d_ball(&p->h, sx, sy, rad, skin, shade, KEY_X, KEY_Y, 0.98f, 0.35f);
    d_ball(&p->h, sx + rad * KEY_X * 0.22f, sy + rad * KEY_Y * 0.22f, rad * 0.60f,
           clerp(skin, c3(1.0f, 1.0f, 0.96f), 0.26f), skin, KEY_X, KEY_Y, 0.38f, rad * 0.26f);
    d_occ(&p->h, sx, sy, rad * 1.0f, 0.84f, rad * 0.12f);
    d_ball(&p->h, sx, sy, rad * 0.90f, skin, shade, KEY_X, KEY_Y, 0.94f, 0.35f);

    for (int i = 0; i < 3; i++) {
        float a = pl->heading * 0.35f + (float)i * 2.4f;
        float rr = rad * (0.20f + 0.16f * (float)i);
        d_ball(&p->h, sx + cosf(a) * rr, sy + sinf(a) * rr, rad * 0.15f,
               cscl(shade, 1.35f), cscl(shade, 0.85f), KEY_X, KEY_Y, 0.45f, rad * 0.1f);
    }

    int eyes = st->n[CP_PART_EYE];
    for (int i = 0; i < eyes && i < 4; i++) {
        float s = (i & 1) ? 1.0f : -1.0f;
        float tier = 0.32f + 0.30f * (float)(i / 2);
        float a = pl->heading + s * tier;
        float ex = sx + cosf(a) * rad * 0.98f, ey = sy + sinf(a) * rad * 0.98f;
        d_fil_over(&p->h, sx + cosf(a) * rad * 0.5f, sy + sinf(a) * rad * 0.5f,
                   ex, ey, rad * 0.11f, rad * 0.09f, pale, 0.95f, 0.35f);
        draw_eye(p, ex, ey, rad * 0.27f, pl->heading);
    }

    /* The discharge, on the frame it happens. The one thing in this renderer
     * that is genuinely emitting, so the one thing drawn additively. */
    if (w->elec_flash > 0.0f) {
        float k = sat(w->elec_flash / 0.20f);
        float er = st->elec_radius * p->scale;
        d_arc(&p->h, sx, sy, er * (1.0f - k * 0.25f), er * 0.06f, 0.0f, 6.2832f,
              c3(0.65f, 0.90f, 1.0f), 2.4f * k);
        d_glow(&p->h, sx, sy, er * 0.8f, c3(0.35f, 0.65f, 1.0f), 0.5f * k);
    }
}

/* ------------------------------------------------------------------ *
 * hud
 *
 * Dark on light, which is the inversion the whole renderer implies: a thin
 * bright readout that worked over near-black water disappears over a lit pond.
 * Same layout in frame units as the rest of the project.
 * ------------------------------------------------------------------ */

static void pond_hud(Pond *p, const CpWorld *w)
{
    char buf[80];
    float u = p->h.ui, M = 26.0f * u;
    C3 ink = c3(0.05f, 0.09f, 0.11f);

    /* A pale plate to sit on, because dark text over water with a bright cell
     * behind it is the same legibility problem as before with the contrast
     * reversed. */
    float pw = 268.0f * u, ph = 66.0f * u;
    for (int y = 0; y < (int)ph; y++)
        for (int x = 0; x < (int)pw; x++) {
            float a = 0.62f * (1.0f - sat((float)x / pw * 1.15f - 0.15f) * 0.35f);
            hdr_over(&p->h, (int)(M - 8.0f * u) + x, (int)(M - 8.0f * u) + y,
                     c3(0.93f, 0.96f, 0.97f), a);
        }

    d_text_over(&p->h, M, M, 3.4f * u, "CELL STAGE", ink, 0.98f);
    snprintf(buf, sizeof(buf), "TIER %d/%d  GEN %d/%d",
             cp_world_tier(w) + 1, CP_TIERS, w->generation + 1, CP_GENERATIONS);
    d_text_over(&p->h, M, M + 30.0f * u, 2.3f * u, buf, cscl(ink, 1.6f), 0.95f);

    /* The meter, bottom centre, as a bar rather than a number: what matters
     * while playing is how much is left, not the value. */
    float bw = 320.0f * u, bh = 9.0f * u;
    float bx = (p->h.W - bw) * 0.5f, by = p->h.H - 34.0f * u;
    for (int y = 0; y < (int)bh; y++)
        for (int x = 0; x < (int)bw; x++)
            hdr_over(&p->h, (int)bx + x, (int)by + y, c3(0.10f, 0.14f, 0.16f), 0.42f);
    float fillw = bw * sat(w->dna / CP_DNA_GOAL);
    for (int y = 1; y < (int)bh - 1; y++)
        for (int x = 1; x < (int)fillw; x++)
            hdr_over(&p->h, (int)bx + x, (int)by + y, c3(0.55f, 0.92f, 0.45f), 0.95f);

    /* Health, immediately under it, in red, on the same scale. */
    float hp = w->player.hp_max > 0.0f ? w->player.hp / w->player.hp_max : 0.0f;
    float hy = by + bh + 4.0f * u;
    for (int y = 0; y < (int)(bh * 0.7f); y++)
        for (int x = 0; x < (int)bw; x++)
            hdr_over(&p->h, (int)bx + x, (int)hy + y, c3(0.10f, 0.14f, 0.16f), 0.42f);
    for (int y = 1; y < (int)(bh * 0.7f) - 1; y++)
        for (int x = 1; x < (int)(bw * sat(hp)); x++)
            hdr_over(&p->h, (int)bx + x, (int)hy + y, c3(0.92f, 0.32f, 0.30f), 0.95f);
}

/* ------------------------------------------------------------------ *
 * entry
 * ------------------------------------------------------------------ */

void cp_render_pond(const CpWorld *w, uint8_t *rgba, int W, int H)
{
    if (!w || !rgba || W < 16 || H < 16) return;

    Pond p;
    p.h.px = (float *)malloc(sizeof(float) * (size_t)W * H * 3);
    if (!p.h.px) return;
    p.h.W = W; p.h.H = H;
    p.h.expo = POND_EXPOSURE;
    p.h.ui = (float)W / 1280.0f;
    if (p.h.ui < 0.5f) p.h.ui = 0.5f;
    p.h.step = w->step;
    p.time = (float)w->step * CP_DT;
    p.seed = w->seed;
    p.tier = cp_world_tier(w);
    pond_palette(&p, p.tier);

    /* The same growth pullback the darkfield renderer uses, for the same
     * reason: at a fixed scale an hour of growth is only a fatter sprite. */
    float grow = w->stats.radius0 > 0.01f ? w->player.r / w->stats.radius0 : 1.0f;
    if (grow < 1.0f) grow = 1.0f;
    p.scale = POND_SCALE_REF * (float)W / 1280.0f / powf(grow, 0.72f);

    float viewW = (float)W / p.scale, viewH = (float)H / p.scale;
    p.camx = dclampf(w->player.x - viewW * 0.5f, 0.0f, CP_WORLD_W - viewW);
    p.camy = dclampf(w->player.y - viewH * 0.5f, 0.0f, CP_WORLD_H - viewH);
    if (CP_WORLD_W < viewW) p.camx = (CP_WORLD_W - viewW) * 0.5f;
    if (CP_WORLD_H < viewH) p.camy = (CP_WORLD_H - viewH) * 0.5f;

    /* Painter's order, back to front. There is no z-buffer here because in a
     * top-down scene the layers are known and fixed, and sorting 700 sprites
     * every frame to discover an order that never changes is work for nothing. */
    draw_water(&p);
    draw_far_layer(&p);
    draw_food(&p, w);
    draw_npcs(&p, w);
    draw_player(&p, w);
    draw_bubbles(&p);          /* in front: bubbles drift between you and it */

    /* No bloom.
     *
     * It was 41ms of a 64ms frame - two thirds of the cost of the picture -
     * and measuring it only confirmed what the strength constant had already
     * admitted: it was set to 0.10 because anything more turned clear water
     * into fog. Brightfield has no dark surround for a halo to live in, so a
     * four-octave gather was buying a difference that had to be squinted at
     * and paying two thirds of the frame for it. The darkfield renderer keeps
     * its bloom, because there the glow *is* the picture. */
    pond_hud(&p, w);
    resolve(&p.h, rgba);
    free(p.h.px);
}
