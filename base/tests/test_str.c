#include "test_common.h"
#define STR_IMPLEMENTATION
#include "core/str.h"

int main(void)
{
    char buf[32];

    print_cstr("test_str\n");

    TEST_CHECK(str_len("") == 0, "str_len: empty string");
    TEST_CHECK(str_len("hello") == 5, "str_len: hello");

    TEST_CHECK(str_cmp("abc", "abc") == 0, "str_cmp: equal strings");
    TEST_CHECK(str_cmp("abc", "abd") < 0, "str_cmp: abc < abd");
    TEST_CHECK(str_cmp("abd", "abc") > 0, "str_cmp: abd > abc");
    TEST_CHECK(str_cmp("ab", "abc") < 0, "str_cmp: shorter prefix is less");
    TEST_CHECK(str_cmp("", "") == 0, "str_cmp: two empty strings are equal");

    TEST_CHECK(str_ncmp("abcxx", "abcyy", 3) == 0, "str_ncmp: equal within n");
    TEST_CHECK(str_ncmp("abc", "abd", 3) < 0, "str_ncmp: differs within n");
    TEST_CHECK(str_ncmp("ab", "abc", 3) != 0, "str_ncmp: shorter string differs before n");

    str_cpy(buf, "copied");
    TEST_CHECK(str_cmp(buf, "copied") == 0, "str_cpy: round-trips through str_cmp");

    mem_set(buf, 'X', sizeof(buf));
    str_ncpy(buf, "hi", 5);
    TEST_CHECK(buf[0] == 'h' && buf[1] == 'i' && buf[2] == '\0' && buf[3] == '\0' && buf[4] == '\0',
               "str_ncpy: copies then zero-pads the remainder");

    {
        char *p = str_chr("hello world", 'w');
        TEST_CHECK(p != NULL && str_cmp(p, "world") == 0, "str_chr: finds a present character");
        TEST_CHECK(str_chr("hello", 'z') == NULL, "str_chr: NULL for an absent character");
        p = str_chr("hello", '\0');
        TEST_CHECK(p != NULL && *p == '\0', "str_chr: finds the terminating NUL like real strchr");
    }

    {
        char *p = str_str("hello world", "wor");
        TEST_CHECK(p != NULL && str_cmp(p, "world") == 0, "str_str: finds a present substring");
        TEST_CHECK(str_str("hello world", "xyz") == NULL, "str_str: NULL for an absent substring");
        p = str_str("hello", "");
        TEST_CHECK(p != NULL && str_cmp(p, "hello") == 0, "str_str: empty needle matches at the start");
    }

    {
        char src[] = "memcpy me";
        char dst[16];
        void *ret = mem_cpy(dst, src, sizeof(src));
        TEST_CHECK(ret == dst && mem_cmp(dst, src, sizeof(src)) == 0, "mem_cpy: byte-exact copy");
    }

    {
        /* overlapping, dst > src: the hazard mem_cpy is allowed to get
         * wrong and mem_move must not. */
        char data[11] = "0123456789";
        mem_move(data + 2, data, 5); /* "01234" -> shifted right by 2 */
        TEST_CHECK(mem_cmp(data, "0101234789", 10) == 0, "mem_move: correct with dst overlapping past src");
    }

    {
        char data[11] = "0123456789";
        mem_move(data, data + 2, 5); /* "23456" -> shifted left by 2 */
        TEST_CHECK(mem_cmp(data, "2345656789", 10) == 0, "mem_move: correct with src overlapping past dst");
    }

    TEST_CHECK(mem_cmp("abc", "abc", 3) == 0, "mem_cmp: equal buffers");
    TEST_CHECK(mem_cmp("abc", "abd", 3) < 0, "mem_cmp: differs at last byte");
    TEST_CHECK(mem_cmp("\x00\x01", "\x00\x02", 2) < 0, "mem_cmp: differs at a NUL-containing byte (not string-length-bounded)");

    {
        u8 data[8];
        mem_set(data, 0xAB, sizeof(data));
        {
            size_t i;
            i32 all_set = 1;
            for (i = 0; i < sizeof(data); i++) if (data[i] != 0xAB) all_set = 0;
            TEST_CHECK(all_set, "mem_set: fills every byte with the given value");
        }
    }

    print_flush();
    return test_failures_ != 0;
}
