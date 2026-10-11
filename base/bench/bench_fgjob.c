#define PRINT_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define FGJOB_IMPLEMENTATION
#include "core/print.h"
#include "core/arena_alloc.h"
#include "core/fine_grained.h"
#include "bench_util.h"

static void noop_job(i64 id, void *ctx) { (void)id; (void)ctx; }

/* the design fgjob.h used to use: spawn N pthreads, join, every call - kept
 * here ONLY so the improvement has a live baseline instead of a claim in a
 * comment. Not exported, not used by anything else. */
typedef struct {
    i64 volatile next;
    i64 n;
    Job_Dispatch_Fn fn;
    void *ctx;
} Naive_Arg_;

static void *naive_worker_(void *arg_)
{
    Naive_Arg_ *arg = (Naive_Arg_ *)arg_;
    i64 start, stop, i;
    for (;;) {
        start = __sync_fetch_and_add(&arg->next, FGJOB_CHUNK);
        if (start >= arg->n) break;
        stop = start + FGJOB_CHUNK;
        if (stop > arg->n) stop = arg->n;
        for (i = start; i < stop; i++) arg->fn(i, arg->ctx);
    }
    return NULL;
}

static void naive_run_(i32 n_workers, i64 n, Job_Dispatch_Fn fn, void *ctx)
{
    pthread_t threads[FGJOB_MAX_WORKERS];
    Naive_Arg_ arg;
    i32 i;

    if (n_workers > FGJOB_MAX_WORKERS) n_workers = FGJOB_MAX_WORKERS;
    arg.next = 0;
    arg.n = n;
    arg.fn = fn;
    arg.ctx = ctx;

    for (i = 0; i < n_workers; i++) pthread_create(&threads[i], NULL, naive_worker_, &arg);
    for (i = 0; i < n_workers; i++) pthread_join(threads[i], NULL);
}

int main(void)
{
    Thread_Pool pool;
    i64 sizes[] = { 1, 10, 100, 1000, 10000, 100000 };
    size_t si;
    i32 n_workers;

    thread_pool_init(&pool, 0);
    n_workers = pool.n_workers;
    print_cstr("bench_fgjob: workers="); print_i64(n_workers); print_char('\n');
    print_cstr("naive = spawn N pthreads + join, every call (the old fgjob.h design)\n");
    print_cstr("pool  = persistent condvar-parked workers (the current design)\n\n");

    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        i64 n = sizes[si];
        i64 iters = 500;
        f64 t_naive, t_pool;

        BENCH_TIMEIT(t_naive, iters, { naive_run_(n_workers, n, noop_job, NULL); });
        BENCH_TIMEIT(t_pool, iters, { thread_pool_run(&pool, n, noop_job, NULL); });

        print_cstr("n_jobs="); print_u64((u64)n);
        print_cstr("\tnaive="); print_u64((u64)(t_naive * 1e9)); print_cstr(" ns");
        print_cstr("\tpool="); print_u64((u64)(t_pool * 1e9)); print_cstr(" ns");
        print_cstr("\tspeedup="); print_u64(bench_speedup_pct(t_naive, t_pool)); print_cstr("% (100 = same)\n");
    }

    thread_pool_destroy(&pool);
    print_flush();
    return 0;
}
