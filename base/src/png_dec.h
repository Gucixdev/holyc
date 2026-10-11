#ifndef HOLYC_PNG_DEC_H_
#define HOLYC_PNG_DEC_H_

/* PNG decoder, ported from stb_image.h (Sean Barrett, MIT/public domain
 * dual license). Scoped down from stb_image's own generality: 8-bit
 * depth only (1/2/4/16-bit are a documented gap, not silently missing),
 * non-interlaced only (Adam7 is a documented gap), no tRNS chunk support
 * (palette/color-key transparency - a documented gap; alpha-channel color
 * types 4/6 work fully). Color types 0 (grayscale), 2 (RGB), 3 (palette),
 * 4 (grayscale+alpha), 6 (RGBA) are all supported and always converted to
 * RGBA8 output - one consistent pixel format rather than stb_image's
 * request-N-channels flexibility, simpler for this project's actual needs.
 *
 * Depends on deflate.h for IDAT decompression - shared with any other
 * zlib/DEFLATE consumer, not a PNG-specific copy.
 *
 * Filter reconstruction (the per-scanline None/Sub/Up/Average/Paeth
 * predictors) and the Paeth predictor's branch-free reformulation are
 * unchanged from the original - that formulation looks different from the
 * PNG spec's own reference code but is mathematically equivalent with
 * better data dependencies for the compiler to pipeline. */

#include "base.h"
#include "arena_alloc.h"
#include "deflate.h"

/* returns arena-owned RGBA8 pixels (out_width * out_height * 4 bytes,
 * row-major, no padding) on success, NULL on any parse/format error or
 * unsupported feature (bit depth != 8, interlaced, tRNS present). */
u8 *png_decode(Arena *a, const u8 *data, size_t size, i32 *out_width, i32 *out_height);

#endif /* HOLYC_PNG_DEC_H_ */

#ifdef PNG_DEC_IMPLEMENTATION
#ifndef PNG_DEC_IMPLEMENTATION_
#define PNG_DEC_IMPLEMENTATION_


static u32 png_get32be_(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

#define PNG_FOURCC_(c0, c1, c2, c3) \
    (((u32)(u8)(c0) << 24) | ((u32)(u8)(c1) << 16) | ((u32)(u8)(c2) << 8) | (u32)(u8)(c3))

static i32 png_paeth_(i32 a, i32 b, i32 c)
{
    i32 thresh = c * 3 - (a + b);
    i32 lo = a < b ? a : b;
    i32 hi = a < b ? b : a;
    i32 t0 = (hi <= thresh) ? lo : c;
    i32 t1 = (thresh <= lo) ? hi : t0;
    return t1;
}

#define PNG_F_NONE 0
#define PNG_F_SUB 1
#define PNG_F_UP 2
#define PNG_F_AVG 3
#define PNG_F_PAETH 4
#define PNG_F_AVG_FIRST 5 /* first-row substitute for AVG: prior row treated as all zero */

static const u8 png_first_row_filter_[5] = { PNG_F_NONE, PNG_F_SUB, PNG_F_NONE, PNG_F_AVG_FIRST, PNG_F_SUB };

/* unfilters `raw` (1 filter-type byte + img_width_bytes payload, per row,
 * y rows total) into img_n-channel-per-pixel scanlines in `out`. Kept
 * separate from color-type/palette expansion below - defiltering is the
 * same regardless of what the bytes eventually mean. */
static i32 png_unfilter_(Arena *a, const u8 *raw, size_t raw_len, u8 *out,
                          u32 width, u32 height, i32 img_n)
{
    u32 img_width_bytes = width * (u32)img_n;
    u8 *filter_buf;
    u32 j;
    size_t k;

    if (raw_len < (size_t)(img_width_bytes + 1) * height) return 0;

    filter_buf = (u8 *)arena_alloc(a, (size_t)img_width_bytes * 2);

    for (j = 0; j < height; j++) {
        u8 *cur = filter_buf + (j & 1) * img_width_bytes;
        u8 *prior = filter_buf + (~j & 1) * img_width_bytes;
        u8 *dest = out + (size_t)img_width_bytes * j;
        i32 filter = *raw++;

        if (filter > 4) return 0;
        if (j == 0) filter = png_first_row_filter_[filter];

        switch (filter) {
        case PNG_F_NONE:
            memcpy(cur, raw, img_width_bytes);
            break;
        case PNG_F_SUB:
            memcpy(cur, raw, (size_t)img_n);
            for (k = (size_t)img_n; k < img_width_bytes; k++)
                cur[k] = (u8)(raw[k] + cur[k - (size_t)img_n]);
            break;
        case PNG_F_UP:
            for (k = 0; k < img_width_bytes; k++)
                cur[k] = (u8)(raw[k] + prior[k]);
            break;
        case PNG_F_AVG:
            for (k = 0; k < (size_t)img_n; k++)
                cur[k] = (u8)(raw[k] + (prior[k] >> 1));
            for (k = (size_t)img_n; k < img_width_bytes; k++)
                cur[k] = (u8)(raw[k] + ((prior[k] + cur[k - (size_t)img_n]) >> 1));
            break;
        case PNG_F_PAETH:
            for (k = 0; k < (size_t)img_n; k++)
                cur[k] = (u8)(raw[k] + prior[k]); /* prior[k] == paeth(0,prior[k],0) */
            for (k = (size_t)img_n; k < img_width_bytes; k++)
                cur[k] = (u8)(raw[k] + png_paeth_(cur[k - (size_t)img_n], prior[k], prior[k - (size_t)img_n]));
            break;
        case PNG_F_AVG_FIRST:
            memcpy(cur, raw, (size_t)img_n);
            for (k = (size_t)img_n; k < img_width_bytes; k++)
                cur[k] = (u8)(raw[k] + (cur[k - (size_t)img_n] >> 1));
            break;
        }

        raw += img_width_bytes;
        memcpy(dest, cur, img_width_bytes);
    }
    return 1;
}

static void png_expand_to_rgba_(u8 *out, const u8 *scan, u32 width, u32 height,
                                 i32 color, const u8 *palette)
{
    u32 total = width * height;
    u32 i;

    switch (color) {
    case 0: /* grayscale */
        for (i = 0; i < total; i++) {
            u8 v = scan[i];
            out[i * 4 + 0] = v; out[i * 4 + 1] = v; out[i * 4 + 2] = v; out[i * 4 + 3] = 255;
        }
        break;
    case 2: /* RGB */
        for (i = 0; i < total; i++) {
            out[i * 4 + 0] = scan[i * 3 + 0];
            out[i * 4 + 1] = scan[i * 3 + 1];
            out[i * 4 + 2] = scan[i * 3 + 2];
            out[i * 4 + 3] = 255;
        }
        break;
    case 3: /* palette */
        for (i = 0; i < total; i++) {
            u8 idx = scan[i];
            out[i * 4 + 0] = palette[idx * 4 + 0];
            out[i * 4 + 1] = palette[idx * 4 + 1];
            out[i * 4 + 2] = palette[idx * 4 + 2];
            out[i * 4 + 3] = palette[idx * 4 + 3];
        }
        break;
    case 4: /* grayscale + alpha */
        for (i = 0; i < total; i++) {
            u8 v = scan[i * 2 + 0];
            out[i * 4 + 0] = v; out[i * 4 + 1] = v; out[i * 4 + 2] = v;
            out[i * 4 + 3] = scan[i * 2 + 1];
        }
        break;
    case 6: /* RGBA */
        memcpy(out, scan, (size_t)total * 4);
        break;
    }
}

static i32 png_channels_for_color_(i32 color)
{
    switch (color) {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1; /* palette index, 1 byte/pixel pre-expansion */
    case 4: return 2;
    case 6: return 4;
    default: return -1;
    }
}

u8 *png_decode(Arena *a, const u8 *data, size_t size, i32 *out_width, i32 *out_height)
{
    static const u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    const u8 *p = data;
    const u8 *end = data + size;
    u32 width = 0, height = 0;
    i32 depth = 0, color = -1, interlace = 0;
    u8 palette[1024]; /* 256 entries * RGBA */
    u32 pal_len = 0;
    i32 have_ihdr = 0;
    u8 *idata = NULL;
    size_t idata_len = 0, idata_cap = 0;
    i32 img_n = 0; /* always set before use (only reached once have_ihdr is true) - initialized here only to satisfy -Wmaybe-uninitialized's flow analysis across the chunk loop */

    if (size < 8 || memcmp(data, sig, 8) != 0) return NULL;
    p += 8;

    while (p + 8 <= end) {
        u32 len = png_get32be_(p);
        u32 type_id = png_get32be_(p + 4);
        const u8 *chunk_data = p + 8;
        i32 end_reached = 0;
        if (chunk_data + len + 4 > end) return NULL;

        switch (type_id) {
        case PNG_FOURCC_('I', 'H', 'D', 'R'):
            if (have_ihdr || len != 13) return NULL;
            have_ihdr = 1;
            width = png_get32be_(chunk_data);
            height = png_get32be_(chunk_data + 4);
            depth = chunk_data[8];
            color = chunk_data[9];
            interlace = chunk_data[12];
            if (width == 0 || height == 0) return NULL;
            if (depth != 8) return NULL;       /* documented gap: 1/2/4/16-bit */
            if (interlace != 0) return NULL;   /* documented gap: Adam7 */
            img_n = png_channels_for_color_(color);
            if (img_n < 0) return NULL;
            break;
        case PNG_FOURCC_('P', 'L', 'T', 'E'): {
            u32 i;
            if (!have_ihdr || len > 256 * 3 || len % 3 != 0) return NULL;
            pal_len = len / 3;
            for (i = 0; i < pal_len; i++) {
                palette[i * 4 + 0] = chunk_data[i * 3 + 0];
                palette[i * 4 + 1] = chunk_data[i * 3 + 1];
                palette[i * 4 + 2] = chunk_data[i * 3 + 2];
                palette[i * 4 + 3] = 255;
            }
            break;
        }
        case PNG_FOURCC_('t', 'R', 'N', 'S'):
            return NULL; /* documented gap: transparency chunk not supported */
        case PNG_FOURCC_('I', 'D', 'A', 'T'):
            if (!have_ihdr) return NULL;
            if (idata_len + len > idata_cap) {
                size_t new_cap = idata_cap == 0 ? (len > 4096 ? len : 4096) : idata_cap;
                while (idata_len + len > new_cap) new_cap *= 2;
                idata = (u8 *)arena_realloc(a, idata, idata_cap, new_cap);
                idata_cap = new_cap;
            }
            memcpy(idata + idata_len, chunk_data, len);
            idata_len += len;
            break;
        case PNG_FOURCC_('I', 'E', 'N', 'D'):
            end_reached = 1;
            break;
        default:
            /* unrecognized/ancillary chunk types are silently skipped, matching PNG's own extensibility rule */
            break;
        }
        if (end_reached) break;

        p = chunk_data + len + 4; /* + 4 for the CRC, not verified (same tradeoff as skipping Adler32 in deflate.h) */
    }

    if (!have_ihdr || idata == NULL) return NULL;
    if (color == 3 && pal_len == 0) return NULL;

    {
        u8 *expanded;
        size_t expanded_len;
        u8 *unfiltered;
        u8 *rgba;

        if (!deflate_decode(a, idata, idata_len, 1, &expanded, &expanded_len)) return NULL;

        unfiltered = (u8 *)arena_alloc(a, (size_t)width * height * (size_t)img_n);
        if (!png_unfilter_(a, expanded, expanded_len, unfiltered, width, height, img_n)) return NULL;

        rgba = (u8 *)arena_alloc(a, (size_t)width * height * 4);
        png_expand_to_rgba_(rgba, unfiltered, width, height, color, palette);

        *out_width = (i32)width;
        *out_height = (i32)height;
        return rgba;
    }
}

#endif /* PNG_DEC_IMPLEMENTATION_ */
#endif /* PNG_DEC_IMPLEMENTATION */
