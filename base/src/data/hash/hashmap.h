#ifndef HOLYC_HASHMAP_H_
#define HOLYC_HASHMAP_H_

/* Insertion-ordered map, built on hashtable.h's Ht the same way
 * hashset.h is (own struct wrapping a Ht, no reimplementing probing) -
 * what Ht alone can't give you is iteration in the order keys were
 * FIRST inserted (open addressing's slot order is whatever the hash
 * happened to land on). This keeps its own parallel array of
 * (key, key_len) pairs in insertion order, growing by doubling the
 * same way array.h's dynamic mode and react.h's Signal do - own copy
 * of that pattern, not a dependency on array.h.
 *
 * Same two-#define requirement as hashset.h: needs
 * HASHTABLE_IMPLEMENTATION defined too wherever HASH_MAP_IMPLEMENTATION
 * is, since this calls real ht_* functions that need their own bodies
 * compiled in. */

#include "data/hash/hashtable.h"

typedef struct {
    const void *key;
    size_t key_len;
} Hash_Map_Key;

typedef struct {
    Ht ht;
    Hash_Map_Key *keys; /* arena-owned, insertion order */
    size_t count;
    size_t capacity;
} Hash_Map;

void hash_map_init(Arena *a, Hash_Map *m, size_t capacity_hint);

/* overwrites the value if key already exists - insertion order is
 * unaffected by an overwrite (it stays at its ORIGINAL position, same
 * as Python dict / JS Map). */
void hash_map_put(Arena *a, Hash_Map *m, const void *key, size_t key_len, void *value);

void *hash_map_get(const Hash_Map *m, const void *key, size_t key_len); /* NULL if not found */
i32 hash_map_remove(Hash_Map *m, const void *key, size_t key_len);        /* 1 if removed, 0 if not found */
size_t hash_map_count(const Hash_Map *m);

/* iterate in insertion order: for (i = 0; i < m->count; i++) - use
 * m->keys[i].key/.key_len directly, or hash_map_key_at/hash_map_value_at. */
const Hash_Map_Key *hash_map_key_at(const Hash_Map *m, size_t i);
void *hash_map_value_at(const Hash_Map *m, size_t i);

#endif /* HOLYC_HASHMAP_H_ */

#ifdef HASH_MAP_IMPLEMENTATION
#ifndef HASH_MAP_IMPLEMENTATION_
#define HASH_MAP_IMPLEMENTATION_

static i32 hash_map_key_eq_(const void *a, size_t a_len, const void *b, size_t b_len)
{
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    size_t i;
    if (a_len != b_len) return 0;
    for (i = 0; i < a_len; i++) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}

static void hash_map_memcpy_(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

void hash_map_init(Arena *a, Hash_Map *m, size_t capacity_hint)
{
    ht_init(a, &m->ht, capacity_hint);
    m->keys = 0;
    m->count = 0;
    m->capacity = 0;
}

void hash_map_put(Arena *a, Hash_Map *m, const void *key, size_t key_len, void *value)
{
    if (ht_get(&m->ht, key, key_len) == NULL) {
        if (m->count >= m->capacity) {
            size_t new_capacity = m->capacity == 0 ? 8 : m->capacity * 2;
            Hash_Map_Key *new_keys = (Hash_Map_Key *)arena_alloc(a, new_capacity * sizeof(Hash_Map_Key));
            if (m->count > 0) hash_map_memcpy_(new_keys, m->keys, m->count * sizeof(Hash_Map_Key));
            m->keys = new_keys;
            m->capacity = new_capacity;
        }
        {
            void *key_copy = arena_alloc(a, key_len);
            hash_map_memcpy_(key_copy, key, key_len);
            m->keys[m->count].key = key_copy;
            m->keys[m->count].key_len = key_len;
            m->count++;
        }
    }
    ht_put(a, &m->ht, key, key_len, value);
}

void *hash_map_get(const Hash_Map *m, const void *key, size_t key_len)
{
    return ht_get(&m->ht, key, key_len);
}

i32 hash_map_remove(Hash_Map *m, const void *key, size_t key_len)
{
    size_t i;
    if (!ht_delete(&m->ht, key, key_len)) return 0;
    for (i = 0; i < m->count; i++) {
        if (hash_map_key_eq_(m->keys[i].key, m->keys[i].key_len, key, key_len)) {
            m->keys[i] = m->keys[m->count - 1];
            m->count--;
            return 1;
        }
    }
    return 1; /* deleted from ht but not found in keys[] should never happen; ht_delete already reported success */
}

size_t hash_map_count(const Hash_Map *m) { return m->count; }

const Hash_Map_Key *hash_map_key_at(const Hash_Map *m, size_t i) { return &m->keys[i]; }

void *hash_map_value_at(const Hash_Map *m, size_t i)
{
    return ht_get(&m->ht, m->keys[i].key, m->keys[i].key_len);
}

#endif /* HASH_MAP_IMPLEMENTATION_ */
#endif /* HASH_MAP_IMPLEMENTATION */
