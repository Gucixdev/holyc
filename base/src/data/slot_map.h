#ifndef HOLYC_SLOT_MAP_H_
#define HOLYC_SLOT_MAP_H_

/* Slot_Map - a fixed-capacity, caller-buffer-owned slot container with
 * STABLE indices, the real behavioral difference from array.h's Array:
 * Array's remove is swap-with-last (O(1), but the index of whatever
 * USED to be last just silently changed - fine for an unordered bag,
 * wrong for anything holding onto an index as a handle). Slot_Map never
 * moves an occupied slot - insert hands back the index it used, and
 * that index stays valid (same slot, same data) until THAT slot is
 * explicitly removed, no matter what else gets inserted/removed
 * elsewhere in the bucket meanwhile. The classic use case: object-pool
 * / entity-slot patterns where some OTHER system is holding onto the
 * index as a handle (e.g. gamedev/ecs) and needs it to keep meaning
 * the same thing across frames.
 *
 * The cost of that guarantee: a freed slot leaves a hole (tracked via
 * a `free_hint` cursor that scans forward for the next empty slot on
 * insert, amortized O(1) in the common case of few holes, O(capacity)
 * worst case if the bucket is nearly full of holes - a real, honest
 * tradeoff, not hidden). No arena, no growth - same "static, caller
 * owns the buffer, full is a checkable failure not silent truncation"
 * stance array_static_init takes. */

#include "core/base.h"

typedef struct {
    void *items;      /* capacity * elem_size bytes, caller-owned */
    u8 *occupied;      /* capacity bytes, caller-owned - 1 = slot in use, 0 = free */
    size_t elem_size;
    size_t capacity;
    size_t count;      /* how many slots are currently occupied */
    size_t free_hint;  /* next index to try first on insert - avoids rescanning from 0 every time */
} Slot_Map;

/* items_buffer must hold capacity*elem_size bytes; occupied_buffer must
 * hold capacity bytes - both caller-owned, never freed or reallocated
 * by this file. */
void slot_map_init(Slot_Map *b, void *items_buffer, u8 *occupied_buffer, size_t capacity, size_t elem_size);

/* copies elem_size bytes from `elem` into the first free slot found
 * (starting from free_hint). Writes the slot's index to *out_index
 * (if out_index is non-NULL) and returns 1 on success; returns 0 (out_index
 * left untouched) if every slot is occupied. */
i32 slot_map_insert(Slot_Map *b, const void *elem, size_t *out_index);

/* frees the slot at `index` (does NOT touch its bytes - a stale read
 * via slot_map_get after this returns NULL anyway, since occupied[index]
 * is now 0). Returns 1 if the slot was occupied and is now free, 0 if
 * index was out of range or already free (a safe no-op either way,
 * not a crash on double-remove). */
i32 slot_map_remove(Slot_Map *b, size_t index);

/* NULL if index is out of range OR that slot isn't currently occupied -
 * otherwise a pointer into the bucket's own storage for that slot
 * (stable until that specific slot is removed, per this file's whole
 * point). */
void *slot_map_get(const Slot_Map *b, size_t index);

i32 slot_map_is_occupied(const Slot_Map *b, size_t index);

/* frees every slot at once (occupied[] all zeroed, count = 0, free_hint
 * reset to 0) - O(capacity), same cost as re-initializing occupied by hand. */
void slot_map_clear(Slot_Map *b);

#endif /* HOLYC_SLOT_MAP_H_ */

#ifdef SLOT_MAP_IMPLEMENTATION
#ifndef SLOT_MAP_IMPLEMENTATION_
#define SLOT_MAP_IMPLEMENTATION_

void slot_map_init(Slot_Map *b, void *items_buffer, u8 *occupied_buffer, size_t capacity, size_t elem_size)
{
    size_t i;
    b->items = items_buffer;
    b->occupied = occupied_buffer;
    b->elem_size = elem_size;
    b->capacity = capacity;
    b->count = 0;
    b->free_hint = 0;
    for (i = 0; i < capacity; i++) b->occupied[i] = 0;
}

static void slot_map_copy_(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

i32 slot_map_insert(Slot_Map *b, const void *elem, size_t *out_index)
{
    size_t i;

    if (b->count >= b->capacity) return 0;

    for (i = b->free_hint; i < b->capacity; i++) {
        if (!b->occupied[i]) {
            slot_map_copy_((u8 *)b->items + i * b->elem_size, elem, b->elem_size);
            b->occupied[i] = 1;
            b->count++;
            b->free_hint = i + 1;
            if (out_index) *out_index = i;
            return 1;
        }
    }
    /* wrap around: free_hint had walked past the last hole earlier
     * in the bucket's life - a real, expected case (removes and
     * inserts don't have to happen in matching order), not a bug */
    for (i = 0; i < b->free_hint; i++) {
        if (!b->occupied[i]) {
            slot_map_copy_((u8 *)b->items + i * b->elem_size, elem, b->elem_size);
            b->occupied[i] = 1;
            b->count++;
            b->free_hint = i + 1;
            if (out_index) *out_index = i;
            return 1;
        }
    }
    return 0; /* unreachable if count < capacity, kept as a real safety net */
}

i32 slot_map_remove(Slot_Map *b, size_t index)
{
    if (index >= b->capacity) return 0;
    if (!b->occupied[index]) return 0;
    b->occupied[index] = 0;
    b->count--;
    if (index < b->free_hint) b->free_hint = index; /* reuse the newly-freed slot before scanning further out */
    return 1;
}

void *slot_map_get(const Slot_Map *b, size_t index)
{
    if (index >= b->capacity) return NULL;
    if (!b->occupied[index]) return NULL;
    return (u8 *)b->items + index * b->elem_size;
}

i32 slot_map_is_occupied(const Slot_Map *b, size_t index)
{
    if (index >= b->capacity) return 0;
    return b->occupied[index] != 0;
}

void slot_map_clear(Slot_Map *b)
{
    size_t i;
    for (i = 0; i < b->capacity; i++) b->occupied[i] = 0;
    b->count = 0;
    b->free_hint = 0;
}

#endif /* SLOT_MAP_IMPLEMENTATION_ */
#endif /* SLOT_MAP_IMPLEMENTATION */
