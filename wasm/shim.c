/* The whole C runtime the WebAssembly build needs.
 *
 * cpore's claim is that it depends on libc and libm and nothing else. In a
 * browser it does not get those either, so the honest options were to pull in
 * a toolchain that brings its own (Emscripten, which is a large dependency to
 * take on in order to prove you have none) or to write the parts actually
 * used. The parts actually used are: an allocator, five memory functions, and
 * the transcendentals - and the browser already has the transcendentals, so
 * those are imports rather than code.
 *
 * What is left is this file. It is deliberately small and deliberately dumb.
 */

#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * memory
 * ------------------------------------------------------------------ */

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    unsigned char v = (unsigned char)c;
    /* Word at a time where it can be. The renderers clear multi-megabyte
     * float buffers every frame, and a byte loop over twenty megabytes is
     * measurable next to the marching that follows it. */
    while (n && ((uintptr_t)p & 3u)) { *p++ = v; n--; }
    uint32_t w = (uint32_t)v * 0x01010101u;
    while (n >= 4) { *(uint32_t *)p = w; p += 4; n -= 4; }
    while (n--) *p++ = v;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n && (((uintptr_t)p | (uintptr_t)q) & 3u)) { *p++ = *q++; n--; }
    while (n >= 4) { *(uint32_t *)p = *(const uint32_t *)q; p += 4; q += 4; n -= 4; }
    while (n--) *p++ = *q++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    if (p == q || n == 0) return d;
    if (p < q) return memcpy(d, s, n);
    p += n; q += n;
    while (n--) *--p = *--q;
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (; n--; x++, y++) if (*x != *y) return (int)*x - (int)*y;
    return 0;
}

size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if (*s == (char)c) return (char *)s;
    return c ? NULL : (char *)s;
}

int abs(int v) { return v < 0 ? -v : v; }

/* ------------------------------------------------------------------ *
 * the allocator
 *
 * First fit over a singly linked list of blocks, with coalescing on free.
 * That is a 1970s allocator and it is the right one here: the editor makes a
 * handful of large, long-lived allocations when a session opens, and two or
 * three per frame that are freed before the next one. There is no workload in
 * this program that a size-bucketed allocator would serve better, and every
 * line of allocator is a line that is not the renderer.
 *
 * Memory comes from WebAssembly's linear memory, grown a page at a time and
 * never given back - the module lives as long as the tab does, and nothing
 * else is competing for the address space.
 * ------------------------------------------------------------------ */

#define ALIGN 16u
#define PAGE  65536u

typedef struct Block {
    size_t        size;      /* payload bytes, not counting this header */
    struct Block *next;      /* next block in address order             */
    uint32_t      free;
    uint32_t      pad;       /* keep the header a multiple of ALIGN     */
} Block;

static Block *heap_head = NULL;
static unsigned char *brk_ptr = NULL;
static unsigned char *brk_end = NULL;

extern unsigned char __heap_base;   /* placed by wasm-ld after the data */

static void *grow(size_t need)
{
    if (!brk_ptr) { brk_ptr = &__heap_base; brk_end = brk_ptr; }
    if ((size_t)(brk_end - brk_ptr) < need) {
        size_t want = need - (size_t)(brk_end - brk_ptr);
        size_t pages = (want + PAGE - 1) / PAGE;
        /* ask for a little more than needed: growing is a syscall-shaped
         * operation and the caller is usually about to ask again */
        if (pages < 16) pages = 16;
        if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return NULL;
        brk_end += pages * PAGE;
    }
    void *p = brk_ptr;
    brk_ptr += need;
    return p;
}

static size_t align_up(size_t n) { return (n + (ALIGN - 1u)) & ~(size_t)(ALIGN - 1u); }

void *malloc(size_t n)
{
    if (n == 0) return NULL;
    n = align_up(n);

    Block *prev = NULL, *b = heap_head;
    for (; b; prev = b, b = b->next) {
        if (!b->free || b->size < n) continue;
        /* Split only when the remainder could hold something. A block cut to
         * leave sixteen spare bytes is a block that will never be reused and
         * a header that will never be reclaimed. */
        if (b->size >= n + sizeof(Block) + ALIGN) {
            Block *rest = (Block *)((unsigned char *)(b + 1) + n);
            rest->size = b->size - n - sizeof(Block);
            rest->next = b->next;
            rest->free = 1;
            rest->pad = 0;
            b->size = n;
            b->next = rest;
        }
        b->free = 0;
        return (void *)(b + 1);
    }

    Block *nb = (Block *)grow(sizeof(Block) + n);
    if (!nb) return NULL;
    nb->size = n;
    nb->next = NULL;
    nb->free = 0;
    nb->pad = 0;
    if (prev) prev->next = nb;
    else      heap_head = nb;
    return (void *)(nb + 1);
}

void free(void *p)
{
    if (!p) return;
    Block *b = ((Block *)p) - 1;
    b->free = 1;
    /* Coalesce forward as far as it goes. Backward coalescing would want a
     * doubly linked list; forward alone is enough because the frame-by-frame
     * pattern here is allocate-several-then-free-several in order. */
    while (b->next && b->next->free
           && (unsigned char *)(b + 1) + b->size == (unsigned char *)b->next) {
        b->size += sizeof(Block) + b->next->size;
        b->next = b->next->next;
    }
}

void *calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    if (sz && total / sz != n) return NULL;      /* overflow */
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (n == 0) { free(p); return NULL; }
    Block *b = ((Block *)p) - 1;
    if (b->size >= n) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, b->size);
    free(p);
    return q;
}

/* ------------------------------------------------------------------ *
 * stdio, or the absence of it
 *
 * The renderers' HUDs format with snprintf and the editor's viewport draws no
 * HUD, so --gc-sections removes those paths and nothing below is reached.
 * They exist so that a path which does survive fails as an empty string
 * rather than as a link error six months from now.
 * ------------------------------------------------------------------ */

/* Enough snprintf to draw a HUD.
 *
 * This started life as a stub that wrote an empty string, on the reasoning
 * that the editor's viewport draws no HUD and --gc-sections would drop the
 * callers. That stopped being true the moment the cell stage became playable
 * in a browser: every readout on screen is formatted through here, and a stub
 * turns them all into blank space with no error anywhere.
 *
 * %d, %u, %s, %c, %% and a fixed-point %f are the whole set the HUDs use.
 * Width and precision are supported only in the forms they actually appear in.
 * Everything unrecognised is copied through literally, which makes a missing
 * conversion visible in the output instead of silent.
 */
static char *fmt_u(char *p, unsigned long v)
{
    char tmp[24];
    int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

int vsnprintf_(char *buf, size_t n, const char *fmt, __builtin_va_list ap)
{
    /* Formatted into a local buffer and copied, so the width logic never has
     * to think about the caller's bound. The HUD strings are short and the
     * bound is checked once, at the end. */
    char out[512];
    char *p = out;
    char *end = out + sizeof(out) - 1;

    for (; *fmt && p < end; fmt++) {
        if (*fmt != '%') { *p++ = *fmt; continue; }
        fmt++;
        if (*fmt == '%') { *p++ = '%'; continue; }

        /* flags and width, of which only zero-padding and a plain field width
         * are ever asked for */
        int zero = 0, width = 0, prec = -1;
        if (*fmt == '-') fmt++;                 /* left-align: ignored */
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }
        while (*fmt == 'l') fmt++;

        char field[64];
        char *q = field;
        switch (*fmt) {
        case 'd': case 'i': {
            long v = __builtin_va_arg(ap, int);
            if (v < 0) { *q++ = '-'; v = -v; }
            q = fmt_u(q, (unsigned long)v);
            break;
        }
        case 'u':
            q = fmt_u(q, (unsigned long)__builtin_va_arg(ap, unsigned int));
            break;
        case 'c':
            *q++ = (char)__builtin_va_arg(ap, int);
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s && q < field + sizeof(field) - 1) *q++ = *s++;
            break;
        }
        case 'f': case 'F': {
            /* Fixed point, because a HUD never needs an exponent and doing
             * this properly would be more code than the rest of the file. */
            double v = __builtin_va_arg(ap, double);
            if (prec < 0) prec = 6;
            if (prec > 9) prec = 9;
            if (v < 0.0) { *q++ = '-'; v = -v; }
            unsigned long scale = 1;
            for (int i = 0; i < prec; i++) scale *= 10u;
            /* Round half away from zero, then split, so 0.95 at one place is
             * 1.0 rather than 0.9. */
            unsigned long whole = (unsigned long)v;
            double frac = v - (double)whole;
            unsigned long f = (unsigned long)(frac * (double)scale + 0.5);
            if (f >= scale) { whole++; f -= scale; }
            q = fmt_u(q, whole);
            if (prec > 0) {
                *q++ = '.';
                for (int i = prec - 1; i >= 0; i--) {
                    unsigned long d = f;
                    for (int k = 0; k < i; k++) d /= 10u;
                    *q++ = (char)('0' + (d % 10u));
                }
            }
            break;
        }
        default:
            /* Unknown conversion: show it, do not swallow it. */
            *q++ = '%';
            *q++ = *fmt ? *fmt : '?';
            break;
        }

        int len = (int)(q - field);
        for (int i = len; i < width && p < end; i++) *p++ = zero ? '0' : ' ';
        for (int i = 0; i < len && p < end; i++) *p++ = field[i];
        if (!*fmt) break;
    }
    *p = '\0';

    int total = (int)(p - out);
    if (buf && n) {
        size_t copy = (size_t)total < n - 1 ? (size_t)total : n - 1;
        memcpy(buf, out, copy);
        buf[copy] = '\0';
    }
    return total;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int r = vsnprintf_(buf, n, fmt, ap);
    __builtin_va_end(ap);
    return r;
}

int printf(const char *fmt, ...) { (void)fmt; return 0; }

/* ------------------------------------------------------------------ *
 * what the host needs to see
 * ------------------------------------------------------------------ */

/* Where a JS caller should write into, and read out of. Exporting the
 * allocator rather than a fixed scratch buffer means the page can size its
 * framebuffer to the canvas instead of to a guess. */
void *cp_wasm_alloc(int32_t n) { return malloc((size_t)n); }
void  cp_wasm_free(void *p)    { free(p); }
