#ifndef SPORE_TERRAIN_H
#define SPORE_TERRAIN_H

/* A small, deterministic height field. The creature stage only ever asks it
 * three questions - how high is the ground here, which way does it face, is
 * this water - so that is the whole interface. */

#include "spore/math3d.h"

typedef struct {
    float world_size;   /* square world, [0, world_size] on X and Z */
    float water_y;      /* height at or below which terrain counts as water */
    unsigned seed;
} Terrain;

void  terrain_init(Terrain *t, float world_size, float water_y, unsigned seed);
float terrain_height(const Terrain *t, float x, float z);
Vec3  terrain_normal(const Terrain *t, float x, float z);
int   terrain_is_water(const Terrain *t, float x, float z);

#endif
