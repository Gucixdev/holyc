#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define DEFLATE_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#include "codec/deflate.h"
#include "deflate_test_data.h"
#include <string.h>

int main(void)
{
    Arena a = {0};

    print_cstr("test_deflate\n");

    /* dynamic-huffman-coded, back-reference-heavy payload (zlib level 9 on
     * repetitive text - real-world-shaped compressed data, not synthetic) */
    {
        u8 *out;
        size_t out_len;
        i32 ok = deflate_decode(&a, deflate_test_compressed, deflate_test_compressed_len, 1, &out, &out_len);
        TEST_CHECK(ok, "decode succeeds on a real zlib stream (dynamic huffman + back-references)");
        TEST_CHECK(out_len == deflate_test_original_len, "decompressed length matches the original");
        if (ok && out_len == deflate_test_original_len) {
            i32 match = (memcmp(out, deflate_test_original, out_len) == 0);
            TEST_CHECK(match, "decompressed bytes match the original exactly");
        }
    }

    /* level-0 zlib output uses uncompressed ("stored") DEFLATE blocks -
     * exercises a completely different code path than the dynamic-huffman
     * case above */
    {
        u8 *out;
        size_t out_len;
        i32 ok = deflate_decode(&a, deflate_test_stored_compressed, deflate_test_stored_compressed_len, 1, &out, &out_len);
        TEST_CHECK(ok, "decode succeeds on a stored (uncompressed) DEFLATE block");
        TEST_CHECK(out_len == deflate_test_stored_original_len, "stored-block decompressed length matches original");
        if (ok && out_len == deflate_test_stored_original_len) {
            i32 match = (memcmp(out, deflate_test_stored_original, out_len) == 0);
            TEST_CHECK(match, "stored-block decompressed bytes match the original exactly");
        }
    }

    /* malformed input must fail cleanly, not crash/hang */
    {
        u8 garbage[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        u8 *out;
        size_t out_len;
        i32 ok = deflate_decode(&a, garbage, sizeof(garbage), 1, &out, &out_len);
        TEST_CHECK(!ok, "garbage input is rejected rather than crashing or hanging");
    }

    /* NOT a strict "truncation is always detected" claim - this decoder,
     * like the stb_image algorithm it's ported from, does not verify the
     * Adler32 trailer and pads a truncated bitstream with silent zero
     * bits rather than erroring at every possible cut point (real zlib
     * would catch this via the checksum; this minimal decoder trades that
     * away for simplicity, same tradeoff upstream made). The real
     * guarantee is just that it terminates and doesn't corrupt memory -
     * this test would hang or crash under ASan if that guarantee didn't
     * hold, which is what it's actually checking. */
    {
        u8 *out;
        size_t out_len;
        deflate_decode(&a, deflate_test_compressed, 3, 1, &out, &out_len);
        TEST_CHECK(1, "decoding a truncated stream terminates without crashing (see comment above)");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
