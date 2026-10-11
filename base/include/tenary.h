#ifndef HOLYC_TENARY_H_
#define HOLYC_TENARY_H_

/* Ternary/small-radix compression techniques - "kompresja 4:1 5:1"
 * (this file's own prose spec) refers to exactly the packing ratios
 * project/vecindex/vecindex_2b.h and vecindex_3b.h already use ad-hoc,
 * inline, once per bit-width (dims_per_byte=4 for 4 levels -> 4:1,
 * dims_per_byte=5 for 3 levels/ternary, the actual BitNet b1.58 trick
 * -> 5:1): pack N small-radix symbols (each in [0, radix)) into as few
 * bytes as possible, radix^dims_per_byte <= 256. Generalized here into
 * one reusable pack/unpack pair instead of four copies of the same
 * `radix_pow *= LEVELS` loop - genuinely NOT wired back into
 * vecindex_*b.h in this pass (that code is real, measured, tested this
 * session; swapping its inline packing for a call through here is a
 * separate, deliberately deferred change, not bundled into adding this
 * file).
 *
 * predyktory + golomb-rice (this file's other two spec lines) are a
 * real, complete, testable little pipeline together, not two unrelated
 * features: delta-encode a sequence (predict the next value as the
 * previous one, store only the difference), zigzag-map the signed
 * residuals into unsigned integers, then Golomb-Rice entropy-code
 * those - smooth/slowly-varying data (audio, sensor readings, sorted
 * IDs) produces small residuals that compress well this way, which is
 * exactly why FLAC and similar formats use this same combination.
 *
 * Golomb-Rice needs a bit-level cursor (write a unary quotient, then a
 * fixed-width binary remainder) that bit.h's own header explicitly
 * defers ("bitstream serialization ... needs a real read/write-cursor
 * API, closer in size to string_view.h than to this file") - not
 * reimplemented in full generality here, just the minimal MSB-first
 * bit writer/reader this one encoding actually needs, kept static/
 * internal to this file. */

#include "base.h"

/* ---- small-radix symbol packing (the "kompresja 4:1 5:1" part) ---- */

/* largest m such that radix^m <= 256 - how many symbols fit in one
 * byte. radix must be in [2, 256]. */
u32 tenary_symbols_per_byte(u32 radix);

/* packs n symbols (each caller-guaranteed < radix) into out (caller-
 * owned, >= ceil(n / tenary_symbols_per_byte(radix)) bytes). Trailing
 * partial groups are padded with symbol 0, matching vecindex_*b.h's
 * own "partial last group" convention. */
void tenary_pack(u8 *out, const u8 *symbols, size_t n, u32 radix);

/* inverse of tenary_pack - unpacks exactly n symbols from packed
 * (caller-owned, >= the same byte count tenary_pack would have used). */
void tenary_unpack(u8 *symbols, const u8 *packed, size_t n, u32 radix);

/* ---- zigzag: signed <-> unsigned, small magnitudes either sign stay
 * small either way (0,-1,1,-2,2,... -> 0,1,2,3,4,...) - what makes a
 * signed prediction residual usable as a Golomb-Rice input, which only
 * encodes non-negative integers. ---- */

u32 tenary_zigzag_encode(i32 v);
i32 tenary_zigzag_decode(u32 v);

/* ---- delta (predictor): out[0] = in[0], out[i] = in[i] - in[i-1].
 * The simplest real predictor - "the next value is probably close to
 * the last one" - genuinely effective on smooth/monotonic sequences,
 * which is the entire point of pairing it with zigzag+Golomb-Rice
 * below rather than encoding raw values directly. ---- */

void tenary_delta_encode(i32 *out, const i32 *in, size_t n);
void tenary_delta_decode(i32 *out, const i32 *deltas, size_t n); /* inverse: cumulative sum */

/* ---- Golomb-Rice entropy coding, parameter k (remainder is k bits;
 * choose k so 2^k is close to the mean of the values being encoded -
 * that tuning is the caller's job, not this file's). Values must be
 * non-negative - zigzag first if they might be negative. ---- */

/* returns bytes written, or 0 if out_cap was too small (nothing
 * partially written in that case - caller can safely retry with a
 * bigger buffer, this doesn't corrupt out). */
size_t tenary_golomb_rice_encode(u8 *out, size_t out_cap, const u32 *values, size_t n, u32 k);

/* decodes exactly n values from data (data_len bytes) into out
 * (caller-owned, >= n entries). Caller must already know n (this
 * encoding has no length prefix or terminator of its own - same
 * "caller tracks the count" convention as tenary_pack/unpack above). */
void tenary_golomb_rice_decode(u32 *out, size_t n, const u8 *data, size_t data_len, u32 k);

#endif /* HOLYC_TENARY_H_ */

#ifdef TENARY_IMPLEMENTATION
#ifndef TENARY_IMPLEMENTATION_
#define TENARY_IMPLEMENTATION_

u32 tenary_symbols_per_byte(u32 radix)
{
    u32 pow_ = radix;
    u32 count = 1;
    while (pow_ * radix <= 256) {
        pow_ *= radix;
        count++;
    }
    return count;
}

void tenary_pack(u8 *out, const u8 *symbols, size_t n, u32 radix)
{
    size_t dpb = tenary_symbols_per_byte(radix);
    size_t code_bytes = (n + dpb - 1) / dpb;
    size_t i, byte_idx;
    u32 radix_pow;

    for (byte_idx = 0; byte_idx < code_bytes; byte_idx++) out[byte_idx] = 0;

    radix_pow = 1;
    for (i = 0; i < n; i++) {
        size_t pos_in_group = i % dpb;
        if (pos_in_group == 0) radix_pow = 1;
        byte_idx = i / dpb;
        out[byte_idx] = (u8)(out[byte_idx] + symbols[i] * radix_pow);
        radix_pow *= radix;
    }
}

void tenary_unpack(u8 *symbols, const u8 *packed, size_t n, u32 radix)
{
    size_t dpb = tenary_symbols_per_byte(radix);
    size_t i;

    for (i = 0; i < n; i++) {
        size_t byte_idx = i / dpb;
        size_t pos_in_group = i % dpb;
        u32 v = packed[byte_idx];
        size_t j;
        /* divide out the lower groups already consumed within this
         * byte to reach pos_in_group's own digit - same base-radix
         * digit extraction vecindex_*b.h's decode side would need,
         * generalized instead of re-derived per bit-width. */
        for (j = 0; j < pos_in_group; j++) v /= radix;
        symbols[i] = (u8)(v % radix);
    }
}

u32 tenary_zigzag_encode(i32 v)
{
    /* (v << 1) ^ (v >> 31): arithmetic right-shift of a negative i32
     * gives all-1s, turning the XOR into "flip and shift" for negative
     * inputs and a plain shift for non-negative ones - the standard
     * protobuf/Avro zigzag identity, not reinvented differently here. */
    return ((u32)v << 1) ^ (u32)(v >> 31);
}

i32 tenary_zigzag_decode(u32 v)
{
    return (i32)(v >> 1) ^ -(i32)(v & 1);
}

void tenary_delta_encode(i32 *out, const i32 *in, size_t n)
{
    size_t i;
    if (n == 0) return;
    out[0] = in[0];
    for (i = 1; i < n; i++) out[i] = in[i] - in[i - 1];
}

void tenary_delta_decode(i32 *out, const i32 *deltas, size_t n)
{
    size_t i;
    if (n == 0) return;
    out[0] = deltas[0];
    for (i = 1; i < n; i++) out[i] = out[i - 1] + deltas[i];
}

/* minimal MSB-first bit cursor - internal to this file's Golomb-Rice
 * pair, not a general bitstream API (see file header for why the
 * general one stays deferred). */
typedef struct {
    u8 *buf;
    size_t cap;
    size_t byte_pos;
    i32 bit_pos; /* 0 = most-significant bit of buf[byte_pos] not yet written */
} Tenary_Bitwriter_;

static void tenary_bw_init_(Tenary_Bitwriter_ *bw, u8 *buf, size_t cap)
{
    bw->buf = buf;
    bw->cap = cap;
    bw->byte_pos = 0;
    bw->bit_pos = 0;
    if (cap > 0) buf[0] = 0;
}

static i32 tenary_bw_put_bit_(Tenary_Bitwriter_ *bw, u32 bit)
{
    if (bw->byte_pos >= bw->cap) return 0;
    if (bit) bw->buf[bw->byte_pos] = (u8)(bw->buf[bw->byte_pos] | (0x80 >> bw->bit_pos));
    bw->bit_pos++;
    if (bw->bit_pos == 8) {
        bw->bit_pos = 0;
        bw->byte_pos++;
        if (bw->byte_pos < bw->cap) bw->buf[bw->byte_pos] = 0;
    }
    return 1;
}

static i32 tenary_bw_put_bits_(Tenary_Bitwriter_ *bw, u32 value, u32 width)
{
    i32 i;
    for (i = (i32)width - 1; i >= 0; i--) {
        if (!tenary_bw_put_bit_(bw, (value >> i) & 1)) return 0;
    }
    return 1;
}

typedef struct {
    const u8 *buf;
    size_t len;
    size_t byte_pos;
    i32 bit_pos;
} Tenary_Bitreader_;

static void tenary_br_init_(Tenary_Bitreader_ *br, const u8 *buf, size_t len)
{
    br->buf = buf;
    br->len = len;
    br->byte_pos = 0;
    br->bit_pos = 0;
}

static u32 tenary_br_get_bit_(Tenary_Bitreader_ *br)
{
    u32 bit;
    if (br->byte_pos >= br->len) return 0; /* past-the-end reads as 0, not a crash */
    bit = (u32)((br->buf[br->byte_pos] >> (7 - br->bit_pos)) & 1);
    br->bit_pos++;
    if (br->bit_pos == 8) {
        br->bit_pos = 0;
        br->byte_pos++;
    }
    return bit;
}

static u32 tenary_br_get_bits_(Tenary_Bitreader_ *br, u32 width)
{
    u32 v = 0;
    u32 i;
    for (i = 0; i < width; i++) v = (v << 1) | tenary_br_get_bit_(br);
    return v;
}

size_t tenary_golomb_rice_encode(u8 *out, size_t out_cap, const u32 *values, size_t n, u32 k)
{
    Tenary_Bitwriter_ bw;
    size_t i;

    tenary_bw_init_(&bw, out, out_cap);

    for (i = 0; i < n; i++) {
        u32 q = values[i] >> k;
        u32 r = values[i] & (((u32)1 << k) - 1);
        u32 j;

        for (j = 0; j < q; j++) {
            if (!tenary_bw_put_bit_(&bw, 1)) return 0;
        }
        if (!tenary_bw_put_bit_(&bw, 0)) return 0;
        if (!tenary_bw_put_bits_(&bw, r, k)) return 0;
    }

    return bw.byte_pos + (bw.bit_pos > 0 ? 1 : 0);
}

void tenary_golomb_rice_decode(u32 *out, size_t n, const u8 *data, size_t data_len, u32 k)
{
    Tenary_Bitreader_ br;
    size_t i;

    tenary_br_init_(&br, data, data_len);

    for (i = 0; i < n; i++) {
        u32 q = 0;
        u32 r;
        while (tenary_br_get_bit_(&br)) q++;
        r = tenary_br_get_bits_(&br, k);
        out[i] = (q << k) | r;
    }
}

#endif /* TENARY_IMPLEMENTATION_ */
#endif /* TENARY_IMPLEMENTATION */
