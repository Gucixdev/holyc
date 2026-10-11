#ifndef HOLYC_EASING_H_
#define HOLYC_EASING_H_

/* Every f(t)-in-[0,1]-out-[0,1] interpolation shape this project has -
 * lerp (lerpf/clampf/smoothstepf/vec2_lerp/vec3_lerp), quaternion slerp/
 * nlerp, and the named Penner-style easing curves (quad/cubic/sine/
 * back) - merged into one file (was lerp.h/slerp.h/easing.h, merged
 * back) since together they're "this
 * project's interpolation toolkit". Each piece still stands alone under
 * its OWN IMPLEMENTATION guard macro (LERP_IMPLEMENTATION/
 * SLERP_IMPLEMENTATION/EASING_IMPLEMENTATION, unchanged from the split
 * files) - a caller wanting only one still only pulls in that one's
 * code.
 *
 * lerp.h's own history: checked cglm's util.h and HandmadeMath's
 * HMM_Lerp before writing this - cglm's set is the more complete one
 * (clamped variant, smoothstep, inverse-lerp), ported that shape rather
 * than HandmadeMath's smaller one. Scalar f32 only; vec2/vec3 lerp is
 * one line each on top of mathx.h's existing vec ops.
 *
 * slerp.h's own history: HandmadeMath and cglm implement the identical
 * textbook spherical-linear-interpolation formula for quaternions,
 * ported here rather than picking one arbitrarily since there was
 * nothing to choose between (shortest-path sign fix + nlerp fallback
 * near cos_theta=1 to avoid dividing by a near-zero sin(angle) - see
 * quat_slerp's own body comment). Depends on mathx.h/linear_algebra.h
 * for Quat.
 *
 * easing.h's own history: named easing curves (the well-known "Penner
 * easing equations" shapes, general knowledge at this point, not a
 * specific port). Scope, stated plainly: quad/cubic/sine/back only -
 * quart/quint/expo/circ/elastic/bounce are a documented gap, not
 * silently missing. Every function satisfies f(0)==0, f(1)==1 exactly,
 * checked as a real property in tests/test_easing.c. */

#include "base.h"
#include "mathx.h"
#include "linear_algebra.h"
#include "contracts.h"

f32 clampf(f32 v, f32 lo, f32 hi);

f32 lerpf(f32 from, f32 to, f32 t);
f32 lerpcf(f32 from, f32 to, f32 t); /* t clamped to [0,1] first */
f32 percentf(f32 from, f32 to, f32 cur); /* inverse lerp: what t gave `cur`? */

f32 stepf(f32 edge, f32 x); /* 0 if x < edge, else 1 */
f32 smoothf(f32 t); /* the smoothstep polynomial itself: t*t*(3-2t), t already in [0,1] */
f32 smoothstepf(f32 edge0, f32 edge1, f32 x); /* smoothf() of x remapped into [0,1] over [edge0,edge1] */

vec2 vec2_lerp(vec2 from, vec2 to, f32 t);
vec3 vec3_lerp(vec3 from, vec3 to, f32 t);

Quat quat_nlerp(Quat a, Quat b, f32 t); /* normalized lerp - cheap, fine for small angular deltas */
Quat quat_slerp(Quat a, Quat b, f32 t);

f32 ease_in_quad(f32 t);
f32 ease_out_quad(f32 t);
f32 ease_in_out_quad(f32 t);

f32 ease_in_cubic(f32 t);
f32 ease_out_cubic(f32 t);
f32 ease_in_out_cubic(f32 t);

f32 ease_in_sine(f32 t);
f32 ease_out_sine(f32 t);
f32 ease_in_out_sine(f32 t);

/* "back" easing overshoots past the target before settling - a real,
 * named, distinct visual character from quad/cubic/sine, not another
 * shade of the same S-curve. c1=1.70158 is the standard published
 * constant (controls how far the overshoot goes), not something to
 * approximate. */
f32 ease_in_back(f32 t);
f32 ease_out_back(f32 t);
f32 ease_in_out_back(f32 t);

#endif /* HOLYC_EASING_H_ */

#ifdef LERP_IMPLEMENTATION
#ifndef LERP_IMPLEMENTATION_
#define LERP_IMPLEMENTATION_

/* branchless 3-way select: C's relational operators already yield exact
 * 0.0f/1.0f on cast (no rounding, IEEE754 int-to-float of 0 or 1 is
 * exact), so multiply-accumulate picks exactly one of {lo, hi, v} with no
 * conditional jump. Assumes lo <= hi, same as the branching form this
 * replaced - lo > hi was never a defined case either way. */
f32 clampf(f32 v, f32 lo, f32 hi)
{
    f32 below = (f32)(v < lo);
    f32 above = (f32)(v > hi);
    f32 within = 1.0f - below - above;
    f32 result = below * lo + above * hi + within * v;
    HOLYC_REQUIRES(lo <= hi);
    HOLYC_ENSURES(result >= lo && result <= hi);
    return result;
}

f32 lerpf(f32 from, f32 to, f32 t)
{
    return from + t * (to - from);
}

f32 lerpcf(f32 from, f32 to, f32 t)
{
    return lerpf(from, to, clampf(t, 0.0f, 1.0f));
}

f32 percentf(f32 from, f32 to, f32 cur)
{
    return (cur - from) / (to - from);
}

f32 stepf(f32 edge, f32 x)
{
    return (f32)(x >= edge);
}

f32 smoothf(f32 t)
{
    return t * t * (3.0f - 2.0f * t);
}

f32 smoothstepf(f32 edge0, f32 edge1, f32 x)
{
    return smoothf(clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f));
}

vec2 vec2_lerp(vec2 from, vec2 to, f32 t)
{
    return vec2_make(lerpf(from.x, to.x, t), lerpf(from.y, to.y, t));
}

vec3 vec3_lerp(vec3 from, vec3 to, f32 t)
{
    return vec3_make(lerpf(from.x, to.x, t), lerpf(from.y, to.y, t), lerpf(from.z, to.z, t));
}

#endif /* LERP_IMPLEMENTATION_ */
#endif /* LERP_IMPLEMENTATION */

#ifdef SLERP_IMPLEMENTATION
#ifndef SLERP_IMPLEMENTATION_
#define SLERP_IMPLEMENTATION_

/* acos/sin here still come from libm (real bug found and fixed during
 * this merge: the pre-merge slerp.h called both with no `#include
 * <math.h>` at all - implicit-declaration UB under this project's
 * `-std=c89 -pedantic` build, the same bug class the fabsf/test_voronoi
 * fix already caught elsewhere this session). Not yet hand-rolled to
 * match base.h/easing.h's own "0 glibc" Taylor-series sin/cos - a real,
 * documented gap (acos in particular needs a good approximation valid
 * across the whole [-1,1] domain, more work than a drop-in replacement),
 * not attempted in this pass. */
#include <math.h>

Quat quat_nlerp(Quat a, Quat b, f32 t)
{
    Quat r;
    r.x = a.x + t * (b.x - a.x);
    r.y = a.y + t * (b.y - a.y);
    r.z = a.z + t * (b.z - a.z);
    r.w = a.w + t * (b.w - a.w);
    return quat_normalize(r);
}

Quat quat_slerp(Quat a, Quat b, f32 t)
{
    f32 cos_theta = quat_dot(a, b);
    f32 angle, sin_theta, wa, wb;
    Quat r;

    {
        /* shortest path on the hypersphere: q and -q represent the same
         * rotation, but interpolating toward the "far" one takes the long
         * way around. Branchless sign-select: cos_theta<0 gives sign=-1
         * (negate b and flip cos_theta, same as the original's if-body),
         * cos_theta>=0 gives sign=1 (no-op) - one multiply per component
         * instead of a conditional negate. */
        f32 sign = 1.0f - 2.0f * (f32)(cos_theta < 0.0f);
        b = quat_make(b.x * sign, b.y * sign, b.z * sign, b.w * sign);
        cos_theta *= sign;
    }

    /* NOT branchless, deliberately: this is an algorithm-selection guard,
     * not a value pick between two constants. Near cos_theta=1, sin(angle)
     * below is close to zero - computing the full slerp formula
     * unconditionally would divide by that near-zero value every call,
     * not just the rare near-parallel one, trading a rare (and therefore
     * well-predicted) branch for real precision loss on every slerp call
     * regardless of whether it was ever near-parallel. */
    if (cos_theta > 0.9995f) {
        return quat_nlerp(a, b, t);
    }

    angle = (f32)acos((f64)cos_theta);
    sin_theta = (f32)sin((f64)angle);
    wa = (f32)sin((f64)((1.0f - t) * angle)) / sin_theta;
    wb = (f32)sin((f64)(t * angle)) / sin_theta;

    r.x = wa * a.x + wb * b.x;
    r.y = wa * a.y + wb * b.y;
    r.z = wa * a.z + wb * b.z;
    r.w = wa * a.w + wb * b.w;
    return r;
}

#endif /* SLERP_IMPLEMENTATION_ */
#endif /* SLERP_IMPLEMENTATION */

#ifdef EASING_IMPLEMENTATION
#ifndef EASING_IMPLEMENTATION_
#define EASING_IMPLEMENTATION_

/* Range-reduced Taylor-series sin/cos, replacing math.h's - own local
 * copy (not shared with slerp's own libm calls above - see this
 * project's standing "no cross-IMPLEMENTATION-guard dependency" rule),
 * matching base.h's own quat_sin_/quat_cos_ pattern (0 glibc push,
 * no-glibc rule). */
static f64 easing_mod_2pi_(f64 x)
{
    const f64 two_pi = 6.283185307179586476925286766559;
    const f64 pi = 3.14159265358979323846;
    f64 n = (f64)(i64)(x / two_pi);
    f64 r = x - n * two_pi;
    if (r > pi) r -= two_pi;
    if (r < -pi) r += two_pi;
    return r;
}

static f64 easing_sin_(f64 x)
{
    f64 r = easing_mod_2pi_(x);
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

static f64 easing_cos_(f64 x)
{
    f64 r = easing_mod_2pi_(x);
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

f32 ease_in_quad(f32 t) { return t * t; }
f32 ease_out_quad(f32 t) { return t * (2.0f - t); }
f32 ease_in_out_quad(f32 t)
{
    if (t < 0.5f) return 2.0f * t * t;
    {
        f32 u = -2.0f * t + 2.0f;
        return 1.0f - (u * u) / 2.0f;
    }
}

f32 ease_in_cubic(f32 t) { return t * t * t; }
f32 ease_out_cubic(f32 t)
{
    f32 u = 1.0f - t;
    return 1.0f - u * u * u;
}
f32 ease_in_out_cubic(f32 t)
{
    if (t < 0.5f) return 4.0f * t * t * t;
    {
        f32 u = -2.0f * t + 2.0f;
        return 1.0f - (u * u * u) / 2.0f;
    }
}

f32 ease_in_sine(f32 t) { return 1.0f - (f32)easing_cos_((f64)t * 1.5707963267948966); }
f32 ease_out_sine(f32 t) { return (f32)easing_sin_((f64)t * 1.5707963267948966); }
f32 ease_in_out_sine(f32 t) { return -((f32)easing_cos_(3.14159265358979323846 * (f64)t) - 1.0f) / 2.0f; }

f32 ease_in_back(f32 t)
{
    const f32 c1 = 1.70158f;
    const f32 c3 = c1 + 1.0f;
    return c3 * t * t * t - c1 * t * t;
}

f32 ease_out_back(f32 t)
{
    const f32 c1 = 1.70158f;
    const f32 c3 = c1 + 1.0f;
    f32 u = t - 1.0f;
    return 1.0f + c3 * u * u * u + c1 * u * u;
}

f32 ease_in_out_back(f32 t)
{
    const f32 c1 = 1.70158f;
    const f32 c2 = c1 * 1.525f;

    if (t < 0.5f) {
        f32 u = 2.0f * t;
        return (u * u * ((c2 + 1.0f) * u - c2)) / 2.0f;
    }
    {
        f32 u = 2.0f * t - 2.0f;
        return (u * u * ((c2 + 1.0f) * u + c2) + 2.0f) / 2.0f;
    }
}

#endif /* EASING_IMPLEMENTATION_ */
#endif /* EASING_IMPLEMENTATION */
