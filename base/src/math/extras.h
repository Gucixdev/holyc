#ifndef HOLYC_EXTRAS_H_
#define HOLYC_EXTRAS_H_

/* Ratio utilities: simplify a ratio to lowest terms (gcd-based),
 * derive one from a pair of dimensions (e.g. 1920x1080 -> 16:9), and
 * convert to a float. Kept in its own small file per this file's own
 * original musing (comparing itself to merkletree.h/marmur.h - small,
 * single-purpose files living on their own rather than crammed into a
 * bigger one) - NOT a lerp/interpolation file, that's easing.h's job
 * already (src/math/easing.h) - no duplication with it here. */

#include "core/base.h"

typedef struct {
    i32 num;
    i32 den;
} Ratio;

Ratio ratio_simplify(i32 num, i32 den);
Ratio ratio_from_dimensions(i32 width, i32 height);
f64 ratio_to_f64(Ratio r);
i32 ratio_eq(Ratio a, Ratio b); /* compares simplified forms, so 2:4 == 1:2 */

#endif /* HOLYC_EXTRAS_H_ */

#ifdef EXTRAS_IMPLEMENTATION
#ifndef EXTRAS_IMPLEMENTATION_
#define EXTRAS_IMPLEMENTATION_

static i32 extras_gcd_(i32 a, i32 b)
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        i32 t = b;
        b = a % b;
        a = t;
    }
    return a;
}

Ratio ratio_simplify(i32 num, i32 den)
{
    Ratio r;
    i32 g = extras_gcd_(num, den);

    if (g == 0) { r.num = num; r.den = den; return r; } /* both 0 - nothing meaningful to divide by, return as-is */

    r.num = num / g;
    r.den = den / g;

    /* keep the sign on num, not den, so a negative ratio always reads
     * as -n:d rather than n:-d */
    if (r.den < 0) { r.num = -r.num; r.den = -r.den; }

    return r;
}

Ratio ratio_from_dimensions(i32 width, i32 height)
{
    return ratio_simplify(width, height);
}

f64 ratio_to_f64(Ratio r)
{
    if (r.den == 0) return 0.0;
    return (f64)r.num / (f64)r.den;
}

i32 ratio_eq(Ratio a, Ratio b)
{
    Ratio sa = ratio_simplify(a.num, a.den);
    Ratio sb = ratio_simplify(b.num, b.den);
    return sa.num == sb.num && sa.den == sb.den;
}

#endif /* EXTRAS_IMPLEMENTATION_ */
#endif /* EXTRAS_IMPLEMENTATION */
