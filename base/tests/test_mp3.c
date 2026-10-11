#include "test_common.h"
#define MP3_DEC_IMPLEMENTATION
#include "codec/mp3_dec.h"
#include "mp3_fixture.h"
#include <string.h>

/* MP3 bitstreams can't be hand-crafted byte-by-byte the way tests/test_wav.c
 * hand-crafts a minimal RIFF file - Huffman-coded frequency data, a bit
 * reservoir spanning frames, scale factors etc. have no "just type the
 * bytes" form a human can construct and land on a valid, decodable frame.
 * So instead of a synthetic fixture, tests/mp3_fixture.h embeds a real,
 * valid MP3 prefix (country.mp3, CC0, bundled with raylib's examples) and
 * this file checks it against decode results captured once via a real
 * decode run (channels/sample_rate/layer/bitrate straight from `file`,
 * exact PCM frame counts and amplitude bounds from an actual decode) -
 * an honest correctness oracle given no independent second MP3 decoder is
 * available in this project to cross-check against (unlike jpeg.h/png.h,
 * which cross-check against PIL's decode). Malformed/truncated-input
 * handling is covered the same way the other parsers (deflate.h, png.h,
 * jpeg.h, tar.h) cover it: garbage bytes and a mid-stream cut must fail
 * or stop cleanly, never crash or hang. */

#define MP3_TEST_MAX_FRAMES 70000

static f32 pcm_f32_[MP3_TEST_MAX_FRAMES * 2];
static i16 pcm_s16_[MP3_TEST_MAX_FRAMES * 2];

int main(void)
{
    print_cstr("test_mp3\n");

    /* low-level: mp3dec_decode_frame decodes exactly one frame straight
     * from the real fixture, no framing/streaming state around it */
    {
        mp3dec dec;
        mp3dec_frame_info info;
        i16 pcm[MP3_MAX_SAMPLES_PER_FRAME]; /* mp3d_sample_t is i16 by default (MP3_FLOAT_OUTPUT unset) */
        i32 frames;

        memset(&info, 0, sizeof(info));
        mp3dec_init(&dec);
        frames = mp3dec_decode_frame(&dec, mp3_fixture_data, (i32)mp3_fixture_size, pcm, &info);

        TEST_CHECK(frames > 0, "low-level: the first real MP3 frame decodes successfully");
        TEST_CHECK(info.channels == 2, "low-level: decoded frame reports 2 channels (matches source file)");
        TEST_CHECK(info.sample_rate == 44100, "low-level: decoded frame reports 44100 Hz (matches source file)");
        TEST_CHECK(info.layer == 3, "low-level: decoded frame reports Layer III");
        TEST_CHECK(info.bitrate_kbps == 128, "low-level: decoded frame reports 128 kbps (matches source file)");
        TEST_CHECK(info.frame_bytes > 0 && info.frame_bytes < (i32)mp3_fixture_size,
                   "low-level: frame_bytes is a sane in-range value");
    }

    /* low-level: a buffer with no valid frame sync anywhere must yield 0
     * decoded frames, not garbage output or a crash - passing NULL for
     * pcm is documented as valid ("skip decoding this frame") */
    {
        mp3dec dec;
        mp3dec_frame_info info;
        u8 garbage[64];
        i32 frames;
        size_t i;

        for (i = 0; i < sizeof(garbage); i++) garbage[i] = (u8)(i * 7 + 3); /* never contains 0xFF sync byte pattern */

        memset(&info, 0, sizeof(info));
        mp3dec_init(&dec);
        frames = mp3dec_decode_frame(&dec, garbage, (i32)sizeof(garbage), NULL, &info);
        TEST_CHECK(frames == 0, "low-level: non-MP3 garbage yields 0 decoded frames, not a crash");
    }

    /* high-level: full bulk decode of the real fixture - the exact PCM
     * frame count is fully deterministic (same input, same algorithm,
     * every run), so this is a real correctness assertion, not just
     * "didn't crash": 66816 frames was independently confirmed by
     * decoding this exact 24576-byte fixture with a standalone driver
     * before this file was written. */
    {
        mp3_decoder d;
        i32 ok;
        u64 total;

        ok = mp3_init_memory(&d, mp3_fixture_data, mp3_fixture_size);
        TEST_CHECK(ok, "high-level: mp3_init_memory succeeds on a real MP3 prefix");
        TEST_CHECK(d.channels == 2 && d.sample_rate == 44100,
                   "high-level: init reports the correct channels/sample_rate");

        total = mp3_read_pcm_frames_f32(&d, MP3_TEST_MAX_FRAMES, pcm_f32_);
        TEST_CHECK(total == 66816, "high-level: bulk read decodes the exact expected PCM frame count (66816)");

        {
            u64 i;
            f32 max_abs = 0.0f;
            u64 nonzero = 0;
            for (i = 0; i < total * d.channels; i++) {
                f32 a = pcm_f32_[i] < 0 ? -pcm_f32_[i] : pcm_f32_[i];
                if (a > max_abs) max_abs = a;
                if (a > 0.001f) nonzero++;
            }
            TEST_CHECK(max_abs > 0.1f && max_abs < 1.5f,
                       "high-level: decoded audio has a sane, non-silent peak amplitude");
            TEST_CHECK(nonzero > (total * d.channels) / 4,
                       "high-level: decoded audio is mostly non-silent, not all-zero/garbage");
        }
    }

    /* high-level streaming: the same fixture read through many small,
     * oddly-sized chunks (mimicking a caller pulling a player buffer's
     * worth at a time) must reach the exact same total as one bulk call -
     * proves the cross-frame bit-reservoir/state machine survives being
     * read in small pieces that don't line up with MP3 frame boundaries */
    {
        mp3_decoder d;
        u64 total = 0;
        f32 chunk[131]; /* deliberately not a multiple of any MP3 frame's PCM count */
        i32 ok = mp3_init_memory(&d, mp3_fixture_data, mp3_fixture_size);

        TEST_CHECK(ok, "streaming: mp3_init_memory succeeds (streaming setup)");
        for (;;) {
            u64 got = mp3_read_pcm_frames_f32(&d, (sizeof(chunk) / sizeof(chunk[0])) / d.channels, chunk);
            if (got == 0) break;
            total += got;
        }
        TEST_CHECK(total == 66816, "streaming: many small chunked reads reach the same exact total as one bulk read");
    }

    /* high-level: the s16 output path must decode the same exact frame
     * count as the f32 path - same underlying decode, different only in
     * the final sample representation */
    {
        mp3_decoder d;
        u64 total;
        i32 ok = mp3_init_memory(&d, mp3_fixture_data, mp3_fixture_size);

        TEST_CHECK(ok, "s16 path: mp3_init_memory succeeds");
        total = mp3_read_pcm_frames_s16(&d, MP3_TEST_MAX_FRAMES, pcm_s16_);
        TEST_CHECK(total == 66816, "s16 path: decodes the same exact PCM frame count as the f32 path");
    }

    /* mp3dec_f32_to_s16: known-value conversion, independent of any real decode */
    {
        f32 in[5];
        i16 out[5];

        in[0] = 0.0f; in[1] = 1.0f; in[2] = -1.0f; in[3] = 0.5f; in[4] = -0.5f;
        mp3dec_f32_to_s16(in, out, 5);

        TEST_CHECK(out[0] == 0, "f32_to_s16: 0.0 converts to 0");
        TEST_CHECK(out[1] >= 32000, "f32_to_s16: 1.0 converts near the positive s16 ceiling");
        TEST_CHECK(out[2] <= -32000, "f32_to_s16: -1.0 converts near the negative s16 floor");
        TEST_CHECK(out[3] > 16000 && out[3] < 16800, "f32_to_s16: 0.5 converts to roughly half-scale");
        TEST_CHECK(out[4] < -16000 && out[4] > -16800, "f32_to_s16: -0.5 converts to roughly negative half-scale");
    }

    /* malformed/edge input: must fail cleanly, never crash or hang */
    {
        i32 ok;
        mp3_decoder d;

        ok = mp3_init_memory(&d, NULL, 0);
        TEST_CHECK(!ok, "malformed: NULL data + zero size is rejected cleanly");

        ok = mp3_init_memory(&d, mp3_fixture_data, 0);
        TEST_CHECK(!ok, "malformed: real data but zero size is rejected cleanly");

        {
            u8 garbage[256];
            size_t i;
            for (i = 0; i < sizeof(garbage); i++) garbage[i] = (u8)(i * 31 + 17); /* no valid MP3 frame sync anywhere */
            ok = mp3_init_memory(&d, garbage, sizeof(garbage));
            TEST_CHECK(!ok, "malformed: a buffer with no valid MP3 frame sync anywhere is rejected cleanly");
        }

        {
            /* only the first 3 bytes of a real frame header - not enough
             * to find/validate one complete frame */
            ok = mp3_init_memory(&d, mp3_fixture_data, 3);
            TEST_CHECK(!ok, "malformed: a buffer too short to contain one full frame is rejected cleanly");
        }
    }

    /* truncated stream: a real fixture cut off mid-stream (not on any
     * frame boundary) must decode some valid frames and then stop
     * cleanly - not crash, not hang, not read past the buffer - rather
     * than requiring a complete, well-formed file */
    {
        mp3_decoder d;
        u64 total;
        size_t truncated_size = mp3_fixture_size / 2;
        i32 ok = mp3_init_memory(&d, mp3_fixture_data, truncated_size);

        TEST_CHECK(ok, "truncated: init still succeeds since valid frames exist before the cut");
        total = mp3_read_pcm_frames_f32(&d, MP3_TEST_MAX_FRAMES, pcm_f32_);
        TEST_CHECK(total > 0 && total < 66816,
                   "truncated: decodes a partial, smaller-than-full frame count and stops cleanly");
    }

    print_flush();
    return test_failures_ != 0;
}
