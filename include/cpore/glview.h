/* glview - the native GPU present path.
 *
 * The sim stays dependency-free and renders screenshots on the CPU. This is
 * the other end: an X11+GLX window that presents frames on the GPU with
 * integer pixel-art scaling, vsync, and an FPS counter. No SDL, no GLFW, no
 * loader - fixed-function GL + GLX 1.3 only, so it builds on the stock
 * system headers (GL/glx.h, X11/Xlib.h) that are already installed.
 *
 * Deliberately thin: the sim never knows about this file. The upgrade path
 * is documented below and needs no changes here beyond filling in the
 * shader tier:
 *
 *   1. Scene-as-data already exists: cp4_pose_prims() hands the creature
 *      over as round-cone primitives, and cp_vis_palette() hands every
 *      style's palette to "a GPU shader, say" (cpore.h). Those two APIs
 *      ARE the GPU contract; this file owns the context they will run in.
 *   2. Next tier: upload the low-res linear frame as a texture and do the
 *      palette quantise + ordered dither + upscale in a fragment shader
 *      (runtime-loaded via glXGetProcAddress, fixed-function fallback).
 *   3. After that: a GPU terrain mesh from cp4_height() + instanced
 *      impostors from cp4_pose_prims(), replacing the CPU marcher for the
 *      interactive tier. The screenshot tier stays CPU - beauty is tiered,
 *      not traded away (GAME_DESIGN.md M1).
 */
#ifndef CPORE_GLVIEW_H
#define CPORE_GLVIEW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GlvWindow GlvWindow;

/* Open a window. scale: integer upscale of the low-res frame (0 = fit).
 * Returns NULL on failure (no X display, no GLX). */
GlvWindow *glv_open(const char *title, int frame_w, int frame_h, int scale);
/* Resize the presented frame (e.g. stage switch changes resolution). */
void glv_set_frame(GlvWindow *g, int frame_w, int frame_h);
void glv_close(GlvWindow *g);

/* Present one RGBA frame (frame_w*frame_h as given to open/set_frame).
 * Nearest-neighbour, aspect-correct, letterboxed. */
void glv_present(GlvWindow *g, const uint8_t *rgba);

/* Input: poll X events. Returns 0 on quit (window close / Escape).
 * keys: caller-owned int[256] zeroed once; indexed by KeySym (a-z, space,
 * arrows as 0xFF51..). down[k]=1 while held, pressed[k]=edge this poll. */
void glv_poll(GlvWindow *g, int *down, int *pressed);

/* Frame pacing: block to hold `fps` presents/s. Returns measured fps. */
double glv_tick(GlvWindow *g, int fps);

/* Last measured present rate (for the HUD FPS counter). */
double glv_fps(const GlvWindow *g);

#ifdef __cplusplus
}
#endif
#endif /* CPORE_GLVIEW_H */
