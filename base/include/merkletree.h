#ifndef HOLYC_MERKLETREE_H_
#define HOLYC_MERKLETREE_H_

/* Classic Merkle (hash) tree: every leaf is sha256(block), every
 * internal node is sha256(left_child || right_child) - the standard
 * technique for verifying one block belongs to a known dataset
 * (given just its root hash) without needing the WHOLE dataset on
 * hand, via a proof (the sibling hash at each level up to the root).
 * An odd node at any level is promoted unchanged to the next level
 * (duplicated-last-node is a common alternative convention; this file
 * uses plain promotion instead, documented here since it changes the
 * exact root hash a given block set produces). */

#include "base.h"
#include "arena_alloc.h"
#define SHA256_HASH_IMPLEMENTATION
#include "sha256_hash.h"

typedef struct {
    u8 hash[SHA256_DIGEST_SIZE];
} Merkle_Hash;

/* computes the root hash over `blocks[0..count)` (each `sizes[i]`
 * bytes). count must be >= 1. */
Merkle_Hash merkletree_root(const void **blocks, const size_t *sizes, size_t count, Merkle_Hash *out_root);

/* builds the inclusion proof for leaf `index` out of `count` total
 * leaves - the sibling hash at every level from the leaf up to (but
 * not including) the root. Writes at most `max_proof_len` sibling
 * hashes into out_proof (in leaf-to-root order) and returns how many
 * were actually written; a tree with N leaves never needs more than
 * ceil(log2(N)) siblings, so a caller can size out_proof generously
 * (e.g. 32 covers over 4 billion leaves) without risking truncation. */
size_t merkletree_proof(const void **blocks, const size_t *sizes, size_t count, size_t index, Merkle_Hash *out_proof, size_t max_proof_len);

/* recomputes the root from `leaf_data`/`leaf_size`, `index`, and the
 * proof siblings, and compares it against `expected_root` - the whole
 * point of a Merkle proof: verifying membership using only the leaf
 * itself, its index, the proof, and a root hash obtained separately
 * (never needing the rest of the dataset). Returns 1 if it matches. */
i32 merkletree_verify(const void *leaf_data, size_t leaf_size, size_t index, size_t leaf_count, const Merkle_Hash *proof, size_t proof_len, Merkle_Hash expected_root);

#endif /* HOLYC_MERKLETREE_H_ */

#ifdef MERKLETREE_IMPLEMENTATION
#ifndef MERKLETREE_IMPLEMENTATION_
#define MERKLETREE_IMPLEMENTATION_

static Merkle_Hash merkletree_hash_leaf_(const void *data, size_t size)
{
    Merkle_Hash h;
    sha256_hash(data, size, h.hash);
    return h;
}

static Merkle_Hash merkletree_hash_pair_(Merkle_Hash a, Merkle_Hash b)
{
    u8 combined[SHA256_DIGEST_SIZE * 2];
    Merkle_Hash h;
    size_t i;
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) combined[i] = a.hash[i];
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) combined[SHA256_DIGEST_SIZE + i] = b.hash[i];
    sha256_hash(combined, sizeof(combined), h.hash);
    return h;
}

/* builds every level of the tree into `a`, returning the root and
 * (if levels_out/level_sizes_out are non-NULL) the full level array
 * for proof generation - level 0 is the leaves, the last level is the
 * single-node root. */
static Merkle_Hash **merkletree_build_levels_(Arena *a, const void **blocks, const size_t *sizes, size_t count, size_t *out_level_count, size_t **out_level_sizes)
{
    size_t max_levels = 1;
    size_t n = count;
    Merkle_Hash **levels;
    size_t *level_sizes;
    size_t li;

    while (n > 1) { n = (n + 1) / 2; max_levels++; }

    levels = (Merkle_Hash **)arena_alloc(a, max_levels * sizeof(Merkle_Hash *));
    level_sizes = (size_t *)arena_alloc(a, max_levels * sizeof(size_t));

    levels[0] = (Merkle_Hash *)arena_alloc(a, count * sizeof(Merkle_Hash));
    level_sizes[0] = count;
    {
        size_t i;
        for (i = 0; i < count; i++) levels[0][i] = merkletree_hash_leaf_(blocks[i], sizes[i]);
    }

    li = 0;
    while (level_sizes[li] > 1) {
        size_t prev_count = level_sizes[li];
        size_t next_count = (prev_count + 1) / 2;
        size_t i;

        levels[li + 1] = (Merkle_Hash *)arena_alloc(a, next_count * sizeof(Merkle_Hash));
        for (i = 0; i < next_count; i++) {
            size_t left = i * 2;
            size_t right = left + 1;
            if (right < prev_count) {
                levels[li + 1][i] = merkletree_hash_pair_(levels[li][left], levels[li][right]);
            } else {
                levels[li + 1][i] = levels[li][left]; /* odd one out - promoted unchanged */
            }
        }
        level_sizes[li + 1] = next_count;
        li++;
    }

    *out_level_count = li + 1;
    *out_level_sizes = level_sizes;
    return levels;
}

Merkle_Hash merkletree_root(const void **blocks, const size_t *sizes, size_t count, Merkle_Hash *out_root)
{
    Arena scratch = {0};
    size_t level_count;
    size_t *level_sizes;
    Merkle_Hash **levels = merkletree_build_levels_(&scratch, blocks, sizes, count, &level_count, &level_sizes);
    Merkle_Hash root = levels[level_count - 1][0];
    arena_free(&scratch);
    if (out_root) *out_root = root;
    return root;
}

size_t merkletree_proof(const void **blocks, const size_t *sizes, size_t count, size_t index, Merkle_Hash *out_proof, size_t max_proof_len)
{
    Arena scratch = {0};
    size_t level_count;
    size_t *level_sizes;
    Merkle_Hash **levels = merkletree_build_levels_(&scratch, blocks, sizes, count, &level_count, &level_sizes);
    size_t li;
    size_t idx = index;
    size_t n = 0;

    for (li = 0; li < level_count - 1 && n < max_proof_len; li++) {
        size_t sibling = (idx % 2 == 0) ? idx + 1 : idx - 1;
        if (sibling < level_sizes[li]) {
            out_proof[n++] = levels[li][sibling];
        }
        /* if there's no sibling (odd one out, promoted unchanged),
         * nothing is appended for this level - verify's own promotion
         * logic mirrors this exactly */
        idx /= 2;
    }

    arena_free(&scratch);
    return n;
}

i32 merkletree_verify(const void *leaf_data, size_t leaf_size, size_t index, size_t leaf_count, const Merkle_Hash *proof, size_t proof_len, Merkle_Hash expected_root)
{
    Merkle_Hash current = merkletree_hash_leaf_(leaf_data, leaf_size);
    size_t idx = index;
    size_t level_size = leaf_count;
    size_t pi = 0;
    size_t i;

    for (i = 0; level_size > 1; i++) {
        size_t sibling = (idx % 2 == 0) ? idx + 1 : idx - 1;

        if (sibling < level_size) {
            if (pi >= proof_len) return 0; /* proof ran out but a real sibling was needed - can't verify */
            if (idx % 2 == 0) {
                current = merkletree_hash_pair_(current, proof[pi]);
            } else {
                current = merkletree_hash_pair_(proof[pi], current);
            }
            pi++;
        }
        /* else: odd one out at this level, current is promoted unchanged */

        idx /= 2;
        level_size = (level_size + 1) / 2;
    }

    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        if (current.hash[i] != expected_root.hash[i]) return 0;
    }
    return 1;
}

#endif /* MERKLETREE_IMPLEMENTATION_ */
#endif /* MERKLETREE_IMPLEMENTATION */
