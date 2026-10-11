#ifndef HOLYC_BENCH_UTIL_H_
#define HOLYC_BENCH_UTIL_H_

/* Tiny shared benchmark helper - same idea as tests/test_util.h's
 * TEST_CHECK, but for the timing boilerplate every bench_*.c file was
 * separately reimplementing: a `now_()` clock_gettime wrapper (byte-
 * for-byte identical across bench_arena.c/bench_morton.c/
 * bench_hashtable.c/etc before this existed) and the "run N iterations,
 * average the elapsed time, turn two timings into a speedup percentage"
 * pattern every single bench file hand-wrote from scratch.
 *
 * Deliberately NOT a do-everything harness that also owns printing -
 * every bench's actual headline (what units, what's on the line, "ns/op"
 * vs "us" vs "queries/s") varies too much to force through one rigid
 * print function, and `./bench`'s own scraping only cares about a
 * `speedup=NUM%` substring existing somewhere in the output, not its
 * surrounding formatting - so this stays two small, independent, easy-
 * to-swap-or-drop pieces (a timer, a percentage calculation + a timing-
 * loop macro), not a framework a bench file has to buy into wholesale. */

#include "types/base.h"
#include <time.h>

static f64 bench_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

/* runs `code` exactly `iters` times back to back, then sets `out_seconds`
 * to the average per-iteration elapsed time - replaces the "t0=now();
 * for(...){code} t1=now(); avg=(t1-t0)/iters;" block every bench file
 * used to hand-write. `code` is a brace-delimited statement block, e.g.:
 *
 *   f64 t_naive;
 *   BENCH_TIMEIT(t_naive, iters, { sink += naive_get_(...); });
 *
 * Uses its own `bench_timeit_it_` loop variable (not `i`) so it never
 * shadows a loop variable the caller's own `code` block might use. */
#define BENCH_TIMEIT(out_seconds, iters, code)                     \
    do {                                                           \
        i64 bench_timeit_it_;                                      \
        f64 bench_timeit_t0_ = bench_now();                        \
        for (bench_timeit_it_ = 0; bench_timeit_it_ < (iters); bench_timeit_it_++) { \
            code                                                   \
        }                                                          \
        (out_seconds) = (bench_now() - bench_timeit_t0_) / (f64)(iters); \
    } while (0)

/* the repeated "(u64)(baseline/fast*100)" percentage calculation every
 * bench file's own final print line duplicated - 100 = tied, bigger =
 * fast is better, matching every existing bench's own convention and
 * what `./bench`'s speedup=NUM% scraping expects. fast==0 is guarded
 * (a suspiciously-optimized-away measurement) by returning 0 rather
 * than dividing by zero - a real, if rare, measurement-noise case on
 * very fast operations at low iteration counts. */
static u64 bench_speedup_pct(f64 baseline_seconds, f64 fast_seconds)
{
    if (fast_seconds <= 0.0) return 0;
    return (u64)(baseline_seconds / fast_seconds * 100.0);
}

#endif /* HOLYC_BENCH_UTIL_H_ */
