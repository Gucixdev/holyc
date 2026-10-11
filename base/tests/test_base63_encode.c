#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define BASE63_DECODE_IMPLEMENTATION
#define BASE63_ENCODE_IMPLEMENTATION
#include "arena_alloc.h"
#include "base63.h"

#include <string.h>

int main(void)
{
    Arena a = {0};

    print_cstr("test_base63_encode\n");

    /* round-trip: encode then decode reproduces the original bytes */
    {
        u8 raw[5] = { 1, 2, 3, 4, 5 };
        char *text;
        size_t text_len;
        u8 *decoded;
        size_t decoded_size;
        i32 err;

        text = base63_encode(&a, raw, 5, &text_len);
        TEST_CHECK(text != NULL, "encode: succeeds on a plain byte buffer");

        err = base63_decode(&a, text, text_len, &decoded, &decoded_size);
        TEST_CHECK(err == BASE63_ESUCCESS, "decode: round-tripped text decodes successfully");
        TEST_CHECK(decoded_size == 5 && memcmp(decoded, raw, 5) == 0,
                   "round-trip: decoded bytes match the original exactly");
    }

    /* leading zero bytes survive the round trip */
    {
        u8 raw[4] = { 0, 0, 65, 66 };
        char *text;
        size_t text_len;
        u8 *decoded;
        size_t decoded_size;
        i32 err;

        text = base63_encode(&a, raw, 4, &text_len);
        TEST_CHECK(text[0] == '0' && text[1] == '0',
                   "encode: each leading zero byte becomes a leading '0' character");

        err = base63_decode(&a, text, text_len, &decoded, &decoded_size);
        TEST_CHECK(err == BASE63_ESUCCESS && decoded_size == 4 && memcmp(decoded, raw, 4) == 0,
                   "round-trip: leading zero bytes preserved exactly");
    }

    /* single zero byte */
    {
        u8 raw[1] = { 0 };
        char *text;
        size_t text_len;
        u8 *decoded;
        size_t decoded_size;
        i32 err;

        text = base63_encode(&a, raw, 1, &text_len);
        TEST_CHECK(text_len == 1 && text[0] == '0', "encode: a single zero byte becomes a single '0' character");

        err = base63_decode(&a, text, text_len, &decoded, &decoded_size);
        TEST_CHECK(err == BASE63_ESUCCESS && decoded_size == 1 && decoded[0] == 0,
                   "round-trip: single zero byte survives exactly");
    }

    /* empty input */
    {
        char *text;
        size_t text_len;

        text = base63_encode(&a, (const u8 *)"", 0, &text_len);
        (void)text;
        TEST_CHECK(text_len == 0, "encode: empty input produces empty output");
    }

    /* only the documented '-' symbol (the 63rd, non-alphanumeric one) appears when expected */
    {
        u8 raw[3] = { 0xFF, 0xFF, 0xFF };
        char *text;
        size_t text_len;
        i32 saw_dash;
        size_t i;

        text = base63_encode(&a, raw, 3, &text_len);
        saw_dash = 0;
        for (i = 0; i < text_len; i++) {
            if (text[i] == '-') saw_dash = 1;
            TEST_CHECK((text[i] >= '0' && text[i] <= '9') ||
                       (text[i] >= 'A' && text[i] <= 'Z') ||
                       (text[i] >= 'a' && text[i] <= 'z') ||
                       text[i] == '-',
                       "encode: every output character is in the documented base63 alphabet");
        }
        (void)saw_dash; /* not asserted on its own - just documents the alphabet includes '-' at all */
    }

    /* round-trip across many pseudo-random byte buffers, several lengths */
    {
        u32 rng = 12345u;
        size_t len;
        i32 all_ok = 1;

        for (len = 1; len <= 64; len++) {
            u8 buf[64];
            char *text;
            size_t text_len;
            u8 *decoded;
            size_t decoded_size;
            i32 err;
            size_t i;

            for (i = 0; i < len; i++) {
                rng = rng * 1103515245u + 12345u;
                buf[i] = (u8)(rng >> 16);
            }

            text = base63_encode(&a, buf, len, &text_len);
            err = base63_decode(&a, text, text_len, &decoded, &decoded_size);
            if (err != BASE63_ESUCCESS || decoded_size != len || memcmp(decoded, buf, len) != 0) {
                all_ok = 0;
            }
        }
        TEST_CHECK(all_ok, "round-trip: 64 pseudo-random buffers of lengths 1..64 all encode/decode exactly");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
