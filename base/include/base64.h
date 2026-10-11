#ifndef HOLYC_BASE64_H_
#define HOLYC_BASE64_H_

/* Standard base64 (RFC 4648 section 4 - the "A-Za-z0-9+/" alphabet, "="
 * padding). Encode and decode live in this one file (was
 * base64_decode.h/base64_encode.h, merged back) - each still stands alone under its OWN IMPLEMENTATION
 * guard macro (BASE64_DECODE_IMPLEMENTATION/BASE64_ENCODE_IMPLEMENTATION,
 * unchanged from the split files): decode uses a lookup TABLE
 * (base64_decode_table_), encode uses the plain alphabet STRING
 * (base64_encode_alphabet_) - different shapes for different directions,
 * so unlike base63.h/qoi_codec.h there's no shared-name collision to
 * dodge between the two IMPLEMENTATION blocks here. */

#include "base.h"
#include "arena_alloc.h"

typedef enum {
    BASE64_ESUCCESS  = 0,
    BASE64_EBADCHAR  = -1, /* a byte in the input isn't in the alphabet or padding */
    BASE64_EBADLEN   = -2  /* input length (ignoring nothing - '=' counts) isn't a multiple of 4 */
} Base64_Error;

i32 base64_decode(Arena *a, const char *data, size_t size, u8 **out_data, size_t *out_size);

/* out_size is set to the encoded length (always 4*ceil(in_size/3), no
 * trailing NUL included - caller adds one if a C string is wanted). */
char *base64_encode(Arena *a, const u8 *data, size_t in_size, size_t *out_size);

const char *base64_strerror(i32 err);

#endif /* HOLYC_BASE64_H_ */

#ifdef BASE64_DECODE_IMPLEMENTATION
#ifndef BASE64_DECODE_IMPLEMENTATION_
#define BASE64_DECODE_IMPLEMENTATION_

/* -1 marks a byte that isn't in the alphabet; -2 marks '=' specifically
 * (distinguished from a hard error so padding can only appear where the
 * format actually allows it: the last 1-2 symbols of the last group). */
static i32 base64_decode_table_(u8 c)
{
    if (c >= 'A' && c <= 'Z') return (i32)(c - 'A');
    if (c >= 'a' && c <= 'z') return (i32)(c - 'a') + 26;
    if (c >= '0' && c <= '9') return (i32)(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
}

const char *base64_strerror(i32 err)
{
    switch (err) {
    case BASE64_ESUCCESS: return "success";
    case BASE64_EBADCHAR: return "character not in the base64 alphabet";
    case BASE64_EBADLEN:  return "input length is not a multiple of 4";
    default: break;
    }
    return "unknown error";
}

i32 base64_decode(Arena *a, const char *data, size_t size, u8 **out_data, size_t *out_size)
{
    u8 *out;
    size_t group, n_groups, o;

    if (size == 0) {
        *out_data = NULL;
        *out_size = 0;
        return BASE64_ESUCCESS;
    }

    if (size % 4 != 0) return BASE64_EBADLEN;

    n_groups = size / 4;
    out = (u8 *)arena_alloc(a, n_groups * 3);
    o = 0;

    for (group = 0; group < n_groups; group++) {
        i32 v[4];
        size_t k;
        size_t base = group * 4;
        i32 is_last = (group == n_groups - 1);

        for (k = 0; k < 4; k++) {
            v[k] = base64_decode_table_((u8)data[base + k]);
            if (v[k] == -1) return BASE64_EBADCHAR;
            /* '=' is only valid in the trailing 1-2 positions of the LAST group */
            if (v[k] == -2 && !(is_last && k >= 2)) return BASE64_EBADCHAR;
        }

        out[o++] = (u8)(((u32)v[0] << 2) | (((u32)v[1] >> 4) & 0x03));
        if (v[2] != -2) out[o++] = (u8)((((u32)v[1] & 0x0F) << 4) | (((u32)v[2] >> 2) & 0x0F));
        if (v[3] != -2) out[o++] = (u8)((((u32)v[2] & 0x03) << 6) | (u32)v[3]);
    }

    *out_data = out;
    *out_size = o;
    return BASE64_ESUCCESS;
}

#endif /* BASE64_DECODE_IMPLEMENTATION_ */
#endif /* BASE64_DECODE_IMPLEMENTATION */

#ifdef BASE64_ENCODE_IMPLEMENTATION
#ifndef BASE64_ENCODE_IMPLEMENTATION_
#define BASE64_ENCODE_IMPLEMENTATION_

static const char base64_encode_alphabet_[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

char *base64_encode(Arena *a, const u8 *data, size_t in_size, size_t *out_size)
{
    size_t out_len = 4 * ((in_size + 2) / 3);
    char *out = (char *)arena_alloc(a, out_len);
    size_t i, o;

    o = 0;
    for (i = 0; i + 3 <= in_size; i += 3) {
        u32 n = ((u32)data[i] << 16) | ((u32)data[i + 1] << 8) | (u32)data[i + 2];
        out[o++] = base64_encode_alphabet_[(n >> 18) & 0x3F];
        out[o++] = base64_encode_alphabet_[(n >> 12) & 0x3F];
        out[o++] = base64_encode_alphabet_[(n >> 6) & 0x3F];
        out[o++] = base64_encode_alphabet_[n & 0x3F];
    }

    if (in_size - i == 1) {
        u32 n = (u32)data[i] << 16;
        out[o++] = base64_encode_alphabet_[(n >> 18) & 0x3F];
        out[o++] = base64_encode_alphabet_[(n >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (in_size - i == 2) {
        u32 n = ((u32)data[i] << 16) | ((u32)data[i + 1] << 8);
        out[o++] = base64_encode_alphabet_[(n >> 18) & 0x3F];
        out[o++] = base64_encode_alphabet_[(n >> 12) & 0x3F];
        out[o++] = base64_encode_alphabet_[(n >> 6) & 0x3F];
        out[o++] = '=';
    }

    *out_size = out_len;
    return out;
}

#endif /* BASE64_ENCODE_IMPLEMENTATION_ */
#endif /* BASE64_ENCODE_IMPLEMENTATION */
