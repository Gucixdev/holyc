#ifndef HOLYC_WEBP_H_
#define HOLYC_WEBP_H_

/* WebP container + VP8L header reader, from scratch - deliberately NOT a
 * pixel decoder (stated plainly, matching this project's house style for
 * honest scope cuts - see arena_alloc.h's fp64_mul/fp64_div comment,
 * bit.h's own "not implemented here" list).
 *
 * What this reads: the RIFF container (`RIFF....WEBP` header, chunk
 * walking by FourCC+size) down to a `VP8L` chunk, then that chunk's own
 * 5-byte bitstream header (signature byte 0x2F, then a 32-bit
 * LSB-first-packed field: 14 bits width-1, 14 bits height-1, 1 bit
 * alpha-is-used, 3 bits version) - enough to answer "how big is this
 * image and does it have alpha", nothing more.
 *
 * What this explicitly does NOT read (real, deliberate scope cuts, not
 * missing-by-accident):
 *   - actual pixel data. VP8L pixel decode needs full prefix (Huffman)
 *     meta-group decoding plus 4 spatial transforms (predictor, color,
 *     subtract-green, color-indexing) applied in sequence - genuinely
 *     one of the more involved image formats to decode correctly,
 *     comparable to (or harder than) the JPEG Huffman/IDCT work already
 *     done in src/codec/jpeg_dec.h. Worth its own dedicated pass later,
 *     not attempted here.
 *   - lossy WebP (`VP8 ` chunks - a completely different, VP8-video-
 *     codec-derived bitstream, unrelated to VP8L despite the name).
 *   - the extended `VP8X` container (alpha/animation/ICC/EXIF/XMP -
 *     `webp_decode` reports WEBP_EUNSUPPORTED for these rather than
 *     guessing at a subset). */

#include "base.h"

/* own local memset_/memcmp_ - own copy per this project's standing "no
 * cross-IMPLEMENTATION-guard dependency" rule (see str.h's own
 * sv_strlen_/sv_memcmp_ for the same pattern), avoiding a <string.h>
 * dependency for two small byte loops. */
static void *webp_memset_(void *dst, i32 val, size_t n)
{
    u8 *d = (u8 *)dst;
    size_t i;
    for (i = 0; i < n; i++) d[i] = (u8)val;
    return dst;
}

static i32 webp_memcmp_(const void *a, const void *b, size_t n)
{
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    size_t i;
    for (i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (i32)pa[i] - (i32)pb[i];
    }
    return 0;
}

typedef enum {
    WEBP_ESUCCESS     = 0,
    WEBP_EBADRIFF     = -1, /* missing "RIFF"/"WEBP" magic */
    WEBP_ENOVP8L      = -2, /* no VP8L chunk found (only VP8 lossy and/or VP8X extended present - see file header) */
    WEBP_ETRUNCATED   = -3, /* a chunk's declared size runs past the buffer, or the VP8L chunk is < 5 bytes */
    WEBP_EBADSIG      = -4  /* VP8L chunk found but its signature byte isn't 0x2F */
} Webp_Error;

typedef struct {
    u32 width;    /* real pixel width (already +1'd from the stored width-1 field) */
    u32 height;   /* real pixel height (already +1'd from the stored height-1 field) */
    i32 has_alpha;
    u32 version;  /* VP8L version number field - spec says always 0 for now, read anyway */
} Webp_Info;

/* only reads container+VP8L-header metadata - see file header. Returns
 * WEBP_ESUCCESS with *out filled on any valid VP8L-containing WebP file. */
i32 webp_decode_info(const u8 *data, size_t size, Webp_Info *out);
const char *webp_strerror(i32 err);

#endif /* HOLYC_WEBP_H_ */

#ifdef WEBP_IMPLEMENTATION
#ifndef WEBP_IMPLEMENTATION_
#define WEBP_IMPLEMENTATION_

static u32 webp_read_u32le_(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

#define WEBP_FOURCC_(c0, c1, c2, c3) \
    ((u32)(u8)(c0) | ((u32)(u8)(c1) << 8) | ((u32)(u8)(c2) << 16) | ((u32)(u8)(c3) << 24))

const char *webp_strerror(i32 err)
{
    switch (err) {
    case WEBP_ESUCCESS:   return "success";
    case WEBP_EBADRIFF:   return "not a RIFF/WEBP file";
    case WEBP_ENOVP8L:    return "no VP8L (lossless) chunk found - lossy VP8 and VP8X are not supported";
    case WEBP_ETRUNCATED: return "chunk runs past end of buffer";
    case WEBP_EBADSIG:    return "VP8L chunk has a bad signature byte (expected 0x2F)";
    default: break;
    }
    return "unknown error";
}

i32 webp_decode_info(const u8 *data, size_t size, Webp_Info *out)
{
    size_t pos;

    webp_memset_(out, 0, sizeof(*out));

    if (size < 12) return WEBP_EBADRIFF;
    if (webp_memcmp_(data, "RIFF", 4) != 0 || webp_memcmp_(data + 8, "WEBP", 4) != 0) return WEBP_EBADRIFF;

    pos = 12;
    while (pos + 8 <= size) {
        u32 chunk_id;
        u32 chunk_size;
        size_t chunk_data;

        chunk_id = webp_read_u32le_(data + pos);
        chunk_size = webp_read_u32le_(data + pos + 4);
        chunk_data = pos + 8;

        if (chunk_data + (size_t)chunk_size > size) return WEBP_ETRUNCATED;

        if (chunk_id == WEBP_FOURCC_('V', 'P', '8', 'L')) {
            const u8 *h = data + chunk_data;
            u32 packed;

            if (chunk_size < 5) return WEBP_ETRUNCATED;
            if (h[0] != 0x2F) return WEBP_EBADSIG;

            packed = webp_read_u32le_(h + 1);
            out->width = (packed & 0x3FFFu) + 1u;
            out->height = ((packed >> 14) & 0x3FFFu) + 1u;
            out->has_alpha = (i32)((packed >> 28) & 0x1u);
            out->version = (packed >> 29) & 0x7u;
            return WEBP_ESUCCESS;
        }

        /* RIFF chunks are padded to an even byte boundary. Any other
           chunk (VP8 , VP8X, ALPH, ANIM, ICCP, EXIF, XMP , ...) is
           skipped by its declared size - see file header for scope. */
        pos = chunk_data + (size_t)chunk_size + (chunk_size & 1);
    }

    return WEBP_ENOVP8L;
}

#endif /* WEBP_IMPLEMENTATION_ */
#endif /* WEBP_IMPLEMENTATION */
