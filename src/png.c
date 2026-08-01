#include "cpore/cpore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal PNG writer with a real (fixed-Huffman) DEFLATE compressor.
 * No libpng, no zlib, no stb - the whole point of the project is that the
 * dependency list is `libc` and `libm`. */

/* ---------------- bit writer ---------------- */

typedef struct {
    uint8_t *buf;
    size_t   len, cap;
    uint32_t acc;
    int      nbits;
    int      err;
} BitW;

static void bw_byte(BitW *w, uint8_t b)
{
    if (w->len == w->cap) {
        size_t nc = w->cap ? w->cap * 2 : 1 << 16;
        uint8_t *nb = (uint8_t *)realloc(w->buf, nc);
        if (!nb) { w->err = 1; return; }
        w->buf = nb; w->cap = nc;
    }
    w->buf[w->len++] = b;
}

/* deflate bit order: LSB-first within a byte */
static void bw_bits(BitW *w, uint32_t v, int n)
{
    w->acc |= (v & ((1u << n) - 1u)) << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        bw_byte(w, (uint8_t)(w->acc & 0xFF));
        w->acc >>= 8;
        w->nbits -= 8;
    }
}

/* Huffman codes are stored MSB-first, then fed into the LSB-first stream */
static void bw_huff(BitW *w, uint32_t code, int n)
{
    for (int i = n - 1; i >= 0; i--) bw_bits(w, (code >> i) & 1u, 1);
}

static void bw_flush(BitW *w)
{
    if (w->nbits > 0) bw_byte(w, (uint8_t)(w->acc & 0xFF));
    w->acc = 0; w->nbits = 0;
}

/* ---------------- fixed Huffman symbol emission ---------------- */

static void emit_sym(BitW *w, int sym)
{
    if      (sym <= 143) bw_huff(w, 0x30u + sym, 8);
    else if (sym <= 255) bw_huff(w, 0x190u + (sym - 144), 9);
    else if (sym <= 279) bw_huff(w, (uint32_t)(sym - 256), 7);
    else                 bw_huff(w, 0xC0u + (sym - 280), 8);
}

static const uint16_t LBASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,
                                    59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  LEXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DBASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,
                                    769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  DEXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static void emit_match(BitW *w, int len, int dist)
{
    int li = 28;
    while (li > 0 && len < LBASE[li]) li--;
    emit_sym(w, 257 + li);
    if (LEXTRA[li]) bw_bits(w, (uint32_t)(len - LBASE[li]), LEXTRA[li]);

    int di = 29;
    while (di > 0 && dist < DBASE[di]) di--;
    bw_huff(w, (uint32_t)di, 5);
    if (DEXTRA[di]) bw_bits(w, (uint32_t)(dist - DBASE[di]), DEXTRA[di]);
}

/* ---------------- LZ77 ---------------- */

#define HASH_BITS  15
#define HASH_SIZE  (1 << HASH_BITS)
#define MAX_CHAIN  32
#define MAX_DIST   32768
#define MAX_MATCH  258

static inline uint32_t hash3(const uint8_t *p)
{
    uint32_t h = (uint32_t)p[0] * 0x9E3779B1u
               ^ (uint32_t)p[1] * 0x85EBCA77u
               ^ (uint32_t)p[2] * 0xC2B2AE3Du;
    return h >> (32 - HASH_BITS);
}

static uint8_t *deflate_fixed(const uint8_t *src, size_t n, size_t *out_len)
{
    BitW w;
    memset(&w, 0, sizeof(w));

    int32_t *head = (int32_t *)malloc(sizeof(int32_t) * HASH_SIZE);
    int32_t *prev = (int32_t *)malloc(sizeof(int32_t) * (n ? n : 1));
    if (!head || !prev) { free(head); free(prev); return NULL; }
    memset(head, 0xFF, sizeof(int32_t) * HASH_SIZE);

    bw_bits(&w, 1, 1);   /* BFINAL */
    bw_bits(&w, 1, 2);   /* BTYPE = fixed Huffman */

    size_t i = 0;
    while (i < n) {
        int best_len = 0, best_dist = 0;

        if (i + 3 <= n) {
            uint32_t h = hash3(src + i);
            int32_t cand = head[h];
            for (int chain = 0; cand >= 0 && chain < MAX_CHAIN; chain++) {
                size_t dist = i - (size_t)cand;
                if (dist == 0 || dist > MAX_DIST) break;
                size_t maxl = n - i;
                if (maxl > MAX_MATCH) maxl = MAX_MATCH;
                size_t l = 0;
                while (l < maxl && src[cand + l] == src[i + l]) l++;
                if ((int)l > best_len) {
                    best_len = (int)l; best_dist = (int)dist;
                    if (l >= MAX_MATCH) break;
                }
                cand = prev[cand];
            }
            prev[i] = head[h];
            head[h] = (int32_t)i;
        }

        if (best_len >= 3) {
            emit_match(&w, best_len, best_dist);
            /* keep the hash chains dense across the skipped bytes */
            for (size_t k = 1; k < (size_t)best_len && i + k + 3 <= n; k++) {
                uint32_t h2 = hash3(src + i + k);
                prev[i + k] = head[h2];
                head[h2] = (int32_t)(i + k);
            }
            i += (size_t)best_len;
        } else {
            emit_sym(&w, src[i]);
            i++;
        }
    }

    emit_sym(&w, 256);   /* end of block */
    bw_flush(&w);

    free(head);
    free(prev);
    if (w.err) { free(w.buf); return NULL; }
    *out_len = w.len;
    return w.buf;
}

/* ---------------- checksums ---------------- */

static uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = 1;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static uint32_t adler32_buf(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/* ---------------- png container ---------------- */

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char *tag, const uint8_t *data, size_t n)
{
    uint8_t hdr[8];
    put_be32(hdr, (uint32_t)n);
    memcpy(hdr + 4, tag, 4);
    if (fwrite(hdr, 1, 8, f) != 8) return -1;
    if (n && fwrite(data, 1, n, f) != n) return -1;

    uint32_t crc = crc32_buf(0, hdr + 4, 4);
    if (n) crc = crc32_buf(crc, data, n);
    uint8_t tail[4];
    put_be32(tail, crc);
    return fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

int cp_png_write(const char *path, const uint8_t *rgba, int width, int height)
{
    if (width <= 0 || height <= 0) return -1;

    /* Sub filter (type 1) - cheap, and it flattens the gradients nicely */
    const size_t stride = (size_t)width * 4;
    const size_t raw_n = (stride + 1) * (size_t)height;
    uint8_t *raw = (uint8_t *)malloc(raw_n);
    if (!raw) return -1;

    for (int y = 0; y < height; y++) {
        uint8_t *dst = raw + (stride + 1) * (size_t)y;
        const uint8_t *src = rgba + stride * (size_t)y;
        dst[0] = 1;
        for (size_t x = 0; x < stride; x++)
            dst[1 + x] = (uint8_t)(src[x] - (x >= 4 ? src[x - 4] : 0));
    }

    size_t z_n = 0;
    uint8_t *z = deflate_fixed(raw, raw_n, &z_n);
    if (!z) { free(raw); return -1; }

    uint8_t *idat = (uint8_t *)malloc(z_n + 6);
    if (!idat) { free(raw); free(z); return -1; }
    idat[0] = 0x78; idat[1] = 0x01;                 /* zlib header, no dict */
    memcpy(idat + 2, z, z_n);
    put_be32(idat + 2 + z_n, adler32_buf(raw, raw_n));
    free(z);

    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(idat); return -1; }

    static const uint8_t sig[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    int rc = (fwrite(sig, 1, 8, f) == 8) ? 0 : -1;

    uint8_t ihdr[13];
    put_be32(ihdr, (uint32_t)width);
    put_be32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;      /* bit depth   */
    ihdr[9] = 6;      /* RGBA        */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    if (!rc) rc = write_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    if (!rc) rc = write_chunk(f, "IDAT", idat, z_n + 6);
    if (!rc) rc = write_chunk(f, "IEND", NULL, 0);

    fclose(f);
    free(raw);
    free(idat);
    return rc;
}
