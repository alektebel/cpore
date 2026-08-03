#include "cpore/land.h"
#include "sdfbody.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ *
 * Stage-3 renderer.
 *
 * The aquatic stage could get away with two analytic planes for its world.
 * Land cannot: terrain is the stage's subject, so the background is a real
 * ray-marched heightfield. Because cp4_height() is a pure function of seed and
 * position there is nothing to store and nothing to stream - the marcher just
 * evaluates the world wherever the ray happens to be.
 *
 * Creatures come from the shared distance field in sdfbody.h, the same code
 * the fish use, so a body plan that reads well in one stage reads well in the
 * other. What is different here is everything above the ground: sky, sun,
 * clouds, terrain shadows and aerial haze instead of water, fog and caustics.
 * ------------------------------------------------------------------ */

#define PI CP_PI
#define MAXW 512
#define MAXH 320

/* highest ground the noise can produce, plus slack - the marcher uses it to
 * abandon rays that are climbing away into open sky */
#define PEAK   190.0f
#define FARCLIP 2600.0f

static inline V3 cv(Cp4Vec v) { return v3(v.x, v.y, v.z); }

typedef struct {
    uint8_t *fb;
    float   *zb;
    int      W, H;
    V3       eye, fwd, right, up;
    float    focal;
    float    hazek;      /* 1 in the world, 0 for a studio portrait */
    uint32_t seed;
    float    time;
    /* Where the camera itself is. The stage has four media and three of them
     * look nothing like standing on a hill, so the background, the fog colour
     * and the fog distance all switch on these. */
    int      submerged, buried;
} Ctx;

/* Direction the light travels. y is down, so a sun in the sky sends light
 * along +y - the same convention the aquatic stage settled on. */
static const V3 SUN = { 0.38f, 0.72f, -0.26f };

static inline void put(Ctx *c, int x, int y, V3 col)
{
    uint8_t *p = c->fb + 4 * ((size_t)y * c->W + x);
    p[0] = (uint8_t)(tonemap(col.x) * 255.0f);
    p[1] = (uint8_t)(tonemap(col.y) * 255.0f);
    p[2] = (uint8_t)(tonemap(col.z) * 255.0f);
    p[3] = 255;
}

/* ---------------- sky ---------------- */

static float rhash(uint32_t s, int x, int z)
{
    uint32_t h = s ^ (uint32_t)(x * 374761393) ^ (uint32_t)(z * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFu) / 65535.0f;
}

static float rnoise(uint32_t s, float x, float z)
{
    float fx = floorf(x), fz = floorf(z);
    int xi = (int)fx, zi = (int)fz;
    float xf = x - fx, zf = z - fz;
    float u = xf * xf * (3.0f - 2.0f * xf), v = zf * zf * (3.0f - 2.0f * zf);
    float a = rhash(s, xi, zi),     b = rhash(s, xi + 1, zi);
    float c = rhash(s, xi, zi + 1), d = rhash(s, xi + 1, zi + 1);
    float ab = a + (b - a) * u, cd = c + (d - c) * u;
    return ab + (cd - ab) * v;
}

static float fbm2(uint32_t s, float x, float z)
{
    return rnoise(s, x, z) * 0.55f + rnoise(s ^ 0x77u, x * 2.1f, z * 2.1f) * 0.28f
         + rnoise(s ^ 0xC3u, x * 4.3f, z * 4.3f) * 0.17f;
}

/* The horizon colour is also the haze colour. Getting this wrong is the
 * fastest way to ruin a landscape: too bright and every hill past the first
 * one is the same pale grey, which is exactly what the first build did. */
static V3 horizon_col(void) { return v3(0.52f, 0.76f, 0.85f); }

static V3 sky_col(const Ctx *c, V3 ray)
{
    float up = clampf(-ray.y, 0.0f, 1.0f);          /* y is down */

    /* Underground there is no sky at all, and underwater the sky is a ceiling
     * of light a long way off rather than a dome. */
    if (c->buried) return v3(0.035f, 0.024f, 0.016f);
    if (c->submerged) {
        V3 deep = v3(0.030f, 0.115f, 0.155f);
        V3 surf = v3(0.240f, 0.520f, 0.560f);
        float t = powf(up, 1.4f);
        return v3(mixf(deep.x, surf.x, t), mixf(deep.y, surf.y, t), mixf(deep.z, surf.z, t));
    }
    /* Saturated on purpose. A realistic pale sky is only a few percent from
     * neutral, and a 32-colour quantiser rounds that straight to grey. */
    /* Cyan-leaning rather than a true sky blue. The abyss palette has a cyan
     * ramp and a violet ramp and nothing between them, so a physically
     * plausible blue quantises to purple - the first land sky came out
     * lavender. Aiming at the ramp that exists is cheaper than adding entries
     * that would change how stage 2 looks. */
    V3 zen = v3(0.22f, 0.62f, 0.78f);
    V3 hor = horizon_col();
    float t = powf(up, 0.55f);
    V3 col = v3(mixf(hor.x, zen.x, t), mixf(hor.y, zen.y, t), mixf(hor.z, zen.z, t));

    /* sun disc and its glow */
    V3 tosun = mul(norm(SUN), -1.0f);
    float d = clampf(dot(ray, tosun), 0.0f, 1.0f);
    col = add(col, mul(v3(1.00f, 0.92f, 0.72f), powf(d, 620.0f) * 5.0f));
    col = add(col, mul(v3(0.90f, 0.76f, 0.52f), powf(d, 6.0f) * 0.30f));

    /* Clouds on a plane high overhead. Projecting the ray onto it rather than
     * painting screen-space noise is what keeps them still while the camera
     * turns, which is most of what sells them as sky rather than as grain. */
    if (ray.y < -0.035f) {
        float tp = (-560.0f - c->eye.y) / ray.y;
        if (tp > 0.0f && tp < 60000.0f) {
            V3 h = add(c->eye, mul(ray, tp));
            float drift = c->time * 3.5f;
            float f = fbm2(c->seed ^ 0xBEEFu, (h.x + drift) * 0.0011f, h.z * 0.0011f);
            /* fbm2 averages 0.5, so a 0.46 threshold was total overcast -
             * the sky came out as one flat sheet of grey */
            float cov = clampf((f - 0.63f) * 5.0f, 0.0f, 1.0f) * clampf(up * 2.6f, 0.0f, 1.0f);
            V3 cl = v3(0.96f, 0.96f, 0.98f);
            /* undersides stay grey, or the sky turns into cotton wool */
            float lit = 0.62f + 0.38f * clampf(dot(ray, tosun), 0.0f, 1.0f);
            col = v3(mixf(col.x, cl.x * lit, cov), mixf(col.y, cl.y * lit, cov),
                     mixf(col.z, cl.z * lit, cov));
        }
    }
    return col;
}

/* What the distance turns into, and how fast. Air buries a ridge at three
 * kilometres, water at a couple of hundred metres, and soil at arm's length -
 * the fog distance is most of what tells the eye which medium it is in. */
static V3 medium_fog(const Ctx *c, float *dist_out, float *cap)
{
    if (c->buried)    { *dist_out = 26.0f;   *cap = 1.00f; return v3(0.045f, 0.030f, 0.020f); }
    if (c->submerged) { *dist_out = 300.0f;  *cap = 0.95f; return v3(0.055f, 0.185f, 0.235f); }
    *dist_out = 3000.0f; *cap = 0.80f;
    return horizon_col();
}

static V3 hazed(const Ctx *c, V3 col, float dist)
{
    if (c->hazek <= 0.0f) return col;
    float fd, cap;
    V3 h = medium_fog(c, &fd, &cap);
    float near = c->buried ? 2.0f : (c->submerged ? 25.0f : 200.0f);
    float d = dist - near;                    /* no haze on what is right here */
    if (d < 0.0f) d = 0.0f;
    /* capped, so even the furthest ridge keeps a little of its own colour and
     * the horizon stays a place rather than a wall */
    float f = (1.0f - expf(-d / fd)) * cap * c->hazek;
    return v3(mixf(col.x, h.x, f), mixf(col.y, h.y, f), mixf(col.z, h.z, f));
}

/* ---------------- terrain ---------------- */

/* March the heightfield with a geometrically growing step, then interpolate
 * back to the crossing. Growing the step is what keeps a 3km view distance to
 * about 130 samples instead of thousands. */
static int terrain_march(const Ctx *c, V3 ro, V3 rd, float tmax, float *tout)
{
    float t = 1.5f, dt = 1.6f, lh = -1.0f;
    for (int i = 0; i < 190 && t < tmax; i++) {
        V3 q = add(ro, mul(rd, t));
        if (q.y < -PEAK && rd.y <= 0.0f) return 0;      /* climbing into sky */
        float h = q.y - cp4_height(c->seed, q.x, q.z);  /* > 0 means underground */
        if (h > 0.0f) {
            /* One linear guess is not enough: by 2km the step is tens of units
             * wide and the error shows up as horizontal terraces across every
             * distant slope. Four bisections cost almost nothing and remove
             * them. */
            float lo = t - dt, hi = t;
            for (int k = 0; k < 4; k++) {
                float mid = 0.5f * (lo + hi);
                V3 m = add(ro, mul(rd, mid));
                if (m.y - cp4_height(c->seed, m.x, m.z) > 0.0f) hi = mid;
                else lo = mid;
            }
            *tout = 0.5f * (lo + hi);
            return 1;
        }
        lh = h;
        t += dt;
        dt *= 1.028f;
    }
    (void)lh;
    return 0;
}

/* The view from inside the soil.
 *
 * The surface marcher is no use here: it looks for the first point where a ray
 * goes *into* the ground, and from a burrow every ray starts there. What
 * matters underground is the opposite crossing - where the ray comes out into
 * open air - because that is the only place light gets in. Everything nearer
 * than that is dirt, and the fog distance of twenty-six units does the rest. */
static int terrain_exit(const Ctx *c, V3 ro, V3 rd, float tmax, float *tout)
{
    float t = 0.5f, dt = 0.9f;
    for (int i = 0; i < 90 && t < tmax; i++) {
        V3 q = add(ro, mul(rd, t));
        if (q.y < cp4_height(c->seed, q.x, q.z)) {   /* out of the ground */
            float lo = t - dt, hi = t;
            for (int k = 0; k < 5; k++) {
                float mid = 0.5f * (lo + hi);
                V3 m = add(ro, mul(rd, mid));
                if (m.y < cp4_height(c->seed, m.x, m.z)) hi = mid;
                else lo = mid;
            }
            *tout = 0.5f * (lo + hi);
            return 1;
        }
        t += dt;
        dt *= 1.06f;
    }
    return 0;
}

/* One shadow ray over the heightfield. Coarse on purpose - hills casting long
 * shadows is worth far more to readability than the shadows being exact. */
static float terrain_shadow(const Ctx *c, V3 q)
{
    V3 l = mul(norm(SUN), -1.0f);
    float t = 6.0f, dt = 7.0f;
    for (int i = 0; i < 16; i++) {
        V3 s = add(q, mul(l, t));
        if (s.y < -PEAK) break;
        if (s.y > cp4_height(c->seed, s.x, s.z)) return 0.35f;
        t += dt;
        dt *= 1.28f;
    }
    return 1.0f;
}

/* Ground colour from elevation and slope. Sand at the waterline, rock where it
 * is too steep to hold soil, pale scree on the peaks, and grass in between
 * with its hue drifting on a low-frequency noise so the map has regions
 * instead of one uniform green. */
static V3 ground_albedo(const Ctx *c, V3 q, float slope, float dist)
{
    float elev = -q.y;
    /* two scales of patchiness: broad regions, then meadow-sized patches that
     * still resolve a few hundred units out */
    float band  = fbm2(c->seed ^ 0x2Au, q.x * 0.00085f, q.z * 0.00085f);
    float patch = rnoise(c->seed ^ 0x91u, q.x * 0.0065f, q.z * 0.0065f);

    V3 grass = v3(0.13f + 0.12f * band, 0.27f + 0.14f * band, 0.10f + 0.08f * band);
    V3 dry   = v3(0.34f, 0.31f, 0.15f);
    V3 rock  = v3(0.25f, 0.23f, 0.21f);
    V3 sand  = v3(0.48f, 0.42f, 0.27f);
    V3 scree = v3(0.46f, 0.47f, 0.50f);

    /* dry grassland in the lowlands, greener as it climbs */
    float wet = clampf((elev + 20.0f) / 120.0f, 0.0f, 1.0f);
    float mix = clampf(wet * (0.55f + 0.75f * patch), 0.0f, 1.0f);
    V3 col = v3(mixf(dry.x, grass.x, mix), mixf(dry.y, grass.y, mix),
                mixf(dry.z, grass.z, mix));

    /* Below the waterline the land ramp is simply wrong - it paints a drowned
     * seabed as dry grassland, which is what made the first underwater shot a
     * flat brown sheet. Silt and sand instead, paling as it shallows. */
    if (elev < -CP4_SEA) {
        float d = clampf((-elev - CP4_SEA) / 90.0f, 0.0f, 1.0f);
        V3 silt = v3(mixf(0.52f, 0.20f, d), mixf(0.48f, 0.22f, d), mixf(0.36f, 0.20f, d));
        float ripple = 0.86f + 0.28f * rnoise(c->seed ^ 0x6Bu, q.x * 0.035f, q.z * 0.02f);
        return mul(silt, ripple);
    }

    float rocky = clampf((0.80f - slope) * 4.0f, 0.0f, 1.0f);
    col = v3(mixf(col.x, rock.x, rocky), mixf(col.y, rock.y, rocky),
             mixf(col.z, rock.z, rocky));

    float shore = clampf(1.0f - fabsf(elev + CP4_SEA) / 22.0f, 0.0f, 1.0f);
    col = v3(mixf(col.x, sand.x, shore), mixf(col.y, sand.y, shore),
             mixf(col.z, sand.z, shore));

    float high = clampf((elev - 96.0f) / 34.0f, 0.0f, 1.0f);
    col = v3(mixf(col.x, scree.x, high), mixf(col.y, scree.y, high),
             mixf(col.z, scree.z, high));

    /* Fine speckle, so a flat field is not a flat colour - but it has to fade
     * out with distance. One sample per pixel of an 0.08-frequency noise turns
     * into salt and pepper the moment a pixel covers more than a few units. */
    float near = clampf(1.0f - dist / 420.0f, 0.0f, 1.0f);
    float g = 1.0f + 0.20f * (rnoise(c->seed ^ 0x5Fu, q.x * 0.08f, q.z * 0.08f) - 0.5f) * near;
    return mul(col, g);
}

/* Creatures drop a soft patch on the ground. A projection rather than a shadow
 * ray, but grounding is what stops everything looking like it floats. */
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

static void draw_world(Ctx *c, const Cp4World *w)
{
    Blot blot[28];
    int nblot = gather_blots(w, blot, 28);
    V3 sun = norm(SUN);
    V3 tosun = mul(sun, -1.0f);

    for (int y = 0; y < c->H; y++) {
        for (int x = 0; x < c->W; x++) {
            float px = ((float)x + 0.5f - c->W * 0.5f) / c->focal;
            float py = -((float)y + 0.5f - c->H * 0.5f) / c->focal;
            V3 ray = norm(add(add(mul(c->right, px), mul(c->up, py)), c->fwd));

            /* Buried: soil in every direction, and a bright mouth wherever
             * the tunnel breaks the surface. */
            if (c->buried) {
                float te = 0.0f;
                int out = terrain_exit(c, c->eye, ray, 200.0f, &te);
                float wall = out ? te : 200.0f;

                /* The wall of the burrow, lit as if by the light behind you:
                 * near soil is bright, far soil is black. That falloff is the
                 * only depth cue down here, because there is no horizon and no
                 * sky to silhouette anything against. */
                V3 q = add(c->eye, mul(ray, wall > 30.0f ? 30.0f : wall));
                float grain = fbm2(c->seed ^ 0x7Du, q.x * 0.11f, q.z * 0.11f)
                            + 0.35f * rnoise(c->seed ^ 0x11u, q.x * 0.55f, q.z * 0.55f);
                float lampl = expf(-wall / 22.0f);
                float e = (0.22f + 0.55f * grain) * lampl;
                V3 col = v3(e * 1.15f, e * 0.78f, e * 0.48f);

                /* Daylight, but only through a genuinely short throat of soil.
                 * Blending on the exit distance alone made every ray find the
                 * sky and the whole burrow rendered as a pale grey field. */
                if (out) {
                    float up = clampf(-ray.y, 0.0f, 1.0f);
                    float thin = expf(-te / 5.5f) * (0.25f + 0.75f * up);
                    V3 day = v3(0.50f + 0.30f * up, 0.64f + 0.26f * up, 0.74f + 0.22f * up);
                    col = v3(mixf(col.x, day.x, thin), mixf(col.y, day.y, thin),
                             mixf(col.z, day.z, thin));
                }
                put(c, x, y, col);
                /* The soil is a backdrop, not geometry: there is no tunnel
                 * mesh, so writing the exit distance here put a wall in front
                 * of the animal and the z-test threw the whole creature away.
                 * Depth underground is carried by the lamp falloff instead. */
                c->zb[(size_t)y * c->W + x] = 1e30f;
                continue;
            }

            float tg = 0.0f;
            int hit = terrain_march(c, c->eye, ray, FARCLIP, &tg);

            /* the lake: a plane, but only where the ground beneath it is
             * actually below the waterline */
            float tw = 1e30f;
            int water = 0;
            if (ray.y > 0.0015f) {
                float t = (CP4_SEA - c->eye.y) / ray.y;
                if (t > 0.0f && (!hit || t < tg)) {
                    V3 h = add(c->eye, mul(ray, t));
                    if (cp4_height(c->seed, h.x, h.z) > CP4_SEA) { tw = t; water = 1; }
                }
            }

            V3 col;
            float dist;
            if (water) {
                V3 q = add(c->eye, mul(ray, tw));
                /* two crossed ripples for the surface normal */
                float rx = sinf(q.x * 0.06f + c->time * 1.7f) * 0.045f
                         + sinf((q.x + q.z) * 0.031f - c->time * 1.1f) * 0.030f;
                float rz = sinf(q.z * 0.052f - c->time * 1.4f) * 0.045f
                         + cosf((q.x - q.z) * 0.028f + c->time * 0.9f) * 0.030f;
                V3 n = norm(v3(rx, -1.0f, rz));
                V3 refl = sub(ray, mul(n, 2.0f * dot(ray, n)));
                V3 sky = sky_col(c, norm(refl));

                /* depth of water at this point tints what shows through */
                float depth = clampf(cp4_height(c->seed, q.x, q.z) - CP4_SEA, 0.0f, 90.0f);
                V3 deep = v3(0.06f + 0.10f * (1.0f - depth / 90.0f),
                             0.20f + 0.22f * (1.0f - depth / 90.0f),
                             0.26f + 0.20f * (1.0f - depth / 90.0f));
                /* Fresnel: water is a mirror at grazing angles and a window
                 * straight down, and getting that one term right does more for
                 * a lake than any amount of colour tuning */
                float f = powf(1.0f - clampf(-dot(ray, n), 0.0f, 1.0f), 4.0f);
                f = 0.05f + 0.95f * f;
                col = v3(mixf(deep.x, sky.x, f), mixf(deep.y, sky.y, f),
                         mixf(deep.z, sky.z, f));
                float spec = powf(clampf(dot(norm(refl), tosun), 0.0f, 1.0f), 90.0f);
                col = add(col, mul(v3(1.0f, 0.94f, 0.78f), spec * 1.6f));
                dist = tw;
            } else if (hit) {
                V3 q = add(c->eye, mul(ray, tg));
                float nx, ny, nz;
                cp4_normal(c->seed, q.x, q.z, &nx, &ny, &nz);
                V3 n = v3(nx, ny, nz);
                float slope = clampf(-ny, 0.0f, 1.0f);

                V3 alb = ground_albedo(c, q, slope, tg);
                for (int b = 0; b < nblot; b++) {
                    float dx = q.x - blot[b].x, dz = q.z - blot[b].z;
                    float spread = blot[b].r * 1.15f;
                    float d2 = (dx * dx + dz * dz) / (spread * spread);
                    if (d2 >= 1.0f) continue;
                    float k = (1.0f - d2) * (1.0f - d2);
                    alb = mul(alb, 1.0f - 0.45f * k);
                }

                float lam = clampf(-dot(n, sun), 0.0f, 1.0f);
                float sh = tg < 900.0f ? terrain_shadow(c, q) : 1.0f;
                /* sky light lands on what faces up; a warm bounce comes back
                 * off everything else */
                float skyamt = clampf(slope, 0.0f, 1.0f);
                V3 lightv = add(add(mul(v3(1.00f, 0.94f, 0.80f), 1.00f * lam * sh),
                                    mul(v3(0.30f, 0.40f, 0.60f), 0.26f * skyamt)),
                                mul(v3(0.26f, 0.22f, 0.15f), 0.13f));
                col = v3(alb.x * lightv.x, alb.y * lightv.y, alb.z * lightv.z);
                dist = tg;
            } else {
                col = sky_col(c, ray);
                dist = FARCLIP * 4.0f;
            }

            if (dist < FARCLIP * 2.0f) col = hazed(c, col, dist);
            put(c, x, y, col);
            c->zb[(size_t)y * c->W + x] = (dist < FARCLIP * 2.0f)
                                        ? dist * dot(ray, c->fwd) : 1e30f;
        }
    }
}

/* ---------------- creature geometry ---------------- */

typedef struct {
    V3    base, mark;
    int   pattern;
    float freq;
    V3    origin, fwd, right, up;
} Skin;

static V3 part_albedo4(int t, float *emissive)
{
    *emissive = 0.0f;
    switch (t) {
    case CP4_MOUTH_G: return v3(0.72f, 0.60f, 0.44f);
    case CP4_MOUTH_C: return v3(0.94f, 0.90f, 0.80f);
    case CP4_MOUTH_O: return v3(0.88f, 0.72f, 0.34f);
    case CP4_LEG:     return v3(0.54f, 0.46f, 0.36f);
    case CP4_FOOT:    return v3(0.38f, 0.32f, 0.26f);
    case CP4_CLAW:    return v3(0.20f, 0.19f, 0.22f);
    case CP4_HORN:    return v3(0.86f, 0.82f, 0.68f);
    case CP4_PLATE:   return v3(0.46f, 0.44f, 0.42f);
    case CP4_EYE:     return v3(0.97f, 0.98f, 1.00f);
    case CP4_EAR:     return v3(0.66f, 0.50f, 0.46f);
    case CP4_VOICE:   return v3(0.92f, 0.42f, 0.40f);
    case CP4_PLUME:   return v3(0.86f, 0.30f, 0.62f);
    case CP4_WING:    return v3(0.60f, 0.62f, 0.72f);
    case CP4_FIN:     return v3(0.42f, 0.70f, 0.80f);
    case CP4_GILL:    return v3(0.84f, 0.34f, 0.36f);
    case CP4_DIGGER:  return v3(0.74f, 0.68f, 0.52f);
    default:          return v3(0.6f, 0.6f, 0.6f);
    }
}

/* Build a land animal in world space.
 *
 * The one real departure from the fish is legs: they are not decorative studs
 * on the hull but a two-bone chain that reaches the ground and swings on the
 * gait phase, because on land the contact between animal and terrain is the
 * thing the eye checks first. */
static int build_prims4(const Cp4Beast *b, int is_player, Prim *out,
                        V3 *centre, float *bound, Skin *skin)
{
    V3 fwd, right, up;
    basis3(b->yaw, b->pitch, &fwd, &right, &up);

    int nseg = b->g.nseg < 2 ? 2 : b->g.nseg;
    float R = b->s.radius;
    float L = b->s.length;

    float base[3], mark[3];
    cp4_genome_colour(&b->g, base, mark);
    V3 body = v3(base[0], base[1], base[2]);
    if (is_player) {
        /* Brighten the agent's own animal without touching its hue.
         *
         * The obvious tint - lerp toward a fixed colour - produces a
         * low-chroma mid tone, and a low-chroma mid tone is the one thing a
         * 32-entry palette has no good answer for: the quantiser reached for
         * the nearest dark violet and the player rendered as a black
         * silhouette in its own frame. Scaling value up keeps the chroma the
         * palette needs to place it correctly. */
        float mx = body.x > body.y ? body.x : body.y;
        if (body.z > mx) mx = body.z;
        if (mx > 1e-3f && mx < 0.86f) body = mul(body, 0.86f / mx);
    }
    skin->base = body;
    skin->mark = v3(mark[0], mark[1], mark[2]);
    skin->pattern = b->g.pattern;
    skin->freq = 0.02f + 0.16f * ((float)b->g.pscale / 255.0f);
    skin->origin = cv(b->p);
    skin->fwd = fwd; skin->right = right; skin->up = up;

    float arch  = (float)b->g.arch  / 127.0f * R * 1.3f;
    float sweep = (float)b->g.sweep / 127.0f * R * 0.9f;

    V3 segpos[CP4_MAX_SEG];
    float segrad[CP4_MAX_SEG];
    for (int i = 0; i < nseg; i++) {
        float t = (float)i / (float)(nseg - 1);
        float along = (0.5f - t) * L;
        float bend = sinf(PI * t);
        /* a walking animal sways, it does not undulate - a tenth of the
         * amplitude the fish use */
        float sway = sinf(b->phase * 0.5f - t * 1.4f) * R * 0.10f;
        segpos[i] = add(add(add(cv(b->p), mul(fwd, along)),
                            mul(right, sway + sweep * bend)),
                        mul(up, arch * bend));
        int li = i < CP4_MAX_SEG ? i : CP4_MAX_SEG - 1;
        segrad[i] = R * cp4_profile(&b->g, t)
                      * (1.0f + (float)b->g.lump[li] / 127.0f * 0.40f);
        if (segrad[i] < R * 0.15f) segrad[i] = R * 0.15f;
    }

    int n = 0;
    float bk = R * 0.34f;
    for (int i = 0; i + 1 < nseg; i++)
        push(out, &n, segpos[i], segpos[i + 1], segrad[i], segrad[i + 1],
             bk, body, 0.0f, 1.0f);
    {
        V3 tail = add(segpos[nseg - 1], mul(fwd, -L * 0.20f));
        push(out, &n, segpos[nseg - 1], tail, segrad[nseg - 1],
             R * 0.14f, bk, body, 0.0f, 1.0f);
    }

    int legi = 0;
    for (int i = 0; i < CP4_MAX_PARTS; i++) {
        int t = b->g.part[i].type;
        if (t == CP4_NONE) continue;
        int sg = b->g.part[i].seg;
        if (sg >= nseg) sg = nseg - 1;

        float er;
        V3 col = part_albedo4(t, &er);
        float sc = 0.45f + 1.45f * ((float)b->g.part[i].scale / 255.0f);
        int copies = b->g.part[i].mirror ? 2 : 1;

        for (int m = 0; m < copies; m++) {
            int yaw_u = m ? ((256 - b->g.part[i].yaw) & 0xFF) : b->g.part[i].yaw;
            float py = (float)yaw_u * (2.0f * PI / 256.0f);
            float pp = (float)b->g.part[i].pitch * (PI / 128.0f);
            float cy = cosf(py), sy = sinf(py), cpp = cosf(pp), spp = sinf(pp);
            V3 ax = norm(add(add(mul(fwd, cy * cpp), mul(right, sy * cpp)), mul(up, spp)));
            V3 bs = add(segpos[sg], mul(ax, segrad[sg] * 0.60f));

            switch (t) {
            case CP4_LEG: {
                /* A leg is the one part that is not free to point wherever the
                 * gene says. It sprouts from the lower flank and it ends on the
                 * ground, because on land the contact between animal and
                 * terrain is the first thing the eye checks - a leg hanging in
                 * space reads as a broken model, not as an odd body plan.
                 *
                 * The foot plane is exact rather than estimated: p.y is the
                 * ground minus stand, so ground is p.y + stand. */
                float side = m ? -1.0f : 1.0f;
                V3 down = v3(0.0f, 1.0f, 0.0f);          /* y is down */
                V3 hip = add(add(segpos[sg], mul(right, side * segrad[sg] * 0.78f)),
                             mul(down, segrad[sg] * 0.34f));
                float ground_y = b->p.y + b->s.stand;
                float len = ground_y - hip.y;
                if (len < R * 0.35f) len = R * 0.35f;

                /* alternating gait: opposite legs a half cycle apart */
                float ph = b->phase + (float)legi * 1.7f;
                float stride = cosf(ph) * len * 0.34f;
                float lift = clampf(sinf(ph), 0.0f, 1.0f) * len * 0.26f;
                V3 foot = v3(hip.x + fwd.x * stride, ground_y - lift,
                             hip.z + fwd.z * stride);
                V3 knee = add(add(mul(add(hip, foot), 0.5f), mul(fwd, len * 0.20f)),
                              mul(right, side * len * 0.10f));

                float tk = R * 0.30f * sc;
                push(out, &n, hip, knee, tk, tk * 0.76f, R * 0.16f, col, 0.0f, 0.0f);
                push(out, &n, knee, foot, tk * 0.76f, tk * 0.55f, R * 0.12f, col, 0.0f, 0.0f);
                push(out, &n, foot, add(foot, mul(fwd, R * 0.30f * sc)),
                     tk * 0.62f, tk * 0.46f, R * 0.08f,
                     part_albedo4(CP4_FOOT, &er), 0.0f, 0.0f);
                legi++;
                break;
            }
            case CP4_FOOT: {
                /* a broad pad, spread flat on the ground under the body */
                float side = m ? -1.0f : 1.0f;
                V3 at = v3(segpos[sg].x + right.x * side * R * 0.62f,
                           b->p.y + b->s.stand - R * 0.10f,
                           segpos[sg].z + right.z * side * R * 0.62f);
                push(out, &n, at, add(at, mul(fwd, R * 0.46f * sc)),
                     R * 0.30f * sc, R * 0.22f * sc, R * 0.10f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_HORN:
                push(out, &n, bs, add(bs, mul(ax, R * 1.45f * sc)),
                     R * 0.28f * sc, R * 0.03f, R * 0.14f, col, 0.0f, 0.0f);
                break;
            case CP4_CLAW:
                push(out, &n, bs, add(bs, mul(ax, R * 0.60f * sc)),
                     R * 0.16f * sc, R * 0.02f, R * 0.07f, col, 0.0f, 0.0f);
                break;
            case CP4_PLATE: {
                /* a shield lying against the flank, not a lump stuck on it */
                V3 e = add(bs, mul(ax, R * 0.10f));
                push(out, &n, e, add(e, mul(fwd, R * 0.55f * sc)),
                     R * 0.62f * sc, R * 0.50f * sc, R * 0.30f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_MOUTH_C:
            case CP4_MOUTH_O:
            case CP4_MOUTH_G: {
                float jl = (t == CP4_MOUTH_C ? 0.95f : t == CP4_MOUTH_O ? 0.80f : 0.62f);
                push(out, &n, bs, add(bs, mul(ax, R * jl * sc)),
                     R * 0.46f * sc, R * 0.22f * sc, R * 0.22f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_EYE: {
                V3 e = add(bs, mul(ax, R * 0.20f));
                float er2 = R * 0.26f * (0.7f + 0.5f * sc);
                push(out, &n, e, e, er2, er2, R * 0.04f, col, 0.0f, 0.0f);
                V3 pu = add(e, mul(ax, er2 * 0.70f));
                push(out, &n, pu, pu, er2 * 0.46f, er2 * 0.46f, R * 0.03f,
                     v3(0.05f, 0.05f, 0.08f), 0.0f, 0.0f);
                break;
            }
            case CP4_EAR: {
                V3 tip = add(add(bs, mul(ax, R * 0.85f * sc)), mul(up, R * 0.35f * sc));
                push(out, &n, bs, tip, R * 0.26f * sc, R * 0.07f, R * 0.10f,
                     col, 0.0f, 0.0f);
                break;
            }
            case CP4_VOICE: {
                /* the throat sac inflates on the call - the one part of the
                 * body that animates because of what the animal is doing */
                float puff = 1.0f + 0.35f * clampf(sinf(b->sing_t * 9.0f), 0.0f, 1.0f)
                                  * (b->sing_t > 0.0f ? 1.0f : 0.0f);
                V3 e = add(bs, mul(ax, R * 0.30f));
                float rr = R * 0.42f * sc * puff;
                push(out, &n, e, e, rr, rr, R * 0.20f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_PLUME: {
                /* a fan, because one spike does not read as display */
                for (int f = -1; f <= 1; f++) {
                    V3 dir = norm(add(ax, mul(right, (float)f * 0.45f)));
                    V3 tip = add(bs, mul(dir, R * 1.5f * sc));
                    push(out, &n, bs, tip, R * 0.10f * sc, R * 0.30f * sc,
                         R * 0.08f, f ? skin->mark : col, 0.0f, 0.0f);
                }
                break;
            }
            case CP4_WING: {
                float beat = sinf(b->phase * 0.8f + (float)m * 3.1f) * 0.25f;
                V3 dir = norm(add(ax, mul(up, beat)));
                push(out, &n, bs, add(bs, mul(dir, R * 2.0f * sc)),
                     R * 0.46f * sc, R * 0.10f * sc, R * 0.22f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_FIN: {
                /* a broad blade that sweeps, so a swimmer reads as propelling
                 * itself rather than as a legged animal that fell in */
                float sweep_f = sinf(b->phase * 1.4f + (float)i + (float)m * 3.1f) * 0.40f;
                V3 dir = norm(add(ax, mul(up, sweep_f)));
                push(out, &n, bs, add(bs, mul(dir, R * 1.5f * sc)),
                     R * 0.42f * sc, R * 0.09f * sc, R * 0.24f, col, 0.0f, 0.0f);
                break;
            }
            case CP4_GILL: {
                /* three slits along the flank, the one part that says "this
                 * body belongs in the water" while it is standing on a beach */
                for (int q = 0; q < 3; q++) {
                    V3 at = add(bs, mul(fwd, -(float)q * R * 0.26f));
                    push(out, &n, at, add(at, mul(up, -R * 0.34f * sc)),
                         R * 0.10f * sc, R * 0.05f * sc, R * 0.05f, col, 0.0f, 0.0f);
                }
                break;
            }
            case CP4_DIGGER: {
                /* a heavy spade, blunter and wider than a claw */
                V3 dir = norm(add(ax, mul(fwd, 0.55f)));
                push(out, &n, bs, add(bs, mul(dir, R * 0.85f * sc)),
                     R * 0.30f * sc, R * 0.34f * sc, R * 0.12f, col, 0.0f, 0.0f);
                break;
            }
            default:
                push(out, &n, bs, add(bs, mul(ax, R * 0.30f * sc)),
                     R * 0.32f * sc, R * 0.28f * sc, R * 0.18f, col, er, 0.0f);
                break;
            }
        }
    }

    prim_bounds(out, n, cv(b->p), centre, bound);
    return n;
}

/* Markings, applied only where the surface is trunk rather than appendage.
 * Colour carries as much perceived variety as shape, and quantising to 32
 * colours punishes gradients, so the patterns are deliberately crisp. */
static V3 apply_pattern(const Skin *sk, V3 q, V3 albedo, float bodyw)
{
    if (bodyw <= 0.02f || sk->pattern == CP4_PAT_PLAIN) return albedo;
    V3 d = sub(q, sk->origin);
    float along = dot(d, sk->fwd), side = dot(d, sk->right), vert = dot(d, sk->up);
    float m = 0.0f;
    switch (sk->pattern) {
    case CP4_PAT_BANDS:
        m = sinf(along * sk->freq * 6.0f) > 0.15f ? 1.0f : 0.0f;
        break;
    case CP4_PAT_SPOTS:
        m = (sinf(along * sk->freq * 5.0f) * sinf(side * sk->freq * 5.0f)
           * sinf(vert * sk->freq * 5.0f)) > 0.30f ? 1.0f : 0.0f;
        break;
    case CP4_PAT_COUNTER:
        m = clampf(0.5f + vert * 0.16f, 0.0f, 1.0f);
        break;
    case CP4_PAT_STRIPES:
        m = sinf(atan2f(vert, side) * 4.0f + along * sk->freq * 0.9f) > 0.1f ? 1.0f : 0.0f;
        break;
    case CP4_PAT_MOTTLE: {
        float a = sinf(along * sk->freq * 3.1f) + sinf(side * sk->freq * 4.7f)
                + sinf(vert * sk->freq * 2.3f) + sinf((along + side) * sk->freq * 6.1f);
        m = a > 0.55f ? 1.0f : 0.0f;
        break;
    }
    case CP4_PAT_GRADIENT:
        m = clampf(0.5f - along * sk->freq * 0.55f, 0.0f, 1.0f);
        m = m > 0.5f ? (m - 0.5f) * 2.0f : 0.0f;
        break;
    case CP4_PAT_RINGS:
        m = sinf(sqrtf(side * side + vert * vert) * sk->freq * 7.0f) > 0.2f ? 1.0f : 0.0f;
        break;
    default: break;
    }
    m *= bodyw;
    return v3(mixf(albedo.x, sk->mark.x, m), mixf(albedo.y, sk->mark.y, m),
              mixf(albedo.z, sk->mark.z, m));
}

static void shade_hit(Ctx *c, int x, int y, V3 nrm, V3 albedo, float em, float vz,
                      float ao, float shadow)
{
    V3 sun = norm(SUN);
    float lam = clampf(-dot(nrm, sun), 0.0f, 1.0f);
    float ndotv = clampf(-dot(nrm, c->fwd), 0.0f, 1.0f);
    float rim = powf(1.0f - ndotv, 2.6f);
    float up_amt = clampf(-nrm.y, 0.0f, 1.0f);

    /* Sun, sky and ground bounce, kept well apart in both colour and value.
     *
     * The sky term is a dome, not a second spotlight. Writing it as
     * 0.30 * up_amt looks reasonable and is badly wrong outdoors: a flank
     * facing sideways picks up almost nothing, so any animal whose lit side
     * happened to face away from the sun rendered as a black cutout in broad
     * daylight. A real overcast contribution never drops to zero, hence the
     * floor - and once it is there the whole scene stops needing the key light
     * to be legible. */
    float key   = 1.15f * lam * shadow;
    float sky   = 0.46f * (0.35f + 0.65f * up_amt);
    float grnd  = 0.28f * (1.0f - up_amt);
    float fill  = 0.26f * ndotv;

    /* Underground and deep underwater the sun is not a factor, so the body is
     * lit from the camera with a distance falloff instead. Without it an
     * animal in its own burrow is black on black - the shading model has to
     * follow the medium the same way the background does. */
    if (c->buried || c->submerged) {
        float lamp = expf(-vz / (c->buried ? 55.0f : 190.0f));
        float k = (c->buried ? 0.95f : 0.55f) * lamp * (0.30f + 0.70f * ndotv);
        V3 tint = c->buried ? v3(1.00f, 0.86f, 0.66f) : v3(0.72f, 0.94f, 1.00f);
        V3 base = c->buried ? v3(0.10f, 0.09f, 0.07f)
                            : mul(v3(0.16f, 0.34f, 0.40f), 0.55f);
        V3 lit = add(mul(v3(albedo.x * tint.x, albedo.y * tint.y, albedo.z * tint.z),
                         k + (c->submerged ? 0.42f * (0.35f + 0.65f * up_amt) * ao : 0.0f)),
                     mul(albedo, 0.0f));
        lit = add(lit, mul(base, ao));
        if (c->submerged) lit = add(lit, mul(v3(albedo.x, albedo.y, albedo.z),
                                             0.30f * lam * shadow));
        lit = add(lit, mul(v3(0.72f, 0.80f, 0.92f), rim * 0.16f * ao));
        if (em > 0.0f) lit = add(lit, mul(albedo, em));
        put(c, x, y, hazed(c, lit, vz));
        return;
    }

    V3 sunc = v3(1.00f, 0.95f, 0.82f);
    V3 skyc = v3(0.66f, 0.80f, 1.00f);
    V3 bncc = v3(0.78f, 0.70f, 0.46f);
    V3 col = add(add(mul(v3(albedo.x * sunc.x, albedo.y * sunc.y, albedo.z * sunc.z), key),
                     mul(v3(albedo.x * skyc.x, albedo.y * skyc.y, albedo.z * skyc.z),
                         (sky + fill) * ao)),
                 mul(v3(albedo.x * bncc.x, albedo.y * bncc.y, albedo.z * bncc.z), grnd * ao));
    col = add(col, mul(v3(0.72f, 0.80f, 0.92f), rim * 0.22f * ao));
    if (em > 0.0f) col = add(col, mul(albedo, em));
    put(c, x, y, hazed(c, col, vz));
}

static void march_creature(Ctx *c, const Prim *pr, int n, V3 centre, float bound,
                           const Skin *sk)
{
    V3 d = sub(centre, c->eye);
    float vz = dot(d, c->fwd);
    if (vz < 4.0f) return;
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
                t += dist * 0.8f;
            }
            if (!hit) continue;

            float hz = dot(sub(q, c->eye), c->fwd);
            if (hz >= *zp) continue;
            *zp = hz;

            V3 albedo; float em, bw;
            creature_sdf(pr, n, q, &albedo, &em, &bw);
            albedo = apply_pattern(sk, q, albedo, bw);
            V3 nrm = sdf_normal(pr, n, q);
            float ao = sdf_ao(pr, n, q, nrm);
            float sh = sdf_shadow(pr, n, q, mul(norm(SUN), -1.0f));
            shade_hit(c, x, y, nrm, albedo, em, hz, ao, sh);
        }
    }
}

/* ---------------- sphere impostors (everything far away) ---------------- */

static void sphere(Ctx *c, V3 wp, float rad, V3 albedo, float emissive)
{
    V3 d = sub(wp, c->eye);
    float vz = dot(d, c->fwd);
    if (vz < 6.0f) return;

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
            float z = vz - rad * nz;
            float *zp = &c->zb[(size_t)y * c->W + x];
            if (z >= *zp) continue;
            *zp = z;

            V3 n = add(add(mul(c->right, nx), mul(c->up, -ny)), mul(c->fwd, -nz));
            float lam = clampf(-dot(n, sun), 0.0f, 1.0f);
            float up_amt = clampf(-n.y, 0.0f, 1.0f);
            /* same dome floor as shade_hit, for the same reason */
            float k = 1.15f * lam + 0.34f * (0.35f + 0.65f * up_amt) + 0.13f * nz;
            V3 col = v3(albedo.x * k, albedo.y * k, albedo.z * k);
            if (emissive > 0.0f) col = add(col, mul(albedo, emissive));
            put(c, x, y, hazed(c, col, z));
        }
    }
}

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

#define MARCH_MIN_PX 9.0f

static void draw_creature(Ctx *c, const Cp4Beast *b, int is_player)
{
    Prim pr[MAX_PRIM];
    Skin sk;
    V3 centre;
    float bound;
    int n = build_prims4(b, is_player, pr, &centre, &bound, &sk);
    if (n <= 0) return;

    V3 d = sub(centre, c->eye);
    float vz = dot(d, c->fwd);
    if (vz < 4.0f) return;
    float px = c->focal * bound / vz;

    if (px >= MARCH_MIN_PX) march_creature(c, pr, n, centre, bound, &sk);
    else                    impostor_creature(c, pr, n);
}

/* ---------------- scenery ---------------- */

static void draw_flora(Ctx *c, const Cp4World *w)
{
    for (int i = 0; i < CP4_MAX_FLORA; i++) {
        const Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        /* A tuber is buried, so it is only visible from inside the soil, and a
         * frond of kelp underwater is hardly worth drawing from a hilltop
         * three hundred units up. Culling by medium is both cheaper and more
         * honest than drawing everything and letting the fog hide it. */
        int med = cp4_flora_medium(f->type);
        if (med == CP4_UNDER && !c->buried) continue;
        V3 at = cv(f->p);
        uint32_t sd = (uint32_t)i * 2654435761u;

        if (f->type == CP4_FLORA_KELP) {
            /* a holdfast and three fronds leaning on the current */
            float h = f->r * 2.4f;
            float lean = sinf(c->time * 0.6f + (float)i) * f->r * 0.35f;
            V3 leaf = v3(0.16f, 0.42f, 0.30f);
            sphere(c, v3(at.x, at.y - f->r * 0.20f, at.z), f->r * 0.28f,
                   v3(0.24f, 0.30f, 0.20f), 0.0f);
            for (int q = 0; q < 3; q++) {
                float a = (float)q * 2.09f;
                sphere(c, v3(at.x + cosf(a) * f->r * 0.30f + lean,
                             at.y - h * (0.45f + 0.18f * (float)q),
                             at.z + sinf(a) * f->r * 0.30f),
                       f->r * 0.34f, leaf, 0.0f);
            }
            continue;
        }
        if (f->type == CP4_FLORA_TUBER) {
            /* a pale knot of root, lit only by whatever the burrow lets in */
            sphere(c, at, f->r * 0.55f, v3(0.72f, 0.64f, 0.40f), 0.06f);
            sphere(c, v3(at.x + f->r * 0.35f, at.y - f->r * 0.20f, at.z),
                   f->r * 0.32f, v3(0.62f, 0.54f, 0.32f), 0.04f);
            continue;
        }
        (void)sd;
        if (f->type == CP4_FLORA_BUSH) {
            /* a trunk and three lobes reads as a shrub from any angle, which a
             * single sphere very much does not */
            float h = f->r * 1.5f;
            uint32_t s = (uint32_t)i * 2654435761u;
            float tint = (float)((s >> 8) & 0xFF) / 255.0f;
            V3 leaf = v3(0.14f + 0.14f * tint, 0.34f + 0.20f * tint, 0.13f + 0.08f * tint);
            sphere(c, v3(at.x, at.y - h * 0.30f, at.z), f->r * 0.22f,
                   v3(0.32f, 0.24f, 0.16f), 0.0f);
            sphere(c, v3(at.x, at.y - h, at.z), f->r * 0.80f, leaf, 0.0f);
            sphere(c, v3(at.x + f->r * 0.55f, at.y - h * 0.72f, at.z + f->r * 0.20f),
                   f->r * 0.52f, leaf, 0.0f);
            sphere(c, v3(at.x - f->r * 0.44f, at.y - h * 0.66f, at.z - f->r * 0.40f),
                   f->r * 0.48f, leaf, 0.0f);
        } else {
            V3 meat = v3(0.56f, 0.22f, 0.20f);
            sphere(c, v3(at.x, at.y - f->r * 0.45f, at.z), f->r * 0.62f, meat, 0.0f);
            sphere(c, v3(at.x + f->r * 0.5f, at.y - f->r * 0.30f, at.z), f->r * 0.34f,
                   v3(0.80f, 0.78f, 0.70f), 0.0f);
        }
    }
}

/* The player's own nest. It has to look built rather than found - a ring of
 * stones would be indistinguishable from a rival's - so it is a woven bowl
 * with eggs in it, and the eggs are the thing worth seeing. */
static void draw_home(Ctx *c, const Cp4World *w)
{
    const Cp4Home *h = &w->home;
    if (!h->alive) return;
    V3 at = cv(h->p);
    V3 wall = v3(0.46f, 0.33f, 0.18f);
    for (int i = 0; i < 12; i++) {
        float a = (float)i / 12.0f * 2.0f * PI;
        sphere(c, v3(at.x + cosf(a) * 22.0f, at.y - 7.0f, at.z + sinf(a) * 22.0f),
               7.0f, wall, 0.0f);
    }
    sphere(c, v3(at.x, at.y - 2.0f, at.z), 15.0f, v3(0.30f, 0.22f, 0.12f), 0.0f);
    /* how full the larder is, as eggs rather than as a number */
    int eggs = (int)(h->store / 20.0f);
    if (eggs > 3) eggs = 3;
    for (int i = 0; i < eggs; i++) {
        float a = (float)i * 2.09f;
        sphere(c, v3(at.x + cosf(a) * 7.0f, at.y - 10.0f, at.z + sinf(a) * 7.0f),
               5.0f, v3(0.88f, 0.86f, 0.74f), 0.05f);
    }
}

/* A nest is a ring of stones, coloured by how the species feels about you.
 * The diplomacy state has to be visible in the world and not only in the HUD,
 * or the whole impress-or-eat fork happens off screen. */
static void draw_nests(Ctx *c, const Cp4World *w)
{
    for (int k = 0; k < CP4_MAX_NESTS; k++) {
        const Cp4Nest *nst = &w->nest[k];
        if (!nst->alive) continue;
        float s = nst->standing;
        V3 tone = s >= 0.65f ? v3(0.35f, 0.85f, 0.45f)
                : s <= -0.5f ? v3(0.88f, 0.28f, 0.22f)
                             : v3(0.72f, 0.66f, 0.52f);
        V3 at = cv(nst->p);
        for (int i = 0; i < 9; i++) {
            float a = (float)i / 9.0f * 2.0f * PI;
            float rx = at.x + cosf(a) * 26.0f, rz = at.z + sinf(a) * 26.0f;
            float ry = cp4_height(c->seed, rx, rz);
            sphere(c, v3(rx, ry - 4.0f, rz), 6.0f, tone, 0.0f);
        }
        /* a totem in the middle, so a nest is findable across a valley */
        sphere(c, v3(at.x, at.y - 22.0f, at.z), 7.0f, tone, 0.10f);
        sphere(c, v3(at.x, at.y - 34.0f, at.z), 4.5f, tone, 0.16f);
    }
}

/* ---------------- outline pass ---------------- */

static void outline_pass(Ctx *c)
{
    for (int y = 1; y < c->H - 1; y++) {
        for (int x = 1; x < c->W - 1; x++) {
            float z = c->zb[(size_t)y * c->W + x];
            if (z > 1e29f) continue;
            float thr = 0.06f * z + 2.0f;
            const float zl = c->zb[(size_t)y * c->W + x - 1];
            const float zr = c->zb[(size_t)y * c->W + x + 1];
            const float zu = c->zb[(size_t)(y - 1) * c->W + x];
            const float zd = c->zb[(size_t)(y + 1) * c->W + x];
            if (zl - z > thr || zr - z > thr || zu - z > thr || zd - z > thr) {
                uint8_t *p = c->fb + 4 * ((size_t)y * c->W + x);
                p[0] = (uint8_t)(p[0] * 0.38f);
                p[1] = (uint8_t)(p[1] * 0.40f);
                p[2] = (uint8_t)(p[2] * 0.44f);
            }
        }
    }
}

/* ---------------- HUD ---------------- */

static void draw_hud4(uint8_t *fb, int W, int H, const Cp4World *w)
{
    char buf[80];
    const Cp4Beast *p = &w->player;

    #define PANEL(X,Y,PW,PH) do {                                             \
        cp_px_rect(fb, W, H, (X), (Y), (PW), (PH), 0.05f, 0.06f, 0.04f, 0.84f);\
        cp_px_rect(fb, W, H, (X), (Y), (PW), 1, 0.52f, 0.62f, 0.40f, 0.55f);  \
    } while (0)
    #define BAR(X,Y,BW,F,R,G,B) do {                                          \
        cp_px_rect(fb, W, H, (X), (Y), (BW), 3, 0.03f, 0.04f, 0.03f, 1.0f);   \
        int _f = (int)((BW) * clampf((F), 0.0f, 1.0f));                       \
        if (_f > 0) cp_px_rect(fb, W, H, (X), (Y), _f, 3, (R), (G), (B), 1.0f); \
    } while (0)

    PANEL(3, 3, 112, 40);
    cp_px_text(fb, W, H, 6, 6, 1, "CREATURE STAGE", 0.78f, 0.92f, 0.62f, 1.0f);
    snprintf(buf, sizeof(buf), "G%d/%d T%d", w->generation + 1, CP4_GENERATIONS, w->step);
    cp_px_text(fb, W, H, 6, 15, 1, buf, 0.56f, 0.62f, 0.46f, 1.0f);
    BAR(6, 24, 48, p->hp / p->hp_max, 0.86f, 0.28f, 0.30f);
    BAR(58, 24, 48, w->dna / CP4_DNA_GOAL, 0.42f, 0.78f, 0.94f);
    BAR(6, 30, 48, p->stam / (p->s.stamina > 1.0f ? p->s.stamina : 1.0f),
        0.92f, 0.80f, 0.30f);
    BAR(58, 30, 48, p->energy / 170.0f, 0.44f, 0.84f, 0.44f);
    snprintf(buf, sizeof(buf), "ALLY %d  FOE %d", w->allies, w->enemies);
    cp_px_text(fb, W, H, 6, 35, 1, buf, 0.62f, 0.86f, 0.60f, 1.0f);

    /* Which medium, and how long you may stay in it. In three of the four the
     * clock is the thing that kills you, so it belongs next to health. */
    {
        static const float MC[CP4_MEDIUM_COUNT][3] = {
            { 0.62f, 0.86f, 0.50f }, { 0.40f, 0.80f, 0.92f },
            { 0.86f, 0.88f, 0.96f }, { 0.84f, 0.66f, 0.40f },
        };
        int m = p->medium % CP4_MEDIUM_COUNT;
        snprintf(buf, sizeof(buf), "%s", cp4_medium_name(m));
        for (char *q = buf; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
        cp_px_rect(fb, W, H, 118, 6, 4, 4, MC[m][0], MC[m][1], MC[m][2], 1.0f);
        cp_px_text(fb, W, H, 124, 5, 1, buf, MC[m][0], MC[m][1], MC[m][2], 1.0f);
        if (p->s.breath < 1.0e8f && (m == CP4_IN_WATER || m == CP4_UNDER)) {
            float f = clampf(p->breath / (p->s.breath > 1.0f ? p->s.breath : 1.0f),
                             0.0f, 1.0f);
            cp_px_rect(fb, W, H, 118, 13, 40, 3, 0.03f, 0.04f, 0.03f, 1.0f);
            int fw = (int)(40.0f * f);
            if (fw > 0)
                cp_px_rect(fb, W, H, 118, 13, fw, 3,
                           f < 0.3f ? 0.94f : 0.42f, f < 0.3f ? 0.36f : 0.82f, 0.94f, 1.0f);
        }
    }

    /* Population panel: nothing scripts these numbers, they are whatever
     * survived out there while the player was busy. */
    PANEL(3, H - 46, 122, 43);
    cp_px_text(fb, W, H, 6, H - 43, 1, "POPULATION", 0.78f, 0.92f, 0.62f, 1.0f);
    snprintf(buf, sizeof(buf), "N %-3d GEN %.1f", w->pop, (double)w->mean_gen);
    cp_px_text(fb, W, H, 6, H - 34, 1, buf, 0.74f, 0.80f, 0.66f, 1.0f);
    snprintf(buf, sizeof(buf), "BIRTH %-4d DIE %d", w->births, w->deaths);
    cp_px_text(fb, W, H, 6, H - 26, 1, buf, 0.64f, 0.70f, 0.58f, 1.0f);
    snprintf(buf, sizeof(buf), "LEGS %.1f CHARM %.2f",
             (double)w->mean_legs, (double)w->mean_charm);
    cp_px_text(fb, W, H, 6, H - 18, 1, buf, 0.60f, 0.84f, 0.62f, 1.0f);
    snprintf(buf, sizeof(buf), "WON %-3d ATE %d", w->befriended, w->kills);
    cp_px_text(fb, W, H, 6, H - 10, 1, buf, 0.86f, 0.72f, 0.44f, 1.0f);

    /* The world has no edges, so how far you have gone and what you have found
     * out there is a score in its own right. */
    {
        int mine = 0;
        for (int i = 0; i < CP4_MAX_BEASTS; i++)
            if (w->beast[i].alive && w->beast[i].nest == CP4_OWN_NEST) mine++;
        snprintf(buf, sizeof(buf), "ROAM %d", (int)w->travelled);
        cp_px_text(fb, W, H, 130, H - 10, 1, buf, 0.60f, 0.78f, 0.86f, 1.0f);
        snprintf(buf, sizeof(buf), "FOUND %d", w->discovered);
        cp_px_text(fb, W, H, 130, H - 18, 1, buf, 0.60f, 0.78f, 0.86f, 1.0f);
        if (w->home.alive) {
            snprintf(buf, sizeof(buf), "NEST %d KIN %d", (int)w->home.store, mine);
            cp_px_text(fb, W, H, 130, H - 26, 1, buf, 0.90f, 0.80f, 0.50f, 1.0f);
        }
    }

    /* own build */
    PANEL(W - 128, H - 30, 108, 27);
    snprintf(buf, sizeof(buf), "%dDNA %dP %dSEG",
             (int)p->s.cost, p->s.n_parts, p->g.nseg);
    cp_px_text(fb, W, H, W - 125, H - 27, 1, buf, 0.66f, 0.78f, 0.54f, 1.0f);
    int col = 0;
    for (int t = 1; t < CP4_PART_COUNT; t++) {
        int n = p->s.n[t];
        if (!n) continue;
        float er;
        V3 cl = part_albedo4(t, &er);
        int x = W - 125 + (col % 6) * 17, y = H - 17 + (col / 6) * 8;
        cp_px_rect(fb, W, H, x, y, 5, 5, cl.x, cl.y, cl.z, 1.0f);
        snprintf(buf, sizeof(buf), "%d", n);
        cp_px_text(fb, W, H, x + 7, y - 1, 1, buf, 0.82f, 0.86f, 0.76f, 1.0f);
        col++;
    }

    if (w->status != CP4_RUN) {
        const char *msg = w->status == CP4_EVOLVED ? "EVOLVE - TRIBE"
                        : w->status == CP4_DEAD    ? "KILLED" : "TIME UP";
        int tw = cp_px_text_w(msg, 1);
        int bx = (W - tw) / 2, by = H / 2 - 7;
        PANEL(bx - 6, by - 4, tw + 12, 16);
        cp_px_text(fb, W, H, bx, by, 1, msg,
                   w->status == CP4_EVOLVED ? 0.62f : 1.0f,
                   w->status == CP4_EVOLVED ? 0.94f : 0.46f, 0.52f, 1.0f);
    }
    #undef PANEL
    #undef BAR
}

/* ---------------- entry ---------------- */

void cp4_render_styled(const Cp4World *w, uint8_t *rgba, int OW, int OH, int style)
{
    int32_t lw = 320, lh = 180;
    cp_vis_dims(style, &lw, &lh);
    if (lw > MAXW) lw = MAXW;
    if (lh > MAXH) lh = MAXH;

    uint8_t *fb = (uint8_t *)malloc((size_t)lw * lh * 4);
    float   *zb = (float *)malloc(sizeof(float) * (size_t)lw * lh);
    if (!fb || !zb) { free(fb); free(zb); return; }

    const Cp4Beast *p = &w->player;
    V3 pf, pr, pu;
    basis3(p->yaw, 0.0f, &pf, &pr, &pu);

    Ctx c;
    c.fb = fb; c.zb = zb; c.W = lw; c.H = lh;
    c.focal = (float)lw * 0.70f;   /* wide: this stage is about the view */
    c.hazek = 1.0f;
    c.seed = w->seed;
    c.time = (float)w->step * CP4_DT;

    /* Chase camera: behind and above, looking slightly down. The player grows
     * across four generations, so the camera backs off with it.
     *
     * The aim point sits a long way in front of the animal on purpose. Aiming
     * at the player instead puts the whole downward tilt into the shot and
     * shoves the horizon off the top of the frame, so the picture becomes a
     * study of dirt. Looking well ahead flattens the pitch to a few degrees
     * and lets the land actually have a skyline. */
    float back = p->s.length * 2.7f + p->s.stand * 3.4f + 62.0f;
    float lift = p->s.stand * 1.7f + 27.0f;

    if (p->medium == CP4_UNDER) {
        /* A burrow is a tight place, so the camera follows the animal into it
         * rather than hanging in the air above. Keeping it above ground while
         * shading the frame as soil is what made the first underground shot a
         * flat grey field: every ray escaped into daylight immediately. */
        back *= 0.36f;   /* a burrow is a tight place */
        /* At the animal's own depth, not just under the surface: a camera
         * pinned to the roof looks down a shaft at nothing. */
        c.eye = add(cv(p->p), mul(pf, -back));
        c.eye.y = p->p.y - p->s.stand * 0.30f;
        float g = cp4_height(w->seed, c.eye.x, c.eye.z);
        if (c.eye.y < g + 4.0f) c.eye.y = g + 4.0f;      /* stay in the soil */
    } else {
        /* a touch off the centre line: dead astern shows the player its own
         * backside and hides every part mounted on the flanks */
        c.eye = add(add(add(cv(p->p), mul(pf, -back)), mul(pu, lift)),
                    mul(pr, back * 0.20f));
        /* never let the camera end up inside a hill */
        float g = cp4_height(w->seed, c.eye.x, c.eye.z) - 14.0f;
        if (c.eye.y > g) c.eye.y = g;
        if (c.eye.y < -CP4_SKY) c.eye.y = -CP4_SKY;
    }

    /* The view mode follows the eye, not the animal. Deriving it from the
     * player's medium instead let the two disagree - the shading said "soil"
     * while the camera sat in the open air twenty units above the surface. */
    {
        float ge = cp4_height(w->seed, c.eye.x, c.eye.z);
        c.buried    = c.eye.y > ge;
        c.submerged = !c.buried && ge > CP4_SEA && c.eye.y > CP4_SEA;
    }

    V3 look;
    if (p->medium == CP4_UNDER)
        look = norm(sub(add(cv(p->p), mul(pf, 40.0f)), c.eye));
    else
        look = norm(sub(add(cv(p->p), mul(pf, 170.0f)), c.eye));
    c.fwd = look;
    c.right = norm(v3(-look.z, 0.0f, look.x));
    c.up = norm(v3(c.right.z * look.y - c.right.y * look.z,
                   c.right.x * look.z - c.right.z * look.x,
                   c.right.y * look.x - c.right.x * look.y));

    draw_world(&c, w);
    draw_nests(&c, w);
    draw_home(&c, w);
    draw_flora(&c, w);
    for (int i = 0; i < CP4_MAX_BEASTS; i++)
        if (w->beast[i].alive) draw_creature(&c, &w->beast[i], 0);
    draw_creature(&c, p, 1);

    outline_pass(&c);
    draw_hud4(fb, lw, lh, w);
    cp_vis_quantise(fb, lw, lh, style);
    cp_vis_blit(fb, lw, lh, rgba, OW, OH, style);

    free(fb);
    free(zb);
}

void cp4_render(const Cp4World *w, uint8_t *rgba, int W, int H)
{
    cp4_render_styled(w, rgba, W, H, CP_VIS_ABYSS);
}

/* ---------------- portrait ----------------
 * A genome space is only as good as the variety you can see in it, so this
 * renders one creature against a plain backdrop with nothing else in frame. */
void cp4_render_portrait(const Cp4Genome *g, uint8_t *fb, int lw, int lh,
                         int style, uint32_t seed)
{
    float *zb = (float *)malloc(sizeof(float) * (size_t)lw * lh);
    if (!zb) return;

    Cp4Beast b;
    memset(&b, 0, sizeof(b));
    b.g = *g;
    cp4_genome_stats(&b.g, &b.s);
    b.hp = b.hp_max = b.s.hp_max;
    b.alive = 1;
    b.p.x = 0.0f; b.p.y = -b.s.stand; b.p.z = 0.0f;
    b.yaw = 0.0f;
    b.phase = (float)(seed % 128) * 0.05f;

    Prim pr[MAX_PRIM];
    Skin sk;
    V3 centre;
    float bound;
    int n = build_prims4(&b, 0, pr, &centre, &bound, &sk);

    Ctx c;
    c.fb = fb; c.zb = zb; c.W = lw; c.H = lh;
    c.focal = (float)lw * 1.15f;
    c.hazek = 0.0f;
    c.seed = seed;
    c.time = 0.0f;
    c.submerged = c.buried = 0;
    /* Three-quarter view, but only barely raised. Straight side-on hides
     * everything mounted fore and aft; look down more than about ten degrees
     * and the body hides the legs, which on a land animal is most of what
     * there is to see. */
    V3 dir = norm(v3(-0.66f, -0.15f, 0.74f));
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
        V3 bg = v3(mixf(0.10f, 0.20f, t), mixf(0.14f, 0.22f, t), mixf(0.20f, 0.16f, t));
        for (int x = 0; x < lw; x++) {
            put(&c, x, y, bg);
            zb[(size_t)y * lw + x] = 1e30f;
        }
    }
    /* A ground plane at y = 0, which is exactly where the legs reach. Without
     * it the animal floats and the whole point of the leg rig is invisible. */
    for (int y = 0; y < lh; y++) {
        for (int x = 0; x < lw; x++) {
            float px = ((float)x + 0.5f - lw * 0.5f) / c.focal;
            float py = -((float)y + 0.5f - lh * 0.5f) / c.focal;
            V3 ray = norm(add(add(mul(c.right, px), mul(c.up, py)), c.fwd));
            if (ray.y <= 0.001f) continue;
            float t = (0.0f - c.eye.y) / ray.y;
            if (t <= 0.0f || t > 4000.0f) continue;
            V3 h = add(c.eye, mul(ray, t));
            float r = sqrtf(h.x * h.x + h.z * h.z);
            float fade = clampf(1.0f - r / (bound * 3.4f), 0.0f, 1.0f);
            if (fade <= 0.0f) continue;
            float g = 0.24f + 0.08f * rnoise(seed, h.x * 0.25f, h.z * 0.25f);
            V3 gc = v3(g * 0.92f, g * 1.06f, g * 0.66f);
            uint8_t *pp = fb + 4 * ((size_t)y * lw + x);
            V3 cur = v3(pp[0] / 255.0f, pp[1] / 255.0f, pp[2] / 255.0f);
            put(&c, x, y, v3(mixf(cur.x, gc.x, fade), mixf(cur.y, gc.y, fade),
                             mixf(cur.z, gc.z, fade)));
            zb[(size_t)y * lw + x] = t * dot(ray, c.fwd);
        }
    }

    march_creature(&c, pr, n, centre, bound, &sk);
    outline_pass(&c);
    cp_vis_quantise(fb, lw, lh, style);
    free(zb);
}
