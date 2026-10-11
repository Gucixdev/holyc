#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define WAV_DEC_IMPLEMENTATION
#define WAV_ENC_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "codec/wav.h"

#include <string.h>

int main(void)
{
    Arena a = {0};

    print_cstr("test_wav\n");

    /* round-trip: 16-bit PCM stereo */
    {
        i16 src[8] = { -32768, 32767, 0, 1, -1, 12345, -12345, 100 };
        Wav_Audio audio;
        u8 *bytes;
        size_t nbytes;
        Wav_Audio decoded;
        i32 err;
        i32 ok;
        size_t i;

        memset(&audio, 0, sizeof(audio));
        audio.sample_rate = 44100;
        audio.channels = 2;
        audio.bits_per_sample = 16;
        audio.format = WAV_FORMAT_PCM;
        audio.frame_count = 4; /* 8 i16 samples / 2 channels */
        audio.samples = src;

        err = wav_encode(&a, &audio, &bytes, &nbytes);
        TEST_CHECK(err == WAV_ESUCCESS, "encode: 16-bit PCM stereo succeeds");

        err = wav_decode(&a, bytes, nbytes, &decoded);
        TEST_CHECK(err == WAV_ESUCCESS, "decode: round-tripped bytes decode successfully");
        TEST_CHECK(decoded.sample_rate == 44100 && decoded.channels == 2 &&
                   decoded.bits_per_sample == 16 && decoded.format == WAV_FORMAT_PCM,
                   "decode: format fields match what was encoded");
        TEST_CHECK(decoded.frame_count == 4, "decode: frame_count matches");

        ok = 1;
        for (i = 0; i < 8; i++) if (((i16 *)decoded.samples)[i] != src[i]) ok = 0;
        TEST_CHECK(ok, "decode: every sample round-trips exactly (lossless format)");
    }

    /* round-trip: 32-bit float mono */
    {
        f32 src[5] = { -1.0f, 1.0f, 0.0f, 0.333333f, -0.5f };
        Wav_Audio audio;
        u8 *bytes;
        size_t nbytes;
        Wav_Audio decoded;
        i32 err;
        i32 ok;
        size_t i;

        memset(&audio, 0, sizeof(audio));
        audio.sample_rate = 48000;
        audio.channels = 1;
        audio.bits_per_sample = 32;
        audio.format = WAV_FORMAT_IEEE_FLOAT;
        audio.frame_count = 5;
        audio.samples = src;

        err = wav_encode(&a, &audio, &bytes, &nbytes);
        TEST_CHECK(err == WAV_ESUCCESS, "encode: 32-bit float mono succeeds");

        err = wav_decode(&a, bytes, nbytes, &decoded);
        TEST_CHECK(err == WAV_ESUCCESS, "decode: float round-trip decodes successfully");

        ok = 1;
        for (i = 0; i < 5; i++) if (((f32 *)decoded.samples)[i] != src[i]) ok = 0;
        TEST_CHECK(ok, "decode: every float sample round-trips exactly");
    }

    /* hand-crafted minimal valid WAV, not from our own encoder - 2 frames,
     * mono, 8-bit PCM, values 0x00 and 0xFF chosen to be unambiguous */
    {
        u8 raw[] = {
            'R','I','F','F', 28,0,0,0, 'W','A','V','E',
            'f','m','t',' ', 16,0,0,0,
            1,0,             /* format_tag = PCM */
            1,0,             /* channels = 1 */
            0x44,0xAC,0,0,   /* sample_rate = 44100 */
            0x44,0xAC,0,0,   /* byte_rate (mono, 8-bit -> same as sample_rate) */
            1,0,             /* block_align */
            8,0,             /* bits_per_sample */
            'd','a','t','a', 2,0,0,0,
            0x00, 0xFF
        };
        Wav_Audio decoded;
        i32 err = wav_decode(&a, raw, sizeof(raw), &decoded);

        TEST_CHECK(err == WAV_ESUCCESS, "decode: hand-crafted minimal WAV parses successfully");
        TEST_CHECK(decoded.sample_rate == 44100 && decoded.channels == 1 && decoded.bits_per_sample == 8,
                   "decode: hand-crafted WAV's format fields read correctly");
        TEST_CHECK(decoded.frame_count == 2 &&
                   ((u8 *)decoded.samples)[0] == 0x00 && ((u8 *)decoded.samples)[1] == 0xFF,
                   "decode: hand-crafted WAV's sample data reads correctly");
    }

    /* an unrecognized chunk (LIST) between fmt and data must be skipped,
     * not choke the parser - this is the realistic case, not an edge case */
    {
        u8 raw[] = {
            'R','I','F','F', 40,0,0,0, 'W','A','V','E',
            'f','m','t',' ', 16,0,0,0,
            1,0, 1,0, 0x44,0xAC,0,0, 0x44,0xAC,0,0, 1,0, 8,0,
            'L','I','S','T', 4,0,0,0, 'I','N','F','O', /* unrecognized, must be skipped */
            'd','a','t','a', 2,0,0,0,
            0x7F, 0x80
        };
        Wav_Audio decoded;
        i32 err = wav_decode(&a, raw, sizeof(raw), &decoded);

        TEST_CHECK(err == WAV_ESUCCESS, "decode: an unrecognized chunk before data is skipped, not fatal");
        TEST_CHECK(decoded.frame_count == 2 &&
                   ((u8 *)decoded.samples)[0] == 0x7F && ((u8 *)decoded.samples)[1] == 0x80,
                   "decode: data after a skipped chunk still reads correctly");
    }

    /* malformed input: not a RIFF file at all - must return an error, not crash */
    {
        u8 garbage[16] = { 0 };
        Wav_Audio decoded;
        i32 err = wav_decode(&a, garbage, sizeof(garbage), &decoded);
        TEST_CHECK(err == WAV_EBADRIFF, "decode: non-RIFF input is rejected cleanly");
    }

    /* malformed input: a chunk size that claims more data than the buffer holds */
    {
        u8 raw[] = {
            'R','I','F','F', 100,0,0,0, 'W','A','V','E',
            'f','m','t',' ', 16,0,0,0,
            1,0, 1,0, 0x44,0xAC,0,0, 0x44,0xAC,0,0, 1,0, 8,0,
            'd','a','t','a', 0xFF,0xFF,0,0 /* claims 65535 bytes, buffer has none */
        };
        Wav_Audio decoded;
        i32 err = wav_decode(&a, raw, sizeof(raw), &decoded);
        TEST_CHECK(err == WAV_ETRUNCATED, "decode: an oversized chunk-size claim is rejected, not read out of bounds");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
