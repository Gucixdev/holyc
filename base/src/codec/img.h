#ifndef HOLYC_IMG_H_
#define HOLYC_IMG_H_

/* Thin format-dispatching image loader over the decoders this project
 * already has (png_dec.h, jpeg_dec.h) plus one implemented directly here:
 * uncompressed Windows BMP. Not a decoder itself for PNG/JPEG - just
 * magic-byte sniffing + a call to the existing decode function,
 * normalized into one common result shape so a caller that doesn't know
 * (or care) which of the three formats a buffer holds can just call
 * img_decode() once.
 *
 * BMP is implemented here rather than as its own img/bmp_dec.h file: it
 * is simple enough (no compression to reason about - BI_RGB only, the
 * documented gap below) that a separate file would be more boilerplate
 * (its own IMPLEMENTATION guard, its own header comment) than code, and
 * unlike PNG/JPEG there is no other consumer for a standalone BMP
 * decoder anywhere else in this project.
 *
 * Scope, stated plainly: BMP support is BI_RGB (uncompressed) 24-bit
 * (BGR) and 32-bit (BGRA) only, top-down or bottom-up row order, no
 * palette (1/4/8-bit indexed) support, no RLE compression (BI_RLE4/
 * BI_RLE8), no BITMAPV4HEADER/BITMAPV5HEADER extended fields - all
 * documented gaps, not silently missing, same convention png_dec.h and
 * jpeg_dec.h already use for their own scope cuts. */

#include "core/base.h"
#include "core/arena_alloc.h"

typedef enum {
    IMG_FORMAT_UNKNOWN = 0,
    IMG_FORMAT_PNG     = 1,
    IMG_FORMAT_JPEG    = 2,
    IMG_FORMAT_BMP     = 3
} Img_Format;

typedef struct {
    i32 width;
    i32 height;
    i32 channels;   /* always 4 (RGBA8) - one consistent pixel format,
                       matching png_dec.h/jpeg_dec.h's own convention */
    u8 *pixels;     /* arena-owned, width*height*4 bytes, row-major, no padding */
} Img_Result;

/* sniffs `data`'s magic bytes and returns which format img_decode()
 * would attempt, without decoding anything - useful for a caller that
 * wants to reject/accept by format before paying for a full decode. */
Img_Format img_detect_format(const u8 *data, size_t size);

/* decodes any of PNG/JPEG/BMP into a common RGBA8 result. Returns 1 on
 * success (out is filled in, arena-owned), 0 on any parse/format error,
 * unrecognized magic bytes, or an unsupported feature within a
 * recognized format (see png_dec.h/jpeg_dec.h/this file's own BMP scope
 * notes above for what "unsupported" can mean per format). */
i32 img_decode(Arena *a, const u8 *data, size_t size, Img_Result *out);

#endif /* HOLYC_IMG_H_ */

#ifdef IMG_IMPLEMENTATION
#ifndef IMG_IMPLEMENTATION_
#define IMG_IMPLEMENTATION_

#include "codec/png_dec.h"
#include "codec/jpeg_dec.h"

static u32 img_read_u32le_(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 img_read_u16le_(const u8 *p)
{
    return (u16)((u32)p[0] | ((u32)p[1] << 8));
}

static i32 img_read_i32le_(const u8 *p)
{
    return (i32)img_read_u32le_(p);
}

Img_Format img_detect_format(const u8 *data, size_t size)
{
    static const u8 png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

    if (size >= 8 && memcmp(data, png_sig, 8) == 0) return IMG_FORMAT_PNG;
    if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) return IMG_FORMAT_JPEG;
    if (size >= 2 && data[0] == 'B' && data[1] == 'M') return IMG_FORMAT_BMP;
    return IMG_FORMAT_UNKNOWN;
}

/* BI_RGB uncompressed 24/32-bit BMP only - see this file's own top
 * comment for the full, deliberate list of what is NOT supported. */
static i32 img_decode_bmp_(Arena *a, const u8 *data, size_t size, Img_Result *out)
{
    u32 data_offset;
    i32 header_size;
    i32 width;
    i32 height;
    u16 bpp;
    u32 compression;
    i32 flip;
    u32 row_bytes_src;
    u32 row_bytes_padded;
    i32 bytes_per_pixel;
    u8 *pixels;
    i32 y;

    if (size < 14 + 4) return 0;
    if (data[0] != 'B' || data[1] != 'M') return 0;

    data_offset = img_read_u32le_(data + 10);
    header_size = img_read_i32le_(data + 14);

    /* only the classic BITMAPINFOHEADER (40 bytes) layout is understood -
       BITMAPV4/V5HEADER (108/124 bytes) put extra fields after it, which
       are simply ignored if present since width/height/bpp/compression
       sit at the same offsets in all of them, but the extended color-
       space/gamma fields themselves are a documented gap. */
    if (header_size < 40) return 0;
    if (size < 14u + (u32)header_size) return 0;

    width  = img_read_i32le_(data + 18);
    height = img_read_i32le_(data + 22);
    bpp        = img_read_u16le_(data + 28);
    compression = img_read_u32le_(data + 30);

    if (width <= 0) return 0;
    if (height == 0) return 0;
    if (compression != 0) return 0; /* BI_RGB only - no RLE, no bitfields */
    if (bpp != 24 && bpp != 32) return 0; /* no palette formats */

    flip = 0;
    if (height < 0) {
        flip = 1; /* negative height means top-down row order already */
        height = -height;
    }

    bytes_per_pixel = (i32)(bpp / 8);
    row_bytes_src = (u32)width * (u32)bytes_per_pixel;
    row_bytes_padded = (row_bytes_src + 3u) & ~3u; /* rows padded to a 4-byte boundary */

    if ((u64)data_offset + (u64)row_bytes_padded * (u64)height > (u64)size) return 0;

    pixels = (u8 *)arena_alloc(a, (size_t)width * (size_t)height * 4u);

    for (y = 0; y < height; y++) {
        i32 src_row = flip ? y : (height - 1 - y); /* BMP rows are bottom-up unless height was negative */
        const u8 *row = data + data_offset + (size_t)src_row * row_bytes_padded;
        u8 *dst_row = pixels + (size_t)y * (size_t)width * 4u;
        i32 x;

        for (x = 0; x < width; x++) {
            const u8 *px = row + (size_t)x * (size_t)bytes_per_pixel;
            u8 *d = dst_row + (size_t)x * 4u;
            d[0] = px[2]; /* B,G,R -> R,G,B */
            d[1] = px[1];
            d[2] = px[0];
            d[3] = (bytes_per_pixel == 4) ? px[3] : 0xFFu;
        }
    }

    out->width = width;
    out->height = height;
    out->channels = 4;
    out->pixels = pixels;
    return 1;
}

i32 img_decode(Arena *a, const u8 *data, size_t size, Img_Result *out)
{
    Img_Format fmt = img_detect_format(data, size);

    out->width = 0;
    out->height = 0;
    out->channels = 0;
    out->pixels = NULL;

    switch (fmt) {
    case IMG_FORMAT_PNG:
        out->pixels = png_decode(a, data, size, &out->width, &out->height);
        if (out->pixels == NULL) return 0;
        out->channels = 4;
        return 1;
    case IMG_FORMAT_JPEG:
        out->pixels = jpeg_decode(a, data, size, &out->width, &out->height);
        if (out->pixels == NULL) return 0;
        out->channels = 4;
        return 1;
    case IMG_FORMAT_BMP:
        return img_decode_bmp_(a, data, size, out);
    case IMG_FORMAT_UNKNOWN:
    default:
        return 0;
    }
}

#endif /* IMG_IMPLEMENTATION_ */
#endif /* IMG_IMPLEMENTATION */
