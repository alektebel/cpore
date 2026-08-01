#include "cpore/cpore.h"

/* The body plan is the first decision an agent makes and the one it cannot
 * take back. Parts are drawn from a shared budget, so every build is a
 * commitment: speed costs teeth, armour costs speed, and a mouth you don't
 * have is food you cannot eat. */

void cp_morph_default(CpMorph *m)
{
    m->herb = 1; m->carn = 0; m->spike = 0;
    m->cilia = 2; m->flag = 0; m->elec = 0;
}

static int morph_total(const CpMorph *m)
{
    return m->herb + m->carn + m->spike + m->cilia + m->flag + m->elec;
}

void cp_morph_clamp(CpMorph *m)
{
    static const uint8_t cap[6] = { 2, 2, 4, 4, 2, 2 };
    uint8_t *p[6] = { &m->herb, &m->carn, &m->spike, &m->cilia, &m->flag, &m->elec };

    for (int i = 0; i < 6; i++)
        if (*p[i] > cap[i]) *p[i] = cap[i];

    /* a cell with no mouth at all cannot eat; grant one before budgeting so
     * that the trim below accounts for it */
    if (m->herb == 0 && m->carn == 0) m->herb = 1;

    /* spend down to the budget, trimming the largest group first so a build
     * degrades gracefully instead of losing a whole capability - but never
     * trim away the last mouth */
    while (morph_total(m) > CP_MORPH_MAX_PARTS) {
        int best = -1;
        for (int i = 0; i < 6; i++) {
            if (*p[i] == 0) continue;
            int last_mouth = (i == 0 && *p[0] == 1 && m->carn == 0)
                          || (i == 1 && *p[1] == 1 && m->herb == 0);
            if (last_mouth) continue;
            if (best < 0 || *p[i] > *p[best]) best = i;
        }
        if (best < 0) break;
        (*p[best])--;
    }
}

void cp_morph_random(CpMorph *m, CpRng *r)
{
    m->herb  = (uint8_t)cp_rng_int(r, 3);
    m->carn  = (uint8_t)cp_rng_int(r, 3);
    m->spike = (uint8_t)cp_rng_int(r, 5);
    m->cilia = (uint8_t)cp_rng_int(r, 5);
    m->flag  = (uint8_t)cp_rng_int(r, 3);
    m->elec  = (uint8_t)cp_rng_int(r, 3);
    cp_morph_clamp(m);
}

void cp_morph_stats(const CpMorph *m, CpStats *o)
{
    int parts = morph_total(m);

    /* locomotion: cilia give sustained speed, flagella give acceleration */
    o->max_speed = 150.0f + 46.0f * m->cilia + 24.0f * m->flag;
    o->accel     = 620.0f + 90.0f * m->cilia + 260.0f * m->flag;
    o->drag      = 2.6f;

    /* mass costs mobility - a heavily built cell is a slow cell */
    float mass = 1.0f + 0.07f * parts + 0.03f * m->spike + 0.03f * m->carn;
    o->max_speed /= mass;
    o->accel     /= mass;

    o->hp_max  = 100.0f + 14.0f * m->spike + 10.0f * m->carn;
    o->attack  = 6.0f * m->carn + 9.0f * m->spike + 7.0f * m->elec;
    o->armor   = 0.10f * m->spike + 0.06f * m->carn;
    if (o->armor > 0.6f) o->armor = 0.6f;

    o->herb_eff = m->herb ? (0.55f + 0.45f * m->herb) : 0.0f;
    o->carn_eff = m->carn ? (0.55f + 0.45f * m->carn) : 0.0f;

    o->radius0 = 14.0f + 0.9f * parts;
    o->upkeep  = 0.28f + 0.16f * parts;   /* bigger builds burn more */
}
