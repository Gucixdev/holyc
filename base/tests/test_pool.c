#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define POOL_ALLOC_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "core/pool_alloc.h"

int main(void)
{
    Arena a = {0};
    Pool pool;
    void *mem = arena_alloc(&a, 8 * 32);
    void *p1, *p2, *p3;

    print_cstr("test_pool\n");

    pool_init(&pool, mem, 8 * 32, 32);

    p1 = pool_alloc(&pool);
    p2 = pool_alloc(&pool);
    TEST_CHECK(p1 != p2, "two allocations return distinct chunks");

    pool_dealloc(&pool, p1);
    p3 = pool_alloc(&pool);
    TEST_CHECK(p3 == p1, "dealloc'd chunk is reused (LIFO free list)");

    pool_free_all(&pool);
    p1 = pool_alloc(&pool);
    TEST_CHECK(p1 != NULL, "pool_free_all resets the free list");

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
