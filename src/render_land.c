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
#define MAXW 640
#define MAXH 360

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
    V3       sun;        /* where the light comes from this frame */
    float    day;        /* 0 at midnight, 1 at noon */
    /* Where the camera itself is. The stage has four media and three of them
     * look nothing like standing on a hill, so the background, the fog colour
     * and the fog distance all switch on these. */
    int      submerged, buried;
} Ctx;

/* Direction the light travels. y is down, so a sun in the sky sends light
 * along +y - the same convention the aquatic stage settled on.
 *
 * It is no longer a constant: the sun rises, crosses and sets. Keeping the
 * vector convention means every shading term below took the change without
 * being rewritten. */
static V3 sun_dir(int32_t step)
{
    float a = cp4_sun_angle(step);
    float el = -cosf(a);                 /* -1 at midnight, +1 at noon */
    V3 d = v3(0.42f * sinf(a), 0.35f + 0.75f * el, -0.30f * cosf(a));
    return norm(d);
}

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
 * one is the same pale grey, which is exactly what the first build did.
 *
 * These are pre-tonemap, and the blue runs well past 1.0 on purpose. The
 * filmic curve compresses the top of the range hard, so a horizon written as
 * a legal 0.85 comes out of it around 0.67 - a middling grey-cyan, and the
 * quantiser then has to choose between a rock entry and a sky entry on almost
 * equal terms. Driving the channels apart before the curve is what keeps the
 * horizon on the sky ramp after it. */
static V3 horizon_col(void) { return v3(0.60f, 1.05f, 2.10f); }


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
    /* A real sky blue now that the land palette has six steps of it to land
     * on. The aquatic stage's palette had a cyan ramp, a violet ramp and
     * nothing between, so a plausible blue quantised to lavender and the sky
     * had to be written cyan to survive; that constraint is gone.
     *
     * The exponent decides how much of the frame is horizon. Above about 0.5
     * the pale band creeps up into the top of the shot and the sky stops
     * having a gradient at all. */
    V3 zen = v3(0.15f, 0.25f, 0.55f);
    V3 hor = horizon_col();
    float t = powf(up, 0.40f);
    V3 col = v3(mixf(hor.x, zen.x, t), mixf(hor.y, zen.y, t), mixf(hor.z, zen.z, t));

    /* Night. The palette has no true black to spare, so the night sky is a
     * deep violet-blue rather than nothing - which is also what a real one
     * looks like once your eyes have adjusted. */
    {
        V3 nzen = v3(0.030f, 0.035f, 0.115f);
        V3 nhor = v3(0.070f, 0.085f, 0.150f);
        V3 ncol = v3(mixf(nhor.x, nzen.x, t), mixf(nhor.y, nzen.y, t),
                     mixf(nhor.z, nzen.z, t));
        /* the last of the light piles up low in the sky */
        float dusk = clampf(1.0f - fabsf(c->day - 0.32f) / 0.32f, 0.0f, 1.0f);
        float lowband = powf(clampf(1.0f - up * 3.2f, 0.0f, 1.0f), 2.0f);
        ncol = add(ncol, mul(v3(0.62f, 0.28f, 0.20f), dusk * lowband * 0.55f));
        col = v3(mixf(ncol.x, col.x, c->day), mixf(ncol.y, col.y, c->day),
                 mixf(ncol.z, col.z, c->day));

        /* Stars, fixed to the sky rather than the screen. Hashing the ray
         * direction is what keeps them still while the camera turns. */
        if (c->day < 0.55f && up > 0.02f) {
            float sx = ray.x / (up + 0.35f), sz = ray.z / (up + 0.35f);
            float h = rhash(c->seed ^ 0x5741u, (int)floorf(sx * 190.0f),
                            (int)floorf(sz * 190.0f));
            if (h > 0.9955f) {
                float tw = 0.6f + 0.4f * sinf(c->time * 2.3f + h * 400.0f);
                float k = (1.0f - c->day / 0.55f) * tw;
                col = add(col, v3(0.75f * k, 0.80f * k, 0.95f * k));
            }
        }
    }

    /* sun disc and its glow */
    V3 tosun = mul(c->sun, -1.0f);
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
            /* Bright enough to still read as cloud once the filmic curve has
             * had the sky - written at 0.96 they came out darker than the
             * horizon behind them and the sky grew grey holes. */
            V3 cl = mul(v3(1.60f, 1.62f, 1.70f), 0.25f + 0.75f * c->day);
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
    if (c->buried)    { *dist_out = 62.0f;   *cap = 0.88f; return v3(0.055f, 0.038f, 0.026f); }
    if (c->submerged) { *dist_out = 300.0f;  *cap = 0.95f; return v3(0.055f, 0.185f, 0.235f); }
    *dist_out = 3000.0f; *cap = 0.80f;
    {
        V3 d = horizon_col(), n = v3(0.075f, 0.090f, 0.160f);
        return v3(mixf(n.x, d.x, c->day), mixf(n.y, d.y, c->day),
                  mixf(n.z, d.z, c->day));
    }
}

/* Aerial perspective.
 *
 * Distance used to be a lerp toward one horizon colour, which is why every
 * ridge past a kilometre came out the same flat grey and the landscape ended
 * in a wall. Air does two things instead, and doing them separately is what
 * makes a far mountain read as far rather than as washed out:
 *
 *   extinction   the ground's own colour is absorbed on the way to the eye,
 *                and blue is absorbed least - so what survives from a long way
 *                off is blue
 *   in-scatter   sunlight bounces into the beam along its whole length. Away
 *                from the sun that light is blue; toward it, forward
 *                scattering piles it up into a warm haze, which is the whole
 *                reason a sunset behind a range looks like anything
 *
 * Both are per-channel, both scale with the sun, and neither needs anything
 * the renderer does not already know. */
static V3 aerial(const Ctx *c, V3 col, float dist, V3 ray)
{
    if (c->hazek <= 0.0f) return col;

    /* water and soil are not air - short range, single colour, no sun */
    if (c->buried || c->submerged) {
        float fd, cap;
        V3 h = medium_fog(c, &fd, &cap);
        float near = c->buried ? 2.0f : 25.0f;
        float d = dist - near;
        if (d < 0.0f) d = 0.0f;
        float f = (1.0f - expf(-d / fd)) * cap * c->hazek;
        return v3(mixf(col.x, h.x, f), mixf(col.y, h.y, f), mixf(col.z, h.z, f));
    }

    /* Nothing within a couple of hundred units is scattering anything worth
     * seeing. Starting sooner tinted the hillside the animal is standing on. */
    float d = dist - 190.0f;
    if (d < 0.0f) d = 0.0f;

    /* Extinction. The three scale lengths keep Rayleigh's ratio - blue
     * survives furthest - but they are set against this world's view distance,
     * not against a real atmosphere's. Written long enough to be physical they
     * do almost nothing over the two and a half kilometres the marcher can
     * see, and the far coast came back as a black bar across the horizon:
     * ninety percent of its own near-black albedo, delivered intact. */
    float tr = expf(-d / 2700.0f);
    float tg = expf(-d / 2000.0f);
    float tb = expf(-d / 1300.0f);

    V3 tosun = mul(c->sun, -1.0f);
    float cosa = clampf(dot(ray, tosun), 0.0f, 1.0f);
    /* Mie forward lobe: a bright halo in the sun's direction, falling off fast */
    float mie = powf(cosa, 7.0f);
    /* Rayleigh is nearly isotropic, so it just fills in everywhere */
    float ray_amt = 0.75f + 0.25f * cosa * cosa;

    V3 blue = v3(0.34f, 0.52f, 0.86f);
    V3 warm = v3(1.00f, 0.78f, 0.52f);
    float lit = 0.10f + 0.90f * c->day;

    V3 sc = v3((blue.x * ray_amt + warm.x * mie * 1.6f) * lit,
               (blue.y * ray_amt + warm.y * mie * 1.6f) * lit,
               (blue.z * ray_amt + warm.z * mie * 1.6f) * lit);

    /* how much light has been scattered into the beam by this distance */
    float sr = 1.0f - tr, sg = 1.0f - tg, sb = 1.0f - tb;
    return v3(col.x * tr + sc.x * sr,
              col.y * tg + sc.y * sg,
              col.z * tb + sc.z * sb);
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
        float h = q.y - cp4_surface(c->seed, q.x, q.z); /* > 0 means below the surface */
        if (h > 0.0f) {
            /* One linear guess is not enough: by 2km the step is tens of units
             * wide and the error shows up as horizontal terraces across every
             * distant slope. Four bisections cost almost nothing and remove
             * them. */
            float lo = t - dt, hi = t;
            for (int k = 0; k < 4; k++) {
                float mid = 0.5f * (lo + hi);
                V3 m = add(ro, mul(rd, mid));
                if (m.y - cp4_surface(c->seed, m.x, m.z) > 0.0f) hi = mid;
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

/* Ambient occlusion on the heightfield.
 *
 * Creatures have had this since stage 2 and the ground has not, and it shows:
 * without it a gully and a ridge crest with the same surface normal are
 * painted the same colour, so the land reads as a green sheet with a lighting
 * gradient over it rather than as terrain. Direct light does not fix that -
 * the shadow ray only knows about the sun, and at noon almost nothing is in
 * shadow. What separates a valley from a ridge is how much *sky* it can see.
 *
 * Four rings, four samples each, asking how far each neighbour rises above the
 * tangent plane at q. A crest has nothing above it and stays open; a hollow is
 * walled in on every side and closes down. The rings grow quadratically so the
 * same sixteen samples cover both the ditch underfoot and the valley wall a
 * hundred units away. */
static float terrain_ao(const Ctx *c, V3 q, V3 n)
{
    static const float DX[4] = {  0.92f, -0.85f,  0.20f, -0.31f };
    static const float DZ[4] = {  0.39f,  0.53f, -0.98f,  0.95f };
    float occ = 0.0f, wsum = 0.0f;
    for (int i = 1; i <= 4; i++) {
        float r = 7.0f * (float)(i * i);          /* 7, 28, 63, 112 units */
        float w = 1.0f / (float)i;
        for (int k = 0; k < 4; k++) {
            float sx = q.x + DX[k] * r, sz = q.z + DZ[k] * r;
            /* y is down, so a neighbour standing higher has the smaller y and
             * the vector to it leans along the up-pointing normal */
            V3 v = v3(sx - q.x, cp4_height(c->seed, sx, sz) - q.y, sz - q.z);
            float d = dot(n, v);
            occ  += w * clampf(d / r, 0.0f, 1.0f);
            wsum += w;
        }
    }
    return clampf(1.0f - 1.35f * occ / wsum, 0.25f, 1.0f);
}

/* One shadow ray over the heightfield. Coarse on purpose - hills casting long
 * shadows is worth far more to readability than the shadows being exact. */
static float terrain_shadow(const Ctx *c, V3 q)
{
    V3 l = mul(c->sun, -1.0f);
    float res = 1.0f, t = 6.0f, dt = 7.0f;
    for (int i = 0; i < 18; i++) {
        V3 s = add(q, mul(l, t));
        if (s.y < -PEAK) break;
        /* clearance above the ground; negative means the ray is inside a hill */
        float clr = cp4_height(c->seed, s.x, s.z) - s.y;
        if (clr < 0.0f) return 0.30f;
        /* Penumbra. A hard in-or-out test drew every shadow edge as a single
         * hard line cutting across the grass, which at this palette depth
         * reads as a seam in the ground rather than as a shadow. How close the
         * ray passed to the ridge, divided by how far away that ridge was, is
         * the angle it subtends - which is the softness. */
        float k = 1.9f * clr / t;
        if (k < res) res = k;
        t += dt;
        dt *= 1.28f;
    }
    return clampf(res, 0.30f, 1.0f);
}

/* Ground colour from elevation and slope. Sand at the waterline, rock where it
 * is too steep to hold soil, pale scree on the peaks, and grass in between
 * with its hue drifting on a low-frequency noise so the map has regions
 * instead of one uniform green. */
/* Ground colour.
 *
 * Sand at the waterline, rock where it is too steep to hold soil, and
 * otherwise whatever the biome says. Blending between two biome colours on the
 * temperature/moisture values rather than switching on the banded index is
 * what stops the map looking like a political cartogram: the boundary between
 * savanna and desert should be a gradient you walk through, not a line. */
static V3 biome_colour(int b, float band)
{
    /* each biome as a low/high pair, mixed on elevation */
    V3 lo, hi;
    switch (b) {
    case CP4_BIOME_ICE:     lo = v3(0.62f, 0.68f, 0.74f); hi = v3(0.80f, 0.84f, 0.88f); break;
    case CP4_BIOME_TUNDRA:  lo = v3(0.36f, 0.36f, 0.30f); hi = v3(0.52f, 0.54f, 0.52f); break;
    case CP4_BIOME_TAIGA:   lo = v3(0.13f, 0.24f, 0.17f); hi = v3(0.22f, 0.30f, 0.24f); break;
    case CP4_BIOME_FOREST:  lo = v3(0.10f, 0.26f, 0.09f); hi = v3(0.18f, 0.34f, 0.14f); break;
    case CP4_BIOME_GRASS:   lo = v3(0.17f, 0.31f, 0.11f); hi = v3(0.28f, 0.38f, 0.16f); break;
    case CP4_BIOME_SAVANNA: lo = v3(0.36f, 0.32f, 0.13f); hi = v3(0.46f, 0.40f, 0.19f); break;
    case CP4_BIOME_DESERT:  lo = v3(0.56f, 0.44f, 0.24f); hi = v3(0.66f, 0.54f, 0.32f); break;
    default:                lo = v3(0.09f, 0.24f, 0.08f); hi = v3(0.14f, 0.32f, 0.11f); break;
    }
    return v3(mixf(lo.x, hi.x, band), mixf(lo.y, hi.y, band), mixf(lo.z, hi.z, band));
}

static V3 ground_albedo(const Ctx *c, V3 q, float slope, float dist)
{
    float elev = -q.y;

    /* Below the waterline the land ramp is simply wrong - it paints a drowned
     * seabed as dry grassland, which is what made the first underwater shot a
     * flat brown sheet. Silt and sand instead, paling as it shallows. */
    if (elev < -CP4_SEA) {
        float d = clampf((-elev - CP4_SEA) / 90.0f, 0.0f, 1.0f);
        V3 silt = v3(mixf(0.52f, 0.20f, d), mixf(0.48f, 0.22f, d), mixf(0.36f, 0.20f, d));
        float ripple = 0.86f + 0.28f * rnoise(c->seed ^ 0x6Bu, q.x * 0.035f, q.z * 0.02f);
        return mul(silt, ripple);
    }

    float band = clampf((elev + 20.0f) / 150.0f, 0.0f, 1.0f);
    V3 col = biome_colour(cp4_biome(c->seed, q.x, q.z), band);

    /* Patchiness inside a biome, so a meadow is not one flat green. Two
     * octaves rather than one: a single low frequency is a slow wash the eye
     * reads as lighting, and it takes the finer one on top before the ground
     * reads as ground. */
    float patch = rnoise(c->seed ^ 0x91u, q.x * 0.0065f, q.z * 0.0065f);
    float clump = rnoise(c->seed ^ 0xA3u, q.x * 0.024f, q.z * 0.024f);
    col = mul(col, 0.78f + 0.30f * patch + 0.18f * clump);

    V3 rock = v3(0.25f, 0.23f, 0.21f);
    float rocky = clampf((0.80f - slope) * 4.0f, 0.0f, 1.0f);
    col = v3(mixf(col.x, rock.x, rocky), mixf(col.y, rock.y, rocky),
             mixf(col.z, rock.z, rocky));

    /* Beach. The band is wider than the tideline on purpose: at this scale a
     * strictly correct one is a pixel high and the coast loses its edge. */
    V3 sand = v3(0.52f, 0.45f, 0.29f);
    float shore = clampf(1.0f - fabsf(elev + CP4_SEA) / 22.0f, 0.0f, 1.0f);
    col = v3(mixf(col.x, sand.x, shore), mixf(col.y, sand.y, shore),
             mixf(col.z, sand.z, shore));

    /* Wet sand: the strip the water has just left. Sand darkens and saturates
     * when it is wet, which is a two-line change that does more to seat the
     * waterline than the foam on the other side of it does. */
    {
        float wet = clampf(1.0f - (elev + CP4_SEA) / 7.0f, 0.0f, 1.0f)
                  * clampf((elev + CP4_SEA) / 1.5f + 1.0f, 0.0f, 1.0f);
        col = mul(col, 1.0f - 0.40f * wet);
        col.z *= 1.0f + 0.22f * wet;
    }

    /* snow caps everything high enough, whatever biome it stands in */
    V3 snow = v3(0.74f, 0.78f, 0.82f);
    float high = clampf((elev - CP4_SNOWLINE) / 42.0f, 0.0f, 1.0f);
    col = v3(mixf(col.x, snow.x, high), mixf(col.y, snow.y, high),
             mixf(col.z, snow.z, high));

    /* Fine speckle, so a flat field is not a flat colour - but it has to fade
     * out with distance. One sample per pixel of an 0.08-frequency noise turns
     * into salt and pepper the moment a pixel covers more than a few units. */
    float near = clampf(1.0f - dist / 420.0f, 0.0f, 1.0f);
    float g = 1.0f + 0.20f * (rnoise(c->seed ^ 0x5Fu, q.x * 0.08f, q.z * 0.08f) - 0.5f) * near;
    return mul(col, g);
}

#define SCEN_CELL   78.0f
#define SCEN_RANGE  900.0f       /* past this a tree is a couple of pixels */

enum { SCEN_NONE = 0, SCEN_CONIFER, SCEN_BROADLEAF, SCEN_ROCK };

typedef struct {
    int   kind;
    float x, z, y;               /* y is the ground it stands on            */
    float h;                     /* height for a tree, radius for a rock    */
    float r1, r2, r3;            /* the cell's own dice, for shape and hue  */
    int   biome;
} Scen;

/* What, if anything, stands in one cell. Split out of the draw so the ground
 * pass can ask the same question and put a shadow under the answer - a tree
 * that casts nothing sits on the grass like a sticker. */
static int scenery_at(const Ctx *c, int cx, int cz, Scen *s)
{
    float r0 = rhash(c->seed ^ 0x1F17u, cx, cz);
    s->r1 = rhash(c->seed ^ 0x77A3u, cx, cz);
    s->r2 = rhash(c->seed ^ 0x3D5Bu, cx, cz);
    s->r3 = rhash(c->seed ^ 0x6E21u, cx, cz);

    s->x = ((float)cx + 0.15f + 0.70f * s->r1) * SCEN_CELL;
    s->z = ((float)cz + 0.15f + 0.70f * s->r2) * SCEN_CELL;
    s->y = cp4_height(c->seed, s->x, s->z);
    if (s->y > CP4_SEA - 6.0f) return 0;            /* nothing grows in the sea */

    s->biome = cp4_biome(c->seed, s->x, s->z);

    /* how crowded this biome is, and what it grows */
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
    default:                density = 0.14f; kind = SCEN_ROCK; break;   /* ice */
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
    /* steep ground holds rock, not roots */
    {
        float nx, ny, nz;
        cp4_normal(c->seed, s->x, s->z, &nx, &ny, &nz);
        if (clampf(-ny, 0.0f, 1.0f) < 0.80f) { kind = SCEN_ROCK; density *= 1.4f; }
    }
    if (r0 > density) return 0;

    /* Two size classes rather than one uniform range. A stand where every
     * trunk is within a factor of two of every other reads as a crop; real
     * cover is a few tall ones with everything else coming up underneath. */
    float scale = s->r3 > 0.82f ? (1.35f + 0.55f * s->r1) : (0.50f + 0.62f * s->r3);

    s->kind = kind;
    s->h = kind == SCEN_ROCK ? (5.0f + 9.0f * s->r1) * scale
                             : (26.0f + 24.0f * s->r1) * scale;
    return 1;
}

/* A window of scenery cells around the camera, built once a frame.
 *
 * The ground pass needs to know what is standing over each point it shades,
 * and asking the cell hash per pixel would mean re-deriving height, biome and
 * normal nine times for every square of grass. Deriving the window once and
 * looking into it is the same answer for a fraction of the work, and it costs
 * nothing in fidelity because the cells are a pure function of position. */
#define SCEN_WIN 15

typedef struct {
    int           cx0, cz0;
    unsigned char have[SCEN_WIN * SCEN_WIN];
    Scen          s[SCEN_WIN * SCEN_WIN];
} ScenGrid;

static void scen_grid_build(const Ctx *c, ScenGrid *g)
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
 * Without this a wood is a set of stickers on a lawn - the canopy is the
 * single biggest thing shading a forest floor, and the terrain shadow ray
 * knows nothing about it because the trees are not in the heightfield. */
static float scen_shade(const ScenGrid *g, float x, float z)
{
    int ci = (int)floorf(x / SCEN_CELL) - g->cx0;
    int cj = (int)floorf(z / SCEN_CELL) - g->cz0;
    float k = 0.0f;
    for (int dj = -1; dj <= 1; dj++) {
        int j = cj + dj;
        if ((unsigned)j >= (unsigned)SCEN_WIN) continue;
        for (int di = -1; di <= 1; di++) {
            int i = ci + di;
            if ((unsigned)i >= (unsigned)SCEN_WIN) continue;
            int n = j * SCEN_WIN + i;
            if (!g->have[n]) continue;
            const Scen *s = &g->s[n];
            float rad = s->kind == SCEN_ROCK ? s->h * 1.6f : s->h * 0.40f;
            float dx = x - s->x, dz = z - s->z;
            float d2 = (dx * dx + dz * dz) / (rad * rad);
            if (d2 >= 1.0f) continue;
            float f = (1.0f - d2) * (1.0f - d2);
            if (f > k) k = f;
        }
    }
    return k;
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
    ScenGrid *sg = (ScenGrid *)malloc(sizeof(ScenGrid));
    if (sg) scen_grid_build(c, sg);
    V3 sun = c->sun;
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
                /* Depth is folded into the noise coordinates. Sampled on x
                 * and z alone every pixel in a column reads nearly the same
                 * soil, so the only thing left varying across the frame was
                 * the distance falloff - and a burrow lit purely by distance
                 * comes out as a set of concentric rings, like a target. */
                float grain = fbm2(c->seed ^ 0x7Du, q.x * 0.11f + q.y * 0.09f,
                                   q.z * 0.11f - q.y * 0.07f)
                            + 0.35f * rnoise(c->seed ^ 0x11u, q.x * 0.55f + q.y * 0.4f,
                                             q.z * 0.55f - q.y * 0.3f);
                /* Falloff on the *tunnel* wall, not on the whole thickness of
                 * the hill. There is no tunnel geometry, so an unclamped exit
                 * distance meant most rays travelled two hundred units of
                 * solid earth, and the burrow rendered as a black rectangle
                 * with a HUD on it. Clamping the distance is the same thing as
                 * saying the wall is never further away than a burrow is wide. */
                float lit_d = wall > 46.0f ? 46.0f : wall;
                float lampl = 0.30f + 0.70f * expf(-lit_d / 26.0f);
                float e = (0.13f + 0.34f * grain) * lampl;
                V3 col = v3(e * 1.25f, e * 0.84f, e * 0.52f);

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

            /* Water is no longer a plane laid over the world. The marcher
             * stops at whichever surface is uppermost - waterline or ground -
             * and the two come out of one evaluation, so a lake, a coast and a
             * river bend are the same case and there is no plane to intersect
             * separately. The sea used to be a plane and rivers simply could
             * not exist under that model. */
            float tw = tg, wdepth = 0.0f;
            int water = 0;
            if (hit) {
                V3 h = add(c->eye, mul(ray, tg));
                float wl, gr = cp4_height_water(c->seed, h.x, h.z, &wl);
                if (gr - wl > 0.30f) { water = 1; wdepth = gr - wl; }
            }

            V3 col;
            float dist;
            if (water) {
                V3 q = add(c->eye, mul(ray, tw));
                /* two crossed ripples for the surface normal */
                /* Ripples have to die off with distance. At a kilometre one
                 * pixel spans several wavelengths, so a full-amplitude normal
                 * samples the wave at whatever phase it lands on and the far
                 * water breaks up into horizontal streaks. */
                float ramp = clampf(1.0f - tw / 700.0f, 0.10f, 1.0f);
                float rx = (sinf(q.x * 0.06f + c->time * 1.7f) * 0.045f
                         + sinf((q.x + q.z) * 0.031f - c->time * 1.1f) * 0.030f) * ramp;
                float rz = (sinf(q.z * 0.052f - c->time * 1.4f) * 0.045f
                         + cosf((q.x - q.z) * 0.028f + c->time * 0.9f) * 0.030f) * ramp;
                V3 n = norm(v3(rx, -1.0f, rz));
                V3 refl = sub(ray, mul(n, 2.0f * dot(ray, n)));
                V3 sky = sky_col(c, norm(refl));

                /* depth of water at this point tints what shows through */
                float depth = clampf(wdepth, 0.0f, 90.0f);
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

                /* Surf. A coastline drawn as a plane meeting a slope is a
                 * clean geometric edge, and a clean edge is the one thing no
                 * real shore has - it is the white line that tells you where
                 * the land stops. Foam rides the shallows, thickest where the
                 * bottom is nearly at the surface, and is broken up by a noise
                 * band that crawls so it reads as water rather than as paint. */
                {
                    /* Kept to a narrow band. Judged on shallowness alone it
                     * whitened every sandbar and tidal flat in the world, and
                     * a lagoon came out as a sheet of milk. */
                    float shal = clampf(1.0f - depth / 11.0f, 0.0f, 1.0f);
                    float band = rnoise(c->seed ^ 0x2Du, q.x * 0.055f + c->time * 0.35f,
                                        q.z * 0.055f - c->time * 0.22f);
                    float foam = shal * shal * (0.30f + 0.85f * band);
                    /* the last couple of units are solid wash */
                    foam += clampf(1.0f - depth / 2.5f, 0.0f, 1.0f) * 0.55f;
                    foam = clampf(foam, 0.0f, 1.0f) * (0.30f + 0.70f * c->day);
                    V3 wash = v3(0.95f, 1.04f, 1.10f);
                    col = v3(mixf(col.x, wash.x, foam), mixf(col.y, wash.y, foam),
                             mixf(col.z, wash.z, foam));
                }
                dist = tw;
            } else if (hit) {
                V3 q = add(c->eye, mul(ray, tg));
                float nx, ny, nz;
                cp4_normal(c->seed, q.x, q.z, &nx, &ny, &nz);
                V3 n = v3(nx, ny, nz);
                float slope = clampf(-ny, 0.0f, 1.0f);

                V3 alb = ground_albedo(c, q, slope, tg);
                /* What the canopy takes out of the sky above this patch. The
                 * query is pushed toward the sun first: a tree's shadow lies
                 * where the light does not reach, not in a ring under the
                 * trunk, and at low sun that is most of the shadow's length. */
                float canopy = 0.0f;
                if (sg && tg < 640.0f) {
                    float drop = -tosun.y < 0.15f ? 0.15f : -tosun.y;
                    float reach = 24.0f / drop;
                    if (reach > 90.0f) reach = 90.0f;
                    canopy = scen_shade(sg, q.x + tosun.x * reach, q.z + tosun.z * reach);
                }
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
                /* Sixteen height samples a pixel is not free, so it stops
                 * where it stops paying: past a few hundred units a valley is
                 * a handful of pixels wide and the aerial perspective is
                 * already doing the separating. */
                float ao = tg < 620.0f ? terrain_ao(c, q, n) : 1.0f;
                ao *= 1.0f - 0.55f * canopy;
                sh *= 1.0f - 0.62f * canopy;
                /* sky light lands on what faces up; a warm bounce comes back
                 * off everything else */
                float skyamt = clampf(slope, 0.0f, 1.0f);
                float night = 1.0f - c->day;
                V3 lightv = add(add(mul(v3(1.00f, 0.94f, 0.80f),
                                        1.00f * lam * sh * (0.10f + 0.90f * c->day)
                                        * (0.55f + 0.45f * ao)),
                                    mul(v3(0.30f, 0.40f, 0.60f),
                                        (0.26f * c->day + 0.10f * night) * skyamt * ao)),
                                mul(v3(0.26f, 0.22f, 0.15f),
                                    (0.13f * c->day + 0.05f * night) * ao));
                col = v3(alb.x * lightv.x, alb.y * lightv.y, alb.z * lightv.z);
                dist = tg;
            } else {
                col = sky_col(c, ray);
                dist = FARCLIP * 4.0f;
            }

            if (dist < FARCLIP * 2.0f) col = aerial(c, col, dist, ray);
            put(c, x, y, col);
            c->zb[(size_t)y * c->W + x] = (dist < FARCLIP * 2.0f)
                                        ? dist * dot(ray, c->fwd) : 1e30f;
        }
    }
    free(sg);
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
                      float ao, float shadow, V3 ray)
{
    V3 sun = c->sun;
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
    /* Everything the sun does fades with it, and what is left at night is the
     * sky dome plus a little moonlight - never zero, or the frame goes black
     * and a 32-colour palette has nothing to say about it. */
    float night = 1.0f - c->day;
    float key   = 1.15f * lam * shadow * (0.12f + 0.88f * c->day);
    float sky   = (0.46f * c->day + 0.13f * night) * (0.35f + 0.65f * up_amt);
    float grnd  = (0.28f * c->day + 0.06f * night) * (1.0f - up_amt);
    float fill  = (0.26f * c->day + 0.10f * night) * ndotv;

    /* Underground and deep underwater the sun is not a factor, so the body is
     * lit from the camera with a distance falloff instead. Without it an
     * animal in its own burrow is black on black - the shading model has to
     * follow the medium the same way the background does. */
    if (c->buried || c->submerged) {
        float lamp = expf(-vz / (c->buried ? 55.0f : 190.0f));
        /* Underground the animal has to hold its own against the soil behind
         * it, and the soil is the brighter of the two: lit at the old strength
         * a burrower came out as a black cutout on a tan field. */
        float k = (c->buried ? 1.70f : 0.55f) * lamp * (0.35f + 0.65f * ndotv);
        V3 tint = c->buried ? v3(1.00f, 0.86f, 0.66f) : v3(0.72f, 0.94f, 1.00f);
        V3 base = c->buried ? v3(0.20f, 0.17f, 0.12f)
                            : mul(v3(0.16f, 0.34f, 0.40f), 0.55f);
        V3 lit = add(mul(v3(albedo.x * tint.x, albedo.y * tint.y, albedo.z * tint.z),
                         k + (c->submerged ? 0.42f * (0.35f + 0.65f * up_amt) * ao : 0.0f)),
                     mul(albedo, 0.0f));
        lit = add(lit, mul(base, ao));
        if (c->submerged) lit = add(lit, mul(v3(albedo.x, albedo.y, albedo.z),
                                             0.30f * lam * shadow));
        lit = add(lit, mul(c->buried ? v3(1.00f, 0.80f, 0.56f) : v3(0.72f, 0.80f, 0.92f),
                           rim * (c->buried ? 0.40f : 0.16f) * ao));
        if (em > 0.0f) lit = add(lit, mul(albedo, em));
        put(c, x, y, aerial(c, lit, vz, ray));
        return;
    }

    V3 sunc = v3(1.00f, 0.95f, 0.82f);
    V3 skyc = c->day > 0.4f ? v3(0.66f, 0.80f, 1.00f) : v3(0.46f, 0.56f, 0.96f);
    V3 bncc = v3(0.78f, 0.70f, 0.46f);
    V3 col = add(add(mul(v3(albedo.x * sunc.x, albedo.y * sunc.y, albedo.z * sunc.z), key),
                     mul(v3(albedo.x * skyc.x, albedo.y * skyc.y, albedo.z * skyc.z),
                         (sky + fill) * ao)),
                 mul(v3(albedo.x * bncc.x, albedo.y * bncc.y, albedo.z * bncc.z), grnd * ao));
    col = add(col, mul(v3(0.72f, 0.80f, 0.92f), rim * 0.22f * ao));
    if (em > 0.0f) col = add(col, mul(albedo, em));
    put(c, x, y, aerial(c, col, vz, ray));
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
            float sh = sdf_shadow(pr, n, q, mul(c->sun, -1.0f));
            shade_hit(c, x, y, nrm, albedo, em, hz, ao, sh, ray);
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

    V3 sun = c->sun;
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
            /* Same dome floor as shade_hit, for the same reason, and the same
             * day term: written without it every tree, boulder and bush in the
             * world stayed in full sunlight through the night while the ground
             * they stood on went black, which read as a forest of lamps. */
            float k = 1.15f * lam * (0.12f + 0.88f * c->day)
                    + (0.34f * c->day + 0.11f * (1.0f - c->day))
                      * (0.35f + 0.65f * up_amt)
                    + 0.13f * nz * (0.25f + 0.75f * c->day);
            V3 col = v3(albedo.x * k, albedo.y * k, albedo.z * k);
            if (emissive > 0.0f) col = add(col, mul(albedo, emissive));
            /* the ray through this very pixel, so the scattering halo lines up
             * with the one the terrain behind it is getting */
            float rx = ((float)x + 0.5f - c->W * 0.5f) / c->focal;
            float ry = -((float)y + 0.5f - c->H * 0.5f) / c->focal;
            V3 vray = norm(add(add(mul(c->right, rx), mul(c->up, ry)), c->fwd));
            put(c, x, y, aerial(c, col, z, vray));
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
        /* and the reverse: from inside the soil a bush growing on the surface
         * above is a black blob hanging in the earth */
        if (med != CP4_UNDER && c->buried) continue;
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

/* An upright sliver, tapering as it rises: a blade of grass, a reed, a stalk.
 *
 * Built out of spheres a tuft reads as a handful of peas, and it is the wrong
 * shape in the way that matters - what makes near ground look like ground is
 * a thousand thin vertical edges, and a sphere has none. This is a line in
 * screen space with a depth ramp along it, which is all a blade needs to be. */
static void blade(Ctx *c, V3 base, V3 tip, float wide, V3 low, V3 high)
{
    V3 db = sub(base, c->eye), dt = sub(tip, c->eye);
    float bz = dot(db, c->fwd), tz = dot(dt, c->fwd);
    if (bz < 5.0f || tz < 5.0f) return;

    float bx = c->W * 0.5f + c->focal * dot(db, c->right) / bz;
    float by = c->H * 0.5f - c->focal * dot(db, c->up) / bz;
    float tx = c->W * 0.5f + c->focal * dot(dt, c->right) / tz;
    float ty = c->H * 0.5f - c->focal * dot(dt, c->up) / tz;

    float ex = tx - bx, ey = ty - by;
    float len = sqrtf(ex * ex + ey * ey);
    if (len < 0.7f || len > (float)c->H) return;
    int n = (int)len + 1;
    float pw = c->focal * wide / bz;

    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n;
        float px = bx + ex * t, py = by + ey * t;
        /* Biased toward the eye by a fraction of a unit. Without it the base
         * of every blade z-fights the ground it is standing on. */
        float z = bz + (tz - bz) * t - 0.5f;
        int hw = (int)(pw * (1.0f - 0.80f * t));
        V3 col = v3(mixf(low.x, high.x, t), mixf(low.y, high.y, t),
                    mixf(low.z, high.z, t));
        int yy = (int)py;
        if ((unsigned)yy >= (unsigned)c->H) continue;
        for (int dx = -hw; dx <= hw; dx++) {
            int xx = (int)px + dx;
            if ((unsigned)xx >= (unsigned)c->W) continue;
            float *zp = &c->zb[(size_t)yy * c->W + xx];
            if (z >= *zp) continue;
            *zp = z;
            put(c, xx, yy, col);
        }
    }
}

/* Ground cover.
 *
 * The last thing wrong with the near ground was that it was correct: shaded,
 * occluded, textured, and still obviously a painted surface, because at ten
 * units away a real meadow is not a surface at all. This scatters tufts on a
 * fine grid out to the distance where a blade stops being a pixel, hashed the
 * same way the trees are, so it costs nothing to store and streams with the
 * unbounded world for free.
 *
 * It is drawn after the terrain and before the animals, so a creature still
 * occludes the grass it is standing in. */
static void draw_cover(Ctx *c, const Cp4World *w)
{
    if (c->buried || c->submerged) return;

    const float CELL = 5.5f;
    const float RANGE = 340.0f;

    int x0 = (int)floorf((c->eye.x - RANGE) / CELL);
    int x1 = (int)floorf((c->eye.x + RANGE) / CELL);
    int z0 = (int)floorf((c->eye.z - RANGE) / CELL);
    int z1 = (int)floorf((c->eye.z + RANGE) / CELL);

    for (int cz = z0; cz <= z1; cz++) {
        for (int cx = x0; cx <= x1; cx++) {
            float r0 = rhash(w->seed ^ 0x3311u, cx, cz);
            float r1 = rhash(w->seed ^ 0x9C4Du, cx, cz);
            float r2 = rhash(w->seed ^ 0x5E07u, cx, cz);

            float px = ((float)cx + r1) * CELL;
            float pz = ((float)cz + r2) * CELL;
            float dx = px - c->eye.x, dz = pz - c->eye.z;
            float d2 = dx * dx + dz * dz;
            if (d2 > RANGE * RANGE) continue;

            float gy = cp4_height(c->seed, px, pz);
            /* Nothing on the beach. A tuft rooted at the waterline takes its
             * colour from wet sand, which draws a stand of dead brown twigs
             * along every shore in the world. */
            if (gy > CP4_SEA - 11.0f) continue;

            int bi = cp4_biome(c->seed, px, pz);
            /* Cover follows the same fertility the flora does, so a desert
             * stays bare and a jungle floor is thick. Reading it from the sim
             * rather than from a second table is what keeps the picture and
             * the simulation agreeing about where things grow. */
            float dens = 0.22f + 0.70f * cp4_fertility(bi);
            if (bi == CP4_BIOME_DESERT || bi == CP4_BIOME_ICE) dens *= 0.28f;
            /* Held flat almost all the way out and then cut, rather than
             * thinned from the camera outwards: a gradient in density reads as
             * a bald patch around the player, which is worse than the hard
             * edge it was meant to hide. The edge itself is hidden by fading
             * the blades into the ground colour instead. */
            float dist = sqrtf(d2);
            float edge = clampf((RANGE - dist) / (RANGE * 0.30f), 0.0f, 1.0f);
            dens *= edge;
            if (r0 > dens) continue;

            V3 q = v3(px, gy, pz);
            V3 alb = ground_albedo(c, q, 1.0f, dist);
            /* Roots sit in their own shade and tips catch the sun: that split
             * is the whole reason a tuft reads as three-dimensional. */
            float lit = 0.35f + 0.85f * c->day;
            V3 low  = mul(alb, 0.42f * (0.22f + 0.78f * c->day));
            V3 high = mul(v3(alb.x * 1.25f + 0.05f, alb.y * 1.45f + 0.09f,
                             alb.z * 1.10f + 0.03f), lit);

            int nb = 2 + (int)(r1 * 3.0f);
            /* fade into the ground it grows out of, so the far edge of the
             * scatter has nothing to see */
            float mixin = 1.0f - edge;
            low  = v3(mixf(low.x,  alb.x, mixin), mixf(low.y,  alb.y, mixin),
                      mixf(low.z,  alb.z, mixin));
            high = v3(mixf(high.x, alb.x, mixin), mixf(high.y, alb.y, mixin),
                      mixf(high.z, alb.z, mixin));
            float hgt = 4.5f + 5.5f * r2;
            for (int b = 0; b < nb; b++) {
                float a = (r0 + (float)b * 0.79f) * 6.2832f;
                float lean = 0.35f + 0.55f * rhash(c->seed ^ (uint32_t)(0x77u + b), cx, cz);
                float sx = px + cosf(a) * CELL * 0.22f;
                float sz = pz + sinf(a) * CELL * 0.22f;
                V3 base = v3(sx, gy + 0.6f, sz);
                V3 tip  = v3(sx + cosf(a) * hgt * lean, gy - hgt,
                             sz + sinf(a) * hgt * lean);
                V3 hi = high;
                if (b & 1) hi = mul(hi, 0.86f);
                blade(c, base, tip, 0.30f, low, hi);
            }
        }
    }
}

/* Scenery.
 *
 * Trees, boulders and snags, hashed straight out of the cell they stand in.
 * Nothing about them is stored and nothing about them is simulated - they are
 * scenery in the strict sense, and the bushes the animals actually eat are
 * still the separate, simulated Cp4Flora. Mixing the two would be the obvious
 * shortcut and the wrong one: food has to be finite, recycled and eaten, and
 * a forest has to be everywhere without costing anything.
 *
 * Because the cell hash is a pure function of position, this streams with the
 * unbounded world exactly the way the terrain does. */
static void draw_scenery(Ctx *c, const Cp4World *w)
{
    (void)w;
    int x0 = (int)floorf((c->eye.x - SCEN_RANGE) / SCEN_CELL);
    int x1 = (int)floorf((c->eye.x + SCEN_RANGE) / SCEN_CELL);
    int z0 = (int)floorf((c->eye.z - SCEN_RANGE) / SCEN_CELL);
    int z1 = (int)floorf((c->eye.z + SCEN_RANGE) / SCEN_CELL);

    for (int cz = z0; cz <= z1; cz++) {
        for (int cx = x0; cx <= x1; cx++) {
            Scen s;
            if (!scenery_at(c, cx, cz, &s)) continue;

            float dx = s.x - c->eye.x, dz = s.z - c->eye.z;
            if (dx * dx + dz * dz > SCEN_RANGE * SCEN_RANGE) continue;

            if (s.kind == SCEN_ROCK) {
                /* a boulder and a couple of chips off it */
                float rr = s.h;
                float tone = 0.26f + 0.16f * s.r2;
                V3 rock = v3(tone * 1.05f, tone * 1.00f, tone * 0.94f);
                if (s.biome == CP4_BIOME_DESERT) rock = v3(tone * 1.45f, tone * 1.20f, tone * 0.80f);
                if (s.biome == CP4_BIOME_ICE)    rock = v3(tone * 1.35f, tone * 1.42f, tone * 1.50f);
                sphere(c, v3(s.x, s.y - rr * 0.55f, s.z), rr, rock, 0.0f);
                sphere(c, v3(s.x + rr * 0.9f, s.y - rr * 0.25f, s.z - rr * 0.5f),
                       rr * 0.45f, rock, 0.0f);
                continue;
            }

            /* A tree. The crown's colour is jittered per tree and not just per
             * biome: a wood painted in one green is a green wall, and the
             * variation between individual canopies is most of what makes the
             * treeline of a real one legible at a distance. */
            float h = s.h;
            float bark = 0.15f + 0.10f * s.r2;
            V3 trunk = v3(bark * 1.35f, bark * 1.00f, bark * 0.62f);
            float leafv = 0.26f + 0.30f * s.r2;
            float warm = rhash(c->seed ^ 0x4B93u, (int)s.x, (int)s.z);     /* autumn dice */
            V3 leaf = s.biome == CP4_BIOME_JUNGLE
                        ? v3(leafv * 0.38f, leafv * 1.05f, leafv * 0.36f)
                    : s.biome == CP4_BIOME_TAIGA
                        ? v3(leafv * 0.36f, leafv * 0.74f, leafv * 0.54f)
                        : v3(leafv * (0.48f + 0.85f * warm * warm),
                             leafv * (1.00f - 0.18f * warm),
                             leafv * (0.40f - 0.16f * warm));

            sphere(c, v3(s.x, s.y - h * 0.30f, s.z), h * 0.075f, trunk, 0.0f);
            sphere(c, v3(s.x, s.y - h * 0.62f, s.z), h * 0.060f, trunk, 0.0f);

            if (s.kind == SCEN_CONIFER) {
                /* conifer: a stack of narrowing tiers */
                for (int t = 0; t < 5; t++) {
                    float f = (float)t / 4.0f;
                    sphere(c, v3(s.x, s.y - h * (0.46f + 0.52f * f), s.z),
                           h * (0.31f - 0.24f * f), leaf, 0.0f);
                }
            } else {
                /* Broadleaf: a big lobe with smaller ones hung off it. Three
                 * equal spheres came out as a clover leaf - the sizes have to
                 * disagree or the silhouette stays a circle. */
                float a0 = s.r2 * 6.283f;
                sphere(c, v3(s.x, s.y - h * 0.80f, s.z), h * 0.27f, leaf, 0.0f);
                for (int t = 0; t < 4; t++) {
                    float a = a0 + (float)t * 1.571f;
                    float rr = h * (0.13f + 0.09f * rhash(c->seed ^ (uint32_t)(0x91u + t),
                                                          (int)s.x, (int)s.z));
                    sphere(c, v3(s.x + cosf(a) * h * 0.24f,
                                 s.y - h * (0.66f + 0.20f * (float)(t & 1)),
                                 s.z + sinf(a) * h * 0.24f),
                           rr, leaf, 0.0f);
                }
            }
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
            /* Near geometry only. A keyline exists to separate a subject from
             * what is behind it, and at the horizon everything is behind
             * everything: the dithered edge where the far coast meets the sky
             * is thousands of ground pixels each with a sky pixel next to
             * them, so the whole skyline came back inked into a hard dark bar
             * running the width of the frame. */
            if (z > 1e29f || z > 900.0f) continue;
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

/* The HUD is laid out once on a 320x180 grid and drawn through a scale, so a
 * style with a bigger internal buffer gets a bigger HUD rather than a HUD that
 * shrinks into the corner. Every coordinate below is in that grid. */
typedef struct { uint8_t *fb; int W, H, s; } Hud;

static void hud_rect(const Hud *u, int x, int y, int w, int h,
                     float r, float g, float b, float a)
{
    cp_px_rect(u->fb, u->W, u->H, x * u->s, y * u->s, w * u->s, h * u->s, r, g, b, a);
}

static void hud_text(const Hud *u, int x, int y, const char *s,
                     float r, float g, float b)
{
    cp_px_text(u->fb, u->W, u->H, x * u->s, y * u->s, u->s, s, r, g, b, 1.0f);
}

/* The frame is the point, so the HUD claims as little of it as it can get away
 * with: two corners and a strip, no headings, and nothing that repeats what
 * the picture already says. */
static void draw_hud4(uint8_t *fb, int W, int H, const Cp4World *w, int scale)
{
    char buf[80];
    const Cp4Beast *p = &w->player;
    Hud u = { fb, W, H, scale < 1 ? 1 : scale };
    const int DW = W / u.s, DH = H / u.s;

    #define PANEL(X,Y,PW,PH) do {                                             \
        hud_rect(&u, (X), (Y), (PW), (PH), 0.05f, 0.06f, 0.04f, 0.80f);       \
        hud_rect(&u, (X), (Y), (PW), 1, 0.52f, 0.62f, 0.40f, 0.50f);          \
    } while (0)
    #define BAR(X,Y,BW,F,R,G,B) do {                                          \
        hud_rect(&u, (X), (Y), (BW), 3, 0.03f, 0.04f, 0.03f, 1.0f);           \
        int _f = (int)((BW) * clampf((F), 0.0f, 1.0f));                       \
        if (_f > 0) hud_rect(&u, (X), (Y), _f, 3, (R), (G), (B), 1.0f);       \
    } while (0)

    /* Vitals: the four numbers that can end the run, and nothing else. */
    PANEL(3, 3, 106, 25);
    snprintf(buf, sizeof(buf), "G%d/%d T%d  A%d F%d", w->generation + 1,
             CP4_GENERATIONS, w->step, w->allies, w->enemies);
    hud_text(&u, 6, 6, buf, 0.70f, 0.82f, 0.58f);
    BAR(6, 15, 47, p->hp / p->hp_max, 0.86f, 0.28f, 0.30f);
    BAR(56, 15, 47, w->dna / CP4_DNA_GOAL, 0.42f, 0.78f, 0.94f);
    BAR(6, 21, 47, p->stam / (p->s.stamina > 1.0f ? p->s.stamina : 1.0f),
        0.92f, 0.80f, 0.30f);
    BAR(56, 21, 47, p->energy / 170.0f, 0.44f, 0.84f, 0.44f);

    /* Which medium, and how long you may stay in it. In three of the four the
     * clock is the thing that kills you, so it belongs next to health. */
    {
        static const float MC[CP4_MEDIUM_COUNT][3] = {
            { 0.62f, 0.86f, 0.50f }, { 0.40f, 0.80f, 0.92f },
            { 0.86f, 0.88f, 0.96f }, { 0.84f, 0.66f, 0.40f },
        };
        int m = p->medium % CP4_MEDIUM_COUNT;
        int bi = cp4_biome(w->seed, p->p.x, p->p.z);
        snprintf(buf, sizeof(buf), "%s %s", cp4_medium_name(m), cp4_biome_name(bi));
        for (char *q = buf; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
        /* Where you are and what kind of place it is, on its own dark strip.
         * Against an open sky an olive label is simply invisible. */
        int lw2 = cp_px_text_w(buf, 1) + 14;
        hud_rect(&u, 111, 3, lw2, 10, 0.05f, 0.06f, 0.04f, 0.80f);
        hud_rect(&u, 114, 6, 4, 4, MC[m][0], MC[m][1], MC[m][2], 1.0f);
        hud_text(&u, 121, 5, buf, MC[m][0], MC[m][1], MC[m][2]);
        if (p->s.breath < 1.0e8f && (m == CP4_IN_WATER || m == CP4_UNDER)) {
            float f = clampf(p->breath / (p->s.breath > 1.0f ? p->s.breath : 1.0f),
                             0.0f, 1.0f);
            hud_rect(&u, 113, 15, 40, 3, 0.03f, 0.04f, 0.03f, 1.0f);
            int fw = (int)(40.0f * f);
            if (fw > 0)
                hud_rect(&u, 113, 15, fw, 3,
                         f < 0.3f ? 0.94f : 0.42f, f < 0.3f ? 0.36f : 0.82f, 0.94f, 1.0f);
        }
    }

    /* Population and travel, three lines. Nothing scripts these numbers - they
     * are whatever survived out there while the player was busy. */
    {
        int mine = 0;
        for (int i = 0; i < CP4_MAX_BEASTS; i++)
            if (w->beast[i].alive && w->beast[i].nest == CP4_OWN_NEST) mine++;
        PANEL(3, DH - 30, 138, 27);
        snprintf(buf, sizeof(buf), "POP %-3d G%.1f  +%d -%d",
                 w->pop, (double)w->mean_gen, w->births, w->deaths);
        hud_text(&u, 6, DH - 27, buf, 0.74f, 0.80f, 0.66f);
        snprintf(buf, sizeof(buf), "LEG %.1f CHR %.2f  WON %d",
                 (double)w->mean_legs, (double)w->mean_charm, w->befriended);
        hud_text(&u, 6, DH - 19, buf, 0.60f, 0.84f, 0.62f);
        if (w->home.alive)
            snprintf(buf, sizeof(buf), "ROAM %d FOUND %d  NEST %d/%d",
                     (int)w->travelled, w->discovered, (int)w->home.store, mine);
        else
            snprintf(buf, sizeof(buf), "ROAM %d FOUND %d",
                     (int)w->travelled, w->discovered);
        hud_text(&u, 6, DH - 11, buf, 0.62f, 0.78f, 0.86f);
    }

    /* own build */
    PANEL(DW - 128, DH - 30, 108, 27);
    snprintf(buf, sizeof(buf), "%dDNA %dP %dSEG",
             (int)p->s.cost, p->s.n_parts, p->g.nseg);
    hud_text(&u, DW - 125, DH - 27, buf, 0.66f, 0.78f, 0.54f);
    int col = 0;
    for (int t = 1; t < CP4_PART_COUNT; t++) {
        int n = p->s.n[t];
        if (!n) continue;
        float er;
        V3 cl = part_albedo4(t, &er);
        int x = DW - 125 + (col % 6) * 17, y = DH - 17 + (col / 6) * 8;
        hud_rect(&u, x, y, 5, 5, cl.x, cl.y, cl.z, 1.0f);
        snprintf(buf, sizeof(buf), "%d", n);
        hud_text(&u, x + 7, y - 1, buf, 0.82f, 0.86f, 0.76f);
        col++;
    }

    if (w->status != CP4_RUN) {
        const char *msg = w->status == CP4_EVOLVED ? "EVOLVE - TRIBE"
                        : w->status == CP4_DEAD    ? "KILLED" : "TIME UP";
        int tw = cp_px_text_w(msg, 1);
        int bx = (DW - tw) / 2, by = DH / 2 - 7;
        PANEL(bx - 6, by - 4, tw + 12, 16);
        hud_text(&u, bx, by, msg,
                 w->status == CP4_EVOLVED ? 0.62f : 1.0f,
                 w->status == CP4_EVOLVED ? 0.94f : 0.46f, 0.52f);
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
    c.sun = sun_dir(w->step);
    c.day = w->daylight;

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
         * flat grey field: every ray escaped into daylight immediately.
         *
         * The stand-off is a compromise between two failures. The surface
         * camera's distance leaves the animal three quarters fogged out, since
         * the fog down here is measured in tens of units; pulled right in, the
         * frame is the animal's own back end lit head-on, which comes out as a
         * dark disc. Half, off the centre line, shows a flank. */
        back *= 0.55f;
        c.eye = add(add(cv(p->p), mul(pf, -back)), mul(pr, back * 0.42f));
        /* At the animal's own depth, not just under the surface: a camera
         * pinned to the roof looks down a shaft at nothing. */
        c.eye.y = p->p.y - p->s.stand * 0.55f;
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
    /* Neither belongs underground: a tree hanging in the soil three
     * units from the camera is not scenery, it is a bug with roots. */
    if (!c.buried) { draw_scenery(&c, w); draw_cover(&c, w); }
    draw_nests(&c, w);
    draw_home(&c, w);
    draw_flora(&c, w);
    for (int i = 0; i < CP4_MAX_BEASTS; i++)
        if (w->beast[i].alive) draw_creature(&c, &w->beast[i], 0);
    draw_creature(&c, p, 1);

    outline_pass(&c);
    draw_hud4(fb, lw, lh, w, lh / 180 < 1 ? 1 : lh / 180);
    cp_vis_quantise(fb, lw, lh, style);
    cp_vis_blit(fb, lw, lh, rgba, OW, OH, style);

    free(fb);
    free(zb);
}

void cp4_render(const Cp4World *w, uint8_t *rgba, int W, int H)
{
    cp4_render_styled(w, rgba, W, H, CP_VIS_TERRA);
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
    c.sun = norm(v3(0.38f, 0.72f, -0.26f));   /* a portrait is always noon */
    c.day = 1.0f;
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
