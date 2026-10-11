#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define STR_IMPLEMENTATION
#define SUFFIX_IMPLEMENTATION
#include "core/base.h"
#include "data/suffix.h"
#include <string.h> /* strlen/strncmp - test-only convenience, matches test_qoi.c's own precedent */

int main(void)
{
    Arena arena = {0};
    Suffix_Array sa;
    static const char *text = "banana";
    size_t pos;

    print_cstr("test_suffix\n");

    suffix_build(&arena, &sa, text, strlen(text));

    TEST_CHECK(suffix_search(&sa, "ana", 3, &pos) == 1, "finds a real substring");
    TEST_CHECK(pos == 1 || pos == 3, "found position is one of the two real occurrences of 'ana'");
    TEST_CHECK(strncmp(text + pos, "ana", 3) == 0, "the reported position genuinely starts the pattern in the source text");

    TEST_CHECK(suffix_search(&sa, "banana", 6, &pos) == 1, "finds the whole text as its own substring");
    TEST_CHECK(pos == 0, "whole-text match starts at position 0");

    TEST_CHECK(suffix_search(&sa, "a", 1, &pos) == 1, "finds a single-character substring");
    TEST_CHECK(suffix_search(&sa, "nan", 3, &pos) == 1, "finds a middle substring");

    TEST_CHECK(suffix_search(&sa, "xyz", 3, NULL) == 0, "an absent substring is correctly not found");
    TEST_CHECK(suffix_search(&sa, "bananas", 7, NULL) == 0, "a string longer than any real match (proper superstring) is not found");
    TEST_CHECK(suffix_search(&sa, "", 0, NULL) == 0, "an empty pattern is a defined non-match, not undefined behavior");

    {
        Suffix_Array empty_sa;
        suffix_build(&arena, &empty_sa, "", 0);
        TEST_CHECK(suffix_search(&empty_sa, "a", 1, NULL) == 0, "searching an empty indexed text is safe and finds nothing");
    }

    print_flush();
    return test_failures_ != 0;
}
