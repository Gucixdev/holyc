#include "test_common.h"
#define STRING_VIEW_IMPLEMENTATION
#define TEXT_OPS_IMPLEMENTATION
#include "core/str.h"
#include "util/text_ops.h"

static String_View sv_(const char *s) { return sv_from_cstr(s); }

int main(void)
{
    print_cstr("test_text_ops\n");

    TEST_CHECK(text_ops_count_lines(sv_("")) == 0, "count_lines: an empty string has 0 lines");
    TEST_CHECK(text_ops_count_lines(sv_("one line")) == 1, "count_lines: one line, no trailing newline, counts as 1");
    TEST_CHECK(text_ops_count_lines(sv_("a\nb\nc")) == 3, "count_lines: two internal newlines make 3 lines");
    TEST_CHECK(text_ops_count_lines(sv_("a\nb\nc\n")) == 3, "count_lines: a trailing newline doesn't add a phantom 4th line");
    TEST_CHECK(text_ops_count_lines(sv_("\n\n")) == 2, "count_lines: two blank lines (matches wc -l convention)");

    TEST_CHECK(text_ops_count_words(sv_("")) == 0, "count_words: empty string has 0 words");
    TEST_CHECK(text_ops_count_words(sv_("hello world")) == 2, "count_words: simple two-word sentence");
    TEST_CHECK(text_ops_count_words(sv_("  hello   world  ")) == 2, "count_words: extra whitespace doesn't inflate the count");
    TEST_CHECK(text_ops_count_words(sv_("a\nb\tc")) == 3, "count_words: newlines and tabs are whitespace too, same as spaces");

    TEST_CHECK(text_ops_count_occurrences(sv_("abababab"), sv_("ab")) == 4, "count_occurrences: non-overlapping matches counted correctly");
    TEST_CHECK(text_ops_count_occurrences(sv_("aaaa"), sv_("aa")) == 2, "count_occurrences: overlapping-candidate matches are counted non-overlapping (aaaa/aa = 2, not 3)");
    TEST_CHECK(text_ops_count_occurrences(sv_("hello"), sv_("xyz")) == 0, "count_occurrences: no match at all is 0");
    TEST_CHECK(text_ops_count_occurrences(sv_("hello"), sv_("")) == 0, "count_occurrences: an empty needle matches nowhere, not len+1 times");

    {
        String_View parts[8];
        size_t n = text_ops_split(sv_("a,b,c"), ',', parts, 8);
        TEST_CHECK(n == 3, "split: 'a,b,c' on ',' gives 3 fields");
        TEST_CHECK(sv_eq(parts[0], sv_("a")) && sv_eq(parts[1], sv_("b")) && sv_eq(parts[2], sv_("c")), "split: each field matches exactly");
    }
    {
        String_View parts[2];
        size_t n = text_ops_split(sv_("a,b,c,d"), ',', parts, 2);
        TEST_CHECK(n == 2, "split: hitting max_parts=2 stops at exactly 2 fields");
        TEST_CHECK(sv_eq(parts[0], sv_("a")), "split: the first field before the cap is normal");
        TEST_CHECK(sv_eq(parts[1], sv_("b,c,d")), "split: the last field absorbs everything remaining, not truncated or dropped");
    }
    {
        String_View parts[4];
        size_t n = text_ops_split(sv_("solo"), ',', parts, 4);
        TEST_CHECK(n == 1 && sv_eq(parts[0], sv_("solo")), "split: no delimiter present gives exactly one field, the whole string");
    }

    {
        String_View lines[8];
        size_t n = text_ops_grep_lines(sv_("apple\nbanana\ncherry\napricot\n"), sv_("ap"), lines, 8);
        TEST_CHECK(n == 2, "grep_lines: exactly 2 of 4 lines contain \"ap\"");
        TEST_CHECK(sv_eq(lines[0], sv_("apple")) && sv_eq(lines[1], sv_("apricot")), "grep_lines: the matching lines are the right ones, in order");
    }
    {
        String_View lines[1];
        size_t n = text_ops_grep_lines(sv_("ap\nap\nap\n"), sv_("ap"), lines, 1);
        TEST_CHECK(n == 1, "grep_lines: a max_lines cap of 1 stops after the first match even though more exist");
    }

    print_flush();
    return test_failures_ != 0;
}
