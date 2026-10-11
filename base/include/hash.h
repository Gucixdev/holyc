#ifndef HOLYC_MATH_HASH_H_
#define HOLYC_MATH_HASH_H_

/* Integer/coordinate hashing for procedural generation - deterministic
 * pseudo-random values FROM a coordinate (2D/3D grid cell, a seed),
 * the building block noise.h's own gradient/value noise sit on top of
 * (a lattice point's hash feeds its gradient vector) and what
 * dungeon/city procgen would seed per-cell randomness from. Distinct
 * from datastruct/hash/marmur.h and tinyxxhash.h, which hash arbitrary
 * BYTE BUFFERS - these hash small fixed sets of integers directly, no
 * buffer/length involved, cheap enough to call per-lattice-point in a
 * tight noise loop. */

#include "base.h"

u32 math_hash_u32(u32 x);              /* avalanche-mixes a single u32 */
u32 math_hash2_u32(u32 x, u32 y);      /* combines 2 coordinates into one hash */
u32 math_hash3_u32(u32 x, u32 y, u32 z); /* combines 3 coordinates into one hash */
u32 math_hash_seeded(u32 x, u32 seed);  /* same coordinate, different seed -> different (but still deterministic) stream */

#endif /* HOLYC_MATH_HASH_H_ */

#ifdef MATH_HASH_IMPLEMENTATION
#ifndef MATH_HASH_IMPLEMENTATION_
#define MATH_HASH_IMPLEMENTATION_

/* murmur3's own 32-bit finalizer (fmix32) - a well-known, well-tested
 * avalanche mix: every input bit flips roughly half the output bits. */
u32 math_hash_u32(u32 x)
{
    x ^= x >> 16;
    x *= 0x85ebca6bu;
    x ^= x >> 13;
    x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

u32 math_hash2_u32(u32 x, u32 y)
{
    return math_hash_u32(x ^ math_hash_u32(y));
}

u32 math_hash3_u32(u32 x, u32 y, u32 z)
{
    return math_hash_u32(x ^ math_hash_u32(y ^ math_hash_u32(z)));
}

u32 math_hash_seeded(u32 x, u32 seed)
{
    return math_hash_u32(x ^ math_hash_u32(seed));
}

#endif /* MATH_HASH_IMPLEMENTATION_ */
#endif /* MATH_HASH_IMPLEMENTATION */
