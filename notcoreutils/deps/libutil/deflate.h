#ifndef HOLYC_DEFLATE_H_
#define HOLYC_DEFLATE_H_

/* DEFLATE (RFC 1951) / zlib (RFC 1950) decompressor, ported from
 * stb_image.h's embedded zlib decoder (Sean Barrett, MIT/public domain
 * dual license). General-purpose, not PNG-specific despite PNG being the
 * first consumer (png.h includes this) - deliberately its own file rather
 * than folded into png.h: this is a shared LOW-LEVEL algorithm (like
 * arena_alloc.h or base_type.h being depended on everywhere), not the
 * kind of shared HIGH-LEVEL abstraction this project avoids (see how
 * vecindex_1b.h/2b.h/3b.h/4b.h stayed four independent files instead of
 * one generic one - that call was about not hiding genuinely different
 * packing schemes behind one runtime-generic layer, not a rule against
 * any shared code at all).
 *
 * Algorithm unchanged from the original: canonical Huffman decoding with
 * a fast direct-lookup table for short codes (STBI__ZFAST_BITS-equivalent,
 * kept as DEFLATE_FAST_BITS) and a slow bit-reversal path for the rest,
 * dynamic/fixed/stored block handling per RFC 1951. What changed: malloc/
 * realloc -> Arena (arena_realloc's doubling growth replaces stb's own
 * manual expand-on-demand, same idea), int/char/unsigned -> i32/u8/u32,
 * C++-style line comments converted to C89 block comments, no rich error
 * strings (this project's other parsers - ini.h, tar.h - all just return
 * 0/1, matched here instead of carrying over stb's own error-message
 * plumbing). */

#include "base.h"
#include "arena_alloc.h"

/* src need not be a complete zlib stream in one buffer up front in the
 * general case, but this port - like the original when driven from PNG -
 * requires the whole compressed stream available at once (PNG's own IDAT
 * chunks get concatenated into one buffer by png.h before calling this).
 * parse_zlib_header: 1 for a full zlib stream (2-byte header + Adler32
 * trailer, RFC 1950), 0 for a raw headerless DEFLATE stream (RFC 1951) -
 * PNG always uses the former. On success returns 1, sets *out_data
 * (arena-owned) and *out_len to the decompressed bytes. Returns 0 on any
 * malformed input. */
i32 deflate_decode(Arena *a, const u8 *src, size_t src_len, i32 parse_zlib_header,
                    u8 **out_data, size_t *out_len);

#endif /* HOLYC_DEFLATE_H_ */

#ifdef DEFLATE_IMPLEMENTATION
#ifndef DEFLATE_IMPLEMENTATION_
#define DEFLATE_IMPLEMENTATION_

#define DEFLATE_FAST_BITS 9
#define DEFLATE_FAST_MASK ((1 << DEFLATE_FAST_BITS) - 1)
#define DEFLATE_NSYMS 288

typedef struct {
    u16 fast[1 << DEFLATE_FAST_BITS];
    u16 firstcode[16];
    i32 maxcode[17];
    u16 firstsymbol[16];
    u8 size[DEFLATE_NSYMS];
    u16 value[DEFLATE_NSYMS];
} Deflate_Huffman_;

typedef struct {
    Arena *a;
    const u8 *in;
    const u8 *in_end;
    i32 num_bits;
    i32 hit_eof_once;
    u32 code_buffer;

    u8 *out;
    u8 *out_start;
    u8 *out_end;

    Deflate_Huffman_ z_length, z_distance;
} Deflate_State_;

static i32 deflate_bit_reverse16_(i32 n)
{
    n = ((n & 0xAAAA) >> 1) | ((n & 0x5555) << 1);
    n = ((n & 0xCCCC) >> 2) | ((n & 0x3333) << 2);
    n = ((n & 0xF0F0) >> 4) | ((n & 0x0F0F) << 4);
    n = ((n & 0xFF00) >> 8) | ((n & 0x00FF) << 8);
    return n;
}

static i32 deflate_bit_reverse_(i32 v, i32 bits)
{
    return deflate_bit_reverse16_(v) >> (16 - bits);
}

static i32 deflate_build_huffman_(Deflate_Huffman_ *z, const u8 *sizelist, i32 num)
{
    i32 i, k = 0;
    i32 code, next_code[16], sizes[17];

    memset(sizes, 0, sizeof(sizes));
    memset(z->fast, 0, sizeof(z->fast));
    for (i = 0; i < num; i++) sizes[sizelist[i]]++;
    sizes[0] = 0;
    for (i = 1; i < 16; i++)
        if (sizes[i] > (1 << i)) return 0;

    code = 0;
    for (i = 1; i < 16; i++) {
        next_code[i] = code;
        z->firstcode[i] = (u16)code;
        z->firstsymbol[i] = (u16)k;
        code = code + sizes[i];
        if (sizes[i] && code - 1 >= (1 << i)) return 0;
        z->maxcode[i] = code << (16 - i);
        code <<= 1;
        k += sizes[i];
    }
    z->maxcode[16] = 0x10000;

    for (i = 0; i < num; i++) {
        i32 s = sizelist[i];
        if (s) {
            i32 c = next_code[s] - z->firstcode[s] + z->firstsymbol[s];
            u16 fastv = (u16)((s << 9) | i);
            z->size[c] = (u8)s;
            z->value[c] = (u16)i;
            if (s <= DEFLATE_FAST_BITS) {
                i32 j = deflate_bit_reverse_(next_code[s], s);
                while (j < (1 << DEFLATE_FAST_BITS)) {
                    z->fast[j] = fastv;
                    j += (1 << s);
                }
            }
            next_code[s]++;
        }
    }
    return 1;
}

static i32 deflate_eof_(Deflate_State_ *z) { return z->in >= z->in_end; }
static u8 deflate_get8_(Deflate_State_ *z) { return deflate_eof_(z) ? 0 : *z->in++; }

static void deflate_fill_bits_(Deflate_State_ *z)
{
    do {
        if (z->code_buffer >= (1u << z->num_bits)) {
            z->in = z->in_end;
            return;
        }
        z->code_buffer |= (u32)deflate_get8_(z) << z->num_bits;
        z->num_bits += 8;
    } while (z->num_bits <= 24);
}

static u32 deflate_receive_(Deflate_State_ *z, i32 n)
{
    u32 k;
    if (z->num_bits < n) deflate_fill_bits_(z);
    k = z->code_buffer & ((1u << n) - 1);
    z->code_buffer >>= n;
    z->num_bits -= n;
    return k;
}

static i32 deflate_huffman_decode_slowpath_(Deflate_State_ *a, Deflate_Huffman_ *z)
{
    i32 b, s, k;
    k = deflate_bit_reverse_((i32)a->code_buffer, 16);
    for (s = DEFLATE_FAST_BITS + 1; ; s++)
        if (k < z->maxcode[s]) break;
    if (s >= 16) return -1;
    b = (k >> (16 - s)) - z->firstcode[s] + z->firstsymbol[s];
    if (b >= DEFLATE_NSYMS) return -1;
    if (z->size[b] != s) return -1;
    a->code_buffer >>= s;
    a->num_bits -= s;
    return z->value[b];
}

static i32 deflate_huffman_decode_(Deflate_State_ *a, Deflate_Huffman_ *z)
{
    i32 b, s;
    if (a->num_bits < 16) {
        if (deflate_eof_(a)) {
            if (!a->hit_eof_once) {
                a->hit_eof_once = 1;
                a->num_bits += 16; /* implicit zero padding so speculative decode can proceed; consuming any of it is caught below as malformed */
            } else {
                return -1;
            }
        } else {
            deflate_fill_bits_(a);
        }
    }
    b = z->fast[a->code_buffer & DEFLATE_FAST_MASK];
    if (b) {
        s = b >> 9;
        a->code_buffer >>= s;
        a->num_bits -= s;
        return b & 511;
    }
    return deflate_huffman_decode_slowpath_(a, z);
}

static i32 deflate_expand_(Deflate_State_ *z, u8 *cur_out, size_t n)
{
    size_t cur, limit, new_limit;
    u8 *q;
    cur = (size_t)(cur_out - z->out_start);
    limit = (size_t)(z->out_end - z->out_start);
    new_limit = limit;
    if (new_limit == 0) new_limit = 128;
    while (cur + n > new_limit) new_limit *= 2;
    q = (u8 *)arena_realloc(z->a, z->out_start, limit, new_limit);
    z->out_start = q;
    z->out = q + cur;
    z->out_end = q + new_limit;
    return 1;
}

static const i32 deflate_length_base_[31] = {
    3,4,5,6,7,8,9,10,11,13, 15,17,19,23,27,31,35,43,51,59,
    67,83,99,115,131,163,195,227,258,0,0
};
static const i32 deflate_length_extra_[31] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0,0,0
};
static const i32 deflate_dist_base_[32] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577,0,0
};
static const i32 deflate_dist_extra_[32] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static i32 deflate_parse_huffman_block_(Deflate_State_ *a)
{
    u8 *out = a->out;
    for (;;) {
        i32 z = deflate_huffman_decode_(a, &a->z_length);
        if (z < 256) {
            if (z < 0) return 0;
            if (out >= a->out_end) {
                if (!deflate_expand_(a, out, 1)) return 0;
                out = a->out;
            }
            *out++ = (u8)z;
        } else {
            const u8 *p;
            i32 len, dist;
            if (z == 256) {
                a->out = out;
                if (a->hit_eof_once && a->num_bits < 16) return 0;
                return 1;
            }
            if (z >= 286) return 0;
            z -= 257;
            len = deflate_length_base_[z];
            /* deflate_receive_(a, 0) is a provable no-op (masks with
             * (1<<0)-1==0, shifts code_buffer/num_bits by 0) - safe to
             * call unconditionally instead of guarding on
             * deflate_length_extra_[z]/deflate_dist_extra_[z] being
             * nonzero, since a zero-extra-bits symbol then adds exactly
             * 0, matching the old guarded behavior exactly. */
            len += (i32)deflate_receive_(a, deflate_length_extra_[z]);
            z = deflate_huffman_decode_(a, &a->z_distance);
            if (z < 0 || z >= 30) return 0;
            dist = deflate_dist_base_[z];
            dist += (i32)deflate_receive_(a, deflate_dist_extra_[z]);
            if ((size_t)(out - a->out_start) < (size_t)dist) return 0;
            if ((size_t)len > (size_t)(a->out_end - out)) {
                if (!deflate_expand_(a, out, (size_t)len)) return 0;
                out = a->out;
            }
            p = out - dist;
            if (dist == 1) {
                u8 v = *p;
                while (len--) *out++ = v;
            } else {
                while (len--) *out++ = *p++;
            }
        }
    }
}

static i32 deflate_compute_huffman_codes_(Deflate_State_ *a)
{
    static const u8 length_dezigzag[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    Deflate_Huffman_ z_codelength;
    u8 lencodes[286 + 32 + 137];
    u8 codelength_sizes[19];
    i32 i, n, hlit, hdist, hclen, ntot;

    hlit = (i32)deflate_receive_(a, 5) + 257;
    hdist = (i32)deflate_receive_(a, 5) + 1;
    hclen = (i32)deflate_receive_(a, 4) + 4;
    ntot = hlit + hdist;

    memset(codelength_sizes, 0, sizeof(codelength_sizes));
    for (i = 0; i < hclen; i++) {
        i32 s = (i32)deflate_receive_(a, 3);
        codelength_sizes[length_dezigzag[i]] = (u8)s;
    }
    if (!deflate_build_huffman_(&z_codelength, codelength_sizes, 19)) return 0;

    n = 0;
    while (n < ntot) {
        i32 c = deflate_huffman_decode_(a, &z_codelength);
        if (c < 0 || c >= 19) return 0;
        if (c < 16) {
            lencodes[n++] = (u8)c;
        } else {
            u8 fill = 0;
            switch (c) {
            case 16:
                if (n == 0) return 0;
                fill = lencodes[n - 1];
                c = (i32)deflate_receive_(a, 2) + 3;
                break;
            case 17:
                c = (i32)deflate_receive_(a, 3) + 3;
                break;
            case 18:
                c = (i32)deflate_receive_(a, 7) + 11;
                break;
            default:
                return 0;
            }
            if (ntot - n < c) return 0;
            memset(lencodes + n, fill, (size_t)c);
            n += c;
        }
    }
    if (n != ntot) return 0;
    if (!deflate_build_huffman_(&a->z_length, lencodes, hlit)) return 0;
    if (!deflate_build_huffman_(&a->z_distance, lencodes + hlit, hdist)) return 0;
    return 1;
}

static i32 deflate_parse_uncompressed_block_(Deflate_State_ *a)
{
    u8 header[4];
    i32 len, nlen, k;

    /* deflate_receive_(a, 0) is a provable no-op (see the length/dist
     * extra-bits comment in deflate_parse_huffman_block_) - safe to
     * call unconditionally instead of guarding on num_bits already
     * being byte-aligned. */
    deflate_receive_(a, a->num_bits & 7);
    k = 0;
    while (a->num_bits > 0) {
        header[k++] = (u8)(a->code_buffer & 255);
        a->code_buffer >>= 8;
        a->num_bits -= 8;
    }
    if (a->num_bits < 0) return 0;
    while (k < 4) header[k++] = deflate_get8_(a);

    len = header[1] * 256 + header[0];
    nlen = header[3] * 256 + header[2];
    if (nlen != (len ^ 0xffff)) return 0;
    if (a->in + len > a->in_end) return 0;
    /* unlike deflate_parse_huffman_block_'s expand sites, nothing here
     * reads a stale local copy of a->out afterward - the memcpy below
     * always reads it fresh, so the two nested guards collapse safely
     * into one. */
    if (a->out + len > a->out_end && !deflate_expand_(a, a->out, (size_t)len)) return 0;
    memcpy(a->out, a->in, (size_t)len);
    a->in += len;
    a->out += len;
    return 1;
}

static i32 deflate_parse_header_(Deflate_State_ *a)
{
    i32 cmf = deflate_get8_(a);
    i32 cm = cmf & 15;
    i32 flg = deflate_get8_(a);
    if (deflate_eof_(a)) return 0;
    if ((cmf * 256 + flg) % 31 != 0) return 0;
    if (flg & 32) return 0; /* preset dictionary not supported */
    if (cm != 8) return 0;  /* DEFLATE is the only compression method zlib defines */
    return 1;
}

static const u8 deflate_default_length_[DEFLATE_NSYMS] = {
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9, 9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7, 7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8
};
static const u8 deflate_default_dist_[32] = {
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5
};

static i32 deflate_parse_(Deflate_State_ *a, i32 parse_header)
{
    i32 final, type;
    if (parse_header)
        if (!deflate_parse_header_(a)) return 0;
    a->num_bits = 0;
    a->code_buffer = 0;
    a->hit_eof_once = 0;
    do {
        final = (i32)deflate_receive_(a, 1);
        type = (i32)deflate_receive_(a, 2);
        switch (type) {
        case 0:
            if (!deflate_parse_uncompressed_block_(a)) return 0;
            break;
        case 1:
            if (!deflate_build_huffman_(&a->z_length, deflate_default_length_, DEFLATE_NSYMS)) return 0;
            if (!deflate_build_huffman_(&a->z_distance, deflate_default_dist_, 32)) return 0;
            if (!deflate_parse_huffman_block_(a)) return 0;
            break;
        case 2:
            if (!deflate_compute_huffman_codes_(a)) return 0;
            if (!deflate_parse_huffman_block_(a)) return 0;
            break;
        default: /* type == 3, reserved by RFC 1951 */
            return 0;
        }
    } while (!final);
    return 1;
}

i32 deflate_decode(Arena *a, const u8 *src, size_t src_len, i32 parse_zlib_header,
                    u8 **out_data, size_t *out_len)
{
    Deflate_State_ z;
    size_t initial = src_len * 2 + 64; /* guess; deflate_expand_ doubles from here as needed */

    z.a = a;
    z.in = src;
    z.in_end = src + src_len;
    z.out_start = (u8 *)arena_alloc(a, initial);
    z.out = z.out_start;
    z.out_end = z.out_start + initial;

    if (!deflate_parse_(&z, parse_zlib_header)) return 0;

    *out_data = z.out_start;
    *out_len = (size_t)(z.out - z.out_start);
    return 1;
}

#endif /* DEFLATE_IMPLEMENTATION_ */
#endif /* DEFLATE_IMPLEMENTATION */
