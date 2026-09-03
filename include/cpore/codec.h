/* Genome share codes - Spore's pollination, as strings.
 *
 * A genome is small POD; encoded as a short pasteable string it becomes a
 * thing you send a friend, paste into the game to invade your world with
 * their lineage, or hand to the lab as a designed starting point. No server:
 * the string IS the transport.
 *
 * Format: "CP1-" | "CP3-" | "CP4-" + base64url (no padding) of the canonical
 * field bytes + one checksum byte. Canonical order is fixed field-by-field,
 * never a raw struct dump, so a code made on one machine decodes on another
 * whatever the compiler's padding is. Decode clamps every field into range;
 * the caller still normalises into its generation budget before use. */
#ifndef CPORE_CODEC_H
#define CPORE_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include "cpore/cpore.h"
#include "cpore/aqua.h"
#include "cpore/land.h"

#ifdef __cplusplus
extern "C" {
#endif

/* canonical byte lengths (prefix and checksum excluded) */
#define CP_CODEC_CELL_LEN 24
#define CP_CODEC_AQUA_LEN 80
#define CP_CODEC_LAND_LEN 157

/* encoded string lengths incl. prefix, excl. NUL (b64 of len+1 checksum) */
#define CP_CODEC_CELL_STR 38
#define CP_CODEC_AQUA_STR 112
#define CP_CODEC_LAND_STR 215

/* encode into out (caller provides >= STR+1 bytes). Returns strlen, or 0. */
int cp_codec_cell(const CpGenome *g, char *out, size_t cap);
int cp_codec_aqua(const Cp3Genome *g, char *out, size_t cap);
int cp_codec_land(const Cp4Genome *g, char *out, size_t cap);

/* decode a share string into g (fields clamped). 0 ok, -1 bad prefix/format,
 * -2 bad checksum, -3 bad length. */
int cp_decode_cell(const char *s, CpGenome *g);
int cp_decode_aqua(const char *s, Cp3Genome *g);
int cp_decode_land(const char *s, Cp4Genome *g);

/* raw helpers, shared so every stage uses one alphabet */
int cp_b64enc(const uint8_t *in, size_t n, char *out, size_t cap);
int cp_b64dec(const char *s, uint8_t *out, size_t cap); /* returns bytes, or -1 */

#ifdef __cplusplus
}
#endif
#endif /* CPORE_CODEC_H */
