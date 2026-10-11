#ifndef HOLYC_WAV_H_
#define HOLYC_WAV_H_

/* From scratch - not ported from any vendored library. WAV (RIFF/WAVE) is
 * simple enough (a RIFF container, a "fmt " chunk, a "data" chunk of raw
 * interleaved PCM) not to need one; unlike mp3_dec.h (real MP3 decoding is
 * a multi-week DSP-correctness effort not worth attempting fresh when a
 * mature decoder exists to port instead), getting this right from the
 * RIFF spec directly is entirely reasonable.
 *
 * Unrecognized chunks (LIST, fact, cue, ...) are skipped by their
 * declared size rather than rejected - real-world WAV files routinely
 * carry metadata chunks a minimal reader has no reason to understand.
 *
 * Encode and decode live in this one file (was wav_dec.h/wav_enc.h,
 * merged back) - each still stands alone
 * under its OWN IMPLEMENTATION guard macro (WAV_DEC_IMPLEMENTATION/
 * WAV_ENC_IMPLEMENTATION, unchanged from the split files), they just
 * share Wav_Error/Wav_Format/Wav_Audio/wav_strerror since both need the
 * exact same vocabulary to describe "what a WAV file's audio looks
 * like" rather than duplicating those definitions. */

#include "core/base.h"
#include "core/arena_alloc.h"

typedef enum {
    WAV_ESUCCESS      =  0,
    WAV_EBADRIFF      = -1, /* missing "RIFF"/"WAVE" magic */
    WAV_ENOFMT        = -2, /* no "fmt " chunk before "data" */
    WAV_ENODATA       = -3, /* no "data" chunk found */
    WAV_ETRUNCATED    = -4, /* a chunk's declared size runs past the buffer */
    WAV_EUNSUPPORTED  = -5  /* a format_tag/bits_per_sample combination this reader doesn't handle */
} Wav_Error;

typedef enum {
    WAV_FORMAT_PCM        = 1,
    WAV_FORMAT_IEEE_FLOAT = 3
} Wav_Format;

typedef struct {
    u32 sample_rate;
    u16 channels;
    u16 bits_per_sample;
    u16 format;         /* Wav_Format */
    size_t frame_count;  /* samples per channel, not total sample count */
    void *samples;        /* arena-owned, interleaved: frame0[ch0,ch1,...], frame1[...], ...
                             raw bytes at bits_per_sample width - i16* for 16-bit PCM,
                             f32* for 32-bit IEEE float, u8* for 8-bit PCM */
} Wav_Audio;

/* supports 8/16-bit PCM and 32-bit IEEE float, the three overwhelmingly
 * common cases; 24-bit PCM is a documented gap (WAV_EUNSUPPORTED) - its
 * 3-byte-per-sample packing doesn't map onto any of this project's fixed
 * integer types without a distinct unpacking path this file doesn't have
 * yet. */
i32 wav_decode(Arena *a, const u8 *data, size_t size, Wav_Audio *out);

/* writes a minimal valid WAV (RIFF + "fmt " + "data", no extra chunks)
 * into an arena-owned buffer. audio->samples must already be in the
 * target format/width described by the other fields - this does not
 * convert. */
i32 wav_encode(Arena *a, const Wav_Audio *audio, u8 **out_data, size_t *out_size);

const char *wav_strerror(i32 err);

#endif /* HOLYC_WAV_H_ */

#ifdef WAV_DEC_IMPLEMENTATION
#ifndef WAV_DEC_IMPLEMENTATION_
#define WAV_DEC_IMPLEMENTATION_


static u32 wav_read_u32le_(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u16 wav_read_u16le_(const u8 *p)
{
    return (u16)((u32)p[0] | ((u32)p[1] << 8));
}

#define WAV_FOURCC_(c0, c1, c2, c3) \
    ((u32)(u8)(c0) | ((u32)(u8)(c1) << 8) | ((u32)(u8)(c2) << 16) | ((u32)(u8)(c3) << 24))

const char *wav_strerror(i32 err)
{
    switch (err) {
    case WAV_ESUCCESS:     return "success";
    case WAV_EBADRIFF:     return "not a RIFF/WAVE file";
    case WAV_ENOFMT:       return "missing fmt chunk";
    case WAV_ENODATA:      return "missing data chunk";
    case WAV_ETRUNCATED:   return "chunk runs past end of buffer";
    case WAV_EUNSUPPORTED: return "unsupported format/bit depth combination";
    default: break;
    }
    return "unknown error";
}

i32 wav_decode(Arena *a, const u8 *data, size_t size, Wav_Audio *out)
{
    size_t pos;
    i32 have_fmt = 0;
    u16 format_tag = 0;

    memset(out, 0, sizeof(*out));

    if (size < 12) return WAV_EBADRIFF;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return WAV_EBADRIFF;

    pos = 12;
    while (pos + 8 <= size) {
        u32 chunk_id;
        u32 chunk_size;
        size_t chunk_data;

        chunk_id = wav_read_u32le_(data + pos);
        chunk_size = wav_read_u32le_(data + pos + 4);
        chunk_data = pos + 8;

        if (chunk_data + (size_t)chunk_size > size) return WAV_ETRUNCATED;

        switch (chunk_id) {
        case WAV_FOURCC_('f', 'm', 't', ' '):
            if (chunk_size < 16) return WAV_EUNSUPPORTED;
            format_tag = wav_read_u16le_(data + chunk_data);
            out->channels = wav_read_u16le_(data + chunk_data + 2);
            out->sample_rate = wav_read_u32le_(data + chunk_data + 4);
            out->bits_per_sample = wav_read_u16le_(data + chunk_data + 14);
            out->format = format_tag;
            have_fmt = 1;
            break;
        case WAV_FOURCC_('d', 'a', 't', 'a'): {
            size_t bytes_per_frame;
            void *dst;

            if (!have_fmt) return WAV_ENOFMT;

            if (!((format_tag == WAV_FORMAT_PCM && (out->bits_per_sample == 8 || out->bits_per_sample == 16)) ||
                  (format_tag == WAV_FORMAT_IEEE_FLOAT && out->bits_per_sample == 32))) {
                return WAV_EUNSUPPORTED;
            }

            bytes_per_frame = (size_t)out->channels * (out->bits_per_sample / 8);
            if (bytes_per_frame == 0) return WAV_EUNSUPPORTED;

            out->frame_count = (size_t)chunk_size / bytes_per_frame;
            dst = arena_alloc(a, (size_t)chunk_size);
            memcpy(dst, data + chunk_data, (size_t)chunk_size);
            out->samples = dst;

            return WAV_ESUCCESS; /* data is conventionally the last chunk read to care about */
        }
        default:
            break;
        }

        /* RIFF chunks are padded to an even byte boundary */
        pos = chunk_data + (size_t)chunk_size + (chunk_size & 1);
    }

    return have_fmt ? WAV_ENODATA : WAV_ENOFMT;
}

#endif /* WAV_DEC_IMPLEMENTATION_ */
#endif /* WAV_DEC_IMPLEMENTATION */

#ifdef WAV_ENC_IMPLEMENTATION
#ifndef WAV_ENC_IMPLEMENTATION_
#define WAV_ENC_IMPLEMENTATION_


static void wav_write_u32le_(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF); p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF); p[3] = (u8)((v >> 24) & 0xFF);
}

static void wav_write_u16le_(u8 *p, u16 v)
{
    p[0] = (u8)(v & 0xFF); p[1] = (u8)((v >> 8) & 0xFF);
}

i32 wav_encode(Arena *a, const Wav_Audio *audio, u8 **out_data, size_t *out_size)
{
    size_t bytes_per_frame = (size_t)audio->channels * (audio->bits_per_sample / 8);
    size_t data_bytes = audio->frame_count * bytes_per_frame;
    size_t total = 12 + 8 + 16 + 8 + data_bytes;
    u8 *buf;
    u32 byte_rate = audio->sample_rate * (u32)bytes_per_frame;
    u16 block_align = (u16)bytes_per_frame;

    if (bytes_per_frame == 0) return WAV_EUNSUPPORTED;

    buf = (u8 *)arena_alloc(a, total);

    memcpy(buf, "RIFF", 4);
    wav_write_u32le_(buf + 4, (u32)(total - 8));
    memcpy(buf + 8, "WAVE", 4);

    memcpy(buf + 12, "fmt ", 4);
    wav_write_u32le_(buf + 16, 16);
    wav_write_u16le_(buf + 20, audio->format);
    wav_write_u16le_(buf + 22, audio->channels);
    wav_write_u32le_(buf + 24, audio->sample_rate);
    wav_write_u32le_(buf + 28, byte_rate);
    wav_write_u16le_(buf + 32, block_align);
    wav_write_u16le_(buf + 34, audio->bits_per_sample);

    memcpy(buf + 36, "data", 4);
    wav_write_u32le_(buf + 40, (u32)data_bytes);
    memcpy(buf + 44, audio->samples, data_bytes);

    *out_data = buf;
    *out_size = total;
    return WAV_ESUCCESS;
}

#endif /* WAV_ENC_IMPLEMENTATION_ */
#endif /* WAV_ENC_IMPLEMENTATION */
