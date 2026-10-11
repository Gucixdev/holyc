#ifndef HOLYC_MAPOPS_H_
#define HOLYC_MAPOPS_H_

/* filter/fold/foreach over datastruct/array.h's type-erased Array -
 * the functional trio this file's own one-line spec named. Function-
 * pointer + void* userdata based (C89 has no closures), same shape
 * this project already uses for callbacks elsewhere (Signal_Fn in
 * react.h, Action_Fn in action_queue.h). */

#include "core/base.h"
#include "core/arena_alloc.h"
#include "data/array.h"

typedef i32 (*Mapops_Predicate)(const void *elem, void *userdata);
typedef void (*Mapops_Fn)(void *elem, void *userdata);
typedef void (*Mapops_Fold_Fn)(void *acc, const void *elem, void *userdata);

/* copies every element of src for which pred returns nonzero into dst,
 * in original order. dst must already be array_dynamic_init'd (or a
 * big-enough array_static_init'd buffer) with the SAME elem_size as
 * src. Returns the number of elements copied. */
size_t mapops_filter(Arena *a, Array *dst, const Array *src, Mapops_Predicate pred, void *userdata);

/* calls fn once per element, in order, with a pointer INTO the array's
 * own storage - fn may mutate the element in place. */
void mapops_foreach(Array *arr, Mapops_Fn fn, void *userdata);

/* folds left-to-right: fn(acc, elem, userdata) for every element in
 * order, mutating *acc in place (acc is caller-owned, already holding
 * whatever the fold's starting value should be). */
void mapops_fold(const Array *arr, void *acc, Mapops_Fold_Fn fn, void *userdata);

#endif /* HOLYC_MAPOPS_H_ */

#ifdef MAPOPS_IMPLEMENTATION
#ifndef MAPOPS_IMPLEMENTATION_
#define MAPOPS_IMPLEMENTATION_

size_t mapops_filter(Arena *a, Array *dst, const Array *src, Mapops_Predicate pred, void *userdata)
{
    size_t i;
    size_t n = 0;
    for (i = 0; i < src->count; i++) {
        void *elem = array_get(src, i);
        if (pred(elem, userdata)) {
            if (array_push(a, dst, elem)) n++;
        }
    }
    return n;
}

void mapops_foreach(Array *arr, Mapops_Fn fn, void *userdata)
{
    size_t i;
    for (i = 0; i < arr->count; i++) {
        fn(array_get(arr, i), userdata);
    }
}

void mapops_fold(const Array *arr, void *acc, Mapops_Fold_Fn fn, void *userdata)
{
    size_t i;
    for (i = 0; i < arr->count; i++) {
        fn(acc, array_get(arr, i), userdata);
    }
}

#endif /* MAPOPS_IMPLEMENTATION_ */
#endif /* MAPOPS_IMPLEMENTATION */
