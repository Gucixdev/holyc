#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define STRING_VIEW_IMPLEMENTATION
#define STR_IMPLEMENTATION
#define TEXT_IMPLEMENTATION
#include "arena_alloc.h"
#include "text.h"

static i32 sv_is_(String_View sv, const char *expect)
{
    size_t len = 0;
    while (expect[len]) len++;
    return sv.count == len && sv_eq(sv, sv_from_parts(expect, len));
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_text\n");

    /* split */
    {
        String_View out[8];
        size_t n = text_split(SV("a,b,,d"), ',', out, 8);
        TEST_CHECK(n == 4, "text_split: \"a,b,,d\" on ',' yields 4 pieces (an empty piece between two delimiters counts)");
        TEST_CHECK(sv_is_(out[0], "a") && sv_is_(out[1], "b") && sv_is_(out[2], "") && sv_is_(out[3], "d"),
                   "text_split: pieces are exactly \"a\",\"b\",\"\",\"d\" in order");
    }
    {
        String_View out[2];
        size_t n = text_split(SV("a,b,c,d,e"), ',', out, 2);
        TEST_CHECK(n == 5, "text_split: the TRUE piece count is returned even when it exceeds max_out");
        TEST_CHECK(sv_is_(out[0], "a") && sv_is_(out[1], "b"), "text_split: only the first max_out pieces are actually written");
    }
    {
        String_View out[4];
        size_t n = text_split(SV("solo"), ',', out, 4);
        TEST_CHECK(n == 1 && sv_is_(out[0], "solo"), "text_split: a string with no delimiter at all yields exactly one piece (the whole thing)");
    }

    /* words */
    {
        String_View out[8];
        size_t n = text_words(SV("  hello   world  "), out, 8);
        TEST_CHECK(n == 2, "text_words: leading/trailing whitespace and runs between words collapse to exactly 2 words");
        TEST_CHECK(sv_is_(out[0], "hello") && sv_is_(out[1], "world"), "text_words: the words themselves are exactly \"hello\"/\"world\", no stray whitespace");
    }
    {
        String_View out[4];
        size_t n = text_words(SV("   "), out, 4);
        TEST_CHECK(n == 0, "text_words: an all-whitespace input yields zero words");
    }

    /* lines */
    {
        String_View out[8];
        size_t n = text_lines(SV("first\nsecond\r\nthird"), out, 8);
        TEST_CHECK(n == 3, "text_lines: 3 lines from a mix of LF and CRLF endings");
        TEST_CHECK(sv_is_(out[0], "first") && sv_is_(out[1], "second") && sv_is_(out[2], "third"),
                   "text_lines: CRLF's trailing \\r is stripped, all 3 lines match exactly");
    }
    {
        String_View out[4];
        size_t n = text_lines(SV("a\nb\n"), out, 4);
        TEST_CHECK(n == 2, "text_lines: a trailing newline does not create a spurious extra empty final line");
    }

    /* join */
    {
        String_View parts[3];
        String_View joined;
        parts[0] = SV("foo"); parts[1] = SV("bar"); parts[2] = SV("baz");
        joined = text_join(&a, parts, 3, SV(", "));
        TEST_CHECK(sv_is_(joined, "foo, bar, baz"), "text_join: 3 parts joined with \", \" produces the exact expected string");
    }
    {
        String_View parts[1];
        String_View joined;
        parts[0] = SV("alone");
        joined = text_join(&a, parts, 1, SV(", "));
        TEST_CHECK(sv_is_(joined, "alone"), "text_join: a single part produces itself with no separator applied at all");
    }

    /* reverse */
    {
        char buf[6];
        buf[0]='a';buf[1]='b';buf[2]='c';buf[3]='d';buf[4]='e';buf[5]='\0';
        text_reverse(buf, 5);
        TEST_CHECK(buf[0]=='e'&&buf[1]=='d'&&buf[2]=='c'&&buf[3]=='b'&&buf[4]=='a', "text_reverse: \"abcde\" reverses to \"edcba\" in place");
        TEST_CHECK(buf[5]=='\0', "text_reverse: does not touch bytes past the given length");
    }
    {
        char single[1] = { 'x' };
        text_reverse(single, 1);
        TEST_CHECK(single[0] == 'x', "text_reverse: a length-1 buffer is unchanged (no out-of-bounds access)");
        text_reverse(single, 0);
        TEST_CHECK(single[0] == 'x', "text_reverse: a length-0 call is a safe no-op");
    }

    /* count */
    TEST_CHECK(text_count(SV("mississippi"), 's') == 4, "text_count: 's' appears 4 times in \"mississippi\"");
    TEST_CHECK(text_count(SV("mississippi"), 'z') == 0, "text_count: a character absent from the string counts 0");

    /* ucase/lcase */
    {
        char buf[16];
        i32 i;
        const char *src = "Hello, World!";
        for (i = 0; src[i]; i++) buf[i] = src[i];
        text_ucase(buf, 13);
        TEST_CHECK(buf[0]=='H'&&buf[1]=='E'&&buf[2]=='L', "text_ucase: letters are uppercased");
        TEST_CHECK(buf[5]==','&&buf[6]==' ', "text_ucase: non-letter bytes (',', ' ', '!') are left untouched");
    }
    {
        char buf[16];
        i32 i;
        const char *src = "Hello, World!";
        for (i = 0; src[i]; i++) buf[i] = src[i];
        text_lcase(buf, 13);
        TEST_CHECK(buf[0]=='h'&&buf[7]=='w', "text_lcase: letters are lowercased, including after the space");
        TEST_CHECK(buf[12]=='!', "text_lcase: the trailing '!' is untouched");
    }

    print_flush();
    return test_failures_ != 0;
}
