#ifndef HOLYC_TINYXXHASH_H_
#define HOLYC_TINYXXHASH_H_

/* XXH32 (Yann Collet, public domain / BSD) - general-knowledge
 * algorithm, not a port. XXH64/XXH3 are real, separate undertakings,
 * deliberately not attempted here - XXH32 alone is a complete, useful
 * deliverable.
 *
 * Named tinyxxhash.h, not xxhash.h, deliberately: this machine has a
 * REAL system header at /usr/include/xxhash.h (the reference xxHash
 * single-header library, installed as a package) - a project file
 * literally named xxhash.h would silently shadow it for any
 * `#include <xxhash.h>` anywhere in the include path (the exact
 * hazard tinytar.h's own rename already fixed for <tar.h>, and
 * ./check's name-hygiene section now catches automatically).
 *
 * Every constant below (the 5 PRIME32 values, the 16-byte-stripe
 * accumulator mixing, the finalization avalanche) is a specific,
 * must-be-exact published value, not something approximated -
 * verified against TWO independent real reference implementations
 * (the `xxhash` Python package AND the `xxhsum` reference CLI binary,
 * which agree exactly) across a real test vector table, not just
 * internal self-consistency - see tests/test_tinyxxhash.c and
 * docs/progress.md. */

#include "core/base.h"

u32 xxhash32(const void *data, size_t len, u32 seed);

#endif /* HOLYC_TINYXXHASH_H_ */

#ifdef TINYXXHASH_IMPLEMENTATION
#ifndef TINYXXHASH_IMPLEMENTATION_
#define TINYXXHASH_IMPLEMENTATION_

#define XXH_PRIME32_1_ 2654435761u
#define XXH_PRIME32_2_ 2246822519u
#define XXH_PRIME32_3_ 3266489917u
#define XXH_PRIME32_4_  668265263u
#define XXH_PRIME32_5_  374761393u

static u32 xxh32_rotl_(u32 x, i32 r)
{
    return (x << r) | (x >> (32 - r));
}

static u32 xxh32_read_u32le_(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static u32 xxh32_round_(u32 acc, u32 input)
{
    acc += input * XXH_PRIME32_2_;
    acc = xxh32_rotl_(acc, 13);
    acc *= XXH_PRIME32_1_;
    return acc;
}

u32 xxhash32(const void *data, size_t len, u32 seed)
{
    const u8 *p = (const u8 *)data;
    const u8 *end = p + len;
    u32 h32;

    if (len >= 16) {
        const u8 *limit = end - 16;
        u32 v1 = seed + XXH_PRIME32_1_ + XXH_PRIME32_2_;
        u32 v2 = seed + XXH_PRIME32_2_;
        u32 v3 = seed;
        u32 v4 = seed - XXH_PRIME32_1_;

        do {
            v1 = xxh32_round_(v1, xxh32_read_u32le_(p));      p += 4;
            v2 = xxh32_round_(v2, xxh32_read_u32le_(p));      p += 4;
            v3 = xxh32_round_(v3, xxh32_read_u32le_(p));      p += 4;
            v4 = xxh32_round_(v4, xxh32_read_u32le_(p));      p += 4;
        } while (p <= limit);

        h32 = xxh32_rotl_(v1, 1) + xxh32_rotl_(v2, 7) + xxh32_rotl_(v3, 12) + xxh32_rotl_(v4, 18);
    } else {
        h32 = seed + XXH_PRIME32_5_;
    }

    h32 += (u32)len;

    while (p + 4 <= end) {
        h32 += xxh32_read_u32le_(p) * XXH_PRIME32_3_;
        h32 = xxh32_rotl_(h32, 17) * XXH_PRIME32_4_;
        p += 4;
    }

    while (p < end) {
        h32 += (u32)(*p) * XXH_PRIME32_5_;
        h32 = xxh32_rotl_(h32, 11) * XXH_PRIME32_1_;
        p++;
    }

    h32 ^= h32 >> 15;
    h32 *= XXH_PRIME32_2_;
    h32 ^= h32 >> 13;
    h32 *= XXH_PRIME32_3_;
    h32 ^= h32 >> 16;

    return h32;
}

#endif /* TINYXXHASH_IMPLEMENTATION_ */
#endif /* TINYXXHASH_IMPLEMENTATION */
