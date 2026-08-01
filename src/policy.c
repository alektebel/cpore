#include "cpore/cpore.h"
#include <math.h>
#include <string.h>

/* A hand-written baseline. Not smart - it has no memory, no planning and no
 * notion of the DNA goal - but it eats, it runs away, it discharges when
 * swarmed, and it fills in the design head at generation boundaries. It is
 * the bar a learned policy has to clear. */

#define PART_SCALE  (2.0f / (float)(CP_PART_COUNT - 1))

/* encode a genome into the design half of the action vector */
static void encode_design(const CpGenome *g, float *design)
{
    for (int i = 0; i < CP_MAX_PARTS; i++) {
        design[i * 2]     = (float)g->part[i].type * PART_SCALE - 1.0f;
        design[i * 2 + 1] = (float)g->part[i].angle * (2.0f / 255.0f) - 1.0f;
    }
}

/* Keep building the kind of cell we already are. Switching identity halfway
 * through wastes the parts already paid for. */
static int style_of(const CpStats *st)
{
    if (st->carn_eff > st->herb_eff)   return CP_STYLE_HUNTER;
    if (st->n[CP_PART_EYE] >= 2)       return CP_STYLE_SCOUT;
    if (st->n[CP_PART_SPIKE] >= 2 ||
        st->n[CP_PART_POISON] >= 1)    return CP_STYLE_TANK;
    return CP_STYLE_GRAZER;
}

void cp_policy_greedy(const CpWorld *w, float act[CP_ACT_DIM])
{
    const CpCell *p = &w->player;
    const CpStats *st = &w->stats;
    float ax = 0.0f, ay = 0.0f;

    /* threats first: sum repulsion from everything bigger and armed */
    float flee_x = 0.0f, flee_y = 0.0f, flee_w = 0.0f;
    float prey_x = 0.0f, prey_y = 0.0f, prey_d = 1e18f;
    int   in_zap_range = 0;

    float attack = st->spike_dmg + st->jaw_dmg;

    for (int i = 0; i < CP_MAX_CELLS; i++) {
        const CpCell *c = &w->cells[i];
        if (!c->alive) continue;
        float dx = c->x - p->x, dy = c->y - p->y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < 0.01f) continue;
        if (d > st->percep) continue;          /* we cannot act on what we cannot see */

        /* Something we clearly outclass is lunch, not a threat. Without this
         * exclusion an armed build spends the whole episode retreating from
         * cells it could kill in a quarter of a second. */
        int outclassed = (attack > 4.0f && p->r > c->r * 1.25f);
        int dangerous = (c->attack > 0.0f && c->r >= p->r * 0.70f && !outclassed);
        float reach = 130.0f + c->r * 2.6f;
        if (dangerous && d < reach) {
            float k = (reach - d) / reach;
            k *= 0.6f + 0.9f * (c->r / p->r);
            flee_x -= dx / d * k; flee_y -= dy / d * k; flee_w += k;
        }
        if (outclassed && st->carn_eff > 0.0f && d < 300.0f && d < prey_d) {
            prey_d = d; prey_x = dx / d; prey_y = dy / d;
        }
        if (st->elec_dmg > 0.0f && d < st->elec_radius + c->r) in_zap_range++;
    }

    /* nearest edible pellet - squared distances only, one sqrt at the end */
    float food_x = 0.0f, food_y = 0.0f, food_d2 = 1e18f;
    float percep2 = st->percep * st->percep;
    for (int i = 0; i < CP_MAX_FOOD; i++) {
        const CpFood *f = &w->food[i];
        if (f->type == CP_FOOD_NONE) continue;
        if (f->type == CP_FOOD_PLANT ? st->herb_eff <= 0.0f : st->carn_eff <= 0.0f) continue;
        float dx = f->x - p->x, dy = f->y - p->y;
        float d2 = dx * dx + dy * dy;
        if (d2 > percep2) continue;
        if (d2 < food_d2) { food_d2 = d2; food_x = dx; food_y = dy; }
    }
    float food_d = 1e18f;
    if (food_d2 < 1e17f) {
        food_d = sqrtf(food_d2);
        if (food_d > 0.001f) { food_x /= food_d; food_y /= food_d; }
    }

    /* A hungry predator should hunt harder, not give up. The obvious gate
     * ("only chase prey above 55% health") puts carnivore builds into a
     * starvation spiral: low health stops them hunting, so they only scavenge,
     * so they stay at low health. */
    int starving = p->hp < p->hp_max * 0.35f;
    int hunt = prey_d < 1e17f && (food_d > prey_d * 1.5f || !starving);

    if (flee_w > 0.35f) {
        ax = flee_x; ay = flee_y;                       /* survival beats lunch */
    } else if (hunt) {
        ax = prey_x; ay = prey_y;
    } else if (food_d < 1e17f) {
        ax = food_x + flee_x * 0.5f;
        ay = food_y + flee_y * 0.5f;
    } else {
        /* nothing in range: sweep toward the middle rather than sit still */
        ax = (CP_WORLD_W * 0.5f - p->x);
        ay = (CP_WORLD_H * 0.5f - p->y);
        float n = sqrtf(ax * ax + ay * ay);
        if (n > 1.0f) { ax /= n; ay /= n; }
    }

    /* Stay off the walls - but smoothly. A constant push that switches on at a
     * fixed distance exactly cancels the (unit-length) target attraction, and
     * the agent parks on that threshold forever. */
    const float M = 140.0f;
    if (p->x < M)                 ax += (1.0f - p->x / M) * 1.3f;
    if (p->x > CP_WORLD_W - M)    ax -= (1.0f - (CP_WORLD_W - p->x) / M) * 1.3f;
    if (p->y < M)                 ay += (1.0f - p->y / M) * 1.3f;
    if (p->y > CP_WORLD_H - M)    ay -= (1.0f - (CP_WORLD_H - p->y) / M) * 1.3f;

    float n = sqrtf(ax * ax + ay * ay);
    if (n > 1.0f) { ax /= n; ay /= n; }

    memset(act, 0, sizeof(float) * CP_ACT_DIM);
    act[0] = ax;
    act[1] = ay;
    act[2] = (flee_w > 0.9f) ? 1.0f : 0.0f;             /* burst away when cornered */
    /* discharge is worth its health cost when more than one thing is on us,
     * or when a single attacker has us pinned */
    act[3] = (in_zap_range >= 2 || (in_zap_range >= 1 && flee_w > 0.6f)) ? 1.0f : 0.0f;

    /* design head: sampled by the sim only at a generation boundary */
    int next_gen = w->generation + 1;
    if (next_gen > CP_GENERATIONS - 1) next_gen = CP_GENERATIONS - 1;
    CpGenome g;
    cp_genome_autodesign(&g, NULL, CP_GEN_BUDGET[next_gen], style_of(st));
    encode_design(&g, act + CP_ACT_CTRL);
}
