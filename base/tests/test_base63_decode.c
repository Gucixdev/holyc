#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define BASE63_DECODE_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "codec/base63.h"
#include <string.h>

static const char base63_test_alphabet_[] =
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "-";

/* test-only encoder (mirrors base63_decode's own bignum technique in
 * reverse) - exists purely to generate valid base63 input for the
 * round-trip tests below, since the real module is decode-only. */
static void base63_test_encode_(Arena *a, const u8 *data, size_t size, char **out_text, size_t *out_len)
{
    u8 digits[8192];
    size_t ndigits = 0;
    size_t leading_zero_bytes = 0;
    size_t i;
    char *text;

    while (leading_zero_bytes < size && data[leading_zero_bytes] == 0) leading_zero_bytes++;

    for (i = leading_zero_bytes; i < size; i++) {
        u32 carry = data[i];
        size_t j;
        for (j = 0; j < ndigits; j++) {
            u32 v = (u32)digits[j] * 256u + carry;
            digits[j] = (u8)(v % 63u);
            carry = v / 63u;
        }
        while (carry > 0) {
            digits[ndigits++] = (u8)(carry % 63u);
            carry /= 63u;
        }
    }

    text = (char *)arena_alloc(a, leading_zero_bytes + ndigits + 1);
    for (i = 0; i < leading_zero_bytes; i++) text[i] = '0';
    for (i = 0; i < ndigits; i++) text[leading_zero_bytes + i] = base63_test_alphabet_[digits[ndigits - 1 - i]];
    text[leading_zero_bytes + ndigits] = '\0';

    *out_text = text;
    *out_len = leading_zero_bytes + ndigits;
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_base63_decode\n");

    {
        u8 raw[5] = { 1, 2, 3, 4, 5 };
        char *text;
        size_t text_len;
        u8 *decoded;
        size_t decoded_size;
        i32 err;

        base63_test_encode_(&a, raw, 5, &text, &text_len);
        err = base63_decode(&a, text, text_len, &decoded, &decoded_size);
        TEST_CHECK(err == BASE63_ESUCCESS, "decode succeeds on round-tripped bytes");
        TEST_CHECK(decoded_size == 5 && memcmp(decoded, raw, 5) == 0, "decoded bytes match the original exactly");
    }

    {
        u8 raw[4] = { 0, 0, 65, 66 };
        char *text;
        size_t text_len;
        u8 *decoded;
        size_t decoded_size;
        i32 err;

        base63_test_encode_(&a, raw, 4, &text, &text_len);
        err = base63_decode(&a, text, text_len, &decoded, &decoded_size);
        TEST_CHECK(err == BASE63_ESUCCESS, "decode succeeds on input with leading zero bytes");
        TEST_CHECK(decoded_size == 4 && memcmp(decoded, raw, 4) == 0, "leading zero bytes survive the round trip");
    }

    {
        u8 raw[1] = { 0 };
        char *text;
        size_t text_len;
        u8 *decoded;
        size_t decoded_size;
        i32 err;

        base63_test_encode_(&a, raw, 1, &text, &text_len);
        err = base63_decode(&a, text, text_len, &decoded, &decoded_size);
        TEST_CHECK(err == BASE63_ESUCCESS, "decode succeeds on a single zero byte");
        TEST_CHECK(decoded_size == 1 && decoded[0] == 0, "single zero byte round-trips correctly");
    }

    {
        u8 *decoded;
        size_t decoded_size;
        i32 err = base63_decode(&a, "abc!def", 7, &decoded, &decoded_size);
        TEST_CHECK(err == BASE63_EBADCHAR, "a character outside the alphabet is rejected");
    }

    {
        u8 *decoded;
        size_t decoded_size;
        i32 err = base63_decode(&a, "", 0, &decoded, &decoded_size);
        TEST_CHECK(err == BASE63_ESUCCESS && decoded_size == 0, "empty input decodes to zero bytes, not an error");
    }

    print_flush();
    return test_failures_ != 0;
}
