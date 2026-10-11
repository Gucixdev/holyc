#ifndef HOLYC_JPEG_DEC_H_
#define HOLYC_JPEG_DEC_H_

/* Baseline JPEG decoder, ported from stb_image.h (Sean Barrett, MIT/
 * public domain dual license). Scoped down: baseline (SOF0/SOF1) only -
 * progressive JPEG (SOF2) is a documented gap, not silently missing.
 * Grayscale (1-component) and YCbCr (3-component) supported; CMYK/YCCK
 * (4-component, Adobe APP14) is a documented gap. Standard chroma
 * subsampling ratios (4:4:4, 4:2:2, 4:2:0, and the vertical-only 4:4:0)
 * via dedicated resampling kernels, arbitrary ratios via a generic
 * nearest-neighbor fallback. Always outputs RGBA8, matching png.h's
 * convention, rather than stb_image's request-N-channels flexibility.
 *
 * Algorithm unchanged from the original in the parts that were ported:
 * canonical Huffman decoding with the FAST_BITS direct-lookup
 * acceleration table and a build_fast_ac table that resolves short
 * run-length + magnitude AC codes in one lookup, the AAN-derived scaled
 * integer IDCT (STBI__IDCT_1D's derivation from jidctint's DCT_ISLOW is
 * unchanged - it looks nothing like the DCT-III definition in a textbook
 * but is bit-for-bit what libjpeg produces), and the reduced-precision
 * fixed-point YCbCr->RGB matrix. What changed: malloc -> Arena, the
 * refill-callback stream abstraction -> plain pointer/end-pointer since
 * this port always has the whole compressed buffer available at once (no
 * reason to carry stb_image's incremental-read design when there's
 * nothing incremental about how this project receives image bytes),
 * int/short/unsigned -> i32/i16/u32, SSE2/NEON SIMD kernels dropped (the
 * scalar kernels are what's ported, not a performance regression from
 * anything this project had before), no rich error strings (matches
 * deflate.h/png.h's own plain 0/1 convention). */

#include "core/base.h"
#include "core/arena_alloc.h"

/* returns arena-owned RGBA8 pixels (out_width * out_height * 4 bytes,
 * row-major, no padding) on success, NULL on any parse/format error or
 * unsupported feature (progressive scan, CMYK, non-8-bit precision,
 * arithmetic coding). */
u8 *jpeg_decode(Arena *a, const u8 *data, size_t size, i32 *out_width, i32 *out_height);

#endif /* HOLYC_JPEG_DEC_H_ */

#ifdef JPEG_DEC_IMPLEMENTATION
#ifndef JPEG_DEC_IMPLEMENTATION_
#define JPEG_DEC_IMPLEMENTATION_


#define JPEG_FAST_BITS 9

typedef struct {
    u8 fast[1 << JPEG_FAST_BITS];
    u16 code[256];
    u8 values[256];
    u8 size[257];
    u32 maxcode[18];
    i32 delta[17];
} Jpeg_Huffman_;

typedef struct {
    i32 id, h, v, tq;
    i32 hd, ha;
    i32 dc_pred;
    i32 x, y;   /* effective pixel dims for this component */
    i32 w2, h2; /* padded/allocated dims (multiples of 8, big enough for interleaved MCUs) */
    u8 *data;    /* decoded post-IDCT samples, w2 * h2, arena-owned */
    u8 *linebuf; /* resampling scratch, img_x + 3 bytes, arena-owned */
} Jpeg_Component_;

typedef struct {
    Arena *a;
    const u8 *s;
    const u8 *s_end;

    Jpeg_Component_ img_comp[4];
    i32 img_n, img_x, img_y;
    i32 img_h_max, img_v_max, img_mcu_w, img_mcu_h, img_mcu_x, img_mcu_y;

    u16 dequant[4][64];
    Jpeg_Huffman_ huff_dc[4], huff_ac[4];
    i16 fast_ac[4][1 << JPEG_FAST_BITS];

    u32 code_buffer;
    i32 code_bits;
    u8 marker;
    i32 nomore;

    i32 restart_interval, todo;
    i32 scan_n, order[4];
    i32 rgb;
    i32 jfif;
} Jpeg_State_;

#define JPEG_MARKER_NONE 0xff

static i32 jpeg_at_eof_(Jpeg_State_ *j) { return j->s >= j->s_end; }
/* 0, not 0xff, at EOF - matches stb_image's own stbi__get8 exactly. This
 * matters beyond "what value comes back": jpeg_grow_buffer_unsafe_ treats
 * a 0xff byte as a possible marker lead-in and loops consuming further
 * 0xff fill bytes - returning 0xff at EOF instead of 0 turns that into an
 * infinite loop once the stream runs out (0xff forever, never a
 * terminating non-0xff byte), which is exactly what happened before this
 * was caught by a real truncated-input test hanging. */
static u8 jpeg_get8_(Jpeg_State_ *j) { return jpeg_at_eof_(j) ? 0 : *j->s++; }
static u32 jpeg_get16be_(Jpeg_State_ *j) { u32 hi = jpeg_get8_(j); u32 lo = jpeg_get8_(j); return (hi << 8) | lo; }
static void jpeg_skip_(Jpeg_State_ *j, i32 n) { j->s += n; if (j->s > j->s_end) j->s = j->s_end; }

static const u8 jpeg_dezigzag_[64 + 15] = {
    0,1,8,16,9,2,3,10, 17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34, 27,20,13,6,7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63,
    63,63,63,63,63,63,63,63, 63,63,63,63,63,63,63
};

static i32 jpeg_build_huffman_(Jpeg_Huffman_ *h, const i32 *count)
{
    i32 i, j, k = 0;
    u32 code;

    for (i = 0; i < 16; i++)
        for (j = 0; j < count[i]; j++) {
            h->size[k++] = (u8)(i + 1);
            if (k >= 257) return 0;
        }
    h->size[k] = 0;

    code = 0;
    k = 0;
    for (j = 1; j <= 16; j++) {
        h->delta[j] = k - (i32)code;
        if (h->size[k] == j) {
            while (h->size[k] == j) h->code[k++] = (u16)(code++);
            if (code - 1 >= (1u << j)) return 0;
        }
        h->maxcode[j] = code << (16 - j);
        code <<= 1;
    }
    h->maxcode[17] = 0xffffffffu;

    memset(h->fast, 255, sizeof(h->fast));
    for (i = 0; i < k; i++) {
        i32 s = h->size[i];
        if (s <= JPEG_FAST_BITS) {
            i32 c = h->code[i] << (JPEG_FAST_BITS - s);
            i32 m = 1 << (JPEG_FAST_BITS - s);
            for (j = 0; j < m; j++) h->fast[c + j] = (u8)i;
        }
    }
    return 1;
}

static void jpeg_build_fast_ac_(i16 *fast_ac, Jpeg_Huffman_ *h)
{
    i32 i;
    for (i = 0; i < (1 << JPEG_FAST_BITS); i++) {
        u8 fast = h->fast[i];
        fast_ac[i] = 0;
        if (fast < 255) {
            i32 rs = h->values[fast];
            i32 run = (rs >> 4) & 15;
            i32 magbits = rs & 15;
            i32 len = h->size[fast];
            if (magbits && len + magbits <= JPEG_FAST_BITS) {
                i32 k = ((i << len) & ((1 << JPEG_FAST_BITS) - 1)) >> (JPEG_FAST_BITS - magbits);
                i32 m = 1 << (magbits - 1);
                if (k < m) k += (i32)(~0u << magbits) + 1;
                if (k >= -128 && k <= 127) fast_ac[i] = (i16)((k * 256) + (run * 16) + (len + magbits));
            }
        }
    }
}

static void jpeg_grow_buffer_unsafe_(Jpeg_State_ *j)
{
    do {
        u32 b = j->nomore ? 0 : jpeg_get8_(j);
        if (b == 0xff) {
            u32 c = jpeg_get8_(j);
            while (c == 0xff) c = jpeg_get8_(j);
            if (c != 0) {
                j->marker = (u8)c;
                j->nomore = 1;
                return;
            }
        }
        j->code_buffer |= b << (24 - j->code_bits);
        j->code_bits += 8;
    } while (j->code_bits <= 24);
}

static const u32 jpeg_bmask_[17] = {
    0,1,3,7,15,31,63,127,255,511,1023,2047,4095,8191,16383,32767,65535
};
static const i32 jpeg_bias_[16] = {
    0,-1,-3,-7,-15,-31,-63,-127,-255,-511,-1023,-2047,-4095,-8191,-16383,-32767
};

static i32 jpeg_huff_decode_(Jpeg_State_ *j, Jpeg_Huffman_ *h)
{
    u32 temp;
    i32 c, k;

    if (j->code_bits < 16) jpeg_grow_buffer_unsafe_(j);

    c = (i32)((j->code_buffer >> (32 - JPEG_FAST_BITS)) & ((1u << JPEG_FAST_BITS) - 1));
    k = h->fast[c];
    if (k < 255) {
        i32 s = h->size[k];
        if (s > j->code_bits) return -1;
        j->code_buffer <<= s;
        j->code_bits -= s;
        return h->values[k];
    }

    temp = j->code_buffer >> 16;
    for (k = JPEG_FAST_BITS + 1; ; k++)
        if (temp < h->maxcode[k]) break;
    if (k == 17) { j->code_bits -= 16; return -1; }
    if (k > j->code_bits) return -1;

    c = (i32)((j->code_buffer >> (32 - k)) & jpeg_bmask_[k]) + h->delta[k];
    if (c < 0 || c >= 256) return -1;

    j->code_bits -= k;
    j->code_buffer <<= k;
    return h->values[c];
}

static i32 jpeg_extend_receive_(Jpeg_State_ *j, i32 n)
{
    u32 k;
    i32 sgn;
    if (j->code_bits < n) jpeg_grow_buffer_unsafe_(j);
    if (j->code_bits < n) return 0;

    sgn = (i32)(j->code_buffer >> 31);
    k = (j->code_buffer << n) | (j->code_buffer >> (32 - n)); /* rotate left n */
    j->code_buffer = k & ~jpeg_bmask_[n];
    k &= jpeg_bmask_[n];
    j->code_bits -= n;
    return (i32)k + (jpeg_bias_[n] & (sgn - 1));
}

static i32 jpeg_decode_block_(Jpeg_State_ *j, i16 data[64], Jpeg_Huffman_ *hdc, Jpeg_Huffman_ *hac,
                               i16 *fac, i32 b, u16 *dequant)
{
    i32 diff, dc, t, k;

    if (j->code_bits < 16) jpeg_grow_buffer_unsafe_(j);
    t = jpeg_huff_decode_(j, hdc);
    if (t < 0 || t > 15) return 0;

    memset(data, 0, 64 * sizeof(data[0]));

    diff = t ? jpeg_extend_receive_(j, t) : 0;
    dc = j->img_comp[b].dc_pred + diff;
    j->img_comp[b].dc_pred = dc;
    data[0] = (i16)(dc * dequant[0]);

    k = 1;
    do {
        u32 zig;
        i32 c, r, s;
        if (j->code_bits < 16) jpeg_grow_buffer_unsafe_(j);
        c = (i32)((j->code_buffer >> (32 - JPEG_FAST_BITS)) & ((1u << JPEG_FAST_BITS) - 1));
        r = fac[c];
        if (r) {
            k += (r >> 4) & 15;
            s = r & 15;
            if (s > j->code_bits) return 0;
            j->code_buffer <<= s;
            j->code_bits -= s;
            zig = jpeg_dezigzag_[k++];
            data[zig] = (i16)((r >> 8) * dequant[zig]);
        } else {
            i32 rs = jpeg_huff_decode_(j, hac);
            if (rs < 0) return 0;
            s = rs & 15;
            r = rs >> 4;
            if (s == 0) {
                if (rs != 0xf0) break;
                k += 16;
            } else {
                k += r;
                zig = jpeg_dezigzag_[k++];
                data[zig] = (i16)(jpeg_extend_receive_(j, s) * dequant[zig]);
            }
        }
    } while (k < 64);
    return 1;
}

/* AAN-derived scaled integer IDCT, unchanged from jidctint's DCT_ISLOW
 * (see file header) - the constant-folding shape here is deliberate, not
 * arbitrary: it produces bit-identical output to libjpeg's own reference
 * implementation, which real-world JPEG encoders were tuned against. */
#define JPEG_F2F_(x) ((i32)(((x) * 4096 + 0.5)))
#define JPEG_FSH_(x) ((x) * 4096)

static void jpeg_idct_1d_(i32 s0, i32 s1, i32 s2, i32 s3, i32 s4, i32 s5, i32 s6, i32 s7,
                           i32 *out_t0, i32 *out_t1, i32 *out_t2, i32 *out_t3,
                           i32 *out_x0, i32 *out_x1, i32 *out_x2, i32 *out_x3)
{
    i32 t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;
    p2 = s2; p3 = s6;
    p1 = (p2 + p3) * JPEG_F2F_(0.5411961f);
    t2 = p1 + p3 * JPEG_F2F_(-1.847759065f);
    t3 = p1 + p2 * JPEG_F2F_(0.765366865f);
    p2 = s0; p3 = s4;
    t0 = JPEG_FSH_(p2 + p3);
    t1 = JPEG_FSH_(p2 - p3);
    x0 = t0 + t3; x3 = t0 - t3;
    x1 = t1 + t2; x2 = t1 - t2;
    t0 = s7; t1 = s5; t2 = s3; t3 = s1;
    p3 = t0 + t2; p4 = t1 + t3;
    p1 = t0 + t3; p2 = t1 + t2;
    p5 = (p3 + p4) * JPEG_F2F_(1.175875602f);
    t0 = t0 * JPEG_F2F_(0.298631336f);
    t1 = t1 * JPEG_F2F_(2.053119869f);
    t2 = t2 * JPEG_F2F_(3.072711026f);
    t3 = t3 * JPEG_F2F_(1.501321110f);
    p1 = p5 + p1 * JPEG_F2F_(-0.899976223f);
    p2 = p5 + p2 * JPEG_F2F_(-2.562915447f);
    p3 = p3 * JPEG_F2F_(-1.961570560f);
    p4 = p4 * JPEG_F2F_(-0.390180644f);
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;
    *out_t0 = t0; *out_t1 = t1; *out_t2 = t2; *out_t3 = t3;
    *out_x0 = x0; *out_x1 = x1; *out_x2 = x2; *out_x3 = x3;
}

static u8 jpeg_clamp_(i32 x)
{
    if ((u32)x > 255) { if (x < 0) return 0; return 255; }
    return (u8)x;
}

static void jpeg_idct_block_(u8 *out, i32 out_stride, i16 data[64])
{
    i32 i, val[64];
    i32 *v = val;
    i16 *d = data;
    u8 *o;

    for (i = 0; i < 8; i++, d++, v++) {
        i32 t0, t1, t2, t3, x0, x1, x2, x3;
        if (d[8] == 0 && d[16] == 0 && d[24] == 0 && d[32] == 0 && d[40] == 0 && d[48] == 0 && d[56] == 0) {
            i32 dcterm = d[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dcterm;
        } else {
            jpeg_idct_1d_(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56],
                          &t0, &t1, &t2, &t3, &x0, &x1, &x2, &x3);
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[0] = (x0 + t3) >> 10; v[56] = (x0 - t3) >> 10;
            v[8] = (x1 + t2) >> 10; v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10; v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10; v[32] = (x3 - t0) >> 10;
        }
    }

    for (i = 0, v = val, o = out; i < 8; i++, v += 8, o += out_stride) {
        i32 t0, t1, t2, t3, x0, x1, x2, x3;
        jpeg_idct_1d_(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                      &t0, &t1, &t2, &t3, &x0, &x1, &x2, &x3);
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        o[0] = jpeg_clamp_((x0 + t3) >> 17); o[7] = jpeg_clamp_((x0 - t3) >> 17);
        o[1] = jpeg_clamp_((x1 + t2) >> 17); o[6] = jpeg_clamp_((x1 - t2) >> 17);
        o[2] = jpeg_clamp_((x2 + t1) >> 17); o[5] = jpeg_clamp_((x2 - t1) >> 17);
        o[3] = jpeg_clamp_((x3 + t0) >> 17); o[4] = jpeg_clamp_((x3 - t0) >> 17);
    }
}

#define JPEG_RESTART_(x) ((x) >= 0xd0 && (x) <= 0xd7)

static u8 jpeg_get_marker_(Jpeg_State_ *j)
{
    u8 x;
    if (j->marker != JPEG_MARKER_NONE) { x = j->marker; j->marker = JPEG_MARKER_NONE; return x; }
    x = jpeg_get8_(j);
    if (x != 0xff) return JPEG_MARKER_NONE;
    while (x == 0xff) x = jpeg_get8_(j);
    return x;
}

static void jpeg_reset_(Jpeg_State_ *j)
{
    j->code_bits = 0;
    j->code_buffer = 0;
    j->nomore = 0;
    j->img_comp[0].dc_pred = j->img_comp[1].dc_pred = j->img_comp[2].dc_pred = j->img_comp[3].dc_pred = 0;
    j->marker = JPEG_MARKER_NONE;
    j->todo = j->restart_interval ? j->restart_interval : 0x7fffffff;
}

static i32 jpeg_parse_entropy_coded_data_(Jpeg_State_ *z)
{
    i16 data[64];
    jpeg_reset_(z);

    if (z->scan_n == 1) {
        i32 n = z->order[0];
        i32 w = (z->img_comp[n].x + 7) >> 3;
        i32 h = (z->img_comp[n].y + 7) >> 3;
        i32 i, jj;
        for (jj = 0; jj < h; jj++) {
            for (i = 0; i < w; i++) {
                i32 ha = z->img_comp[n].ha;
                if (!jpeg_decode_block_(z, data, z->huff_dc + z->img_comp[n].hd, z->huff_ac + ha,
                                         z->fast_ac[ha], n, z->dequant[z->img_comp[n].tq])) return 0;
                jpeg_idct_block_(z->img_comp[n].data + z->img_comp[n].w2 * jj * 8 + i * 8, z->img_comp[n].w2, data);
                if (--z->todo <= 0) {
                    if (z->code_bits < 24) jpeg_grow_buffer_unsafe_(z);
                    if (!JPEG_RESTART_(z->marker)) return 1;
                    jpeg_reset_(z);
                }
            }
        }
        return 1;
    } else {
        i32 i, jj, k, x, y;
        for (jj = 0; jj < z->img_mcu_y; jj++) {
            for (i = 0; i < z->img_mcu_x; i++) {
                for (k = 0; k < z->scan_n; k++) {
                    i32 n = z->order[k];
                    for (y = 0; y < z->img_comp[n].v; y++) {
                        for (x = 0; x < z->img_comp[n].h; x++) {
                            i32 x2 = (i * z->img_comp[n].h + x) * 8;
                            i32 y2 = (jj * z->img_comp[n].v + y) * 8;
                            i32 ha = z->img_comp[n].ha;
                            if (!jpeg_decode_block_(z, data, z->huff_dc + z->img_comp[n].hd, z->huff_ac + ha,
                                                     z->fast_ac[ha], n, z->dequant[z->img_comp[n].tq])) return 0;
                            jpeg_idct_block_(z->img_comp[n].data + z->img_comp[n].w2 * y2 + x2, z->img_comp[n].w2, data);
                        }
                    }
                }
                if (--z->todo <= 0) {
                    if (z->code_bits < 24) jpeg_grow_buffer_unsafe_(z);
                    if (!JPEG_RESTART_(z->marker)) return 1;
                    jpeg_reset_(z);
                }
            }
        }
        return 1;
    }
}

static i32 jpeg_process_marker_(Jpeg_State_ *z, i32 m)
{
    i32 l;
    switch (m) {
    case JPEG_MARKER_NONE:
        return 0;
    case 0xDD:
        if (jpeg_get16be_(z) != 4) return 0;
        z->restart_interval = (i32)jpeg_get16be_(z);
        return 1;
    case 0xDB:
        l = (i32)jpeg_get16be_(z) - 2;
        while (l > 0) {
            i32 q = jpeg_get8_(z);
            i32 sixteen = (q >> 4) != 0;
            i32 t = q & 15, i;
            if (t > 3) return 0;
            for (i = 0; i < 64; i++)
                z->dequant[t][jpeg_dezigzag_[i]] = (u16)(sixteen ? jpeg_get16be_(z) : jpeg_get8_(z));
            l -= (sixteen ? 129 : 65);
        }
        return l == 0;
    case 0xC4:
        l = (i32)jpeg_get16be_(z) - 2;
        while (l > 0) {
            u8 *v;
            i32 sizes[16], i, n = 0;
            i32 q = jpeg_get8_(z);
            i32 tc = q >> 4;
            i32 th = q & 15;
            if (tc > 1 || th > 3) return 0;
            for (i = 0; i < 16; i++) { sizes[i] = jpeg_get8_(z); n += sizes[i]; }
            if (n > 256) return 0;
            l -= 17;
            if (tc == 0) {
                if (!jpeg_build_huffman_(z->huff_dc + th, sizes)) return 0;
                v = z->huff_dc[th].values;
            } else {
                if (!jpeg_build_huffman_(z->huff_ac + th, sizes)) return 0;
                v = z->huff_ac[th].values;
            }
            for (i = 0; i < n; i++) v[i] = jpeg_get8_(z);
            if (tc != 0) jpeg_build_fast_ac_(z->fast_ac[th], z->huff_ac + th);
            l -= n;
        }
        return l == 0;
    }

    switch (m) {
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: case 0xE6: case 0xE7:
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xEC: case 0xED: case 0xEE: case 0xEF:
    case 0xFE:
        l = (i32)jpeg_get16be_(z);
        if (l < 2) return 0;
        l -= 2;
        if (m == 0xE0 && l >= 5) {
            static const u8 tag[5] = { 'J','F','I','F','\0' };
            i32 ok = 1, i;
            for (i = 0; i < 5; i++) if (jpeg_get8_(z) != tag[i]) ok = 0;
            l -= 5;
            if (ok) z->jfif = 1;
        }
        jpeg_skip_(z, l);
        return 1;
    default:
        return 0; /* unknown marker where a comment/APPn/DQT/DHT/DRI/SOF/SOS/EOI was expected: reject rather than guess */
    }
}

static i32 jpeg_process_scan_header_(Jpeg_State_ *z)
{
    i32 i, ls, spec_start, spec_end, succ;
    ls = (i32)jpeg_get16be_(z);
    z->scan_n = jpeg_get8_(z);
    if (z->scan_n < 1 || z->scan_n > 4 || z->scan_n > z->img_n) return 0;
    if (ls != 6 + 2 * z->scan_n) return 0;
    for (i = 0; i < z->scan_n; i++) {
        i32 id = jpeg_get8_(z), which;
        i32 q = jpeg_get8_(z);
        for (which = 0; which < z->img_n; which++) if (z->img_comp[which].id == id) break;
        if (which == z->img_n) return 0;
        z->img_comp[which].hd = q >> 4; if (z->img_comp[which].hd > 3) return 0;
        z->img_comp[which].ha = q & 15; if (z->img_comp[which].ha > 3) return 0;
        z->order[i] = which;
    }
    spec_start = jpeg_get8_(z);
    spec_end = jpeg_get8_(z);
    succ = jpeg_get8_(z);
    if (spec_start != 0 || spec_end != 63 || succ != 0) return 0; /* baseline requires this; progressive (which varies these) is a documented gap */
    return 1;
}

static i32 jpeg_process_frame_header_(Jpeg_State_ *z)
{
    i32 lf, p, i, q, h_max = 1, v_max = 1, c;
    lf = (i32)jpeg_get16be_(z); if (lf < 11) return 0;
    p = jpeg_get8_(z); if (p != 8) return 0; /* 8-bit precision only */
    z->img_y = (i32)jpeg_get16be_(z); if (z->img_y == 0) return 0;
    z->img_x = (i32)jpeg_get16be_(z); if (z->img_x == 0) return 0;
    c = jpeg_get8_(z);
    if (c != 3 && c != 1) return 0; /* CMYK (c==4) is a documented gap */
    z->img_n = c;
    if (lf != 8 + 3 * z->img_n) return 0;

    z->rgb = 0;
    for (i = 0; i < z->img_n; i++) {
        static const u8 rgb_tag[3] = { 'R','G','B' };
        z->img_comp[i].id = jpeg_get8_(z);
        if (z->img_n == 3 && z->img_comp[i].id == rgb_tag[i]) z->rgb++;
        q = jpeg_get8_(z);
        z->img_comp[i].h = q >> 4; if (!z->img_comp[i].h || z->img_comp[i].h > 4) return 0;
        z->img_comp[i].v = q & 15; if (!z->img_comp[i].v || z->img_comp[i].v > 4) return 0;
        z->img_comp[i].tq = jpeg_get8_(z); if (z->img_comp[i].tq > 3) return 0;
    }

    for (i = 0; i < z->img_n; i++) {
        if (z->img_comp[i].h > h_max) h_max = z->img_comp[i].h;
        if (z->img_comp[i].v > v_max) v_max = z->img_comp[i].v;
    }
    for (i = 0; i < z->img_n; i++) {
        if (h_max % z->img_comp[i].h != 0) return 0;
        if (v_max % z->img_comp[i].v != 0) return 0;
    }

    z->img_h_max = h_max; z->img_v_max = v_max;
    z->img_mcu_w = h_max * 8; z->img_mcu_h = v_max * 8;
    z->img_mcu_x = (z->img_x + z->img_mcu_w - 1) / z->img_mcu_w;
    z->img_mcu_y = (z->img_y + z->img_mcu_h - 1) / z->img_mcu_h;

    for (i = 0; i < z->img_n; i++) {
        z->img_comp[i].x = (z->img_x * z->img_comp[i].h + h_max - 1) / h_max;
        z->img_comp[i].y = (z->img_y * z->img_comp[i].v + v_max - 1) / v_max;
        z->img_comp[i].w2 = z->img_mcu_x * z->img_comp[i].h * 8;
        z->img_comp[i].h2 = z->img_mcu_y * z->img_comp[i].v * 8;
        z->img_comp[i].data = (u8 *)arena_alloc(z->a, (size_t)z->img_comp[i].w2 * (size_t)z->img_comp[i].h2);
        z->img_comp[i].linebuf = NULL;
    }
    return 1;
}

#define JPEG_SOI_(x) ((x) == 0xd8)
#define JPEG_EOI_(x) ((x) == 0xd9)
#define JPEG_SOF_(x) ((x) == 0xc0 || (x) == 0xc1)
#define JPEG_SOF_PROGRESSIVE_(x) ((x) == 0xc2)
#define JPEG_SOS_(x) ((x) == 0xda)

static i32 jpeg_decode_header_(Jpeg_State_ *z)
{
    i32 m;
    z->jfif = 0;
    z->marker = JPEG_MARKER_NONE;
    m = jpeg_get_marker_(z);
    if (!JPEG_SOI_(m)) return 0;
    m = jpeg_get_marker_(z);
    while (!JPEG_SOF_(m)) {
        if (JPEG_SOF_PROGRESSIVE_(m)) return 0; /* progressive: documented gap */
        if (!jpeg_process_marker_(z, m)) return 0;
        m = jpeg_get_marker_(z);
        while (m == JPEG_MARKER_NONE) {
            if (jpeg_at_eof_(z)) return 0;
            m = jpeg_get_marker_(z);
        }
    }
    return jpeg_process_frame_header_(z);
}

static u8 jpeg_skip_junk_at_end_(Jpeg_State_ *j)
{
    while (!jpeg_at_eof_(j)) {
        u8 x = jpeg_get8_(j);
        while (x == 0xff) {
            if (jpeg_at_eof_(j)) return JPEG_MARKER_NONE;
            x = jpeg_get8_(j);
            if (x != 0x00 && x != 0xff) return x;
        }
    }
    return JPEG_MARKER_NONE;
}

static i32 jpeg_decode_image_(Jpeg_State_ *j)
{
    i32 m;
    j->restart_interval = 0;
    if (!jpeg_decode_header_(j)) return 0;
    m = jpeg_get_marker_(j);
    while (!JPEG_EOI_(m)) {
        if (JPEG_SOS_(m)) {
            if (!jpeg_process_scan_header_(j)) return 0;
            if (!jpeg_parse_entropy_coded_data_(j)) return 0;
            if (j->marker == JPEG_MARKER_NONE) j->marker = jpeg_skip_junk_at_end_(j);
            m = jpeg_get_marker_(j);
            while (JPEG_RESTART_(m)) m = jpeg_get_marker_(j);
        } else {
            if (!jpeg_process_marker_(j, m)) return 0;
            m = jpeg_get_marker_(j);
        }
    }
    return 1;
}

/* ---- resampling (chroma upsampling to the full-resolution pixel grid) ---- */

static u8 jpeg_div4_(i32 x) { return (u8)(x >> 2); }
static u8 jpeg_div16_(i32 x) { return (u8)(x >> 4); }

static u8 *jpeg_resample_1_(u8 *out, u8 *in_near, u8 *in_far, i32 w, i32 hs)
{
    (void)out; (void)in_far; (void)w; (void)hs;
    return in_near;
}
static u8 *jpeg_resample_v_2_(u8 *out, u8 *in_near, u8 *in_far, i32 w, i32 hs)
{
    i32 i;
    (void)hs;
    for (i = 0; i < w; i++) out[i] = jpeg_div4_(3 * in_near[i] + in_far[i] + 2);
    return out;
}
static u8 *jpeg_resample_h_2_(u8 *out, u8 *in_near, u8 *in_far, i32 w, i32 hs)
{
    i32 i;
    (void)in_far; (void)hs;
    if (w == 1) { out[0] = out[1] = in_near[0]; return out; }
    out[0] = in_near[0];
    out[1] = jpeg_div4_(in_near[0] * 3 + in_near[1] + 2);
    for (i = 1; i < w - 1; i++) {
        i32 n = 3 * in_near[i] + 2;
        out[i * 2 + 0] = jpeg_div4_(n + in_near[i - 1]);
        out[i * 2 + 1] = jpeg_div4_(n + in_near[i + 1]);
    }
    out[i * 2 + 0] = jpeg_div4_(in_near[w - 2] * 3 + in_near[w - 1] + 2);
    out[i * 2 + 1] = in_near[w - 1];
    return out;
}
static u8 *jpeg_resample_hv_2_(u8 *out, u8 *in_near, u8 *in_far, i32 w, i32 hs)
{
    i32 i, t0, t1;
    (void)hs;
    if (w == 1) { out[0] = out[1] = jpeg_div4_(3 * in_near[0] + in_far[0] + 2); return out; }
    t1 = 3 * in_near[0] + in_far[0];
    out[0] = jpeg_div4_(t1 + 2);
    for (i = 1; i < w; i++) {
        t0 = t1;
        t1 = 3 * in_near[i] + in_far[i];
        out[i * 2 - 1] = jpeg_div16_(3 * t0 + t1 + 8);
        out[i * 2] = jpeg_div16_(3 * t1 + t0 + 8);
    }
    out[w * 2 - 1] = jpeg_div4_(t1 + 2);
    return out;
}
static u8 *jpeg_resample_generic_(u8 *out, u8 *in_near, u8 *in_far, i32 w, i32 hs)
{
    i32 i, jj;
    (void)in_far;
    for (i = 0; i < w; i++) for (jj = 0; jj < hs; jj++) out[i * hs + jj] = in_near[i];
    return out;
}

#define JPEG_F2FIXED_(x) (((i32)((x) * 4096.0f + 0.5f)) << 8)

static void jpeg_ycbcr_to_rgb_row_(u8 *out, const u8 *y, const u8 *pcb, const u8 *pcr, i32 count)
{
    i32 i;
    for (i = 0; i < count; i++) {
        i32 y_fixed = (y[i] << 20) + (1 << 19);
        i32 r, g, b;
        i32 cr = pcr[i] - 128;
        i32 cb = pcb[i] - 128;
        r = y_fixed + cr * JPEG_F2FIXED_(1.40200f);
        g = y_fixed + (cr * -JPEG_F2FIXED_(0.71414f)) + ((cb * -JPEG_F2FIXED_(0.34414f)) & 0xffff0000);
        b = y_fixed + cb * JPEG_F2FIXED_(1.77200f);
        r >>= 20; g >>= 20; b >>= 20;
        out[0] = jpeg_clamp_(r);
        out[1] = jpeg_clamp_(g);
        out[2] = jpeg_clamp_(b);
        out[3] = 255;
        out += 4;
    }
}

typedef struct {
    u8 *(*resample)(u8 *, u8 *, u8 *, i32, i32);
    u8 *line0, *line1;
    i32 hs, vs, w_lores, ystep, ypos;
} Jpeg_Resample_;

u8 *jpeg_decode(Arena *a, const u8 *data, size_t size, i32 *out_width, i32 *out_height)
{
    Jpeg_State_ z;
    Jpeg_Resample_ res_comp[4];
    u8 *coutput[4];
    u8 *output;
    i32 k, is_rgb;
    u32 i, jj;

    memset(&z, 0, sizeof(z));
    z.a = a;
    z.s = data;
    z.s_end = data + size;

    if (!jpeg_decode_image_(&z)) return NULL;

    is_rgb = z.img_n == 3 && z.rgb == 3;

    for (k = 0; k < z.img_n; k++) {
        Jpeg_Resample_ *r = &res_comp[k];
        z.img_comp[k].linebuf = (u8 *)arena_alloc(a, (size_t)z.img_x + 3);
        r->hs = z.img_h_max / z.img_comp[k].h;
        r->vs = z.img_v_max / z.img_comp[k].v;
        r->ystep = r->vs >> 1;
        r->w_lores = (z.img_x + r->hs - 1) / r->hs;
        r->ypos = 0;
        r->line0 = r->line1 = z.img_comp[k].data;

        switch (r->hs * 8 + r->vs) {
        case 1 * 8 + 1: r->resample = jpeg_resample_1_; break;
        case 1 * 8 + 2: r->resample = jpeg_resample_v_2_; break;
        case 2 * 8 + 1: r->resample = jpeg_resample_h_2_; break;
        case 2 * 8 + 2: r->resample = jpeg_resample_hv_2_; break;
        default: r->resample = jpeg_resample_generic_; break;
        }
    }

    output = (u8 *)arena_alloc(a, (size_t)z.img_x * (size_t)z.img_y * 4);

    for (jj = 0; jj < (u32)z.img_y; jj++) {
        u8 *out = output + 4 * (size_t)z.img_x * jj;
        for (k = 0; k < z.img_n; k++) {
            Jpeg_Resample_ *r = &res_comp[k];
            i32 y_bot = r->ystep >= (r->vs >> 1);
            coutput[k] = r->resample(z.img_comp[k].linebuf, y_bot ? r->line1 : r->line0, y_bot ? r->line0 : r->line1,
                                      r->w_lores, r->hs);
            if (++r->ystep >= r->vs) {
                r->ystep = 0;
                r->line0 = r->line1;
                if (++r->ypos < z.img_comp[k].y) r->line1 += z.img_comp[k].w2;
            }
        }
        if (z.img_n == 3) {
            if (is_rgb) {
                u8 *y = coutput[0];
                for (i = 0; i < (u32)z.img_x; i++) {
                    out[0] = y[i]; out[1] = coutput[1][i]; out[2] = coutput[2][i]; out[3] = 255;
                    out += 4;
                }
            } else {
                jpeg_ycbcr_to_rgb_row_(out, coutput[0], coutput[1], coutput[2], z.img_x);
            }
        } else {
            u8 *y = coutput[0];
            for (i = 0; i < (u32)z.img_x; i++) {
                out[0] = out[1] = out[2] = y[i]; out[3] = 255;
                out += 4;
            }
        }
    }

    *out_width = z.img_x;
    *out_height = z.img_y;
    return output;
}

#endif /* JPEG_DEC_IMPLEMENTATION_ */
#endif /* JPEG_DEC_IMPLEMENTATION */
