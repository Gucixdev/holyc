#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define POOL_ALLOC_IMPLEMENTATION
#define FGJOB_IMPLEMENTATION
#define REF_COUNT_IMPLEMENTATION
#include "arena_alloc.h"
#include "pool_alloc.c"
#include "fine_grained.h"
#include "ref_count.h"

static i32 on_zero_calls_ = 0;
static void on_zero_(void *ctx) { (void)ctx; on_zero_calls_++; }

/* the composition the header docs itself: retain/release around a
 * pool-allocated chunk, on_zero firing pool_dealloc so the chunk goes back
 * to the free list exactly once, whichever release call happened to be
 * the last one */
typedef struct {
    Ref_Count rc;
    Pool *pool;
    void *chunk;
} Pooled_Handle;

static void pooled_on_zero_(void *ctx)
{
    Pooled_Handle *h = (Pooled_Handle *)ctx;
    pool_dealloc(h->pool, h->chunk);
}

/* stress: N threads each retain then release the same Ref_Count many
 * times, concurrently. If retain/release weren't actually atomic this
 * would corrupt the count under real contention. */
#define STRESS_THREADS_ITERS 20000
static Ref_Count stress_rc_;
static void stress_job_(i64 id, void *ctx)
{
    i64 i;
    (void)ctx; (void)id;
    for (i = 0; i < STRESS_THREADS_ITERS; i++) {
        rc_retain(&stress_rc_);
        rc_release(&stress_rc_);
    }
}

int main(void)
{
    Ref_Count rc;
    i32 r1, r2, r3;

    print_cstr("test_ref_count\n");

    on_zero_calls_ = 0;
    rc_init(&rc, on_zero_, NULL);
    TEST_CHECK(rc_count(&rc) == 1, "rc_init starts at count 1 (the creator's own reference)");

    rc_retain(&rc);
    rc_retain(&rc);
    TEST_CHECK(rc_count(&rc) == 3, "rc_retain increments count");

    r1 = rc_release(&rc);
    TEST_CHECK(!r1 && rc_count(&rc) == 2, "release above zero doesn't fire on_zero, count drops by 1");

    r2 = rc_release(&rc);
    TEST_CHECK(!r2 && rc_count(&rc) == 1, "still not zero after second release");

    r3 = rc_release(&rc);
    TEST_CHECK(r3 && rc_count(&rc) == 0, "the release that hits zero returns 1 (fired on_zero)");
    TEST_CHECK(on_zero_calls_ == 1, "on_zero fired exactly once, not zero times or more than once");

    /* composition with pool: on_zero calls pool_dealloc, not something the
     * caller has to remember to do itself after the last release */
    {
        Arena a = {0};
        Pool pool;
        void *mem = arena_alloc(&a, 4 * 32);
        Pooled_Handle h;
        void *other_chunk;

        pool_init(&pool, mem, 4 * 32, 32);
        h.pool = &pool;
        h.chunk = pool_alloc(&pool);
        rc_init(&h.rc, pooled_on_zero_, &h);

        rc_retain(&h.rc);
        rc_release(&h.rc);
        other_chunk = pool_alloc(&pool);
        TEST_CHECK(other_chunk != h.chunk, "chunk not yet freed while a reference is still outstanding (count==1)");
        pool_dealloc(&pool, other_chunk); /* give it back so the next check has a clean pool state */

        rc_release(&h.rc); /* drops to 0, fires pooled_on_zero_ -> pool_dealloc(h.chunk) */
        other_chunk = pool_alloc(&pool);
        TEST_CHECK(other_chunk == h.chunk, "the chunk came back to the free list exactly when the last reference released");

        arena_free(&a);
    }

    /* concurrent retain/release from real threads must not corrupt the count */
    {
        Thread_Pool pool;
        i32 n_workers;

        rc_init(&stress_rc_, NULL, NULL);
        thread_pool_init(&pool, 0);
        n_workers = pool.n_workers;
        if (n_workers < 2) n_workers = 2; /* force real concurrency even on a 1-core CI box */

        thread_pool_run(&pool, n_workers, stress_job_, NULL);
        thread_pool_destroy(&pool);

        TEST_CHECK(rc_count(&stress_rc_) == 1,
                   "count survives many concurrent retain/release pairs from multiple threads unchanged (still 1)");
    }

    print_flush();
    return test_failures_ != 0;
}
