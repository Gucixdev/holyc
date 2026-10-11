#include "test_common.h"
#define STRING_VIEW_IMPLEMENTATION
#include "str.h"

static i32 is_digit_(char c) { return c >= '0' && c <= '9'; }

int main(void)
{
    String_View sv, chunk;

    print_cstr("test_string_view\n");

    sv = sv_from_cstr("hello world");
    TEST_CHECK(sv.count == 11, "sv_from_cstr counts correctly");

    TEST_CHECK(sv_eq(sv_trim(sv_from_cstr("  padded  ")), SV("padded")), "trim strips both sides");
    TEST_CHECK(sv_eq(sv_trim_left(sv_from_cstr("  x")), SV("x")), "trim_left strips only the left");
    TEST_CHECK(sv_eq(sv_trim_right(sv_from_cstr("x  ")), SV("x")), "trim_right strips only the right");

    sv = sv_from_cstr("a,bb,ccc");
    chunk = sv_chop_by_delim(&sv, ',');
    TEST_CHECK(sv_eq(chunk, SV("a")), "first chop_by_delim yields the first field");
    chunk = sv_chop_by_delim(&sv, ',');
    TEST_CHECK(sv_eq(chunk, SV("bb")), "second chop_by_delim yields the second field");
    chunk = sv_chop_by_delim(&sv, ',');
    TEST_CHECK(sv_eq(chunk, SV("ccc")), "final chop_by_delim yields the last field, no trailing delim needed");

    sv = sv_from_cstr("a=b");
    TEST_CHECK(sv_try_chop_by_delim(&sv, '=', &chunk) == 1, "try_chop_by_delim finds the delimiter");
    TEST_CHECK(sv_eq(chunk, SV("a")) && sv_eq(sv, SV("b")), "try_chop_by_delim splits both sides correctly");
    sv = sv_from_cstr("noequals");
    TEST_CHECK(sv_try_chop_by_delim(&sv, '=', &chunk) == 0, "try_chop_by_delim reports failure when delimiter is absent");

    sv = sv_from_cstr("one::two::three");
    chunk = sv_chop_by_sv(&sv, SV("::"));
    TEST_CHECK(sv_eq(chunk, SV("one")), "chop_by_sv splits on a multi-char delimiter");
    chunk = sv_chop_by_sv(&sv, SV("::"));
    TEST_CHECK(sv_eq(chunk, SV("two")), "chop_by_sv finds the second occurrence");
    TEST_CHECK(sv_eq(sv, SV("three")), "chop_by_sv leaves the remainder after the last delimiter");

    TEST_CHECK(sv_starts_with(SV("hello world"), SV("hello")), "starts_with true case");
    TEST_CHECK(!sv_starts_with(SV("hello"), SV("hello world")), "starts_with false when prefix is longer than the string");
    TEST_CHECK(sv_ends_with(SV("hello world"), SV("world")), "ends_with true case");
    TEST_CHECK(!sv_ends_with(SV("world"), SV("hello world")), "ends_with false when suffix is longer than the string");

    TEST_CHECK(sv_eq_ignorecase(SV("HeLLo"), SV("hello")), "eq_ignorecase matches mixed case");
    TEST_CHECK(!sv_eq_ignorecase(SV("hello"), SV("world")), "eq_ignorecase still rejects different words");

    TEST_CHECK(sv_to_u64(SV("12345")) == 12345, "sv_to_u64 parses a plain number");
    TEST_CHECK(sv_to_u64(SV("42abc")) == 42, "sv_to_u64 stops at the first non-digit");

    sv = sv_from_cstr("99rest");
    TEST_CHECK(sv_chop_u64(&sv) == 99, "sv_chop_u64 parses the numeric prefix");
    TEST_CHECK(sv_eq(sv, SV("rest")), "sv_chop_u64 leaves the non-numeric remainder in place");

    {
        String_View taken = sv_take_left_while(SV("123abc"), is_digit_);
        TEST_CHECK(sv_eq(taken, SV("123")), "take_left_while stops at the first non-matching char");
    }
    {
        sv = sv_from_cstr("123abc");
        chunk = sv_chop_left_while(&sv, is_digit_);
        TEST_CHECK(sv_eq(chunk, SV("123")) && sv_eq(sv, SV("abc")), "chop_left_while advances the source view");
    }

    {
        size_t idx;
        TEST_CHECK(sv_index_of(SV("hello"), 'l', &idx) && idx == 2, "index_of finds the first occurrence");
        TEST_CHECK(!sv_index_of(SV("hello"), 'z', &idx), "index_of reports absence correctly");
    }

    print_flush();
    return test_failures_ != 0;
}
