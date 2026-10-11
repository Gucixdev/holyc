#ifndef HOLYC_MEAN_H_
#define HOLYC_MEAN_H_

/* Arithmetic mean, nothing more - kept independent of variance.h (no
 * mutual dependency in either direction) so either can be swapped or
 * edited without touching the other, same "minimal, independent" split
 * as vector.h/matrix.h/quat.h. variance.h takes an already-computed
 * mean as a plain parameter rather than calling into this file
 * internally, for the same reason. */

#include "types/base.h"

f32 mean_f32(const f32 *data, size_t n);

#endif /* HOLYC_MEAN_H_ */

#ifdef MEAN_IMPLEMENTATION
#ifndef MEAN_IMPLEMENTATION_
#define MEAN_IMPLEMENTATION_

f32 mean_f32(const f32 *data, size_t n)
{
    f64 sum = 0.0; /* f64 accumulator - summing many f32s in f32 loses
                      precision fast, the classic naive-mean footgun */
    size_t i;
    for (i = 0; i < n; i++) sum += (f64)data[i];
    return (f32)(sum / (f64)n);
}

#endif /* MEAN_IMPLEMENTATION_ */
#endif /* MEAN_IMPLEMENTATION */
