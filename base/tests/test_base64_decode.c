#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define BASE64_ENCODE_IMPLEMENTATION
#define BASE64_DECODE_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#include "codec/base64.h"
#include <string.h>

static i32 decode_matches_(Arena *a, const char *b64, const char *expected)
{
    u8 *out;
    size_t out_size;
    i32 err = base64_decode(a, b64, strlen(b64), &out, &out_size);
    if (err != BASE64_ESUCCESS) return 0;
    if (out_size != strlen(expected)) return 0;
    return memcmp(out, expected, out_size) == 0;
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_base64_decode\n");

    /* RFC 4648 section 10's own test vectors */
    TEST_CHECK(decode_matches_(&a, "", ""), "RFC 4648 vector: \"\" -> \"\"");
    TEST_CHECK(decode_matches_(&a, "Zg==", "f"), "RFC 4648 vector: \"Zg==\" -> \"f\"");
    TEST_CHECK(decode_matches_(&a, "Zm8=", "fo"), "RFC 4648 vector: \"Zm8=\" -> \"fo\"");
    TEST_CHECK(decode_matches_(&a, "Zm9v", "foo"), "RFC 4648 vector: \"Zm9v\" -> \"foo\"");
    TEST_CHECK(decode_matches_(&a, "Zm9vYg==", "foob"), "RFC 4648 vector: \"Zm9vYg==\" -> \"foob\"");
    TEST_CHECK(decode_matches_(&a, "Zm9vYmE=", "fooba"), "RFC 4648 vector: \"Zm9vYmE=\" -> \"fooba\"");
    TEST_CHECK(decode_matches_(&a, "Zm9vYmFy", "foobar"), "RFC 4648 vector: \"Zm9vYmFy\" -> \"foobar\"");

    /* round-trip through this project's own encoder across several lengths */
    {
        u8 src0[1] = { 0 };
        u8 src255[1] = { 255 };
        u8 srcbin[6] = { 0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF };
        struct { const u8 *data; size_t len; } cases[3];
        size_t c;
        i32 all_ok = 1;

        cases[0].data = src0;    cases[0].len = 1;
        cases[1].data = src255;  cases[1].len = 1;
        cases[2].data = srcbin;  cases[2].len = 6;

        for (c = 0; c < 3; c++) {
            char *encoded;
            size_t encoded_len;
            u8 *decoded;
            size_t decoded_len;
            i32 err;

            encoded = base64_encode(&a, cases[c].data, cases[c].len, &encoded_len);
            err = base64_decode(&a, encoded, encoded_len, &decoded, &decoded_len);
            if (err != BASE64_ESUCCESS || decoded_len != cases[c].len ||
                memcmp(decoded, cases[c].data, cases[c].len) != 0) {
                all_ok = 0;
            }
        }
        TEST_CHECK(all_ok, "round-trip: encode then decode reproduces the original bytes, several lengths");
    }

    /* malformed input rejected cleanly */
    {
        u8 *out;
        size_t out_size;
        i32 err;

        err = base64_decode(&a, "abc", 3, &out, &out_size); /* not a multiple of 4 */
        TEST_CHECK(err == BASE64_EBADLEN, "decode: length not a multiple of 4 is rejected");

        err = base64_decode(&a, "abc!", 4, &out, &out_size); /* '!' not in alphabet */
        TEST_CHECK(err == BASE64_EBADCHAR, "decode: character outside the alphabet is rejected");

        err = base64_decode(&a, "ab==cdef", 8, &out, &out_size); /* '=' in the middle, not the end */
        TEST_CHECK(err == BASE64_EBADCHAR, "decode: padding character in a non-trailing position is rejected");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
