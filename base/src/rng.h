#ifndef HOLYC_RNG_H_
#define HOLYC_RNG_H_

/* PCG32 (O'Neill 2014, XSH-RR variant) - checked HandmadeMath, cglm,
 * linmath.h, ht.h, Klib and c-libs first: none of the vendored reference
 * libraries actually implement a PRNG, their test harnesses just call
 * libc rand(). This is a general-knowledge algorithm, not a port.
 *
 * Picked over xoshiro256 star-star on purpose: smaller state (one u64
 * plus one u64 increment, vs xoshiro's 256-bit array), simpler to seed
 * correctly (xoshiro wants a well-mixed seed, typically via splitmix64
 * first - one more moving part to get right), and there is no "which
 * variant" question (xoshiro comes in several flavors depending on what
 * you're doing - PCG32 is just PCG32). Passes BigCrush and PractRand
 * despite the small footprint. One obviously-good implementation over
 * several plausible ones, matching this project's own naming/dedup rule.
 *
 * src/ml/nn.h has its own separate xorshift32 for historical reasons (it
 * predates this file, and its own tests already pass against it) - not
 * migrated here, but any new code wanting a shared, better-quality PRNG
 * should reach for this one instead of xorshift32's known weak low bits
 * (xorshift32's bit 0 has period 2, not the full 2^32-1 - fine for weight
 * init, a bad idea for anything drawing small integers via a bitmask or
 * a modulo against a small power of two). */

#include "base.h"
#include "contracts.h"

typedef struct {
    u64 state;
    u64 inc; /* must always end up odd - pcg32_seed() guarantees this */
} Rng;

/* seq selects one of 2^63 independent output streams from the same seed -
 * pass 0 if you don't need that, most callers don't */
void pcg32_seed(Rng *r, u64 seed, u64 seq);
u32  pcg32_u32(Rng *r);
f32  pcg32_f32(Rng *r); /* [0, 1) */
u32  pcg32_range(Rng *r, u32 lo, u32 hi_exclusive); /* [lo, hi_exclusive) */

#endif /* HOLYC_RNG_H_ */

#ifdef RNG_IMPLEMENTATION
#ifndef RNG_IMPLEMENTATION_
#define RNG_IMPLEMENTATION_

u32 pcg32_u32(Rng *r)
{
    u64 old = r->state;
    u32 xorshifted, rot;

    /* the well-understood, provably-long-period part: a 64-bit LCG.
     * 6364136223846793005 is PCG's published multiplier, not a magic
     * number picked here. `UL`, not `ULL`: this project treats `long` as
     * 64-bit (verified true on the platforms this targets), same call
     * already made in tests/test_print.c rather than reach for a
     * `long long` literal suffix. */
    r->state = old * 6364136223846793005UL + r->inc;

    /* the permutation that destroys the LCG's classic low-bit weakness:
     * xor-shift the high bits down, then rotate by a variable amount
     * taken from the old state's very top bits */
    xorshifted = (u32)(((old >> 18) ^ old) >> 27);
    rot = (u32)(old >> 59);
    return (xorshifted >> rot) | (xorshifted << ((u32)(-(i32)rot) & 31));
}

void pcg32_seed(Rng *r, u64 seed, u64 seq)
{
    r->state = 0;
    r->inc = (seq << 1) | 1; /* force odd, as the algorithm requires */
    pcg32_u32(r);
    r->state += seed;
    pcg32_u32(r);
}

f32 pcg32_f32(Rng *r)
{
    return (f32)pcg32_u32(r) / (f32)4294967296.0; /* 2^32, so the result lands in [0,1) not [0,1] */
}

u32 pcg32_range(Rng *r, u32 lo, u32 hi_exclusive)
{
    u32 result;

    /* NOT the same function as pcg.h's own pcg32_range (Pcg32*, u32
     * bound) - that one is unbiased via rejection sampling; this one
     * (Rng*, u32 lo, u32 hi_exclusive) is a plain modulo, a real,
     * separate implementation this file's own header comment already
     * distinguishes from pcg.h. Its checkable property is the same
     * shape as xorshift32_range's, though: the result must land in the
     * stated half-open range, whatever the exact distribution. */
    HOLYC_REQUIRES(hi_exclusive > lo);

    result = lo + pcg32_u32(r) % (hi_exclusive - lo);

    HOLYC_ENSURES(result >= lo && result < hi_exclusive);
    return result;
}

#endif /* RNG_IMPLEMENTATION_ */
#endif /* RNG_IMPLEMENTATION */
