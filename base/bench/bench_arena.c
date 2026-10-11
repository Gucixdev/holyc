#define PRINT_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#include "util/print.h"
#include "alloc/arena_alloc.h"
#include "bench_util.h"
#include <stdlib.h>

/* manifest line 13: "arena/pool - brak leakow bo nie ma malloc". That's a
 * claim, not a fact until it's measured against the thing it replaces -
 * this bench is exactly that comparison, not just our own numbers in
 * isolation. Workload: allocate N small objects, then release all of them.
 * That release is the whole point of the comparison - arena_reset() is one
 * O(#regions) pass; malloc's equivalent is N individual free() calls,
 * which is the realistic thing you'd actually write against malloc for
 * the same "allocate a batch, then release the batch" access pattern. */

typedef struct { i32 a, b, c, d; } Small_Obj; /* 16 bytes - typical AST-node-sized allocation */

/* arena_alloc.h's default backend is ARENA_BACKEND_FLAT: one big mmap()
 * (MAP_NORESERVE, lazily committed by the kernel page by page) reserved
 * once, then a pure bump pointer - "pamiec jako jedna wielka tablica" per
 * the manifest, no glibc heap involved at all in the common case.
 *
 * Getting here took a wrong turn worth keeping visible: a "geometric vs
 * flat region size" comparison (both still routed through glibc malloc)
 * gave THREE different answers across three re-runs on this exact machine
 * - 2.7x one way, 40% the other way, then a 3-9% reversal, all for
 * "the same" ~512MB workload. Chasing which one was "true" was the wrong
 * question. Instrumenting it (AnonHugePages via /proc/self/smaps_rollup,
 * running each variant in a fresh process instead of back-to-back in one)
 * found the actual mechanism: repeated alloc/free cycles of the SAME size
 * warm glibc's per-size-class free list, so a benchmark that loops
 * "allocate a batch, free it, repeat" - which is exactly what a
 * micro-benchmark does, and exactly what this one did - gets faster on
 * later trials purely from glibc-internal caching, and that warm-up effect
 * is much stronger for a single uniform allocation size (flat) than for a
 * spread of sizes (geometric). It was never really "flat vs geometric" -
 * it was "how well does this specific size pattern happen to fit glibc's
 * free-list caching today", which is exactly the kind of question a
 * flat-array design doesn't have to answer at all.
 *
 * This local variant reproduces the OLD default (ARENA_BACKEND_LIBC_MALLOC,
 * flat 64KB regions) so that claim - "bypassing glibc removes the
 * variance, not just changes the average" - has a live comparison instead
 * of being asserted. Not exported, not used by anything else. */
static void *arena_alloc_glibc_(Arena *a, size_t size_bytes)
{
    size_t size = (size_bytes + sizeof(u64) - 1) / sizeof(u64);
    void *result;

    if (a->end == NULL) {
        size_t capacity = ARENA_REGION_DEFAULT_CAPACITY;
        if (capacity < size) capacity = size;
        a->end = (Arena_Region *)malloc(offsetof(Arena_Region, data) + sizeof(u64) * capacity);
        a->end->next = NULL; a->end->count = 0; a->end->capacity = capacity;
        a->begin = a->end;
    }
    while (a->end->count + size > a->end->capacity && a->end->next != NULL) a->end = a->end->next;
    if (a->end->count + size > a->end->capacity) {
        size_t capacity = ARENA_REGION_DEFAULT_CAPACITY;
        if (capacity < size) capacity = size;
        a->end->next = (Arena_Region *)malloc(offsetof(Arena_Region, data) + sizeof(u64) * capacity);
        a->end = a->end->next;
        a->end->next = NULL; a->end->count = 0; a->end->capacity = capacity;
    }
    result = &a->end->data[a->end->count];
    a->end->count += size;
    return result;
}

static void arena_free_glibc_(Arena *a)
{
    Arena_Region *r = a->begin;
    while (r) { Arena_Region *n = r->next; free(r); r = n; }
}

int main(void)
{
    i64 sizes[] = { 100, 10000, 1000000 };
    size_t si;

    print_cstr("bench_arena: alloc N Small_Obj (16B), then release all N\n\n");

    for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
        i64 n = sizes[si];
        i64 iters = (n < 100000) ? 200 : 5;
        f64 t_arena, t_malloc;

        BENCH_TIMEIT(t_arena, iters, {
            Arena a = {0};
            i64 i;
            for (i = 0; i < n; i++) {
                Small_Obj *o = (Small_Obj *)arena_alloc(&a, sizeof(Small_Obj));
                o->a = (i32)i;
            }
            arena_free(&a);
        });

        BENCH_TIMEIT(t_malloc, iters, {
            Small_Obj **ptrs = (Small_Obj **)malloc(sizeof(Small_Obj *) * (size_t)n);
            i64 i;
            for (i = 0; i < n; i++) {
                ptrs[i] = (Small_Obj *)malloc(sizeof(Small_Obj));
                ptrs[i]->a = (i32)i;
            }
            for (i = 0; i < n; i++) free(ptrs[i]);
            free(ptrs);
        });

        print_cstr("n="); print_u64((u64)n);
        print_cstr("  arena="); print_u64((u64)(t_arena * 1e6)); print_cstr(" us");
        print_cstr("  malloc+free="); print_u64((u64)(t_malloc * 1e6)); print_cstr(" us");
        print_cstr("  speedup="); print_u64(bench_speedup_pct(t_malloc, t_arena)); print_cstr("% (100 = same)\n");
    }

    /* ARENA_BACKEND_FLAT (shipped default) vs ARENA_BACKEND_LIBC_MALLOC
     * (the old default) - same N*64B-allocations-with-real-writes workload,
     * 5 trials each. The claim under test isn't "flat is N% faster" (any
     * single such number from this history has already flipped 3 times) -
     * it's "flat has near-zero trial-to-trial variance because it never
     * asks glibc for anything, where glibc-routed allocation visibly warms
     * up between trials". Look at the spread across trials, not just the
     * average. */
    print_cstr("\nARENA_BACKEND_FLAT (shipped) vs ARENA_BACKEND_LIBC_MALLOC (old default), N * 64B allocations, memory actually written\n\n");
    {
        i64 n = 8000000; /* ~512MB */
        int trial, n_trials = 5;

        print_cstr("n="); print_u64((u64)n);
        print_cstr(" (~"); print_u64((u64)(n * 64 / 1024 / 1024)); print_cstr("MB), "); print_i64(n_trials); print_cstr(" trials each\n");

        for (trial = 0; trial < n_trials; trial++) {
            f64 t0, t1, t_flat, t_glibc;
            i64 i;

            {
                Arena a = {0};
                t0 = bench_now();
                for (i = 0; i < n; i++) {
                    char *p = (char *)arena_alloc(&a, 64);
                    memset(p, (int)(i & 0xff), 64);
                }
                t1 = bench_now();
                t_flat = t1 - t0;
                arena_free(&a);
            }

            {
                Arena a = {0};
                t0 = bench_now();
                for (i = 0; i < n; i++) {
                    char *p = (char *)arena_alloc_glibc_(&a, 64);
                    memset(p, (int)(i & 0xff), 64);
                }
                t1 = bench_now();
                t_glibc = t1 - t0;
                arena_free_glibc_(&a);
            }

            print_cstr("  trial "); print_i64(trial);
            print_cstr("  flat="); print_u64((u64)(t_flat * 1000)); print_cstr("ms");
            print_cstr("  glibc="); print_u64((u64)(t_glibc * 1000)); print_cstr("ms\n");
        }
    }

    print_flush();
    return 0;
}
