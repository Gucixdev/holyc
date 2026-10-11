#include "test_common.h"
#define UTF8_IMPLEMENTATION
#include "core/utf8.h"

int main(void)
{
    print_cstr("test_utf8\n");

    {
        u32 cp;
        i32 n = utf8_decode((const u8 *)"A", 1, &cp);
        TEST_CHECK(n == 1 && cp == 'A', "decode: 1-byte ASCII round-trips its own value");
    }
    {
        u32 cp;
        /* U+00F3 ("o" with acute), UTF-8: 0xC3 0xB3 */
        u8 bytes[2] = { 0xC3, 0xB3 };
        i32 n = utf8_decode(bytes, 2, &cp);
        TEST_CHECK(n == 2 && cp == 0xF3, "decode: 2-byte sequence decodes to the correct codepoint");
    }
    {
        u32 cp;
        /* U+4E2D (CJK), UTF-8: 0xE4 0xB8 0xAD */
        u8 bytes[3] = { 0xE4, 0xB8, 0xAD };
        i32 n = utf8_decode(bytes, 3, &cp);
        TEST_CHECK(n == 3 && cp == 0x4E2D, "decode: 3-byte sequence decodes to the correct codepoint");
    }
    {
        u32 cp;
        /* U+1F600 (emoji), UTF-8: 0xF0 0x9F 0x98 0x80 */
        u8 bytes[4] = { 0xF0, 0x9F, 0x98, 0x80 };
        i32 n = utf8_decode(bytes, 4, &cp);
        TEST_CHECK(n == 4 && cp == 0x1F600, "decode: 4-byte sequence decodes to the correct codepoint");
    }
    {
        u8 bytes[3] = { 0xE0, 0x80, 0x80 }; /* overlong encoding of U+0000 */
        u32 cp;
        TEST_CHECK(utf8_decode(bytes, 3, &cp) == 0, "decode: an overlong encoding is rejected");
    }
    {
        u8 bytes[3] = { 0xED, 0xA0, 0x80 }; /* U+D800, a surrogate */
        u32 cp;
        TEST_CHECK(utf8_decode(bytes, 3, &cp) == 0, "decode: a surrogate codepoint is rejected");
    }
    {
        u8 bytes[2] = { 0xC3, 0x28 }; /* invalid continuation byte */
        u32 cp;
        TEST_CHECK(utf8_decode(bytes, 2, &cp) == 0, "decode: a broken continuation byte is rejected");
    }

    {
        u8 out[4];
        TEST_CHECK(utf8_encode(0x1F600, out) == 4 && out[0]==0xF0 && out[1]==0x9F && out[2]==0x98 && out[3]==0x80,
                   "encode: matches the known byte sequence for the same emoji decoded above");
        TEST_CHECK(utf8_encode(0xD800, out) == 0, "encode: refuses to encode a surrogate codepoint");
        TEST_CHECK(utf8_encode(0x110000, out) == 0, "encode: refuses to encode past Unicode's max codepoint");
    }

    TEST_CHECK(utf8_strlen("hello") == 5, "strlen: pure ASCII counts one codepoint per byte");
    TEST_CHECK(utf8_strlen("") == 0, "strlen: empty string is 0");
    {
        /* "a" + U+00F3 + "b" = 1 + 2 + 1 = 4 bytes, 3 codepoints */
        char mixed[5];
        mixed[0] = 'a'; mixed[1] = (char)0xC3; mixed[2] = (char)0xB3; mixed[3] = 'b'; mixed[4] = '\0';
        TEST_CHECK(utf8_strlen(mixed) == 3, "strlen: counts codepoints, not bytes, for a mixed ASCII/multibyte string");
    }

    TEST_CHECK(utf8_validate("hello world", 11), "validate: plain ASCII is valid");
    {
        u8 bad[2] = { 0xC3, 0x28 };
        TEST_CHECK(!utf8_validate(bad, 2), "validate: a malformed sequence anywhere makes the whole buffer invalid");
    }

    print_flush();
    return test_failures_ != 0;
}
