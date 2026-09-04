/* One creature, five games.
 *
 * Each stage of this project has its own genome, and it has to: a cell has an
 * angle on a circle where a land animal has a segment index and a yaw and a
 * pitch, and neither of them has anything to say about a city. Converting
 * between those genomes directly would be twenty conversions that all have to
 * agree, and the first one to drift would quietly break the only thing the
 * campaign is about - that the animal in the third stage is recognisably the
 * one you built in the first.
 *
 * So nothing converts between genomes. There is one small stage-independent
 * record in the middle, and every stage knows only how to read itself into it
 * and write itself out of it. Five stages, ten functions, and no pair of them
 * has to know the other exists.
 *
 * The rule that makes it work is that a stage writes back only what it can
 * express. A cell has no colour genes, so a cell cannot change your colour and
 * whatever you started as comes out the far side of the pond unchanged. A fish
 * has no voice, so nothing that happens underwater can make your species more
 * or less charming. This is not a limitation being worked around; it is the
 * reason the record is trustworthy, because every field in it was last written
 * by a stage that had an opinion about it.
 */
#ifndef CPORE_LINEAGE_H
#define CPORE_LINEAGE_H

#include <stdint.h>

#include "cpore/cpore.h"
#include "cpore/aqua.h"
#include "cpore/land.h"
#include "cpore/civ.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What survives a change of medium.
 *
 * Everything here is 0..255, and the traits are deliberately *not* the parts.
 * A spike, a jaw and a claw are three different objects in three different
 * stages and they are all the same intention, which is the thing worth
 * carrying. Diet is two numbers rather than one label because omnivory is a
 * real point on the line rather than a third category.
 */
typedef struct {
    uint8_t herb, carn;         /* what it can digest                       */

    uint8_t speed;              /* locomotion: cilia, tails, legs, wings    */
    uint8_t armour;             /* plates, hide, spines turned outward      */
    uint8_t weapon;             /* jaws, spikes, claws, horns               */
    uint8_t sense;              /* eyes, ears, photophores                  */
    uint8_t social;             /* voice, plume, display                    */

    /* The visual identity, which is most of what makes it *yours*. Stage 1
     * has no opinion about any of this and therefore never touches it. */
    uint8_t hue, hue2, hue3;
    uint8_t sat, val;
    uint8_t pattern, pscale;

    /* Proportions that mean the same thing in water and on land. */
    uint8_t nseg, girth;
    int8_t  arch, sweep;

    uint8_t stages;             /* how many stages this lineage has cleared */
    uint8_t pad[3];
} CpLineage;

/* A blank slate: mid-sized, unspecialised, and a colour rather than no
 * colour, because a lineage with sat 0 would come out of five stages grey. */
void cp_lineage_default(CpLineage *l);
/* A random starting creature, for a campaign that does not begin in an editor. */
void cp_lineage_random(CpLineage *l, CpRng *r);

/* ---- reading a stage back into the lineage ----
 * Each of these updates the fields its stage can express and leaves the rest
 * exactly as it found them. */
void cp_lineage_from_cell(CpLineage *l, const CpGenome *g);
void cp_lineage_from_aqua(CpLineage *l, const Cp3Genome *g);
void cp_lineage_from_land(CpLineage *l, const Cp4Genome *g);

/* ---- writing the lineage into a stage ----
 * Each picks the stage's own designer style nearest the lineage's intent,
 * spends the budget through it, then imposes the identity - colour and
 * proportion - and tops up whatever budget is left with the parts that most
 * express the strongest traits. The result is always a legal genome within
 * `budget`, because each ends by normalising. */
void cp_lineage_to_cell(const CpLineage *l, CpGenome *g, int budget, CpRng *r);
void cp_lineage_to_aqua(const CpLineage *l, Cp3Genome *g, int budget, CpRng *r);
void cp_lineage_to_land(const CpLineage *l, Cp4Genome *g, int budget, CpRng *r);

/* The civilisation stage takes multipliers rather than a body, so this is
 * where the chain stops being morphology and starts being doctrine. Same
 * 0.80..1.60 range cp5_legacy_from_creature uses, so the two are
 * interchangeable and a campaign can be joined at either end. */
void cp_lineage_to_legacy(const CpLineage *l, Cp5Legacy *out);

/* Which of a stage's designer styles a lineage most resembles. Exposed
 * because it is genuinely useful on its own - it is the answer to "what kind
 * of animal is this", in that stage's own vocabulary. */
int  cp_lineage_style_cell(const CpLineage *l);
int  cp_lineage_style_aqua(const CpLineage *l);
int  cp_lineage_style_land(const CpLineage *l);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_LINEAGE_H */
