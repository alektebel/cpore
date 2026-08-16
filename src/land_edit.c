/* The creature editor's session ABI.
 *
 * Everything above this file speaks in structs: a Cp4Genome, a Cp4View, a
 * studio. That is the right shape for C and the wrong shape for anything
 * calling in from outside it - ctypes and WebAssembly both have to be handed
 * a struct layout, and a struct layout is a promise this project changes
 * every time it adds a gene. Twice now the genome has grown a field.
 *
 * So the boundary is drawn here instead: an opaque handle, integers and flat
 * arrays, and no caller anywhere needs to know what a Cp4Genome looks like.
 * A Python binding and a browser front end want exactly the same surface -
 * open a session, tell it where the mouse is, get pixels back - which is why
 * there is one of these and not two.
 *
 * Every call is in screen coordinates where a mouse would be, because the
 * whole point of the studio underneath is that pixels and genes agree.
 */

#include "cpore/land.h"

#include <stdlib.h>
#include <string.h>

struct Cp4Edit {
    Cp4Studio *studio;
    Cp4Genome  g;
    Cp4View    view;
    int        budget;
    int        w, h;
};

Cp4Edit *cp4_edit_create(int32_t w, int32_t h, int32_t budget)
{
    if (w < 16 || h < 16) return NULL;
    Cp4Edit *e = (Cp4Edit *)calloc(1, sizeof(Cp4Edit));
    if (!e) return NULL;
    e->studio = cp4_studio_new((int)w, (int)h);
    if (!e->studio) { free(e); return NULL; }
    e->w = (int)w; e->h = (int)h;
    e->budget = budget > 0 ? (int)budget : CP4_GEN_BUDGET[CP4_GENERATIONS - 1];
    cp4_genome_clear(&e->g);
    cp4_genome_starter(&e->g);
    e->view.azimuth = 2.3562f;
    e->view.elev = 0.26f;
    e->view.zoom = 1.0f;
    e->view.phase = 0.0f;
    e->view.quality = 2;
    return e;
}

void cp4_edit_free(Cp4Edit *e)
{
    if (!e) return;
    cp4_studio_free(e->studio);
    free(e);
}

void cp4_edit_budget(Cp4Edit *e, int32_t budget)
{
    if (e && budget > 0) e->budget = (int)budget;
}

/* ---- loading a body plan ---- *
 * parts is CP4_MAX_PARTS groups of eight int32: type, seg, yaw, pitch, scale,
 * mirror, len, bend - the same marshalling cp4_env_reset already uses, so a
 * caller that can build an environment can build an editor session. */
void cp4_edit_load(Cp4Edit *e, const int32_t *parts, int32_t nseg, int32_t girth)
{
    if (!e) return;
    cp4_genome_clear(&e->g);
    if (parts) {
        for (int i = 0; i < CP4_MAX_PARTS; i++) {
            const int32_t *p = parts + i * 8;
            int t = p[0];
            if (t <= CP4_NONE || t >= CP4_PART_COUNT) continue;
            e->g.part[i].type   = (uint8_t)t;
            e->g.part[i].seg    = (uint8_t)(p[1] < 0 ? 0 : (p[1] >= CP4_MAX_SEG ? CP4_MAX_SEG - 1 : p[1]));
            e->g.part[i].yaw    = (uint8_t)(p[2] & 0xFF);
            e->g.part[i].pitch  = (int8_t)(p[3] > 127 ? 127 : (p[3] < -127 ? -127 : p[3]));
            e->g.part[i].scale  = (uint8_t)(p[4] > 255 ? 255 : (p[4] < 20 ? 20 : p[4]));
            e->g.part[i].mirror = (uint8_t)(p[5] ? 1 : 0);
            e->g.part[i].len    = (uint8_t)(p[6] > 255 ? 255 : (p[6] < 20 ? 20 : p[6]));
            e->g.part[i].bend   = (int8_t)(p[7] > 127 ? 127 : (p[7] < -127 ? -127 : p[7]));
        }
    }
    cp4_genome_spine(&e->g, nseg, girth, -2000, -2000);
}

void cp4_edit_random(Cp4Edit *e, uint32_t seed)
{
    if (!e) return;
    CpRng r;
    cp_rng_seed(&r, seed);
    cp4_genome_random(&e->g, &r, e->budget);
}

void cp4_edit_style(Cp4Edit *e, int32_t style)
{
    if (!e) return;
    if (style < 0 || style >= CP4_STYLE_COUNT) style = 0;
    cp4_genome_autodesign(&e->g, NULL, e->budget, (int)style);
}

void cp4_edit_mutate(Cp4Edit *e, uint32_t seed, float rate)
{
    if (!e) return;
    CpRng r;
    cp_rng_seed(&r, seed);
    cp4_genome_mutate(&e->g, &r, e->budget, rate);
}

/* ---- the camera ---- */

void cp4_edit_view(Cp4Edit *e, float azimuth, float elev, float zoom, float phase)
{
    if (!e) return;
    e->view.azimuth = azimuth;
    /* Stopping short of the poles: straight down a creature is a plan view
     * with no legs in it, and the up vector degenerates there anyway. */
    e->view.elev = elev > 1.40f ? 1.40f : (elev < -1.40f ? -1.40f : elev);
    e->view.zoom = zoom > 0.15f ? (zoom < 8.0f ? zoom : 8.0f) : 0.15f;
    e->view.phase = phase;
}

void cp4_edit_orbit(Cp4Edit *e, float dazimuth, float delev)
{
    if (!e) return;
    cp4_edit_view(e, e->view.azimuth + dazimuth, e->view.elev + delev,
                  e->view.zoom, e->view.phase);
}

void cp4_edit_get_view(const Cp4Edit *e, float *out /* 4 */)
{
    if (!e || !out) return;
    out[0] = e->view.azimuth; out[1] = e->view.elev;
    out[2] = e->view.zoom;    out[3] = e->view.phase;
}

void cp4_edit_render(Cp4Edit *e, uint8_t *rgba, int32_t quality)
{
    if (!e || !rgba) return;
    Cp4View v = e->view;
    v.quality = (int)quality;
    cp4_studio_render(e->studio, &e->g, &v, rgba);
}

/* ---- what the mouse does ---- */

/* Is the pointer over the body, and where? cp4_edit_pick answers "which part"
 * and correctly says none over bare trunk, which leaves a front end with no
 * way to ask the question a hover cursor and a drop preview both need. Out
 * takes seg, yaw and pitch - the genes that would put a part right there. */
int32_t cp4_edit_surface(Cp4Edit *e, int32_t x, int32_t y, int32_t *out /* 3 */)
{
    if (!e) return 0;
    int32_t seg = 0, yaw = 0, pitch = 0;
    if (!cp4_studio_surface(e->studio, &e->g, &e->view, (int)x, (int)y,
                            &seg, &yaw, &pitch))
        return 0;
    if (out) { out[0] = seg; out[1] = yaw; out[2] = pitch; }
    return 1;
}

int32_t cp4_edit_pick(Cp4Edit *e, int32_t x, int32_t y)
{
    if (!e) return -1;
    return cp4_studio_pick(e->studio, &e->g, &e->view, (int)x, (int)y);
}

/* Drop a new part where the pointer is. Returns the slot it went into, or -1
 * if the pointer was not over the body, there was no slot free, or the budget
 * could not take it - and in every one of those cases nothing has changed,
 * which is the contract that lets a front end just try it and report. */
int32_t cp4_edit_drop(Cp4Edit *e, int32_t x, int32_t y, int32_t type, int32_t mirror)
{
    if (!e) return -1;
    int32_t seg, yaw, pitch;
    if (!cp4_studio_surface(e->studio, &e->g, &e->view, (int)x, (int)y,
                            &seg, &yaw, &pitch))
        return -1;
    return cp4_genome_place(&e->g, (int)type, (int)seg, (int)yaw, (int)pitch,
                            (int)mirror, e->budget);
}

int32_t cp4_edit_move(Cp4Edit *e, int32_t slot, int32_t x, int32_t y)
{
    if (!e) return 0;
    int32_t seg, yaw, pitch;
    if (!cp4_studio_surface(e->studio, &e->g, &e->view, (int)x, (int)y,
                            &seg, &yaw, &pitch))
        return 0;
    return cp4_genome_move(&e->g, (int)slot, (int)seg, (int)yaw, (int)pitch);
}

int32_t cp4_edit_remove(Cp4Edit *e, int32_t slot)
{
    return e ? cp4_genome_remove(&e->g, (int)slot) : 0;
}

int32_t cp4_edit_shape(Cp4Edit *e, int32_t slot, int32_t scale, int32_t len, int32_t bend)
{
    return e ? cp4_genome_shape(&e->g, (int)slot, (int)scale, (int)len, (int)bend) : 0;
}

int32_t cp4_edit_mirror(Cp4Edit *e, int32_t slot, int32_t on)
{
    return e ? cp4_genome_mirror(&e->g, (int)slot, (int)on, e->budget) : 0;
}

int32_t cp4_edit_spine_pick(Cp4Edit *e, int32_t x, int32_t y, float grab_px)
{
    if (!e) return -1;
    return cp4_studio_spine_pick(e->studio, &e->g, &e->view, (int)x, (int)y, grab_px);
}

int32_t cp4_edit_spine_drag(Cp4Edit *e, int32_t vert, int32_t x, int32_t y)
{
    if (!e) return 0;
    return cp4_studio_spine_drag(e->studio, &e->g, &e->view, (int)vert, (int)x, (int)y);
}

int32_t cp4_edit_spine_girth(Cp4Edit *e, int32_t vert, float amount)
{
    return e ? cp4_studio_spine_girth(&e->g, (int)vert, amount) : 0;
}

void cp4_edit_spine_set(Cp4Edit *e, int32_t nseg, int32_t girth,
                        int32_t arch, int32_t sweep)
{
    if (e) cp4_genome_spine(&e->g, (int)nseg, (int)girth, (int)arch, (int)sweep);
}

void cp4_edit_paint(Cp4Edit *e, int32_t hue, int32_t hue2, int32_t hue3,
                    int32_t sat, int32_t val)
{
    if (e) cp4_genome_paint(&e->g, (int)hue, (int)hue2, (int)hue3, (int)sat, (int)val);
}

void cp4_edit_coats(Cp4Edit *e, int32_t pattern, int32_t pscale,
                    int32_t pattern2, int32_t pscale2)
{
    if (e) cp4_genome_coats(&e->g, (int)pattern, (int)pscale,
                            (int)pattern2, (int)pscale2);
}

/* ---- reading the session back ---- */

int32_t cp4_edit_cost(const Cp4Edit *e)      { return e ? cp4_genome_cost(&e->g) : 0; }
int32_t cp4_edit_budget_get(const Cp4Edit *e) { return e ? (int32_t)e->budget : 0; }

int32_t cp4_edit_can_afford(const Cp4Edit *e, int32_t type, int32_t mirror)
{
    return e ? cp4_genome_can_afford(&e->g, (int)type, (int)mirror, e->budget) : 0;
}

/* CP4_MAX_PARTS groups of eight, same order as cp4_edit_load takes them. An
 * empty slot comes back with type 0, so a front end can render the roster
 * without a second call to ask how many there are. */
void cp4_edit_genome(const Cp4Edit *e, int32_t *out)
{
    if (!e || !out) return;
    for (int i = 0; i < CP4_MAX_PARTS; i++) {
        int32_t *p = out + i * 8;
        p[0] = e->g.part[i].type;
        p[1] = e->g.part[i].seg;
        p[2] = e->g.part[i].yaw;
        p[3] = e->g.part[i].pitch;
        p[4] = e->g.part[i].scale;
        p[5] = e->g.part[i].mirror;
        p[6] = e->g.part[i].len;
        p[7] = e->g.part[i].bend;
    }
}

/* nseg, girth, arch, sweep, then hue/hue2/hue3/sat/val, then the two coats */
void cp4_edit_body(const Cp4Edit *e, int32_t *out /* 13 */)
{
    if (!e || !out) return;
    out[0] = e->g.nseg;     out[1] = e->g.girth;
    out[2] = e->g.arch;     out[3] = e->g.sweep;
    out[4] = e->g.hue;      out[5] = e->g.hue2;   out[6] = e->g.hue3;
    out[7] = e->g.sat;      out[8] = e->g.val;
    out[9] = e->g.pattern;  out[10] = e->g.pscale;
    out[11] = e->g.pattern2; out[12] = e->g.pscale2;
}

/* The stat block, in the order a readout would list it. Fixed layout on
 * purpose: a caller reading floats out of an array should not have to know
 * what a Cp4Stats looks like either. */
#define EDIT_STAT_N 20

int32_t cp4_edit_stat_count(void) { return EDIT_STAT_N; }

void cp4_edit_stats(const Cp4Edit *e, float *out /* EDIT_STAT_N */)
{
    if (!e || !out) return;
    Cp4Stats s;
    cp4_genome_stats(&e->g, &s);
    out[0]  = s.speed;    out[1]  = s.accel;   out[2]  = s.turn;
    out[3]  = s.jump;     out[4]  = s.grip;    out[5]  = s.hp_max;
    out[6]  = s.armor;    out[7]  = s.bite;    out[8]  = s.claw_dmg;
    out[9]  = s.graze_eff; out[10] = s.carn_eff;
    out[11] = s.sight;    out[12] = s.hearing; out[13] = s.charm;
    out[14] = s.reach;    out[15] = s.carry;   out[16] = s.stamina;
    out[17] = s.swim;     out[18] = s.fly;     out[19] = s.dig;
}

/* Hand the finished animal to the simulation. Normalising here rather than on
 * every edit is the whole reason slots stay stable while editing: compaction
 * happens once, at the moment the genome stops being a document and starts
 * being a creature. */
void cp4_edit_finish(Cp4Edit *e, int32_t *parts_out)
{
    if (!e) return;
    cp4_genome_normalise(&e->g, e->budget);
    if (parts_out) cp4_edit_genome(e, parts_out);
}
