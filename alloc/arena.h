/* arena.h - the two allocators worth having besides malloc: a bump ARENA (many short-lived things, one lifetime) and a fixed-size
 * SLAB (many same-sized things with individual lifetimes). Both sit on mem.h, so a host allocator still owns the real memory.
 *
 * ARENA
 *   alloc is a pointer add (16-byte aligned; hc_arena_alloc_aligned for SIMD/cache-line needs), free is "all at once".
 *   mark/rewind gives a stack discipline for temporaries; reset keeps the first chunk so a steady-state frame allocates nothing
 *   from the OS. Memory never moves, so pointers handed out stay valid until rewind/reset - which is what makes it safe for
 *   event payloads and per-frame scratch.
 * SLAB
 *   O(1) alloc/free of blocks of one size from a free list, chunked growth, blocks never move (pointers stable). Detects the common
 *   double-free. Use it for objects created and destroyed at runtime in any order.
 *
 * Neither is thread-safe; give each thread its own (a per-worker scratch arena is the usual pattern). */
#ifndef HC_ARENA_H
#define HC_ARENA_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hc_arena hc_arena;
typedef struct hc_arena_mark { void *chunk; size_t used; } hc_arena_mark;

hc_arena *hc_arena_new(size_t chunk_size);                 /* chunk_size is a floor of 4 KiB; a larger request gets its own chunk */
void hc_arena_free(hc_arena *a);
void *hc_arena_alloc(hc_arena *a, size_t n);               /* 16-byte aligned, NOT zeroed; NULL on OOM */
void *hc_arena_calloc(hc_arena *a, size_t n);
void *hc_arena_alloc_aligned(hc_arena *a, size_t n, size_t align);   /* align: power of two */
char *hc_arena_strdup(hc_arena *a, const char *s);
hc_arena_mark hc_arena_mark_get(const hc_arena *a);
void hc_arena_rewind(hc_arena *a, hc_arena_mark m);       /* frees everything allocated since the mark */
void hc_arena_reset(hc_arena *a);                          /* frees everything; keeps one chunk for reuse */
size_t hc_arena_used(const hc_arena *a);                   /* bytes handed out (excluding alignment slack of the current chunk) */
size_t hc_arena_reserved(const hc_arena *a);               /* bytes held from the allocator */

typedef struct hc_slab hc_slab;

/* block_size is rounded up to the alignment (>= 16). blocks_per_chunk: growth granule. */
hc_slab *hc_slab_new(size_t block_size, size_t align, size_t blocks_per_chunk);
void hc_slab_destroy(hc_slab *p);                         /* destroys the slab and its memory */
void *hc_slab_alloc(hc_slab *p);                           /* zeroed; NULL on OOM */
bool hc_slab_release(hc_slab *p, void *block);             /* false (and no effect) on NULL / a detected double free */
void hc_slab_reset(hc_slab *p);                            /* every block becomes free again; memory kept */
size_t hc_slab_used(const hc_slab *p);                     /* live blocks */
size_t hc_slab_capacity(const hc_slab *p);                 /* blocks the slab can hold without growing */

#ifdef __cplusplus
}
#endif
#endif
