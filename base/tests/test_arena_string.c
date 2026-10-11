#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define ARENA_STRING_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "core/base.h"

#include <string.h>

int main(void)
{
    Arena a = {0};
    Arena_String s;

    print_cstr("test_arena_string\n");

    arena_string_init(&s, &a);
    TEST_CHECK(s.count == 0, "init: starts empty");

    arena_string_append_char(&s, 'h');
    arena_string_append_char(&s, 'i');
    TEST_CHECK(s.count == 2, "append_char: count grows by 1 each call");
    TEST_CHECK(strncmp(s.items, "hi", 2) == 0, "append_char: bytes match");

    arena_string_append_cstr(&s, " there");
    TEST_CHECK(s.count == 8, "append_cstr: count grows by strlen");
    TEST_CHECK(strncmp(s.items, "hi there", 8) == 0, "append_cstr: bytes match");

    {
        const char *c = arena_string_cstr(&s);
        TEST_CHECK(strcmp(c, "hi there") == 0, "cstr: NUL-terminated, readable as a normal C string");
        TEST_CHECK(s.count == 8, "cstr: does not count the trailing NUL");
    }

    arena_string_append_buf(&s, "!!!extra", 3);
    TEST_CHECK(s.count == 11, "append_buf: appends only n bytes, grows over the previous NUL");
    TEST_CHECK(strncmp(s.items, "hi there!!!", 11) == 0, "append_buf: bytes match, old NUL overwritten");

    arena_string_clear(&s);
    TEST_CHECK(s.count == 0, "clear: count resets to 0");
    TEST_CHECK(s.capacity > 0, "clear: capacity/backing storage untouched (reuse, not free)");

    arena_string_append_u64(&s, 0);
    TEST_CHECK(s.count == 1 && s.items[0] == '0', "append_u64: zero renders as a single '0'");

    arena_string_clear(&s);
    arena_string_append_u64(&s, 12345);
    TEST_CHECK(s.count == 5 && strncmp(s.items, "12345", 5) == 0, "append_u64: multi-digit value");

    arena_string_clear(&s);
    arena_string_append_u64(&s, 100);
    TEST_CHECK(s.count == 3 && strncmp(s.items, "100", 3) == 0, "append_u64: boundary crossing 99->100 (base-10 fast path)");

    arena_string_clear(&s);
    arena_string_append_i64(&s, -42);
    TEST_CHECK(s.count == 3 && strncmp(s.items, "-42", 3) == 0, "append_i64: negative value gets a leading '-'");

    arena_string_clear(&s);
    arena_string_append_i64(&s, 0);
    TEST_CHECK(s.count == 1 && s.items[0] == '0', "append_i64: zero has no leading '-'");

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
