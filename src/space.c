/* Stage 6 - SPACE. See space.h for the design. */
#include "cpore/space.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define PI 3.14159265358979f

/* ---- economy dials (tuned against the doctrine table, not intuition) ---- */
#define SHIP_SPEED       160.0f
#define SHIP_SPEED_UP     0.22f
#define FUEL_MAX         100.0f
#define FUEL_BURN         0.050f
#define FUEL_LIMP         0.12f
#define FUEL_COST         0.5f
#define HULL_MAX         100.0f
#define REPAIR_COST       0.8f
#define CARGO_CAP         20.0f
#define CARGO_CAP_UP     10.0f
#define WEAPONS_BASE      9.0f
#define WEAPONS_UP        0.50f
#define COLONY_COST      90.0f
#define COLONY_POP        14.0f
#define COLONY_DEF        40.0f
#define TAX_RATE          0.020f
#define SELL_ABOVE        30.0f
#define BUY_BELOW         26.0f
#define TRADE_LOTS        12.0f
#define TRADE_PULL        0.180f
#define TRIBUTE_PER_RUN   7.0f
#define SIEGE_BACKFIRE    0.55f
#define CAPTURE_LOOT      1.5f
#define UPGRADE_BASE    110.0f
#define UPGRADE_STEP     110.0f
#define UPGRADE_MAX         3
#define PIRATE_CHANCE     0.0045f
#define DEF_REGEN         0.30f
#define FORTIFY_RATE      2.2f
#define FORTIFY_COST      0.55f
#define DOCK_R            42.0f

static inline float clampf(float v, float a, float b)
{
    return v < a ? a : (v > b ? b : v);
}

static const char *BONUS[CP7_BONUS_COUNT] = { "colonise", "trade", "conquer" };

const char *cp7_bonus_name(int b)
{
    return (b >= 0 && b < CP7_BONUS_COUNT) ? BONUS[b] : "?";
}

void cp7_legacy_default(Cp7Legacy *out)
{
    for (int i = 0; i < CP7_BONUS_COUNT; i++) out->bonus[i] = 1.0f;
}

/* ------------------------------------------------------------------ */
/* the galaxy's address space                                          */
/* ------------------------------------------------------------------ */

uint32_t cp7_star_seed(uint32_t galaxy_seed, int star_index)
{
    if (star_index <= 0) return galaxy_seed;  /* the homeworld IS the arc's planet */
    uint32_t h = galaxy_seed ^ (0x9E3779B9u * (uint32_t)star_index);
    h ^= h >> 16; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

static float hash01(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t h = a ^ (0x9E3779B9u * b) ^ (0x85EBCA6Bu * c);
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFF) / 16777216.0f;
}

/* Two systems never quote the same price for a spice, which is what makes
 * hauling worth doing; a system discounts its own export, so the source of a
 * spice is where you fill the hold. */
static float spice_price(const Cp7World *w, int star, int type)
{
    return 16.0f + 26.0f * hash01(w->seed, (uint32_t)star * 7u + 1u,
                                  (uint32_t)type + 13u);
}

static float dist2f(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

static void ship_stats(Cp7Ship *s)
{
    s->speed     = SHIP_SPEED * (1.0f + SHIP_SPEED_UP * (float)s->up[CP7_UP_ENGINE]);
    s->fuel_max  = FUEL_MAX + 25.0f * (float)s->up[CP7_UP_ENGINE];
    s->cargo_cap = CARGO_CAP + CARGO_CAP_UP * (float)s->up[CP7_UP_CARGO];
    s->weapons   = WEAPONS_BASE * (1.0f + WEAPONS_UP * (float)s->up[CP7_UP_WEAPONS]);
    s->hull_max  = HULL_MAX + 30.0f * (float)s->up[CP7_UP_HULL];
}

static int docked_star(const Cp7World *w)
{
    int best = -1;
    float bd = DOCK_R * DOCK_R;
    for (int i = 0; i < w->n_stars; i++) {
        if (!w->star[i].alive) continue;
        float d = dist2f(w->star[i].x, w->star[i].y, w->ship.x, w->ship.y);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* reset                                                               */
/* ------------------------------------------------------------------ */

void cp7_world_reset(Cp7World *w, uint32_t seed, const Cp7Legacy *legacy)
{
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->seed = seed;
    cp_rng_seed(&w->rng, seed ^ 0xC0FFEEu);
    w->n_empires = CP7_MAX_EMPIRES;

    Cp7Legacy lg;
    if (legacy) lg = *legacy;
    else        cp7_legacy_default(&lg);
    for (int b = 0; b < CP7_BONUS_COUNT; b++)
        w->bonus[b] = clampf(lg.bonus[b], 0.70f, 1.80f);

    /* Two spiral arms with jitter. Stars are placed once and stored, because
     * the map is state the player flies across; it stays POD, so the
     * snapshot story is untouched. */
    w->n_stars = 24 + cp_rng_int(&w->rng, 10);
    for (int i = 0; i < w->n_stars; i++) {
        Cp7Star *s = &w->star[i];
        float t = (float)i / (float)w->n_stars;
        float ang = t * 3.6f * PI + (float)(i & 1) * PI
                  + cp_rng_range(&w->rng, -0.38f, 0.38f);
        float r = 210.0f + t * 880.0f + cp_rng_range(&w->rng, -70.0f, 70.0f);
        s->x = CP7_GW * 0.5f + cosf(ang) * r * 1.12f;
        s->y = CP7_GD * 0.5f + sinf(ang) * r * 0.86f;
        s->alive = 1;
        s->owner = -1;
        s->spice = (uint8_t)cp_rng_int(&w->rng, CP7_SPICE_TYPES);
        s->fertility = 0.6f + 0.8f * hash01(cp7_star_seed(seed, i), 7u, 11u);
    }
    /* Relax: no two systems inside 250 units. */
    for (int pass = 0; pass < 24; pass++) {
        for (int i = 0; i < w->n_stars; i++) {
            for (int j = i + 1; j < w->n_stars; j++) {
                float dx = w->star[j].x - w->star[i].x;
                float dy = w->star[j].y - w->star[i].y;
                float d2 = dx * dx + dy * dy;
                if (d2 > 62500.0f || d2 < 1e-6f) continue;
                float d = sqrtf(d2), push = (250.0f - d) * 0.5f / d;
                w->star[i].x -= dx * push; w->star[i].y -= dy * push;
                w->star[j].x += dx * push; w->star[j].y += dy * push;
            }
        }
    }
    for (int i = 0; i < w->n_stars; i++) {
        w->star[i].x = clampf(w->star[i].x, 120.0f, CP7_GW - 120.0f);
        w->star[i].y = clampf(w->star[i].y, 120.0f, CP7_GD - 120.0f);
    }

    /* The player holds the homeworld; every rival gets a home dealt at a
     * spread angle plus a founded second system. The rest of the galaxy is
     * free, which is what makes colonise a real opening. */
    w->star[0].owner = CP7_PLAYER;
    w->star[0].pop = 34.0f;
    w->star[0].defence = 70.0f;

    uint8_t taken[CP7_MAX_STARS];
    memset(taken, 0, sizeof(taken));
    taken[0] = 1;
    w->empire[CP7_PLAYER].money = 240.0f;
    w->empire[CP7_PLAYER].alive = 1;

    for (int n = 1; n < CP7_MAX_EMPIRES; n++) {
        Cp7Empire *e = &w->empire[n];
        e->money = 150.0f;
        e->alive = 1;
        e->style = (uint8_t)((n - 1) % CP7_BONUS_COUNT);
        e->rel = cp_rng_range(&w->rng, -0.15f, 0.25f);
        float want = ((float)(n - 1) + 0.5f) / (float)(CP7_MAX_EMPIRES - 1)
                   * 2.0f * PI;
        float wx = CP7_GW * 0.5f + cosf(want) * 720.0f;
        float wy = CP7_GD * 0.5f + sinf(want) * 720.0f;
        int best = -1;
        float bestd = 1e18f;
        for (int i = 1; i < w->n_stars; i++) {
            if (taken[i]) continue;
            float d = dist2f(w->star[i].x, w->star[i].y, wx, wy);
            if (d < bestd) { bestd = d; best = i; }
        }
        if (best < 0) best = n % w->n_stars;
        taken[best] = 1;
        w->star[best].owner = n;
        w->star[best].pop = cp_rng_range(&w->rng, 18.0f, 30.0f);
        w->star[best].defence = 45.0f;
        e->stars = 1;
        int second = -1;
        float snd = 1e18f;
        for (int i = 1; i < w->n_stars; i++) {
            if (taken[i] || i == best) continue;
            float d = dist2f(w->star[i].x, w->star[i].y,
                             w->star[best].x, w->star[best].y);
            if (d < snd) { snd = d; second = i; }
        }
        if (second >= 0 && snd < 384400.0f) {
            taken[second] = 1;
            w->star[second].owner = n;
            w->star[second].pop = cp_rng_range(&w->rng, 12.0f, 22.0f);
            w->star[second].defence = 30.0f;
            e->stars = 2;
        }
    }
    w->empire[CP7_PLAYER].stars = 1;

    w->ship.x = w->star[0].x;
    w->ship.y = w->star[0].y;
    w->ship.head = 0.0f;
    ship_stats(&w->ship);
    w->ship.fuel = w->ship.fuel_max;
    w->ship.hull = w->ship.hull_max;

    w->status = CP7_RUN;
}

/* ------------------------------------------------------------------ */
/* verbs                                                              */
/* ------------------------------------------------------------------ */

/* A system changes hands. `peaceful` is the commercial buyout: the people
 * arrive whole and their former owner was paid tribute along the way, so
 * they end the transaction better disposed than they started. Conquest is
 * the other thing: half the population is gone and every power noticed. */
static void take_star(Cp7World *w, Cp7Star *s, int by, int peaceful)
{
    int old = s->owner;
    if (old >= 0 && w->empire[old].stars > 0) {
        w->empire[old].stars--;
        w->empire[old].alive = w->empire[old].stars > 0;
    }
    s->owner = by;
    s->pull = 0.0f;
    s->hold = 140;                      /* a takeover needs time to settle */
    w->empire[by].stars++;
    w->empire[by].alive = 1;
    if (old == CP7_PLAYER) w->lost_stars++;
    if (peaceful) {
        /* bought: the people arrive intact, and the tribute you ran through
         * their space left them with a working garrison */
        if (s->defence < 70.0f) s->defence = 70.0f;
        if (old > CP7_PLAYER)
            w->empire[old].rel = clampf(w->empire[old].rel + 0.15f, -1.0f, 1.0f);
        if (by == CP7_PLAYER) w->flipped++;
   } else {
        s->pop *= 0.5f;
        s->defence = 35.0f;
        w->empire[by].money += s->pop * CAPTURE_LOOT;
        if (by == CP7_PLAYER) {
            /* conquest by the player: the victim remembers, and so does
             * everyone else */
            w->captured++;
            if (old > CP7_PLAYER) w->empire[old].rel = -1.0f;
            for (int n = 1; n < CP7_MAX_EMPIRES; n++)
                if (n != old)
                    w->empire[n].rel = clampf(w->empire[n].rel - 0.08f,
                                              -1.0f, 1.0f);
        }
    }
}

/* One trading tick at a system: sell whatever clears a profit here, fill the
 * hold with whatever is cheap here, and at a rival system every haul is a
 * little loyalty bought - counted against the EMPIRE, not the system, so a
 * route is a campaign, not a pot at one star. */
static void trade_tick(Cp7World *w, int i, float intensity)
{
    Cp7Star *s = &w->star[i];
    Cp7Empire *me = &w->empire[CP7_PLAYER];
    float moved = 0.0f, bought_here = 0.0f;
    for (int t = 0; t < CP7_SPICE_TYPES; t++) {
        if (w->ship.cargo[t] < 0.5f) continue;
        float p = spice_price(w, i, t);
        if (p <= SELL_ABOVE) continue;
        float q = w->ship.cargo[t] < TRADE_LOTS * intensity
                    ? w->ship.cargo[t] : TRADE_LOTS * intensity;
        w->ship.cargo[t] -= q;
        me->money += q * p;
        w->trade_earned += q * (p - 20.0f);
        moved += q;
    }
    float p = spice_price(w, i, s->spice);
    if (p < BUY_BELOW) {
        float used = 0.0f;
        for (int t = 0; t < CP7_SPICE_TYPES; t++) used += w->ship.cargo[t];
        float room = w->ship.cargo_cap - used;
        float qty = TRADE_LOTS * intensity;
        if (qty > room) qty = room;
        if (me->money < qty * p) qty = me->money / p;
        if (qty > 0.0f) {
            w->ship.cargo[s->spice] += qty;
            me->money -= qty * p;
            bought_here = qty;
        }
    }
    if (s->owner > CP7_PLAYER && (moved > 0.0f || bought_here > 0.0f)) {
        Cp7Empire *o = &w->empire[s->owner];
        o->pull += TRADE_PULL * w->bonus[CP7_TRADE] * intensity;
        if (moved > 0.0f) {
            o->money += TRIBUTE_PER_RUN;
            w->trade_runs++;
        }
        if (o->rel < 0.9f) o->rel += 0.09f;
        /* the buyout completes: the empire's weakest system joins you
         * intact, its people unspited, its former owner enriched by every
         * haul you ran through its space */
        if (o->pull >= 1.0f) {
            o->pull -= 1.0f;
            int weakest = -1;
            float wd = 1e18f;
            for (int k = 0; k < w->n_stars; k++) {
                const Cp7Star *t2 = &w->star[k];
                if (!t2->alive || t2->owner != s->owner) continue;
                if (t2->hold > 0) continue;
                if (t2->defence < wd) { wd = t2->defence; weakest = k; }
            }
            if (weakest >= 0) take_star(w, &w->star[weakest], CP7_PLAYER, 1);
            /* nothing free to flip right now: the loyalty is banked, not
             * burned - it pays out the moment their hold lifts */
            else if (o->pull > 0.9f) o->pull = 0.9f;
        }
        s->pull = o->pull;
    }
}

/* A rival settles the free system nearest its own centroid: the settler buys
 * a colony outright, the trader annexes with gold. */
static void rival_expand(Cp7World *w, int n)
{
    Cp7Empire *e = &w->empire[n];
    float cx = 0.0f, cz = 0.0f;
    int cnt = 0;
    for (int i = 0; i < w->n_stars; i++)
        if (w->star[i].alive && w->star[i].owner == n) {
            cx += w->star[i].x; cz += w->star[i].y; cnt++;
        }
    if (cnt <= 0) return;
    cx /= (float)cnt; cz /= (float)cnt;
    int best = -1;
    float bd = 1e18f;
    for (int i = 0; i < w->n_stars; i++) {
        if (!w->star[i].alive || w->star[i].owner >= 0) continue;
        float d = dist2f(w->star[i].x, w->star[i].y, cx, cz);
        if (d < bd) { bd = d; best = i; }
    }
    if (best < 0) return;
    w->star[best].owner = n;
    w->star[best].pop = COLONY_POP * w->star[best].fertility;
    w->star[best].defence = 20.0f;
    w->star[best].pull = 0.0f;
    w->star[best].pirates = 0.0f;
    e->stars++;
    e->alive = 1;
}

/* ------------------------------------------------------------------ */
/* step                                                               */
/* ------------------------------------------------------------------ */

void cp7_world_step(Cp7World *w, const float act[CP7_ACT_DIM])
{
    if (!w) return;
    if (w->status != CP7_RUN) { w->reward = 0.0f; return; }
    Cp7Empire *me = &w->empire[CP7_PLAYER];
    float money_before = me->money;
    int stars_before = me->stars;
    w->reward = -0.002f;

    /* ---- the ship: thrust burns fuel, a dry tank leaves you drifting ---- */
    {
        float tx = clampf(act[CP7_V_THRUST], -1.0f, 1.0f);
        float ty = clampf(act[CP7_V_THRUST + 1], -1.0f, 1.0f);
        float m = sqrtf(tx * tx + ty * ty);
        if (m > 1.0f) { tx /= m; ty /= m; }
        float spd = w->ship.speed * CP7_DT
                  * (w->ship.fuel > 0.0f ? 1.0f : FUEL_LIMP);
        float mvx = tx * spd, mvy = ty * spd;
        w->ship.x = clampf(w->ship.x + mvx, 20.0f, CP7_GW - 20.0f);
        w->ship.y = clampf(w->ship.y + mvy, 20.0f, CP7_GD - 20.0f);
        float dist = sqrtf(mvx * mvx + mvy * mvy);
        w->ship.fuel -= dist * FUEL_BURN
                      * (1.0f - 0.10f * (float)w->ship.up[CP7_UP_ENGINE]);
        if (w->ship.fuel < 0.0f) w->ship.fuel = 0.0f;
        if (m > 0.01f) w->ship.head = atan2f(ty, tx);
    }

    int dock = docked_star(w);

    /* ---- upgrades: one dial, four tracks, bought when flush ---- */
    if (dock >= 0 && fabsf(act[CP7_V_UPGRADE]) > 0.05f) {
        float d = act[CP7_V_UPGRADE];
        int track = d < -0.5f ? CP7_UP_ENGINE : d < 0.0f ? CP7_UP_CARGO
                  : d < 0.5f ? CP7_UP_WEAPONS : CP7_UP_HULL;
        if (w->ship.up[track] < UPGRADE_MAX) {
            float cost = UPGRADE_BASE + UPGRADE_STEP * (float)w->ship.up[track];
            if (me->money >= cost) {
                me->money -= cost;
                w->upgrades += cost;
                w->ship.up[track]++;
                ship_stats(&w->ship);
            }
        }
    }

    /* ---- the verbs, only while docked ---- */
    if (dock >= 0) {
        Cp7Star *s = &w->star[dock];
        if (act[CP7_V_TRADE] > 0.5f)
            trade_tick(w, dock, clampf(act[CP7_V_TRADE], 0.0f, 1.0f));
        if (act[CP7_V_COLONISE] > 0.5f && s->owner < 0
            && me->money >= COLONY_COST) {
            me->money -= COLONY_COST;
            s->owner = CP7_PLAYER;
            s->pop = COLONY_POP * s->fertility;
            s->defence = COLONY_DEF;
            s->pirates = 0.0f;
            s->pull = 0.0f;
            s->hold = 100;      /* a fresh colony gets time to stand up */
            me->stars++;
            w->settled++;
        }
        if (act[CP7_V_ATTACK] > 0.5f) {
            if (s->owner > CP7_PLAYER && s->hold == 0) {
                s->defence -= w->ship.weapons
                            * clampf(act[CP7_V_ATTACK], 0.0f, 1.0f)
                            * w->bonus[CP7_ATTACK];
                w->ship.hull -= SIEGE_BACKFIRE + s->defence * 0.012f;
                if (w->ship.hull < 0.0f) w->ship.hull = 0.0f;
                if (s->defence <= 0.0f) take_star(w, s, CP7_PLAYER, 0);
            } else if (s->owner < 0 && s->pirates > 0.0f) {
                s->pirates = 0.0f;
                w->raids_fought++;
                me->money += 25.0f;
            }
        }
        if (act[CP7_V_RESUPPLY] > 0.5f) {
            int friendly = s->owner == CP7_PLAYER || s->owner < 0
                        || (s->owner > CP7_PLAYER
                            && w->empire[s->owner].rel > -0.2f);
            if (friendly) {
                float m = clampf(act[CP7_V_RESUPPLY], 0.0f, 1.0f);
                float want = (w->ship.fuel_max - w->ship.fuel) * m;
                if (want > me->money / FUEL_COST) want = me->money / FUEL_COST;
                if (want > 0.0f) {
                    me->money -= want * FUEL_COST;
                    w->fuel_spent += want * FUEL_COST;
                    w->ship.fuel += want;
                }
                int ally = s->owner == CP7_PLAYER
                        || (s->owner > CP7_PLAYER
                            && w->empire[s->owner].rel > 0.5f);
                if (ally) {
                    float wr = (w->ship.hull_max - w->ship.hull) * m;
                    if (wr > me->money / REPAIR_COST) wr = me->money / REPAIR_COST;
                    if (wr > 0.0f) {
                        me->money -= wr * REPAIR_COST;
                        w->ship.hull += wr;
                    }
                }
                if (s->owner == CP7_PLAYER) {
                    float pts = FORTIFY_RATE * m;
                    if (pts > me->money / FORTIFY_COST) pts = me->money / FORTIFY_COST;
                    if (pts > 100.0f - s->defence) pts = 100.0f - s->defence;
                    if (pts > 0.0f) {
                        me->money -= pts * FORTIFY_COST;
                        s->defence += pts;
                    }
                }
            }
        }
    }

    /* ---- systems: tax, growth, defence, pirates ---- */
    for (int n = 0; n < w->n_empires; n++) w->empire[n].income = 0.0f;
    for (int i = 0; i < w->n_stars; i++) {
        Cp7Star *s = &w->star[i];
        if (!s->alive || s->owner < 0) continue;
        Cp7Empire *o = &w->empire[s->owner];
        s->pull = o->pull;                 /* loyalty is empire-wide now */
        float style_mult = 1.0f;
        if (s->owner != CP7_PLAYER)
            style_mult = o->style == CP7_TRADE ? 1.5f
                       : o->style == CP7_COLONISE ? 1.1f : 0.8f;
        float tax = s->pop * TAX_RATE * s->fertility * style_mult;
        if (s->pirates > 0.0f) tax *= 0.25f;
        o->money += tax;
        o->income += tax;
        if (o == me) w->tax_earned += tax;
        float cap = 60.0f * s->fertility;
        s->pop += s->pop * 0.0025f * (1.0f - s->pop / cap);
        /* the capital is a fortress: it regenerates what no colony can, so
         * the war that ends you has to go through the whole empire first */
        if (i == 0) s->defence += 1.5f;
        /* systems garrison themselves out of the treasury: defence is a
         * standing cost of empire, not a chore that needs the one ship in
         * the sky. Manual fortification is the fast, cheap option when the
         * ship is actually there. */
        if (s->defence < 80.0f) {
            /* a rich empire can afford thick walls; a poor one is soft.
             * Wealth buys the safety that lets a settler or a trader keep
             * what it grows, and a warlord's raid pays nothing against it. */
            float pts = 0.6f + o->money * 0.002f;
            if (pts > 3.2f) pts = 3.2f;
            if (pts > 100.0f - s->defence) pts = 100.0f - s->defence;
            if (pts * FORTIFY_COST > o->money) pts = o->money / FORTIFY_COST;
            if (pts > 0.0f) {
                o->money -= pts * FORTIFY_COST;
                s->defence += pts;
            }
        }
        if (s->hold > 0) s->hold--;
        if (s->defence < 60.0f + s->pop * 0.4f) s->defence += DEF_REGEN;
        if (s->pirates > 0.0f) {
            s->pirates *= 0.995f;
            s->pop -= s->pirates * 0.0006f;
            if (s->defence >= 45.0f) s->pirates -= 1.5f;
            if (s->pirates < 0.0f) s->pirates = 0.0f;
        } else if (s->pop > 4.0f && s->defence < 35.0f
                   && cp_rng_f(&w->rng) < PIRATE_CHANCE
                                        * (1.0f - s->defence / 40.0f)) {
            s->pirates = 120.0f + 100.0f * cp_rng_f(&w->rng);
            s->pop *= 0.85f;
        }
        if (s->pop < 1.0f) {
            /* a system nobody lives in is free again */
            o->stars--;
            s->owner = -1;
            s->pop = 0.0f;
            s->defence = 0.0f;
            s->pull = 0.0f;
            if (o == me) w->lost_stars++;
        }
        if (o->stars <= 0) o->alive = 0;
    }

    /* ---- rivals: economy and their own expansion ---- */
    for (int n = 1; n < CP7_MAX_EMPIRES; n++) {
        Cp7Empire *e = &w->empire[n];
        if (!e->alive) continue;
        if (e->style == CP7_TRADE) e->money += e->income * 0.4f + 0.35f;
        if (e->style == CP7_COLONISE && e->money >= COLONY_COST * 1.25f
            && (w->step + n * 37) % 200 == 0) {
            e->money -= COLONY_COST;
            rival_expand(w, n);
        }
        if (e->style == CP7_TRADE && e->money >= 300.0f
            && (w->step + n * 53) % 220 == 0) {
            e->money -= 260.0f;
            rival_expand(w, n);
        }
        if (e->style == CP7_ATTACK && e->rel < 0.35f && me->stars > 0
            && w->step > 400 + n * 40
            && (w->step + n * 71) % 200 < 100) {
            /* warlords besiege in campaigns, not at the opening bell: every
             * empire spends its first years consolidating what it has, and
             * even a war has pauses in which the defender regroups - the
             * same campaign pacing the tribe stage learned the hard way.
             * One front: the weakest occupied system they can reach, the
             * player preferred over the other powers. */
            float cx = 0.0f, cz = 0.0f;
            int cnt = 0;
            for (int i = 0; i < w->n_stars; i++)
                if (w->star[i].owner == n && w->star[i].alive) {
                    cx += w->star[i].x; cz += w->star[i].y; cnt++;
                }
            if (cnt > 0) {
                cx /= (float)cnt; cz /= (float)cnt;
                int tgt = -1;
                float bv = 1e18f;
                for (int i = 0; i < w->n_stars; i++) {
                    const Cp7Star *s = &w->star[i];
                    if (!s->alive || s->owner == n || s->owner < 0) continue;
                    if (s->hold > 0) continue;   /* let takeovers settle */
                    if (s->owner == CP7_PLAYER && e->rel > 0.20f) continue;
                    float v = s->defence
                            + sqrtf(dist2f(s->x, s->y, cx, cz)) * 0.02f
                            + (s->owner == CP7_PLAYER ? -12.0f : 0.0f)
                            + (i == 0 ? 30.0f : 0.0f);   /* the capital is taken last */
                    if (v < bv) { bv = v; tgt = i; }
                }
                if (tgt >= 0) {
                    Cp7Star *s = &w->star[tgt];
                    /* wars cost money to run; an empire that cannot pay
                     * cannot press a siege */
                    if (e->money > 120.0f) {
                        e->money -= 0.4f;
                        s->defence -= 1.2f + 0.0007f * (float)w->step;
                        if (s->owner == CP7_PLAYER) {
                            /* hostility deepens while the war runs, but no
                             * further than a peace can still be bought */
                            if (w->empire[n].rel > -0.6f)
                                w->empire[n].rel = clampf(
                                    w->empire[n].rel - 0.002f, -1.0f, 1.0f);
                        }
                        if (s->defence <= 0.0f) take_star(w, s, n, 0);
                    }
                }
            }
        }
        e->rel *= 0.9996f;
    }

    /* ---- ledger: counts are recounted so the map can never drift from the
     * numbers an audit reads ---- */
    {
        int cnt[CP7_MAX_EMPIRES];
        for (int n = 0; n < CP7_MAX_EMPIRES; n++) cnt[n] = 0;
        for (int i = 0; i < w->n_stars; i++)
            if (w->star[i].alive && w->star[i].owner >= 0)
                cnt[w->star[i].owner]++;
        for (int n = 0; n < CP7_MAX_EMPIRES; n++) {
            w->empire[n].stars = cnt[n];
            w->empire[n].alive = cnt[n] > 0;
        }
    }

    w->reward += 2.6f * (float)(me->stars - stars_before)
               + 0.0012f * (me->money - money_before);

    w->step++;

    if (w->ship.hull <= 0.0f && me->stars > 0) {
        /* the hull gave out: you limp home on the emergency cells, poorer,
         * and the game goes on - this is Spore's rule, not a permadeath
         * stage; only losing every system actually ends you */
        int home = 0;
        float hd = 1e18f;
        for (int i = 0; i < w->n_stars; i++) {
            const Cp7Star *s = &w->star[i];
            if (!s->alive || s->owner != CP7_PLAYER) continue;
            float d = dist2f(s->x, s->y, w->ship.x, w->ship.y);
            if (d < hd) { hd = d; home = i; }
        }
        w->ship.x = w->star[home].x;
        w->ship.y = w->star[home].y;
        w->ship.hull = 0.30f * w->ship.hull_max;
        me->money = me->money > 60.0f ? me->money - 60.0f : 0.0f;
    }

    if (me->stars <= 0) {
        w->status = CP7_LOST;
        w->reward -= 12.0f;
    } else if (me->stars * 5 >= w->n_stars * 4) {
        /* domination, not extermination: eight tenths of the galaxy under
         * one flag is what the stage calls a win */
        w->status = CP7_WON;
        w->reward += 30.0f;
    } else if (w->step >= CP7_MAX_STEPS) {
        w->status = CP7_TIMEOUT;
    }
}

/* ------------------------------------------------------------------ */
/* observation                                                        */
/* ------------------------------------------------------------------ */

void cp7_world_observe(const Cp7World *w, float *o)
{
    if (!w || !o) return;
    const Cp7Ship *sh = &w->ship;
    const Cp7Empire *me = &w->empire[CP7_PLAYER];

    /* the CP7_OBS_STARS nearest systems to the ship: what the captain can
     * reach today, not the whole galaxy at once */
    int sel[CP7_OBS_STARS];
    float bd[CP7_OBS_STARS];
    for (int k = 0; k < CP7_OBS_STARS; k++) { sel[k] = -1; bd[k] = 1e18f; }
    for (int i = 0; i < w->n_stars; i++) {
        if (!w->star[i].alive) continue;
        float d = dist2f(w->star[i].x, w->star[i].y, sh->x, sh->y);
        for (int k = 0; k < CP7_OBS_STARS; k++) {
            if (d < bd[k]) {
                for (int m = CP7_OBS_STARS - 1; m > k; m--) {
                    bd[m] = bd[m - 1]; sel[m] = sel[m - 1];
                }
                bd[k] = d; sel[k] = i;
                break;
            }
        }
    }

    int k = 0;
    for (int s = 0; s < CP7_OBS_STARS; s++) {
        if (sel[s] < 0) {
            for (int z = 0; z < CP7_OBS_STAR; z++) o[k++] = 0.0f;
            continue;
        }
        const Cp7Star *st = &w->star[sel[s]];
        o[k++] = st->owner < 0 ? 0.0f
               : st->owner == CP7_PLAYER ? 1.0f
               : 0.4f + 0.5f * (float)st->owner / (float)(CP7_MAX_EMPIRES - 1);
        o[k++] = clampf((st->x - sh->x) / CP7_GW * 2.0f, -1.5f, 1.5f);
        o[k++] = clampf((st->y - sh->y) / CP7_GD * 2.0f, -1.5f, 1.5f);
        o[k++] = sqrtf(dist2f(st->x, st->y, sh->x, sh->y)) / (CP7_GW * 0.75f);
        o[k++] = clampf(st->pop / 60.0f, 0.0f, 1.5f);
        o[k++] = clampf(st->defence / 100.0f, 0.0f, 1.2f);
        o[k++] = (float)st->spice / (float)CP7_SPICE_TYPES;
        o[k++] = spice_price(w, sel[s], st->spice) / 45.0f;
        o[k++] = clampf(st->pirates / 200.0f, 0.0f, 1.5f);
        o[k++] = clampf(st->pull, 0.0f, 1.0f);
    }

    float used = 0.0f;
    for (int t = 0; t < CP7_SPICE_TYPES; t++) used += sh->cargo[t];
    o[k++] = sh->fuel / sh->fuel_max;
    o[k++] = sh->hull / sh->hull_max;
    for (int t = 0; t < CP7_SPICE_TYPES; t++) o[k++] = sh->cargo[t] / sh->cargo_cap;
    o[k++] = used / sh->cargo_cap;
    o[k++] = clampf(me->money / 800.0f, 0.0f, 3.0f);
    for (int u = 0; u < CP7_UP_COUNT; u++) o[k++] = (float)sh->up[u] / 3.0f;

    for (int n = 1; n < CP7_MAX_EMPIRES; n++) {
        const Cp7Empire *e = &w->empire[n];
        o[k++] = e->alive ? clampf(e->rel, -1.0f, 1.0f) : 0.0f;
        o[k++] = (float)e->stars / (float)CP7_MAX_STARS;
        o[k++] = e->alive ? (float)e->style / (float)CP7_BONUS_COUNT : -1.0f;
    }

    o[k++] = (float)w->step / (float)CP7_MAX_STEPS;
    o[k++] = (float)me->stars / (float)w->n_stars;
    o[k++] = clampf(me->income / 50.0f, 0.0f, 3.0f);
    {
        float hostile = 0.0f;
        for (int n = 1; n < CP7_MAX_EMPIRES; n++)
            if (w->empire[n].alive && w->empire[n].rel < -0.2f) hostile += 1.0f;
        o[k++] = hostile / (float)(CP7_MAX_EMPIRES - 1);
    }
}

/* ------------------------------------------------------------------ */
/* the scripted captain                                               */
/* ------------------------------------------------------------------ */

/* Play to the species' strength, keep the ship fuelled and whole, and take
 * the nearest profitable action rather than the best one anywhere. It is not
 * clever; it is the floor a learned policy has to clear. */
void cp7_policy_greedy(const Cp7World *w, float act[CP7_ACT_DIM])
{
    if (!w || !act) return;
    for (int i = 0; i < CP7_ACT_DIM; i++) act[i] = 0.0f;

    const Cp7Ship *sh = &w->ship;
    const Cp7Empire *me = &w->empire[CP7_PLAYER];
    int dock = docked_star(w);

    int role = CP7_COLONISE;
    for (int b = 1; b < CP7_BONUS_COUNT; b++)
        if (w->bonus[b] > w->bonus[role]) role = b;

    int target = -1, verb = -1;

    /* emergencies beat doctrine: a dry tank, a holed hull, or a system that
     * is about to fall all dock first */
    {
        int weak = -1;
        float wd = 1e18f;
        for (int i = 0; i < w->n_stars; i++) {
            const Cp7Star *s = &w->star[i];
            if (!s->alive || s->owner != CP7_PLAYER) continue;
            if (s->hold > 0) continue;   /* a fresh colony stands up alone */
            if (s->defence < wd) { wd = s->defence; weak = i; }
        }
        if (weak >= 0 && wd < 50.0f) {
            target = weak;
            verb = CP7_V_RESUPPLY;
        }
    }
    if (target < 0 &&
        (sh->fuel < 0.22f * sh->fuel_max || sh->hull < 0.45f * sh->hull_max)) {
        float bd = 1e18f;
        for (int i = 0; i < w->n_stars; i++) {
            const Cp7Star *s = &w->star[i];
            if (!s->alive) continue;
            if (!(s->owner == CP7_PLAYER || s->owner < 0)) continue;
            float d = dist2f(s->x, s->y, sh->x, sh->y);
            if (d < bd) { bd = d; target = i; }
        }
        verb = CP7_V_RESUPPLY;
    }

    float used = 0.0f;
    int best_type = 0;
    for (int t = 0; t < CP7_SPICE_TYPES; t++) {
        used += sh->cargo[t];
        if (sh->cargo[t] > sh->cargo[best_type]) best_type = t;
    }

    /* patched up before the next siege, always */
    if (target < 0 && sh->hull < 0.6f * sh->hull_max) {
        float bd = 1e18f;
        for (int i = 0; i < w->n_stars; i++) {
            const Cp7Star *s = &w->star[i];
            if (!s->alive || s->owner != CP7_PLAYER) continue;
            float d = dist2f(s->x, s->y, sh->x, sh->y);
            if (d < bd) { bd = d; target = i; }
        }
        verb = CP7_V_RESUPPLY;
    }

    /* settle when flush, siege when armed, otherwise earn by hauling */
    if (target < 0) {
        int free_exists = 0;
        for (int i = 0; i < w->n_stars; i++)
            if (w->star[i].alive && w->star[i].owner < 0) { free_exists = 1; break; }
        /* settle when flush, whatever the doctrine: a trader's profit is
         * supposed to become colonies, and a conqueror banks nothing by
         * leaving money in the hold */
        int want_settle = me->money >= COLONY_COST * (role == CP7_COLONISE
                                                       ? 1.0f : 1.6f);
        int want_attack = (role == CP7_ATTACK
                           || (role == CP7_COLONISE && !free_exists
                               && me->money >= 200.0f));

        if (want_attack && me->money >= 120.0f && sh->hull > 0.6f * sh->hull_max) {
            float bv = 1e18f;
            for (int i = 0; i < w->n_stars; i++) {
                const Cp7Star *s = &w->star[i];
                if (!s->alive || s->owner <= CP7_PLAYER) continue;
                float v = s->defence + s->pop * 0.5f
                        + sqrtf(dist2f(s->x, s->y, sh->x, sh->y)) * 0.01f;
                if (v < bv) { bv = v; target = i; }
            }
            if (target >= 0) verb = CP7_V_ATTACK;
        } else if (want_settle && free_exists) {
            float bd = 1e18f;
            for (int i = 0; i < w->n_stars; i++) {
                const Cp7Star *s = &w->star[i];
                if (!s->alive || s->owner >= 0) continue;
                float d = dist2f(s->x, s->y, sh->x, sh->y);
                if (d < bd) { bd = d; target = i; }
            }
            if (target >= 0) verb = CP7_V_COLONISE;
        }
    }

    if (target < 0) {
        /* trade loop: haul the best-held spice to whoever quotes most - and
         * into a rival's system in preference, because commerce is also
         * diplomacy; or go fill the hold where the export is cheapest. Only
         * genuinely profitable endpoints count: a market that will not buy
         * or sell is not a port. */
        if (used > 0.55f * sh->cargo_cap) {
            float bv = -1e18f;
            for (int i = 0; i < w->n_stars; i++) {
                const Cp7Star *s = &w->star[i];
                if (!s->alive) continue;
                if (spice_price(w, i, best_type) <= SELL_ABOVE + 2.0f) continue;
                /* the most hostile owners are the ones worth buying: a
                 * route through their space is the peace treaty */
                float peace = 0.0f;
                if (s->owner > CP7_PLAYER)
                    peace = 45.0f + (1.0f - w->empire[s->owner].rel) * 40.0f;
                float v = spice_price(w, i, best_type) + peace
                        - sqrtf(dist2f(s->x, s->y, sh->x, sh->y)) * 0.008f;
                if (v > bv) { bv = v; target = i; }
            }
        } else {
            float bv = 1e18f;
            for (int i = 0; i < w->n_stars; i++) {
                const Cp7Star *s = &w->star[i];
                if (!s->alive) continue;
                if (spice_price(w, i, s->spice) >= BUY_BELOW - 2.0f) continue;
                float v = spice_price(w, i, s->spice)
                        + (s->owner > CP7_PLAYER ? -45.0f : 0.0f)
                        + sqrtf(dist2f(s->x, s->y, sh->x, sh->y)) * 0.008f;
                if (v < bv) { bv = v; target = i; }
            }
        }
        if (target >= 0) verb = CP7_V_TRADE;
    }

    /* nothing to colonise and nobody to fight: haul, or idle at home */
    if (target < 0) {
        float bd = 1e18f;
        for (int i = 0; i < w->n_stars; i++) {
            const Cp7Star *s = &w->star[i];
            if (!s->alive) continue;
            float d = dist2f(s->x, s->y, sh->x, sh->y);
            if (d < bd) { bd = d; target = i; }
        }
        verb = CP7_V_TRADE;
    }

    if (target >= 0) {
        const Cp7Star *s = &w->star[target];
        float dx = s->x - sh->x, dy = s->y - sh->y;
        float d = sqrtf(dx * dx + dy * dy);
        /* docked at the target: cut the engines, orbiting a star burns fuel
         * for nothing */
        if (target == dock) {
            act[CP7_V_THRUST] = 0.0f;
            act[CP7_V_THRUST + 1] = 0.0f;
            if (verb >= 0) act[verb] = 1.0f;
        } else if (d > 1.0f) {
            act[CP7_V_THRUST] = dx / d;
            act[CP7_V_THRUST + 1] = dy / d;
        }
        /* docked home with money to spare: keep the walls up, pirates and
         * warlords both bill you for forgetting */
        if (dock >= 0 && w->star[dock].owner == CP7_PLAYER
            && w->star[dock].defence < 85.0f && me->money > 100.0f)
            act[CP7_V_RESUPPLY] = 0.6f;
    }

    /* spend the surplus: engine for a settler, hold for a trader, guns for a
     * conqueror */
    if (me->money > 240.0f) {
        act[CP7_V_UPGRADE] = role == CP7_TRADE ? -0.25f
                           : role == CP7_ATTACK ? 0.25f : -0.75f;
    }
}

/* ------------------------------------------------------------------ */
/* the galaxy map                                                     */
/* ------------------------------------------------------------------ */

static const float ECOL[CP7_MAX_EMPIRES][3] = {
    { 0.55f, 0.95f, 1.00f },   /* player: cyan-white */
    { 1.00f, 0.62f, 0.35f },   /* orange */
    { 0.95f, 0.45f, 0.75f },   /* magenta */
    { 0.45f, 0.90f, 0.50f },   /* green */
    { 0.95f, 0.85f, 0.40f },   /* yellow */
    { 0.65f, 0.55f, 0.95f },   /* violet */
    { 0.95f, 0.40f, 0.40f },   /* red */
    { 0.45f, 0.75f, 0.95f },   /* blue */
};

static void px_line(uint8_t *fb, int W, int H, int x0, int y0, int x1, int y1,
                    float r, float g, float b, float a)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        cp_px_rect(fb, W, H, x0, y0, 1, 1, r, g, b, a);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void px_disc(uint8_t *fb, int W, int H, float cx, float cy, float rad,
                    float r, float g, float b, float a)
{
    int y0 = (int)(cy - rad), y1 = (int)(cy + rad);
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float half2 = rad * rad - dy * dy;
        if (half2 <= 0.0f) continue;
        int half = (int)sqrtf(half2);
        cp_px_rect(fb, W, H, (int)(cx - (float)half), y, half * 2 + 1, 1,
                   r, g, b, a);
    }
}

static void px_ring(uint8_t *fb, int W, int H, float cx, float cy, float rad,
                    float r, float g, float b, float a)
{
    int steps = (int)(rad * 8.0f) + 12;
    for (int i = 0; i < steps; i++) {
        float t = (float)i / (float)steps * 2.0f * PI;
        int x = (int)(cx + cosf(t) * rad);
        int y = (int)(cy + sinf(t) * rad);
        cp_px_rect(fb, W, H, x, y, 1, 1, r, g, b, a);
    }
}

void cp7_render(const Cp7World *w, uint8_t *rgba, int width, int height)
{
    if (!w || !rgba || width < 8 || height < 8) return;

    /* night: dust in two scales, and a faint nebular blotch here and there */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            uint8_t *p = rgba + ((size_t)y * width + x) * 4;
            float r = 0.016f, g = 0.022f, b = 0.045f;
            uint32_t cx = (uint32_t)x, cy = (uint32_t)y;
            float h = hash01(w->seed, cx, cy);
            if (h > 0.9985f) {
                float v = 0.35f + 0.55f * hash01(cx, cy, 3u);
                r += v; g += v; b += v * 1.2f;
            }
            float nb = hash01(w->seed ^ 7u, cx >> 4, cy >> 4);
            if (nb > 0.80f) {
                float t = (nb - 0.80f) / 0.20f;
                r += 0.018f * t; g += 0.011f * t; b += 0.050f * t;
            }
            p[0] = (uint8_t)(r * 255.0f);
            p[1] = (uint8_t)(g * 255.0f);
            p[2] = (uint8_t)(b * 255.0f);
            p[3] = 255;
        }
    }

    /* territory: a faint line between systems of the same power */
    for (int i = 0; i < w->n_stars; i++) {
        const Cp7Star *a = &w->star[i];
        if (!a->alive || a->owner < 0) continue;
        for (int j = i + 1; j < w->n_stars; j++) {
            const Cp7Star *b = &w->star[j];
            if (!b->alive || b->owner != a->owner) continue;
            if (dist2f(a->x, a->y, b->x, b->y) > 450.0f * 450.0f) continue;
            px_line(rgba, width, height,
                    (int)(a->x / CP7_GW * width), (int)(a->y / CP7_GD * height),
                    (int)(b->x / CP7_GW * width), (int)(b->y / CP7_GD * height),
                    ECOL[a->owner][0], ECOL[a->owner][1], ECOL[a->owner][2],
                    0.28f);
        }
    }

    for (int i = 0; i < w->n_stars; i++) {
        const Cp7Star *s = &w->star[i];
        if (!s->alive) continue;
        float sx = s->x / CP7_GW * (float)width;
        float sy = s->y / CP7_GD * (float)height;
        const float *col;
        float colv[3];
        if (s->owner < 0) { colv[0] = 0.40f; colv[1] = 0.44f; colv[2] = 0.50f; col = colv; }
        else col = ECOL[s->owner];
        float rad = 2.0f + s->pop * 0.06f + (s->owner == CP7_PLAYER ? 1.2f : 0.0f);
        if (rad > 8.0f) rad = 8.0f;
        px_disc(rgba, width, height, sx, sy, rad, col[0], col[1], col[2], 1.0f);
        /* a bright core so small systems still read */
        px_disc(rgba, width, height, sx, sy, rad * 0.4f, 1.0f, 1.0f, 1.0f, 0.8f);
        if (s->owner == CP7_PLAYER && i == 0)
            px_ring(rgba, width, height, sx, sy, rad + 4.0f, 1.0f, 1.0f, 1.0f, 0.7f);
        if (s->defence > 55.0f)
            px_ring(rgba, width, height, sx, sy, rad + 2.5f, col[0], col[1], col[2], 0.55f);
        if (s->pull > 0.0f)
            cp_px_rect(rgba, width, height, (int)sx - 4, (int)sy + (int)rad + 3,
                       (int)(8.0f * s->pull) + 1, 2, 1.0f, 1.0f, 1.0f, 0.85f);
        if (s->pirates > 0.0f && ((w->step / 20) & 1)) {
            cp_px_rect(rgba, width, height, (int)sx + (int)rad + 3,
                       (int)sy - (int)rad - 4, 2, 2, 1.0f, 0.25f, 0.2f, 0.95f);
        }
    }

    /* the ship: a bright wedge pointed down its heading */
    {
        float sx = w->ship.x / CP7_GW * (float)width;
        float sy = w->ship.y / CP7_GD * (float)height;
        float c = cosf(w->ship.head), sn = sinf(w->ship.head);
        int nx = (int)(sx + c * 8.0f),  ny = (int)(sy + sn * 8.0f);
        int lx = (int)(sx - c * 4.0f + sn * 4.0f);
        int ly = (int)(sy - sn * 4.0f - c * 4.0f);
        int rx = (int)(sx - c * 4.0f - sn * 4.0f);
        int ry = (int)(sy - sn * 4.0f + c * 4.0f);
        px_line(rgba, width, height, nx, ny, lx, ly, 0.55f, 0.95f, 1.0f, 1.0f);
        px_line(rgba, width, height, lx, ly, rx, ry, 0.55f, 0.95f, 1.0f, 1.0f);
        px_line(rgba, width, height, rx, ry, nx, ny, 0.55f, 0.95f, 1.0f, 1.0f);
        px_disc(rgba, width, height, sx, sy, 2.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    {
        char buf[160];
        const Cp7Empire *me = &w->empire[CP7_PLAYER];
        static const char *ST[] = { "", " LOST", " WON", " TIMEOUT" };
        snprintf(buf, sizeof(buf),
                 "SPACE%s  money %.0f  income %.1f  stars %d/%d  step %d",
                 ST[w->status], (double)me->money, (double)me->income,
                 me->stars, w->n_stars, w->step);
        cp_px_text(rgba, width, height, 6, 6, 1, buf, 0.85f, 0.92f, 1.0f, 1.0f);
        snprintf(buf, sizeof(buf),
                 "fuel %.0f%%  hull %.0f%%  cargo %.0f/%.0f  up E%d C%d W%d H%d",
                 (double)(w->ship.fuel / w->ship.fuel_max * 100.0f),
                 (double)(w->ship.hull / w->ship.hull_max * 100.0f),
                 (double)(w->ship.cargo[0] + w->ship.cargo[1]
                          + w->ship.cargo[2] + w->ship.cargo[3]),
                 (double)w->ship.cargo_cap, w->ship.up[0], w->ship.up[1],
                 w->ship.up[2], w->ship.up[3]);
        cp_px_text(rgba, width, height, 6, 16, 1, buf, 0.60f, 0.72f, 0.85f, 1.0f);
    }
}

void cp7_render_styled(const Cp7World *w, uint8_t *rgba, int width, int height,
                       int style)
{
    (void)style;   /* v1: one honest map tier; palette tier follows the civ path */
    cp7_render(w, rgba, width, height);
}
