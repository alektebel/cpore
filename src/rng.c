#include "cpore/cpore.h"

/* xoshiro128** - small, fast, and entirely contained in the world struct so
 * that a snapshot round-trips the whole stochastic future. */

static inline uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }

void cp_rng_seed(CpRng *r, uint32_t seed)
{
    /* splitmix32 to spread a single integer over the 128-bit state */
    uint32_t z = seed + 0x9E3779B9u;
    for (int i = 0; i < 4; i++) {
        z += 0x9E3779B9u;
        uint32_t t = z;
        t = (t ^ (t >> 16)) * 0x21F0AAADu;
        t = (t ^ (t >> 15)) * 0x735A2D97u;
        r->s[i] = t ^ (t >> 15);
    }
    if ((r->s[0] | r->s[1] | r->s[2] | r->s[3]) == 0) r->s[0] = 1u;
}

uint32_t cp_rng_u32(CpRng *r)
{
    uint32_t res = rotl(r->s[1] * 5u, 7) * 9u;
    uint32_t t = r->s[1] << 9;
    r->s[2] ^= r->s[0];
    r->s[3] ^= r->s[1];
    r->s[1] ^= r->s[2];
    r->s[0] ^= r->s[3];
    r->s[2] ^= t;
    r->s[3] = rotl(r->s[3], 11);
    return res;
}

float cp_rng_f(CpRng *r)
{
    return (float)(cp_rng_u32(r) >> 8) * (1.0f / 16777216.0f);
}

float cp_rng_range(CpRng *r, float a, float b)
{
    return a + (b - a) * cp_rng_f(r);
}

int cp_rng_int(CpRng *r, int n)
{
    if (n <= 1) return 0;
    return (int)(cp_rng_u32(r) % (uint32_t)n);
}
