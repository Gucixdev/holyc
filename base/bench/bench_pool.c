#define PRINT_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define POOL_ALLOC_IMPLEMENTATION
#include "core/print.h"
#include "core/arena_alloc.h"
#include "core/pool_alloc.c"
#include "bench_util.h"
#include <stdlib.h>

/* Different access pattern than bench_arena on purpose: pool's whole point
 * is individual alloc/dealloc CHURN of same-sized chunks without ever
 * releasing the whole batch at once (think: entity handles, particle
 * slots) - the thing malloc/free is normally used for. Arena can't do this
 * pattern at all (no arena_dealloc exists - that's exactly why pool exists
 * as arena's "yin-yang" complement, not a replacement). */

#define CHUNK_SIZE 32

int main(void)
{
    i64 counts[] = { 1000, 100000, 5000000 };
    size_t ci;

    print_cstr("bench_pool: N alloc/dealloc/alloc churn cycles, fixed 32B chunks\n\n");

    for (ci = 0; ci < sizeof(counts) / sizeof(counts[0]); ci++) {
        i64 n = counts[ci];
        i64 iters = (n < 1000000) ? 50 : 3;
        f64 t_pool, t_malloc;
        void *buf = malloc((size_t)n * CHUNK_SIZE); /* pool's fixed backing buffer, sized for worst case */

        /* pool_init does an O(n) free-list build - a real cost, but a
         * one-time setup cost, not a per-churn one. Paying it inside the
         * timed loop would conflate "build a fresh pool" with "steady-state
         * churn" and unfairly bury pool at large n under setup it wouldn't
         * pay in real use (init once at startup, churn for the program's
         * lifetime). Init once outside the clock. No reset needed between
         * `it` passes either: alloc-one/free-previous always returns N-1 of
         * N chunks to the free list by the time the inner loop ends, same
         * steady state every time. */
        {
            Pool p;
            void *held = NULL;
            pool_init(&p, buf, (size_t)n * CHUNK_SIZE, CHUNK_SIZE);

            BENCH_TIMEIT(t_pool, iters, {
                i64 i;
                for (i = 0; i < n; i++) {
                    void *x = pool_alloc(&p);
                    if (held) pool_dealloc(&p, held); /* churn: alloc one, free the previous */
                    held = x;
                }
            });
        }

        BENCH_TIMEIT(t_malloc, iters, {
            i64 i;
            void *held = NULL;
            for (i = 0; i < n; i++) {
                void *x = malloc(CHUNK_SIZE);
                if (held) free(held);
                held = x;
            }
            free(held);
        });

        print_cstr("n="); print_u64((u64)n);
        print_cstr("  pool="); print_u64((u64)(t_pool * 1e6)); print_cstr(" us");
        print_cstr("  malloc/free="); print_u64((u64)(t_malloc * 1e6)); print_cstr(" us");
        print_cstr("  speedup="); print_u64(bench_speedup_pct(t_malloc, t_pool)); print_cstr("% (100 = same)\n");

        free(buf);
    }

    print_flush();
    return 0;
}
