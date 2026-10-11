#ifndef HOLYC_SCAFFOLD_H_
#define HOLYC_SCAFFOLD_H_

/* SCAFFOLD_ARRAY_DEFINE(T, Name) - one macro line instead of hand-
 * declaring a typed growable-array struct + push/get/clear wrappers
 * every time one's needed. Built directly on arena_alloc.h's own
 * arena_da_append (the `items`/`count`/`capacity` growable-array
 * pattern already used throughout this codebase) - this just generates
 * the struct declaration and the small set of typed accessor functions
 * around it, same token-pasted-prefix convention as OPTION_DEFINE/
 * RESULT_DEFINE (help/result.h) and FABRIC_DEFINE (help/fabric.h).
 *
 * Different from datastruct/array.h's Array: that one is runtime-typed
 * (void*, one binary works for any element size). This is compile-time
 * typed (one concrete struct+functions per T, real type checking, no
 * void* casts at call sites) - reach for this when T is fixed and
 * known at the declaration site, which is the common case.
 *
 *   SCAFFOLD_ARRAY_DEFINE(i32, I32_Array)
 *   ...
 *   I32_Array xs; I32_Array_init(&xs);
 *   I32_Array_push(&arena, &xs, 5);
 *   i32 *v = I32_Array_get(&xs, 0);
 */

#include "base.h"
#include "arena_alloc.h"

#define SCAFFOLD_ARRAY_DEFINE(T, Name)                                    \
typedef struct {                                                          \
    T *items;                                                             \
    size_t count;                                                         \
    size_t capacity;                                                      \
} Name;                                                                    \
static void Name##_init(Name *da_)                                        \
{                                                                          \
    da_->items = NULL;                                                    \
    da_->count = 0;                                                       \
    da_->capacity = 0;                                                    \
}                                                                          \
static void Name##_push(Arena *a_, Name *da_, T item_)                    \
{                                                                          \
    arena_da_append(a_, da_, item_);                                      \
}                                                                          \
static T *Name##_get(Name *da_, size_t index_)                            \
{                                                                          \
    return (index_ < da_->count) ? &da_->items[index_] : NULL;            \
}                                                                          \
static void Name##_clear(Name *da_)                                       \
{                                                                          \
    da_->count = 0;                                                       \
}

#endif /* HOLYC_SCAFFOLD_H_ */
