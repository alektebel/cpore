/* The codex - discovery made into an event.
 *
 * BOTW's shrines work because finding one IS the reward: fanfare, card,
 * collection. The census counter was not that. A codex entry fires the first
 * time a lineage is sighted: a card with a name, where it lives, what it
 * eats and whether it fears you - and the count is already in the
 * observation, so the same mechanism is the lab's exploration reward.
 *
 * POD, memcpy-able, no RNG of its own: insertion order is deterministic, and
 * procedural names derive from the genome hash. An LLM namer can overwrite
 * `name` later (Part V of the design doc); the C core never knows. */
#ifndef CPORE_CODEX_H
#define CPORE_CODEX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CPDX_MAX    64
#define CPDX_NAME   24

typedef struct {
    uint32_t genome_hash;
    char     name[CPDX_NAME];
    uint8_t  biome;        /* CP4_BIOME_* (or 0xff outside land) */
    uint8_t  medium;       /* CP4_ON_GROUND.. (or 0xff) */
    uint8_t  diet;         /* 0 grazer / 1 predator / 2 omnivore-or-unknown */
    uint8_t  disposition;  /* 0 wary / 1 neutral / 2 friendly, at sighting */
    int32_t  seen_step;
    int32_t  sightings;
    float    x, z;         /* where it was first met */
    uint8_t  used, pad[3];
} CpdxEntry;

typedef struct {
    CpdxEntry entry[CPDX_MAX];
    int32_t   n;
} CpdxCodex;

void cpdx_reset(CpdxCodex *c);
/* Note a sighting. Returns 1 if this is a NEW species (fire the card!),
 * 0 if already catalogued, -1 if the codex is full (still counted). */
int cpdx_note(CpdxCodex *c, uint32_t genome_hash, int biome, int medium,
              int diet, int disposition, int32_t step, float x, float z);
/* Procedural field name from a hash ("Keelback Strider"). Always available
 * offline; an LLM plugin may overwrite entry.name afterwards. */
void cpdx_default_name(uint32_t hash, char *out /* CPDX_NAME */);
/* FNV-1a over canonical genome bytes - stable across machines. */
uint32_t cpdx_hash_bytes(const uint8_t *b, uint32_t n);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_CODEX_H */
