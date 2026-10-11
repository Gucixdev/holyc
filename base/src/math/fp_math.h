#ifndef HOLYC_FP_MATH_H_
#define HOLYC_FP_MATH_H_

/* Extends `src/core/base_type.h`'s existing fp16/fp32/fp64 Q-format
 * fixed-point types with the one operation they don't have: sqrt (that
 * file's own comment already covers why add/sub need no wrapper - plain
 * +/- on the underlying int - and documents fp64_mul/fp64_div as a
 * known, deliberate gap). This file does NOT redefine fp16/fp32/fp64 or
 * duplicate from_f32/to_f32/mul/div - it `#include`s base_type.h for
 * those and only adds what's missing, checked before writing a single
 * line here (a real, avoidable duplication this project already made
 * once this session with vector.h/matrix.h/quat.h's own pcg.h mixup -
 * verify before you build, not after).
 *
 * fp32 only (Q16.16) for now - the most commonly useful width; fp16/
 * fp64 sqrt are a documented gap, not silently missing, same
 * "in scope but not yet done" honesty base_type.h's own fp64_mul/div
 * comment already models. Trig functions (sin/cos/atan2 in fixed
 * point) are ALSO a documented gap - a real, separate, bigger
 * undertaking (a CORDIC implementation or a lookup table with
 * interpolation), not attempted here. */

#include "types/base.h"
#include "types/contracts.h"

/* integer square root via the classic bit-by-bit ("digit by digit")
 * algorithm, computed at u64 width so the full Q16.16 fixed-point
 * precision survives - see the implementation for the derivation.
 * Negative input is a domain error; returns 0 (a defined, if
 * nonsensical, result rather than undefined behavior). */
fp32 fp32_sqrt(fp32 x);

#endif /* HOLYC_FP_MATH_H_ */

#ifdef FP_MATH_IMPLEMENTATION
#ifndef FP_MATH_IMPLEMENTATION_
#define FP_MATH_IMPLEMENTATION_

/* standard digit-by-digit u64 integer square root - finds the largest
 * res such that res*res <= n, one 2-bit "digit" of the result at a
 * time from the most significant end. */
static u64 fp_math_isqrt64_(u64 n)
{
    u64 orig_n = n; /* n itself is the loop's own working accumulator below, mutated as it goes - the ENSURES checks at the end need the ORIGINAL value, not whatever n has been whittled down to */
    u64 res = 0;
    u64 bit = (u64)1 << 62; /* the highest even power-of-4 bit position for a 64-bit input */

    while (bit > n) bit >>= 2;

    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }

    /* the actual defining property of integer square root - res is the
     * LARGEST value whose square doesn't exceed n. The upper-bound
     * check computes (res+1)*(res+1), which would itself overflow u64
     * if res were near U64_MAX/2 - not a real concern for THIS file's
     * only caller (fp32_sqrt feeds n <= (u32_max)<<16, so res stays
     * under 2^24), and proof_cbmc's own harness bounds n explicitly for
     * the same reason arena_alloc's harness bounds allocation sizes. */
    HOLYC_ENSURES(res * res <= orig_n);
    HOLYC_ENSURES(orig_n < (res + 1) * (res + 1));

    return res;
}

fp32 fp32_sqrt(fp32 x)
{
    u64 x_q32_32;

    if (x <= 0) return 0;

    /* x is Q16.16 (x_real * 2^16). isqrt(x_real * 2^32) == sqrt(x_real) * 2^16,
       exactly the Q16.16 representation of sqrt(x_real) - so widen x by
       another 16 bits (to u64, avoiding i32 overflow) before taking the
       integer square root. */
    x_q32_32 = (u64)(u32)x << 16;
    return (fp32)fp_math_isqrt64_(x_q32_32);
}

#endif /* FP_MATH_IMPLEMENTATION_ */
#endif /* FP_MATH_IMPLEMENTATION */
