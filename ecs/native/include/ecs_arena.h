/* ecs_arena.h - the two allocators an ECS wants besides malloc: a bump ARENA (many short-lived things, one lifetime) and a fixed-size
 * SLAB (many same-sized things with individual lifetimes). Both sit on ecs_mem.h, so a host allocator still owns the real memory.
 *
 * ARENA
 *   alloc is a pointer add (16-byte aligned; ecs_arena_alloc_aligned for SIMD/cache-line needs), free is "all at once".
 *   mark/rewind gives a stack discipline for temporaries; reset keeps the first chunk so a steady-state frame allocates nothing
 *   from the OS. Memory never moves, so pointers handed out stay valid until rewind/reset - which is what makes it safe for
 *   event payloads and per-frame scratch (the event bus's payloads live here).
 * SLAB
 *   O(1) alloc/free of blocks of one size from a free list, chunked growth, blocks never move (pointers stable). Detects the common
 *   double-free. Use it for objects created and destroyed at runtime in any order (active effects, subscriptions, ability instances).
 *
 * Neither is thread-safe; give each thread its own (a per-worker scratch arena is the usual pattern). */
#ifndef ECS_ARENA_H
#define ECS_ARENA_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ecs_arena ecs_arena;
typedef struct ecs_arena_mark { void *chunk; size_t used; } ecs_arena_mark;

ecs_arena *ecs_arena_new(size_t chunk_size);                 /* chunk_size is a floor of 4 KiB; a larger request gets its own chunk */
void ecs_arena_free(ecs_arena *a);
void *ecs_arena_alloc(ecs_arena *a, size_t n);               /* 16-byte aligned, NOT zeroed; NULL on OOM */
void *ecs_arena_calloc(ecs_arena *a, size_t n);
void *ecs_arena_alloc_aligned(ecs_arena *a, size_t n, size_t align);   /* align: power of two */
char *ecs_arena_strdup(ecs_arena *a, const char *s);
ecs_arena_mark ecs_arena_mark_get(const ecs_arena *a);
void ecs_arena_rewind(ecs_arena *a, ecs_arena_mark m);       /* frees everything allocated since the mark */
void ecs_arena_reset(ecs_arena *a);                          /* frees everything; keeps one chunk for reuse */
size_t ecs_arena_used(const ecs_arena *a);                   /* bytes handed out (excluding alignment slack of the current chunk) */
size_t ecs_arena_reserved(const ecs_arena *a);               /* bytes held from the allocator */

typedef struct ecs_slab ecs_slab;

/* block_size is rounded up to the alignment (>= 16). blocks_per_chunk: growth granule. */
ecs_slab *ecs_slab_new(size_t block_size, size_t align, size_t blocks_per_chunk);
void ecs_slab_destroy(ecs_slab *p);                         /* destroys the slab and its memory */
void *ecs_slab_alloc(ecs_slab *p);                           /* zeroed; NULL on OOM */
bool ecs_slab_release(ecs_slab *p, void *block);             /* false (and no effect) on NULL / a detected double free */
void ecs_slab_reset(ecs_slab *p);                            /* every block becomes free again; memory kept */
size_t ecs_slab_used(const ecs_slab *p);                     /* live blocks */
size_t ecs_slab_capacity(const ecs_slab *p);                 /* blocks the slab can hold without growing */

#ifdef __cplusplus
}
#endif
#endif
