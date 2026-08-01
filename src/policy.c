#include "cpore/cpore.h"
#include <math.h>

/* A hand-written baseline. Not smart - it has no memory, no planning and no
 * notion of the DNA goal - but it eats, and it runs away, which makes it the
 * bar a learned policy has to clear. */

void cp_policy_greedy(const CpWorld *w, float act[CP_ACT_DIM])
{
    const CpCell *p = &w->player;
    const CpStats *st = &w->stats;
    float ax = 0.0f, ay = 0.0f;

    /* threats first: sum repulsion from everything bigger and armed */
    float flee_x = 0.0f, flee_y = 0.0f, flee_w = 0.0f;
    float prey_x = 0.0f, prey_y = 0.0f, prey_d = 1e18f;

    for (int i = 0; i < CP_MAX_CELLS; i++) {
        const CpCell *c = &w->cells[i];
        if (!c->alive) continue;
        float dx = c->x - p->x, dy = c->y - p->y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < 0.01f) continue;

        /* Something we clearly outclass is lunch, not a threat. Without this
         * exclusion an armed build spends the whole episode retreating from
         * cells it could kill in a quarter of a second, and every carnivore
         * body plan looks non-viable. */
        int outclassed = (st->attack > 4.0f && p->r > c->r * 1.25f);
        int dangerous = (c->attack > 0.0f && c->r >= p->r * 0.70f && !outclassed);
        float reach = 130.0f + c->r * 2.6f;
        if (dangerous && d < reach) {
            float k = (reach - d) / reach;
            /* the bigger it is relative to us, the harder we run */
            k *= 0.6f + 0.9f * (c->r / p->r);
            flee_x -= dx / d * k; flee_y -= dy / d * k; flee_w += k;
        }
        /* something we can kill and turn into meat */
        if (outclassed && st->carn_eff > 0.0f && d < 300.0f && d < prey_d) {
            prey_d = d; prey_x = dx / d; prey_y = dy / d;
        }
    }

    /* nearest edible pellet - squared distances only, one sqrt at the end */
    float food_x = 0.0f, food_y = 0.0f, food_d2 = 1e18f;
    for (int i = 0; i < CP_MAX_FOOD; i++) {
        const CpFood *f = &w->food[i];
        if (f->type == CP_FOOD_NONE) continue;
        if (f->type == CP_FOOD_PLANT ? st->herb_eff <= 0.0f : st->carn_eff <= 0.0f) continue;
        float dx = f->x - p->x, dy = f->y - p->y;
        float d2 = dx * dx + dy * dy;
        if (d2 < food_d2) { food_d2 = d2; food_x = dx; food_y = dy; }
    }
    float food_d = 1e18f;
    if (food_d2 < 1e17f) {
        food_d = sqrtf(food_d2);
        if (food_d > 0.001f) { food_x /= food_d; food_y /= food_d; }
    }

    /* A hungry predator should hunt harder, not give up. The obvious gate
     * here ("only chase prey above 55%% health") puts carnivore builds into a
     * starvation spiral: low health stops them hunting, so they only scavenge,
     * so they stay at low health. Hunt whenever there is nothing easier. */
    int starving = p->hp < p->hp_max * 0.35f;
    int hunt = prey_d < 1e17f && (food_d > prey_d * 1.5f || !starving);

    if (flee_w > 0.35f) {
        ax = flee_x; ay = flee_y;                       /* survival beats lunch */
    } else if (hunt) {
        ax = prey_x; ay = prey_y;
    } else if (food_d < 1e17f) {
        ax = food_x + flee_x * 0.5f;
        ay = food_y + flee_y * 0.5f;
    }

    /* Stay off the walls - but smoothly. A constant push that switches on at a
     * fixed distance exactly cancels the (unit-length) target attraction, and
     * the agent parks on that threshold forever. Sparse-target builds starved
     * to death in a corner with prey 100px away. Ramp it instead, so the push
     * is zero at the trigger distance and only wins right against the wall. */
    const float M = 140.0f;
    if (p->x < M)                 ax += (1.0f - p->x / M) * 1.3f;
    if (p->x > CP_WORLD_W - M)    ax -= (1.0f - (CP_WORLD_W - p->x) / M) * 1.3f;
    if (p->y < M)                 ay += (1.0f - p->y / M) * 1.3f;
    if (p->y > CP_WORLD_H - M)    ay -= (1.0f - (CP_WORLD_H - p->y) / M) * 1.3f;

    float n = sqrtf(ax * ax + ay * ay);
    if (n > 1.0f) { ax /= n; ay /= n; }

    act[0] = ax;
    act[1] = ay;
    act[2] = (flee_w > 0.9f) ? 1.0f : 0.0f;             /* burst away when cornered */
}
