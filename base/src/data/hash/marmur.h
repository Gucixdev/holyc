#ifndef HOLYC_MARMUR_H_
#define HOLYC_MARMUR_H_

/* MurmurHash3, x86_32 variant only (Austin Appleby, public domain) -
 * general-knowledge algorithm, not a port. The 128-bit x64 variant is a
 * real, separate undertaking and deliberately not attempted here -
 * 32-bit alone is a complete, useful deliverable (this project's
 * `src/core/base_type.h` already has `fnv1a_hash` as the simplest
 * option; this is a second, better-distributed non-cryptographic hash
 * for callers who specifically want MurmurHash3 - e.g. matching an
 * external system/format that specifies it).
 *
 * Every constant below (0xcc9e2d51, 0x1b873593, the rotl-15/13 shifts,
 * 0xe6546b64, and the finalization avalanche constants 0x85ebca6b/
 * 0xc2b2ae35) is a specific, must-be-exact published value, not
 * something approximated - verified against a real, independent
 * reference implementation (the `mmh3` Python package) across a real
 * test vector table, not just internal self-consistency - see
 * tests/test_marmur.c and docs/progress.md. */

#include "core/base.h"

u32 murmur3_32(const void *data, size_t len, u32 seed);

#endif /* HOLYC_MARMUR_H_ */

#ifdef MARMUR_IMPLEMENTATION
#ifndef MARMUR_IMPLEMENTATION_
#define MARMUR_IMPLEMENTATION_

static u32 marmur_rotl32_(u32 x, i32 r)
{
    return (x << r) | (x >> (32 - r));
}

static u32 marmur_fmix32_(u32 h)
{
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

u32 murmur3_32(const void *data, size_t len, u32 seed)
{
    const u8 *p = (const u8 *)data;
    size_t n_blocks = len / 4;
    u32 h1 = seed;
    const u32 c1 = 0xcc9e2d51u;
    const u32 c2 = 0x1b873593u;
    size_t i;
    u32 k1;
    const u8 *tail;

    for (i = 0; i < n_blocks; i++) {
        const u8 *b = p + i * 4;
        u32 k = (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);

        k *= c1;
        k = marmur_rotl32_(k, 15);
        k *= c2;

        h1 ^= k;
        h1 = marmur_rotl32_(h1, 13);
        h1 = h1 * 5u + 0xe6546b64u;
    }

    tail = p + n_blocks * 4;
    k1 = 0;
    switch (len & 3) {
    case 3: k1 ^= (u32)tail[2] << 16; /* fall through */
    case 2: k1 ^= (u32)tail[1] << 8;  /* fall through */
    case 1:
        k1 ^= (u32)tail[0];
        k1 *= c1;
        k1 = marmur_rotl32_(k1, 15);
        k1 *= c2;
        h1 ^= k1;
        break;
    default: break;
    }

    h1 ^= (u32)len;
    h1 = marmur_fmix32_(h1);

    return h1;
}

#endif /* MARMUR_IMPLEMENTATION_ */
#endif /* MARMUR_IMPLEMENTATION */
