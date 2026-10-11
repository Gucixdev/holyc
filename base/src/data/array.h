#ifndef HOLYC_ARRAY_H_
#define HOLYC_ARRAY_H_

/* Type-erased (void*, runtime elem_size) array container - two modes on
 * one shape, not two separate types:
 *
 *   STATIC:  array_static_init gives it a caller-owned, fixed-size
 *            buffer (a stack array, a struct field, anything) and NO
 *            arena - array_push on a full static array fails (returns
 *            0) rather than growing, since there is nowhere to grow
 *            INTO without an allocator.
 *
 *   DYNAMIC: array_dynamic_init gives it no buffer at all - the first
 *            array_push allocates from the Arena passed to it, and
 *            every push after that grows (doubling capacity) the same
 *            way arena_alloc.h's own arena_da_append does internally.
 *
 * Different from arena_da_append (arena_alloc.h's own growable-array
 * macro): that one is compile-time typed (generates code specialized
 * to a named struct's own T `items` field via macro expansion, zero
 * runtime overhead, but needs a distinct struct declared per element
 * type). This file is the opposite tradeoff - ONE runtime type usable
 * for any element size without declaring anything new, at the cost of
 * void*-based type erasure (the caller tracks what T actually is) and
 * one extra multiply (elem_size) per access. Reach for arena_da_append
 * when the element type is fixed and known at the call site (the
 * common case, see event_queue.h/priority_queue.h's own items[]
 * fields); reach for this file when the element type itself needs to
 * vary at runtime, or when avoiding a one-off struct declaration
 * genuinely matters more than the type safety it would have given up. */

#include "types/base.h"
#include "alloc/arena_alloc.h"

typedef struct {
    void *items;
    size_t count;
    size_t capacity;
    size_t elem_size;
    i32 is_dynamic; /* 0 = static (caller buffer, never grows), 1 = dynamic (arena-owned, grows) */
} Array;

/* buffer must hold at least capacity*elem_size bytes, caller-owned
 * (stack, static, arena, doesn't matter - this file never frees or
 * reallocates it). */
void array_static_init(Array *a, void *buffer, size_t capacity, size_t elem_size);

/* no buffer yet - the first array_push provides one via `a`. */
void array_dynamic_init(Array *a, size_t elem_size);

/* copies elem_size bytes from `elem` into the array. Static arrays
 * return 0 (and leave the array unchanged) once full; dynamic arrays
 * only return 0 if `arena` is NULL (a real caller mistake, not a
 * capacity limit) - they grow instead of failing. `arena` is ignored
 * for a static array (pass NULL). */
i32 array_push(Arena *arena, Array *a, const void *elem);

/* NULL if index is out of range, otherwise a pointer INTO the array's
 * own storage (valid until the next push that grows a dynamic array -
 * same "pointer may be invalidated by growth" caveat arena_da_append's
 * own items pointer already has). */
void *array_get(const Array *a, size_t index);

/* swap-with-last removal (O(1), does not preserve order) - copies the
 * last element's bytes over index's slot, then shrinks count by one.
 * Returns 0 if index is out of range. */
i32 array_remove_swap(Array *a, size_t index);

void array_clear(Array *a); /* count = 0; keeps the buffer/capacity as-is */

#endif /* HOLYC_ARRAY_H_ */

#ifdef ARRAY_IMPLEMENTATION
#ifndef ARRAY_IMPLEMENTATION_
#define ARRAY_IMPLEMENTATION_

void array_static_init(Array *a, void *buffer, size_t capacity, size_t elem_size)
{
    a->items = buffer;
    a->count = 0;
    a->capacity = capacity;
    a->elem_size = elem_size;
    a->is_dynamic = 0;
}

void array_dynamic_init(Array *a, size_t elem_size)
{
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
    a->elem_size = elem_size;
    a->is_dynamic = 1;
}

static void array_copy_(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

i32 array_push(Arena *arena, Array *a, const void *elem)
{
    if (!a->is_dynamic) {
        if (a->count >= a->capacity) return 0;
        array_copy_((u8 *)a->items + a->count * a->elem_size, elem, a->elem_size);
        a->count++;
        return 1;
    }

    if (!arena) return 0;

    if (a->count >= a->capacity) {
        size_t new_capacity = a->capacity == 0 ? 8 : a->capacity * 2;
        void *new_items = arena_alloc(arena, new_capacity * a->elem_size);
        if (a->count > 0) array_copy_(new_items, a->items, a->count * a->elem_size);
        a->items = new_items;
        a->capacity = new_capacity;
    }

    array_copy_((u8 *)a->items + a->count * a->elem_size, elem, a->elem_size);
    a->count++;
    return 1;
}

void *array_get(const Array *a, size_t index)
{
    if (index >= a->count) return NULL;
    return (u8 *)a->items + index * a->elem_size;
}

i32 array_remove_swap(Array *a, size_t index)
{
    if (index >= a->count) return 0;

    a->count--;
    if (index != a->count) {
        array_copy_((u8 *)a->items + index * a->elem_size,
                     (u8 *)a->items + a->count * a->elem_size,
                     a->elem_size);
    }
    return 1;
}

void array_clear(Array *a)
{
    a->count = 0;
}

#endif /* ARRAY_IMPLEMENTATION_ */
#endif /* ARRAY_IMPLEMENTATION */
