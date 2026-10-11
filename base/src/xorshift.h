#ifndef HOLYC_XORSHIFT_H_
#define HOLYC_XORSHIFT_H_

/* xorshift32 (Marsaglia 2003) - the simplest, well-documented xorshift
 * variant: a single u32 state, three xor-shifts, period 2^32-1 (every
 * value except 0, which is why seed 0 is rejected below). General-
 * knowledge algorithm, not a port.
 *
 * `src/math/random/rng.h` (PCG32) already exists as this project's
 * default general-purpose PRNG and its own header comment explains why
 * it's usually the better choice - xorshift32's LOW bits are known-weak
 * (bit 0 alone has period 2, not the full 2^32-1), a real problem for
 * anything drawing small integers via a bitmask or a modulo against a
 * small power of two. This file exists for callers who specifically
 * want xorshift32 itself (its own smaller state, or matching an
 * external system/paper that specifies it) - not a default recommendation
 * over rng.h. */

#include "base.h"
#include "contracts.h"

typedef struct {
    u32 state;
} Xorshift32;

/* seed must not be 0 (0 is xorshift32's fixed point - it would never
 * advance) - seed 0 is silently remapped to 1 rather than treated as an
 * error, since a caller passing e.g. a hash that happened to be 0 is a
 * normal case, not a bug worth failing on. */
void xorshift32_seed(Xorshift32 *r, u32 seed);
u32  xorshift32_u32(Xorshift32 *r);
f32  xorshift32_f32(Xorshift32 *r); /* [0, 1) */
u32  xorshift32_range(Xorshift32 *r, u32 lo, u32 hi_exclusive); /* [lo, hi_exclusive) */

#endif /* HOLYC_XORSHIFT_H_ */

#ifdef XORSHIFT_IMPLEMENTATION
#ifndef XORSHIFT_IMPLEMENTATION_
#define XORSHIFT_IMPLEMENTATION_

void xorshift32_seed(Xorshift32 *r, u32 seed)
{
    r->state = (seed == 0) ? 1u : seed;
}

u32 xorshift32_u32(Xorshift32 *r)
{
    u32 x = r->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->state = x;
    return x;
}

f32 xorshift32_f32(Xorshift32 *r)
{
    return (f32)xorshift32_u32(r) / (f32)4294967296.0; /* 2^32, lands in [0,1) not [0,1] */
}

u32 xorshift32_range(Xorshift32 *r, u32 lo, u32 hi_exclusive)
{
    u32 result;

    HOLYC_REQUIRES(hi_exclusive > lo); /* hi_exclusive <= lo would divide by zero (or wrap to a huge modulus) below - a real precondition, not previously stated anywhere */

    result = lo + xorshift32_u32(r) % (hi_exclusive - lo);

    HOLYC_ENSURES(result >= lo && result < hi_exclusive);
    return result;
}

#endif /* XORSHIFT_IMPLEMENTATION_ */
#endif /* XORSHIFT_IMPLEMENTATION */
