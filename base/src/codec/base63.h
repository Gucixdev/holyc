#ifndef HOLYC_BASE63_H_
#define HOLYC_BASE63_H_

/* "base63" is not an RFC/standard format - there is nothing to port or
 * cite, so the alphabet is a deliberate choice made here, documented
 * since nobody could otherwise guess it: 10 digits + 26 uppercase + 26
 * lowercase (62, the same set base64/base58 draw from) plus ONE more
 * symbol ('-') to reach 63. The real-world motivation for a base with
 * one MORE symbol than base64 rather than one fewer (like base58) is
 * the opposite of base58's problem: base58 drops visually-confusable
 * characters (0/O, I/l) to make hand-transcription safer; base63
 * instead uses every alphanumeric character AND is odd specifically so
 * '-' can serve as a single, unambiguous separator/terminator byte in
 * contexts (URLs, filenames, shell args) where base64's '+' and '/'
 * need escaping - basically base64url's alphabet with the padding
 * character removed and folded into the data alphabet instead of kept
 * as a separate rule.
 *
 * NOT a bit-grouping scheme like base64 (64 = 2^6, so 3 bytes <-> 4
 * base64 digits divides evenly). 63 is not a power of 2, so there is no
 * fixed-width digit grouping at all: this is arbitrary-precision radix
 * conversion, the same "treat the whole string as one big number, do
 * long division/multiplication by hand" technique base58 decoders use
 * (e.g. Bitcoin addresses) - decode is repeated multiply-by-63-add-digit
 * over a big-endian byte buffer that grows one byte at a time as the
 * value overflows what's already allocated; encode is the same
 * technique run in reverse (repeated divide-by-63, digits collected
 * least-significant first then reversed).
 *
 * Encode and decode live in this one file (was base63_decode.h/
 * base63_encode.h, merged back) - each
 * still stands alone under its OWN IMPLEMENTATION guard macro
 * (BASE63_DECODE_IMPLEMENTATION/BASE63_ENCODE_IMPLEMENTATION, unchanged
 * from the split files), and each keeps its own distinctly-named copy
 * of the shared alphabet (base63_decode_alphabet_/base63_encode_alphabet_
 * - NOT a cross-file static reference) so either side still compiles
 * standalone under only its own IMPLEMENTATION macro, and both macros
 * can be defined together in one translation unit without a
 * duplicate-symbol error. */

#include "core/base.h"
#include "core/arena_alloc.h"

typedef enum {
    BASE63_ESUCCESS  = 0,
    BASE63_EBADCHAR  = -1 /* a byte in the input isn't in the base63 alphabet */
} Base63_Error;

/* '0' (the alphabet's zero-value symbol) leading the input is treated
 * the same way base58 treats leading '1's: each one contributes exactly
 * one leading zero byte to the output, since a leading zero digit is
 * otherwise invisible to pure positional-value bignum math (0 * any
 * base^k is still 0, so it wouldn't otherwise widen the output at all -
 * this is the standard fix). out_size is set to the decoded byte count. */
i32 base63_decode(Arena *a, const char *text, size_t text_size, u8 **out_data, size_t *out_size);

/* each leading zero BYTE of the input becomes exactly one leading '0'
 * CHARACTER of the output (mirrors how base63_decode treats leading '0'
 * characters - the standard fix for positional bignum math otherwise
 * making a leading zero digit invisible). out_size is set to the
 * encoded character count (no NUL terminator included). */
char *base63_encode(Arena *a, const u8 *data, size_t size, size_t *out_size);

const char *base63_strerror(i32 err);

#endif /* HOLYC_BASE63_H_ */

#ifdef BASE63_DECODE_IMPLEMENTATION
#ifndef BASE63_DECODE_IMPLEMENTATION_
#define BASE63_DECODE_IMPLEMENTATION_

static const char base63_decode_alphabet_[] =
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "-";

const char *base63_strerror(i32 err)
{
    switch (err) {
    case BASE63_ESUCCESS: return "success";
    case BASE63_EBADCHAR: return "character not in the base63 alphabet";
    default: break;
    }
    return "unknown error";
}

static i32 base63_digit_value_(char c, u32 *out_val)
{
    size_t i;
    for (i = 0; i < 63; i++) {
        if (base63_decode_alphabet_[i] == c) {
            *out_val = (u32)i;
            return 1;
        }
    }
    return 0;
}

i32 base63_decode(Arena *a, const char *text, size_t text_size, u8 **out_data, size_t *out_size)
{
    /* big-endian bignum, built up in a fixed-size stack scratch buffer
     * (worst case: base63 packs at least ~5.98 bits/digit, so the
     * decoded byte count never exceeds the input digit count - one byte
     * of scratch per input digit is always enough headroom). */
    u8 scratch[4096];
    size_t used;
    size_t leading_zeros;
    size_t i;
    u8 *out;

    if (text_size > sizeof(scratch)) return BASE63_EBADCHAR; /* not a real error case in practice, just the scratch limit */

    leading_zeros = 0;
    while (leading_zeros < text_size && text[leading_zeros] == '0') leading_zeros++;

    used = 0; /* number of significant bytes currently held in scratch, right-aligned */
    for (i = leading_zeros; i < text_size; i++) {
        u32 digit;
        u32 carry;
        size_t j;

        if (!base63_digit_value_(text[i], &digit)) return BASE63_EBADCHAR;

        /* scratch[sizeof(scratch)-used .. sizeof(scratch)-1] holds the
         * current value, most-significant byte first; multiply by 63,
         * add digit, propagating carry toward the front, growing `used`
         * by one byte whenever a carry survives past the current width -
         * exactly long multiplication by hand. */
        carry = digit;
        for (j = 0; j < used; j++) {
            size_t idx = sizeof(scratch) - 1 - j;
            u32 v = (u32)scratch[idx] * 63u + carry;
            scratch[idx] = (u8)(v & 0xFFu);
            carry = v >> 8;
        }
        while (carry > 0) {
            used++;
            scratch[sizeof(scratch) - used] = (u8)(carry & 0xFFu);
            carry >>= 8;
        }
    }

    out = (u8 *)arena_alloc(a, leading_zeros + used);
    for (i = 0; i < leading_zeros; i++) out[i] = 0;
    for (i = 0; i < used; i++) out[leading_zeros + i] = scratch[sizeof(scratch) - used + i];

    *out_data = out;
    *out_size = leading_zeros + used;
    return BASE63_ESUCCESS;
}

#endif /* BASE63_DECODE_IMPLEMENTATION_ */
#endif /* BASE63_DECODE_IMPLEMENTATION */

#ifdef BASE63_ENCODE_IMPLEMENTATION
#ifndef BASE63_ENCODE_IMPLEMENTATION_
#define BASE63_ENCODE_IMPLEMENTATION_

/* own local copy of the decoder's alphabet, under its own name
 * (base63_encode_alphabet_, not base63_decode_alphabet_) - deliberately
 * NOT shared via cross-file static reference: the decoder's copy only
 * exists when BASE63_DECODE_IMPLEMENTATION is defined, and this block
 * must stand alone under BASE63_ENCODE_IMPLEMENTATION only (same
 * independence wav.h keeps between its own encode/decode internals). A
 * distinct name also avoids a duplicate-symbol error when a caller
 * defines both IMPLEMENTATION macros in the same translation unit. Both
 * copies must stay identical - it's the format's alphabet, not an
 * implementation choice either side could vary. */
static const char base63_encode_alphabet_[] =
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "-";

char *base63_encode(Arena *a, const u8 *data, size_t size, size_t *out_size)
{
    /* big-endian working copy of the input, long-divided by 63 in place
     * repeatedly; a byte can never encode to MORE digits than its own
     * bit width divided by log2(63) (~5.98), so size itself is already
     * a safe upper bound on how many digits the non-zero part produces -
     * this scratch just needs to hold the input itself to divide it. */
    u8 work[4096];
    char digits[4096]; /* collected least-significant-first, reversed after */
    size_t leading_zeros, work_len, n_digits, i;
    char *out;
    size_t out_len;

    if (size > sizeof(work)) return NULL; /* not a real error case in practice, just the scratch limit */

    leading_zeros = 0;
    while (leading_zeros < size && data[leading_zeros] == 0) leading_zeros++;

    memcpy(work, data + leading_zeros, size - leading_zeros);
    work_len = size - leading_zeros;

    n_digits = 0;
    while (work_len > 0) {
        u32 remainder = 0;
        size_t i2;
        size_t new_len = 0;
        i32 leading = 1;

        /* one long-division pass: divide the big-endian number in
         * work[0..work_len) by 63, writing the quotient back in place
         * (shrinking leading zero bytes as they appear) and keeping the
         * final remainder as this pass's digit. */
        for (i2 = 0; i2 < work_len; i2++) {
            u32 v = (remainder << 8) | (u32)work[i2];
            u8 q = (u8)(v / 63u);
            remainder = v % 63u;
            if (leading && q == 0) continue; /* skip leading zero bytes of the quotient */
            leading = 0;
            work[new_len++] = q;
        }
        work_len = new_len;

        digits[n_digits++] = base63_encode_alphabet_[remainder];
    }

    out_len = leading_zeros + n_digits;
    out = (char *)arena_alloc(a, out_len);

    for (i = 0; i < leading_zeros; i++) out[i] = base63_encode_alphabet_[0]; /* '0' */
    for (i = 0; i < n_digits; i++) out[leading_zeros + i] = digits[n_digits - 1 - i]; /* reverse */

    *out_size = out_len;
    return out;
}

#endif /* BASE63_ENCODE_IMPLEMENTATION_ */
#endif /* BASE63_ENCODE_IMPLEMENTATION */
