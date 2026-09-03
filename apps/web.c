/* The browser editor's C side.
 *
 * The genome, its prices and the renderer that draws it all already exist and
 * are not duplicated here: this file is only the surface the page reaches
 * through. It keeps one genome and one framebuffer in module memory, so the
 * page edits bytes in place and asks for a frame, rather than marshalling a
 * body plan across the boundary sixty times a second.
 *
 * Cp4Genome is all one-byte fields, which is what makes that work - JavaScript
 * can write a leg's length straight into the struct with no packing rules to
 * agree on. cpw_genome_bytes() exists so the page can assert that its idea of
 * the layout still matches the C one.
 */
#include "cpore/land.h"
#include <emscripten.h>

/* The viewport is rendered small and upscaled with nearest-neighbour, the way
 * every other picture this project makes is. */
#define WEB_MAX_W 720
#define WEB_MAX_H 540
/* MAX_PRIM in the renderer is 176; this side only needs to agree that it is
 * not smaller. */
#define MAX_POSE_PRIMS 176

static Cp4Genome G;
static Cp4Stats  S;
static uint8_t   FB[WEB_MAX_W * WEB_MAX_H * 4];
static float     ST[22];

EMSCRIPTEN_KEEPALIVE uint8_t *cpw_genome(void)     { return (uint8_t *)&G; }
EMSCRIPTEN_KEEPALIVE int      cpw_genome_bytes(void) { return (int)sizeof(Cp4Genome); }
EMSCRIPTEN_KEEPALIVE uint8_t *cpw_frame(void)      { return FB; }
EMSCRIPTEN_KEEPALIVE int      cpw_max_w(void)      { return WEB_MAX_W; }
EMSCRIPTEN_KEEPALIVE int      cpw_max_h(void)      { return WEB_MAX_H; }
EMSCRIPTEN_KEEPALIVE int      cpw_budget(void)     { return CP4_GEN_BUDGET[CP4_GENERATIONS - 1]; }
EMSCRIPTEN_KEEPALIVE int      cpw_max_parts(void)  { return CP4_MAX_PARTS; }
EMSCRIPTEN_KEEPALIVE int      cpw_max_seg(void)    { return CP4_MAX_SEG; }

EMSCRIPTEN_KEEPALIVE int cpw_part_count(void)      { return CP4_PART_COUNT; }
EMSCRIPTEN_KEEPALIVE const char *cpw_part_name(int t) { return cp4_part_name(t); }
EMSCRIPTEN_KEEPALIVE int cpw_part_cost(int t)      { return cp4_part_cost(t); }
EMSCRIPTEN_KEEPALIVE const char *cpw_style_name(int s) { return cp4_style_name(s); }
EMSCRIPTEN_KEEPALIVE int cpw_style_count(void)     { return CP4_STYLE_COUNT; }

EMSCRIPTEN_KEEPALIVE void cpw_render(int w, int h, int style, unsigned seed,
                                     float az, float el, float phase)
{
    if (w < 8 || h < 8 || w > WEB_MAX_W || h > WEB_MAX_H) return;
    cp4_render_pose_phase(&G, FB, w, h, style, seed, az, el, phase);
}

/* The body as data, for the WebGL viewport. C still decides what the genome
 * is; the GPU only marches what it is handed. */
static float PRIMS[MAX_POSE_PRIMS * CP4_POSE_PRIM];
static float META[CP4_POSE_META];

EMSCRIPTEN_KEEPALIVE float *cpw_prims(void) { return PRIMS; }
EMSCRIPTEN_KEEPALIVE float *cpw_meta(void)  { return META; }
EMSCRIPTEN_KEEPALIVE int    cpw_prim_stride(void) { return CP4_POSE_PRIM; }
EMSCRIPTEN_KEEPALIVE int    cpw_max_prims(void)   { return MAX_POSE_PRIMS; }
EMSCRIPTEN_KEEPALIVE int cpw_build(float phase)
{
    return cp4_pose_prims(&G, phase, PRIMS, MAX_POSE_PRIMS, META);
}

/* The palette the CPU renderer would have quantised to, so the shader's
 * optional pixel-art pass lands on exactly the same colours. */
static uint8_t PAL[256 * 3];
static float   DITHER;
EMSCRIPTEN_KEEPALIVE uint8_t *cpw_palette(void) { return PAL; }
EMSCRIPTEN_KEEPALIVE float cpw_dither(void) { return DITHER; }
EMSCRIPTEN_KEEPALIVE int cpw_palette_load(int style)
{
    return cp_vis_palette(style, PAL, 256, &DITHER);
}

EMSCRIPTEN_KEEPALIVE int  cpw_cost(void) { return cp4_genome_cost(&G); }
EMSCRIPTEN_KEEPALIVE void cpw_normalise(int budget) { cp4_genome_normalise(&G, budget); }
EMSCRIPTEN_KEEPALIVE void cpw_starter(void) { cp4_genome_starter(&G); }

EMSCRIPTEN_KEEPALIVE void cpw_autodesign(int style, int budget, unsigned seed)
{
    CpRng r;
    cp_rng_seed(&r, seed);
    cp4_genome_autodesign(&G, &r, budget, style);
}

EMSCRIPTEN_KEEPALIVE void cpw_random(int budget, unsigned seed)
{
    CpRng r;
    cp_rng_seed(&r, seed);
    cp4_genome_random(&G, &r, budget);
}

EMSCRIPTEN_KEEPALIVE void cpw_mutate(int budget, unsigned seed, float rate)
{
    CpRng r;
    cp_rng_seed(&r, seed);
    cp4_genome_mutate(&G, &r, budget, rate);
}

/* The stat block, packed in the order the page prints it. Named fields rather
 * than a raw struct view, because Cp4Stats ends in byte counts and a float
 * window over it would be a layout trap. */
EMSCRIPTEN_KEEPALIVE float *cpw_stats(void)
{
    cp4_genome_stats(&G, &S);
    ST[0]  = S.speed;   ST[1]  = S.accel;   ST[2]  = S.turn;    ST[3] = S.jump;
    ST[4]  = S.grip;    ST[5]  = S.hp_max;  ST[6]  = S.armor;   ST[7] = S.bite;
    ST[8]  = S.claw_dmg; ST[9] = S.charm;   ST[10] = S.social_reach;
    ST[11] = S.sight;   ST[12] = S.hearing; ST[13] = S.reach;   ST[14] = S.carry;
    ST[15] = S.stamina; ST[16] = S.upkeep;  ST[17] = S.swim;    ST[18] = S.fly;
    ST[19] = S.dig;     ST[20] = S.breath;  ST[21] = (float)S.n_parts;
    return ST;
}
