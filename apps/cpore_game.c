/* cpore_game - the native playable game. No browser, no WASM.
 *
 *   ./build/cpore_game [--stage land] [--seed 7] [--genome CP4-...] [--scale 3]
 *
 * One process, six stages: 1-6 switch cell/aqua/land/tribe/civ/space live,
 * and the arc chains - the tribe founds from your live land genome, civ
 * inherits its legacy, space inherits the nation's doctrine, so playing
 * upward is the campaign. Zero design head during play, so the scripted
 * designer rebuilds you at generation boundaries exactly as it does for an
 * RL policy; N opens the editor-lite (rebuild toward an archetype at the
 * current budget), C prints the share code of the body you are driving, F
 * takes a full-quality still with the screenshot renderer.
 *
 * Rendering: cell/aqua use their real CPU renderers (fast tiers); land,
 * tribe and civ play on a GPU-presented top-down map computed at half res
 * per frame (the ray-marched first-person view stays the photo tier - F).
 * Present goes through glview (X11+GLX, fixed-function, no extra deps).
 *
 * Controls (land): WASD/arrows move, R/F rise/sink (fly/swim), SPACE bite,
 *   E sing, Q dig, B nest, N redesign, G autopilot.
 * Cell: WASD steer, SPACE burst, E zap.  Aqua: A/D yaw, W thrust, R/F pitch,
 *   SPACE bite.  Tribe: LEFT/RIGHT pick rival, UP/DOWN stance, G/Y/U
 *   workforce presets, N build hut.  Civ: Z/X/C doctrine, arrows+ENTER focus.
 * Space: WASD thrust, SPACE trade, E colonise, Q attack, R resupply,
 *   T cycle upgrade track.
 * Global: 1-6 stage, R restart, F photo, C share code, ESC quit.
 */
#include "cpore/cpore.h"
#include "cpore/aqua.h"
#include "cpore/land.h"
#include "cpore/civ.h"
#include "cpore/tribe.h"
#include "cpore/space.h"
#include "cpore/codec.h"
#include "cpore/codex.h"
#include "cpore/glview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define FW 320
#define FH 180
#define MW 160
#define MH 90

#define K_LEFT 0x51
#define K_UP 0x52
#define K_RIGHT 0x53
#define K_DOWN 0x54
#define K_ESC 0x1B
#define K_RET 0x0D

typedef struct {
    CpWorld w;
    Cp3World a;
    Cp4World l;
    Cp6World t;
    Cp5World c;
    Cp7World s;
    int stage;               /* 0 cell 1 aqua 2 land 3 tribe 4 civ 5 space */
    uint32_t seed;
    int auto_pilot;
    int style_sel;           /* editor-lite archetype cursor */
    int cursor;              /* tribe/civ rival-city cursor */
    float trib_stance[CP6_MAX_TRIBES];
    float trib_work[3];
    int trib_hut;
    int civ_focus;
    int civ_doctrine;
    int space_dial;          /* ship upgrade track cursor */
    CpdxCodex codex;
    char banner[128];
    int banner_t;
} Game;

static inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? v : v); }

static void px(uint8_t *fb, int x, int y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || y < 0 || x >= FW || y >= FH) return;
    {
        uint8_t *p = fb + ((size_t)y * FW + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
    }
}

/* half-res terrain layer, upscaled 2x into fb */
static void map_terrain(Game *g, uint8_t *fb, float camx, float camz,
                        float span_x, float span_z, float dim)
{
    int mx, my;
    for (my = 0; my < MH; my++) {
        for (mx = 0; mx < MW; mx++) {
            float wx = camx + ((float)mx / MW - 0.5f) * span_x;
            float wz = camz + ((float)my / MH - 0.5f) * span_z;
            float wl, h = cp4_height_water(g->seed, wx, wz, &wl);
            float r, gg, b;
            if (h > CP4_SEA - 12.0f) { r = 14; gg = 34; b = 66; }
            else {
                float t = clampf((CP4_SEA - 12.0f - h) / 170.0f, 0.0f, 1.0f);
                r = 30 + t * 120; gg = 66 + t * 70; b = 26 + t * 60;
                if (h < CP4_SEA - 150.0f) { r = 225; gg = 232; b = 240; }
            }
            r *= dim; gg *= dim; b *= dim;
            {
                int x, y;
                for (y = 0; y < 2; y++)
                    for (x = 0; x < 2; x++)
                        px(fb, mx * 2 + x, my * 2 + y,
                           (uint8_t)r, (uint8_t)gg, (uint8_t)b);
            }
        }
    }
}

static void map_dot(Game *g, uint8_t *fb, float camx, float camz,
                    float span_x, float span_z, float wx, float wz,
                    int sz, uint8_t r, uint8_t gg, uint8_t b)
{
    int cx = (int)(((wx - camx) / span_x + 0.5f) * FW);
    int cy = (int)(((wz - camz) / span_z + 0.5f) * FH);
    int x, y;
    (void)g;
    for (y = -sz; y <= sz; y++)
        for (x = -sz; x <= sz; x++)
            px(fb, cx + x, cy + y, r, gg, b);
}

static void draw_land(Game *g, uint8_t *fb)
{
    Cp4World *w = &g->l;
    float camx = w->player.p.x, camz = w->player.p.z;
    float dim = 0.35f + 0.65f * cp4_daylight(w->step);
    int i;
    map_terrain(g, fb, camx, camz, 640.0f, 360.0f, dim);
    for (i = 0; i < w->n_flora; i++) {
        Cp4Flora *f = &w->flora[i];
        if (f->type == CP4_FLORA_NONE) continue;
        if (f->type == CP4_FLORA_BUSH) map_dot(g, fb, camx, camz, 640, 360, f->p.x, f->p.z, 0, 60, 200, 80);
        else if (f->type == CP4_FLORA_KELP) map_dot(g, fb, camx, camz, 640, 360, f->p.x, f->p.z, 0, 40, 180, 160);
        else if (f->type == CP4_FLORA_TUBER) map_dot(g, fb, camx, camz, 640, 360, f->p.x, f->p.z, 0, 170, 130, 70);
        else map_dot(g, fb, camx, camz, 640, 360, f->p.x, f->p.z, 0, 200, 60, 60);
    }
    for (i = 0; i < CP4_MAX_BEASTS; i++) {
        Cp4Beast *b = &w->beast[i];
        uint32_t hh;
        if (!b->alive) continue;
        map_dot(g, fb, camx, camz, 640, 360, b->p.x, b->p.z, 0, 240, 240, 240);
        /* codex: first sighting of a lineage is an event */
        hh = cpdx_hash_bytes((const uint8_t *)&b->g, (uint32_t)sizeof(b->g));
        if (hh) {
            int is_new = cpdx_note(&g->codex, hh, cp4_biome(g->seed, b->p.x, b->p.z),
                                   b->medium, 2, b->nest == CP4_OWN_NEST ? 2 : 1,
                                   w->step, b->p.x, b->p.z);
            if (is_new == 1 && g->codex.n <= CPDX_MAX) {
                snprintf(g->banner, sizeof(g->banner), "FOUND: %.23s",
                         g->codex.entry[g->codex.n - 1].name);
                g->banner_t = 180;
            }
        }
    }
    for (i = 0; i < CP4_MAX_NESTS; i++) {
        if (!w->nest[i].alive) continue;
        map_dot(g, fb, camx, camz, 640, 360, w->nest[i].p.x, w->nest[i].p.z, 1, 250, 220, 90);
    }
    if (w->home.alive)
        map_dot(g, fb, camx, camz, 640, 360, w->home.p.x, w->home.p.z, 1, 90, 220, 255);
    /* player + facing */
    map_dot(g, fb, camx, camz, 640, 360, w->player.p.x, w->player.p.z, 1, 255, 255, 255);
    {
        float fx = cosf(w->player.yaw), fz = sinf(w->player.yaw);
        map_dot(g, fb, camx, camz, 640, 360,
                w->player.p.x + fx * 14.0f, w->player.p.z + fz * 14.0f, 0, 255, 80, 80);
    }
}

static void draw_tribe(Game *g, uint8_t *fb)
{
    Cp6World *w = &g->t;
    Cp6Tribe *p = &w->tribe[CP6_PLAYER];
    int i;
    map_terrain(g, fb, p->x, p->z, 900.0f, 500.0f, 1.0f);
    for (i = 0; i < w->n_tribes; i++) {
        Cp6Tribe *t = &w->tribe[i];
        uint8_t r = 250, gg = 220, b = 90;
        if (!t->alive) continue;
        if (i == CP6_PLAYER) { r = 255; gg = 255; b = 255; }
        else if (t->allied) { r = 90; gg = 255; b = 120; }
        else if (t->standing < -0.3f) { r = 255; gg = 80; b = 80; }
        map_dot(g, fb, p->x, p->z, 900, 500, t->x, t->z, 2, r, gg, b);
        if (i - 1 == g->cursor && i > 0)
            map_dot(g, fb, p->x, p->z, 900, 500, t->x, t->z, 3, 255, 255, 255);
    }
}

static void draw_civ(Game *g, uint8_t *fb)
{
    Cp5World *w = &g->c;
    float camx = CP5_W * 0.5f, camz = CP5_D * 0.5f;
    int i;
    static const uint8_t OC[4][3] = { {255,255,255}, {255,90,90}, {90,255,120}, {120,160,255} };
    map_terrain(g, fb, camx, camz, CP5_W, CP5_D * 0.56f, 1.0f);
    for (i = 0; i < w->n_cities; i++) {
        Cp5City *c = &w->city[i];
        const uint8_t *cc;
        if (!c->alive) continue;
        cc = (c->owner >= 0 && c->owner < 4) ? OC[c->owner] : OC[3];
        map_dot(g, fb, camx, camz, CP5_W, CP5_D * 0.56f, c->p.x, c->p.z, 2, cc[0], cc[1], cc[2]);
        if (i == g->civ_focus)
            map_dot(g, fb, camx, camz, CP5_W, CP5_D * 0.56f, c->p.x, c->p.z, 3, 255, 255, 0);
    }
    for (i = 0; i < CP5_MAX_UNITS; i++) {
        Cp5Unit *u = &w->unit[i];
        const uint8_t *cc;
        if (!u->alive) continue;
        cc = (u->owner >= 0 && u->owner < 4) ? OC[u->owner] : OC[3];
        map_dot(g, fb, camx, camz, CP5_W, CP5_D * 0.56f, u->p.x, u->p.z, 0, cc[0], cc[1], cc[2]);
    }
}

static void draw_space(Game *g, uint8_t *fb)
{
    cp7_render(&g->s, fb, FW, FH);
}

static void hud(Game *g, uint8_t *fb, double fps)
{
    char buf[160];
    const char *stg[6] = { "CELL", "AQUA", "CREATURE", "TRIBE", "CIV", "SPACE" };
    buf[0] = '\0';
    if (g->stage == 0) {
        static const char *st[] = { "ALIVE", "DIED", "EVOLVED", "TIMEOUT" };
        snprintf(buf, sizeof(buf), "%s fps %.0f gen %d dna %.0f/%.0f hp %.0f %s %s", stg[g->stage],
                 fps, g->w.generation + 1, (double)g->w.dna, (double)CP_DNA_GOAL,
                 (double)g->w.player.hp, st[g->w.status], g->auto_pilot ? "AUTO" : "");
    } else if (g->stage == 1) {
        snprintf(buf, sizeof(buf), "%s fps %.0f gen %d bio %.0f/%.0f hp %.0f %s", stg[g->stage],
                 fps, g->a.generation + 1, (double)g->a.biomass, (double)CP3_BIOMASS_GOAL,
                 (double)g->a.player.hp, g->auto_pilot ? "AUTO" : "");
    } else if (g->stage == 2) {
        snprintf(buf, sizeof(buf), "%s fps %.0f gen %d dna %.0f/%.0f hp %.0f codex %d %s", stg[g->stage],
                 fps, g->l.generation + 1, (double)g->l.dna, (double)CP4_DNA_GOAL,
                 (double)g->l.player.hp, g->codex.n, g->auto_pilot ? "AUTO" : "");
    } else if (g->stage == 3) {
        Cp6Tribe *p = &g->t.tribe[CP6_PLAYER];
        snprintf(buf, sizeof(buf), "%s fps %.0f members %.0f stores %.0f allied %d razed %d %s",
                 stg[g->stage], fps, (double)p->members, (double)p->stores,
                 g->t.allied, g->t.razed, g->auto_pilot ? "AUTO" : "");
    } else if (g->stage == 4) {
        snprintf(buf, sizeof(buf), "%s fps %.0f cities %d money %.0f %s", stg[g->stage],
                 fps, g->c.nation[CP5_PLAYER].cities,
                 (double)g->c.nation[CP5_PLAYER].money, g->auto_pilot ? "AUTO" : "");
    } else {
        static const char *DN[4] = { "engine", "cargo", "weapons", "hull" };
        snprintf(buf, sizeof(buf), "%s fps %.0f stars %d money %.0f fuel %.0f hull %.0f dial %s %s",
                 stg[g->stage], fps, g->s.empire[CP7_PLAYER].stars,
                 (double)g->s.empire[CP7_PLAYER].money,
                 (double)g->s.ship.fuel, (double)g->s.ship.hull,
                 DN[g->space_dial], g->auto_pilot ? "AUTO" : "");
    }
    cp_px_text(fb, FW, FH, 4, FH - 12, 1, buf, 1.0f, 1.0f, 1.0f, 1.0f);
    if (g->banner_t > 0) {
        cp_px_text(fb, FW, FH, 40, 20, 1, g->banner, 1.0f, 0.9f, 0.3f, 1.0f);
        g->banner_t--;
    }
}

static void reset_stage(Game *g, int stage)
{
    g->stage = stage;
    if (stage == 0) cp_world_reset(&g->w, g->seed, NULL);
    else if (stage == 1) cp3_world_reset(&g->a, g->seed, NULL);
    else if (stage == 2) { cp4_world_reset(&g->l, g->seed, NULL); cpdx_reset(&g->codex); }
    /* tribe founds from the live creature genome; civ inherits its legacy:
     * playing upward IS the campaign. */
    else if (stage == 3) cp6_world_reset(&g->t, g->seed, &g->l.player.g, NULL, 0);
    else if (stage == 4) {
        float leg[3];
        Cp5Legacy lg;
        cp5_legacy_from_world(&g->l, leg);
        for (int a = 0; a < 3; a++) lg.bonus[a] = leg[a];
        cp5_world_reset(&g->c, g->seed, &lg);
    }
    /* space inherits the nation's own doctrine multipliers: the empire you
     * grew in civ is the power you reach the stars with */
    else if (stage == 5) {
        float leg[CP7_BONUS_COUNT];
        Cp7Legacy lg;
        cp7_legacy_from_civ(&g->c, leg);
        for (int b = 0; b < CP7_BONUS_COUNT; b++) lg.bonus[b] = leg[b];
        cp7_world_reset(&g->s, g->seed, &lg);
    }
}

static void share_code(Game *g)
{
    if (g->stage == 0) {
        char s[CP_CODEC_CELL_STR + 1];
        if (cp_codec_cell(&g->w.genome, s, sizeof(s))) printf("share: %s\n", s);
    } else if (g->stage == 1) {
        char s[CP_CODEC_AQUA_STR + 1];
        if (cp_codec_aqua(&g->a.player.g, s, sizeof(s))) printf("share: %s\n", s);
    } else {
        char s[CP_CODEC_LAND_STR + 1];
        if (cp_codec_land(&g->l.player.g, s, sizeof(s))) printf("share: %s\n", s);
    }
    fflush(stdout);
}

static void photo(Game *g)
{
    char path[64];
    printf("photo: exposing... (full-quality render, may take a few seconds)\n");
    fflush(stdout);
    if (g->stage == 0) {
        uint8_t *hi = malloc(1280 * 720 * 4);
        snprintf(path, sizeof(path), "game_cell_%u.png", g->seed);
        cp_render_styled(&g->w, hi, 1280, 720, CP_VIS_ABYSS);
        cp_png_write(path, hi, 1280, 720);
        free(hi);
    } else if (g->stage == 1) {
        uint8_t *hi = malloc(1280 * 720 * 4);
        snprintf(path, sizeof(path), "game_aqua_%u.png", g->seed);
        cp3_render_styled(&g->a, hi, 1280, 720, CP_VIS_ABYSS);
        cp_png_write(path, hi, 1280, 720);
        free(hi);
    } else if (g->stage == 2) {
        uint8_t *hi = malloc(640 * 360 * 4);
        snprintf(path, sizeof(path), "game_land_%u.png", g->seed);
        cp4_render_styled(&g->l, hi, 640, 360, CP_VIS_TERRA);
        cp_png_write(path, hi, 640, 360);
        free(hi);
    } else if (g->stage == 3) {
        snprintf(path, sizeof(path), "game_tribe_%u.png", g->seed);
        { uint8_t *hi = malloc(640 * 360 * 4);
          cp6_render(&g->t, hi, 640, 360); cp_png_write(path, hi, 640, 360); free(hi); }
    } else if (g->stage == 4) {
        uint8_t *hi = malloc(640 * 360 * 4);
        snprintf(path, sizeof(path), "game_civ_%u.png", g->seed);
        cp5_render_styled(&g->c, hi, 640, 360, CP_VIS_ABYSS);
        cp_png_write(path, hi, 640, 360);
        free(hi);
    } else {
        uint8_t *hi = malloc(1280 * 720 * 4);
        snprintf(path, sizeof(path), "game_space_%u.png", g->seed);
        cp7_render(&g->s, hi, 1280, 720);
        cp_png_write(path, hi, 1280, 720);
        free(hi);
    }
    snprintf(g->banner, sizeof(g->banner), "saved %s", path);
    g->banner_t = 180;
    printf("photo: %s\n", path);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    Game g;
    GlvWindow *win;
    uint8_t *fb;
    int *down, *pressed;
    uint32_t seed = 7;
    int stage = 2, scale = 3, frame = 0;
    const char *genome_arg = NULL;
    double acc = 0.0, last = 0.0, now;
    struct timespec ts;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--stage") && i + 1 < argc) {
            const char *s = argv[++i];
            stage = (!strcmp(s, "cell")) ? 0 : (!strcmp(s, "aqua")) ? 1 : (!strcmp(s, "tribe")) ? 3
                    : (!strcmp(s, "civ")) ? 4 : (!strcmp(s, "space")) ? 5 : 2;
        } else if (!strcmp(argv[i], "--genome") && i + 1 < argc) genome_arg = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
        else { printf("usage: cpore_game [--stage cell|aqua|land|tribe|civ|space] [--seed N] [--genome CODE] [--scale K]\n"); return 1; }
    }

    memset(&g, 0, sizeof(g));
    g.seed = seed;
    g.trib_work[0] = 0.6f; g.trib_work[1] = 0.25f; g.trib_work[2] = 0.15f;
    cp_world_reset(&g.w, seed, NULL);
    cp3_world_reset(&g.a, seed, NULL);
    cp4_world_reset(&g.l, seed, NULL);
    cpdx_reset(&g.codex);
    cp6_world_reset(&g.t, seed, &g.l.player.g, NULL, 0);
    {
        float leg[3];
        Cp5Legacy lg;
        cp5_legacy_from_world(&g.l, leg);
        for (int a = 0; a < 3; a++) lg.bonus[a] = leg[a];
        cp5_world_reset(&g.c, seed, &lg);
    }
    {
        Cp7Legacy lg;
        cp7_legacy_from_civ(&g.c, lg.bonus);
        cp7_world_reset(&g.s, seed, &lg);
    }
    /* a pasted share code applies to the opening stage's body */
    if (genome_arg) {
        if (!strncmp(genome_arg, "CP1-", 4)) {
            CpGenome cg; if (!cp_decode_cell(genome_arg, &cg)) cp_world_apply_genome(&g.w, &cg);
        } else if (!strncmp(genome_arg, "CP3-", 4)) {
            Cp3Genome cg; if (!cp_decode_aqua(genome_arg, &cg)) cp3_world_apply_genome(&g.a, &cg);
        } else if (!strncmp(genome_arg, "CP4-", 4)) {
            Cp4Genome cg; if (!cp_decode_land(genome_arg, &cg)) cp4_world_apply_genome(&g.l, &cg);
        } else printf("game: ignoring unrecognised --genome (want CP1-/CP3-/CP4-...)\n");
        g.stage = (!strncmp(genome_arg, "CP1-", 4)) ? 0 : (!strncmp(genome_arg, "CP3-", 4)) ? 1 : 2;
    } else g.stage = stage;

    win = glv_open("cpore - native game", FW, FH, scale);
    if (!win) return 1;
    fb = malloc((size_t)FW * FH * 4);
    down = calloc(256, sizeof(int));
    pressed = calloc(256, sizeof(int));
    if (!fb || !down || !pressed) { printf("oom\n"); return 1; }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    last = ts.tv_sec + ts.tv_nsec * 1e-9;
    printf("cpore native game: 1-6 stage, N redesign, C share code, F photo, G autopilot, ESC quit\n");

    while (1) {
        float dt;
        glv_poll(win, down, pressed);
        if (pressed[K_ESC] || down[K_ESC]) break;

        if (pressed['1']) reset_stage(&g, 0);
        if (pressed['2']) reset_stage(&g, 1);
        if (pressed['3']) reset_stage(&g, 2);
        if (pressed['4']) reset_stage(&g, 3);
        if (pressed['5']) reset_stage(&g, 4);
        if (pressed['6']) reset_stage(&g, 5);
        if (pressed['t']) reset_stage(&g, g.stage);
        if (pressed['g']) { g.auto_pilot = !g.auto_pilot; printf("autopilot %s\n", g.auto_pilot ? "on" : "off"); }
        if (pressed['c']) share_code(&g);
        if (pressed['f']) photo(&g);
        if (pressed['n']) {
            static const char *SN[4] = { "grazer", "hunter", "tank", "scout" };
            static const char *SA[3] = { "grazer", "hunter", "diver" };
            if (g.stage == 0) {
                g.style_sel = (g.style_sel + 1) % CP_STYLE_COUNT;
                cp_world_redesign(&g.w, g.style_sel);
                snprintf(g.banner, sizeof(g.banner), "editor: %s", SN[g.style_sel]);
            } else if (g.stage == 1) {
                g.style_sel = (g.style_sel + 1) % CP3_STYLE_COUNT;
                cp3_world_redesign(&g.a, g.style_sel);
                snprintf(g.banner, sizeof(g.banner), "editor: %s", SA[g.style_sel]);
            } else if (g.stage == 2) {
                g.style_sel = (g.style_sel + 1) % CP4_STYLE_COUNT;
                cp4_world_redesign(&g.l, g.style_sel);
                snprintf(g.banner, sizeof(g.banner), "editor: %s", cp4_style_name(g.style_sel));
            } else {
                snprintf(g.banner, sizeof(g.banner), "editor: tribes evolve, not built");
            }
            g.banner_t = 120;
        }

        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = ts.tv_sec + ts.tv_nsec * 1e-9;
        dt = (float)(now - last);
        if (dt > 0.25f) dt = 0.25f;
        last = now;
        acc += dt;

        while (acc >= 1.0f / 15.0f) {
            acc -= 1.0f / 15.0f;
            if (g.stage == 0 && g.w.status == CP_RUN) {
                float act[CP_ACT_DIM] = { 0 };
                if (g.auto_pilot) cp_policy_greedy(&g.w, act);
                else {
                    if (down['a'] || down[K_LEFT]) act[0] -= 1.0f;
                    if (down['d'] || down[K_RIGHT]) act[0] += 1.0f;
                    if (down['w'] || down[K_UP]) act[1] -= 1.0f;
                    if (down['s'] || down[K_DOWN]) act[1] += 1.0f;
                    if (down[32]) act[2] = 1.0f;
                    if (down['e']) act[3] = 1.0f;
                }
                cp_world_step(&g.w, act);
            } else if (g.stage == 1 && g.a.status == CP3_RUN) {
                float act[CP3_ACT_DIM] = { 0 };
                if (g.auto_pilot) cp3_policy_greedy(&g.a, act);
                else {
                    if (down['a'] || down[K_LEFT]) act[0] -= 1.0f;
                    if (down['d'] || down[K_RIGHT]) act[0] += 1.0f;
                    if (down['r']) act[1] -= 1.0f;
                    if (down['f']) act[1] += 1.0f;
                    if (down['w'] || down[K_UP]) act[2] += 1.0f;
                    if (down['s'] || down[K_DOWN]) act[2] -= 0.6f;
                    if (down[32]) act[3] = 1.0f;
                }
                cp3_world_step(&g.a, act);
            } else if (g.stage == 2 && g.l.status == CP4_RUN) {
                float act[CP4_ACT_DIM] = { 0 };
                if (g.auto_pilot) cp4_policy_greedy(&g.l, act);
                else {
                    if (down['a'] || down[K_LEFT]) act[0] -= 1.0f;
                    if (down['d'] || down[K_RIGHT]) act[0] += 1.0f;
                    if (down['w'] || down[K_UP]) act[2] += 1.0f;
                    if (down['s'] || down[K_DOWN]) act[2] -= 0.6f;
                    if (down['r']) act[3] += 1.0f;
                    if (down['f']) act[3] -= 1.0f;
                    if (down[32]) act[4] = 1.0f;
                    if (down['e']) act[5] = 1.0f;
                    if (down['q']) act[6] = 1.0f;
                    if (pressed['b']) act[7] = 1.0f;
                }
                cp4_world_step(&g.l, act);
            } else if (g.stage == 3 && g.t.status == CP6_RUN) {
                float act[CP6_ACT_DIM] = { 0 };
                if (g.auto_pilot) cp6_policy_greedy(&g.t, act);
                else {
                    if (pressed[K_LEFT]) g.cursor = (g.cursor + 4) % 5;
                    if (pressed[K_RIGHT]) g.cursor = (g.cursor + 1) % 5;
                    if (down[K_UP]) g.trib_stance[g.cursor + 1] += 0.05f;
                    if (down[K_DOWN]) g.trib_stance[g.cursor + 1] -= 0.05f;
                    if (pressed['g']) { g.trib_work[0] = 0.7f; g.trib_work[1] = 0.2f; g.trib_work[2] = 0.1f; }
                    if (pressed['y']) { g.trib_work[0] = 0.3f; g.trib_work[1] = 0.5f; g.trib_work[2] = 0.2f; }
                    if (pressed['u']) { g.trib_work[0] = 0.4f; g.trib_work[1] = 0.2f; g.trib_work[2] = 0.4f; }
                    if (pressed['n']) g.trib_hut = 1;
                    for (int k = 0; k < 3; k++) act[k] = g.trib_work[k];
                    for (int k = 0; k < 5; k++)
                        act[3 + k] = clampf(g.trib_stance[k + 1], -1.0f, 1.0f);
                    act[CP6_ACT_DIM - 1] = g.trib_hut ? 0.6f : 0.0f;
                    g.trib_hut = 0;
                    for (int k = 0; k < 5; k++) g.trib_stance[k + 1] *= 0.995f;
                }
                cp6_world_step(&g.t, act);
            } else if (g.stage == 4 && g.c.status == CP5_RUN) {
                float act[CP5_ACT_DIM] = { 0 };
                if (g.auto_pilot) cp5_policy_greedy(&g.c, act);
                else {
                    if (pressed['z']) g.civ_doctrine = 0;
                    if (pressed['x']) g.civ_doctrine = 1;
                    if (pressed['c']) g.civ_doctrine = 2;
                    if (pressed[K_LEFT]) g.civ_focus = (g.civ_focus + g.c.n_cities - 1) % (g.c.n_cities ? g.c.n_cities : 1);
                    if (pressed[K_RIGHT]) g.civ_focus = (g.civ_focus + 1) % (g.c.n_cities ? g.c.n_cities : 1);
                    act[g.civ_doctrine] = 1.0f;
                    if (g.civ_focus >= 0 && g.civ_focus < CP5_MAX_CITIES)
                        act[CP5_APPROACH_COUNT + g.civ_focus] = 1.0f;
                }
                cp5_world_step(&g.c, act);
            } else if (g.stage == 5 && g.s.status == CP7_RUN) {
                float act[CP7_ACT_DIM] = { 0 };
                static const float DIAL[4] = { -0.75f, -0.25f, 0.25f, 0.75f };
                if (g.auto_pilot) cp7_policy_greedy(&g.s, act);
                else {
                    if (down['a'] || down[K_LEFT]) act[CP7_V_THRUST] -= 1.0f;
                    if (down['d'] || down[K_RIGHT]) act[CP7_V_THRUST] += 1.0f;
                    if (down['w'] || down[K_UP]) act[CP7_V_THRUST + 1] += 1.0f;
                    if (down['s'] || down[K_DOWN]) act[CP7_V_THRUST + 1] -= 1.0f;
                    if (down[32]) act[CP7_V_TRADE] = 1.0f;      /* space: trade */
                    if (down['e']) act[CP7_V_COLONISE] = 1.0f;
                    if (down['q']) act[CP7_V_ATTACK] = 1.0f;
                    if (down['r']) act[CP7_V_RESUPPLY] = 1.0f;
                    if (pressed['t']) g.space_dial = (g.space_dial + 1) % 4;
                    act[CP7_V_UPGRADE] = DIAL[g.space_dial];
                }
                cp7_world_step(&g.s, act);
            }
        }
        /* B is edge-triggered for nest: re-fire not needed; pressed[] was
         * consumed above inside the tick loop, so clear stale edges */
        memset(pressed, 0, sizeof(int) * 256);

        /* ---- present ---- */
        memset(fb, 0, (size_t)FW * FH * 4);
        if (g.stage == 0) cp_render_styled(&g.w, fb, FW, FH, CP_VIS_ABYSS);
        else if (g.stage == 1) {
            if ((frame % 3) == 0) cp3_render_styled(&g.a, fb, FW, FH, CP_VIS_ABYSS);
        } else if (g.stage == 2) draw_land(&g, fb);
        else if (g.stage == 3) draw_tribe(&g, fb);
        else if (g.stage == 4) draw_civ(&g, fb);
        else draw_space(&g, fb);
        /* keep last aqua frame rather than black between throttled renders */
        {
            static uint8_t aqua_fb[FW * FH * 4];
            if (g.stage == 1) {
                if ((frame % 3) == 0) memcpy(aqua_fb, fb, sizeof(aqua_fb));
                else memcpy(fb, aqua_fb, sizeof(aqua_fb));
            }
        }
        hud(&g, fb, glv_fps(win));
        glv_present(win, fb);
        glv_tick(win, 60);
        frame++;
    }

    glv_close(win);
    free(fb); free(down); free(pressed);
    printf("game over: stage %d seed %u\n", g.stage, g.seed);
    return 0;
}
