#ifndef HOLYC_VERIFY_H_
#define HOLYC_VERIFY_H_

/* Formal-verification-flavored checked arithmetic - the real, achievable
 * first slice of a bigger stated goal (own prose spec: "weryfikacja
 * formalna h+c... laczenie i upraszczanie skladni hsbmc i ascl" - unify
 * ESBMC-style bounded-model-checking assertions and Frama-C/ACSL-style
 * formal contracts into one project-own syntax). Building an actual SMT-
 * backed bounded model checker (what ESBMC/CBMC are) or a Frama-C-grade
 * specification/proof toolchain is a research-tool-scale undertaking,
 * not attempted here - what IS real and shipped in this file is the
 * RUNTIME half: every hazard a caller would otherwise want a model
 * checker to prove absent (division by zero, division overflow,
 * add/sub/mul overflow, non-finite float results) is instead checked
 * explicitly, every time, unconditionally (NOT compiled out under
 * NDEBUG the way HOLYC_ASSERT is - this project's own bit.h/base.h
 * asserts are debug guardrails, these are correctness guarantees) and
 * reported through a Proof_Error result rather than trapping blind.
 *
 * Convention: `verify is: <precondition>` directly above the check that
 * enforces it - the terse, one-line comment style the spec asked for
 * ("verify is" instead of writing out "if this holds") - executable
 * documentation, not itself an annotation any tool parses yet. A real
 * static scanner that reads these comments and cross-checks them against
 * an ESBMC/ACSL-style external prover is the explicitly bigger, not-yet-
 * attempted part of the original goal. */

#include "base.h"
#include <string.h> /* memcpy - verify_finite_f32/f64's bit-pattern read */

typedef enum {
    PROOF_ESUCCESS     = 0,
    PROOF_EDIV_BY_ZERO = -1, /* divisor is exactly 0 */
    PROOF_EOVERFLOW    = -2  /* result does not fit the target type */
} Proof_Error;

/* verify is: b != 0, and not (a == i32 min && b == -1) */
Proof_Error verify_div_i32(i32 a, i32 b, i32 *out);
/* verify is: b != 0, and not (a == i64 min && b == -1) */
Proof_Error verify_div_i64(i64 a, i64 b, i64 *out);

/* verify is: a + b fits in i32 without wrapping */
Proof_Error verify_add_i32(i32 a, i32 b, i32 *out);
/* verify is: a - b fits in i32 without wrapping */
Proof_Error verify_sub_i32(i32 a, i32 b, i32 *out);
/* verify is: a * b fits in i32 without wrapping */
Proof_Error verify_mul_i32(i32 a, i32 b, i32 *out);

/* verify is: a + b fits in i64 without wrapping */
Proof_Error verify_add_i64(i64 a, i64 b, i64 *out);
/* verify is: a - b fits in i64 without wrapping */
Proof_Error verify_sub_i64(i64 a, i64 b, i64 *out);
/* verify is: a * b fits in i64 without wrapping */
Proof_Error verify_mul_i64(i64 a, i64 b, i64 *out);

/* verify is: x is neither NaN nor +-Infinity */
Proof_Error verify_finite_f32(f32 x);
/* verify is: x is neither NaN nor +-Infinity */
Proof_Error verify_finite_f64(f64 x);

const char *verify_strerror(Proof_Error err);

#endif /* HOLYC_VERIFY_H_ */

#ifdef PROOF_IMPLEMENTATION
#ifndef PROOF_IMPLEMENTATION_
#define PROOF_IMPLEMENTATION_

const char *verify_strerror(Proof_Error err)
{
    switch (err) {
    case PROOF_ESUCCESS:     return "success";
    case PROOF_EDIV_BY_ZERO: return "division by zero";
    case PROOF_EOVERFLOW:    return "result overflows the target type";
    default: break;
    }
    return "unknown error";
}

Proof_Error verify_div_i32(i32 a, i32 b, i32 *out)
{
    /* verify is: b != 0 */
    if (b == 0) return PROOF_EDIV_BY_ZERO;
    /* verify is: not (a == i32 min && b == -1) - the one i32/i32 division
     * that overflows: i32 min's magnitude has no positive i32
     * counterpart to negate into. */
    if (a == (i32)((u32)1 << 31) && b == -1) return PROOF_EOVERFLOW;
    *out = a / b;
    return PROOF_ESUCCESS;
}

Proof_Error verify_div_i64(i64 a, i64 b, i64 *out)
{
    /* verify is: b != 0 */
    if (b == 0) return PROOF_EDIV_BY_ZERO;
    /* verify is: not (a == i64 min && b == -1) - same hazard as
     * verify_div_i32, one width up. */
    if (a == (i64)((u64)1 << 63) && b == -1) return PROOF_EOVERFLOW;
    *out = a / b;
    return PROOF_ESUCCESS;
}

Proof_Error verify_add_i32(i32 a, i32 b, i32 *out)
{
    /* verify is: a + b fits in i32 without wrapping */
    if (__builtin_add_overflow(a, b, out)) return PROOF_EOVERFLOW;
    return PROOF_ESUCCESS;
}

Proof_Error verify_sub_i32(i32 a, i32 b, i32 *out)
{
    /* verify is: a - b fits in i32 without wrapping */
    if (__builtin_sub_overflow(a, b, out)) return PROOF_EOVERFLOW;
    return PROOF_ESUCCESS;
}

Proof_Error verify_mul_i32(i32 a, i32 b, i32 *out)
{
    /* verify is: a * b fits in i32 without wrapping */
    if (__builtin_mul_overflow(a, b, out)) return PROOF_EOVERFLOW;
    return PROOF_ESUCCESS;
}

Proof_Error verify_add_i64(i64 a, i64 b, i64 *out)
{
    /* verify is: a + b fits in i64 without wrapping */
    if (__builtin_add_overflow(a, b, out)) return PROOF_EOVERFLOW;
    return PROOF_ESUCCESS;
}

Proof_Error verify_sub_i64(i64 a, i64 b, i64 *out)
{
    /* verify is: a - b fits in i64 without wrapping */
    if (__builtin_sub_overflow(a, b, out)) return PROOF_EOVERFLOW;
    return PROOF_ESUCCESS;
}

Proof_Error verify_mul_i64(i64 a, i64 b, i64 *out)
{
    /* verify is: a * b fits in i64 without wrapping */
    if (__builtin_mul_overflow(a, b, out)) return PROOF_EOVERFLOW;
    return PROOF_ESUCCESS;
}

Proof_Error verify_finite_f32(f32 x)
{
    /* verify is: x's exponent field is not all-1s (that pattern is
     * reserved for NaN/+-Infinity in IEEE-754) - read through a same-
     * size memcpy, not a pointer-cast reinterpret, to stay clear of the
     * strict-aliasing UB a `*(u32*)&x` cast would risk. */
    u32 bits;
    memcpy(&bits, &x, sizeof(bits));
    if (((bits >> 23) & 0xFFu) == 0xFFu) return PROOF_EOVERFLOW;
    return PROOF_ESUCCESS;
}

Proof_Error verify_finite_f64(f64 x)
{
    /* verify is: x's exponent field is not all-1s - same check as
     * verify_finite_f32, f64's exponent is 11 bits wide instead of 8. */
    u64 bits;
    memcpy(&bits, &x, sizeof(bits));
    if (((bits >> 52) & 0x7FFu) == 0x7FFu) return PROOF_EOVERFLOW;
    return PROOF_ESUCCESS;
}

#endif /* PROOF_IMPLEMENTATION_ */
#endif /* PROOF_IMPLEMENTATION */
