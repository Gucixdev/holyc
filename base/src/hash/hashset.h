#ifndef HOLYC_HASHSET_H_
#define HOLYC_HASHSET_H_

/* Set of byte-keys, built directly ON hashtable.h's Ht rather than
 * reimplementing open addressing/probing/tombstones a second time -
 * presence is tracked via a fixed sentinel pointer as the "value"
 * every key maps to (never dereferenced, its address is the only thing
 * that matters). Same relationship slerp.h has to linear_algebra.h/
 * mathx.h: this file needs HASHTABLE_IMPLEMENTATION defined too (not
 * auto-forwarded) wherever HASH_SET_IMPLEMENTATION is - two separate
 * `#define`s at the call site, matching this project's own "no cross-
 * IMPLEMENTATION-guard dependency" rule (this file's OWN functions are
 * still guarded independently; it just calls into ht_* functions that
 * need their own real bodies compiled in somewhere in the same TU). */

#include "hashtable.h"

typedef struct {
    Ht ht;
} Hash_Set;

void hash_set_init(Arena *a, Hash_Set *set, size_t capacity_hint);
void hash_set_add(Arena *a, Hash_Set *set, const void *key, size_t key_len);
i32  hash_set_contains(const Hash_Set *set, const void *key, size_t key_len);
i32  hash_set_remove(Hash_Set *set, const void *key, size_t key_len); /* 1 if it was present and removed, 0 if not found */

#endif /* HOLYC_HASHSET_H_ */

#ifdef HASH_SET_IMPLEMENTATION
#ifndef HASH_SET_IMPLEMENTATION_
#define HASH_SET_IMPLEMENTATION_

/* the sentinel "this key is a member" marker - its ADDRESS is the
 * value every present key maps to in the underlying Ht, its own
 * contents are never read. A fixed non-NULL address also keeps
 * hash_set_contains unambiguous against Ht's own "NULL means not
 * found" convention. */
static i32 hash_set_present_marker_ = 1;

void hash_set_init(Arena *a, Hash_Set *set, size_t capacity_hint)
{
    ht_init(a, &set->ht, capacity_hint);
}

void hash_set_add(Arena *a, Hash_Set *set, const void *key, size_t key_len)
{
    ht_put(a, &set->ht, key, key_len, &hash_set_present_marker_);
}

i32 hash_set_contains(const Hash_Set *set, const void *key, size_t key_len)
{
    return ht_get(&set->ht, key, key_len) != NULL;
}

i32 hash_set_remove(Hash_Set *set, const void *key, size_t key_len)
{
    return ht_delete(&set->ht, key, key_len);
}

#endif /* HASH_SET_IMPLEMENTATION_ */
#endif /* HASH_SET_IMPLEMENTATION */
