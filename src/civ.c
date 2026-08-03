#include "cpore/civ.h"
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ *
 * Stage 4 - CIVILISATION.
 *
 * The unit of play stops being one animal and becomes a nation. What carries
 * over is the planet - cp4_height() from stage 3, same seed, same hills - and
 * the species' strengths, which arrive as three multipliers rather than as a
 * body plan.
 *
 * The three approaches have to be genuinely different or the stage is one
 * mechanic wearing three hats:
 *
 *   military  - fast, spends the target's defence, and the pop it captures is
 *               halved. Cheap per unit, expensive in what it destroys.
 *   economic  - slow to arrive but pays for itself, buys allegiance directly
 *               and leaves the city intact. Needs money you do not have early.
 *   religious - no damage at all; parks outside and converts over time, which
 *               a garrison can interrupt. The patient answer.
 *
 * Same contract as every other stage: POD world, RNG inside the struct, no
 * allocation, no I/O, no globals.
 * ------------------------------------------------------------------ */

#define PI            3.14159265358979f
#define CITY_DEF_MAX  100.0f
#define CITY_ALG_MAX  100.0f
#define UNIT_SPEED    120.0f
#define ARRIVE_R       40.0f

static const float UNIT_COST[CP5_APPROACH_COUNT]  = { 26.0f, 42.0f, 34.0f };
static const float UNIT_HP[CP5_APPROACH_COUNT]    = { 46.0f, 22.0f, 28.0f };
static const float UNIT_SPD[CP5_APPROACH_COUNT]   = { 1.15f, 0.72f, 0.90f };

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static const char *APPROACH[CP5_APPROACH_COUNT] = { "military", "economic", "religious" };

const char *cp5_approach_name(int a)
{
    return (a >= 0 && a < CP5_APPROACH_COUNT) ? APPROACH[a] : "?";
}

/* ---------------- what stage 3 hands forward ---------------- */

/* A body that won by biting is a military power; one that won by singing is a
 * missionary power; one that fed itself efficiently is a trading power. This
 * is the only place the two stages are coupled, and it is deliberately a
 * three-float summary rather than a shared struct. */
void cp5_legacy_from_creature(const Cp4Genome *g, Cp5Legacy *out)
{
    Cp4Stats s;
    cp4_genome_stats(g, &s);
    float mil = (s.bite + s.claw_dmg) / 55.0f + s.armor * 0.6f;
    float rel = s.charm / 2.2f;
    float eco = (s.graze_eff + s.carn_eff) / 2.4f + (float)s.n[CP4_LEG] * 0.05f;
    out->bonus[CP5_MIL] = clampf(0.80f + mil * 0.45f, 0.80f, 1.60f);
    out->bonus[CP5_ECO] = clampf(0.80f + eco * 0.45f, 0.80f, 1.60f);
    out->bonus[CP5_REL] = clampf(0.80f + rel * 0.45f, 0.80f, 1.60f);
}

void cp5_legacy_default(Cp5Legacy *out)
{
    for (int i = 0; i < CP5_APPROACH_COUNT; i++) out->bonus[i] = 1.0f;
}

/* ---------------- geography ---------------- */

static float dist2(Cp5Vec a, Cp5Vec b)
{
    float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

/* A city site has to be dry, and far enough from every other city that the map
 * is a set of places rather than one blob. */
static int site_ok(const Cp5World *w, Cp5Vec p, float sep)
{
    if (cp4_height(w->seed, p.x, p.z) > CP5_SEA - 12.0f) return 0;
    for (int i = 0; i < w->n_cities; i++)
        if (dist2(w->city[i].p, p) < sep * sep) return 0;
    return 1;
}

/* ---------------- reset ---------------- */

void cp5_world_reset(Cp5World *w, uint32_t seed, const Cp5Legacy *legacy)
{
    memset(w, 0, sizeof(*w));
    w->seed = seed;
    cp_rng_seed(&w->rng, seed);

    Cp5Legacy lg;
    if (legacy) lg = *legacy;
    else        cp5_legacy_default(&lg);

    w->n_nations = CP5_MAX_NATIONS;
    for (int n = 0; n < CP5_MAX_NATIONS; n++) {
        Cp5Nation *na = &w->nation[n];
        na->money = 120.0f;
        na->alive = 1;
        if (n == CP5_PLAYER) {
            for (int a = 0; a < CP5_APPROACH_COUNT; a++) na->bonus[a] = lg.bonus[a];
            na->style = CP5_MIL;
        } else {
            /* rivals are specialists: a nation that does everything is a
             * nation with no character, and nothing to counter */
            na->style = (uint8_t)(n % CP5_APPROACH_COUNT);
            for (int a = 0; a < CP5_APPROACH_COUNT; a++)
                na->bonus[a] = (a == na->style) ? cp_rng_range(&w->rng, 1.15f, 1.45f)
                                                : cp_rng_range(&w->rng, 0.80f, 1.00f);
        }
    }

    /* Lay out cities on the dry ground of the stage-3 world. The player gets
     * the first site; the rest are dealt round-robin so no nation starts with
     * a numerical advantage. */
    for (int tries = 0; tries < 4000 && w->n_cities < CP5_MAX_CITIES; tries++) {
        Cp5Vec p;
        p.x = cp_rng_range(&w->rng, 140.0f, CP5_W - 140.0f);
        p.z = cp_rng_range(&w->rng, 140.0f, CP5_D - 140.0f);
        float sep = tries < 3000 ? 380.0f : 240.0f;
        if (!site_ok(w, p, sep)) continue;
        Cp5City *c = &w->city[w->n_cities];
        memset(c, 0, sizeof(*c));
        c->p = p;
        c->owner = w->n_cities < CP5_MAX_NATIONS ? w->n_cities : -1;   /* -1: free city */
        c->pop = cp_rng_range(&w->rng, 26.0f, 46.0f);
        c->defence = CITY_DEF_MAX * cp_rng_range(&w->rng, 0.55f, 0.85f);
        c->allegiance = CITY_ALG_MAX * (c->owner < 0 ? 0.45f : 0.85f);
        c->contest = -1;
        c->alive = 1;
        w->n_cities++;
    }

    /* Spice, on the geysers between the cities. Owning the ground near one is
     * what makes a city worth more than its population. */
    for (int tries = 0; tries < 3000 && w->n_spice < CP5_MAX_CITIES; tries++) {
        Cp5Vec p;
        p.x = cp_rng_range(&w->rng, 90.0f, CP5_W - 90.0f);
        p.z = cp_rng_range(&w->rng, 90.0f, CP5_D - 90.0f);
        if (cp4_height(w->seed, p.x, p.z) > CP5_SEA - 8.0f) continue;
        int clash = 0;
        for (int i = 0; i < w->n_spice; i++)
            if (dist2(w->spice[i], p) < 220.0f * 220.0f) clash = 1;
        if (clash) continue;
        w->spice[w->n_spice++] = p;
    }

    /* a city banks whatever spice lies inside its working radius */
    for (int i = 0; i < w->n_cities; i++)
        for (int k = 0; k < w->n_spice; k++)
            if (dist2(w->city[i].p, w->spice[k]) < 300.0f * 300.0f)
                w->city[i].spice++;

    for (int i = 0; i < w->n_cities; i++)
        if (w->city[i].owner >= 0) w->nation[w->city[i].owner].cities++;

    w->status = CP5_RUN;
}

/* ---------------- units ---------------- */

static int spawn_unit(Cp5World *w, int owner, int type, Cp5Vec at, int target)
{
    for (int i = 0; i < CP5_MAX_UNITS; i++) {
        Cp5Unit *u = &w->unit[i];
        if (u->alive) continue;
        memset(u, 0, sizeof(*u));
        u->p = at;
        u->owner = owner;
        u->type = (uint8_t)type;
        u->hp = UNIT_HP[type];
        u->target = target;
        u->alive = 1;
        return i;
    }
    return -1;
}

/* The city a nation should work on next. Everyone uses this - the rivals as
 * their whole strategy, the player only when the policy leaves the target
 * priorities blank. */
static int pick_target(const Cp5World *w, int nation, int approach)
{
    int best = -1;
    float bestv = -1e18f;
    for (int i = 0; i < w->n_cities; i++) {
        const Cp5City *c = &w->city[i];
        if (!c->alive || c->owner == nation) continue;
        /* nearest own city, so a target across the map is worth less */
        float near = 1e18f;
        for (int k = 0; k < w->n_cities; k++) {
            if (!w->city[k].alive || w->city[k].owner != nation) continue;
            float d2 = dist2(w->city[k].p, c->p);
            if (d2 < near) near = d2;
        }
        if (near > 1e17f) near = 1e6f;
        float resist = (approach == CP5_MIL) ? c->defence : c->allegiance;
        float v = (c->pop * 0.6f + (float)c->spice * 22.0f)
                - resist * 0.55f - sqrtf(near) * 0.05f;
        if (c->owner < 0) v += 30.0f;          /* free cities are the cheap win */
        if (v > bestv) { bestv = v; best = i; }
    }
    return best;
}

/* ---------------- the three approaches ---------------- */

/* Military: grind the garrison down, then take what is left of the city.
 * Capturing by force costs the city half its population, which is the whole
 * reason the other two approaches are worth their higher price. */
static void resolve_military(Cp5World *w, Cp5Unit *u, Cp5City *c)
{
    Cp5Nation *na = &w->nation[u->owner];
    float dmg = 5.0f * na->bonus[CP5_MIL];
    float garrison = 4.5f + c->pop * 0.05f;
    c->defence -= dmg;
    u->hp -= garrison;
    c->unrest = clampf(c->unrest + 0.05f, 0.0f, 1.0f);
    if (c->defence <= 0.0f) {
        int old = c->owner;
        if (old >= 0 && w->nation[old].cities > 0) w->nation[old].cities--;
        if (old >= 0) w->nation[old].lost++;
        c->owner = u->owner;
        c->pop *= 0.5f;
        c->defence = CITY_DEF_MAX * 0.25f;
        c->allegiance = CITY_ALG_MAX * 0.35f;
        c->contest = -1;
        c->convert = 0.0f;
        w->nation[u->owner].cities++;
        w->nation[u->owner].taken++;
        u->alive = 0;
        if (u->owner == CP5_PLAYER) w->captured++;
        else if (old == CP5_PLAYER) w->cities_lost++;
    }
    if (u->hp <= 0.0f) u->alive = 0;
}

/* Economic: a trade ship arrives with cargo, spends it on allegiance, and goes
 * home richer. It never damages anything, so the city it flips arrives whole. */
static void resolve_economic(Cp5World *w, Cp5Unit *u, Cp5City *c)
{
    Cp5Nation *na = &w->nation[u->owner];
    /* One delivery has to be worth the round trip. At 6.5 a hundred points of
     * allegiance needed fifteen arrivals and outpaced nothing - trade flipped
     * exactly zero cities across every seed. */
    float pull = 26.0f * na->bonus[CP5_ECO];
    c->allegiance -= pull;
    na->money += 10.0f + (float)c->spice * 4.0f;     /* the route pays */
    if (c->allegiance <= 0.0f) {
        int old = c->owner;
        if (old >= 0 && w->nation[old].cities > 0) w->nation[old].cities--;
        if (old >= 0) w->nation[old].lost++;
        c->owner = u->owner;
        c->allegiance = CITY_ALG_MAX * 0.55f;
        c->contest = -1;
        c->convert = 0.0f;
        w->nation[u->owner].cities++;
        w->nation[u->owner].taken++;
        if (u->owner == CP5_PLAYER) w->bought++;
        else if (old == CP5_PLAYER) w->cities_lost++;
    }
    u->alive = 0;
}

/* Religious: the unit parks outside and preaches. It does no damage and takes
 * none, but only one nation can hold a conversion at a time, so a rival
 * arriving resets the work - which is what makes this the patient answer
 * rather than simply the cheap one. */
static void resolve_religious(Cp5World *w, Cp5Unit *u, Cp5City *c)
{
    Cp5Nation *na = &w->nation[u->owner];
    if (c->contest != u->owner) {
        if (c->convert > 0.0f) { c->convert = 0.0f; }
        c->contest = u->owner;
    }
    /* A garrison is a real answer to a missionary, in both directions: walls
     * slow the preaching down and they wear the preacher out. Without this the
     * religious approach was strictly better than the other two - it took no
     * damage, never left, and swept whole maps unopposed. */
    float walls = c->defence / CITY_DEF_MAX;
    c->convert += 0.040f * na->bonus[CP5_REL] * (1.0f + c->unrest) * (1.0f - 0.50f * walls);
    c->allegiance -= 1.4f * na->bonus[CP5_REL];
    u->hp -= 0.40f + c->defence * 0.012f;
    if (u->hp <= 0.0f) { u->alive = 0; return; }
    if (c->convert >= 1.0f) {
        int old = c->owner;
        if (old >= 0 && w->nation[old].cities > 0) w->nation[old].cities--;
        if (old >= 0) w->nation[old].lost++;
        c->owner = u->owner;
        c->allegiance = CITY_ALG_MAX * 0.75f;   /* converts stay converted */
        c->convert = 0.0f;
        c->contest = -1;
        c->unrest = 0.0f;
        w->nation[u->owner].cities++;
        w->nation[u->owner].taken++;
        u->alive = 0;
        if (u->owner == CP5_PLAYER) w->converted++;
        else if (old == CP5_PLAYER) w->cities_lost++;
    }
}

/* ---------------- step ---------------- */

/* Build at most one unit per call. nation_build is invoked once per city per
 * tick, so a large empire really does out-produce a small one instead of both
 * being capped at one unit a tick. */
static void nation_build(Cp5World *w, int n, const float mix[CP5_APPROACH_COUNT],
                         const float *prio)
{
    Cp5Nation *na = &w->nation[n];
    if (!na->alive || na->cities <= 0) return;

    /* Which of its own cities is producing this tick. Round-robin by step, so
     * a big empire builds proportionally faster without needing a per-city
     * timer. */
    int home = -1, seen = 0;
    int want = w->step % (na->cities > 0 ? na->cities : 1);
    for (int i = 0; i < w->n_cities; i++) {
        if (!w->city[i].alive || w->city[i].owner != n) continue;
        if (seen++ == want) { home = i; break; }
    }
    if (home < 0) return;

    /* pick the approach by the requested mix, biased by what the nation is
     * actually good at - a mix of zeroes falls back to its own style */
    int type = -1;
    float bestv = -1.0f;
    for (int a = 0; a < CP5_APPROACH_COUNT; a++) {
        float v = clampf(mix[a], 0.0f, 1.0f) * na->bonus[a];
        if (v > bestv) { bestv = v; type = a; }
    }
    if (bestv <= 0.001f) type = na->style;

    if (na->money < UNIT_COST[type]) return;

    int target = -1;
    if (prio) {
        float bp = -1e18f;
        for (int i = 0; i < w->n_cities; i++) {
            if (!w->city[i].alive || w->city[i].owner == n) continue;
            if (prio[i] > bp) { bp = prio[i]; target = i; }
        }
        if (bp <= -0.999f) target = -1;      /* the agent asked for nobody */
    }
    if (target < 0) target = pick_target(w, n, type);
    if (target < 0) return;

    na->money -= UNIT_COST[type];
    na->built++;
    w->spent[type] += UNIT_COST[type];
    if (n == CP5_PLAYER) w->units_built[type]++;
    spawn_unit(w, n, type, w->city[home].p, target);
}

void cp5_world_step(Cp5World *w, const float act[CP5_ACT_DIM])
{
    if (w->status != CP5_RUN) { w->reward = 0.0f; return; }
    const float dt = CP5_DT;
    float reward = -0.002f;
    int cities_before = w->nation[CP5_PLAYER].cities;
    float money_before = w->nation[CP5_PLAYER].money;

    float mix[CP5_APPROACH_COUNT];
    for (int a = 0; a < CP5_APPROACH_COUNT; a++) mix[a] = clampf(act[a], 0.0f, 1.0f);
    const float *prio = act + CP5_APPROACH_COUNT;
    /* tax: more money now, more unrest later - and unrest is what makes a city
     * easy for somebody else to convert */
    float tax = clampf(act[CP5_APPROACH_COUNT + CP5_MAX_CITIES] * 0.5f + 0.5f, 0.0f, 1.0f);
    float posture = clampf(act[CP5_APPROACH_COUNT + CP5_MAX_CITIES + 1] * 0.5f + 0.5f, 0.0f, 1.0f);

    /* ---- cities: population, income, defence, loyalty ---- */
    /* Clear the income accumulator *before* filling it. This used to sit after
     * the loop, which meant every reader - the HUD, the observation, and the
     * baseline's decision about whether it could afford to save up - saw a
     * permanent zero, and the baseline fell back to the cheapest unit forever.
     * A trading species spent the whole stage building soldiers. */
    for (int n = 0; n < w->n_nations; n++) w->nation[n].income = 0.0f;

    for (int i = 0; i < w->n_cities; i++) {
        Cp5City *c = &w->city[i];
        if (!c->alive) continue;
        int owned = c->owner >= 0;
        float t = owned && c->owner == CP5_PLAYER ? tax : 0.55f;

        c->pop = clampf(c->pop + (0.9f + 0.35f * (float)c->spice) * (1.0f - t * 0.45f) * dt,
                        4.0f, 120.0f);
        if (owned) {
            Cp5Nation *na = &w->nation[c->owner];
            float inc = (c->pop * 0.11f + (float)c->spice * 1.5f)
                      * (0.45f + 0.95f * t) * na->bonus[CP5_ECO];
            na->money += inc * dt;
            na->income += inc;
        }
        /* Defence regrows toward whatever the posture asks for, and posture is
         * a real trade: a nation turtling behind full walls is not out
         * building anything. */
        float want_def = CITY_DEF_MAX * (owned && c->owner == CP5_PLAYER
                                         ? 0.45f + 0.55f * posture : 0.85f);
        /* Last stand. A nation down to one city digs in, which stops the stage
         * turning on whoever happened to get jumped first: elimination should
         * be the end of a losing campaign, not an early accident. */
        int last = owned && w->nation[c->owner].cities <= 1;
        if (last && want_def < CITY_DEF_MAX * 0.90f) want_def = CITY_DEF_MAX * 0.90f;
        /* Walls rebuild slowly. At 0.28 they regrew faster than a besieging
         * nation could deliver units, so military conquest was arithmetically
         * impossible for anyone who did not already own half the map - the
         * player sat on one city with nineteen troops in the field and took
         * nothing for three thousand ticks. */
        c->defence = clampf(c->defence + (want_def - c->defence) * 0.055f * dt,
                            0.0f, CITY_DEF_MAX);
        c->unrest = clampf(c->unrest + (t - 0.55f) * 0.06f * dt - 0.02f * dt, 0.0f, 1.0f);
        c->allegiance = clampf(c->allegiance
                               + (CITY_ALG_MAX * (owned ? 0.9f : 0.5f) - c->allegiance)
                                 * (last ? 0.090f : 0.035f) * dt * (1.0f - c->unrest * 0.7f),
                               0.0f, CITY_ALG_MAX);
        /* a conversion nobody is tending decays back */
        if (c->contest >= 0) {
            c->convert -= 0.05f * dt;
            if (c->convert <= 0.0f) { c->convert = 0.0f; c->contest = -1; }
        }
    }

    /* ---- production ---- */
    for (int n = 0; n < w->n_nations; n++) {
        Cp5Nation *na = &w->nation[n];
        if (!na->alive) continue;
        int rounds = 1 + na->cities / 3;
        for (int k = 0; k < rounds; k++) {
            if (n == CP5_PLAYER) {
                nation_build(w, n, mix, prio);
            } else {
                float m[CP5_APPROACH_COUNT] = { 0.0f, 0.0f, 0.0f };
                m[na->style] = 1.0f;
                nation_build(w, n, m, NULL);
            }
        }
    }

    /* ---- units move and resolve ---- */
    for (int i = 0; i < CP5_MAX_UNITS; i++) {
        Cp5Unit *u = &w->unit[i];
        if (!u->alive) continue;
        if (u->target < 0 || u->target >= w->n_cities || !w->city[u->target].alive) {
            u->alive = 0;
            continue;
        }
        Cp5City *c = &w->city[u->target];
        /* a target that became ours on the way home is no longer a target */
        if (c->owner == u->owner) { u->alive = 0; continue; }

        float dx = c->p.x - u->p.x, dz = c->p.z - u->p.z;
        float d = sqrtf(dx * dx + dz * dz);
        if (d > ARRIVE_R) {
            float step = UNIT_SPEED * UNIT_SPD[u->type] * dt;
            if (step > d - ARRIVE_R) step = d - ARRIVE_R;
            u->p.x += dx / d * step;
            u->p.z += dz / d * step;
            continue;
        }

        switch (u->type) {
        case CP5_MIL: resolve_military(w, u, c); break;
        case CP5_ECO: resolve_economic(w, u, c); break;
        default:      resolve_religious(w, u, c); break;
        }
    }

    /* ---- book-keeping ---- */
    {
        int alive_nations = 0;
        for (int n = 0; n < w->n_nations; n++) {
            w->nation[n].alive = w->nation[n].cities > 0;
            if (w->nation[n].alive) alive_nations++;
        }
        int taken = w->nation[CP5_PLAYER].cities - cities_before;
        reward += 2.2f * (float)taken;
        reward += 0.0015f * (w->nation[CP5_PLAYER].money - money_before);
        (void)alive_nations;
    }

    w->step++;
    if (w->nation[CP5_PLAYER].cities <= 0) {
        w->status = CP5_LOST;
        reward -= 12.0f;
    } else if (w->nation[CP5_PLAYER].cities >= w->n_cities) {
        w->status = CP5_WON;
        reward += 30.0f;
    } else if (w->step >= CP5_MAX_STEPS) {
        w->status = CP5_TIMEOUT;
    }
    w->reward = reward;
}

/* ---------------- observation ---------------- */

void cp5_world_observe(const Cp5World *w, float *o)
{
    const Cp5Nation *me = &w->nation[CP5_PLAYER];
    int k = 0;

    /* the player's centre of gravity, so city offsets mean something */
    float cx = CP5_W * 0.5f, cz = CP5_D * 0.5f;
    {
        float sx = 0.0f, sz = 0.0f;
        int n = 0;
        for (int i = 0; i < w->n_cities; i++)
            if (w->city[i].alive && w->city[i].owner == CP5_PLAYER) {
                sx += w->city[i].p.x; sz += w->city[i].p.z; n++;
            }
        if (n) { cx = sx / n; cz = sz / n; }
    }

    for (int i = 0; i < CP5_MAX_CITIES; i++) {
        if (i >= w->n_cities || !w->city[i].alive) {
            for (int z = 0; z < CP5_OBS_CITY; z++) o[k++] = 0.0f;
            continue;
        }
        const Cp5City *c = &w->city[i];
        o[k++] = c->owner == CP5_PLAYER ? 1.0f : 0.0f;
        o[k++] = c->owner < 0 ? 1.0f : 0.0f;
        o[k++] = (c->owner > CP5_PLAYER) ? (float)c->owner / (float)CP5_MAX_NATIONS : 0.0f;
        o[k++] = c->pop / 120.0f;
        o[k++] = c->defence / CITY_DEF_MAX;
        o[k++] = c->allegiance / CITY_ALG_MAX;
        o[k++] = (float)c->spice * 0.25f;
        o[k++] = clampf((c->p.x - cx) / CP5_W, -1.5f, 1.5f);
        o[k++] = clampf((c->p.z - cz) / CP5_D, -1.5f, 1.5f);
    }

    o[k++] = clampf(me->money / 400.0f, 0.0f, 3.0f);
    o[k++] = clampf(me->income / 40.0f, 0.0f, 3.0f);
    o[k++] = (float)me->cities / (float)CP5_MAX_CITIES;
    for (int a = 0; a < CP5_APPROACH_COUNT; a++) o[k++] = me->bonus[a];
    /* units in the field, by type, so the agent can see its own commitments */
    {
        float n[CP5_APPROACH_COUNT] = { 0.0f, 0.0f, 0.0f };
        for (int i = 0; i < CP5_MAX_UNITS; i++)
            if (w->unit[i].alive && w->unit[i].owner == CP5_PLAYER)
                n[w->unit[i].type] += 1.0f;
        for (int a = 0; a < CP5_APPROACH_COUNT; a++) o[k++] = n[a] * 0.1f;
    }
    o[k++] = (float)w->step / (float)CP5_MAX_STEPS;
    o[k++] = (float)w->n_cities / (float)CP5_MAX_CITIES;
    o[k++] = clampf((float)w->cities_lost * 0.2f, 0.0f, 2.0f);

    for (int n = 0; n < CP5_MAX_NATIONS; n++) {
        const Cp5Nation *na = &w->nation[n];
        o[k++] = (float)na->cities / (float)CP5_MAX_CITIES;
        o[k++] = clampf(na->money / 400.0f, 0.0f, 3.0f);
        o[k++] = na->alive ? (float)na->style / (float)CP5_APPROACH_COUNT : -1.0f;
    }
}

/* ---------------- scripted baseline ---------------- */

/* Play to the species' strength, spend on the cheapest city that will fall,
 * and tax hard only while there is nothing to lose. It is not clever; it is
 * the floor a learned policy has to clear. */
void cp5_policy_greedy(const Cp5World *w, float act[CP5_ACT_DIM])
{
    const Cp5Nation *me = &w->nation[CP5_PLAYER];
    memset(act, 0, sizeof(float) * CP5_ACT_DIM);

    int best_a = CP5_MIL;
    for (int a = 1; a < CP5_APPROACH_COUNT; a++)
        if (me->bonus[a] > me->bonus[best_a]) best_a = a;
    /* Saving for the right unit beats buying the wrong one. The first cut of
     * this fell back to the cheapest whenever the treasury dipped, which -
     * since military is the cheapest - meant a trading or devout species never
     * once used its own strength. Only fall back when there is genuinely
     * nothing else to do with the money. */
    if (me->money < UNIT_COST[best_a] && me->income < 1.0f) {
        int cheap = 0;
        for (int a = 1; a < CP5_APPROACH_COUNT; a++)
            if (UNIT_COST[a] < UNIT_COST[cheap]) cheap = a;
        if (me->money >= UNIT_COST[cheap]) best_a = cheap;
    }
    act[best_a] = 1.0f;

    int tgt = pick_target(w, CP5_PLAYER, best_a);
    for (int i = 0; i < CP5_MAX_CITIES; i++) act[CP5_APPROACH_COUNT + i] = -1.0f;
    if (tgt >= 0) act[CP5_APPROACH_COUNT + tgt] = 1.0f;

    /* Tax high while safe, back off once cities start falling. Posture never
     * drops below the 0.85 every rival holds - the baseline used to garrison
     * at 0.64 and was simply the softest target on the map. */
    act[CP5_APPROACH_COUNT + CP5_MAX_CITIES] = w->cities_lost > 0 ? -0.2f : 0.6f;
    act[CP5_APPROACH_COUNT + CP5_MAX_CITIES + 1] = w->cities_lost > 0 ? 1.0f : 0.5f;
}
