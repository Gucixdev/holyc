#ifndef HOLYC_QOI_CODEC_H_
#define HOLYC_QOI_CODEC_H_

/* QOI ("Quite OK Image") encoder+decoder, from scratch against Dominic
 * Szablewski's public format spec (qoiformat.org) - a small, fully
 * documented format, not worth vendoring a reference implementation for
 * (same reasoning wav.h/png_dec.h give for going straight from spec).
 *
 * Named qoi_codec.h, not qoi.h: this machine has a REAL system header at
 * /usr/include/qoi.h (the reference qoi.h single-header library,
 * installed as a package) - a project file literally named qoi.h would
 * silently shadow it for any `#include <qoi.h>` anywhere in the include
 * path (this project's ./build adds every src/ subdirectory to -I, and
 * GCC's -I applies to <angle-bracket> includes too - the exact hazard
 * tinytar.h's own rename fixed for <tar.h>, and ./check's name-hygiene
 * section now catches automatically). Encode and decode live in this one
 * file (was qoi_dec.h/qoi_enc.h, merged back) - each still stands alone under its OWN IMPLEMENTATION
 * guard macro (QOI_DEC_IMPLEMENTATION/QOI_ENC_IMPLEMENTATION, unchanged
 * from the split files), and each keeps its own distinctly-named copy of
 * the shared hash helper (qoi_hash_/qoi_enc_hash_ - NOT a cross-file
 * static reference) so either side still compiles standalone under only
 * its own IMPLEMENTATION macro, and both macros can be defined together
 * in one translation unit (e.g. test_qoi.c) without a duplicate-symbol
 * error. */

#include "types/base.h"
#include "alloc/arena_alloc.h"

typedef enum {
    QOI_ESUCCESS     =  0,
    QOI_EBADMAGIC    = -1, /* missing "qoif" magic */
    QOI_EBADCHANNELS = -2, /* channels byte is neither 3 (RGB) nor 4 (RGBA) */
    QOI_ETRUNCATED   = -3  /* pixel stream ends before width*height pixels decoded */
} Qoi_Error;

typedef struct {
    u32 width;
    u32 height;
    u8 channels;    /* 3 (RGB) or 4 (RGBA) - matches the source file, not upsampled */
    u8 colorspace;  /* 0 = sRGB with linear alpha, 1 = all linear - passed through, not interpreted */
    u8 *pixels;     /* arena-owned, [width*height*channels] bytes, row-major, no padding */
} Qoi_Image;

i32 qoi_decode(Arena *a, const u8 *data, size_t size, Qoi_Image *out);
const char *qoi_strerror(i32 err);

/* channels must be 3 (RGB) or 4 (RGBA), matching pixels' own layout - no
 * implicit RGB->RGBA upsampling or vice versa. colorspace is passed
 * through into the file header uninterpreted, same as the decoder reads
 * it out uninterpreted. Returns the arena-owned encoded byte count via
 * *out_size; 0 return value alongside a non-null return means allocation
 * of a zero-pixel image, not failure - encode has no error path other
 * than a bad channel count (mirrors qoi_decode's own Qoi_Error shape for
 * that one case). */
i32 qoi_encode(Arena *a, const u8 *pixels, u32 width, u32 height, u8 channels, u8 colorspace,
               u8 **out_data, size_t *out_size);

#endif /* HOLYC_QOI_CODEC_H_ */

#ifdef QOI_DEC_IMPLEMENTATION
#ifndef QOI_DEC_IMPLEMENTATION_
#define QOI_DEC_IMPLEMENTATION_

/* running-array index: the same hash every QOI encoder/decoder uses,
 * part of the format itself (not a design choice) - both sides must
 * compute it identically for QOI_OP_INDEX to round-trip. */
static u32 qoi_hash_(u8 r, u8 g, u8 b, u8 a)
{
    return ((u32)r * 3u + (u32)g * 5u + (u32)b * 7u + (u32)a * 11u) % 64u;
}

static u32 qoi_read_u32be_(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

const char *qoi_strerror(i32 err)
{
    switch (err) {
    case QOI_ESUCCESS:     return "success";
    case QOI_EBADMAGIC:    return "not a QOI file (bad magic)";
    case QOI_EBADCHANNELS: return "channels byte is neither 3 nor 4";
    case QOI_ETRUNCATED:   return "pixel stream ends before width*height pixels decoded";
    default: break;
    }
    return "unknown error";
}

i32 qoi_decode(Arena *a, const u8 *data, size_t size, Qoi_Image *out)
{
    u8 index[64][4]; /* running array of the last 64 distinct pixels seen, keyed by qoi_hash_ */
    u8 px[4];
    size_t pos, pixel_count, i;
    u8 *pixels;

    memset(out, 0, sizeof(*out));
    memset(index, 0, sizeof(index));

    if (size < 14 || memcmp(data, "qoif", 4) != 0) return QOI_EBADMAGIC;

    out->width = qoi_read_u32be_(data + 4);
    out->height = qoi_read_u32be_(data + 8);
    out->channels = data[12];
    out->colorspace = data[13];

    if (out->channels != 3 && out->channels != 4) return QOI_EBADCHANNELS;

    pixel_count = (size_t)out->width * (size_t)out->height;
    pixels = (u8 *)arena_alloc(a, pixel_count * (size_t)out->channels);
    out->pixels = pixels;

    px[0] = 0; px[1] = 0; px[2] = 0; px[3] = 255;
    pos = 14;

    for (i = 0; i < pixel_count; i++) {
        if (pos < size && data[pos] == 0xFE) {
            /* QOI_OP_RGB: tag + r,g,b, alpha carried over unchanged */
            if (pos + 4 > size) return QOI_ETRUNCATED;
            px[0] = data[pos + 1]; px[1] = data[pos + 2]; px[2] = data[pos + 3];
            pos += 4;
        } else if (pos < size && data[pos] == 0xFF) {
            /* QOI_OP_RGBA: tag + r,g,b,a */
            if (pos + 5 > size) return QOI_ETRUNCATED;
            px[0] = data[pos + 1]; px[1] = data[pos + 2]; px[2] = data[pos + 3]; px[3] = data[pos + 4];
            pos += 5;
        } else {
            u8 tag, tag2;

            if (pos >= size) return QOI_ETRUNCATED;
            tag = (u8)(data[pos] >> 6);

            if (tag == 0) {
                /* QOI_OP_INDEX: 6-bit index into the running array */
                u8 idx = (u8)(data[pos] & 0x3F);
                px[0] = index[idx][0]; px[1] = index[idx][1];
                px[2] = index[idx][2]; px[3] = index[idx][3];
                pos += 1;
            } else if (tag == 1) {
                /* QOI_OP_DIFF: 2-bit r/g/b diffs, biased by 2 (range -2..1) */
                px[0] = (u8)(px[0] + (((data[pos] >> 4) & 0x03) - 2));
                px[1] = (u8)(px[1] + (((data[pos] >> 2) & 0x03) - 2));
                px[2] = (u8)(px[2] + ((data[pos] & 0x03) - 2));
                pos += 1;
            } else if (tag == 2) {
                /* QOI_OP_LUMA: 6-bit green diff (bias 32) + a second byte
                 * with 4-bit dr-dg / db-dg (bias 8 each) */
                i32 dg;
                if (pos + 2 > size) return QOI_ETRUNCATED;
                dg = (i32)(data[pos] & 0x3F) - 32;
                tag2 = data[pos + 1];
                px[0] = (u8)(px[0] + dg + (((i32)(tag2 >> 4) & 0x0F) - 8));
                px[1] = (u8)(px[1] + dg);
                px[2] = (u8)(px[2] + dg + (((i32)(tag2 & 0x0F)) - 8));
                pos += 2;
            } else {
                /* QOI_OP_RUN: 6-bit run length, biased by -1 (range 1..62 -
                 * 63/64 are reserved, they're the QOI_OP_RGB/RGBA tags) */
                u32 run = (u32)(data[pos] & 0x3F) + 1;
                u32 idx = qoi_hash_(px[0], px[1], px[2], px[3]);
                pos += 1;

                index[idx][0] = px[0]; index[idx][1] = px[1];
                index[idx][2] = px[2]; index[idx][3] = px[3];

                while (run > 0 && i < pixel_count) {
                    pixels[i * out->channels + 0] = px[0];
                    pixels[i * out->channels + 1] = px[1];
                    pixels[i * out->channels + 2] = px[2];
                    if (out->channels == 4) pixels[i * out->channels + 3] = px[3];
                    i++;
                    run--;
                }
                i--; /* the outer for-loop's own i++ accounts for the last pixel of this run */
                continue;
            }
        }

        {
            u32 idx = qoi_hash_(px[0], px[1], px[2], px[3]);
            index[idx][0] = px[0]; index[idx][1] = px[1];
            index[idx][2] = px[2]; index[idx][3] = px[3];
        }

        pixels[i * out->channels + 0] = px[0];
        pixels[i * out->channels + 1] = px[1];
        pixels[i * out->channels + 2] = px[2];
        if (out->channels == 4) pixels[i * out->channels + 3] = px[3];
    }

    return QOI_ESUCCESS;
}

#endif /* QOI_DEC_IMPLEMENTATION_ */
#endif /* QOI_DEC_IMPLEMENTATION */

#ifdef QOI_ENC_IMPLEMENTATION
#ifndef QOI_ENC_IMPLEMENTATION_
#define QOI_ENC_IMPLEMENTATION_

static void qoi_write_u32be_(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

/* own local copy of the decoder's hash function, under its OWN name
 * (qoi_enc_hash_, not qoi_hash_) - deliberately NOT shared via
 * cross-file static reference: the decoder's copy only exists when
 * QOI_DEC_IMPLEMENTATION is defined, and this block must stand alone
 * under QOI_ENC_IMPLEMENTATION only (same independence wav.h keeps
 * between WAV_DEC_IMPLEMENTATION/WAV_ENC_IMPLEMENTATION's own read/write
 * helper pairs - two distinctly-named statics, not one shared name). A
 * distinct name also avoids a duplicate-symbol error when a caller (e.g.
 * this project's own test_qoi.c) defines BOTH IMPLEMENTATION macros in
 * the same translation unit. Both copies must compute identically -
 * it's part of the QOI format itself, not an implementation choice
 * either side could vary. */
static u32 qoi_enc_hash_(u8 r, u8 g, u8 b, u8 a)
{
    return ((u32)r * 3u + (u32)g * 5u + (u32)b * 7u + (u32)a * 11u) % 64u;
}

i32 qoi_encode(Arena *a, const u8 *pixels, u32 width, u32 height, u8 channels, u8 colorspace,
               u8 **out_data, size_t *out_size)
{
    u8 index[64][4];
    u8 px[4], px_prev[4];
    size_t pixel_count, i, pos;
    /* worst case: every pixel becomes a QOI_OP_RGBA (5 bytes) - header
     * (14) + worst-case pixel stream + 8-byte end marker is a safe upper
     * bound to allocate once, rather than a growable buffer for a format
     * whose max expansion is this cheaply known up front. */
    size_t cap;
    u8 *buf;
    u32 run;

    if (channels != 3 && channels != 4) return QOI_EBADCHANNELS;

    pixel_count = (size_t)width * (size_t)height;
    cap = 14 + pixel_count * 5 + 8;
    buf = (u8 *)arena_alloc(a, cap);

    memcpy(buf, "qoif", 4);
    qoi_write_u32be_(buf + 4, width);
    qoi_write_u32be_(buf + 8, height);
    buf[12] = channels;
    buf[13] = colorspace;
    pos = 14;

    memset(index, 0, sizeof(index));
    px_prev[0] = 0; px_prev[1] = 0; px_prev[2] = 0; px_prev[3] = 255;
    run = 0;

    for (i = 0; i < pixel_count; i++) {
        px[0] = pixels[i * channels + 0];
        px[1] = pixels[i * channels + 1];
        px[2] = pixels[i * channels + 2];
        px[3] = (channels == 4) ? pixels[i * channels + 3] : px_prev[3];

        if (px[0] == px_prev[0] && px[1] == px_prev[1] && px[2] == px_prev[2] && px[3] == px_prev[3]) {
            run++;
            if (run == 62 || i == pixel_count - 1) {
                buf[pos++] = (u8)(0xC0 | (run - 1));
                run = 0;
            }
        } else {
            u32 idx;

            if (run > 0) {
                buf[pos++] = (u8)(0xC0 | (run - 1));
                run = 0;
            }

            idx = qoi_enc_hash_(px[0], px[1], px[2], px[3]);

            if (index[idx][0] == px[0] && index[idx][1] == px[1] &&
                index[idx][2] == px[2] && index[idx][3] == px[3]) {
                buf[pos++] = (u8)idx; /* QOI_OP_INDEX - top 2 bits already 0 */
            } else {
                index[idx][0] = px[0]; index[idx][1] = px[1];
                index[idx][2] = px[2]; index[idx][3] = px[3];

                if (px[3] == px_prev[3]) {
                    i32 dr = (i32)px[0] - (i32)px_prev[0];
                    i32 dg = (i32)px[1] - (i32)px_prev[1];
                    i32 db = (i32)px[2] - (i32)px_prev[2];
                    i32 dr_dg = dr - dg;
                    i32 db_dg = db - dg;

                    if (dr >= -2 && dr <= 1 && dg >= -2 && dg <= 1 && db >= -2 && db <= 1) {
                        buf[pos++] = (u8)(0x40 | ((dr + 2) << 4) | ((dg + 2) << 2) | (db + 2));
                    } else if (dg >= -32 && dg <= 31 && dr_dg >= -8 && dr_dg <= 7 && db_dg >= -8 && db_dg <= 7) {
                        buf[pos++] = (u8)(0x80 | (dg + 32));
                        buf[pos++] = (u8)(((dr_dg + 8) << 4) | (db_dg + 8));
                    } else {
                        buf[pos++] = 0xFE;
                        buf[pos++] = px[0]; buf[pos++] = px[1]; buf[pos++] = px[2];
                    }
                } else {
                    buf[pos++] = 0xFF;
                    buf[pos++] = px[0]; buf[pos++] = px[1]; buf[pos++] = px[2]; buf[pos++] = px[3];
                }
            }
        }

        px_prev[0] = px[0]; px_prev[1] = px[1]; px_prev[2] = px[2]; px_prev[3] = px[3];
    }

    memset(buf + pos, 0, 7);
    buf[pos + 7] = 0x01;
    pos += 8;

    *out_data = buf;
    *out_size = pos;
    return QOI_ESUCCESS;
}

#endif /* QOI_ENC_IMPLEMENTATION_ */
#endif /* QOI_ENC_IMPLEMENTATION */
