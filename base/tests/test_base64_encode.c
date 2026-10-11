#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define BASE64_ENCODE_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "codec/base64.h"
#include <string.h>

static i32 check_vec_(Arena *a, const char *input, const char *expected)
{
    size_t out_size;
    char *out = base64_encode(a, (const u8 *)input, strlen(input), &out_size);
    return out_size == strlen(expected) && memcmp(out, expected, out_size) == 0;
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_base64_encode\n");

    /* RFC 4648 section 10's own test vectors */
    TEST_CHECK(check_vec_(&a, "", ""), "RFC 4648 vector: \"\"");
    TEST_CHECK(check_vec_(&a, "f", "Zg=="), "RFC 4648 vector: \"f\"");
    TEST_CHECK(check_vec_(&a, "fo", "Zm8="), "RFC 4648 vector: \"fo\"");
    TEST_CHECK(check_vec_(&a, "foo", "Zm9v"), "RFC 4648 vector: \"foo\"");
    TEST_CHECK(check_vec_(&a, "foob", "Zm9vYg=="), "RFC 4648 vector: \"foob\"");
    TEST_CHECK(check_vec_(&a, "fooba", "Zm9vYmE="), "RFC 4648 vector: \"fooba\"");
    TEST_CHECK(check_vec_(&a, "foobar", "Zm9vYmFy"), "RFC 4648 vector: \"foobar\"");

    {
        u8 raw[3] = { 0x00, 0x00, 0x00 };
        size_t out_size;
        char *out = base64_encode(&a, raw, 3, &out_size);
        TEST_CHECK(out_size == 4 && memcmp(out, "AAAA", 4) == 0, "all-zero 3-byte block encodes to AAAA");
    }
    {
        u8 raw[3] = { 0xFF, 0xFF, 0xFF };
        size_t out_size;
        char *out = base64_encode(&a, raw, 3, &out_size);
        TEST_CHECK(out_size == 4 && memcmp(out, "////", 4) == 0, "all-ones 3-byte block encodes to ////");
    }

    print_flush();
    return test_failures_ != 0;
}
