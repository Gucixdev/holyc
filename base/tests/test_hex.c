#include "test_common.h"
#define HEX_IMPLEMENTATION
#include "data/hex.h"

int main(void)
{
    print_cstr("test_hex\n");

    {
        u8 data[] = { 0x00, 0xFF, 0x1A, 0x2B, 0xDE, 0xAD, 0xBE, 0xEF };
        char out[16];
        hex_encode(data, sizeof(data), out);
        TEST_CHECK(out[0] == '0' && out[1] == '0', "hex_encode: 0x00 -> \"00\"");
        TEST_CHECK(out[2] == 'f' && out[3] == 'f', "hex_encode: 0xFF -> \"ff\" (lowercase)");
        TEST_CHECK(out[4] == '1' && out[5] == 'a', "hex_encode: 0x1A -> \"1a\"");
        TEST_CHECK(out[8] == 'd' && out[9] == 'e', "hex_encode: 0xDE -> \"de\"");
        TEST_CHECK(out[14] == 'e' && out[15] == 'f', "hex_encode: 0xEF -> \"ef\"");
    }

    {
        u8 out[4];
        i32 ok = hex_decode("deadbeef", 8, out);
        TEST_CHECK(ok == 1, "hex_decode: well-formed input succeeds");
        TEST_CHECK(out[0] == 0xDE && out[1] == 0xAD && out[2] == 0xBE && out[3] == 0xEF,
                   "hex_decode: decodes back to the exact original bytes");
    }

    {
        u8 out[4];
        i32 ok = hex_decode("DEADBEEF", 8, out);
        TEST_CHECK(ok == 1, "hex_decode: uppercase hex digits are also accepted");
        TEST_CHECK(out[0] == 0xDE && out[3] == 0xEF, "hex_decode: uppercase decodes to the same bytes as lowercase");
    }

    {
        u8 out[4];
        TEST_CHECK(hex_decode("abc", 3, out) == 0, "hex_decode: odd-length input is rejected");
    }

    {
        u8 out[4];
        TEST_CHECK(hex_decode("zz00", 4, out) == 0, "hex_decode: a non-hex-digit character is rejected");
    }

    {
        u8 out[2] = { 0x11, 0x22 };
        i32 ok = hex_decode("aazz", 4, out); /* second pair is bad - should fail without touching out at all */
        TEST_CHECK(ok == 0, "hex_decode: a bad digit anywhere in the input rejects the whole call");
        TEST_CHECK(out[0] == 0x11 && out[1] == 0x22, "hex_decode: out is left completely untouched on failure, not partially written");
    }

    /* round-trip across a range of lengths */
    {
        u8 src[32];
        char hexed[64];
        u8 back[32];
        size_t len;
        i32 all_ok = 1;

        for (len = 0; len <= sizeof(src); len++) {
            size_t i;
            for (i = 0; i < len; i++) src[i] = (u8)((i * 37 + len) & 0xFF);
            hex_encode(src, len, hexed);
            if (!hex_decode(hexed, len * 2, back)) { all_ok = 0; break; }
            {
                size_t j;
                for (j = 0; j < len; j++) if (back[j] != src[j]) { all_ok = 0; break; }
            }
            if (!all_ok) break;
        }
        TEST_CHECK(all_ok, "hex_encode/hex_decode round-trip exactly across lengths 0..32");
    }

    print_flush();
    return test_failures_ != 0;
}
