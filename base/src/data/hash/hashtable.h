#ifndef HOLYC_HASHTABLE_H_
#define HOLYC_HASHTABLE_H_

/* Generic hash table - checked three candidates before writing this:
 *
 *   ht.h: open addressing, triangular-number probing (index=(index+step)
 *   &mask; step+=1 - guaranteed to visit every slot in a power-of-2 table
 *   before repeating, unlike naive quadratic probing), FNV-1a hash,
 *   tombstone deletion, resize-to-double at 70% load factor.
 *
 *   c-libs' zdx_hashtable.h: double-hash (djb2 then fnv1) fallback into a
 *   quadratic-ish probe with an odd modulo cap - more idiosyncratic, less
 *   proven a design.
 *
 *   c-libs' zdx_fast_hashtable.h: fixed capacity at init (no resize),
 *   15-char key cap, too specialized to be a general-purpose table.
 *
 * ht.h's algorithm won on being the clean, standard, well-tested shape
 * (the same probing idea CPython's dict uses, minus the perturbation
 * term). Its generic key/value TYPE support is `_Generic`/`__typeof__`/
 * C++ `decltype` macro sugar, none of which is C89 - stripped entirely
 * and replaced with the shape this project already uses elsewhere
 * (perfecthash.h, vecindex.h): explicit byte-length keys, so the same
 * function works for C strings and fixed-size binary keys uniformly.
 * Values are opaque `void *` (caller-owned) rather than inline-stored -
 * simpler than threading a value_size through every call, and the common
 * case (pointer to something already living in an Arena) needs nothing
 * more. Keys ARE copied into arena-owned storage on insert, so the
 * caller's key buffer doesn't need to outlive the call - same convention
 * as vec_index_set_centroid(). */

#include "types/base.h"
#include "alloc/arena_alloc.h"

#define HT_SLOT_EMPTY     0
#define HT_SLOT_OCCUPIED  1
#define HT_SLOT_TOMBSTONE 2 /* distinct from EMPTY on purpose: probing must stop at EMPTY but
                              * continue past a TOMBSTONE, or a deleted key would break the
                              * probe chain for every key that happened to land past it */

typedef struct {
    const void *key; /* arena-owned copy; meaningless when state != HT_SLOT_OCCUPIED */
    size_t key_len;
    void *value;       /* caller-owned, opaque */
    u32 hash;           /* cached - avoids re-hashing the key on every probe and on resize */
    i32 state;
} Ht_Slot;

typedef struct {
    Ht_Slot *slots;
    size_t capacity; /* always a power of 2 */
    size_t count;     /* live (occupied) entries */
    size_t filled;    /* occupied + tombstone - what the 70% load-factor check is measured against */
} Ht;

void  ht_init(Arena *a, Ht *ht, size_t capacity_hint); /* capacity_hint rounds up to the next power of 2 */
void  ht_put(Arena *a, Ht *ht, const void *key, size_t key_len, void *value); /* overwrites the value if key already exists */
void *ht_get(const Ht *ht, const void *key, size_t key_len); /* NULL if not found */
i32   ht_delete(Ht *ht, const void *key, size_t key_len); /* 1 if it was present and removed, 0 if not found */

#endif /* HOLYC_HASHTABLE_H_ */

#ifdef HASHTABLE_IMPLEMENTATION
#ifndef HASHTABLE_IMPLEMENTATION_
#define HASHTABLE_IMPLEMENTATION_

#ifndef HT_DEFAULT_CAPACITY
#define HT_DEFAULT_CAPACITY 16
#endif

static size_t ht_next_pow2_(size_t n)
{
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* returns the slot holding `key` if present, NULL otherwise. Stops at the
 * first EMPTY slot (the key, if it existed, could never have probed past
 * an EMPTY slot to land further along - that's the whole invariant open
 * addressing with tombstones relies on), but keeps going through
 * TOMBSTONEs and non-matching OCCUPIED slots. */
static Ht_Slot *ht_find_(Ht_Slot *slots, size_t capacity, u32 hash, const void *key, size_t key_len)
{
    size_t mask = capacity - 1;
    size_t index = hash & mask;
    size_t step = 1;

    for (;;) {
        Ht_Slot *slot = &slots[index];
        switch (slot->state) {
        case HT_SLOT_EMPTY:
            return NULL;
        case HT_SLOT_OCCUPIED:
            if (slot->hash == hash && slot->key_len == key_len && memcmp(slot->key, key, key_len) == 0) {
                return slot;
            }
            break;
        default: /* HT_SLOT_TOMBSTONE - keep probing */
            break;
        }
        index = (index + step) & mask;
        step++;
    }
}

/* for insertion: returns the existing slot if the key is already present
 * (caller overwrites the value there), otherwise the first TOMBSTONE seen
 * along the probe chain (preferred, keeps the table compact) or the
 * terminating EMPTY slot if no tombstone was seen. Must still walk the
 * FULL chain to the first EMPTY even after spotting a tombstone - stopping
 * early at the first tombstone would risk inserting a duplicate while the
 * real key sits further down the same chain. */
static Ht_Slot *ht_find_insert_slot_(Ht_Slot *slots, size_t capacity, u32 hash, const void *key, size_t key_len)
{
    size_t mask = capacity - 1;
    size_t index = hash & mask;
    size_t step = 1;
    Ht_Slot *first_tombstone = NULL;

    for (;;) {
        Ht_Slot *slot = &slots[index];
        switch (slot->state) {
        case HT_SLOT_EMPTY:
            return first_tombstone ? first_tombstone : slot;
        case HT_SLOT_TOMBSTONE:
            if (!first_tombstone) first_tombstone = slot;
            break;
        default: /* HT_SLOT_OCCUPIED */
            if (slot->hash == hash && slot->key_len == key_len && memcmp(slot->key, key, key_len) == 0) {
                return slot;
            }
            break;
        }
        index = (index + step) & mask;
        step++;
    }
}

static void ht_grow_(Arena *a, Ht *ht)
{
    size_t new_capacity = ht->capacity * 2;
    Ht_Slot *new_slots = (Ht_Slot *)arena_alloc(a, sizeof(Ht_Slot) * new_capacity);
    size_t i;

    for (i = 0; i < new_capacity; i++) new_slots[i].state = HT_SLOT_EMPTY;

    /* tombstones are not carried over - this rebuild is exactly what
     * reclaims the probe-chain cost they were accumulating */
    for (i = 0; i < ht->capacity; i++) {
        Ht_Slot *old = &ht->slots[i];
        if (old->state == HT_SLOT_OCCUPIED) {
            Ht_Slot *dst = ht_find_insert_slot_(new_slots, new_capacity, old->hash, old->key, old->key_len);
            dst->key = old->key;
            dst->key_len = old->key_len;
            dst->value = old->value;
            dst->hash = old->hash;
            dst->state = HT_SLOT_OCCUPIED;
        }
    }

    ht->slots = new_slots;
    ht->capacity = new_capacity;
    ht->filled = ht->count;
}

void ht_init(Arena *a, Ht *ht, size_t capacity_hint)
{
    size_t cap = ht_next_pow2_(capacity_hint > 0 ? capacity_hint : HT_DEFAULT_CAPACITY);
    size_t i;

    ht->slots = (Ht_Slot *)arena_alloc(a, sizeof(Ht_Slot) * cap);
    for (i = 0; i < cap; i++) ht->slots[i].state = HT_SLOT_EMPTY;
    ht->capacity = cap;
    ht->count = 0;
    ht->filled = 0;
}

void ht_put(Arena *a, Ht *ht, const void *key, size_t key_len, void *value)
{
    Ht_Slot *slot;
    u32 hash;

    /* grow before inserting, same as ht.h itself does - occasionally
     * grows one call earlier than strictly necessary (a put() that only
     * overwrites an existing key's value didn't need more room), but
     * checking that would mean walking the probe chain twice */
    if ((ht->filled + 1) * 100 >= 70 * ht->capacity) {
        ht_grow_(a, ht);
    }

    hash = fnv1a_hash(key, key_len, 0);
    slot = ht_find_insert_slot_(ht->slots, ht->capacity, hash, key, key_len);

    if (slot->state == HT_SLOT_OCCUPIED) {
        slot->value = value;
        return;
    }

    ht->filled += (slot->state == HT_SLOT_EMPTY); /* reusing a tombstone doesn't grow `filled` */

    {
        void *key_copy = arena_alloc(a, key_len);
        arena_memcpy(key_copy, key, key_len);
        slot->key = key_copy;
    }
    slot->key_len = key_len;
    slot->value = value;
    slot->hash = hash;
    slot->state = HT_SLOT_OCCUPIED;
    ht->count++;
}

void *ht_get(const Ht *ht, const void *key, size_t key_len)
{
    u32 hash = fnv1a_hash(key, key_len, 0);
    Ht_Slot *slot = ht_find_(ht->slots, ht->capacity, hash, key, key_len);
    return slot ? slot->value : NULL;
}

i32 ht_delete(Ht *ht, const void *key, size_t key_len)
{
    u32 hash = fnv1a_hash(key, key_len, 0);
    Ht_Slot *slot = ht_find_(ht->slots, ht->capacity, hash, key, key_len);

    if (!slot) return 0;

    slot->state = HT_SLOT_TOMBSTONE;
    slot->key = NULL;
    slot->value = NULL;
    ht->count--;
    return 1;
}

#endif /* HASHTABLE_IMPLEMENTATION_ */
#endif /* HASHTABLE_IMPLEMENTATION */
