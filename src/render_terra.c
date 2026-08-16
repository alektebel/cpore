/* "Vista" - the creature stage in continuous tone.
 *
 * A second renderer for stage 3, alongside the pixel one in render_land.c, on
 * the same argument the cell stage made: almost nothing in a palette pipeline
 * survives having its palette removed. Coverage there is thresholded, which
 * reads as deliberate only while every edge sits on the pixel grid; the
 * shading is tuned against a fixed 48 colours; and the outline pass exists to
 * fight quantisation artefacts that are not present here.
 *
 * What this one is after is the thing a landscape actually does, which is not
 * detail. Look at any wide shot of real country and the information is almost
 * entirely in how contrast and saturation decay with distance: a ridge two
 * kilometres out is not a smaller ridge, it is a flatter, bluer, lower-contrast
 * one. Stack four of those and the eye reads depth before it reads anything
 * else. So the atmosphere here is not a fog term applied at the end - it is the
 * primary drawing tool, deliberately stronger than physics would have it, and
 * everything else is arranged so as not to fight it.
 *
 * Three consequences follow, and they are why this is a separate file:
 *
 *   - Light is posterised, not colour. The flat look of a stylised landscape
 *     comes from banding the *response* to light per material, which keeps
 *     albedo continuous and boundaries clean. Banding the final image instead
 *     couples every material to every other one and weaves the result.
 *   - Shadows shift hue rather than merely darkening. The shaded side of a
 *     hill in daylight is lit by the sky, and the sky is blue; a shadow that
 *     only loses value reads as dirt.
 *   - Foliage transmits. A backlit leaf glows, and that single term does more
 *     to make a tree look like a tree than any amount of silhouette work.
 *
 * The other half of the file is arithmetic. The pixel renderer spends 95% of
 * its frame inside cp4_height, because it ray-marches a two-dimensional
 * function at roughly a hundred samples per pixel and each of those is ten
 * octaves of value noise with derivatives. That is the wrong algorithm for
 * the shape of the problem, and it is what makes everything above unaffordable.
 * Sampling the field once into a grid and marching the grid costs about twenty
 * times less, which is what buys the resolution, the supersampled edges and
 * the extra light terms.
 */

#include "cpore/land.h"
#include "sdfbody.h"
#include "landbody.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "hdrcanvas.h"

#define TPI CP_PI

/* Highest ground the noise can produce, plus slack, and how far the marcher
 * will look. Both are in world units and y is down, so "above PEAK" is
 * y < -PEAK. */
#define PEAK    190.0f
#define FARCLIP 2400.0f

/* Depth written by the sky, and the value the buffer is cleared to. They have
 * to differ: the depth test rejects anything not strictly nearer than what is
 * already there, so clearing to the sky's own depth means no sky pixel ever
 * passes its own test and the whole upper half of the frame keeps whatever
 * was in the allocation. */
#define Z_SKY   1.0e9f
#define Z_CLEAR 1.0e30f

/* ------------------------------------------------------------------ *
 * the terrain tile
 *
 * cp4_height is a pure function of seed and position, which is what lets the
 * world be unbounded and need no storage. It is also ten octaves of value
 * noise carrying its own derivatives, and a ray marcher asks for it about a
 * hundred times per pixel - so the purity that makes the simulation elegant
 * makes the renderer cost ten seconds a frame.
 *
 * The fix is not to make the function cheaper. It is to notice that a frame
 * only ever looks at one neighbourhood of it, and that the function is
 * two-dimensional while the sampling is three-dimensional. Evaluate it once
 * onto a grid around the camera and march that instead: a million samples
 * built once, rather than twenty million taken one at a time.
 *
 * Everything downstream then reads the grid and not the field - normals,
 * ambient occlusion, shadow rays, the height a tree stands on - so there is
 * one terrain rather than two that disagree at the seams.
 * ------------------------------------------------------------------ */

#define TILE_N    1024               /* cells per side                     */
#define TILE_CELL 5.0f               /* world units per cell               */
/* 5120 units across, so +-2560 around the camera: past the marcher's reach,
 * which means no ray ever leaves the tile while it still matters. */

typedef struct {
    float  x0, z0;                   /* world position of cell (0,0)       */
    float *surf;                     /* topmost of ground and water        */
    float *grnd;                      /* ground alone, so water is knowable */
} Tile;

static int tile_build(Tile *t, uint32_t seed, float cx, float cz)
{
    size_t n = (size_t)TILE_N * TILE_N;
    t->surf = (float *)malloc(sizeof(float) * n);
    t->grnd = (float *)malloc(sizeof(float) * n);
    if (!t->surf || !t->grnd) { free(t->surf); free(t->grnd); return 0; }

    /* Snapped to the cell grid. Without this the sampling lattice slides
     * under the camera as it walks and every surface shimmers, which is the
     * one artefact a cache like this can introduce that the true function
     * cannot have. */
    t->x0 = floorf((cx - TILE_N * 0.5f * TILE_CELL) / TILE_CELL) * TILE_CELL;
    t->z0 = floorf((cz - TILE_N * 0.5f * TILE_CELL) / TILE_CELL) * TILE_CELL;

    for (int j = 0; j < TILE_N; j++) {
        float wz = t->z0 + (float)j * TILE_CELL;
        float *rs = t->surf + (size_t)j * TILE_N;
        float *rg = t->grnd + (size_t)j * TILE_N;
        for (int i = 0; i < TILE_N; i++) {
            float wy;
            float g = cp4_height_water(seed, t->x0 + (float)i * TILE_CELL, wz, &wy);
            rg[i] = g;
            rs[i] = wy < g ? wy : g;      /* y is down: smaller is higher */
        }
    }
    return 1;
}

static void tile_free(Tile *t) { free(t->surf); free(t->grnd); t->surf = t->grnd = NULL; }

/* Bilinear, clamped at the rim. A ray that leaves the tile is already past
 * FARCLIP and about to be handed to the sky. */
static inline float tile_at(const float *m, const Tile *t, float x, float z)
{
    float fx = (x - t->x0) / TILE_CELL, fz = (z - t->z0) / TILE_CELL;
    if (fx < 0.0f) fx = 0.0f;
    if (fz < 0.0f) fz = 0.0f;
    if (fx > (float)(TILE_N - 2)) fx = (float)(TILE_N - 2);
    if (fz > (float)(TILE_N - 2)) fz = (float)(TILE_N - 2);
    int i = (int)fx, j = (int)fz;
    float u = fx - (float)i, v = fz - (float)j;
    u = u * u * (3.0f - 2.0f * u);
    v = v * v * (3.0f - 2.0f * v);
    const float *r0 = m + (size_t)j * TILE_N + i;
    const float *r1 = r0 + TILE_N;
    return mixf(mixf(r0[0], r0[1], u), mixf(r1[0], r1[1], u), v);
}

static inline float tile_surf(const Tile *t, float x, float z) { return tile_at(t->surf, t, x, z); }
static inline float tile_grnd(const Tile *t, float x, float z) { return tile_at(t->grnd, t, x, z); }

/* Skyward normal from the cached field. Taken over a whole cell rather than a
 * hair's breadth: the grid is bilinear, so a narrow difference returns the
 * facet's own constant slope and the terrain shades as a chequerboard of
 * flats. A cell-wide stencil averages across the joins and gives back the
 * smooth surface the samples were taken from. */
static V3 tile_normal(const Tile *t, float x, float z)
{
    const float e = TILE_CELL;
    float hx = tile_surf(t, x + e, z) - tile_surf(t, x - e, z);
    float hz = tile_surf(t, x, z + e) - tile_surf(t, x, z - e);
    float dx = hx / (2.0f * e), dz = hz / (2.0f * e);
    float l = sqrtf(dx * dx + 1.0f + dz * dz);
    return v3(dx / l, -1.0f / l, dz / l);      /* y up is negative */
}

/* ------------------------------------------------------------------ *
 * context
 * ------------------------------------------------------------------ */

typedef struct {
    Hdr      h;
    float   *zb;                 /* distance per pixel, for the focus pass */
    int      W, H;
    V3       eye, fwd, right, up;
    float    focal;              /* pixels, from the field of view         */
    float    fdist;              /* what the lens is focused on            */
    uint32_t seed;
    float    time;
    V3       sun, sunc;
    float    day, mist;
    int      submerged, buried;
    /* A creature viewport is the same shading model with the world taken
     * away: no heightfield to shadow against and no kilometres of air to
     * look through. One flag short-circuits both rather than threading a
     * second set of branches through every term below. */
    int      studio;
    /* How much light transport the creature marcher is allowed: 0 flat, 1
     * ambient occlusion, 2 occlusion and self-shadowing. The world always
     * wants 2; an editor dragging a limb wants 0, and would rather have the
     * frame. Screen size alone cannot express that - a creature filling the
     * viewport is exactly when the shadow is most expensive and exactly when
     * the user is least able to see it. */
    int      detail;
    Tile     tile;
} Land;

static inline C3 v2c(V3 v) { return c3(v.x, v.y, v.z); }

static inline void putz(Land *c, int x, int y, V3 col, float z)
{
    if ((unsigned)x >= (unsigned)c->W || (unsigned)y >= (unsigned)c->H) return;
    size_t i = (size_t)y * c->W + x;
    if (z >= c->zb[i]) return;
    c->zb[i] = z;
    float *t = c->h.px + 3 * i;
    t[0] = col.x; t[1] = col.y; t[2] = col.z;
}

/* Partial coverage over a depth test. An antialiased silhouette has to blend
 * with whatever is behind it, but only the majority of a pixel gets to own
 * the depth - otherwise a hairline of grass writes its distance across the
 * whole frame and everything behind it defocuses wrongly. */
static inline void putz_cov(Land *c, int x, int y, V3 col, float z, float cov)
{
    if ((unsigned)x >= (unsigned)c->W || (unsigned)y >= (unsigned)c->H) return;
    if (cov <= 0.0f) return;
    size_t i = (size_t)y * c->W + x;
    if (z >= c->zb[i]) return;
    if (cov > 1.0f) cov = 1.0f;
    float *t = c->h.px + 3 * i;
    t[0] = mixf(t[0], col.x, cov);
    t[1] = mixf(t[1], col.y, cov);
    t[2] = mixf(t[2], col.z, cov);
    if (cov > 0.5f) c->zb[i] = z;
}

/* ------------------------------------------------------------------ *
 * noise and wind
 * ------------------------------------------------------------------ */

static inline V3 clerp3(V3 a, V3 b, float t)
{
    return v3(mixf(a.x, b.x, t), mixf(a.y, b.y, t), mixf(a.z, b.z, t));
}

static float rhash(uint32_t s, int x, int z)
{
    uint32_t h = (uint32_t)x * 0x27D4EB2Du ^ (uint32_t)z * 0x9E3779B1u ^ s;
    h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / 16777216.0f;
}

static float rnoise(uint32_t s, float x, float z)
{
    float fx = floorf(x), fz = floorf(z);
    int ix = (int)fx, iz = (int)fz;
    float tx = x - fx, tz = z - fz;
    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);
    float a = rhash(s, ix, iz),     b = rhash(s, ix + 1, iz);
    float c = rhash(s, ix, iz + 1), d = rhash(s, ix + 1, iz + 1);
    return mixf(mixf(a, b, tx), mixf(c, d, tx), tz);
}

static float fbm2(uint32_t s, float x, float z)
{
    return rnoise(s, x, z) * 0.55f + rnoise(s ^ 0x9Eu, x * 2.1f, z * 2.1f) * 0.28f
         + rnoise(s ^ 0x37u, x * 4.3f, z * 4.3f) * 0.17f;
}

static void wind_dir(const Land *c, float *wx, float *wz)
{
    float a = 0.8f + 0.55f * rnoise(c->seed ^ 0x2C1Fu, c->time * 0.006f, 0.0f);
    *wx = cosf(a); *wz = sinf(a);
}

static float wind_gust(const Land *c, float x, float z)
{
    float u = x * 0.0016f - c->time * 0.10f, v = z * 0.0016f - c->time * 0.06f;
    float g = rnoise(c->seed ^ 0x77E1u, u, v) * 0.68f
            + rnoise(c->seed ^ 0x55C7u, u * 3.3f, v * 3.3f) * 0.32f;
    g = clampf((g - 0.28f) * 1.9f, 0.0f, 1.0f);
    return g * g;
}

/* ------------------------------------------------------------------ *
 * the sun
 * ------------------------------------------------------------------ */

static V3 sun_dir(int32_t step)
{
    float a = cp4_sun_angle(step);
    float el = -cosf(a);
    return norm(v3(0.42f * sinf(a), 0.35f + 0.75f * el, -0.30f * cosf(a)));
}

/* Sunlight reddens as it sinks, because a long path through air scatters the
 * blue out of it - the same physics the aerial term models from the other
 * end, seen from the sun's side. One lerp buys the whole golden hour, and
 * without it noon and dusk are the same picture at different brightnesses. */
static V3 sun_colour(V3 sun)
{
    float t = sstep(0.0f, 0.55f, clampf(sun.y, 0.0f, 1.0f));
    V3 low  = v3(1.42f, 0.52f, 0.17f);
    V3 high = v3(1.05f, 1.00f, 0.92f);
    return v3(mixf(low.x, high.x, t), mixf(low.y, high.y, t), mixf(low.z, high.z, t));
}

/* ------------------------------------------------------------------ *
 * materials and light
 *
 * The whole stylisation lives in this section. Nothing below the shading
 * function knows about bands, hue shifts or transmission - they get an albedo
 * and a normal and a material tag, and the tag decides how light is allowed
 * to behave on them.
 * ------------------------------------------------------------------ */

enum { MAT_GROUND = 0, MAT_ROCK, MAT_WATER, MAT_FOLIAGE, MAT_BARK, MAT_HIDE,
       MAT_SNOW, MAT_COUNT };

typedef struct {
    float bands;     /* how many steps the key light is quantised to, 0 = off */
    float soft;      /* width of the transition between steps, in band units  */
    float wrap;      /* how far light bends past the terminator               */
    float spec;      /* specular strength                                     */
    float gloss;     /* Blinn exponent                                        */
    float trans;     /* how much light comes through from behind              */
} Mat;

/* Grass and foliage band hardest, because they are the materials a stylised
 * landscape reads as "drawn": a meadow in two tones with a clean boundary is
 * the whole look. Rock stays nearly continuous - a cliff quantised to three
 * steps reads as a papercraft model, not as stone - and hide keeps a very
 * wide terminator so an animal stays round. */
static const Mat MAT[MAT_COUNT] = {
    /* bands soft  wrap  spec  gloss trans */
    {  3.0f, 0.18f, 0.22f, 0.04f,  22.0f, 0.00f },   /* ground  */
    {  0.0f, 0.00f, 0.14f, 0.06f,  30.0f, 0.00f },   /* rock    */
    {  0.0f, 0.00f, 0.05f, 1.30f, 220.0f, 0.00f },   /* water   */
    {  2.0f, 0.22f, 0.30f, 0.10f,  26.0f, 1.00f },   /* foliage */
    {  0.0f, 0.00f, 0.18f, 0.03f,  18.0f, 0.00f },   /* bark    */
    {  0.0f, 0.00f, 0.48f, 0.22f,  46.0f, 0.22f },   /* hide    */
    {  2.0f, 0.26f, 0.20f, 0.30f,  70.0f, 0.00f },   /* snow    */
};

/* Posterise a 0..1 response into flat plateaus with soft joins.
 *
 * This is the one function that separates a stylised landscape from a
 * photographic one, and it has to act on the *lighting* rather than on the
 * image: banding the response leaves albedo continuous, so two different
 * greens under the same light stay two different greens, and the boundary
 * between plateaus is clean because it follows the geometry. Banding the
 * finished frame instead - which is what a palette does - couples every
 * material to every other one, and the boundary follows the histogram. */
static inline float ramp(float t, float bands, float soft)
{
    if (bands < 1.5f) return t;
    float s = t * bands;
    float i = floorf(s), f = s - i;
    if (i >= bands) { i = bands - 1.0f; f = 1.0f; }
    return (i + sstep(0.5f - soft, 0.5f + soft, f)) / bands;
}

/* Where the shadow half of the world gets its colour.
 *
 * Outdoors in daylight nothing is lit by the sun alone: the shaded side of
 * everything is lit by the whole sky, and the sky is blue. A shadow written
 * as a darker copy of the lit colour is the single most reliable way to make
 * an outdoor render look like an indoor one. */
static V3 ambient(const Land *c, float up)
{
    float night = 1.0f - c->day;
    V3 skyc = clerp3(v3(0.30f, 0.42f, 0.86f), v3(0.42f, 0.56f, 1.00f), c->day);
    V3 grdc = v3(0.52f, 0.44f, 0.30f);
    float sky = (0.25f * c->day + 0.10f * night) * (0.35f + 0.65f * up);
    float grd = (0.12f * c->day + 0.03f * night) * (1.0f - up);
    return add(mul(skyc, sky), mul(grdc, grd));
}

static V3 shade(const Land *c, V3 albedo, V3 n, V3 ray, float ao, float shadow,
                int mat, float thick)
{
    const Mat *m = &MAT[mat];
    V3 tosun = mul(c->sun, -1.0f);
    float up = clampf(-n.y, 0.0f, 1.0f);

    /* Key. Wrapped past the terminator so nothing goes black, then banded. */
    float lam = clampf(-dot(n, c->sun), -1.0f, 1.0f);
    float t = clampf((lam + m->wrap) / (1.0f + m->wrap), 0.0f, 1.0f);
    float key = ramp(t, m->bands, m->soft) * shadow * (0.08f + 0.92f * c->day);

    V3 amb = ambient(c, up);
    V3 light = add(mul(amb, ao), mul(c->sunc, key * 1.42f));
    V3 col = v3(albedo.x * light.x, albedo.y * light.y, albedo.z * light.z);

    /* Specular. Blinn against the half vector, and the only reason wet sand,
     * a river and a wet flank do not all read as matte paper. */
    if (m->spec > 0.0f) {
        V3 toeye = mul(ray, -1.0f);
        V3 hv = norm(add(tosun, toeye));
        float ndh = clampf(-dot(n, hv), 0.0f, 1.0f);
        float s = powf(ndh, m->gloss) * m->spec * shadow * c->day;
        col = add(col, mul(c->sunc, s));
    }

    /* Transmission. A leaf with the sun behind it glows, and that one term
     * does more to make a canopy read as foliage than any amount of work on
     * its silhouette. Cheap here because the geometry already knows roughly
     * how thick it is. */
    if (m->trans > 0.0f && thick > 0.0f) {
        float back = clampf(dot(ray, tosun), 0.0f, 1.0f);
        float tr = powf(back, 3.2f) * thick * m->trans * c->day;
        V3 tint = v3(albedo.x * 1.10f + 0.10f, albedo.y * 1.45f + 0.16f, albedo.z * 0.60f);
        col = add(col, mul(v3(tint.x * c->sunc.x, tint.y * c->sunc.y, tint.z * c->sunc.z),
                           tr * 0.85f));
    }

    return col;
}

/* ------------------------------------------------------------------ *
 * sky and atmosphere
 * ------------------------------------------------------------------ */

static V3 horizon_col(const Land *c)
{
    V3 day = v3(0.52f, 0.70f, 1.00f);
    V3 ngt = v3(0.055f, 0.070f, 0.135f);
    return clerp3(ngt, day, c->day);
}

static V3 sky_col(const Land *c, V3 ray)
{
    float up = clampf(-ray.y, 0.0f, 1.0f);

    if (c->buried) return v3(0.030f, 0.021f, 0.014f);
    if (c->submerged) {
        V3 deep = v3(0.022f, 0.090f, 0.125f);
        V3 surf = v3(0.190f, 0.440f, 0.500f);
        return clerp3(deep, surf, powf(up, 1.4f));
    }

    /* The exponent decides how much of the frame is horizon. Past about 0.5
     * the pale band creeps into the top of the shot and the sky stops having
     * a gradient at all. */
    V3 zen = v3(0.085f, 0.190f, 0.520f);
    V3 hor = horizon_col(c);
    float t = powf(up, 0.42f);
    V3 col = clerp3(hor, zen, t);

    {
        V3 nzen = v3(0.016f, 0.020f, 0.070f);
        V3 nhor = v3(0.040f, 0.050f, 0.105f);
        V3 ncol = clerp3(nhor, nzen, t);
        float dusk = clampf(1.0f - fabsf(c->day - 0.32f) / 0.32f, 0.0f, 1.0f);
        float lowband = powf(clampf(1.0f - up * 3.2f, 0.0f, 1.0f), 2.0f);
        ncol = add(ncol, mul(v3(0.78f, 0.32f, 0.20f), dusk * lowband * 0.60f));
        col = clerp3(ncol, col, c->day);

        /* Stars, hashed off the ray direction so they stay fixed to the sky
         * rather than to the screen while the camera turns. */
        if (c->day < 0.55f && up > 0.02f) {
            float sx = ray.x / (up + 0.35f), sz = ray.z / (up + 0.35f);
            float h = rhash(c->seed ^ 0x5741u, (int)floorf(sx * 220.0f),
                            (int)floorf(sz * 220.0f));
            if (h > 0.9950f) {
                float tw = 0.55f + 0.45f * sinf(c->time * 2.3f + h * 400.0f);
                float k = (1.0f - c->day / 0.55f) * tw;
                float m = (h - 0.9950f) / 0.0050f;
                col = add(col, mul(v3(0.80f, 0.86f, 1.00f), k * (0.25f + 0.95f * m)));
            }
        }
    }

    /* Sun disc, and the aureole around it. Written well past 1.0 on purpose:
     * this is the one thing in the frame that is genuinely a light source,
     * and the bloom needs something above white to bleed. */
    V3 tosun = mul(c->sun, -1.0f);
    float d = clampf(dot(ray, tosun), 0.0f, 1.0f);
    col = add(col, mul(v3(1.00f, 0.94f, 0.78f), powf(d, 3200.0f) * 26.0f));
    col = add(col, mul(v3(1.00f, 0.86f, 0.62f), powf(d, 42.0f) * 0.55f));
    col = add(col, mul(v3(0.94f, 0.80f, 0.58f), powf(d, 5.0f) * 0.22f));

    /* Cloud on a plane high overhead. Projecting onto the plane rather than
     * painting screen-space noise is what keeps it still as the camera turns,
     * which is most of what sells it as sky rather than as grain. */
    if (ray.y < -0.030f) {
        float tp = (-620.0f - c->eye.y) / ray.y;
        if (tp > 0.0f && tp < 90000.0f) {
            V3 h = add(c->eye, mul(ray, tp));
            float drift = c->time * 3.5f;
            float f = fbm2(c->seed ^ 0xBEEFu, (h.x + drift) * 0.00085f, h.z * 0.00085f);
            float cov = sstep(0.52f, 0.78f, f) * clampf(up * 3.0f, 0.0f, 1.0f);
            /* Two tones and a soft edge: a cloud lit from one side has a
             * bright shoulder and a grey base, and getting the base wrong is
             * what turns a sky into cotton wool. */
            float lit = 0.42f + 0.58f * powf(clampf(dot(ray, tosun), 0.0f, 1.0f), 1.6f);
            V3 cl = mul(v3(1.30f, 1.34f, 1.44f), (0.20f + 0.80f * c->day) * lit);
            V3 base = mul(v3(0.46f, 0.52f, 0.68f), 0.25f + 0.75f * c->day);
            V3 cc = clerp3(base, cl, sstep(0.45f, 0.95f, f));
            col = clerp3(col, cc, cov * 0.92f);
        }
    }
    return col;
}

/* Aerial perspective.
 *
 * Two terms, not one. Extinction takes the surface's own colour away with
 * distance and takes blue away slowest, so what survives from far off is
 * blue; in-scatter adds the light the air itself is throwing into the beam,
 * which is blue away from the sun and a warm haze toward it. A single lerp
 * toward one fog colour cannot do both, which is why a one-term landscape
 * ends in a flat grey wall.
 *
 * The scale lengths are set against this world's view distance rather than
 * against a real atmosphere's, and deliberately shorter than physics wants.
 * That is the point: a ridge at eight hundred units resolving to eighty-five
 * percent haze is what makes it a flat blue band, and a flat blue band is
 * both the look and - since it has no high-frequency detail left - the
 * cheapest antialiasing available. */
static V3 aerial(const Land *c, V3 col, float dist, V3 ray)
{
    if (c->studio) return col;
    if (c->buried || c->submerged) {
        float fd = c->buried ? 62.0f : 300.0f;
        float cap = c->buried ? 0.88f : 0.95f;
        V3 h = c->buried ? v3(0.050f, 0.034f, 0.022f) : v3(0.045f, 0.150f, 0.195f);
        float near = c->buried ? 2.0f : 25.0f;
        float d = dist - near;
        if (d < 0.0f) d = 0.0f;
        float f = (1.0f - expf(-d / fd)) * cap;
        return clerp3(col, h, f);
    }

    /* Nothing within a hundred units scatters anything worth seeing, and
     * starting sooner tints the hillside the animal is standing on. */
    float d = dist - 110.0f;
    if (d < 0.0f) d = 0.0f;

    float tr = expf(-d / 1500.0f);
    float tg = expf(-d / 1050.0f);
    float tb = expf(-d /  680.0f);

    V3 tosun = mul(c->sun, -1.0f);
    float cosa = clampf(dot(ray, tosun), 0.0f, 1.0f);
    float mie = powf(cosa, 8.0f);
    float ray_amt = 0.72f + 0.28f * cosa * cosa;

    V3 blue = v3(0.30f, 0.48f, 0.92f);
    V3 warm = v3(1.05f, 0.80f, 0.52f);
    float lit = 0.08f + 0.92f * c->day;

    V3 sc = v3((blue.x * ray_amt + warm.x * mie * 1.7f) * lit,
               (blue.y * ray_amt + warm.y * mie * 1.7f) * lit,
               (blue.z * ray_amt + warm.z * mie * 1.7f) * lit);

    V3 out = v3(col.x * tr + sc.x * (1.0f - tr),
                col.y * tg + sc.y * (1.0f - tg),
                col.z * tb + sc.z * (1.0f - tb));

    /* Ground mist, integrated in closed form.
     *
     * The term above is uniform, so it makes a valley floor and the ridge
     * above it equally blue and the landscape flattens into layers of one
     * wash. Mist has a scale height: it pools in the low ground, and it is
     * what separates one ridge from the next. Density falling exponentially
     * with altitude integrates along a straight ray exactly, so there is
     * nothing to march - one exp and a divide for what a ray marcher would
     * charge fifty samples for. */
    if (c->mist > 0.0f) {
        const float K = 1.0f / 52.0f;
        float a0 = -(c->eye.y + CP4_SEA);
        float D = 0.0031f * c->mist;
        float ky = K * ray.y;
        float base = D * expf(-K * a0);
        float tau = (ky > 1e-4f || ky < -1e-4f)
                  ? base * (expf(ky * dist) - 1.0f) / ky
                  : base * dist;
        if (tau > 6.0f) tau = 6.0f;
        float f = (1.0f - expf(-tau)) * 0.90f;
        float toward = clampf(dot(ray, tosun), 0.0f, 1.0f);
        V3 lit_m = v3(0.56f + 0.72f * toward, 0.64f + 0.46f * toward,
                      0.90f + 0.06f * toward);
        lit_m = mul(lit_m, 0.14f + 0.86f * c->day);
        out = clerp3(out, lit_m, f);
    }
    return out;
}

/* ------------------------------------------------------------------ *
 * marching the tile
 * ------------------------------------------------------------------ */

static int terrain_march(const Land *c, V3 ro, V3 rd, float tmax, float *tout)
{
    float t = 1.0f, dt = 1.1f;
    for (int i = 0; i < 220 && t < tmax; i++) {
        V3 q = add(ro, mul(rd, t));
        if (q.y < -PEAK && rd.y <= 0.0f) return 0;          /* climbing away */
        if (q.y > tile_surf(&c->tile, q.x, q.z)) {          /* below the surface */
            float lo = t - dt, hi = t;
            for (int k = 0; k < 6; k++) {
                float mid = 0.5f * (lo + hi);
                V3 m = add(ro, mul(rd, mid));
                if (m.y > tile_surf(&c->tile, m.x, m.z)) hi = mid;
                else                                     lo = mid;
            }
            *tout = 0.5f * (lo + hi);
            return 1;
        }
        t += dt;
        dt *= 1.031f;
    }
    return 0;
}

static int terrain_exit(const Land *c, V3 ro, V3 rd, float tmax, float *tout)
{
    float t = 0.5f, dt = 0.9f;
    for (int i = 0; i < 110 && t < tmax; i++) {
        V3 q = add(ro, mul(rd, t));
        if (q.y < tile_grnd(&c->tile, q.x, q.z)) {
            float lo = t - dt, hi = t;
            for (int k = 0; k < 5; k++) {
                float mid = 0.5f * (lo + hi);
                V3 m = add(ro, mul(rd, mid));
                if (m.y < tile_grnd(&c->tile, m.x, m.z)) hi = mid;
                else                                     lo = mid;
            }
            *tout = 0.5f * (lo + hi);
            return 1;
        }
        t += dt;
        dt *= 1.06f;
    }
    return 0;
}

/* Ambient occlusion on the heightfield: how much sky a point can see.
 *
 * Direct light does not answer this - the shadow ray only knows about the
 * sun, and at noon almost nothing is in shadow - so without it a gully and a
 * ridge crest with the same normal are painted the same colour and the land
 * reads as a sheet with a gradient over it. Four rings growing quadratically,
 * so the same sixteen samples cover the ditch underfoot and the valley wall a
 * hundred units off. Nearly free now that the field is a lookup. */
static float terrain_ao(const Land *c, V3 q, V3 n)
{
    static const float DX[4] = {  0.92f, -0.85f,  0.20f, -0.31f };
    static const float DZ[4] = {  0.39f,  0.53f, -0.98f,  0.95f };
    float occ = 0.0f, wsum = 0.0f;
    for (int i = 1; i <= 4; i++) {
        float r = 7.0f * (float)(i * i);
        float w = 1.0f / (float)i;
        for (int k = 0; k < 4; k++) {
            float sx = q.x + DX[k] * r, sz = q.z + DZ[k] * r;
            V3 v = v3(sx - q.x, tile_surf(&c->tile, sx, sz) - q.y, sz - q.z);
            occ  += w * clampf(dot(n, v) / r, 0.0f, 1.0f);
            wsum += w;
        }
    }
    return clampf(1.0f - 1.30f * occ / wsum, 0.20f, 1.0f);
}

static float terrain_shadow(const Land *c, V3 q)
{
    if (c->studio) return 1.0f;
    V3 l = mul(c->sun, -1.0f);
    float res = 1.0f, t = 4.0f, dt = 4.5f;
    for (int i = 0; i < 26; i++) {
        V3 s = add(q, mul(l, t));
        if (s.y < -PEAK) break;
        float clr = tile_surf(&c->tile, s.x, s.z) - s.y;
        if (clr < 0.0f) return 0.0f;
        /* Penumbra: how close the ray passed to the ridge divided by how far
         * away that ridge was is the angle it subtends, which is the
         * softness. A hard in-or-out test draws every shadow edge as one line
         * across the grass, and reads as a seam rather than as a shadow. */
        float k = 1.7f * clr / t;
        if (k < res) res = k;
        t += dt;
        dt *= 1.24f;
    }
    return sstep(0.0f, 1.0f, clampf(res, 0.0f, 1.0f));
}

/* ------------------------------------------------------------------ *
 * ground colour
 * ------------------------------------------------------------------ */

static V3 biome_colour(int b, float band)
{
    V3 lo, hi;
    switch (b) {
    case CP4_BIOME_ICE:     lo = v3(0.60f, 0.67f, 0.76f); hi = v3(0.82f, 0.87f, 0.92f); break;
    case CP4_BIOME_TUNDRA:  lo = v3(0.34f, 0.35f, 0.28f); hi = v3(0.53f, 0.55f, 0.50f); break;
    case CP4_BIOME_TAIGA:   lo = v3(0.10f, 0.23f, 0.15f); hi = v3(0.20f, 0.32f, 0.22f); break;
    case CP4_BIOME_FOREST:  lo = v3(0.08f, 0.26f, 0.08f); hi = v3(0.19f, 0.38f, 0.13f); break;
    case CP4_BIOME_GRASS:   lo = v3(0.16f, 0.34f, 0.09f); hi = v3(0.32f, 0.46f, 0.14f); break;
    case CP4_BIOME_SAVANNA: lo = v3(0.38f, 0.33f, 0.11f); hi = v3(0.52f, 0.45f, 0.18f); break;
    case CP4_BIOME_DESERT:  lo = v3(0.58f, 0.44f, 0.22f); hi = v3(0.72f, 0.58f, 0.33f); break;
    default:                lo = v3(0.07f, 0.25f, 0.07f); hi = v3(0.15f, 0.36f, 0.11f); break;
    }
    return clerp3(lo, hi, band);
}

/* Which material the ground under a point behaves as, and what colour it is.
 * Returned together because the two decisions are the same decision: sand is
 * a different material from the cliff above it, not merely a different hue. */
static V3 ground_albedo(const Land *c, V3 q, float slope, float dist, int *mat)
{
    float elev = -q.y;
    *mat = MAT_GROUND;

    if (elev < -CP4_SEA) {
        float d = clampf((-elev - CP4_SEA) / 90.0f, 0.0f, 1.0f);
        V3 silt = clerp3(v3(0.50f, 0.46f, 0.34f), v3(0.16f, 0.20f, 0.19f), d);
        float ripple = 0.86f + 0.28f * rnoise(c->seed ^ 0x6Bu, q.x * 0.035f, q.z * 0.02f);
        return mul(silt, ripple);
    }

    float band = clampf((elev + 20.0f) / 150.0f, 0.0f, 1.0f);

    /* cp4_biome is a hard index, so painting straight from it draws a
     * coloured line across the landscape wherever two biomes meet. Under a
     * palette the dither hid that; in continuous tone it is the first thing
     * the eye finds. Two things fix it, and both are cheap: warp the lookup
     * so the boundary is a ragged natural edge rather than a smooth curve,
     * and read the field twice across the warp so the change happens over
     * tens of units instead of at a step. */
    V3 col;
    {
        /* Four reads spread over a couple of hundred units, warped so the
         * pattern of the spread is not itself visible, and averaged. Where
         * all four agree - which is nearly everywhere - this is one biome at
         * full strength; only within a couple of hundred units of a boundary
         * do they disagree, and there the average is the gradient the hard
         * index cannot provide. */
        static const float OX[4] = {  1.00f, -0.62f,  0.18f, -0.44f };
        static const float OZ[4] = {  0.22f,  0.74f, -1.00f, -0.36f };
        float w1 = rnoise(c->seed ^ 0xC3u, q.x * 0.0040f, q.z * 0.0040f) - 0.5f;
        float w2 = rnoise(c->seed ^ 0xD9u, q.x * 0.0135f, q.z * 0.0135f) - 0.5f;
        float wx = w1 * 150.0f + w2 * 44.0f, wz = w2 * 150.0f - w1 * 44.0f;
        col = v3(0, 0, 0);
        for (int k = 0; k < 4; k++) {
            float bxp = q.x + wx + OX[k] * 105.0f;
            float bzp = q.z + wz + OZ[k] * 105.0f;
            col = add(col, biome_colour(cp4_biome(c->seed, bxp, bzp), band));
        }
        col = mul(col, 0.25f);
    }

    /* Patchiness inside a biome. Two octaves rather than one: a single low
     * frequency is a slow wash the eye reads as lighting, and it takes the
     * finer one on top before the ground reads as ground. */
    float patch = rnoise(c->seed ^ 0x91u, q.x * 0.0065f, q.z * 0.0065f);
    float clump = rnoise(c->seed ^ 0xA3u, q.x * 0.024f, q.z * 0.024f);
    col = mul(col, 0.80f + 0.28f * patch + 0.16f * clump);

    float rocky = clampf((0.80f - slope) * 4.0f, 0.0f, 1.0f);
    if (rocky > 0.0f) {
        V3 rock = v3(0.17f, 0.155f, 0.145f);
        float grain = 0.82f + 0.36f * rnoise(c->seed ^ 0x3Du, q.x * 0.06f, q.z * 0.06f);
        col = clerp3(col, mul(rock, grain), rocky);
        if (rocky > 0.5f) *mat = MAT_ROCK;
    }

    V3 sand = v3(0.56f, 0.48f, 0.31f);
    float shore = clampf(1.0f - fabsf(elev + CP4_SEA) / 22.0f, 0.0f, 1.0f);
    col = clerp3(col, sand, shore);
    {
        /* Wet sand: the strip the water has just left. Sand darkens and
         * saturates when it is wet, which does more to seat a waterline than
         * foam on the other side of it does. */
        float wet = clampf(1.0f - (elev + CP4_SEA) / 7.0f, 0.0f, 1.0f)
                  * clampf((elev + CP4_SEA) / 1.5f + 1.0f, 0.0f, 1.0f);
        col = mul(col, 1.0f - 0.42f * wet);
        col.z *= 1.0f + 0.24f * wet;
    }

    float high = clampf((elev - CP4_SNOWLINE) / 42.0f, 0.0f, 1.0f);
    if (high > 0.0f) {
        col = clerp3(col, v3(0.80f, 0.85f, 0.92f), high);
        if (high > 0.5f) *mat = MAT_SNOW;
    }

    /* Three octaves of surface texture, each fading out at the distance where
     * one screen pixel starts to cover more than its wavelength. Without the
     * fade the finest one turns into salt and pepper across the middle
     * distance; with it, the ground has grain underfoot and reads as a clean
     * shape on the far ridge, which is what a real one does. */
    {
        float f1 = clampf(1.0f - dist / 700.0f, 0.0f, 1.0f);
        float f2 = clampf(1.0f - dist / 300.0f, 0.0f, 1.0f);
        float f3 = clampf(1.0f - dist / 120.0f, 0.0f, 1.0f);
        float n1 = rnoise(c->seed ^ 0x5Fu, q.x * 0.021f, q.z * 0.021f) - 0.5f;
        float n2 = rnoise(c->seed ^ 0x71u, q.x * 0.075f, q.z * 0.075f) - 0.5f;
        float n3 = rnoise(c->seed ^ 0x8Du, q.x * 0.240f, q.z * 0.240f) - 0.5f;
        float g = 1.0f + 0.34f * n1 * f1 + 0.26f * n2 * f2 + 0.20f * n3 * f3;
        col = mul(col, g);
        /* Hue as well as value. Ground that varies only in brightness reads as
         * one material under uneven light; varying the green against the red
         * is what makes it read as different stuff growing in patches. */
        col.y *= 1.0f + 0.13f * n2 * f2;
        col.x *= 1.0f - 0.09f * n1 * f1;
    }
    return col;
}

/* ------------------------------------------------------------------ *
 * scenery placement
 *
 * Trees, boulders and snags, hashed straight out of the cell they stand in.
 * Nothing is stored and nothing is simulated - the bushes the animals
 * actually eat are the separate, simulated Cp4Flora. Because the cell hash is
 * a pure function of position this streams with the unbounded world exactly
 * the way the terrain does.
 * ------------------------------------------------------------------ */

#define SCEN_CELL   78.0f
#define SCEN_RANGE  1150.0f

enum { SCEN_NONE = 0, SCEN_CONIFER, SCEN_BROADLEAF, SCEN_ROCK };

typedef struct {
    int   kind;
    float x, z, y;
    float h;
    float r1, r2, r3;
    int   biome;
} Scen;

/* Everything that stands on the ground has to stand on the *same* ground the
 * marcher drew. Asking cp4_height here instead of the tile puts a tree on the
 * true field and the hillside under it on the cached one, and the two differ
 * by whatever the caching smoothed away - so trunks sink and grass floats.
 * It is also the expensive answer to a question the tile has already been
 * built to answer. */
static int scenery_at(const Land *c, int cx, int cz, Scen *s)
{
    float r0 = rhash(c->seed ^ 0x1F17u, cx, cz);
    s->r1 = rhash(c->seed ^ 0x77A3u, cx, cz);
    s->r2 = rhash(c->seed ^ 0x3D5Bu, cx, cz);
    s->r3 = rhash(c->seed ^ 0x6E21u, cx, cz);

    s->x = ((float)cx + 0.15f + 0.70f * s->r1) * SCEN_CELL;
    s->z = ((float)cz + 0.15f + 0.70f * s->r2) * SCEN_CELL;
    s->y = tile_grnd(&c->tile, s->x, s->z);
    if (s->y > CP4_SEA - 6.0f) return 0;

    s->biome = cp4_biome(c->seed, s->x, s->z);

    float density;
    int kind;
    switch (s->biome) {
    case CP4_BIOME_JUNGLE:  density = 0.78f; kind = SCEN_BROADLEAF; break;
    case CP4_BIOME_FOREST:  density = 0.68f; kind = SCEN_BROADLEAF; break;
    case CP4_BIOME_TAIGA:   density = 0.60f; kind = SCEN_CONIFER;   break;
    case CP4_BIOME_GRASS:   density = 0.20f; kind = s->r3 < 0.5f  ? SCEN_BROADLEAF : SCEN_ROCK; break;
    case CP4_BIOME_SAVANNA: density = 0.16f; kind = s->r3 < 0.65f ? SCEN_BROADLEAF : SCEN_ROCK; break;
    case CP4_BIOME_TUNDRA:  density = 0.22f; kind = s->r3 < 0.35f ? SCEN_CONIFER   : SCEN_ROCK; break;
    case CP4_BIOME_DESERT:  density = 0.12f; kind = SCEN_ROCK; break;
    default:                density = 0.14f; kind = SCEN_ROCK; break;
    }
    /* Above the snowline the trees stop, which is most of what makes a
     * mountain read as a mountain rather than as a tall green hill. */
    {
        float elev = -s->y;
        if (elev > 96.0f) {
            density *= clampf((132.0f - elev) / 36.0f, 0.0f, 1.0f);
            if (kind == SCEN_BROADLEAF) kind = SCEN_CONIFER;
        }
    }
    {
        V3 n = tile_normal(&c->tile, s->x, s->z);
        if (clampf(-n.y, 0.0f, 1.0f) < 0.80f) { kind = SCEN_ROCK; density *= 1.4f; }
    }
    if (r0 > density) return 0;

    /* Two size classes. A stand where every trunk is within a factor of two
     * of every other reads as a crop; real cover is a few tall ones with
     * everything else coming up underneath. */
    float scale = s->r3 > 0.82f ? (1.35f + 0.55f * s->r1) : (0.50f + 0.62f * s->r3);
    s->kind = kind;
    s->h = kind == SCEN_ROCK ? (5.0f + 9.0f * s->r1) * scale
                             : (26.0f + 24.0f * s->r1) * scale;
    return 1;
}

#define SCEN_WIN 15

typedef struct {
    int           cx0, cz0;
    unsigned char have[SCEN_WIN * SCEN_WIN];
    Scen          s[SCEN_WIN * SCEN_WIN];
} ScenGrid;

static void scen_grid_build(const Land *c, ScenGrid *g)
{
    g->cx0 = (int)floorf(c->eye.x / SCEN_CELL) - SCEN_WIN / 2;
    g->cz0 = (int)floorf(c->eye.z / SCEN_CELL) - SCEN_WIN / 2;
    for (int j = 0; j < SCEN_WIN; j++)
        for (int i = 0; i < SCEN_WIN; i++) {
            int k = j * SCEN_WIN + i;
            g->have[k] = (unsigned char)scenery_at(c, g->cx0 + i, g->cz0 + j, &g->s[k]);
        }
}

/* How much light a point on the ground loses to whatever stands near it.
 * Without this a wood is a set of stickers on a lawn: the canopy is the
 * biggest single thing shading a forest floor, and the terrain shadow ray
 * knows nothing about it because the trees are not in the heightfield. */
static float scen_shade(const Land *c, const ScenGrid *g, float x, float z)
{
    /* Where the canopy's shadow actually lands: the crown sits about seven
     * tenths of the way up the tree, and light travelling along `sun` puts
     * its shadow that far downwind of the trunk, divided by how high the sun
     * is. The offset grows without limit as the sun sets, so it is capped -
     * past that the shadow is longer than the window the grid covers. */
    float sy = c->sun.y > 0.10f ? c->sun.y : 0.10f;
    float kx = c->sun.x / sy, kz = c->sun.z / sy;
    float kl = sqrtf(kx * kx + kz * kz);
    if (kl > 3.2f) { kx *= 3.2f / kl; kz *= 3.2f / kl; }

    /* Two cells of reach each way rather than one: a long shadow crosses a
     * cell boundary, and clipping it there draws a straight edge that is not
     * in the landscape. */
    int ci = (int)floorf(x / SCEN_CELL) - g->cx0;
    int cj = (int)floorf(z / SCEN_CELL) - g->cz0;
    float k = 0.0f;
    for (int dj = -2; dj <= 2; dj++) {
        int j = cj + dj;
        if ((unsigned)j >= (unsigned)SCEN_WIN) continue;
        for (int di = -2; di <= 2; di++) {
            int i = ci + di;
            if ((unsigned)i >= (unsigned)SCEN_WIN) continue;
            int n = j * SCEN_WIN + i;
            if (!g->have[n]) continue;
            const Scen *s = &g->s[n];
            float rad = s->kind == SCEN_ROCK ? s->h * 1.5f : s->h * 0.38f;
            float up = s->kind == SCEN_ROCK ? s->h * 0.55f : s->h * 0.72f;
            float dx = x - (s->x + kx * up), dz = z - (s->z + kz * up);
            /* Softening with the length of the throw, the way a real
             * penumbra does: the shadow directly under a rock is sharp and
             * the far end of a tree's is not. */
            float soft = 1.0f + 0.55f * kl;
            float d2 = (dx * dx + dz * dz) / (rad * rad * soft);
            if (d2 >= 1.0f) continue;
            float f = (1.0f - d2) * (1.0f - d2);
            if (f > k) k = f;
        }
    }
    return k;
}

/* ------------------------------------------------------------------ *
 * the world pass
 * ------------------------------------------------------------------ */

typedef struct { float x, z, r, h; } Blot;

static int gather_blots(const Cp4World *w, Blot *out, int max)
{
    int n = 0;
    for (int i = 0; i < CP4_MAX_BEASTS && n < max; i++) {
        if (!w->beast[i].alive) continue;
        out[n].x = w->beast[i].p.x; out[n].z = w->beast[i].p.z;
        out[n].r = w->beast[i].s.length * 0.45f + w->beast[i].s.radius;
        out[n].h = w->beast[i].s.stand;
        n++;
    }
    if (n < max && w->player.alive) {
        out[n].x = w->player.p.x; out[n].z = w->player.p.z;
        out[n].r = w->player.s.length * 0.45f + w->player.s.radius;
        out[n].h = w->player.s.stand;
        n++;
    }
    return n;
}

static void draw_world(Land *c, const Cp4World *w)
{
    Blot blot[28];
    int nblot = gather_blots(w, blot, 28);
    ScenGrid *sg = (ScenGrid *)malloc(sizeof(ScenGrid));
    if (sg) scen_grid_build(c, sg);

    for (int y = 0; y < c->H; y++) {
        for (int x = 0; x < c->W; x++) {
            float sx = ((float)x + 0.5f - c->W * 0.5f) / c->focal;
            float sy = ((float)y + 0.5f - c->H * 0.5f) / c->focal;
            V3 ray = norm(add(add(mul(c->right, sx), mul(c->up, -sy)), c->fwd));

            float t;
            int hit = c->buried ? terrain_exit(c, c->eye, ray, FARCLIP, &t)
                                : terrain_march(c, c->eye, ray, FARCLIP, &t);
            if (!hit) {
                V3 col = sky_col(c, ray);
                if (c->buried || c->submerged) col = aerial(c, col, FARCLIP, ray);
                putz(c, x, y, col, Z_SKY);
                continue;
            }

            V3 q = add(c->eye, mul(ray, t));
            V3 n = tile_normal(&c->tile, q.x, q.z);
            float slope = clampf(-n.y, 0.0f, 1.0f);

            int mat;
            V3 alb = ground_albedo(c, q, slope, t, &mat);

            /* Water is the surface, not the bed under it. Shading the bed and
             * calling it a lake gives a brown field with a blue tint; what
             * makes water read is that it is a mirror with something dim
             * underneath. */
            float gy = tile_grnd(&c->tile, q.x, q.z);
            int is_water = gy > q.y + 0.15f;
            if (is_water) {
                float depth = clampf((gy - q.y) / 40.0f, 0.0f, 1.0f);
                V3 bed = mul(alb, 1.0f - 0.75f * depth);
                V3 body = clerp3(v3(0.10f, 0.28f, 0.34f), v3(0.03f, 0.10f, 0.17f), depth);
                alb = clerp3(bed, body, sstep(0.0f, 0.55f, depth));
                /* ripples, so the specular has something to break up on */
                float rp = sinf(q.x * 0.09f + c->time * 1.6f)
                         + sinf(q.z * 0.11f - c->time * 1.3f);
                n = norm(v3(n.x + rp * 0.020f, n.y, n.z + rp * 0.017f));
                mat = MAT_WATER;
            }

            float ao = terrain_ao(c, q, n);
            float sh = terrain_shadow(c, q);

            /* Everything standing on the ground shades the ground. */
            if (sg) sh *= 1.0f - 0.80f * scen_shade(c, sg, q.x, q.z);
            for (int b = 0; b < nblot; b++) {
                float dx = q.x - blot[b].x, dz = q.z - blot[b].z;
                float r = blot[b].r * (1.0f + blot[b].h * 0.02f);
                float d2 = (dx * dx + dz * dz) / (r * r);
                if (d2 < 1.0f) {
                    float f = (1.0f - d2) * (1.0f - d2);
                    sh *= 1.0f - 0.55f * f;
                    ao *= 1.0f - 0.28f * f;
                }
            }

            V3 col = shade(c, alb, n, ray, ao, sh, mat, 0.0f);
            putz(c, x, y, aerial(c, col, t, ray), t);
        }
    }
    free(sg);
}

/* ------------------------------------------------------------------ *
 * projection helpers
 * ------------------------------------------------------------------ */

/* World point to screen, plus the distance along the view axis. Returns 0 if
 * the point is behind the camera. */
static int project(const Land *c, V3 wp, float *sx, float *sy, float *vz)
{
    V3 d = sub(wp, c->eye);
    float z = dot(d, c->fwd);
    if (z < 0.5f) return 0;
    *vz = z;
    *sx = c->W * 0.5f + dot(d, c->right) / z * c->focal;
    *sy = c->H * 0.5f - dot(d, c->up) / z * c->focal;
    return 1;
}

/* ------------------------------------------------------------------ *
 * canopies and boles
 *
 * A tree at fifty units is not a mesh problem, it is a silhouette problem
 * plus a transmission problem. The lobe below is a sphere impostor with a
 * real normal, a broken outline and a thickness - which is enough to light
 * it, enough to let the sun through it, and enough that a wood reads as a
 * wood instead of as a row of lollipops.
 * ------------------------------------------------------------------ */

static void canopy(Land *c, V3 wp, float rad, V3 albedo, float ragged, float lift, int mat)
{
    float sx, sy, vz;
    if (!project(c, wp, &sx, &sy, &vz)) return;
    float r = rad / vz * c->focal;
    if (r < 0.35f) return;

    int x0 = (int)floorf(sx - r - 1), x1 = (int)ceilf(sx + r + 1);
    int y0 = (int)floorf(sy - r - 1), y1 = (int)ceilf(sy + r + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->W) x1 = c->W - 1;
    if (y1 >= c->H) y1 = c->H - 1;

    /* Silhouette erosion in *world* space, so the outline breaks up the same
     * way whatever the camera does. Doing it in screen space is the classic
     * tell - the leaves crawl as you walk. */
    float inv = 1.0f / r;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float dx = ((float)x + 0.5f - sx) * inv;
            float dy = ((float)y + 0.5f - sy) * inv;
            float d2 = dx * dx + dy * dy;
            if (d2 > 1.30f) continue;

            /* the impostor's normal, as if it were a sphere */
            float nz2 = 1.0f - d2;
            float nzl = nz2 > 0.0f ? sqrtf(nz2) : 0.0f;
            V3 n = norm(add(add(mul(c->right, dx), mul(c->up, -dy)), mul(c->fwd, -nzl)));
            /* canopies are lit from above more than a sphere would be */
            n = norm(add(n, mul(v3(0.0f, -1.0f, 0.0f), lift)));

            V3 sp = add(wp, mul(add(mul(c->right, dx), mul(c->up, -dy)), rad));
            float fq = 1.9f / (rad > 0.6f ? rad : 0.6f);
            float leaf = fbm2(c->seed ^ 0x51A1u, (sp.x + sp.y * 0.7f) * fq,
                              (sp.z - sp.y * 0.5f) * fq);
            float cluster = fbm2(c->seed ^ 0x2E7Bu, (sp.x - sp.y * 0.4f) * fq * 4.5f,
                                 (sp.z + sp.y * 0.6f) * fq * 4.5f);
            /* The break has to happen at the silhouette's own scale, and be
             * bitten out before the edge is softened - smoothing first eats
             * the noise and hands back the circle it started from. */
            float edge = 1.0f - sqrtf(d2) + (leaf - 0.46f) * ragged;
            float cov = sstep(0.0f, 0.10f, edge);
            if (cov <= 0.0f) continue;

            float zz = vz - nzl * rad;
            size_t idx = (size_t)y * c->W + x;
            if (zz >= c->zb[idx]) continue;

            float thick = nzl;
            float sh = terrain_shadow(c, sp);
            float ao = 0.45f + 0.55f * clampf(nzl * 0.6f + 0.55f, 0.0f, 1.0f);
            V3 al = mul(albedo, 0.62f + 0.42f * leaf + 0.38f * cluster);
            V3 col = shade(c, al, n, norm(sub(sp, c->eye)), ao, sh, mat, thick);
            putz_cov(c, x, y, aerial(c, col, zz, norm(sub(sp, c->eye))), zz, cov);
        }
    }
}

/* A tapered limb in space: a trunk, a bough, a blade of grass, a reed. */
static void bole(Land *c, V3 a, V3 b, float ra, float rb, V3 albedo, int mat)
{
    float ax, ay, az, bx, by, bz;
    if (!project(c, a, &ax, &ay, &az)) return;
    if (!project(c, b, &bx, &by, &bz)) return;
    float wa = ra / az * c->focal, wb = rb / bz * c->focal;
    if (wa < 0.16f && wb < 0.16f) return;

    float rmax = (wa > wb ? wa : wb) + 1.5f;
    int x0 = (int)floorf((ax < bx ? ax : bx) - rmax), x1 = (int)ceilf((ax > bx ? ax : bx) + rmax);
    int y0 = (int)floorf((ay < by ? ay : by) - rmax), y1 = (int)ceilf((ay > by ? ay : by) + rmax);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->W) x1 = c->W - 1;
    if (y1 >= c->H) y1 = c->H - 1;

    float vx = bx - ax, vy = by - ay, vv = vx * vx + vy * vy;
    V3 axis = norm(sub(b, a));
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f - ax, py = (float)y + 0.5f - ay;
            float t = vv > 1e-6f ? clampf((px * vx + py * vy) / vv, 0.0f, 1.0f) : 0.0f;
            float dx = px - t * vx, dy = py - t * vy;
            float d = sqrtf(dx * dx + dy * dy);
            float rr = mixf(wa, wb, t);
            float cov = clampf(rr + 0.5f - d, 0.0f, 1.0f);
            if (cov <= 0.0f) continue;
            float zz = mixf(az, bz, t);
            size_t idx = (size_t)y * c->W + x;
            if (zz >= c->zb[idx]) continue;

            /* The normal of a cylinder: perpendicular to the axis, rotated
             * around it by how far across the silhouette this pixel is. */
            float u = rr > 0.01f ? clampf(d / rr, 0.0f, 1.0f) : 0.0f;
            V3 out = norm(add(mul(c->right, dx / (rr + 0.01f)),
                              mul(c->fwd, -sqrtf(clampf(1.0f - u * u, 0.0f, 1.0f)))));
            V3 n = norm(sub(out, mul(axis, dot(out, axis))));

            V3 sp = add(a, mul(sub(b, a), t));
            V3 vr = norm(sub(sp, c->eye));
            float sh = terrain_shadow(c, sp);
            V3 col = shade(c, albedo, n, vr, 0.62f, sh, mat, 0.0f);
            putz_cov(c, x, y, aerial(c, col, zz, vr), zz, cov);
        }
    }
}

/* A limb whose colour runs along it, and which can be faded out as a whole.
 * Grass wants both: dark at the root and lit at the tip, and gone entirely by
 * the distance where a blade is thinner than a pixel. */
static void bole2(Land *c, V3 a, V3 b, float ra, float rb, V3 ca, V3 cb,
                  int mat, float alpha)
{
    float ax, ay, az, bx, by, bz;
    if (!project(c, a, &ax, &ay, &az)) return;
    if (!project(c, b, &bx, &by, &bz)) return;
    float wa = ra / az * c->focal, wb = rb / bz * c->focal;
    if (wa < 0.10f && wb < 0.10f) return;

    float rmax = (wa > wb ? wa : wb) + 1.5f;
    int x0 = (int)floorf((ax < bx ? ax : bx) - rmax), x1 = (int)ceilf((ax > bx ? ax : bx) + rmax);
    int y0 = (int)floorf((ay < by ? ay : by) - rmax), y1 = (int)ceilf((ay > by ? ay : by) + rmax);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->W) x1 = c->W - 1;
    if (y1 >= c->H) y1 = c->H - 1;

    float vx = bx - ax, vy = by - ay, vv = vx * vx + vy * vy;
    V3 axis = norm(sub(b, a));
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f - ax, py = (float)y + 0.5f - ay;
            float t = vv > 1e-6f ? clampf((px * vx + py * vy) / vv, 0.0f, 1.0f) : 0.0f;
            float dx = px - t * vx, dy = py - t * vy;
            float d = sqrtf(dx * dx + dy * dy);
            float rr = mixf(wa, wb, t);
            float cov = clampf(rr + 0.5f - d, 0.0f, 1.0f) * alpha;
            if (cov <= 0.0f) continue;
            float zz = mixf(az, bz, t);
            size_t idx = (size_t)y * c->W + x;
            if (zz >= c->zb[idx]) continue;
            float u = rr > 0.01f ? clampf(d / rr, 0.0f, 1.0f) : 0.0f;
            V3 out = norm(add(mul(c->right, dx / (rr + 0.01f)),
                              mul(c->fwd, -sqrtf(clampf(1.0f - u * u, 0.0f, 1.0f)))));
            V3 nn = norm(sub(out, mul(axis, dot(out, axis))));
            V3 sp = add(a, mul(sub(b, a), t));
            V3 vr = norm(sub(sp, c->eye));
            V3 col = shade(c, clerp3(ca, cb, t), nn, vr, 0.55f + 0.45f * t,
                           terrain_shadow(c, sp), mat, t * 0.8f);
            putz_cov(c, x, y, aerial(c, col, zz, vr), zz, cov);
        }
    }
}

static void draw_scenery(Land *c, const Cp4World *w)
{
    (void)w;
    int x0 = (int)floorf((c->eye.x - SCEN_RANGE) / SCEN_CELL);
    int x1 = (int)floorf((c->eye.x + SCEN_RANGE) / SCEN_CELL);
    int z0 = (int)floorf((c->eye.z - SCEN_RANGE) / SCEN_CELL);
    int z1 = (int)floorf((c->eye.z + SCEN_RANGE) / SCEN_CELL);

    float wdx, wdz;
    wind_dir(c, &wdx, &wdz);

    for (int cz = z0; cz <= z1; cz++) {
        for (int cx = x0; cx <= x1; cx++) {
            Scen s;
            if (!scenery_at(c, cx, cz, &s)) continue;
            float ddx = s.x - c->eye.x, ddz = s.z - c->eye.z;
            if (ddx * ddx + ddz * ddz > SCEN_RANGE * SCEN_RANGE) continue;

            if (s.kind == SCEN_ROCK) {
                float rr = s.h;
                float tone = 0.13f + 0.11f * s.r2;
                V3 rock = v3(tone * 1.06f, tone * 1.00f, tone * 0.92f);
                if (s.biome == CP4_BIOME_DESERT) rock = v3(tone * 1.55f, tone * 1.20f, tone * 0.76f);
                if (s.biome == CP4_BIOME_ICE)    rock = v3(tone * 1.25f, tone * 1.38f, tone * 1.55f);
                canopy(c, v3(s.x, s.y - rr * 0.55f, s.z), rr, rock, 0.42f, 0.10f, MAT_ROCK);
                canopy(c, v3(s.x + rr * 0.9f, s.y - rr * 0.25f, s.z - rr * 0.5f),
                       rr * 0.45f, rock, 0.42f, 0.10f, MAT_ROCK);
                continue;
            }

            float h = s.h;
            float bark = 0.13f + 0.09f * s.r2;
            V3 trunk = v3(bark * 1.40f, bark * 1.00f, bark * 0.62f);
            float leafv = 0.24f + 0.26f * s.r2;
            float warm = rhash(c->seed ^ 0x4B93u, (int)s.x, (int)s.z);
            V3 leaf = s.kind == SCEN_CONIFER
                        ? v3(leafv * 0.30f, leafv * 0.70f, leafv * 0.48f)
                    : s.biome == CP4_BIOME_JUNGLE
                        ? v3(leafv * 0.34f, leafv * 1.08f, leafv * 0.32f)
                    : s.biome == CP4_BIOME_TAIGA
                        ? v3(leafv * 0.32f, leafv * 0.72f, leafv * 0.50f)
                        /* Autumn, and rare: cubed, it is a few trees turning
                         * early rather than a permanent October. */
                        : v3(leafv * (0.46f + 0.70f * warm * warm * warm),
                             leafv * (1.02f - 0.14f * warm),
                             leafv * (0.36f - 0.12f * warm));

            float sway = wind_gust(c, s.x, s.z)
                       * (0.55f + 0.45f * sinf(c->time * 1.7f + s.r1 * 6.28f));

            V3 tb = v3(s.x, s.y, s.z);
            V3 tt = v3(s.x + wdx * h * sway * 0.06f, s.y - h * 0.62f,
                       s.z + wdz * h * sway * 0.06f);
            bole(c, tb, tt, h * 0.055f, h * 0.030f, trunk, MAT_BARK);

            float lean = s.kind == SCEN_CONIFER ? 0.12f : 0.24f;
            for (int t = 0; t < 3; t++) {
                float a = s.r2 * 6.283f + (float)t * 2.094f;
                float fy = 0.42f + 0.20f * (float)t;
                bole(c, v3(s.x, s.y - h * fy, s.z),
                     v3(s.x + cosf(a) * h * lean + wdx * h * sway * 0.10f,
                        s.y - h * (fy + 0.28f),
                        s.z + sinf(a) * h * lean + wdz * h * sway * 0.10f),
                     h * 0.026f, h * 0.012f, trunk, MAT_BARK);
            }

            float ragged = s.kind == SCEN_CONIFER ? 0.55f : 0.80f;
            if (s.kind == SCEN_CONIFER) {
                for (int t = 0; t < 7; t++) {
                    float f = (float)t / 6.0f;
                    float k = h * sway * 0.16f * f;
                    canopy(c, v3(s.x + wdx * k, s.y - h * (0.40f + 0.56f * f), s.z + wdz * k),
                           h * (0.30f - 0.25f * f * f), leaf, ragged, 0.35f, MAT_FOLIAGE);
                }
            } else {
                float a0 = s.r2 * 6.283f;
                float ck = h * sway * 0.17f;
                canopy(c, v3(s.x + wdx * ck, s.y - h * 0.80f, s.z + wdz * ck),
                       h * 0.27f, leaf, ragged, 0.35f, MAT_FOLIAGE);
                for (int t = 0; t < 5; t++) {
                    float a = a0 + (float)t * 1.257f;
                    float rr = h * (0.12f + 0.10f * rhash(c->seed ^ (uint32_t)(0x91u + t),
                                                          (int)s.x, (int)s.z));
                    canopy(c, v3(s.x + cosf(a) * h * 0.25f + wdx * ck * 0.85f,
                                 s.y - h * (0.64f + 0.22f * (float)(t & 1)),
                                 s.z + sinf(a) * h * 0.25f + wdz * ck * 0.85f),
                           rr, leaf, ragged, 0.35f, MAT_FOLIAGE);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ *
 * ground cover
 *
 * The near field. A landscape shot with nothing in the first twenty units is
 * a diagram of a landscape - the eye has nothing to measure the distance
 * against. Grass in the foreground is the cheapest depth cue there is, and
 * since it is also the thing the wind is visible on, it does double duty.
 * ------------------------------------------------------------------ */

static void draw_cover(Land *c, const Cp4World *w)
{
    (void)w;
    const float R = 330.0f;
    float wdx, wdz;
    wind_dir(c, &wdx, &wdz);

    int cx0 = (int)floorf((c->eye.x - R) / 2.2f), cx1 = (int)floorf((c->eye.x + R) / 2.2f);
    int cz0 = (int)floorf((c->eye.z - R) / 2.2f), cz1 = (int)floorf((c->eye.z + R) / 2.2f);

    for (int cz = cz0; cz <= cz1; cz++) {
        for (int cx = cx0; cx <= cx1; cx++) {
            float r0 = rhash(c->seed ^ 0x2B71u, cx, cz);
            float bx = ((float)cx + rhash(c->seed ^ 0x18u, cx, cz)) * 2.2f;
            float bz = ((float)cz + rhash(c->seed ^ 0x39u, cx, cz)) * 2.2f;
            float ddx = bx - c->eye.x, ddz = bz - c->eye.z;
            float d2 = ddx * ddx + ddz * ddz;
            if (d2 > R * R) continue;

            float gy = tile_grnd(&c->tile, bx, bz);
            if (gy > CP4_SEA - 2.0f) continue;
            int biome = cp4_biome(c->seed, bx, bz);
            float fert = cp4_fertility(biome);
            /* Thinning with distance, so the near field is dense and the
             * middle distance does not turn into a fur coat. This is the only
             * density test - the earlier one it used to duplicate simply cut
             * the same blades twice. */
            float d = sqrtf(d2);
            if (r0 > fert * 1.05f * clampf(1.35f - d / R, 0.06f, 1.0f)) continue;

            /* A blade thinner than a pixel cannot be drawn, only aliased, so
             * past the distance where that happens the cover has to go. It
             * thins rather than dims: blades drop out on their own dice, and
             * the survivors stay at full strength until the very end. */
            float keep = sstep(R * 0.99f, R * 0.22f, d);
            if (rhash(c->seed ^ 0x9B4Du, cx, cz) > keep) continue;
            float fade = sstep(R * 1.00f, R * 0.80f, d);
            if (fade <= 0.02f) continue;

            float hgt = (4.5f + 7.0f * rhash(c->seed ^ 0x5Au, cx, cz)) * (0.4f + fert);
            float gust = wind_gust(c, bx, bz);
            float lean = rhash(c->seed ^ 0x6Cu, cx, cz) - 0.5f;
            float bend = gust * (0.6f + 0.4f * sinf(c->time * 2.4f + r0 * 6.28f));

            V3 base = v3(bx, gy, bz);
            /* Every blade leaning the same way is a hatch pattern, not a
             * field: the wind sets the average and the blade's own dice set
             * how far it disagrees. */
            V3 tip = v3(bx + wdx * hgt * bend * 0.55f - wdz * hgt * lean * 0.30f,
                        gy - hgt * (0.75f + 0.5f * rhash(c->seed ^ 0x7Fu, cx, cz)),
                        bz + wdz * hgt * bend * 0.55f + wdx * hgt * lean * 0.30f);
            /* Tips catch more light than bases, and that value split along the
             * blade is what makes a field read as grass rather than as moss.
             * Both ends are jittered per blade, or a lawn comes out as one
             * flat colour with a texture on it. */
            V3 gc = biome_colour(biome, 0.30f);
            float tone = 0.72f + 0.62f * rhash(c->seed ^ 0x4Eu, cx, cz);
            V3 bcol = mul(gc, 0.55f * tone);
            V3 tcol = mul(gc, 1.25f * tone);
            tcol.y *= 1.06f;
            bole2(c, base, tip, 0.20f, 0.045f, bcol, tcol, MAT_FOLIAGE, fade);
        }
    }
}

/* ------------------------------------------------------------------ *
 * flora, creatures, birds
 * ------------------------------------------------------------------ */

static void draw_flora(Land *c, const Cp4World *w)
{
    for (int i = 0; i < CP4_MAX_FLORA; i++) {
        const Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        V3 p = cv(f->p);
        V3 col;
        float rad = f->r;
        switch (f->type) {
        case CP4_FLORA_BUSH:    col = v3(0.16f, 0.42f, 0.13f); break;
        case CP4_FLORA_CARCASS: col = v3(0.42f, 0.16f, 0.13f); break;
        case CP4_FLORA_KELP:    col = v3(0.10f, 0.34f, 0.26f); break;
        default:                col = v3(0.46f, 0.36f, 0.14f); break;
        }
        if (f->type == CP4_FLORA_KELP) {
            for (int k = 0; k < 3; k++) {
                float a = (float)k * 2.09f + (float)i;
                V3 top = v3(p.x + cosf(a) * rad * 0.4f, p.y - rad * 2.4f,
                            p.z + sinf(a) * rad * 0.4f);
                bole(c, p, top, rad * 0.16f, rad * 0.06f, col, MAT_FOLIAGE);
            }
        } else {
            canopy(c, v3(p.x, p.y - rad * 0.6f, p.z), rad, col, 0.60f, 0.30f, MAT_FOLIAGE);
            if (f->type == CP4_FLORA_BUSH) {
                canopy(c, v3(p.x + rad * 0.5f, p.y - rad * 0.35f, p.z + rad * 0.3f),
                       rad * 0.6f, col, 0.60f, 0.30f, MAT_FOLIAGE);
                canopy(c, v3(p.x - rad * 0.45f, p.y - rad * 0.30f, p.z - rad * 0.35f),
                       rad * 0.55f, col, 0.60f, 0.30f, MAT_FOLIAGE);
            }
        }
    }
}

/* Ray-march one animal's distance field.
 *
 * The advantage of the body being an SDF rather than a mesh shows up in the
 * transmission term: thickness is what the field is *for*, so a thin ear or a
 * fin can be lit from behind with the real number rather than a baked
 * approximation of it. Almost no engine can afford that; here it is one extra
 * evaluation along the light direction. */
static void march_creature(Land *c, const Prim *pr, int n, V3 centre, float bound,
                           const Skin *sk)
{
    float sx, sy, vz;
    if (!project(c, centre, &sx, &sy, &vz)) return;
    float rpx = bound / vz * c->focal;

    /* Quality by apparent size, not by world distance. A soft shadow traced
     * through the body costs twenty field evaluations and is worth every one
     * of them on the animal filling the frame; on one twenty pixels across it
     * is twenty evaluations spent on a gradient nobody can see. Same for the
     * march itself - a small beast needs far fewer steps to resolve to within
     * a pixel. */
    int   hi    = c->detail >= 1 && rpx > 46.0f;
    int   shad  = c->detail >= 2 && rpx > 130.0f;   /* only what fills the frame */
    int   steps = rpx > 90.0f ? 56 : (rpx > 30.0f ? 38 : 22);
    /* The hit epsilon is set against a pixel, not against the animal. Chasing
     * the surface to a thirtieth of a unit on a body forty units across is
     * chasing it to a fifth of a pixel, and the marcher pays for every one of
     * those iterations at grazing angles, where they are most numerous. */
    float eps   = bound / (rpx > 1.0f ? rpx : 1.0f) * 0.55f;
    if (eps < 0.03f) eps = 0.03f;
    /* Loop the primitives' own screen extent, not the bounding sphere's.
     *
     * prim_bounds measures from the body's centre to the tip of whatever
     * sticks out furthest - a tail, a wing - so the sphere is generous by
     * construction, and for the player it covers about half the frame while
     * the animal covers a fraction of it. Every pixel in the difference pays
     * a ray-sphere test and several marching steps to discover it was never
     * going to hit anything. Projecting the parts costs one loop over a few
     * dozen of them and takes the area back. */
    float bx0 = 1e9f, bx1 = -1e9f, by0 = 1e9f, by1 = -1e9f;
    for (int i = 0; i < n; i++) {
        for (int e = 0; e < 2; e++) {
            V3 pe = e ? pr[i].b : pr[i].a;
            float rr = (e ? pr[i].rb : pr[i].ra) + pr[i].k;
            float ex, ey, ez;
            if (!project(c, pe, &ex, &ey, &ez)) { bx0 = 0; by0 = 0;
                                                  bx1 = (float)c->W; by1 = (float)c->H;
                                                  i = n; break; }
            float rp = rr / ez * c->focal + 1.0f;
            if (ex - rp < bx0) bx0 = ex - rp;
            if (ex + rp > bx1) bx1 = ex + rp;
            if (ey - rp < by0) by0 = ey - rp;
            if (ey + rp > by1) by1 = ey + rp;
        }
    }
    int x0 = (int)floorf(bx0), x1 = (int)ceilf(bx1);
    int y0 = (int)floorf(by0), y1 = (int)ceilf(by1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->W) x1 = c->W - 1;
    if (y1 >= c->H) y1 = c->H - 1;
    if (x1 < x0 || y1 < y0) return;

    V3 tosun = mul(c->sun, -1.0f);

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = ((float)x + 0.5f - c->W * 0.5f) / c->focal;
            float py = ((float)y + 0.5f - c->H * 0.5f) / c->focal;
            V3 ray = norm(add(add(mul(c->right, px), mul(c->up, -py)), c->fwd));

            /* enter at the bounding sphere rather than at the camera */
            V3 oc = sub(c->eye, centre);
            float b = dot(oc, ray);
            float cc = dot(oc, oc) - bound * bound;
            float disc = b * b - cc;
            if (disc < 0.0f) continue;
            float sq = sqrtf(disc);
            float t = -b - sq;
            float tmax = -b + sq;
            if (tmax < 0.5f) continue;
            if (t < 0.5f) t = 0.5f;

            size_t idx = (size_t)y * c->W + x;
            if (t >= c->zb[idx]) continue;

            /* March for distance only.
             *
             * creature_sdf blends an albedo alongside the distance, and that
             * blend costs an expf per primitive - so asking for it on every
             * step of every ray is fifty transcendentals per sample for a
             * value thrown away everywhere except the one point the ray
             * actually stops at. Marching with NULL and paying for colour
             * once, at the hit, is the same picture for a fraction of the
             * frame.
             *
             * The unreachable test is the other half. If the field says the
             * surface is further off than the ray has left inside the
             * bounding sphere, it cannot be reached - so a ray that grazes
             * the animal and misses stops at once instead of grinding out its
             * whole step budget, and those rays are most of the disc. */
            int got = 0;
            for (int i = 0; i < steps && t < tmax; i++) {
                V3 q = add(c->eye, mul(ray, t));
                float d = creature_sdf(pr, n, q, NULL, NULL, NULL);
                if (d < eps) { got = 1; break; }
                if (d > tmax - t) break;
                t += d * 0.94f;
            }
            if (!got || t >= c->zb[idx]) continue;

            V3 q = add(c->eye, mul(ray, t));
            V3 col3 = v3(0.6f, 0.6f, 0.6f);
            float em = 0.0f, bw = 0.0f;
            creature_sdf(pr, n, q, &col3, &em, &bw);
            V3 nrm = sdf_normal(pr, n, q);
            float ao = hi ? sdf_ao(pr, n, q, nrm) : 0.78f;
            float shadow = terrain_shadow(c, q);
            if (shad) shadow *= sdf_shadow(pr, n, q, tosun);

            V3 albedo = apply_pattern(sk, q, col3, bw);

            /* True thickness, straight out of the field: step to the far side
             * along the light and ask how much body was crossed. */
            float thick = 0.0f;
            if (hi) {
                V3 p2 = add(q, mul(tosun, 1.4f));
                float d2 = creature_sdf(pr, n, p2, NULL, NULL, NULL);
                thick = clampf(1.0f - (d2 + 1.4f) / 3.0f, 0.0f, 1.0f);
            }

            V3 col = shade(c, albedo, nrm, ray, ao, shadow, MAT_HIDE, thick);
            if (em > 0.0f) col = add(col, mul(albedo, em * 1.6f));
            putz(c, x, y, aerial(c, col, t, ray), t);
        }
    }
}

static void draw_creature(Land *c, const Cp4Beast *b, int is_player)
{
    static Prim pr[MAX_PRIM];
    V3 centre;
    float bound;
    Skin sk;
    int n = build_prims4(b, is_player, pr, &centre, &bound, &sk);
    if (n <= 0) return;

    float d = sqrtf(dot(sub(centre, c->eye), sub(centre, c->eye)));
    if (bound / (d > 1.0f ? d : 1.0f) * c->focal < 8.0f) {
        /* Too small to be worth a distance field. One lobe carries the
         * silhouette, and at this size aerial perspective has most of it
         * anyway. */
        float base[3], mark[3], det[3];
        cp4_genome_colour(&b->g, base, mark, det);
        canopy(c, centre, bound * 0.62f, v3(base[0], base[1], base[2]), 0.25f, 0.20f, MAT_HIDE);
        return;
    }
    march_creature(c, pr, n, centre, bound, &sk);
}

static void draw_birds(Land *c, const Cp4World *w)
{
    for (int i = 0; i < 26; i++) {
        float ph = (float)i * 2.7f;
        float r = 260.0f + 190.0f * rhash(w->seed ^ 0x77u, i, 3);
        float a = c->time * (0.05f + 0.03f * rhash(w->seed ^ 0x21u, i, 9)) + ph;
        float bx = c->eye.x + cosf(a) * r;
        float bz = c->eye.z + sinf(a) * r;
        float gy = tile_grnd(&c->tile, bx, bz);
        float by = gy - 90.0f - 70.0f * rhash(w->seed ^ 0x5Cu, i, 1)
                 + 12.0f * sinf(c->time * 0.6f + ph);
        float sx, sy, vz;
        if (!project(c, v3(bx, by, bz), &sx, &sy, &vz)) continue;
        float flap = sinf(c->time * 8.0f + ph);
        float sp = 9.0f / vz * c->focal;
        if (sp < 0.7f) continue;
        V3 col = mul(v3(0.10f, 0.11f, 0.14f), 1.0f);
        col = aerial(c, col, vz, norm(sub(v3(bx, by, bz), c->eye)));
        for (int k = -1; k <= 1; k += 2) {
            float ex = sx + (float)k * sp, ey = sy - flap * sp * 0.55f;
            int steps = (int)(sp * 2.0f) + 2;
            for (int t = 0; t <= steps; t++) {
                float u = (float)t / steps;
                putz_cov(c, (int)(mixf(sx, ex, u)), (int)(mixf(sy, ey, u)), col, vz, 0.85f);
            }
        }
    }
}

static void draw_landmarks(Land *c, const Cp4World *w)
{
    /* Something to walk toward. A landscape with no landmark has no scale and
     * no reason to cross it; these are hashed on a coarse lattice so they are
     * rare, fixed, and visible from a long way off. */
    const float CELL = 900.0f;
    int x0 = (int)floorf((c->eye.x - 2100.0f) / CELL), x1 = (int)floorf((c->eye.x + 2100.0f) / CELL);
    int z0 = (int)floorf((c->eye.z - 2100.0f) / CELL), z1 = (int)floorf((c->eye.z + 2100.0f) / CELL);
    for (int cz = z0; cz <= z1; cz++) {
        for (int cx = x0; cx <= x1; cx++) {
            if (rhash(c->seed ^ 0x4D2Bu, cx, cz) > 0.20f) continue;
            float lx = ((float)cx + 0.5f) * CELL, lz = ((float)cz + 0.5f) * CELL;
            float gy = tile_grnd(&c->tile, lx, lz);
            if (gy > CP4_SEA - 20.0f) continue;
            /* A landmark is a thing on the horizon. Standing next to one, it
             * is a pillar through the middle of the frame. */
            float ddx = lx - c->eye.x, ddz = lz - c->eye.z;
            if (ddx * ddx + ddz * ddz < 520.0f * 520.0f) continue;
            float h = 90.0f + 120.0f * rhash(c->seed ^ 0x1188u, cx, cz);
            V3 col = v3(0.34f, 0.31f, 0.30f);
            int tiers = 5;
            for (int t = 0; t < tiers; t++) {
                float f = (float)t / (float)(tiers - 1);
                bole(c, v3(lx, gy - h * f, lz), v3(lx, gy - h * (f + 1.0f / tiers), lz),
                     (10.0f - 6.0f * f) * (1.0f + 0.4f * rhash(c->seed, cx + t, cz)),
                     (9.0f - 6.0f * f), col, MAT_ROCK);
            }
        }
    }
    (void)w;
}

static void draw_nests(Land *c, const Cp4World *w)
{
    for (int i = 0; i < CP4_MAX_NESTS; i++) {
        const Cp4Nest *nst = &w->nest[i];
        if (!nst->alive) continue;
        V3 p = cv(nst->p);
        p.y = tile_grnd(&c->tile, p.x, p.z);
        /* Colour says how the species feels about the player, which is live
         * state and not decoration. */
        V3 col = nst->standing > 0.25f ? v3(0.30f, 0.44f, 0.20f)
               : nst->standing < -0.25f ? v3(0.44f, 0.16f, 0.14f)
                                        : v3(0.36f, 0.32f, 0.26f);
        for (int k = 0; k < 8; k++) {
            float a = (float)k * 0.785f;
            canopy(c, v3(p.x + cosf(a) * 11.0f, p.y - 1.6f, p.z + sinf(a) * 11.0f),
                   3.0f, col, 0.40f, 0.20f, MAT_BARK);
        }
    }
}

static void draw_home(Land *c, const Cp4World *w)
{
    if (!w->home.alive) return;
    V3 p = cv(w->home.p);
    p.y = tile_grnd(&c->tile, p.x, p.z);
    for (int k = 0; k < 11; k++) {
        float a = (float)k * 0.571f;
        canopy(c, v3(p.x + cosf(a) * 14.0f, p.y - 2.2f, p.z + sinf(a) * 14.0f),
               3.6f, v3(0.40f, 0.33f, 0.22f), 0.40f, 0.20f, MAT_BARK);
    }
    /* the eggs are the thing worth seeing */
    for (int k = 0; k < w->home.eggs && k < 6; k++) {
        float a = (float)k * 1.05f;
        canopy(c, v3(p.x + cosf(a) * 5.0f, p.y - 3.2f, p.z + sinf(a) * 5.0f),
               2.4f, v3(0.86f, 0.82f, 0.70f), 0.10f, 0.45f, MAT_SNOW);
    }
}

/* ------------------------------------------------------------------ *
 * focus
 *
 * Depth of field, from the z buffer that every draw above has been filling.
 * A landscape wants very little of it - enough to seat the foreground and
 * take the edge off the far ridge, not enough to look like a photograph of a
 * model. Done as a blurred copy of the frame lerped in by circle of
 * confusion, which is a fraction of the cost of a gather and, for a scene
 * with this little depth complexity, hard to tell from one.
 * ------------------------------------------------------------------ */

static void focus_pass(Land *c)
{
    int W = c->W, H = c->H;
    size_t n = (size_t)W * H;
    float *blur = (float *)malloc(sizeof(float) * n * 3);
    if (!blur) return;
    box_blur(c->h.px, blur, W, H, 2);

    float zf = c->fdist;
    for (size_t i = 0; i < n; i++) {
        float z = c->zb[i];
        float coc;
        if (z >= Z_SKY) coc = 0.55f;                       /* the sky is at infinity */
        else if (z < zf) coc = clampf((zf - z) / (zf * 0.80f), 0.0f, 1.0f) * 0.55f;
        else             coc = clampf((z - zf) / 1900.0f, 0.0f, 1.0f) * 0.45f;
        if (coc > 1.0f) coc = 1.0f;
        if (coc <= 0.002f) continue;
        float *a = c->h.px + 3 * i;
        const float *b = blur + 3 * i;
        a[0] = mixf(a[0], b[0], coc);
        a[1] = mixf(a[1], b[1], coc);
        a[2] = mixf(a[2], b[2], coc);
    }
    free(blur);
}

/* ------------------------------------------------------------------ *
 * HUD
 *
 * Hairlines and dot matrix, nothing filled. Two solid panels in the corners
 * of a landscape are two holes punched in the thing the renderer exists to
 * show, so the readouts are pushed to the very edge and the centre of frame
 * is kept clear.
 * ------------------------------------------------------------------ */

static const C3 UI     = { 0.88f, 0.95f, 0.78f };
static const C3 UI_DIM = { 0.46f, 0.55f, 0.42f };

static void hud_frame(Hdr *p)
{
    float m = 16.0f * p->ui, l = 30.0f * p->ui;
    float W = (float)p->W, H = (float)p->H;
    struct { float x, y, dx, dy; } k[4] = {
        { m, m, 1, 1 }, { W - m, m, -1, 1 }, { m, H - m, 1, -1 }, { W - m, H - m, -1, -1 }
    };
    for (int i = 0; i < 4; i++) {
        hud_rule(p, k[i].dx > 0 ? k[i].x : k[i].x - l, k[i].y, l, p->ui, UI_DIM, 0.30f);
        hud_rule(p, k[i].x - (k[i].dx > 0 ? 0.0f : p->ui),
                 k[i].dy > 0 ? k[i].y : k[i].y - l, p->ui, l, UI_DIM, 0.30f);
    }
}

static void hud_bar(Hdr *p, float x, float y, float w, float h, float f, C3 col)
{
    hud_rule(p, x, y, w, h, UI_DIM, 0.16f);
    if (f > 0.0f) hud_rule(p, x, y, w * clampf(f, 0.0f, 1.0f), h, col, 1.05f);
}

static void draw_hud(Land *c, const Cp4World *w)
{
    Hdr *p = &c->h;
    char buf[96];
    float u = p->ui, M = 26.0f * u;
    const Cp4Beast *pl = &w->player;

    hud_frame(p);

    /* top left: the run */
    d_text(p, M, M, 3.0f * u, "CREATURE STAGE", UI, 0.85f);
    hud_rule(p, M, M + 26.0f * u, 160.0f * u, u, UI_DIM, 0.45f);
    snprintf(buf, sizeof(buf), "GEN %d/%d   T%d", w->generation + 1, CP4_GENERATIONS, w->step);
    d_text(p, M, M + 34.0f * u, 2.0f * u, buf, UI_DIM, 0.95f);

    hud_bar(p, M, M + 52.0f * u, 150.0f * u, 3.0f * u,
            pl->hp / (pl->hp_max > 0.0f ? pl->hp_max : 1.0f), c3(0.92f, 0.28f, 0.22f));
    hud_bar(p, M, M + 59.0f * u, 150.0f * u, 3.0f * u,
            w->dna / CP4_DNA_GOAL, c3(0.44f, 0.94f, 0.42f));
    hud_bar(p, M, M + 66.0f * u, 150.0f * u, 3.0f * u,
            pl->stam / (pl->s.stamina > 0.0f ? pl->s.stamina : 1.0f),
            c3(0.94f, 0.82f, 0.34f));

    /* top right: where and when */
    {
        static const char *MED[CP4_MEDIUM_COUNT] = { "GROUND", "WATER", "AIR", "SOIL" };
        snprintf(buf, sizeof(buf), "%s  %s", MED[pl->medium % CP4_MEDIUM_COUNT],
                 cp4_biome_name(cp4_biome(w->seed, pl->p.x, pl->p.z)));
        float tw = d_textw(buf, 2.0f * u);
        d_text(p, (float)p->W - M - tw, M, 2.0f * u, buf, UI, 0.85f);
        /* a day dial: one tick for the sun, so the hour is readable at a
         * glance without a number */
        float dx = (float)p->W - M - 9.0f * u, dy = M + 26.0f * u;
        d_arc(p, dx, dy, 8.0f * u, u, 0.0f, 2.0f * TPI, UI_DIM, 0.40f);
        float a = cp4_sun_angle(w->step) - TPI * 0.5f;
        d_disc(p, dx + cosf(a) * 8.0f * u, dy + sinf(a) * 8.0f * u, 2.0f * u,
               c->day > 0.4f ? c3(1.0f, 0.90f, 0.55f) : c3(0.62f, 0.70f, 1.0f), 1.6f, 0.0f);
    }

    /* bottom left: the body */
    {
        float y = (float)p->H - M - 20.0f * u;
        snprintf(buf, sizeof(buf), "%d DNA   %d PARTS   %d SEG",
                 (int)w->player.s.cost, w->player.s.n_parts, w->player.g.nseg);
        d_text(p, M, y, 2.0f * u, buf, UI, 0.80f);
        hud_rule(p, M, y + 19.0f * u, 190.0f * u, u, UI_DIM, 0.45f);
    }

    /* bottom right: what the episode has done */
    {
        snprintf(buf, sizeof(buf), "ATE %d   KILL %d   FRIEND %d",
                 w->ate_plant + w->ate_meat, w->kills, w->befriended);
        float tw = d_textw(buf, 2.0f * u);
        d_text(p, (float)p->W - M - tw, (float)p->H - M - 10.0f * u, 2.0f * u,
               buf, UI_DIM, 0.95f);
    }

    if (w->status != CP4_RUN) {
        const char *msg = w->status == CP4_EVOLVED ? "EVOLVE - CIVILISATION STAGE"
                        : w->status == CP4_DEAD    ? "EATEN"
                                                   : "TIME UP";
        C3 col = w->status == CP4_EVOLVED ? c3(0.55f, 1.00f, 0.60f)
               : w->status == CP4_DEAD    ? c3(1.00f, 0.34f, 0.28f)
                                          : c3(0.95f, 0.88f, 0.50f);
        float sc = 5.0f * u;
        float tw = d_textw(msg, sc);
        float x = ((float)p->W - tw) * 0.5f, y = (float)p->H * 0.5f - 3.5f * sc;
        for (int j = 0; j < p->H; j++) {
            float fy = fabsf((float)j - (float)p->H * 0.5f) / ((float)p->H * 0.5f);
            float k = mixf(0.32f, 1.0f, sstep(0.10f, 0.66f, fy));
            for (int i = 0; i < p->W; i++) hdr_mul(p, i, j, k);
        }
        hud_rule(p, x - 20.0f * u, y - 16.0f * u, tw + 40.0f * u, u, col, 0.90f);
        hud_rule(p, x - 20.0f * u, y + 7.0f * sc + 15.0f * u, tw + 40.0f * u, u, col, 0.90f);
        d_text(p, x, y, sc, msg, col, 1.60f);
    }
}

/* ------------------------------------------------------------------ *
 * entry
 * ------------------------------------------------------------------ */

/* Exposure. Same convention as the darkfield renderer: everything above is
 * scene-referred, and where that set lands on the tonemapping curve is one
 * number decided here. */
#define VISTA_EXPOSURE 1.24f

void cp4_render_vista(const Cp4World *w, uint8_t *rgba, int W, int H)
{
    if (!w || !rgba || W < 16 || H < 16) return;

    Land c;
    memset(&c, 0, sizeof(c));
    c.W = W; c.H = H;
    c.h.px = (float *)malloc(sizeof(float) * (size_t)W * H * 3);
    c.zb   = (float *)malloc(sizeof(float) * (size_t)W * H);
    if (!c.h.px || !c.zb) { free(c.h.px); free(c.zb); return; }
    c.h.W = W; c.h.H = H;
    c.h.expo = VISTA_EXPOSURE;
    c.h.ui = (float)W / 1280.0f;
    if (c.h.ui < 0.5f) c.h.ui = 0.5f;
    c.h.step = w->step;
    for (size_t i = 0; i < (size_t)W * H; i++) c.zb[i] = Z_CLEAR;

    c.focal = (float)W * 0.62f;          /* wide: this stage is about the view */
    c.detail = 2;
    c.seed = w->seed;
    c.time = (float)w->step * CP4_DT;
    c.sun = sun_dir(w->step);
    c.sunc = sun_colour(c.sun);
    c.day = w->daylight;
    {
        /* Mist collects when the ground is cold and the sun is low, which is
         * to say at dawn and at dusk. Tying it to the daylight curve rather
         * than to a constant is what makes those two crossings of the day
         * look like anything. */
        float edge = clampf(1.0f - fabsf(c.day - 0.34f) / 0.34f, 0.0f, 1.0f);
        c.mist = 0.15f + 0.85f * edge;
    }

    const Cp4Beast *p = &w->player;
    V3 pf, pr_, pu;
    basis3(p->yaw, 0.0f, &pf, &pr_, &pu);

    float back = p->s.length * 2.7f + p->s.stand * 3.4f + 62.0f;
    float lift = p->s.stand * 1.7f + 27.0f;

    if (p->medium == CP4_UNDER) {
        back *= 0.55f;
        c.eye = add(add(cv(p->p), mul(pf, -back)), mul(pr_, back * 0.42f));
        c.eye.y = p->p.y - p->s.stand * 0.55f;
        float g = cp4_height(w->seed, c.eye.x, c.eye.z);
        if (c.eye.y < g + 4.0f) c.eye.y = g + 4.0f;
    } else {
        /* a touch off the centre line: dead astern shows the player its own
         * backside and hides every part mounted on the flanks */
        c.eye = add(add(add(cv(p->p), mul(pf, -back)), mul(pu, lift)),
                    mul(pr_, back * 0.20f));
        float g = cp4_height(w->seed, c.eye.x, c.eye.z) - 14.0f;
        if (c.eye.y > g) c.eye.y = g;
        if (c.eye.y < -CP4_SKY) c.eye.y = -CP4_SKY;
    }

    {
        float ge = cp4_height(w->seed, c.eye.x, c.eye.z);
        c.buried    = c.eye.y > ge;
        c.submerged = !c.buried && ge > CP4_SEA && c.eye.y > CP4_SEA;
    }

    /* The aim point sits a long way in front of the animal on purpose. Aiming
     * at it instead puts the whole downward tilt into the shot and shoves the
     * horizon off the top of the frame, so the picture becomes a study of
     * dirt. Looking well ahead flattens the pitch to a few degrees and lets
     * the land have a skyline. */
    V3 look = norm(sub(add(cv(p->p), mul(pf, p->medium == CP4_UNDER ? 40.0f : 170.0f)), c.eye));
    c.fwd = look;
    c.right = norm(v3(-look.z, 0.0f, look.x));
    c.up = norm(v3(c.right.z * look.y - c.right.y * look.z,
                   c.right.x * look.z - c.right.z * look.x,
                   c.right.y * look.x - c.right.x * look.y));
    c.fdist = sqrtf(dot(sub(cv(p->p), c.eye), sub(cv(p->p), c.eye)));

    if (!tile_build(&c.tile, w->seed, c.eye.x, c.eye.z)) {
        free(c.h.px); free(c.zb); return;
    }

    draw_world(&c, w);
    if (!c.buried) {
        draw_landmarks(&c, w);
        draw_scenery(&c, w);
        draw_cover(&c, w);
        draw_birds(&c, w);
    }
    draw_nests(&c, w);
    draw_home(&c, w);
    draw_flora(&c, w);
    for (int i = 0; i < CP4_MAX_BEASTS; i++)
        if (w->beast[i].alive) draw_creature(&c, &w->beast[i], 0);
    draw_creature(&c, p, 1);

    focus_pass(&c);
    bloom(&c.h, 1.10f, 0.30f);
    draw_hud(&c, w);
    resolve(&c.h, rgba);

    tile_free(&c.tile);
    free(c.h.px);
    free(c.zb);
}

/* ------------------------------------------------------------------ *
 * the studio
 *
 * The creature editor's viewport. Same shading model as the landscape - the
 * bodies are the thing both are for - with the world taken away and replaced
 * by a backdrop, a floor and a fixed key light.
 *
 * Two things here exist for the editor rather than for the picture:
 *
 * `quality` selects an internal resolution and a marcher budget together, so
 * a caller can draw a coarse frame while the mouse is moving and a settled
 * one when it stops. That is the whole trick behind every distance-field
 * editor: at a quarter resolution this is fast enough to drag against, and
 * the sharp image lands a fraction of a second after you let go.
 *
 * `cp4_studio_pick` answers "which part is under this pixel", which is the
 * one thing direct manipulation cannot be built without. It is the same march
 * the renderer does, stopped at the first hit and asked for an identity
 * instead of a colour - so picking can never disagree with what is on screen,
 * which is the usual failure of a separate picking representation.
 * ------------------------------------------------------------------ */

struct Cp4Studio {
    Land land;
    int  W, H;          /* output size                                  */
    int  cap;           /* internal pixels allocated (at the finest SS) */
};

/* Internal resolution as a multiple of the output, per quality step. The top
 * one supersamples: at 2x linear the marcher runs four rays per output pixel,
 * which is what removes the stair-stepping from a silhouette that has no
 * pixel grid to hide behind any more. */
static const float STUDIO_SCALE[4] = { 0.25f, 0.5f, 1.0f, 2.0f };

Cp4Studio *cp4_studio_new(int w, int h)
{
    if (w < 16 || h < 16) return NULL;
    Cp4Studio *s = (Cp4Studio *)calloc(1, sizeof(Cp4Studio));
    if (!s) return NULL;
    s->W = w; s->H = h;
    s->cap = (w * 2) * (h * 2);
    s->land.h.px = (float *)malloc(sizeof(float) * (size_t)s->cap * 3);
    s->land.zb   = (float *)malloc(sizeof(float) * (size_t)s->cap);
    if (!s->land.h.px || !s->land.zb) { cp4_studio_free(s); return NULL; }
    return s;
}

void cp4_studio_free(Cp4Studio *s)
{
    if (!s) return;
    free(s->land.h.px);
    free(s->land.zb);
    free(s);
}

void cp4_studio_size(const Cp4Studio *s, int32_t *w, int32_t *h)
{
    if (!s) return;
    if (w) *w = s->W;
    if (h) *h = s->H;
}

/* Build the animal, and stand it on the floor. A genome is not a beast, so
 * the parts of Cp4Beast the rig actually reads - stats, stance, phase - are
 * filled in and the rest left at zero. */
static int studio_build(const Cp4Genome *g, float phase, Prim *pr,
                        V3 *centre, float *bound, Skin *sk)
{
    Cp4Beast b;
    memset(&b, 0, sizeof(b));
    b.g = *g;
    cp4_genome_stats(&b.g, &b.s);
    b.hp = b.hp_max = b.s.hp_max;
    b.alive = 1;
    b.p.x = 0.0f; b.p.y = -b.s.stand; b.p.z = 0.0f;
    b.yaw = 0.0f;
    b.phase = phase;
    return build_prims4(&b, 0, pr, centre, bound, sk);
}

/* Camera, from the turntable. Kept in one place because the picker has to
 * agree with the renderer exactly, and the cheapest way to guarantee that is
 * for there to be one function that decides.
 *
 * The framing fits the body rather than its bounding sphere. prim_bounds
 * measures from the body's centre to whatever sticks out furthest, so a long
 * tail sets the radius and every animal that has one is then framed as though
 * it were a ball of that size - which is why the gallery had a small creature
 * marooned in the middle of every tile. Taking the real extent perpendicular
 * to the view, and aiming at the middle of what is actually there, fills the
 * frame with the animal whatever shape it turned out to be. */
static void studio_camera(Land *c, const Cp4View *v, const Prim *pr, int n,
                          V3 centre, float bound, int W, int H)
{
    float zoom = v->zoom > 0.05f ? v->zoom : 1.0f;
    V3 dir = norm(v3(cosf(v->azimuth) * cosf(v->elev), -sinf(v->elev),
                     sinf(v->azimuth) * cosf(v->elev)));
    c->focal = (float)W * 0.98f;

    /* the middle of the body, not the middle of its bounding sphere */
    V3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for (int i = 0; i < n; i++) {
        for (int e = 0; e < 2; e++) {
            V3 p = e ? pr[i].b : pr[i].a;
            float r = e ? pr[i].rb : pr[i].ra;
            if (p.x - r < lo.x) lo.x = p.x - r;
            if (p.y - r < lo.y) lo.y = p.y - r;
            if (p.z - r < lo.z) lo.z = p.z - r;
            if (p.x + r > hi.x) hi.x = p.x + r;
            if (p.y + r > hi.y) hi.y = p.y + r;
            if (p.z + r > hi.z) hi.z = p.z + r;
        }
    }
    V3 aim = n > 0 ? mul(add(lo, hi), 0.5f) : centre;

    /* Extent across the view and depth along it, then the standard fit. The
     * margin leaves the animal short of the edge, because a creature touching
     * the frame reads as a badly built one rather than a badly framed shot. */
    float ext = 1.0f, depth = 0.0f;
    for (int i = 0; i < n; i++) {
        for (int e = 0; e < 2; e++) {
            V3 rel = sub(e ? pr[i].b : pr[i].a, aim);
            float r = (e ? pr[i].rb : pr[i].ra) + pr[i].k * 0.5f;
            float along = dot(rel, dir);
            V3 perp = sub(rel, mul(dir, along));
            float pl = sqrtf(dot(perp, perp)) + r;
            if (pl > ext) ext = pl;
            float dp = fabsf(along) + r;
            if (dp > depth) depth = dp;
        }
    }
    float halftan = 0.5f * (float)(W < H ? W : H) / c->focal;
    float dist = ext / (halftan * 0.84f) + depth;
    if (dist < bound * 0.9f) dist = bound * 0.9f;

    c->eye = add(aim, mul(dir, dist / zoom));
    V3 look = norm(sub(aim, c->eye));
    c->fwd = look;
    c->right = norm(v3(-look.z, 0.0f, look.x));
    c->up = norm(v3(c->right.z * look.y - c->right.y * look.z,
                    c->right.x * look.z - c->right.z * look.x,
                    c->right.y * look.x - c->right.x * look.y));
    c->W = W; c->H = H;
}

static void studio_light(Land *c)
{
    /* A portrait is always the same hour, because the point of it is to
     * compare one body against another rather than one afternoon against
     * another. Three-quarter key, slightly above. */
    c->sun = norm(v3(0.42f, 0.76f, -0.30f));
    c->sunc = sun_colour(c->sun);
    c->day = 1.0f;
    c->mist = 0.0f;
    c->studio = 1;
    c->submerged = c->buried = 0;
    c->time = 0.0f;
    c->seed = 0x5C0AEu;
}

/* Backdrop and floor.
 *
 * A cyclorama rather than a gradient: dark, cool, falling off toward the
 * corners so the eye is pushed to the middle, with a soft pool of light
 * behind the subject. The floor is a disc that fades out rather than a plane
 * that runs to a horizon - a horizon line behind a creature reads as a
 * landscape, and this is deliberately not one.
 */
static void studio_ground(Land *c, const Prim *pr, int n, float bound,
                          int quality)
{
    V3 tosun = mul(c->sun, -1.0f);
    float R = bound * 2.6f;
    for (int y = 0; y < c->H; y++) {
        for (int x = 0; x < c->W; x++) {
            float px = ((float)x + 0.5f - c->W * 0.5f) / c->focal;
            float py = ((float)y + 0.5f - c->H * 0.5f) / c->focal;
            V3 ray = norm(add(add(mul(c->right, px), mul(c->up, -py)), c->fwd));

            /* backdrop */
            float u = ((float)x + 0.5f) / c->W - 0.5f;
            float w = ((float)y + 0.5f) / c->H - 0.5f;
            float rad = sqrtf(u * u + w * w);
            float pool = expf(-rad * rad * 5.0f);
            V3 bg = add(v3(0.020f, 0.026f, 0.040f), mul(v3(0.10f, 0.12f, 0.17f), pool));
            hdr_set(&c->h, x, y, v2c(bg));
            c->zb[(size_t)y * c->W + x] = Z_CLEAR;

            /* floor at y = 0, which is exactly where the legs reach */
            if (ray.y <= 0.001f) continue;
            float t = (0.0f - c->eye.y) / ray.y;
            if (t <= 0.0f) continue;
            V3 h = add(c->eye, mul(ray, t));
            float r = sqrtf(h.x * h.x + h.z * h.z);
            float fade = sstep(R, R * 0.35f, r);
            if (fade <= 0.001f) continue;

            float grain = 0.90f + 0.20f * rnoise(0x77u, h.x * 0.32f, h.z * 0.32f);
            V3 alb = mul(v3(0.105f, 0.112f, 0.108f), grain);

            /* The contact shadow is the whole reason there is a floor. An
             * animal without one floats, and floating hides exactly the thing
             * the leg rig exists to show. Traced against the real body, so it
             * is the shadow of this creature and not a blob. */
            /* The contact shadow is the whole reason there is a floor: an
             * animal without one floats, and floating hides exactly what the
             * leg rig exists to show. It is also, unguarded, about half the
             * frame - so it is traced only where a body could plausibly be
             * over, only at the settled qualities, and with a tenth of the
             * reach a general soft shadow needs. */
            float sh = 1.0f;
            if (quality >= 2 && r < bound * 1.45f) {
                float res = 1.0f, tt = 0.6f;
                for (int k = 0; k < 12 && tt < bound * 1.6f; k++) {
                    float dd = creature_sdf(pr, n, add(h, mul(tosun, tt)),
                                            NULL, NULL, NULL);
                    if (dd < 0.04f) { res = 0.0f; break; }
                    float kk = 6.0f * dd / tt;
                    if (kk < res) res = kk;
                    tt += clampf(dd, 0.5f, 3.5f);
                }
                sh = sstep(0.0f, 1.0f, clampf(res, 0.0f, 1.0f));
            }

            V3 col = shade(c, alb, v3(0.0f, -1.0f, 0.0f), ray,
                           0.55f + 0.45f * sstep(bound * 2.4f, bound * 0.6f, r),
                           sh, MAT_GROUND, 0.0f);
            V3 out = clerp3(bg, col, fade);
            hdr_set(&c->h, x, y, v2c(out));
            c->zb[(size_t)y * c->W + x] = t * dot(ray, c->fwd);
        }
    }
}

void cp4_studio_render(Cp4Studio *s, const Cp4Genome *g, const Cp4View *v,
                       uint8_t *rgba)
{
    if (!s || !g || !v || !rgba) return;
    int q = v->quality < 0 ? 0 : (v->quality > 3 ? 3 : v->quality);
    float sc = STUDIO_SCALE[q];
    int iw = (int)(s->W * sc), ih = (int)(s->H * sc);
    if (iw < 8) iw = 8;
    if (ih < 8) ih = 8;
    if (iw * ih > s->cap) return;

    static Prim pr[MAX_PRIM];
    V3 centre;
    float bound;
    Skin sk;
    int n = studio_build(g, v->phase, pr, &centre, &bound, &sk);
    if (n <= 0) return;

    Land *c = &s->land;
    c->h.W = iw; c->h.H = ih;
    c->h.expo = VISTA_EXPOSURE * 1.10f;
    c->h.ui = 1.0f;
    c->h.step = 0;
    studio_light(c);
    c->detail = q >= 2 ? 2 : (q >= 1 ? 1 : 0);
    studio_camera(c, v, pr, n, centre, bound, iw, ih);

    studio_ground(c, pr, n, bound, q);
    march_creature(c, pr, n, centre, bound, &sk);

    if (q >= 2) bloom(&c->h, 1.15f, 0.24f);

    /* Resolve into the top-left of the caller's buffer at the internal size,
     * then rescale in place. Doing it this way keeps the film chain shared
     * with every other renderer instead of growing a second copy that drifts
     * out of step with it. */
    uint8_t *tmp = (uint8_t *)malloc((size_t)iw * ih * 4);
    if (!tmp) return;
    resolve(&c->h, tmp);

    /* Box filter when downsampling from the supersampled pass, bilinear when
     * blowing a coarse pass up. The first is what makes quality 3 look
     * antialiased; the second is what makes quality 0 look soft rather than
     * blocky, which is the right failure mode while something is moving. */
    for (int y = 0; y < s->H; y++) {
        for (int x = 0; x < s->W; x++) {
            uint8_t *d = rgba + 4 * ((size_t)y * s->W + x);
            if (sc > 1.001f) {
                int k = (int)(sc + 0.5f);
                int sx0 = x * k, sy0 = y * k;
                int acc[3] = { 0, 0, 0 }, cnt = 0;
                for (int j = 0; j < k; j++)
                    for (int i = 0; i < k; i++) {
                        int sx = sx0 + i, sy = sy0 + j;
                        if (sx >= iw || sy >= ih) continue;
                        const uint8_t *p = tmp + 4 * ((size_t)sy * iw + sx);
                        acc[0] += p[0]; acc[1] += p[1]; acc[2] += p[2];
                        cnt++;
                    }
                if (!cnt) cnt = 1;
                d[0] = (uint8_t)(acc[0] / cnt);
                d[1] = (uint8_t)(acc[1] / cnt);
                d[2] = (uint8_t)(acc[2] / cnt);
            } else {
                float fx = ((float)x + 0.5f) * sc - 0.5f;
                float fy = ((float)y + 0.5f) * sc - 0.5f;
                int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                float tx = fx - (float)x0, ty = fy - (float)y0;
                int x1 = x0 + 1, y1 = y0 + 1;
                if (x0 < 0) x0 = 0;
                if (y0 < 0) y0 = 0;
                if (x1 >= iw) x1 = iw - 1;
                if (y1 >= ih) y1 = ih - 1;
                if (x0 >= iw) x0 = iw - 1;
                if (y0 >= ih) y0 = ih - 1;
                for (int ch = 0; ch < 3; ch++) {
                    float a = tmp[4 * ((size_t)y0 * iw + x0) + ch];
                    float b = tmp[4 * ((size_t)y0 * iw + x1) + ch];
                    float e = tmp[4 * ((size_t)y1 * iw + x0) + ch];
                    float f = tmp[4 * ((size_t)y1 * iw + x1) + ch];
                    d[ch] = (uint8_t)(mixf(mixf(a, b, tx), mixf(e, f, tx), ty) + 0.5f);
                }
            }
            d[3] = 255;
        }
    }
    free(tmp);
}

int cp4_studio_pick(Cp4Studio *s, const Cp4Genome *g, const Cp4View *v,
                    int px, int py)
{
    if (!s || !g || !v) return -1;
    if (px < 0 || py < 0 || px >= s->W || py >= s->H) return -1;

    static Prim pr[MAX_PRIM];
    V3 centre;
    float bound;
    Skin sk;
    int n = studio_build(g, v->phase, pr, &centre, &bound, &sk);
    if (n <= 0) return -1;

    Land *c = &s->land;
    studio_light(c);
    c->detail = 0;
    studio_camera(c, v, pr, n, centre, bound, s->W, s->H);

    float sx = ((float)px + 0.5f - c->W * 0.5f) / c->focal;
    float sy = ((float)py + 0.5f - c->H * 0.5f) / c->focal;
    V3 ray = norm(add(add(mul(c->right, sx), mul(c->up, -sy)), c->fwd));

    V3 oc = sub(c->eye, centre);
    float b = dot(oc, ray);
    float cc = dot(oc, oc) - bound * bound;
    float disc = b * b - cc;
    if (disc < 0.0f) return -1;
    float sq = sqrtf(disc);
    float t = -b - sq, tmax = -b + sq;
    if (tmax < 0.5f) return -1;
    if (t < 0.5f) t = 0.5f;

    /* The same march the renderer runs, stopped at the first hit. Sharing it
     * is the point: a picker with its own idea of where the surface is will
     * eventually disagree with the image, and the user is always right about
     * the image. */
    for (int i = 0; i < 140 && t < tmax; i++) {
        V3 q = add(c->eye, mul(ray, t));
        float d = creature_sdf(pr, n, q, NULL, NULL, NULL);
        if (d < 0.02f) {
            /* Which primitive owns the hit is whichever one is nearest it.
             * The union is smooth, so on a fillet between two parts this is
             * genuinely ambiguous - and nearest is the answer a user dragging
             * at that spot means. */
            float best = 1e9f;
            int bi = -1;
            for (int k = 0; k < n; k++) {
                float dk = sd_cone(q, &pr[k]);
                if (dk < best) { best = dk; bi = k; }
            }
            return bi >= 0 ? pr[bi].part : -1;
        }
        if (d > tmax - t) break;
        t += d * 0.9f;
    }
    return -1;
}
