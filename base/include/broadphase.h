#ifndef HOLYC_BROADPHASE_H_
#define HOLYC_BROADPHASE_H_

/* Sweep-and-prune broad-phase collision: the standard first-pass filter
 * before expensive narrow-phase tests (gjk.h etc) - sort object AABBs
 * by their min-x, then sweep through in that order, only checking a
 * pair once the sweep has established their x-ranges could possibly
 * overlap (an early-exit on the first object whose min.x exceeds the
 * current one's max.x, since nothing further in x-sorted order can
 * overlap either) - genuinely sub-quadratic in practice for reasonably
 * spread-out scenes, unlike a naive check-every-pair loop, while still
 * being simple enough to implement correctly without a spatial tree. */

#include "base.h"
#include "mathx.h"
#include "arena_alloc.h"

typedef struct {
    i64 id;
    vec3 min, max;
} Broadphase_Object;

typedef struct {
    i64 id_a, id_b;
} Broadphase_Pair;

/* does NOT reorder or mutate `objects` - sorts a local index scratch
 * array instead. Writes up to out_capacity overlapping pairs into
 * out_pairs; returns how many were written (results are truncated,
 * not an error, if there are more real overlapping pairs than
 * out_capacity - same "caller-owned buffer, return how many fit"
 * convention `src/gamedev/grid.h`'s own query functions use). */
size_t broadphase_sweep_and_prune(Arena *a, const Broadphase_Object *objects, size_t n,
                                   Broadphase_Pair *out_pairs, size_t out_capacity);

#endif /* HOLYC_BROADPHASE_H_ */

#ifdef BROADPHASE_IMPLEMENTATION
#ifndef BROADPHASE_IMPLEMENTATION_
#define BROADPHASE_IMPLEMENTATION_

static i32 broadphase_aabb_overlap_(const Broadphase_Object *a, const Broadphase_Object *b)
{
    if (a->max.x < b->min.x || b->max.x < a->min.x) return 0;
    if (a->max.y < b->min.y || b->max.y < a->min.y) return 0;
    if (a->max.z < b->min.z || b->max.z < a->min.z) return 0;
    return 1;
}

size_t broadphase_sweep_and_prune(Arena *a, const Broadphase_Object *objects, size_t n,
                                   Broadphase_Pair *out_pairs, size_t out_capacity)
{
    size_t *order;
    size_t i, j, written;

    if (n == 0) return 0;

    /* sort a scratch INDEX array by min.x (plain insertion sort - real
       game object counts per broad-phase pass are small/medium enough
       that this is simple, correct, and fast enough; a caller with
       thousands of objects per pass would want a real spatial
       structure ahead of this stage anyway, not a smarter sort here) -
       this leaves the caller's own `objects` array completely
       untouched, unlike an in-place sort would. */
    order = (size_t *)arena_alloc(a, n * sizeof(size_t));
    for (i = 0; i < n; i++) order[i] = i;

    for (i = 1; i < n; i++) {
        size_t key = order[i];
        f32 key_min_x = objects[key].min.x;
        j = i;
        while (j > 0 && objects[order[j - 1]].min.x > key_min_x) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    written = 0;
    for (i = 0; i < n && written < out_capacity; i++) {
        const Broadphase_Object *oi = &objects[order[i]];
        for (j = i + 1; j < n; j++) {
            const Broadphase_Object *oj = &objects[order[j]];
            if (oj->min.x > oi->max.x) break; /* sorted by min.x - nothing further can overlap oi in x either */
            if (broadphase_aabb_overlap_(oi, oj)) {
                if (written >= out_capacity) break;
                out_pairs[written].id_a = oi->id;
                out_pairs[written].id_b = oj->id;
                written++;
            }
        }
    }

    return written;
}

#endif /* BROADPHASE_IMPLEMENTATION_ */
#endif /* BROADPHASE_IMPLEMENTATION */
