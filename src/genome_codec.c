/* Canonical genome codecs. See codec.h for the format rationale. */
#include "cpore/codec.h"
#include "cpore/cpore.h"
#include "cpore/aqua.h"
#include "cpore/land.h"
#include <string.h>

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int cp_b64enc(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    size_t i = 0;
    if (!in || !out || cap < 1) return 0;
    while (i < n) {
        uint32_t v = 0;
        size_t rem = n - i;
        size_t want = (rem >= 3) ? 4 : rem + 1;   /* unpadded tail: 2 or 3 */
        size_t k;
        if (o + want + 1 > cap) return 0;
        v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        for (k = 2; k < want; k++)
            out[o++] = B64[(v >> (18 - 6 * k)) & 63];
        i += 3;
    }
    if (o >= cap) return 0;
    out[o] = '\0';
    return (int)o;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

int cp_b64dec(const char *s, uint8_t *out, size_t cap)
{
    size_t n, o = 0, i = 0;
    if (!s || !out) return -1;
    n = strlen(s);
    while (i < n) {
        int a, b, c, d;
        uint32_t v;
        if (i + 2 > n) return -1;
        a = b64val(s[i]); b = b64val(s[i + 1]);
        if (a < 0 || b < 0) return -1;
        c = (i + 2 < n) ? b64val(s[i + 2]) : -1;
        d = (i + 3 < n) ? b64val(s[i + 3]) : -1;
        if (c < 0 && d >= 0) return -1;
        v = ((uint32_t)a << 18) | ((uint32_t)b << 12);
        if (o >= cap) return -1;
        out[o++] = (uint8_t)(v >> 16);
        if (c >= 0) {
            if (o >= cap) return -1;
            v |= (uint32_t)c << 6;
            out[o++] = (uint8_t)((v >> 8) & 0xFF);
        }
        if (d >= 0) {
            if (o >= cap) return -1;
            v |= (uint32_t)d;
            out[o++] = (uint8_t)(v & 0xFF);
        }
        i += (d >= 0) ? 4 : (c >= 0 ? 3 : 2);
    }
    return (int)o;
}

static uint8_t cksum(const uint8_t *b, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return (uint8_t)(h & 0xFF);
}

static int enc_wrap(const char *pre, const uint8_t *raw, size_t n,
                    char *out, size_t cap)
{
    uint8_t buf[CP_CODEC_LAND_LEN + 1];
    size_t need, rem;
    char *p;
    if (n + 1 > sizeof(buf)) return 0;
    /* unpadded base64: full groups give 4 chars, a 1-2 byte tail gives +2/+3 */
    rem = (n + 1) % 3;
    need = strlen(pre) + ((n + 1) / 3) * 4 + (rem ? (rem + 1) : 0) + 1;
    if (cap < need) return 0;
    memcpy(buf, raw, n);
    buf[n] = cksum(raw, n);
    strcpy(out, pre);
    p = out + strlen(pre);
    return (int)(strlen(pre) + (size_t)cp_b64enc(buf, n + 1, p, cap - strlen(pre)));
}

static int dec_wrap(const char *s, const char *pre, uint8_t *raw, size_t n)
{
    uint8_t buf[CP_CODEC_LAND_LEN + 1];
    int got;
    size_t len = strlen(pre);
    if (!s || strncmp(s, pre, len) != 0) return -1;
    got = cp_b64dec(s + len, buf, sizeof(buf));
    if (got != (int)(n + 1)) return -3;
    if (buf[n] != cksum(buf, n)) return -2;
    memcpy(raw, buf, n);
    return 0;
}

/* ---- cell: 12 x (type, angle) ---- */

int cp_codec_cell(const CpGenome *g, char *out, size_t cap)
{
    uint8_t raw[CP_CODEC_CELL_LEN];
    int i;
    if (!g || !out) return 0;
    for (i = 0; i < CP_MAX_PARTS; i++) {
        raw[i * 2] = g->part[i].type;
        raw[i * 2 + 1] = g->part[i].angle;
    }
    return enc_wrap("CP1-", raw, sizeof(raw), out, cap);
}

int cp_decode_cell(const char *s, CpGenome *g)
{
    uint8_t raw[CP_CODEC_CELL_LEN];
    int i, rc;
    if (!g) return -1;
    rc = dec_wrap(s, "CP1-", raw, sizeof(raw));
    if (rc) return rc;
    cp_genome_clear(g);
    for (i = 0; i < CP_MAX_PARTS; i++) {
        int t = raw[i * 2];
        if (t < 0) t = 0;
        if (t >= CP_PART_COUNT) t = CP_PART_COUNT - 1;
        g->part[i].type = (uint8_t)t;
        g->part[i].angle = raw[i * 2 + 1];
    }
    return 0;
}

/* ---- aqua: 10 x (type, seg, yaw, pitch, scale, mirror) + body ---- */

int cp_codec_aqua(const Cp3Genome *g, char *out, size_t cap)
{
    uint8_t raw[CP_CODEC_AQUA_LEN];
    int i;
    if (!g || !out) return 0;
    for (i = 0; i < CP3_MAX_PARTS; i++) {
        raw[i * 6 + 0] = g->part[i].type;
        raw[i * 6 + 1] = g->part[i].seg;
        raw[i * 6 + 2] = g->part[i].yaw;
        raw[i * 6 + 3] = (uint8_t)(g->part[i].pitch + 64);
        raw[i * 6 + 4] = g->part[i].scale;
        raw[i * 6 + 5] = g->part[i].mirror ? 1 : 0;
    }
    raw[60] = g->nseg; raw[61] = g->girth;
    raw[62] = g->prof[0]; raw[63] = g->prof[1];
    raw[64] = g->prof[2]; raw[65] = g->prof[3];
    raw[66] = (uint8_t)(g->arch + 128); raw[67] = (uint8_t)(g->sweep + 128);
    raw[68] = g->hue; raw[69] = g->hue2; raw[70] = g->sat; raw[71] = g->val;
    raw[72] = g->pattern; raw[73] = g->pscale;
    for (i = 0; i < CP3_MAX_SEG; i++) raw[74 + i] = (uint8_t)(g->lump[i] + 128);
    return enc_wrap("CP3-", raw, sizeof(raw), out, cap);
}

int cp_decode_aqua(const char *s, Cp3Genome *g)
{
    uint8_t raw[CP_CODEC_AQUA_LEN];
    int i, rc;
    if (!g) return -1;
    rc = dec_wrap(s, "CP3-", raw, sizeof(raw));
    if (rc) return rc;
    cp3_genome_clear(g);
    for (i = 0; i < CP3_MAX_PARTS; i++) {
        int t = raw[i * 6];
        if (t < 0) t = 0;
        if (t >= CP3_PART_COUNT) t = CP3_PART_COUNT - 1;
        g->part[i].type = (uint8_t)t;
        g->part[i].seg = (uint8_t)(raw[i * 6 + 1] >= CP3_MAX_SEG ? CP3_MAX_SEG - 1 : raw[i * 6 + 1]);
        g->part[i].yaw = raw[i * 6 + 2];
        {
            int p = (int)raw[i * 6 + 3] - 64;
            if (p < -64) p = -64;
            if (p > 63) p = 63;
            g->part[i].pitch = (int8_t)p;
        }
        g->part[i].scale = raw[i * 6 + 4];
        g->part[i].mirror = (uint8_t)(raw[i * 6 + 5] ? 1 : 0);
    }
    {
        int ns = raw[60];
        if (ns < 2) ns = 2;
        if (ns > CP3_MAX_SEG) ns = CP3_MAX_SEG;
        g->nseg = (uint8_t)ns;
    }
    g->girth = raw[61];
    for (i = 0; i < 4; i++) g->prof[i] = raw[62 + i];
    g->arch = (int8_t)(raw[66] - 128);
    g->sweep = (int8_t)(raw[67] - 128);
    g->hue = raw[68]; g->hue2 = raw[69]; g->sat = raw[70]; g->val = raw[71];
    g->pattern = (uint8_t)(raw[72] % CP3_PAT_COUNT); g->pscale = raw[73];
    for (i = 0; i < CP3_MAX_SEG; i++) g->lump[i] = (int8_t)(raw[74 + i] - 128);
    return 0;
}

/* ---- land: 16 x 8 fields + spine + coats ---- */

int cp_codec_land(const Cp4Genome *g, char *out, size_t cap)
{
    uint8_t raw[CP_CODEC_LAND_LEN];
    int i, o = 0;
    if (!g || !out) return 0;
    for (i = 0; i < CP4_MAX_PARTS; i++) {
        raw[o++] = g->part[i].type;
        raw[o++] = g->part[i].seg;
        raw[o++] = g->part[i].yaw;
        raw[o++] = (uint8_t)(g->part[i].pitch + 64);
        raw[o++] = g->part[i].scale;
        raw[o++] = g->part[i].mirror ? 1 : 0;
        raw[o++] = g->part[i].len;
        raw[o++] = (uint8_t)(g->part[i].bend + 128);
    }
    raw[o++] = g->nseg; raw[o++] = g->girth;
    for (i = 0; i < 4; i++) raw[o++] = g->prof[i];
    for (i = 0; i < CP4_MAX_SEG; i++) raw[o++] = (uint8_t)(g->lump[i] + 128);
    for (i = 0; i < CP4_MAX_SEG; i++) raw[o++] = (uint8_t)(g->rise[i] + 128);
    raw[o++] = (uint8_t)(g->arch + 128); raw[o++] = (uint8_t)(g->sweep + 128);
    raw[o++] = g->hue; raw[o++] = g->hue2; raw[o++] = g->hue3;
    raw[o++] = g->sat; raw[o++] = g->val;
    raw[o++] = g->pattern; raw[o++] = g->pscale;
    raw[o++] = g->pattern2; raw[o++] = g->pscale2;
    return enc_wrap("CP4-", raw, sizeof(raw), out, cap);
}

int cp_decode_land(const char *s, Cp4Genome *g)
{
    uint8_t raw[CP_CODEC_LAND_LEN];
    int i, o = 0, rc;
    if (!g) return -1;
    rc = dec_wrap(s, "CP4-", raw, sizeof(raw));
    if (rc) return rc;
    cp4_genome_clear(g);
    for (i = 0; i < CP4_MAX_PARTS; i++) {
        int t = raw[o];
        if (t < 0) t = 0;
        if (t >= CP4_PART_COUNT) t = CP4_PART_COUNT - 1;
        g->part[i].type = (uint8_t)t;
        g->part[i].seg = (uint8_t)(raw[o + 1] >= CP4_MAX_SEG ? CP4_MAX_SEG - 1 : raw[o + 1]);
        g->part[i].yaw = raw[o + 2];
        {
            int p = (int)raw[o + 3] - 64;
            if (p < -64) p = -64;
            if (p > 63) p = 63;
            g->part[i].pitch = (int8_t)p;
        }
        g->part[i].scale = raw[o + 4];
        g->part[i].mirror = (uint8_t)(raw[o + 5] ? 1 : 0);
        g->part[i].len = raw[o + 6];
        g->part[i].bend = (int8_t)(raw[o + 7] - 128);
        o += 8;
    }
    {
        int ns = raw[o];
        if (ns < 2) ns = 2;
        if (ns > CP4_MAX_SEG) ns = CP4_MAX_SEG;
        g->nseg = (uint8_t)ns;
    }
    g->girth = raw[o + 1]; o += 2;
    for (i = 0; i < 4; i++) g->prof[i] = raw[o++];
    for (i = 0; i < CP4_MAX_SEG; i++) g->lump[i] = (int8_t)(raw[o++] - 128);
    for (i = 0; i < CP4_MAX_SEG; i++) g->rise[i] = (int8_t)(raw[o++] - 128);
    g->arch = (int8_t)(raw[o++] - 128);
    g->sweep = (int8_t)(raw[o++] - 128);
    g->hue = raw[o++]; g->hue2 = raw[o++]; g->hue3 = raw[o++];
    g->sat = raw[o++]; g->val = raw[o++];
    g->pattern = (uint8_t)(raw[o++] % CP4_PAT_COUNT);
    g->pscale = raw[o++];
    g->pattern2 = (uint8_t)(raw[o++] % CP4_PAT_COUNT);
    g->pscale2 = raw[o++];
    return 0;
}
