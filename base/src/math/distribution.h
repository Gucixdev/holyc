#ifndef HOLYC_DISTRIBUTION_H_
#define HOLYC_DISTRIBUTION_H_

/* Non-uniform distribution sampling, DELIBERATELY decoupled from any
 * specific generator: every function here takes already-generated
 * uniform f32 sample(s) in [0,1) as plain parameters, not a Rng
 * pointer or an Xorshift32 pointer. The caller draws the uniform
 * sample(s) from whichever generator it already has
 * (src/math/random/rng.h's PCG32, xorshift.h, anything else) and
 * passes the number in - this avoids needing a generic RNG dispatch/
 * vtable layer that doesn't exist in this project, and keeps this file
 * testable with fixed, known inputs instead of needing a seeded
 * generator in every test. */

#include "core/base.h"

/* Box-Muller transform (Box & Muller 1958) - the standard method for
 * turning two independent uniform [0,1) samples into two independent
 * standard-normal (mean 0, variance 1) samples. u1 must be > 0 (a
 * uniform generator that can return exactly 0.0 would make log(u1)
 * undefined) - callers drawing from rng.h/xorshift.h in this project
 * never produce exactly 0.0 for a nonzero seed in practice, but this is
 * a real precondition of the math, not just a caller convention. */
void dist_gaussian_box_muller(f32 u1, f32 u2, f32 *out_z0, f32 *out_z1);

/* exponential distribution via inverse-CDF sampling: -ln(1-u)/rate.
 * rate must be > 0 (the distribution's rate parameter, e.g. events per
 * unit time - not a probability itself). */
f32 dist_exponential(f32 u, f32 rate);

#endif /* HOLYC_DISTRIBUTION_H_ */

#ifdef DISTRIBUTION_IMPLEMENTATION
#ifndef DISTRIBUTION_IMPLEMENTATION_
#define DISTRIBUTION_IMPLEMENTATION_

/* Newton-Raphson sqrt + range-reduced Taylor-series sin/cos + a
 * from-scratch natural log, replacing math.h's - own local copies,
 * matching base.h's own vector_sqrt_/quat_sin_/quat_cos_ pattern (0
 * glibc push, no-glibc rule). log(x) works
 * by decomposing x = m * 2^e (m in [1,2)) via the IEEE-754 bit layout
 * directly (union-based type pun, the same sanctioned GCC/Clang trick
 * quake's famous fast-inverse-sqrt uses for f32), then
 * ln(x) = ln(m) + e*ln(2) with ln(m) via 2*atanh((m-1)/(m+1))'s series
 * - converges fast since (m-1)/(m+1) is bounded to [0, 1/3) for m in
 * [1,2), unlike a raw Taylor series for ln(1+t) which converges slowly
 * near t=1. */
static f64 dist_sqrt_(f64 x)
{
    f64 guess;
    i32 i;
    if (x <= 0.0) return 0.0;
    guess = (x > 1.0) ? x : 1.0;
    for (i = 0; i < 40; i++) {
        f64 next = 0.5 * (guess + x / guess);
        if (next == guess) break;
        guess = next;
    }
    return guess;
}

static f64 dist_mod_2pi_(f64 x)
{
    const f64 two_pi = 6.283185307179586476925286766559;
    const f64 pi = 3.14159265358979323846;
    f64 n = (f64)(i64)(x / two_pi);
    f64 r = x - n * two_pi;
    if (r > pi) r -= two_pi;
    if (r < -pi) r += two_pi;
    return r;
}

static f64 dist_sin_(f64 x)
{
    f64 r = dist_mod_2pi_(x);
    f64 x2 = r * r;
    f64 term = r;
    f64 sum = r;
    i32 n;
    for (n = 1; n <= 6; n++) {
        term *= -x2 / ((f64)(2 * n) * (f64)(2 * n + 1));
        sum += term;
    }
    return sum;
}

static f64 dist_cos_(f64 x)
{
    f64 r = dist_mod_2pi_(x);
    f64 x2 = r * r;
    f64 term = 1.0;
    f64 sum = 1.0;
    i32 n;
    for (n = 1; n <= 6; n++) {
        term *= -x2 / ((f64)(2 * n - 1) * (f64)(2 * n));
        sum += term;
    }
    return sum;
}

static f64 dist_log_(f64 x)
{
    union { f64 f; u64 u; } c;
    u64 bits;
    i32 exp2;
    f64 m, y, y2, term, sum;
    i32 n;
    const f64 ln2 = 0.69314718055994530942;

    if (x <= 0.0) return 0.0; /* no NaN/-inf infra without libm; domain error returns 0 rather than garbage */

    c.f = x;
    bits = c.u;
    exp2 = (i32)((bits >> 52) & 0x7ffUL) - 1023;
    bits = (bits & 0x800fffffffffffffUL) | ((u64)1023 << 52); /* reset exponent field to bias -> m in [1,2) */
    c.u = bits;
    m = c.f;

    y = (m - 1.0) / (m + 1.0);
    y2 = y * y;
    term = y;
    sum = y;
    for (n = 1; n <= 8; n++) {
        term *= y2;
        sum += term / (f64)(2 * n + 1);
    }

    return 2.0 * sum + (f64)exp2 * ln2;
}

void dist_gaussian_box_muller(f32 u1, f32 u2, f32 *out_z0, f32 *out_z1)
{
    f32 r = (f32)dist_sqrt_(-2.0 * dist_log_((f64)u1));
    f32 theta = (f32)(2.0 * 3.14159265358979323846 * (f64)u2);

    *out_z0 = r * (f32)dist_cos_((f64)theta);
    *out_z1 = r * (f32)dist_sin_((f64)theta);
}

f32 dist_exponential(f32 u, f32 rate)
{
    return (f32)(-dist_log_(1.0 - (f64)u) / (f64)rate);
}

#endif /* DISTRIBUTION_IMPLEMENTATION_ */
#endif /* DISTRIBUTION_IMPLEMENTATION */
