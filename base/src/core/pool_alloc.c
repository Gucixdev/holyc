#ifndef HOLYC_POOL_ALLOC_C_
#define HOLYC_POOL_ALLOC_C_

/* Fixed-chunk free-list pool. No malloc here - the caller hands over an
 * existing buffer (carve it out of an Arena, a static array, whatever) and
 * this just slices it into equal chunks it can hand out and take back in
 * O(1), no fragmentation because every chunk is the same size. */

#include "core/base.h"
#include "core/contracts.h"

#ifndef POOL_ASSERT
#define POOL_ASSERT HOLYC_ASSERT
#endif

typedef struct Pool_Free_Node Pool_Free_Node;
struct Pool_Free_Node {
    Pool_Free_Node *next;
};

typedef struct {
    u8 *buffer;
    size_t buffer_size;
    size_t chunk_size;
    Pool_Free_Node *head;
} Pool;

size_t pool_align_up(size_t n, size_t align);
void pool_init(Pool *p, void *buffer, size_t buffer_size, size_t chunk_size);
void *pool_alloc(Pool *p);
void pool_dealloc(Pool *p, void *ptr);
void pool_free_all(Pool *p);

#endif /* HOLYC_POOL_ALLOC_C_ */

#ifdef POOL_ALLOC_IMPLEMENTATION
#ifndef POOL_ALLOC_IMPLEMENTATION_
#define POOL_ALLOC_IMPLEMENTATION_

size_t pool_align_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1); /* align must be a power of two */
}

void pool_free_all(Pool *p)
{
    size_t chunk_count = p->buffer_size / p->chunk_size;
    size_t i;
    Pool_Free_Node *node;

    p->head = NULL;
    for (i = 0; i < chunk_count; i++) {
        node = (Pool_Free_Node *)(p->buffer + i * p->chunk_size);
        node->next = p->head;
        p->head = node;
    }
}

void pool_init(Pool *p, void *buffer, size_t buffer_size, size_t chunk_size)
{
    size_t aligned_chunk_size = pool_align_up(chunk_size, sizeof(void *));

    POOL_ASSERT(aligned_chunk_size >= sizeof(Pool_Free_Node));
    POOL_ASSERT(buffer_size >= aligned_chunk_size);

    p->buffer = (u8 *)buffer;
    p->buffer_size = buffer_size;
    p->chunk_size = aligned_chunk_size;
    pool_free_all(p);
}

void *pool_alloc(Pool *p)
{
    Pool_Free_Node *node = p->head;
    void *result;

    POOL_ASSERT(node && "pool out of chunks");
    if (!node) return NULL;

    p->head = node->next;
    result = node; /* not zeroed - no hidden cost, caller writes what it needs */

    /* the pool's own "never overflows its region" property - the exact
     * shape of arena_alloc's own flagship ENSURES, applied here to a
     * fixed-chunk free-list instead of a bump-pointer arena: every
     * pointer handed out must lie fully within the buffer this pool
     * was pool_init'd with, chunk_size and all. */
    HOLYC_ENSURES((u8 *)result >= p->buffer && (u8 *)result + p->chunk_size <= p->buffer + p->buffer_size);

    return result;
}

void pool_dealloc(Pool *p, void *ptr)
{
    Pool_Free_Node *node;

    if (!ptr) return;

    node = (Pool_Free_Node *)ptr;
    node->next = p->head;
    p->head = node;
}

#endif /* POOL_ALLOC_IMPLEMENTATION_ */
#endif /* POOL_ALLOC_IMPLEMENTATION */
