#ifndef HOLYC_MERGE_OPS_H_
#define HOLYC_MERGE_OPS_H_

/* Two distinct, real meanings of "merge" this project actually needs -
 * kept in one file since both are small and both answer "combine two
 * of these into one", not because they share any implementation:
 *
 *   - merge_ops_sorted_merge: the classic merge-sort merge step - two
 *     ALREADY-SORTED array.h Arrays combined into one sorted Array,
 *     O(count_a + count_b), not a sort-the-concatenation shortcut.
 *
 *   - merge_ops_hashmap_merge: config/settings-style merging - every
 *     entry from `src` copied into `dst`, with `overwrite_existing`
 *     controlling which side wins on a key both maps already have
 *     (the same "last-loaded config wins" vs "first-loaded config
 *     wins" choice a real settings-layering system needs both
 *     directions of). */

#include "array.h"
#include "hash/hashmap.h"

/* `out` must be a dynamic Array (array_dynamic_init) of the SAME
 * elem_size as `a` and `b`, and both `a`/`b` must already be sorted
 * ascending per `cmp` (cmp(x,y) < 0 if x<y, 0 if equal, >0 if x>y -
 * the usual qsort-style contract) - this function does not check or
 * enforce that itself, an unsorted input just produces an unsorted
 * (and therefore meaningless) result, not a crash. On an equal
 * comparison, the element from `a` is placed first (a stable merge -
 * matters when duplicate keys carry different payload data elsewhere
 * in the same struct). */
void merge_ops_sorted_merge(Arena *arena, const Array *a, const Array *b,
                             i32 (*cmp)(const void *x, const void *y), Array *out);

/* copies every (key, value) pair from `src` into `dst`. When a key
 * exists in both: overwrite_existing=1 means src's value replaces
 * dst's (src "wins"), 0 means dst's existing value is left alone (dst
 * "wins", src's value for that key is simply not copied over). Either
 * way, dst's own insertion order for a key it already had is
 * preserved (hash_map_put's own contract - overwriting a value never
 * moves a key's position), and any BRAND NEW key from src is appended
 * in src's own iteration order after dst's existing keys. */
void merge_ops_hashmap_merge(Arena *a, Hash_Map *dst, const Hash_Map *src, i32 overwrite_existing);

#endif /* HOLYC_MERGE_OPS_H_ */

#ifdef MERGE_OPS_IMPLEMENTATION
#ifndef MERGE_OPS_IMPLEMENTATION_
#define MERGE_OPS_IMPLEMENTATION_

void merge_ops_sorted_merge(Arena *arena, const Array *a, const Array *b,
                             i32 (*cmp)(const void *x, const void *y), Array *out)
{
    size_t i = 0, j = 0;

    while (i < a->count && j < b->count) {
        const u8 *ea = (const u8 *)a->items + i * a->elem_size;
        const u8 *eb = (const u8 *)b->items + j * b->elem_size;
        if (cmp(ea, eb) <= 0) { array_push(arena, out, ea); i++; }
        else { array_push(arena, out, eb); j++; }
    }
    while (i < a->count) { array_push(arena, out, (const u8 *)a->items + i * a->elem_size); i++; }
    while (j < b->count) { array_push(arena, out, (const u8 *)b->items + j * b->elem_size); j++; }
}

void merge_ops_hashmap_merge(Arena *a, Hash_Map *dst, const Hash_Map *src, i32 overwrite_existing)
{
    size_t i;
    for (i = 0; i < src->count; i++) {
        const Hash_Map_Key *k = hash_map_key_at(src, i);
        void *src_value = hash_map_value_at(src, i);
        void *existing = hash_map_get(dst, k->key, k->key_len);

        if (existing && !overwrite_existing) continue;
        hash_map_put(a, dst, k->key, k->key_len, src_value);
    }
}

#endif /* MERGE_OPS_IMPLEMENTATION_ */
#endif /* MERGE_OPS_IMPLEMENTATION */
