/* The arena's in-place realloc, calloc after a rewind, arena_decommit, and the pool's lazy init: the parts that
 * skip work (no copy, no zeroing above the high-water mark, no touching of chunks never used) must still give
 * the same memory a plain version would. Residency is read with mincore. */
#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define POOL_ALLOC_IMPLEMENTATION
#include "core/core_arena_alloc.h"
#include "core/core_pool_alloc.h"
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* pages of [p, p + n) resident in memory */
static size_t resident_(void *p, size_t n)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE), i, k = 0;
    static u8 vec[1 << 15];
    u8 *lo = (u8 *)((size_t)p & ~(page - 1));
    size_t pages = ((size_t)((u8 *)p + n - lo) + page - 1) / page;
    if (pages > sizeof vec || mincore(lo, pages * page, vec) != 0) return (size_t)-1;
    for (i = 0; i < pages; i++) k += vec[i] & 1;
    return k;
}

static int all_zero_(const u8 *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

int main(void)
{
    Arena a = {0}, b = {0};
    Arena_Mark m;
    Pool pool;
    u8 *p, *q, *r, *buf;
    size_t i;

    print_cstr("test_arena_reuse\n");

    p = (u8 *)arena_alloc(&a, 100);
    memset(p, 7, 100);
    q = (u8 *)arena_realloc(&a, p, 100, 4000);
    TEST_CHECK(q == p, "realloc of the last block grows it in place");
    r = (u8 *)arena_alloc(&a, 8);
    q = (u8 *)arena_realloc(&a, p, 4000, 8000);
    TEST_CHECK(q != p && q > r, "realloc of a block that is no longer last moves it past the newer one");
    TEST_CHECK(q[0] == 7 && q[99] == 7, "the moved block keeps its bytes");

    arena_reset(&a);
    m = arena_snapshot(&a);
    p = (u8 *)arena_alloc(&a, 1 << 20);
    memset(p, 0xAB, 1 << 20);
    arena_rewind(&a, m);
    q = (u8 *)arena_calloc(&a, 3 << 19);
    TEST_CHECK(q == p && all_zero_(q, 3 << 19), "calloc over memory written before the rewind and past it is zero");

    arena_reset(&a);
    p = (u8 *)arena_alloc(&a, 8 << 20);
    memset(p, 0xCD, 8 << 20);
    TEST_CHECK(resident_(p, 8 << 20) > 1000, "8 MiB written is resident");
    arena_reset(&a);
    arena_decommit(&a);
    TEST_CHECK(resident_(p, 8 << 20) <= 1, "after reset and decommit at most the first page stays resident");
    q = (u8 *)arena_calloc(&a, 8 << 20);
    TEST_CHECK(q == p && all_zero_(q, 8 << 20), "calloc after decommit is zero");

    buf = (u8 *)arena_alloc(&b, (size_t)64 << 20);
    pool_init(&pool, buf, (size_t)64 << 20, 64);
    TEST_CHECK(resident_(buf, (size_t)64 << 20) <= 1, "pool_init over 64 MiB touches no chunk");
    for (i = 0; i < 10; i++) {
        p = (u8 *)pool_alloc(&pool);
        if (p != buf + i * 64) break;
    }
    TEST_CHECK(i == 10, "fresh chunks come in address order from the start of the buffer");
    pool_dealloc(&pool, p);
    TEST_CHECK(pool_alloc(&pool) == p, "a chunk handed back is the next one handed out");
    pool_free_all(&pool);
    TEST_CHECK(pool_alloc(&pool) == buf, "after pool_free_all the first chunk is the buffer's first");

    arena_free(&a);
    arena_free(&b);
    print_flush();
    return test_failures_ != 0;
}
