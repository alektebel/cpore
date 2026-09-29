#include "spore/terrain.h"

#include <math.h>

void terrain_init(Terrain *t, float world_size, float water_y, unsigned seed) {
    if (!t) return;
    t->world_size = world_size > 0.0f ? world_size : 256.0f;
    t->water_y = water_y;
    t->seed = seed;
}

/* Sum of a few sines: cheap, smooth, tileable enough for a prototype, and
 * identical on every machine because it is pure arithmetic on the seed. */
static float wave(float x, float z, float fx, float fz, float phase) {
    return sinf(x * fx + phase) * cosf(z * fz - phase);
}

float terrain_height(const Terrain *t, float x, float z) {
    if (!t) return 0.0f;
    float s = (float)(t->seed % 977) * 0.017f;
    float h = 0.0f;
    h += 1.60f * wave(x, z, 0.0130f, 0.0110f, s);
    h += 0.80f * wave(x, z, 0.0310f, 0.0270f, s * 1.7f + 1.1f);
    h += 0.38f * wave(x, z, 0.0710f, 0.0630f, s * 2.3f + 2.4f);
    h += 0.16f * wave(x, z, 0.1600f, 0.1400f, s * 3.1f + 0.7f);
    return h;
}

Vec3 terrain_normal(const Terrain *t, float x, float z) {
    const float e = 0.35f;
    float hl = terrain_height(t, x - e, z);
    float hr = terrain_height(t, x + e, z);
    float hd = terrain_height(t, x, z - e);
    float hu = terrain_height(t, x, z + e);
    return v3_norm(v3(hl - hr, 2.0f * e, hd - hu));
}

int terrain_is_water(const Terrain *t, float x, float z) {
    if (!t) return 0;
    return terrain_height(t, x, z) <= t->water_y;
}
