#ifndef HOLYC_TENARY_GATES_H_
#define HOLYC_TENARY_GATES_H_

/* Real ternary logic gates - "tgates czyli tenary gates min max neg
 * dif ... podobnie jak units tylko biblioteko-narzedzie" (this
 * project's own spec, verbatim): the standard Kleene/Lukasiewicz
 * three-valued-logic primitives, defined over BALANCED ternary
 * (Trit, one of -1/0/+1 - the same representation BitNet's own
 * ternary weights use, and tenary.h's own header comment already
 * names as a real, not hypothetical, motivation). Kept as a separate
 * file from tenary.h on purpose (this project's own explicit
 * decision): tenary.h owns packing/compression (unbalanced
 * [0,radix) symbols, delta+zigzag+Golomb-Rice), this file owns
 * per-trit LOGIC - different concerns, not two copies of the same
 * thing.
 *
 * tmin/tmax ARE ternary AND/OR (min/max over {-1,0,1} is exactly
 * Kleene's own K3 conjunction/disjunction - not a new definition
 * invented here). tneg is ternary NOT (negation - the only sane
 * meaning of NOT over a signed 3-valued domain). tdiff is a three-way
 * COMPARE (sign(a-b): -1 if a<b, 0 if a==b, +1 if a>b) - the natural
 * reading of "difference" as its own gate rather than plain
 * subtraction (a-b can be -2..2, outside Trit's own range; tdiff
 * saturates that back down to a real trit, same idea as a spaceship
 * operator).
 *
 * balanced/SIMD/LUT work for tenary.h itself ("1/5log balanced+packed
 * simd+lut w jednym pliku tenary") is real, separate, queued follow-up
 * work on THAT file, not bundled into this one - see tenary/readme's
 * own roadmap. */

#include "base.h"

typedef i8 Trit; /* -1, 0, or +1 - caller-guaranteed, not clamped on input (see tenary_gates_clamp below for the one place that IS meant to produce an out-of-range sum safely) */

Trit tneg(Trit a);
Trit tmin(Trit a, Trit b);
Trit tmax(Trit a, Trit b);
Trit tdiff(Trit a, Trit b);

/* array/vector forms - real bulk use (a row of trits), matching this
 * project's own array-oriented convention (tenary_pack/unpack take
 * arrays, not single symbols, for the same reason). out may alias a
 * or b (element-wise, no cross-index read-after-write). */
void tneg_array(Trit *out, const Trit *in, size_t n);
void tmin_array(Trit *out, const Trit *a, const Trit *b, size_t n);
void tmax_array(Trit *out, const Trit *a, const Trit *b, size_t n);
void tdiff_array(Trit *out, const Trit *a, const Trit *b, size_t n);

#endif /* HOLYC_TENARY_GATES_H_ */

#ifdef TENARY_GATES_IMPLEMENTATION
#ifndef TENARY_GATES_IMPLEMENTATION_
#define TENARY_GATES_IMPLEMENTATION_

Trit tneg(Trit a) { return (Trit)(-a); }
Trit tmin(Trit a, Trit b) { return a < b ? a : b; }
Trit tmax(Trit a, Trit b) { return a > b ? a : b; }

Trit tdiff(Trit a, Trit b)
{
    i32 d = (i32)a - (i32)b; /* -2..2, widened so the comparison below can't itself overflow a Trit */
    if (d > 0) return 1;
    if (d < 0) return -1;
    return 0;
}

void tneg_array(Trit *out, const Trit *in, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) out[i] = tneg(in[i]);
}

void tmin_array(Trit *out, const Trit *a, const Trit *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) out[i] = tmin(a[i], b[i]);
}

void tmax_array(Trit *out, const Trit *a, const Trit *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) out[i] = tmax(a[i], b[i]);
}

void tdiff_array(Trit *out, const Trit *a, const Trit *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) out[i] = tdiff(a[i], b[i]);
}

#endif /* TENARY_GATES_IMPLEMENTATION_ */
#endif /* TENARY_GATES_IMPLEMENTATION */
