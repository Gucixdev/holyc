#ifndef HOLYC_VARIANCE_H_
#define HOLYC_VARIANCE_H_

/* Population variance/standard deviation - deliberately independent of
 * mean.h (no #include, no call into it): both functions take an
 * already-computed mean as a plain f32 parameter, matching how a
 * caller most naturally uses these together (compute mean once, reuse
 * it for variance without a hidden second pass or a hard dependency
 * either file would otherwise have on the other). Population, not
 * sample, variance (dividing by n, not n-1) - the more common default
 * when the data IS the whole population being described (e.g.
 * benchmark timing samples, a rendered frame's pixel values), not an
 * estimate of some larger population's variance; a caller wanting
 * Bessel's correction can trivially multiply by n/(n-1) themselves. */

#include "types/base.h"

f32 variance_f32(const f32 *data, size_t n, f32 mean);
f32 stddev_f32(const f32 *data, size_t n, f32 mean);

#endif /* HOLYC_VARIANCE_H_ */

#ifdef VARIANCE_IMPLEMENTATION
#ifndef VARIANCE_IMPLEMENTATION_
#define VARIANCE_IMPLEMENTATION_

/* Newton-Raphson sqrt, replacing math.h's - own local copy (see
 * src/base.h's own vector_sqrt_/quat_sqrt_/matrix_sqrt_ for the
 * precedent and algorithm) - part of this project's "0 glibc" push. */
static f64 variance_sqrt_(f64 x)
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

f32 variance_f32(const f32 *data, size_t n, f32 mean)
{
    f64 sum_sq_dev = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        f64 dev = (f64)data[i] - (f64)mean;
        sum_sq_dev += dev * dev;
    }
    return (f32)(sum_sq_dev / (f64)n);
}

f32 stddev_f32(const f32 *data, size_t n, f32 mean)
{
    return (f32)variance_sqrt_((f64)variance_f32(data, n, mean));
}

#endif /* VARIANCE_IMPLEMENTATION_ */
#endif /* VARIANCE_IMPLEMENTATION */
