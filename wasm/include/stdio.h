/* Freestanding stdio.h for the WebAssembly build.
 *
 * The renderers' HUDs format text with snprintf, and the editor's viewport
 * draws no HUD - so with --gc-sections those paths are dropped and nothing
 * here is ever called. The declarations exist so the translation units still
 * compile; the definitions in shim.c exist so that if a path ever does
 * survive the collector, it fails visibly rather than at link time. */
#ifndef CPORE_WASM_STDIO_H
#define CPORE_WASM_STDIO_H
#include <stddef.h>
#include <stdarg.h>
int snprintf(char *buf, size_t n, const char *fmt, ...);
int printf(const char *fmt, ...);
#endif
