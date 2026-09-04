/* Freestanding string.h for the WebAssembly build. */
#ifndef CPORE_WASM_STRING_H
#define CPORE_WASM_STRING_H
#include <stddef.h>
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
char  *strchr(const char *s, int c);
#endif
