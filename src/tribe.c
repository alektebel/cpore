/* Stage 5 - TRIBE. See tribe.h for the design. */
#include "cpore/tribe.h"
#include "cpore/civ.h"   /* legacy bridge: the body arrives as 3 multipliers */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

#define GATHER_RATE  0.55f
#define UPKEEP       0.42f
#define BUILD_RATE   0.10f
/* A war is a campaign, not a thunderbolt: at full strength an even match
 * deals ~1 casualty a tick, so razing a tribe takes dozens of ticks and
 * regrouping between wars matters. Without this both sides wipe on the
 * first tick they meet. */
#define WAR_TICK     0.04f

static float dist2f(float ax, float az, float bx, float bz)
{
    float dx = ax - bx, dz = az - bz;
    return dx * dx + dz * dz;
}

static int site_ok(const Cp6World *w, int placed, float x, float z, float sep)
{
    int i;
    if (x < 140.0f || x > CP6_W - 140.0f || z < 140.0f || z > CP6_D - 140.0f) return 0;
    if (cp4_height(w->seed, x, z) > CP6_SEA - 12.0f) return 0;  /* must be dry */
    for (i = 0; i < placed; i++)
        if (dist2f(w->tribe[i].x, w->tribe[i].z, x, z) < sep * sep) return 0;
    return 1;
}

static void scatter(Cp6World *w, Cp6Tribe *t)
{
    float sep = 420.0f;
    int tries;
    for (tries = 0; tries < 4000; tries++) {
        float x = cp_rng_range(&w->rng, 140.0f, CP6_W - 140.0f);
        float z = cp_rng_range(&w->rng, 140.0f, CP6_D - 140.0f);
        if (tries > 2500) sep = 260.0f;
        if (!site_ok(w, (int)(t - w->tribe), x, z, sep)) continue;
        t->x = x;
        t->z = z;
        return;
    }
    t->x = CP6_W * 0.5f;
    t->z = CP6_D * 0.5f;
}

/* the body arrives as raid/gather/charm, through the same bridge civ uses */
static void bonuses_from_genome(const Cp4Genome *g, float *b)
{
    Cp5Legacy lg;
    cp5_legacy_from_creature(g, &lg);
    b[CP6_RAID] = lg.bonus[CP5_MIL];
    b[CP6_GATHER] = lg.bonus[CP5_ECO];
    b[CP6_CHARM] = lg.bonus[CP5_REL];
}

static float raid_power(const Cp6Tribe *t)
{
    Cp4Stats s;
    cp4_genome_stats(&t->genome, &s);
    return (s.bite + s.claw_dmg) * 0.5f + s.armor * 2.0f + t->tools * 0.15f + 2.0f;
}

static float charm_power(const Cp6Tribe *t)
{
    Cp4Stats s;
    cp4_genome_stats(&t->genome, &s);
    return s.charm * 0.8f + s.social_reach * 0.05f + 1.0f;
}

void cp6_world_reset(Cp6World *w, uint32_t seed, const Cp4Genome *founder,
                     const Cp4Genome *imports, int n_imports)
{
    int i, n_rivals, r;
    Cp4Genome g;
    if (!w) return;
    memset(w, 0, sizeof(*w));
    w->seed = seed;
    cp_rng_seed(&w->rng, seed);

    if (founder) g = *founder;
    else cp4_genome_autodesign(&g, &w->rng, CP4_GEN_BUDGET[0], CP4_STYLE_GRAZER);

    w->n_tribes = CP6_MAX_TRIBES;
    /* player */
    {
        Cp6Tribe *t = &w->tribe[CP6_PLAYER];
        t->genome = g;
        bonuses_from_genome(&g, t->bonus);
        t->members = 10.0f;
        t->stores = 30.0f;
        t->tools = 4.0f;
        t->huts = 1.0f;
        t->standing = 0.0f;
        t->alive = 1;
        scatter(w, t);
    }
    /* rivals: imported lineages first (the invasion), randoms after */
    if (n_imports < 0) n_imports = 0;
    if (n_imports > CP6_MAX_IMPORTS) n_imports = CP6_MAX_IMPORTS;
    n_rivals = CP6_MAX_TRIBES - 1;
    for (r = 0; r < n_rivals; r++) {
        Cp6Tribe *t = &w->tribe[1 + r];
        Cp4Genome rg;
        if (r < n_imports && imports) {
            rg = imports[r];
            cp4_genome_normalise(&rg, CP4_GEN_BUDGET[0]);
        } else {
            cp4_genome_random(&rg, &w->rng, CP4_GEN_BUDGET[0]);
            cp4_genome_normalise(&rg, CP4_GEN_BUDGET[0]);
        }
        t->genome = rg;
        bonuses_from_genome(&rg, t->bonus);
        t->style = (uint8_t)(r % CP6_BONUS_COUNT);
        t->members = cp_rng_range(&w->rng, 8.0f, 13.0f);
        t->stores = 25.0f;
        t->tools = 3.0f;
        t->huts = 1.0f;
        t->standing = 0.0f;
        t->alive = 1;
        scatter(w, t);
    }
    for (i = 0; i < w->n_tribes; i++) {
        if (cp4_height(w->seed, w->tribe[i].x, w->tribe[i].z) > CP6_SEA - 12.0f) {
            w->tribe[i].x = CP6_W * 0.5f;
            w->tribe[i].z = CP6_D * 0.5f;
        }
    }
    w->status = CP6_RUN;
}

/* one tribe's economy tick; shares are normalised gather/build/raise */
static void economy(Cp6World *w, Cp6Tribe *t, float gS, float bS, float rS)
{
    float fert = 0.6f;
    float tools_mult, gather, eat, cap;
    int biome = cp4_biome(w->seed, t->x, t->z);
    if (biome >= 0 && biome < CP4_BIOME_COUNT) fert = cp4_fertility(biome);
    tools_mult = 1.0f + (t->tools > 40.0f ? 40.0f : t->tools) / 20.0f;
    gather = t->members * gS * GATHER_RATE * tools_mult * fert * t->bonus[CP6_GATHER];
    eat = t->members * UPKEEP;
    t->stores += gather - eat;
    if (t->stores < 0.0f) {          /* hunger eats the tribe, not the larder */
        t->members += t->stores * 0.15f;
        t->stores = 0.0f;
    }
    t->tools += t->members * bS * BUILD_RATE;
    t->tools *= 0.9995f;
    cap = 8.0f + t->huts * 6.0f;
    if (rS > 0.0f && t->stores > 20.0f && t->members < cap)
        t->members += rS * (t->stores - 20.0f) * 0.002f * (cap - t->members) / cap * 10.0f;
    if (t == &w->tribe[CP6_PLAYER]) w->gathered += gather;
}

static float score_of(const Cp6World *w)
{
    float s = 0.0f;
    int i;
    const Cp6Tribe *p = &w->tribe[CP6_PLAYER];
    s += (float)w->allied * 3.0f + (float)w->razed * 3.0f;
    s += p->members * 0.02f + p->stores * 0.005f + p->tools * 0.01f;
    for (i = 1; i < w->n_tribes; i++)
        if (w->tribe[i].alive && w->tribe[i].standing > 0.0f)
            s += w->tribe[i].standing * 0.5f;
    return s;
}

static void check_over(Cp6World *w)
{
    int i, done = 1;
    Cp6Tribe *p = &w->tribe[CP6_PLAYER];
    if (p->members < 0.5f) {
        p->members = 0.0f;
        p->alive = 0;
        w->status = CP6_LOST;
        return;
    }
    for (i = 1; i < w->n_tribes; i++) {
        const Cp6Tribe *t = &w->tribe[i];
        if (t->alive && !t->allied) { done = 0; break; }
    }
    if (done) w->status = CP6_WON;
}

void cp6_world_step(Cp6World *w, const float act[CP6_ACT_DIM])
{
    int i;
    float gS, bS, rS, sum;
    Cp6Tribe *p;
    if (!w || w->status != CP6_RUN) return;

    gS = act[0] > 0.0f ? act[0] : 0.0f;
    bS = act[1] > 0.0f ? act[1] : 0.0f;
    rS = act[2] > 0.0f ? act[2] : 0.0f;
    sum = gS + bS + rS;
    if (sum < 1e-6f) { gS = 0.6f; bS = 0.25f; rS = 0.15f; sum = 1.0f; }
    gS /= sum; bS /= sum; rS /= sum;

    p = &w->tribe[CP6_PLAYER];
    economy(w, p, gS, bS, rS);

    /* huts: stored food becomes roofed capacity */
    if (act[CP6_ACT_DIM - 1] > 0.5f && p->stores > 15.0f && (w->step % 40) == 0) {
        p->stores -= 15.0f;
        p->huts += 1.0f;
    }

    /* player stance per rival: +befriend, -raid.
     * A war party cannot be in five places: raiders split across the fronts
     * the player opened, so a single-front war hits at full strength and a
     * five-front war is five skirmishes. */
    float warfronts = 0.0f;
    for (i = 1; i < w->n_tribes; i++) {
        float st;
        if (!w->tribe[i].alive || w->tribe[i].allied) continue;
        st = clampf(act[3 + (i - 1)], -1.0f, 1.0f);
        if (st < -0.05f) warfronts -= st;
    }
    if (warfronts < 1.0f) warfronts = 1.0f;
    for (i = 1; i < w->n_tribes; i++) {
        Cp6Tribe *t = &w->tribe[i];
        float st;
        if (!t->alive || t->allied) continue;
        st = clampf(act[3 + (i - 1)], -1.0f, 1.0f);
        if (st > 0.05f) {
            float gift = st * (p->stores * 0.02f > 2.0f ? 2.0f : p->stores * 0.02f);
            if (gift > 0.0f && p->stores >= gift) {
                p->stores -= gift;
                t->stores += gift;
                t->standing += gift * 0.02f * charm_power(p) * p->bonus[CP6_CHARM];
                w->gifts++;
                if (t->standing >= 1.0f) {
                    t->standing = 1.0f;
                    t->allied = 1;
                    w->allied++;
                }
            }
        } else if (st < -0.05f) {
            float raiders = -st * p->members * 0.5f / warfronts;
            if (raiders > 0.5f) {
                float atk = raiders * raid_power(p) * p->bonus[CP6_RAID];
                float def = t->members * (1.0f + t->tools * 0.02f);
                float dmg = atk / (def + 1.0f) * WAR_TICK;
                float loot;
                float cost = dmg * 0.35f * (t->members / (p->members + 1.0f));
                t->members -= dmg;
                p->members -= cost;
                w->lost_members += cost;
                loot = t->stores * 0.3f;
                if (loot > raiders * 0.8f) loot = raiders * 0.8f;
                t->stores -= loot;
                p->stores += loot;
                w->loot += (int32_t)loot;
                t->standing = -1.0f;
                w->raids++;
                if (t->members < 0.5f) {
                    t->members = 0.0f;
                    t->alive = 0;
                    w->razed++;
                    /* captives join: conquest grows the tribe, which is what
                     * lets a raider afford the next war */
                    p->members += 2.5f;
                }
            }
        }
    }

    /* rivals run themselves: style-shaped economy, opportunistic AI */
    for (i = 1; i < w->n_tribes; i++) {
        Cp6Tribe *t = &w->tribe[i];
        float rg = 0.55f, rb = 0.25f, rr = 0.20f;
        if (!t->alive || t->allied) continue;
        if (t->style == CP6_RAID) { rg = 0.45f; rb = 0.35f; rr = 0.20f; }
        else if (t->style == CP6_CHARM) { rg = 0.60f; rb = 0.15f; rr = 0.25f; }
        economy(w, t, rg, rb, rr);
        if (t->members < 0.5f) {
            t->members = 0.0f;
            t->alive = 0;
            if (!t->allied) w->razed++;   /* starved on its own: still one less rival */
            continue;
        }
        t->standing *= 0.9995f;           /* moods fade, as in stage 3 */
        /* a stronger hostile neighbour raids the player: one skirmish per
         * season, staggered per tribe so five enemies cannot all answer the
         * same insult on the same tick */
        if (t->standing < -0.3f && t->members > p->members * 1.15f &&
            ((w->step + i * 17) % 120) == 0) {
            float atk = t->members * 0.4f * raid_power(t) * t->bonus[CP6_RAID];
            float def = p->members * (1.0f + p->tools * 0.02f);
            float dmg = atk / (def + 1.0f) * WAR_TICK * 6.0f;
            float loot = p->stores * 0.2f;
            p->members -= dmg;
            w->lost_members += dmg;
            p->stores -= loot;
            t->stores += loot;
            if (p->members < 0.5f) { p->members = 0.0f; p->alive = 0; }
        }
        /* a courted neighbour drifts toward alliance on its own */
        if (t->standing > 0.55f && p->stores > 10.0f && (w->step % 90) == 0) {
            t->standing += 0.05f;
            if (t->standing >= 1.0f) {
                t->standing = 1.0f;
                t->allied = 1;
                w->allied++;
            }
        }
    }

    w->step++;
    {
        float s = score_of(w);
        w->reward = s - w->score;
        w->score = s;
    }
    check_over(w);
    if (w->status == CP6_RUN && w->step >= CP6_MAX_STEPS) w->status = CP6_TIMEOUT;
}

void cp6_world_observe(const Cp6World *w, float *o)
{
    int i;
    const Cp6Tribe *p;
    if (!w || !o) return;
    p = &w->tribe[CP6_PLAYER];
    o[0] = p->members / 30.0f;
    o[1] = p->stores / 120.0f;
    o[2] = p->tools / 30.0f;
    o[3] = p->huts / 6.0f;
    o[4] = p->bonus[CP6_RAID] / 1.6f;
    o[5] = p->bonus[CP6_GATHER] / 1.6f;
    o[6] = p->bonus[CP6_CHARM] / 1.6f;
    o[7] = (float)w->step / (float)CP6_MAX_STEPS;
    for (i = 1; i < w->n_tribes; i++) {
        const Cp6Tribe *t = &w->tribe[i];
        float *q = o + 8 + (i - 1) * CP6_OBS_TRIBE;
        q[0] = t->alive ? 1.0f : 0.0f;
        q[1] = t->allied ? 1.0f : 0.0f;
        q[2] = clampf(t->standing, -1.0f, 1.0f);
        q[3] = clampf(t->members / 30.0f, 0.0f, 1.5f);
        q[4] = clampf((t->x - p->x) / CP6_W, -1.0f, 1.0f);
        q[5] = clampf((t->z - p->z) / CP6_D, -1.0f, 1.0f);
    }
    o[8 + (CP6_MAX_TRIBES - 1) * CP6_OBS_TRIBE + 0] = (float)w->allied / 5.0f;
    o[8 + (CP6_MAX_TRIBES - 1) * CP6_OBS_TRIBE + 1] = (float)w->razed / 5.0f;
    o[8 + (CP6_MAX_TRIBES - 1) * CP6_OBS_TRIBE + 2] = clampf(w->score / 20.0f, -1.0f, 2.0f);
}

void cp6_policy_greedy(const Cp6World *w, float act[CP6_ACT_DIM])
{
    int i, weak = -1;
    float weakm = 1e18f;
    const Cp6Tribe *p;
    if (!w || !act) return;
    for (i = 0; i < CP6_ACT_DIM; i++) act[i] = 0.0f;
    p = &w->tribe[CP6_PLAYER];
    act[0] = 0.6f; act[1] = 0.25f; act[2] = 0.15f;
    for (i = 1; i < w->n_tribes; i++) {
        const Cp6Tribe *t = &w->tribe[i];
        if (!t->alive || t->allied) continue;
        if (t->members < weakm) { weakm = t->members; weak = i; }
    }
    for (i = 1; i < w->n_tribes; i++) {
        const Cp6Tribe *t = &w->tribe[i];
        if (!t->alive || t->allied) continue;
        /* raid when the body favours it and the numbers do; else befriend */
        if (p->bonus[CP6_RAID] > p->bonus[CP6_CHARM] &&
            p->members > t->members * 1.1f)
            act[3 + (i - 1)] = -0.8f;
        else
            act[3 + (i - 1)] = (i == weak) ? 0.7f : 0.2f;
    }
    act[CP6_ACT_DIM - 1] = (p->stores > 40.0f) ? 0.6f : 0.0f;
}

/* ---------------- env ABI ---------------- */

struct Cp6Env { Cp6World w; };

Cp6Env *cp6_env_create(uint32_t seed)
{
    Cp6Env *e = (Cp6Env *)calloc(1, sizeof(Cp6Env));
    if (!e) return NULL;
    cp6_world_reset(&e->w, seed, NULL, NULL, 0);
    return e;
}

void cp6_env_free(Cp6Env *e) { free(e); }

void cp6_env_reset(Cp6Env *e, uint32_t seed, const int32_t *parts, float *obs)
{
    Cp4Genome g;
    if (!e) return;
    if (parts) {
        int i;
        cp4_genome_clear(&g);
        for (i = 0; i < CP4_MAX_PARTS; i++) {
            const int32_t *q = parts + i * 8;
            int t = q[0];
            if (t < 0) t = 0;
            if (t >= CP4_PART_COUNT) t = CP4_PART_COUNT - 1;
            g.part[i].type   = (uint8_t)t;
            g.part[i].seg    = (uint8_t)(q[1] < 0 ? 0 : (q[1] >= CP4_MAX_SEG ? CP4_MAX_SEG - 1 : q[1]));
            g.part[i].yaw    = (uint8_t)(q[2] & 0xFF);
            g.part[i].pitch  = (int8_t)(q[3] < -64 ? -64 : (q[3] > 63 ? 63 : q[3]));
            g.part[i].scale  = (uint8_t)(q[4] < 0 ? 0 : (q[4] > 255 ? 255 : q[4]));
            g.part[i].mirror = (uint8_t)(q[5] ? 1 : 0);
            g.part[i].len    = (uint8_t)(q[6] <= 0 ? 128 : (q[6] > 255 ? 255 : q[6]));
            g.part[i].bend   = (int8_t)(q[7] < -127 ? -127 : (q[7] > 127 ? 127 : q[7]));
        }
        {
            int ns = parts[CP4_MAX_PARTS * 8];
            int gi = parts[CP4_MAX_PARTS * 8 + 1];
            g.nseg  = (uint8_t)(ns < 2 ? 2 : (ns > CP4_MAX_SEG ? CP4_MAX_SEG : ns));
            g.girth = (uint8_t)(gi < 0 ? 0 : (gi > 255 ? 255 : gi));
        }
        cp4_genome_normalise(&g, CP4_GEN_BUDGET[0]);
    } else {
        cp4_genome_starter(&g);
    }
    cp6_world_reset(&e->w, seed, &g, NULL, 0);
    if (obs) cp6_world_observe(&e->w, obs);
}

void cp6_env_step(Cp6Env *e, const float *act, float *obs,
                  float *reward, int32_t *terminated, int32_t *truncated)
{
    if (!e) return;
    cp6_world_step(&e->w, act);
    if (obs) cp6_world_observe(&e->w, obs);
    if (reward) *reward = e->w.reward;
    if (terminated) *terminated = (e->w.status == CP6_LOST || e->w.status == CP6_WON);
    if (truncated) *truncated = (e->w.status == CP6_TIMEOUT);
}

int32_t cp6_env_obs_dim(void) { return CP6_OBS_DIM; }
int32_t cp6_env_act_dim(void) { return CP6_ACT_DIM; }
size_t cp6_env_state_size(void) { return sizeof(Cp6World); }
void cp6_env_save(const Cp6Env *e, void *dst)
{
    if (e && dst) memcpy(dst, &e->w, sizeof(Cp6World));
}
void cp6_env_load(Cp6Env *e, const void *src)
{
    if (e && src) memcpy(&e->w, src, sizeof(Cp6World));
}
const Cp6World *cp6_env_world(const Cp6Env *e) { return e ? &e->w : NULL; }

void cp6_env_census(const Cp6Env *e, int32_t *counts, float *vals)
{
    const Cp6World *w;
    float ss = 0.0f;
    int i;
    if (!e) return;
    w = &e->w;
    for (i = 1; i < w->n_tribes; i++)
        if (w->tribe[i].alive) ss += w->tribe[i].standing;
    if (counts) {
        counts[0] = (int32_t)w->tribe[CP6_PLAYER].members;
        counts[1] = w->allied;
        counts[2] = w->razed;
        counts[3] = w->gifts;
        counts[4] = w->raids;
        counts[5] = w->loot;
    }
    if (vals) {
        vals[0] = w->tribe[CP6_PLAYER].stores;
        vals[1] = w->gathered;
        vals[2] = ss;
        vals[3] = w->score;
    }
}

/* ---------------- top-down map render ---------------- */

void cp6_render(const Cp6World *w, uint8_t *rgba, int width, int height)
{
    int x, y, i;
    if (!w || !rgba || width < 8 || height < 8) return;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            float wx = (float)x / (float)width * CP6_W;
            float wz = (float)y / (float)height * CP6_D;
            float h = cp4_height(w->seed, wx, wz);
            float r, g, b;
            uint8_t *p = rgba + ((size_t)y * width + x) * 4;
            if (h > CP6_SEA - 12.0f) { r = 16; g = 38; b = 74; }        /* water */
            else {
                float t = clampf((CP6_SEA - 12.0f - h) / 160.0f, 0.0f, 1.0f);
                r = 34 + t * 90; g = 72 + t * 60; b = 30 + t * 30;     /* lowland..highland */
            }
            p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; p[3] = 255;
        }
    }
    for (i = 0; i < w->n_tribes; i++) {
        const Cp6Tribe *t = &w->tribe[i];
        int px = (int)(t->x / CP6_W * width);
        int py = (int)(t->z / CP6_D * height);
        float r, g, b;
        if (!t->alive) continue;
        if (i == CP6_PLAYER) { r = 255; g = 255; b = 255; }
        else if (t->allied) { r = 90; g = 255; b = 120; }
        else if (t->standing < -0.3f) { r = 255; g = 80; b = 80; }
        else { r = 255; g = 210; b = 120; }
        cp_px_rect(rgba, width, height, px - 3, py - 3, 7, 7, r, g, b, 255.0f);
        cp_px_rect(rgba, width, height, px - 1, py - 1, 3, 3, 0.0f, 0.0f, 0.0f, 255.0f);
    }
    {
        char buf[128];
        const Cp6Tribe *p = &w->tribe[CP6_PLAYER];
        snprintf(buf, sizeof(buf), "members %.0f stores %.0f tools %.0f allied %d razed %d step %d",
                 (double)p->members, (double)p->stores, (double)p->tools,
                 w->allied, w->razed, w->step);
        cp_px_text(rgba, width, height, 6, 6, 1, buf, 255.0f, 255.0f, 255.0f, 255.0f);
    }
}

void cp6_render_styled(const Cp6World *w, uint8_t *rgba, int width, int height,
                       int style)
{
    (void)style;   /* v1: one honest map tier; palette tier follows the civ path */
    cp6_render(w, rgba, width, height);
}

int32_t cp6_env_status(const Cp6Env *e) { return e ? e->w.status : CP6_RUN; }
