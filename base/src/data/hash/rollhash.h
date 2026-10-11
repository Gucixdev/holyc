#ifndef HOLYC_ROLLHASH_H_
#define HOLYC_ROLLHASH_H_

/* Rolling polynomial hash (Rabin-Karp style) - unlike marmur.h/
 * tinyxxhash.h's one-shot murmur3_32()/xxhash32() which must rehash
 * the whole buffer from scratch, rollhash_roll() updates the hash in
 * O(1) as a fixed-size window slides one byte at a time: drop the
 * outgoing byte's contribution, shift, add the incoming byte. That is
 * the whole reason this exists as its own file - built for sliding-
 * window use cases (substring search, content-defined chunking) that
 * a one-shot hash can't serve efficiently.
 *
 * Arithmetic is plain u32, letting overflow wrap mod 2^32 (well-
 * defined for unsigned types in C, not UB) - the same simple/cheap
 * approach rsync's rolling checksum popularized, good enough for
 * chunking/search use, NOT a cryptographic or collision-resistant
 * hash (same disclaimer marmur.h/tinyxxhash.h already carry). */

#include "core/base.h"

#define ROLLHASH_BASE 257u

typedef struct {
    u32 hash;
    u32 base_pow; /* BASE^(window_len-1) - the outgoing byte's weight */
} Roll_Hash;

/* computes the initial hash over `window[0..window_len)` and the
 * base_pow weight rollhash_roll() will need. window_len must be >= 1. */
void rollhash_init(Roll_Hash *r, const u8 *window, size_t window_len);

u32 rollhash_value(const Roll_Hash *r);

/* slides the window forward by exactly one byte: old_byte leaves from
 * the front, new_byte joins at the back. */
void rollhash_roll(Roll_Hash *r, u8 old_byte, u8 new_byte);

#endif /* HOLYC_ROLLHASH_H_ */

#ifdef ROLLHASH_IMPLEMENTATION
#ifndef ROLLHASH_IMPLEMENTATION_
#define ROLLHASH_IMPLEMENTATION_

void rollhash_init(Roll_Hash *r, const u8 *window, size_t window_len)
{
    size_t i;

    r->hash = 0;
    for (i = 0; i < window_len; i++) {
        r->hash = r->hash * ROLLHASH_BASE + window[i];
    }

    r->base_pow = 1;
    for (i = 1; i < window_len; i++) {
        r->base_pow *= ROLLHASH_BASE;
    }
}

u32 rollhash_value(const Roll_Hash *r) { return r->hash; }

void rollhash_roll(Roll_Hash *r, u8 old_byte, u8 new_byte)
{
    r->hash = (r->hash - (u32)old_byte * r->base_pow) * ROLLHASH_BASE + new_byte;
}

#endif /* ROLLHASH_IMPLEMENTATION_ */
#endif /* ROLLHASH_IMPLEMENTATION */
