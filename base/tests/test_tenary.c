#include "test_common.h"
#define TENARY_IMPLEMENTATION
#include "codec/tenary.h"

int main(void)
{
    print_cstr("test_tenary\n");

    /* radix packing ratios - the actual "kompresja 4:1 5:1" from this
     * file's own spec */
    TEST_CHECK(tenary_symbols_per_byte(3) == 5, "ternary (radix=3): 5 symbols/byte (3^5=243<=256), the BitNet b1.58 ratio");
    TEST_CHECK(tenary_symbols_per_byte(4) == 4, "radix=4: 4 symbols/byte (4^4=256 exactly)");
    TEST_CHECK(tenary_symbols_per_byte(2) == 8, "radix=2 (binary): 8 symbols/byte, i.e. plain bits");
    TEST_CHECK(tenary_symbols_per_byte(16) == 2, "radix=16: 2 symbols/byte (matches vecindex_4b.h's dims_per_byte)");

    {
        u8 symbols[5] = {0, 1, 2, 1, 0};
        u8 packed[1];
        u8 back[5];
        tenary_pack(packed, symbols, 5, 3);
        tenary_unpack(back, packed, 5, 3);
        TEST_CHECK(back[0] == 0 && back[1] == 1 && back[2] == 2 && back[3] == 1 && back[4] == 0,
                   "ternary pack/unpack: exact 5-symbol group round-trips");
    }

    {
        /* 7 symbols needs ceil(7/5)=2 bytes, second group partial (2 real + 3 padding) */
        u8 symbols[7] = {2, 2, 2, 2, 2, 1, 0};
        u8 packed[2];
        u8 back[7];
        tenary_pack(packed, symbols, 7, 3);
        tenary_unpack(back, packed, 7, 3);
        {
            i32 all_match = 1;
            size_t i;
            for (i = 0; i < 7; i++) if (back[i] != symbols[i]) all_match = 0;
            TEST_CHECK(all_match, "ternary pack/unpack: partial trailing group still round-trips exactly");
        }
    }

    {
        u8 symbols[8] = {0, 1, 2, 3, 3, 2, 1, 0};
        u8 packed[2];
        u8 back[8];
        tenary_pack(packed, symbols, 8, 4);
        tenary_unpack(back, packed, 8, 4);
        {
            i32 all_match = 1;
            size_t i;
            for (i = 0; i < 8; i++) if (back[i] != symbols[i]) all_match = 0;
            TEST_CHECK(all_match, "radix=4 pack/unpack: exact two-full-group round-trip");
        }
    }

    /* zigzag */
    TEST_CHECK(tenary_zigzag_encode(0) == 0, "zigzag: 0 -> 0");
    TEST_CHECK(tenary_zigzag_encode(-1) == 1, "zigzag: -1 -> 1");
    TEST_CHECK(tenary_zigzag_encode(1) == 2, "zigzag: 1 -> 2");
    TEST_CHECK(tenary_zigzag_encode(-2) == 3, "zigzag: -2 -> 3");
    TEST_CHECK(tenary_zigzag_encode(2) == 4, "zigzag: 2 -> 4");
    {
        i32 values[] = {0, -1, 1, -1000, 1000, -2147483647 - 1, 2147483647};
        size_t i;
        i32 all_roundtrip = 1;
        for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
            if (tenary_zigzag_decode(tenary_zigzag_encode(values[i])) != values[i]) all_roundtrip = 0;
        }
        TEST_CHECK(all_roundtrip, "zigzag: encode/decode round-trips exactly across a wide range including both i32 extremes");
    }

    /* delta */
    {
        i32 in[6] = {100, 102, 105, 104, 110, 110};
        i32 deltas[6];
        i32 back[6];
        i32 i, all_match = 1;

        tenary_delta_encode(deltas, in, 6);
        TEST_CHECK(deltas[0] == 100 && deltas[1] == 2 && deltas[2] == 3 && deltas[3] == -1 && deltas[4] == 6 && deltas[5] == 0,
                   "delta_encode: first value verbatim, rest are differences from the previous value");

        tenary_delta_decode(back, deltas, 6);
        for (i = 0; i < 6; i++) if (back[i] != in[i]) all_match = 0;
        TEST_CHECK(all_match, "delta_decode: exact inverse of delta_encode");
    }

    /* Golomb-Rice, isolated */
    {
        u32 values[8] = {0, 1, 2, 3, 4, 5, 8, 20};
        u8 buf[64];
        u32 back[8];
        size_t n_bytes;
        i32 i, all_match = 1;

        n_bytes = tenary_golomb_rice_encode(buf, sizeof(buf), values, 8, 2);
        TEST_CHECK(n_bytes > 0 && n_bytes < sizeof(buf), "golomb_rice_encode: produces a plausible non-zero byte count");

        tenary_golomb_rice_decode(back, 8, buf, n_bytes, 2);
        for (i = 0; i < 8; i++) if (back[i] != values[i]) all_match = 0;
        TEST_CHECK(all_match, "golomb_rice_decode: exact inverse of golomb_rice_encode");
    }

    {
        u32 one_value[1];
        one_value[0] = 1;
        TEST_CHECK(tenary_golomb_rice_encode(0, 0, one_value, 1, 2) == 0, "golomb_rice_encode: too-small buffer returns 0, not a crash");
    }

    /* full pipeline: delta -> zigzag -> golomb-rice -> decode -> unzigzag ->
     * undelta, matching the original exactly - AND actually smaller than
     * the raw 4-bytes-per-i32 encoding, the whole point of the pipeline. */
    {
        i32 original[16];
        i32 deltas[16], recon_deltas[16], recon[16];
        u32 zigzagged[16], degzigzagged[16];
        u8 encoded[128];
        size_t n_bytes, i;
        i32 all_match = 1;

        for (i = 0; i < 16; i++) original[i] = 1000 + (i32)(i * 3) - (i32)((i % 4) * 2); /* smooth, mildly noisy */

        tenary_delta_encode(deltas, original, 16);
        for (i = 0; i < 16; i++) zigzagged[i] = tenary_zigzag_encode(deltas[i]);

        n_bytes = tenary_golomb_rice_encode(encoded, sizeof(encoded), zigzagged, 16, 3);
        TEST_CHECK(n_bytes > 0, "pipeline: golomb-rice stage produced output");
        TEST_CHECK(n_bytes < 16 * sizeof(i32), "pipeline: encoded size beats the raw 4-bytes/value baseline on smooth data");

        tenary_golomb_rice_decode(degzigzagged, 16, encoded, n_bytes, 3);
        for (i = 0; i < 16; i++) recon_deltas[i] = tenary_zigzag_decode(degzigzagged[i]);
        tenary_delta_decode(recon, recon_deltas, 16);

        for (i = 0; i < 16; i++) if (recon[i] != original[i]) all_match = 0;
        TEST_CHECK(all_match, "pipeline: full delta+zigzag+golomb-rice round-trip reconstructs the original exactly");
    }

    print_flush();
    return test_failures_ != 0;
}
