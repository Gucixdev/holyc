/* alloc_harness SEED OPS: runs OPS random operations on a real Arena (default backend) and a real Pool and prints
 * one line per operation with its result, offsets in bytes from the region's data[0] or the pool's buffer. Every
 * block handed out is filled with a nonzero byte, as a user would; arena_calloc's block must read all zero and
 * arena_realloc's must keep the old bytes ("ok", else "BAD", which the model never prints).
 * lean4/run feeds the output to allocoracle, which recomputes every result from the Lean model; the two outputs
 * must be identical. The pool only gets pool_alloc while a chunk is free and pool_dealloc of a live chunk: the
 * preconditions the model states (Pool.alloc_inv, Pool.dealloc_inv). */
#define ARENA_ALLOC_IMPLEMENTATION
#define POOL_ALLOC_IMPLEMENTATION
#include "core_arena_alloc.h"
#include "core_pool_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static u64 rng_;
static u64 rnd_(u64 n) /* xorshift64, uniform enough below n for picking operations */
{
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 7;
    rng_ ^= rng_ << 17;
    return rng_ % n;
}

#define MAX_MARKS 64
#define MAX_CHUNKS 512
#define MAX_BLOCKS 4096

typedef struct { u8 *p; size_t n; u8 fill; } Block;
static Block blocks_[MAX_BLOCKS];
static size_t nblocks_;

static void push_(u8 *p, size_t n, u8 fill)
{
    memset(p, fill, n);
    if (nblocks_ < MAX_BLOCKS) { blocks_[nblocks_].p = p; blocks_[nblocks_].n = n; blocks_[nblocks_].fill = fill; nblocks_++; }
}

static int all_(const u8 *p, size_t n, u8 v)
{
    size_t i;
    for (i = 0; i < n; i++) if (p[i] != v) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    Arena a = {0}, pa = {0};
    Arena_Mark marks[MAX_MARKS];
    size_t nmarks = 0, nlive = 0, bs = 0, k = 0;
    size_t live[MAX_CHUNKS];
    u8 *base, *buf = NULL;
    Pool p;
    long ops, op;

    rng_ = (argc > 1 ? strtoull(argv[1], NULL, 10) : 1) * 0x9E3779B97F4A7C15ULL | 1;
    ops = argc > 2 ? atol(argv[2]) : 2000;
    base = (u8 *)arena_alloc(&a, 0); /* makes the region; data[0] */
    printf("arena %lu %lu %lu\n", (unsigned long)((size_t)base % (size_t)sysconf(_SC_PAGESIZE)),
           (unsigned long)a.begin->capacity, (unsigned long)sysconf(_SC_PAGESIZE));
    for (op = 0; op < ops; op++) {
        u64 r = rnd_(100);
        u8 fill = (u8)(op | 1);
        if (r < 35) {
            size_t n = rnd_(4) ? (size_t)rnd_(65) : (size_t)rnd_(4097);
            u8 *q = (u8 *)arena_alloc(&a, n);
            printf("a %lu %lu\n", (unsigned long)n, (unsigned long)(q - base));
            push_(q, n, fill);
        } else if (r < 42) {
            size_t n = (size_t)rnd_(300), kk = (size_t)rnd_(13);
            u8 *q = (u8 *)arena_alloc_aligned(&a, n, (size_t)1 << kk);
            printf("l %lu %lu %lu\n", (unsigned long)n, (unsigned long)kk, (unsigned long)(q - base));
            memset(q, fill, n);
        } else if (r < 47) {
            size_t n = (size_t)rnd_(8193);
            u8 *q = (u8 *)arena_calloc(&a, n);
            printf("c %lu %lu %s\n", (unsigned long)n, (unsigned long)(q - base), all_(q, n, 0) ? "ok" : "BAD");
            push_(q, n, fill);
        } else if (r < 52 && nblocks_ > 0) {
            size_t j = rnd_(2) ? nblocks_ - 1 : (size_t)rnd_(nblocks_);
            Block *b = &blocks_[j];
            size_t n = b->n + 1 + (size_t)rnd_(2000);
            u8 *q = (u8 *)arena_realloc(&a, b->p, b->n, n);
            printf("g %lu %lu %lu %s\n", (unsigned long)j, (unsigned long)n, (unsigned long)(q - base),
                   all_(q, b->n, b->fill) ? "ok" : "BAD");
            memset(q, fill, n);
            b->p = q; b->n = n; b->fill = fill;
        } else if (r < 53) {
            arena_decommit(&a);
            printf("d %lu\n", (unsigned long)a.begin->high);
        } else if (r < 58 && nmarks < MAX_MARKS) {
            marks[nmarks++] = arena_snapshot(&a);
            printf("s %lu\n", (unsigned long)a.end->count);
        } else if (r < 63 && nmarks > 0) {
            size_t j = (size_t)rnd_(nmarks);
            arena_rewind(&a, marks[j]);
            nblocks_ = 0;
            printf("r %lu %lu\n", (unsigned long)j, (unsigned long)arena_used(&a));
        } else if (r < 64) {
            arena_reset(&a);
            nblocks_ = 0;
            printf("z %lu\n", (unsigned long)arena_used(&a));
        } else if (r < 66 || !buf) {
            size_t cs = 1 + (size_t)rnd_(128);
            bs = 128 + (size_t)rnd_(3969);
            buf = (u8 *)arena_alloc(&pa, bs);
            pool_init(&p, buf, bs, cs);
            k = bs / p.chunk_size;
            nlive = 0;
            printf("p %lu %lu %lu\n", (unsigned long)bs, (unsigned long)cs, (unsigned long)p.chunk_size);
        } else if (r < 82 && nlive < k) {
            u8 *q = (u8 *)pool_alloc(&p);
            live[nlive++] = (size_t)(q - buf) / p.chunk_size;
            printf("pa %lu\n", (unsigned long)((size_t)(q - buf) / p.chunk_size));
        } else if (r < 99 && nlive > 0) {
            size_t j = (size_t)rnd_(nlive), c = live[j];
            live[j] = live[--nlive];
            pool_dealloc(&p, buf + c * p.chunk_size);
            printf("pd %lu\n", (unsigned long)c);
        } else if (r == 99) {
            pool_free_all(&p);
            nlive = 0;
            printf("pf\n");
        }
    }
    arena_free(&pa);
    arena_free(&a);
    return 0;
}
