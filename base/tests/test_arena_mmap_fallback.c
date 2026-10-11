#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
/* deliberately NOT overriding ARENA_FLAT_RESERVE_BYTES here - this test
 * exists specifically to exercise the real 1GB default reservation failing
 * and falling back to an exact-size request, which test_arena.c's small
 * override would never trigger. */
#include "arena_alloc.h"

#include <sys/resource.h>

int main(void)
{
    struct rlimit rl;
    Arena a = {0};
    i32 all_ok;
    i32 i;

    print_cstr("test_arena_mmap_fallback\n");

    /* 100MB virtual address space cap - well under the 1GB default
     * reservation, so new_region()'s first mmap() must fail and the
     * fallback-to-exact-size retry must be what actually makes this work.
     * Without the fallback fix, this crashes on a MAP_FAILED deref instead
     * of failing a TEST_CHECK - that distinction matters, it is exactly
     * the "assert() compiles out under NDEBUG" scenario the fix targets. */
    rl.rlim_cur = 100 * 1024 * 1024;
    rl.rlim_max = 100 * 1024 * 1024;
    TEST_CHECK(setrlimit(RLIMIT_AS, &rl) == 0, "setrlimit(RLIMIT_AS, 100MB) applied");

    all_ok = 1;
    for (i = 0; i < 1000; i++) {
        i32 *p = (i32 *)arena_alloc(&a, sizeof(i32));
        if (!p) { all_ok = 0; break; }
        *p = i;
    }
    TEST_CHECK(all_ok, "arena_alloc still works when the 1GB over-reservation fails and falls back");

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
