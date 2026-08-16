/* Freestanding stdlib.h for the WebAssembly build. */
#ifndef CPORE_WASM_STDLIB_H
#define CPORE_WASM_STDLIB_H
#include <stddef.h>
void *malloc(size_t n);
void *calloc(size_t n, size_t sz);
void *realloc(void *p, size_t n);
void  free(void *p);
int   abs(int v);
#endif
