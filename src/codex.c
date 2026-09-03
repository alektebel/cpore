/* Codex implementation. */
#include "cpore/codex.h"
#include <string.h>
#include <stdio.h>

void cpdx_reset(CpdxCodex *c)
{
    if (c) memset(c, 0, sizeof(*c));
}

uint32_t cpdx_hash_bytes(const uint8_t *b, uint32_t n)
{
    uint32_t h = 2166136261u;
    uint32_t i;
    if (!b) return 0;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h ? h : 1;
}

static const char *ADJ[] = {
    "Keelback", "Ashen", "Mottled", "Crested", "Burrow", "Reed",
    "Ember", "Pale", "Thorn", "Mire", "Sunset", "Hollow",
    "Ridged", "Silent", "Amber", "Frost", "Dusky", "Long",
};
static const char *NOUN[] = {
    "Strider", "Grazer", "Maw", "Singer", "Darter", "Warden",
    "Tunneler", "Glider", "Stalker", "Wader", "Chorus", "Bulwark",
    "Skimmer", "Rooter", "Howler", "Prowler",
};

void cpdx_default_name(uint32_t hash, char *out)
{
    const char *a, *b;
    if (!out) return;
    a = ADJ[(hash >> 3) % (sizeof(ADJ) / sizeof(ADJ[0]))];
    b = NOUN[(hash >> 11) % (sizeof(NOUN) / sizeof(NOUN[0]))];
    snprintf(out, CPDX_NAME, "%s %s", a, b);
}

int cpdx_note(CpdxCodex *c, uint32_t genome_hash, int biome, int medium,
              int diet, int disposition, int32_t step, float x, float z)
{
    int i;
    if (!c || !genome_hash) return -1;
    for (i = 0; i < c->n; i++) {
        if (c->entry[i].genome_hash == genome_hash) {
            c->entry[i].sightings++;
            return 0;
        }
    }
    if (c->n >= CPDX_MAX) return -1;
    {
        CpdxEntry *e = &c->entry[c->n++];
        memset(e, 0, sizeof(*e));
        e->genome_hash = genome_hash;
        cpdx_default_name(genome_hash, e->name);
        e->biome = (uint8_t)(biome < 0 ? 0xFF : biome);
        e->medium = (uint8_t)(medium < 0 ? 0xFF : medium);
        e->diet = (uint8_t)diet;
        e->disposition = (uint8_t)disposition;
        e->seen_step = step;
        e->sightings = 1;
        e->x = x;
        e->z = z;
        e->used = 1;
    }
    return 1;
}
