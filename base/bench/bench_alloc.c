/* bench_alloc: our arena and pool against libc malloc/free on the same work, one line per workload.
 *   blocks:   1,000,000 blocks of 16-256 bytes, each written in full, then all released - arena_alloc and one
 *             arena_reset against malloc and a free per block
 *   churn:    10,000,000 operations on 64-byte objects over 10,000 slots, each a free of an occupied slot or an
 *             allocation written in full - pool_alloc/pool_dealloc against malloc/free
 *   grow:     an array of 20,000,000 u32 grown one element at a time - arena_da_append against realloc doubling
 *   fresh:    a new arena per run: 4,096 blocks of 64 KiB, each written, then all released - the cost of
 *             committing memory the first time; malloc_trim(0) after the frees makes glibc give its heap back
 *             too, or it would reuse pages already committed
 *   calloc:   64 MiB zeroed in a fresh arena, then one byte per 4 KiB page read - arena_calloc against calloc
 *   bigpool:  pool_init over 64 MiB of 64-byte chunks, then 1,000 chunks handed out and written - against 1,000
 *             mallocs
 * Both arms run the same sequence; one untimed warmup each, then REPS repetitions that alternate which arm goes
 * first; median, min and max per arm. The contract checks in arena_alloc and pool_alloc stay on (no NDEBUG), as
 * shipped. Time only: both arms share one process, so peak memory is not split between them.
 * `bench_alloc NAME` runs one workload. */
#define PRINT_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define POOL_ALLOC_IMPLEMENTATION
#include "core_base.h"
#include "core_arena_alloc.h"
#include "core_pool_alloc.h"
#include "bench_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h> /* malloc_trim */

#define NBLOCKS 1000000
#define NOPS 10000000
#define NSLOTS 10000
#define OBJ 64
#define NGROW 20000000
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
static Arena arena_, pool_arena_;
static Pool pool_;

typedef struct { u32 *items; size_t count, capacity; } U32s;

static void blocks_arena_(void)
{
    size_t i;
    for (i = 0; i < NBLOCKS; i++) {
        u8 *p = (u8 *)arena_alloc(&arena_, sizes_[i]);
        memset(p, (int)i, sizes_[i]);
        ptrs_[i] = p;
    }
    sink_ ^= *(u8 *)ptrs_[NBLOCKS / 2];
    arena_reset(&arena_);
}

static void blocks_malloc_(void)
{
    size_t i;
    for (i = 0; i < NBLOCKS; i++) {
        u8 *p = (u8 *)malloc(sizes_[i]);
        memset(p, (int)i, sizes_[i]);
        ptrs_[i] = p;
    }
    sink_ ^= *(u8 *)ptrs_[NBLOCKS / 2];
    for (i = 0; i < NBLOCKS; i++) free(ptrs_[i]);
}

static void churn_pool_(void)
{
    size_t i;
    rng_ = 42;
    for (i = 0; i < NOPS; i++) {
        u32 s = rnd_() % NSLOTS;
        if (slots_[s]) { pool_dealloc(&pool_, slots_[s]); slots_[s] = NULL; }
        else { slots_[s] = pool_alloc(&pool_); memset(slots_[s], (int)i, OBJ); }
    }
    for (i = 0; i < NSLOTS; i++) if (slots_[i]) { pool_dealloc(&pool_, slots_[i]); slots_[i] = NULL; }
}

static void churn_malloc_(void)
{
    size_t i;
    rng_ = 42;
    for (i = 0; i < NOPS; i++) {
        u32 s = rnd_() % NSLOTS;
        if (slots_[s]) { free(slots_[s]); slots_[s] = NULL; }
        else { slots_[s] = malloc(OBJ); memset(slots_[s], (int)i, OBJ); }
    }
    for (i = 0; i < NSLOTS; i++) if (slots_[i]) { free(slots_[i]); slots_[i] = NULL; }
}

static void grow_arena_(void)
{
    U32s v = {0};
    u32 i;
    for (i = 0; i < NGROW; i++) arena_da_append(&arena_, &v, i);
    sink_ ^= (u8)v.items[NGROW / 3];
    arena_reset(&arena_);
}

static void grow_malloc_(void)
{
    U32s v = {0};
    u32 i;
    for (i = 0; i < NGROW; i++) {
        if (v.count == v.capacity) {
            v.capacity = v.capacity ? v.capacity * 2 : 256;
            v.items = (u32 *)realloc(v.items, v.capacity * sizeof *v.items);
        }
        v.items[v.count++] = i;
    }
    sink_ ^= (u8)v.items[NGROW / 3];
    free(v.items);
}

static void fresh_arena_(void)
{
    Arena a = {0};
    size_t i;
    for (i = 0; i < 4096; i++) memset(arena_alloc(&a, 65536), (int)i, 65536);
    arena_free(&a);
}

static void fresh_malloc_(void)
{
    size_t i;
    for (i = 0; i < 4096; i++) { ptrs_[i] = malloc(65536); memset(ptrs_[i], (int)i, 65536); }
    for (i = 0; i < 4096; i++) free(ptrs_[i]);
    malloc_trim(0);
}

static void calloc_arena_(void)
{
    Arena a = {0};
    u8 *p = (u8 *)arena_calloc(&a, (size_t)64 << 20);
    size_t i;
    for (i = 0; i < ((size_t)64 << 20); i += 4096) sink_ ^= p[i];
    arena_free(&a);
}

static void calloc_malloc_(void)
{
    u8 *p = (u8 *)calloc((size_t)64 << 20, 1);
    size_t i;
    for (i = 0; i < ((size_t)64 << 20); i += 4096) sink_ ^= p[i];
    free(p);
}

static void bigpool_pool_(void)
{
    Arena a = {0};
    Pool p;
    size_t i;
    pool_init(&p, arena_alloc(&a, (size_t)64 << 20), (size_t)64 << 20, OBJ);
    for (i = 0; i < 1000; i++) memset(pool_alloc(&p), (int)i, OBJ);
    arena_free(&a);
}

static void bigpool_malloc_(void)
{
    size_t i;
    for (i = 0; i < 1000; i++) { ptrs_[i] = malloc(OBJ); memset(ptrs_[i], (int)i, OBJ); }
    for (i = 0; i < 1000; i++) free(ptrs_[i]);
}

static f64 time_(void (*f)(void))
{
    f64 t0 = bench_now();
    f();
    return bench_now() - t0;
}

static void sort_(f64 *t, size_t n)
{
    size_t i, j;
    for (i = 1; i < n; i++) for (j = i; j > 0 && t[j - 1] > t[j]; j--) { f64 x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
}

static void run_(const char *name, const char *ours, void (*fo)(void), void (*fm)(void), size_t reps)
{
    f64 to[REPS], tm[REPS];
    size_t i;
    fo();
    fm();
    for (i = 0; i < reps; i++) {
        if (i & 1) { tm[i] = time_(fm); to[i] = time_(fo); }
        else { to[i] = time_(fo); tm[i] = time_(fm); }
    }
    sort_(to, reps);
    sort_(tm, reps);
    printf("%-8s %-6s %9.3f ms (%9.3f-%9.3f)   malloc %9.3f ms (%9.3f-%9.3f)   malloc/%s %6.2fx\n", name, ours,
           to[reps / 2] * 1e3, to[0] * 1e3, to[reps - 1] * 1e3, tm[reps / 2] * 1e3, tm[0] * 1e3, tm[reps - 1] * 1e3,
           ours, tm[reps / 2] / to[reps / 2]);
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : "";
    size_t i;
    rng_ = 7;
    for (i = 0; i < NBLOCKS; i++) sizes_[i] = 16 + rnd_() % 241;
    pool_init(&pool_, arena_alloc(&pool_arena_, (size_t)NSLOTS * OBJ), (size_t)NSLOTS * OBJ, OBJ);
    if (!*only || !strcmp(only, "blocks")) run_("blocks", "arena", blocks_arena_, blocks_malloc_, REPS);
    if (!*only || !strcmp(only, "churn")) run_("churn", "pool", churn_pool_, churn_malloc_, REPS);
    if (!*only || !strcmp(only, "grow")) run_("grow", "arena", grow_arena_, grow_malloc_, REPS);
    if (!*only || !strcmp(only, "fresh")) run_("fresh", "arena", fresh_arena_, fresh_malloc_, 5);
    if (!*only || !strcmp(only, "calloc")) run_("calloc", "arena", calloc_arena_, calloc_malloc_, REPS);
    if (!*only || !strcmp(only, "bigpool")) run_("bigpool", "pool", bigpool_pool_, bigpool_malloc_, REPS);
    arena_free(&arena_);
    arena_free(&pool_arena_);
    return (int)(sink_ & 0);
}
