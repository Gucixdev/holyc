/* bench_alloc: our arena and pool against libc malloc/free on the same work.
 *   variable sizes: 1,000,000 blocks of 16-256 bytes, each written in full, then all of them released -
 *                   arena_alloc and one arena_reset against malloc and a free per block
 *   fixed size:     10,000,000 operations on 64-byte objects over 10,000 slots, each a free of an occupied slot
 *                   or an allocation (written in full) into an empty one - pool_alloc/pool_dealloc on a buffer
 *                   from an arena against malloc/free
 * Both arms run the same random sequence; one untimed warmup each, then 9 repetitions that alternate which arm
 * goes first; median, min and max per arm. The contract checks in arena_alloc and pool_alloc stay on (no NDEBUG),
 * as shipped. Time only: both arms share one process, so peak memory is not split between them. */
#define PRINT_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define POOL_ALLOC_IMPLEMENTATION
#include "core/core_base.h"
#include "core/core_arena_alloc.h"
#include "core/core_pool_alloc.h"
#include "bench_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NBLOCKS 1000000
#define NOPS 10000000
#define NSLOTS 10000
#define OBJ 64
#define REPS 9

static u64 rng_;
static u32 rnd_(void)
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return (u32)(rng_ >> 32);
}

static u32 sizes_[NBLOCKS];
static void *ptrs_[NBLOCKS];
static void *slots_[NSLOTS];
static volatile u8 sink_;

static f64 var_arena_(Arena *a)
{
    f64 t0 = bench_now();
    size_t i;
    for (i = 0; i < NBLOCKS; i++) {
        u8 *p = (u8 *)arena_alloc(a, sizes_[i]);
        memset(p, (int)i, sizes_[i]);
        ptrs_[i] = p;
    }
    sink_ ^= *(u8 *)ptrs_[NBLOCKS / 2];
    arena_reset(a);
    return bench_now() - t0;
}

static f64 var_malloc_(void)
{
    f64 t0 = bench_now();
    size_t i;
    for (i = 0; i < NBLOCKS; i++) {
        u8 *p = (u8 *)malloc(sizes_[i]);
        memset(p, (int)i, sizes_[i]);
        ptrs_[i] = p;
    }
    sink_ ^= *(u8 *)ptrs_[NBLOCKS / 2];
    for (i = 0; i < NBLOCKS; i++) free(ptrs_[i]);
    return bench_now() - t0;
}

static f64 fix_pool_(Pool *p)
{
    f64 t0;
    size_t i;
    rng_ = 42;
    t0 = bench_now();
    for (i = 0; i < NOPS; i++) {
        u32 s = rnd_() % NSLOTS;
        if (slots_[s]) { pool_dealloc(p, slots_[s]); slots_[s] = NULL; }
        else { slots_[s] = pool_alloc(p); memset(slots_[s], (int)i, OBJ); }
    }
    for (i = 0; i < NSLOTS; i++) if (slots_[i]) { pool_dealloc(p, slots_[i]); slots_[i] = NULL; }
    return bench_now() - t0;
}

static f64 fix_malloc_(void)
{
    f64 t0;
    size_t i;
    rng_ = 42;
    t0 = bench_now();
    for (i = 0; i < NOPS; i++) {
        u32 s = rnd_() % NSLOTS;
        if (slots_[s]) { free(slots_[s]); slots_[s] = NULL; }
        else { slots_[s] = malloc(OBJ); memset(slots_[s], (int)i, OBJ); }
    }
    for (i = 0; i < NSLOTS; i++) if (slots_[i]) { free(slots_[i]); slots_[i] = NULL; }
    return bench_now() - t0;
}

static void sort_(f64 *t)
{
    size_t i, j;
    for (i = 1; i < REPS; i++) for (j = i; j > 0 && t[j - 1] > t[j]; j--) { f64 x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
}

static void report_(const char *what, const char *ours, f64 *to, f64 *tm, f64 ops)
{
    sort_(to);
    sort_(tm);
    printf("%s\n", what);
    printf("  %-7s median %8.2f ms  (min %8.2f, max %8.2f)  %6.2f ns/op\n", ours, to[REPS / 2] * 1e3, to[0] * 1e3,
           to[REPS - 1] * 1e3, to[REPS / 2] * 1e9 / ops);
    printf("  %-7s median %8.2f ms  (min %8.2f, max %8.2f)  %6.2f ns/op\n", "malloc", tm[REPS / 2] * 1e3, tm[0] * 1e3,
           tm[REPS - 1] * 1e3, tm[REPS / 2] * 1e9 / ops);
    printf("  malloc / %s = %.2fx (medians)\n\n", ours, tm[REPS / 2] / to[REPS / 2]);
}

int main(void)
{
    Arena a = {0}, pa = {0};
    Pool p;
    f64 ta[REPS], tm[REPS], tp[REPS], tf[REPS];
    size_t i;
    rng_ = 7;
    for (i = 0; i < NBLOCKS; i++) sizes_[i] = 16 + rnd_() % 241;
    pool_init(&p, arena_alloc(&pa, (size_t)NSLOTS * OBJ), (size_t)NSLOTS * OBJ, OBJ);
    var_arena_(&a);
    var_malloc_();
    fix_pool_(&p);
    fix_malloc_();
    for (i = 0; i < REPS; i++) {
        if (i & 1) { tm[i] = var_malloc_(); ta[i] = var_arena_(&a); tf[i] = fix_malloc_(); tp[i] = fix_pool_(&p); }
        else { ta[i] = var_arena_(&a); tm[i] = var_malloc_(); tp[i] = fix_pool_(&p); tf[i] = fix_malloc_(); }
    }
    report_("variable sizes: 1M blocks of 16-256 B, each written, then all released", "arena", ta, tm, NBLOCKS);
    report_("fixed size: 10M alloc-or-free operations on 64 B objects over 10k slots", "pool", tp, tf, NOPS);
    arena_free(&a);
    arena_free(&pa);
    return (int)(sink_ & 0);
}
