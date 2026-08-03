#include "cpore/civ.h"
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ *
 * Stage-4 renderer.
 *
 * The first three stages all put a camera in the world. A civilisation is not
 * a thing you stand next to, so this one is a map: orthographic, straight
 * down, the whole planet at once. That is not a shortcut - the readable state
 * here is who owns what, and a perspective camera hides exactly that.
 *
 * The ground is the same cp4_height() field stage 3 walked around on, shaded
 * with a plain hillshade, so the continent a player remembers from the
 * creature stage is recognisably the continent their cities sit on.
 * ------------------------------------------------------------------ */

#define MAXW 512
#define MAXH 320
#define PI 3.14159265358979f

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

typedef struct { float r, g, b; } C3;
static inline C3 c3(float r, float g, float b) { C3 v = { r, g, b }; return v; }

/* Nation colours. Chosen against the abyss palette's actual entries rather
 * than from a colour wheel: the player gets the cyan ramp, which nothing else
 * in the scene uses, and the rivals take warm, rose and violet so no two are
 * within a quantisation step of each other. */
static const C3 NATION[CP5_MAX_NATIONS] = {
    { 0.29f, 0.71f, 0.82f },   /* player  - cyan   */
    { 0.86f, 0.33f, 0.18f },   /* rival 1 - orange */
    { 0.80f, 0.17f, 0.36f },   /* rival 2 - rose   */
    { 0.58f, 0.44f, 0.86f },   /* rival 3 - violet */
};
static const C3 FREE_CITY = { 0.62f, 0.66f, 0.62f };

static C3 owner_col(int owner)
{
    if (owner < 0 || owner >= CP5_MAX_NATIONS) return FREE_CITY;
    return NATION[owner];
}

static inline float tonemap(float x)
{
    if (x < 0.0f) x = 0.0f;
    float a = x * (2.51f * x + 0.03f);
    float b = x * (2.43f * x + 0.59f) + 0.14f;
    return clampf(a / b, 0.0f, 1.0f);
}

typedef struct {
    uint8_t *fb;
    int      W, H;
    uint32_t seed;
    float    ox, oz, scale;      /* world = origin + pixel * scale */
} Map;

static inline void put(Map *m, int x, int y, C3 c)
{
    if (x < 0 || y < 0 || x >= m->W || y >= m->H) return;
    uint8_t *p = m->fb + 4 * ((size_t)y * m->W + x);
    p[0] = (uint8_t)(tonemap(c.r) * 255.0f);
    p[1] = (uint8_t)(tonemap(c.g) * 255.0f);
    p[2] = (uint8_t)(tonemap(c.b) * 255.0f);
    p[3] = 255;
}

static inline void blend(Map *m, int x, int y, C3 c, float a)
{
    if (x < 0 || y < 0 || x >= m->W || y >= m->H || a <= 0.0f) return;
    uint8_t *p = m->fb + 4 * ((size_t)y * m->W + x);
    p[0] = (uint8_t)(p[0] + (tonemap(c.r) * 255.0f - p[0]) * a);
    p[1] = (uint8_t)(p[1] + (tonemap(c.g) * 255.0f - p[1]) * a);
    p[2] = (uint8_t)(p[2] + (tonemap(c.b) * 255.0f - p[2]) * a);
}

static void world_to_px(const Map *m, float wx, float wz, float *px, float *py)
{
    *px = (wx - m->ox) / m->scale;
    *py = (wz - m->oz) / m->scale;
}

static void disc(Map *m, float cx, float cy, float r, C3 c, float a)
{
    int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
    int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            if (dx * dx + dy * dy > r * r) continue;
            blend(m, x, y, c, a);
        }
}

static void ring(Map *m, float cx, float cy, float r, float thick, C3 c, float a)
{
    float ri = r - thick;
    int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
    int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float d2 = dx * dx + dy * dy;
            if (d2 > r * r || d2 < ri * ri) continue;
            blend(m, x, y, c, a);
        }
}

/* An arc from twelve o'clock, used for both the defence and the conversion
 * gauges. Reading a city's state off its own icon is worth more here than any
 * amount of HUD, because the whole game is which city is about to flip. */
static void arc(Map *m, float cx, float cy, float r, float frac, C3 c)
{
    if (frac <= 0.0f) return;
    int n = (int)(frac * 34.0f) + 1;
    for (int i = 0; i < n; i++) {
        float a = -PI * 0.5f + (float)i / 34.0f * 2.0f * PI;
        if ((float)i / 34.0f > frac) break;
        blend(m, (int)(cx + cosf(a) * r), (int)(cy + sinf(a) * r), c, 0.95f);
    }
}

/* ---------------- terrain ---------------- */

/* Which nation's writ runs at a point, and how strongly. Influence falls off
 * with distance and rises with population, so a big city projects a wide
 * province and a small one barely colours its own hill. */
static int influence_at(const Cp5World *w, float x, float z, float *strength)
{
    int best = -2;
    float bestv = 0.0f, second = 0.0f;
    for (int i = 0; i < w->n_cities; i++) {
        const Cp5City *c = &w->city[i];
        if (!c->alive) continue;
        float dx = c->p.x - x, dz = c->p.z - z;
        float d = sqrtf(dx * dx + dz * dz);
        float reach = 190.0f + c->pop * 3.4f;
        float v = 1.0f - d / reach;
        if (v <= 0.0f) continue;
        if (v > bestv) { second = bestv; bestv = v; best = c->owner; }
        else if (v > second) second = v;
    }
    if (best == -2) { *strength = 0.0f; return -2; }
    /* the margin over the runner-up, so borders read as borders */
    *strength = clampf(bestv, 0.0f, 1.0f) * clampf((bestv - second) * 4.0f + 0.35f, 0.0f, 1.0f);
    return best;
}

static void draw_terrain(Map *m, const Cp5World *w)
{
    for (int y = 0; y < m->H; y++) {
        for (int x = 0; x < m->W; x++) {
            float wx = m->ox + ((float)x + 0.5f) * m->scale;
            float wz = m->oz + ((float)y + 0.5f) * m->scale;
            float h = cp4_height(m->seed, wx, wz);      /* y grows downward */
            float elev = -h;

            C3 col;
            if (h > CP5_SEA) {
                /* open water, shaded by how deep it is */
                float d = clampf((h - CP5_SEA) / 90.0f, 0.0f, 1.0f);
                col = c3(mixf(0.16f, 0.05f, d), mixf(0.40f, 0.17f, d), mixf(0.52f, 0.28f, d));
            } else {
                float nx, ny, nz;
                cp4_normal(m->seed, wx, wz, &nx, &ny, &nz);
                /* hillshade from the north-west, the convention every relief
                 * map uses because it is the one the eye reads as raised */
                float lam = clampf(-(nx * 0.42f + ny * 0.80f + nz * 0.42f), 0.0f, 1.5f);
                float band = clampf((elev + 20.0f) / 130.0f, 0.0f, 1.0f);
                C3 low  = c3(0.30f, 0.28f, 0.14f);
                C3 mid  = c3(0.17f, 0.33f, 0.14f);
                C3 high = c3(0.42f, 0.42f, 0.40f);
                C3 g = band < 0.5f
                     ? c3(mixf(low.r, mid.r, band * 2.0f), mixf(low.g, mid.g, band * 2.0f),
                          mixf(low.b, mid.b, band * 2.0f))
                     : c3(mixf(mid.r, high.r, (band - 0.5f) * 2.0f),
                          mixf(mid.g, high.g, (band - 0.5f) * 2.0f),
                          mixf(mid.b, high.b, (band - 0.5f) * 2.0f));
                float k = 0.45f + 0.85f * lam;
                col = c3(g.r * k, g.g * k, g.b * k);
            }

            /* territory wash */
            float st;
            int own = influence_at(w, wx, wz, &st);
            if (own != -2 && st > 0.0f) {
                C3 nc = owner_col(own);
                float a = st * (h > CP5_SEA ? 0.16f : 0.34f);
                col = c3(mixf(col.r, nc.r, a), mixf(col.g, nc.g, a), mixf(col.b, nc.b, a));
            }
            put(m, x, y, col);
        }
    }

    /* Borders, as a second pass over the influence field. Drawing them from
     * the field rather than from a polygon means they follow the coast and
     * split around a contested city for free. */
    for (int y = 1; y < m->H - 1; y++) {
        for (int x = 1; x < m->W - 1; x++) {
            float wx = m->ox + ((float)x + 0.5f) * m->scale;
            float wz = m->oz + ((float)y + 0.5f) * m->scale;
            float s0, s1, s2;
            int o0 = influence_at(w, wx, wz, &s0);
            if (o0 == -2) continue;
            int o1 = influence_at(w, wx + m->scale, wz, &s1);
            int o2 = influence_at(w, wx, wz + m->scale, &s2);
            if (o1 == o0 && o2 == o0) continue;
            C3 nc = owner_col(o0);
            blend(m, x, y, c3(nc.r * 1.5f + 0.12f, nc.g * 1.5f + 0.12f, nc.b * 1.5f + 0.12f),
                  0.85f);
        }
    }
}

/* ---------------- overlays ---------------- */

static void draw_spice(Map *m, const Cp5World *w)
{
    for (int i = 0; i < w->n_spice; i++) {
        float px, py;
        world_to_px(m, w->spice[i].x, w->spice[i].z, &px, &py);
        disc(m, px, py, 1.6f, c3(0.98f, 0.82f, 0.30f), 0.95f);
        ring(m, px, py, 3.2f, 1.0f, c3(0.70f, 0.52f, 0.14f), 0.55f);
    }
}

static void draw_units(Map *m, const Cp5World *w)
{
    for (int i = 0; i < CP5_MAX_UNITS; i++) {
        const Cp5Unit *u = &w->unit[i];
        if (!u->alive) continue;
        float px, py;
        world_to_px(m, u->p.x, u->p.z, &px, &py);
        C3 c = owner_col(u->owner);
        /* Each approach gets its own mark, not just its own colour: at this
         * scale a unit is three pixels and hue alone does not survive
         * quantisation. */
        switch (u->type) {
        case CP5_MIL:                                  /* a solid blip */
            disc(m, px, py, 1.7f, c, 1.0f);
            break;
        case CP5_ECO:                                  /* a hollow one */
            ring(m, px, py, 2.1f, 1.0f, c, 1.0f);
            break;
        default:                                       /* a cross */
            blend(m, (int)px, (int)py, c, 1.0f);
            blend(m, (int)px + 1, (int)py, c, 0.9f);
            blend(m, (int)px - 1, (int)py, c, 0.9f);
            blend(m, (int)px, (int)py + 1, c, 0.9f);
            blend(m, (int)px, (int)py - 1, c, 0.9f);
            break;
        }
    }
}

static void draw_cities(Map *m, const Cp5World *w)
{
    for (int i = 0; i < w->n_cities; i++) {
        const Cp5City *c = &w->city[i];
        if (!c->alive) continue;
        float px, py;
        world_to_px(m, c->p.x, c->p.z, &px, &py);
        C3 nc = owner_col(c->owner);
        float r = 2.2f + c->pop * 0.022f;

        disc(m, px, py, r + 1.2f, c3(0.03f, 0.05f, 0.06f), 0.60f);
        disc(m, px, py, r, nc, 0.96f);

        /* Defence outside, conversion further out - a city about to flip shows
         * it on its own icon, which matters more here than any HUD number. */
        arc(m, px, py, r + 2.2f, c->defence / 100.0f, c3(0.88f, 0.92f, 0.96f));
        if (c->contest >= 0 && c->convert > 0.0f)
            arc(m, px, py, r + 3.8f, c->convert, owner_col(c->contest));

        /* spice pips, so a rich city is obvious without reading a number */
        for (int k = 0; k < c->spice && k < 4; k++)
            blend(m, (int)(px - 3 + k * 2), (int)(py - r - 5), c3(0.98f, 0.82f, 0.30f), 1.0f);
    }
}

/* ---------------- HUD ---------------- */

static void draw_hud5(uint8_t *fb, int W, int H, const Cp5World *w)
{
    char buf[80];
    const Cp5Nation *me = &w->nation[CP5_PLAYER];

    #define PANEL(X,Y,PW,PH) do {                                             \
        cp_px_rect(fb, W, H, (X), (Y), (PW), (PH), 0.03f, 0.06f, 0.08f, 0.86f);\
        cp_px_rect(fb, W, H, (X), (Y), (PW), 1, 0.30f, 0.62f, 0.70f, 0.55f);  \
    } while (0)

    PANEL(3, 3, 118, 38);
    cp_px_text(fb, W, H, 6, 6, 1, "CIVILISATION", 0.52f, 0.88f, 0.94f, 1.0f);
    snprintf(buf, sizeof(buf), "CITIES %d/%d  T%d", me->cities, w->n_cities, w->step);
    cp_px_text(fb, W, H, 6, 15, 1, buf, 0.68f, 0.78f, 0.80f, 1.0f);
    snprintf(buf, sizeof(buf), "GOLD %-5d +%d", (int)me->money, (int)me->income);
    cp_px_text(fb, W, H, 6, 24, 1, buf, 0.94f, 0.84f, 0.36f, 1.0f);
    snprintf(buf, sizeof(buf), "M%.2f E%.2f R%.2f",
             (double)me->bonus[CP5_MIL], (double)me->bonus[CP5_ECO],
             (double)me->bonus[CP5_REL]);
    cp_px_text(fb, W, H, 6, 33, 1, buf, 0.60f, 0.72f, 0.74f, 1.0f);

    /* how the empire was actually built - the three approaches, side by side */
    PANEL(3, H - 40, 108, 37);
    cp_px_text(fb, W, H, 6, H - 37, 1, "TAKEN BY", 0.52f, 0.88f, 0.94f, 1.0f);
    snprintf(buf, sizeof(buf), "FORCE   %-3d (%d)", w->captured, w->units_built[CP5_MIL]);
    cp_px_text(fb, W, H, 6, H - 28, 1, buf, 0.90f, 0.46f, 0.34f, 1.0f);
    snprintf(buf, sizeof(buf), "TRADE   %-3d (%d)", w->bought, w->units_built[CP5_ECO]);
    cp_px_text(fb, W, H, 6, H - 20, 1, buf, 0.94f, 0.82f, 0.36f, 1.0f);
    snprintf(buf, sizeof(buf), "FAITH   %-3d (%d)", w->converted, w->units_built[CP5_REL]);
    cp_px_text(fb, W, H, 6, H - 12, 1, buf, 0.62f, 0.56f, 0.92f, 1.0f);

    /* the standings, which is the only thing that decides the episode */
    PANEL(W - 82, 3, 79, 12 + CP5_MAX_NATIONS * 8);
    cp_px_text(fb, W, H, W - 79, 6, 1, "POWERS", 0.52f, 0.88f, 0.94f, 1.0f);
    for (int n = 0; n < CP5_MAX_NATIONS; n++) {
        const Cp5Nation *na = &w->nation[n];
        C3 c = owner_col(n);
        int y = 15 + n * 8;
        cp_px_rect(fb, W, H, W - 79, y, 5, 5, c.r, c.g, c.b, na->alive ? 1.0f : 0.30f);
        /* the player has no fixed doctrine - that is the whole point of it
         * being the agent's choice - so label the row rather than the style */
        snprintf(buf, sizeof(buf), "%-9s %d",
                 n == CP5_PLAYER ? "you" : cp5_approach_name(na->style), na->cities);
        cp_px_text(fb, W, H, W - 71, y - 1, 1, buf,
                   na->alive ? 0.80f : 0.36f, na->alive ? 0.86f : 0.38f,
                   na->alive ? 0.84f : 0.38f, 1.0f);
    }

    if (w->status != CP5_RUN) {
        const char *msg = w->status == CP5_WON  ? "THE WORLD IS YOURS"
                        : w->status == CP5_LOST ? "YOUR NATION IS GONE" : "TIME UP";
        int tw = cp_px_text_w(msg, 1);
        int bx = (W - tw) / 2, by = H / 2 - 7;
        PANEL(bx - 6, by - 4, tw + 12, 16);
        cp_px_text(fb, W, H, bx, by, 1, msg,
                   w->status == CP5_WON ? 0.52f : 1.0f,
                   w->status == CP5_WON ? 0.94f : 0.46f, 0.60f, 1.0f);
    }
    #undef PANEL
}

/* ---------------- entry ---------------- */

void cp5_render_styled(const Cp5World *w, uint8_t *rgba, int OW, int OH, int style)
{
    int32_t lw = 320, lh = 180;
    cp_vis_dims(style, &lw, &lh);
    if (lw > MAXW) lw = MAXW;
    if (lh > MAXH) lh = MAXH;

    uint8_t *fb = (uint8_t *)malloc((size_t)lw * lh * 4);
    if (!fb) return;

    Map m;
    m.fb = fb; m.W = lw; m.H = lh;
    m.seed = w->seed;
    /* Fit the whole planet, letterboxed by whichever axis is tighter. The map
     * never scrolls: a civilisation you can only see part of is one you cannot
     * plan against, and the agent's observation covers all of it anyway. */
    float sx = CP5_W / (float)lw, sz = CP5_D / (float)lh;
    m.scale = sx > sz ? sx : sz;
    m.ox = CP5_W * 0.5f - m.scale * lw * 0.5f;
    m.oz = CP5_D * 0.5f - m.scale * lh * 0.5f;

    draw_terrain(&m, w);
    draw_spice(&m, w);
    draw_units(&m, w);
    draw_cities(&m, w);
    draw_hud5(fb, lw, lh, w);

    cp_vis_quantise(fb, lw, lh, style);
    cp_vis_blit(fb, lw, lh, rgba, OW, OH, style);
    free(fb);
}

void cp5_render(const Cp5World *w, uint8_t *rgba, int W, int H)
{
    cp5_render_styled(w, rgba, W, H, CP_VIS_ABYSS);
}
