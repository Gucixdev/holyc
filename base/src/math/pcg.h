#ifndef HOLYC_PCG_H_
#define HOLYC_PCG_H_

/* PCG32 (O'Neill's Permuted Congruential Generator, the pcg32_random_r
 * variant from the reference C implementation, minimal C version -
 * https://www.pcg-random.org) - a real, specific, well-tested RNG
 * algorithm distinct from xorshift.h (same directory): a 64-bit LCG
 * state, output permuted via an xorshift+random-rotate step that
 * gives PCG its much better statistical quality than a plain LCG at
 * roughly the same cost. Two generators seeded from different `seq`
 * values produce independent streams even from the same seed - useful
 * for e.g. one stream per game-AI/procgen system that all still trace
 * back to one master seed. */

#include "types/base.h"
#include "types/contracts.h"

typedef struct {
    u64 state;
    u64 inc;
} Pcg32;

void pcg32_seed(Pcg32 *rng, u64 initstate, u64 initseq);
u32 pcg32_next(Pcg32 *rng);
f32 pcg32_next_f32(Pcg32 *rng); /* [0, 1) */
u32 pcg32_range(Pcg32 *rng, u32 bound); /* [0, bound), unbiased (rejection-sampled, per the reference algorithm) */

#endif /* HOLYC_PCG_H_ */

#ifdef PCG_IMPLEMENTATION
#ifndef PCG_IMPLEMENTATION_
#define PCG_IMPLEMENTATION_

u32 pcg32_next(Pcg32 *rng)
{
    u64 oldstate = rng->state;
    u32 xorshifted;
    u32 rot;

    rng->state = oldstate * 6364136223846793005UL + rng->inc;

    xorshifted = (u32)(((oldstate >> 18) ^ oldstate) >> 27);
    rot = (u32)(oldstate >> 59);
    return (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
}

void pcg32_seed(Pcg32 *rng, u64 initstate, u64 initseq)
{
    rng->state = 0;
    rng->inc = (initseq << 1) | 1;
    pcg32_next(rng);
    rng->state += initstate;
    pcg32_next(rng);
}

f32 pcg32_next_f32(Pcg32 *rng)
{
    return (f32)(pcg32_next(rng) >> 8) / (f32)(1u << 24);
}

u32 pcg32_range(Pcg32 *rng, u32 bound)
{
    /* the reference algorithm's own unbiased-bounded-random technique:
     * reject draws that would make the modulo wrap unevenly across
     * [0, bound) instead of just doing % bound directly (which biases
     * toward small values whenever bound doesn't evenly divide 2^32) */
    u32 threshold;
    HOLYC_REQUIRES(bound > 0); /* bound==0 would divide by zero below - a real precondition, not previously stated anywhere */
    threshold = (u32)(-(i32)bound) % bound;
    for (;;) {
        u32 r = pcg32_next(rng);
        if (r >= threshold) {
            u32 result = r % bound;
            HOLYC_ENSURES(result < bound);
            return result;
        }
    }
}

#endif /* PCG_IMPLEMENTATION_ */
#endif /* PCG_IMPLEMENTATION */
