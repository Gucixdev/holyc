#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define QOI_DEC_IMPLEMENTATION
#define QOI_ENC_IMPLEMENTATION
#include "arena_alloc.h"
#include "qoi_codec.h"

#include <string.h>

int main(void)
{
    Arena a = {0};

    print_cstr("test_qoi\n");

    /* round-trip: small RGB image exercising runs, diffs, and index reuse */
    {
        u8 src[4 * 4 * 3];
        u8 *encoded;
        size_t encoded_size;
        Qoi_Image decoded;
        i32 err;
        i32 ok;
        size_t i;

        /* row 0: a solid run of the same color (QOI_OP_RUN) */
        for (i = 0; i < 4; i++) {
            src[i * 3 + 0] = 200; src[i * 3 + 1] = 50; src[i * 3 + 2] = 50;
        }
        /* row 1: small per-channel steps (QOI_OP_DIFF range) */
        src[12] = 200; src[13] = 50; src[14] = 50;
        src[15] = 201; src[16] = 51; src[17] = 49;
        src[18] = 202; src[19] = 50; src[20] = 50;
        src[21] = 201; src[22] = 51; src[23] = 51;
        /* row 2: revisit row 0's exact color (QOI_OP_INDEX) then something new */
        src[24] = 200; src[25] = 50; src[26] = 50;
        src[27] = 0;   src[28] = 0;  src[29] = 0;
        src[30] = 10;  src[31] = 200; src[32] = 30;
        src[33] = 255; src[34] = 255; src[35] = 255;
        /* row 3: a bigger green-dominant step (QOI_OP_LUMA range) */
        src[36] = 60;  src[37] = 120; src[38] = 60;
        src[39] = 20;  src[40] = 90;  src[41] = 25;
        src[42] = 100; src[43] = 40;  src[44] = 100;
        src[45] = 5;   src[46] = 5;   src[47] = 5;

        err = qoi_encode(&a, src, 4, 4, 3, 0, &encoded, &encoded_size);
        TEST_CHECK(err == QOI_ESUCCESS, "encode: RGB image succeeds");
        TEST_CHECK(memcmp(encoded, "qoif", 4) == 0, "encode: output starts with the qoif magic");

        err = qoi_decode(&a, encoded, encoded_size, &decoded);
        TEST_CHECK(err == QOI_ESUCCESS, "decode: round-tripped bytes decode successfully");
        TEST_CHECK(decoded.width == 4 && decoded.height == 4 && decoded.channels == 3,
                   "decode: dimensions and channel count match what was encoded");

        ok = 1;
        for (i = 0; i < sizeof(src); i++) {
            if (decoded.pixels[i] != src[i]) { ok = 0; break; }
        }
        TEST_CHECK(ok, "decode: every pixel byte round-trips exactly (lossless format)");
    }

    /* round-trip: RGBA image with a real alpha channel */
    {
        u8 src[2 * 2 * 4] = {
            255, 0, 0, 255,
            0, 255, 0, 128,
            0, 0, 255, 0,
            255, 255, 255, 64
        };
        u8 *encoded;
        size_t encoded_size;
        Qoi_Image decoded;
        i32 err;
        i32 ok;
        size_t i;

        err = qoi_encode(&a, src, 2, 2, 4, 0, &encoded, &encoded_size);
        TEST_CHECK(err == QOI_ESUCCESS, "encode: RGBA image succeeds");

        err = qoi_decode(&a, encoded, encoded_size, &decoded);
        TEST_CHECK(err == QOI_ESUCCESS, "decode: RGBA round-trip decodes successfully");
        TEST_CHECK(decoded.channels == 4, "decode: channel count preserved as 4 (RGBA)");

        ok = 1;
        for (i = 0; i < sizeof(src); i++) {
            if (decoded.pixels[i] != src[i]) { ok = 0; break; }
        }
        TEST_CHECK(ok, "decode: every RGBA byte (including alpha) round-trips exactly");
    }

    /* malformed input rejected cleanly */
    {
        Qoi_Image decoded;
        i32 err;

        err = qoi_decode(&a, (const u8 *)"not a qoi file at all", 21, &decoded);
        TEST_CHECK(err == QOI_EBADMAGIC, "decode: non-QOI input is rejected cleanly, not crashed on");

        {
            u8 bad_channels[14] = { 'q', 'o', 'i', 'f', 0, 0, 0, 1, 0, 0, 0, 1, 5, 0 };
            err = qoi_decode(&a, bad_channels, sizeof(bad_channels), &decoded);
            TEST_CHECK(err == QOI_EBADCHANNELS, "decode: an invalid channels byte (5) is rejected");
        }
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
