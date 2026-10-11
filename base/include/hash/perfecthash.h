#ifndef HOLYC_PERFECTHASH_H_
#define HOLYC_PERFECTHASH_H_

/* "Perfect hash generation": build-time minimal perfect hash for a fixed,
 * known set of keys (0 collisions, output range is exactly [0, n_keys) -
 * think reserved-keyword lookup in a tokenizer, not a general hash table).
 *
 * Classic "hash, displace, and compress" algorithm (Czech, Havas &
 * Majewski 1992; the shape here follows Steve Hanov's well-known writeup
 * of it - "Throw away the keys: easy, minimal perfect hashing"):
 *
 *   1. h1(key) = fnv1a(key, seed=0) % n buckets each key into one of n
 *      buckets (n_buckets == n_keys, the simplest version of the scheme).
 *   2. Process buckets largest-first (greedy - big buckets are hardest to
 *      place, so give them first pick of the slot table).
 *   3. For each bucket, search displacement seeds d = 0, 1, 2, ... until
 *      h2(key, d) = fnv1a(key, seed=d) % n lands every key in that bucket
 *      on a free, mutually-distinct slot. Record d for that bucket.
 *   4. Lookup: slot = h2(key, displacement[h1(key)]).
 *
 * Not qsort()+comparator for the largest-first ordering: ISO C89's qsort
 * comparator takes no context argument, so sorting buckets by their size
 * would need that size array reachable from a file-scope global - a race
 * if two fgjob worker threads ever call perfect_hash_build concurrently
 * with different key sets. A counting sort over bucket sizes needs no
 * shared state at all (every array is a local scratch buffer) and is
 * O(n) instead of O(n log n) besides. */

#include "base.h"
#include "arena_alloc.h"

typedef struct {
    i32 *displacement; /* [n] */
    size_t n;            /* == n_keys the hash was built for */
} Perfect_Hash;

#ifndef PERFECT_HASH_MAX_SEED_ATTEMPTS
#define PERFECT_HASH_MAX_SEED_ATTEMPTS 100000
#endif

/* keys must all be distinct. Returns 1 on success. Returns 0 only if the
 * seed search for some bucket exhausted PERFECT_HASH_MAX_SEED_ATTEMPTS -
 * astronomically unlikely for any reasonable key set (this is the standard
 * algorithm's known-good behavior, not a fudge factor); ph is left
 * zeroed on failure. */
i32 perfect_hash_build(Arena *a, Perfect_Hash *ph, const char * const *keys, size_t n_keys);

/* returns a slot in [0, ph->n) for `key`. For a key that was NOT in the
 * original build set this still returns *some* in-range slot with no
 * membership guarantee - same contract every minimal perfect hash makes.
 * Compare against the original key stored at that slot if membership needs
 * checking. */
size_t perfect_hash_lookup(const Perfect_Hash *ph, const char *key);

#endif /* HOLYC_PERFECTHASH_H_ */

#ifdef PERFECTHASH_IMPLEMENTATION
#ifndef PERFECTHASH_IMPLEMENTATION_
#define PERFECTHASH_IMPLEMENTATION_

static u32 perfect_hash_fnv1a_(const char *s, u32 seed)
{
    u32 h = 2166136261u ^ seed;
    while (*s) {
        h ^= (u8)(*s++);
        h *= 16777619u;
    }
    return h;
}

i32 perfect_hash_build(Arena *a, Perfect_Hash *ph, const char * const *keys, size_t n_keys)
{
    Arena_Mark mark;
    i32 *displacement;
    size_t *h1, *bucket_count, *bucket_start, *bucket_items, *cursor;
    size_t *bucket_order, *size_count, *size_offset, *trial;
    u8 *claimed;
    size_t i, b, n, s, max_size;

    if (n_keys == 0) {
        ph->displacement = NULL;
        ph->n = 0;
        return 1;
    }
    n = n_keys;

    /* persists past the rewind below - allocated before the scratch mark */
    displacement = (i32 *)arena_alloc(a, n * sizeof(i32));
    for (i = 0; i < n; i++) displacement[i] = 0;

    mark = arena_snapshot(a);

    h1           = (size_t *)arena_alloc(a, n * sizeof(size_t));
    bucket_count = (size_t *)arena_alloc(a, n * sizeof(size_t));
    bucket_start = (size_t *)arena_alloc(a, (n + 1) * sizeof(size_t));
    bucket_items = (size_t *)arena_alloc(a, n * sizeof(size_t));
    cursor       = (size_t *)arena_alloc(a, n * sizeof(size_t));
    bucket_order = (size_t *)arena_alloc(a, n * sizeof(size_t));
    claimed      = (u8 *)arena_alloc(a, n * sizeof(u8));

    for (i = 0; i < n; i++) { bucket_count[i] = 0; claimed[i] = 0; }
    for (i = 0; i < n; i++) {
        h1[i] = (size_t)(perfect_hash_fnv1a_(keys[i], 0u) % (u32)n);
        bucket_count[h1[i]]++;
    }

    bucket_start[0] = 0;
    for (b = 0; b < n; b++) bucket_start[b + 1] = bucket_start[b] + bucket_count[b];
    for (b = 0; b < n; b++) cursor[b] = bucket_start[b];
    for (i = 0; i < n; i++) bucket_items[cursor[h1[i]]++] = i;

    max_size = 0;
    for (b = 0; b < n; b++) if (bucket_count[b] > max_size) max_size = bucket_count[b];

    /* counting sort of bucket_order by bucket_count, descending - see the
     * file header comment for why this replaces qsort here */
    size_count  = (size_t *)arena_alloc(a, (max_size + 1) * sizeof(size_t));
    size_offset = (size_t *)arena_alloc(a, (max_size + 1) * sizeof(size_t));
    for (s = 0; s <= max_size; s++) size_count[s] = 0;
    for (b = 0; b < n; b++) size_count[bucket_count[b]]++;

    size_offset[max_size] = 0;
    if (max_size > 0) {
        for (s = max_size; s-- > 0; ) {
            size_offset[s] = size_offset[s + 1] + size_count[s + 1];
        }
    }
    for (s = 0; s <= max_size; s++) cursor[s] = size_offset[s]; /* cursor has n >= max_size+1 slots, reused safely */
    for (b = 0; b < n; b++) {
        s = bucket_count[b];
        bucket_order[cursor[s]++] = b;
    }

    trial = (size_t *)arena_alloc(a, (max_size > 0 ? max_size : 1) * sizeof(size_t));

    for (i = 0; i < n; i++) {
        size_t bi = bucket_order[i];
        size_t start = bucket_start[bi];
        size_t count = bucket_count[bi];
        i32 d;

        if (count == 0) continue;

        for (d = 0; d < PERFECT_HASH_MAX_SEED_ATTEMPTS; d++) {
            i32 ok = 1;
            size_t k;
            for (k = 0; k < count && ok; k++) {
                size_t key_i = bucket_items[start + k];
                size_t slot = (size_t)(perfect_hash_fnv1a_(keys[key_i], (u32)d) % (u32)n);
                size_t m;
                if (claimed[slot]) { ok = 0; break; }
                for (m = 0; m < k; m++) {
                    if (trial[m] == slot) { ok = 0; break; }
                }
                if (ok) trial[k] = slot;
            }
            if (ok) {
                size_t k2;
                for (k2 = 0; k2 < count; k2++) claimed[trial[k2]] = 1;
                displacement[bi] = d;
                break;
            }
        }
        if (d == PERFECT_HASH_MAX_SEED_ATTEMPTS) {
            arena_rewind(a, mark);
            ph->displacement = NULL;
            ph->n = 0;
            return 0;
        }
    }

    arena_rewind(a, mark); /* reclaim every scratch array above - displacement was allocated before mark */
    ph->displacement = displacement;
    ph->n = n;
    return 1;
}

size_t perfect_hash_lookup(const Perfect_Hash *ph, const char *key)
{
    size_t slot1 = (size_t)(perfect_hash_fnv1a_(key, 0u) % (u32)ph->n);
    i32 d = ph->displacement[slot1];
    return (size_t)(perfect_hash_fnv1a_(key, (u32)d) % (u32)ph->n);
}

#endif /* PERFECTHASH_IMPLEMENTATION_ */
#endif /* PERFECTHASH_IMPLEMENTATION */
