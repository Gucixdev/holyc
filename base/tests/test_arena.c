#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
/* the default backend (ARENA_BACKEND_FLAT) reserves 1GB up front, so 2000
 * i32s wouldn't cross a region boundary at all under the real default -
 * force both region-sizing knobs tiny here so this test actually exercises
 * the growth-across-regions path it claims to, on the real shipped
 * backend, not a different one swapped in just to make the test work. */
#define ARENA_REGION_DEFAULT_CAPACITY 64
#define ARENA_FLAT_RESERVE_BYTES 512
#include "arena_alloc.h"

typedef struct { i32 *items; size_t count; size_t capacity; } I32_DA_;

int main(void)
{
    Arena a = {0};
    Arena_Mark mark;
    char *s;
    I32_DA_ da = {0, 0, 0};
    i32 i;

    print_cstr("test_arena\n");

    s = arena_sprintf(&a, "hello %d %s", 42, "world");
    TEST_CHECK(s[0] == 'h' && s[6] == '4', "arena_sprintf basic formatting");

    {
        char *dup = arena_strdup(&a, "copy me");
        TEST_CHECK(dup[0] == 'c' && dup[6] == 'e', "arena_strdup copies content");
    }

    mark = arena_snapshot(&a);
    for (i = 0; i < 2000; i++) arena_da_append(&a, &da, i * i);
    TEST_CHECK(da.count == 2000, "arena_da_append grows across region boundary");
    TEST_CHECK(da.items[1999] == 1999 * 1999, "arena_da_append preserves values");

    arena_rewind(&a, mark);
    TEST_CHECK(arena_snapshot(&a).count == mark.count, "arena_rewind returns to snapshot position");

    /* ARENA_ALIGNOF: the classic offsetof(struct{char;T;},T) trick should
     * match every type's real alignment requirement */
    TEST_CHECK(ARENA_ALIGNOF(char) == 1, "ARENA_ALIGNOF(char) == 1");
    TEST_CHECK(ARENA_ALIGNOF(i32) == 4, "ARENA_ALIGNOF(i32) == 4");
    TEST_CHECK(ARENA_ALIGNOF(f64) == 8, "ARENA_ALIGNOF(f64) == 8 (matches _Alignof on this platform)");

    /* arena_alloc_aligned must hand back addresses that are actual
     * multiples of the requested alignment, not just "probably fine" -
     * covers SIMD widths (16 = SSE, 32 = AVX2) as well as small ones */
    {
        size_t aligns[] = { 1, 2, 4, 8, 16, 32, 64 };
        i32 all_aligned = 1;
        size_t ai;
        for (ai = 0; ai < sizeof(aligns) / sizeof(aligns[0]); ai++) {
            void *p = arena_alloc_aligned(&a, 17, aligns[ai]); /* odd size on purpose - alignment shouldn't care */
            if (((size_t)p % aligns[ai]) != 0) all_aligned = 0;
        }
        TEST_CHECK(all_aligned, "arena_alloc_aligned returns a properly aligned address for every power-of-2 alignment tested");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
