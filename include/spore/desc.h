#ifndef SPORE_DESC_H
#define SPORE_DESC_H

/* A named, saved creature: the genome plus the presentation genes the creator
 * lets you paint. Saving is deliberately text so a creature is diffable and
 * hand-editable, which is the point of a creator file. */

#include "spore/creature.h"
#include "spore/genome.h"

typedef struct {
    char  name[24];
    Genome genome;
    Vec3  color;
    Vec3  color2;
    int   pattern;
    float scale;
    SpineColumn spine;   /* the sculpted column, so a save round-trips exactly */
} CreatureDesc;

void desc_defaults(CreatureDesc *d);
int  desc_save(const CreatureDesc *d, const char *path);
int  desc_load(CreatureDesc *d, const char *path);

#endif
