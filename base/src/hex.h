#ifndef HOLYC_HEX_H_
#define HOLYC_HEX_H_

/* Hex encode/decode - bytes <-> lowercase ASCII hex digit pairs (no
 * separators, no "0x" prefix, no uppercase option - the single most
 * common shape, matching e.g. how a hash digest or a MAC/IP-in-hex is
 * usually printed; a caller wanting `AA:BB:CC` or `0xAABBCC` formatting
 * builds it from these primitives rather than this file trying to
 * cover every display convention itself). */

#include "base.h"

/* out must have room for size*2 bytes - no NUL terminator written
 * (matches this project's other codec/base* encoders: the caller adds
 * one only if a C string is actually wanted). */
void hex_encode(const u8 *data, size_t size, char *out);

/* text must have an even length of valid hex digits (0-9, a-f, A-F -
 * both cases accepted on decode even though encode only ever produces
 * lowercase). Returns 1 on success (out filled with size/2 bytes), 0 on
 * a malformed input (odd length or a non-hex-digit byte) - out is left
 * untouched on failure, not partially filled. */
i32 hex_decode(const char *text, size_t text_size, u8 *out);

#endif /* HOLYC_HEX_H_ */

#ifdef HEX_IMPLEMENTATION
#ifndef HEX_IMPLEMENTATION_
#define HEX_IMPLEMENTATION_

static const char hex_encode_digits_[] = "0123456789abcdef";

void hex_encode(const u8 *data, size_t size, char *out)
{
    size_t i;
    for (i = 0; i < size; i++) {
        out[i * 2]     = hex_encode_digits_[(data[i] >> 4) & 0x0Fu];
        out[i * 2 + 1] = hex_encode_digits_[data[i] & 0x0Fu];
    }
}

static i32 hex_digit_value_(char c, u8 *out_val)
{
    if (c >= '0' && c <= '9') { *out_val = (u8)(c - '0'); return 1; }
    if (c >= 'a' && c <= 'f') { *out_val = (u8)(c - 'a' + 10); return 1; }
    if (c >= 'A' && c <= 'F') { *out_val = (u8)(c - 'A' + 10); return 1; }
    return 0;
}

i32 hex_decode(const char *text, size_t text_size, u8 *out)
{
    size_t i;
    u8 hi, lo;

    if (text_size % 2 != 0) return 0;

    /* validate every digit first, THEN decode - so a malformed byte
     * near the end of a long input can't leave `out` partially written
     * before the failure is reported (matches this file's own declared
     * contract: out is untouched on failure, not partially filled). */
    for (i = 0; i < text_size; i++) {
        if (!hex_digit_value_(text[i], &hi)) return 0;
    }

    for (i = 0; i < text_size / 2; i++) {
        hex_digit_value_(text[i * 2], &hi);
        hex_digit_value_(text[i * 2 + 1], &lo);
        out[i] = (u8)((hi << 4) | lo);
    }
    return 1;
}

#endif /* HEX_IMPLEMENTATION_ */
#endif /* HEX_IMPLEMENTATION */
