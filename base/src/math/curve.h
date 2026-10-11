#ifndef HOLYC_CURVE_H_
#define HOLYC_CURVE_H_

/* Curve/interpolation primitives, one file, per the single-file convention - previously split into src/math/curve/{bezier,
 * catmull,hermite,bspline,morton}.h, merged back here. Each algorithm below
 * still stands alone under its OWN IMPLEMENTATION guard macro (BEZIER_/
 * CATMULL_/HERMITE_/BSPLINE_/MORTON_IMPLEMENTATION, unchanged from the
 * split files) - a caller/test wanting only one algorithm still only pulls
 * in that one's code, this file just puts them in one place on disk. */

#include "types/base.h"
#include "types/base.h"
#include "math/mathx.h"

/* ---- bezier ----
 * Cubic Bezier evaluation - the scalar formula ported from cglm's
 * include/cglm/bezier.h, the only one of the checked reference libraries
 * (HandmadeMath, linmath.h) that has one at all.
 *
 * Textbook Bernstein form for 4 control points p0 (start), c0, c1
 * (the two control handles), p1 (end):
 *
 *   B(s) = p0*(1-s)^3 + 3*c0*s*(1-s)^2 + 3*c1*s^2*(1-s) + p1*s^3
 *
 * cglm's version below is the same curve, restructured to share
 * subexpressions instead of computing (1-s)^3, s^2*(1-s), etc separately -
 * fewer total multiplies for the identical result. */

f32 bezierf(f32 p0, f32 c0, f32 c1, f32 p1, f32 s);
vec2 vec2_bezier(vec2 p0, vec2 c0, vec2 c1, vec2 p1, f32 s);
vec3 vec3_bezier(vec3 p0, vec3 c0, vec3 c1, vec3 p1, f32 s);

/* ---- catmull ----
 * Catmull-Rom spline (uniform parametrization). None of the checked
 * reference libraries (cglm, HandmadeMath, linmath.h) implement this -
 * cglm's bezier.h ships a Hermite basis matrix (GLM_HERMITE_MAT) that
 * Catmull-Rom could in principle be built on top of (uniform Catmull-Rom
 * is Hermite interpolation between p1,p2 with tangents (p2-p0)/2 and
 * (p3-p1)/2), but there's no generic "evaluate via basis matrix" utility
 * in this file to plug it into, so this is the direct textbook polynomial
 * form instead, standard and not specific to any library:
 *
 *   p(t) = 0.5 * ( 2*p1
 *                  + (-p0 + p2)*t
 *                  + (2*p0 - 5*p1 + 4*p2 - p3)*t^2
 *                  + (-p0 + 3*p1 - 3*p2 + p3)*t^3 )
 *
 * Interpolates BETWEEN p1 and p2 (t in [0,1]); p0 and p3 are the
 * neighbouring control points that shape the tangents at the endpoints,
 * they are not themselves passed through by the curve. */

f32 catmullf(f32 p0, f32 p1, f32 p2, f32 p3, f32 t);
vec2 vec2_catmull(vec2 p0, vec2 p1, vec2 p2, vec2 p3, f32 t);
vec3 vec3_catmull(vec3 p0, vec3 p1, vec3 p2, vec3 p3, f32 t);

/* ---- hermite ----
 * Cubic Hermite interpolation - the standard textbook basis-function
 * form, same "direct polynomial form, not a library port" stance catmull
 * above takes. Interpolates between p0 (at t=0) and p1 (at t=1), with
 * m0/m1 the TANGENT (velocity, not a control point) at each endpoint -
 * this is the real difference from bezier's 4-control-points-you-can-see
 * form. Uniform Catmull-Rom above is a special case of this with
 * m0=(p2-p0)/2, m1=(p3-p1)/2 for consecutive points p0,p1,p2,p3, but kept
 * as its own separate direct-form implementation rather than expressed in
 * terms of this one.
 *
 * Basis functions (h00/h10/h01/h11, Wikipedia's own standard naming):
 *   h00(t) =  2t^3 - 3t^2 + 1
 *   h10(t) =    t^3 - 2t^2 + t
 *   h01(t) = -2t^3 + 3t^2
 *   h11(t) =    t^3 -  t^2
 *   p(t) = h00*p0 + h10*m0 + h01*p1 + h11*m1 */

f32 hermitef(f32 p0, f32 m0, f32 p1, f32 m1, f32 t);
vec2 vec2_hermite(vec2 p0, vec2 m0, vec2 p1, vec2 m1, f32 t);
vec3 vec3_hermite(vec3 p0, vec3 m0, vec3 p1, vec3 m1, f32 t);

/* ---- bspline ----
 * Uniform cubic B-spline - the standard textbook basis-function form,
 * same "direct polynomial form, not a library port" stance as above.
 *
 * Genuinely different from bezier/catmull/hermite, not a redundant
 * fourth way to write the same curve: this is an APPROXIMATING basis
 * (the curve generally does NOT pass through p0, p1, p2, or p3
 * themselves, only near them - test_curve.c checks this is really true,
 * not accidentally reimplementing an interpolating basis), unlike
 * bezier/catmull/hermite which all pass exactly through their defining
 * endpoints. Interpolates a segment that lies roughly "between" p1 and
 * p2, shaped by neighbors p0/p3 - the same 4-consecutive-control-points
 * shape catmull uses, but a smoother (C2-continuous, vs Catmull-Rom's C1)
 * result at segment boundaries, at the cost of not touching the control
 * points exactly.
 *
 * Basis functions (the standard uniform cubic B-spline blending weights,
 * e.g. as given in any computer-graphics curves reference):
 *   b0(t) = (1-t)^3 / 6
 *   b1(t) = (3t^3 - 6t^2 + 4) / 6
 *   b2(t) = (-3t^3 + 3t^2 + 3t + 1) / 6
 *   b3(t) = t^3 / 6
 *   p(t) = b0*p0 + b1*p1 + b2*p2 + b3*p3
 * (b0+b1+b2+b3 == 1 for all t - partition of unity, checked directly in
 * test_curve.c rather than just asserted here). */

f32 bsplinef(f32 p0, f32 p1, f32 p2, f32 p3, f32 t);
vec2 vec2_bspline(vec2 p0, vec2 p1, vec2 p2, vec2 p3, f32 t);
vec3 vec3_bspline(vec3 p0, vec3 p1, vec3 p2, vec3 p3, f32 t);

/* ---- morton ----
 * Morton (Z-order) curve encode/decode - interleaves the bits of 2 or 3
 * integer coordinates into one code such that spatial locality is mostly
 * preserved (two cells that are near each other in space usually end up
 * with nearby codes), which is the whole point: it turns a 2D/3D grid
 * key into a single integer usable as a sort key or a flat array index
 * while keeping neighboring cells close together in that ordering (unlike
 * a plain row-major index, where two vertically adjacent cells can be an
 * entire row-width apart).
 *
 * 2D: each of x,y is a 16-bit coordinate, interleaved into a 32-bit code
 * (16 bits per axis, all 32 bits of the code used).
 * 3D: each of x,y,z is a 21-bit coordinate (the most that fits 3 into 64
 * bits: 3*21 = 63 <= 64), interleaved into a 64-bit code. Only the low 21
 * bits of each input are used - callers with wider coordinates need to
 * rescale first, same contract bit_extract32/64 already use for
 * out-of-range width arguments (not checked here, caller's responsibility).
 *
 * Uses the classic "spread bits by magic-number AND/shift" technique
 * (the well-known standard technique for this, re-derived here, not
 * copied from anywhere) - O(log n) shifts per axis instead of a per-bit
 * loop; the bit-by-bit approach only exists in bench_morton.c as a
 * deliberately-naive comparison baseline, not as part of the real API.
 *
 * 64-bit hex constants below use the `UL` suffix, not `ULL` - this
 * codebase is C89 (`long long` doesn't exist), and u64 is typedef'd from
 * __UINT64_TYPE__, which is `unsigned long` on this project's actual
 * target (matches rng.h's own "long is 64-bit here" convention). */

u32 morton_encode_2d(u16 x, u16 y);
void morton_decode_2d(u32 code, u16 *x, u16 *y);

u64 morton_encode_3d(u32 x, u32 y, u32 z); /* only the low 21 bits of each input are used */
void morton_decode_3d(u64 code, u32 *x, u32 *y, u32 *z);

#endif /* HOLYC_CURVE_H_ */

#ifdef BEZIER_IMPLEMENTATION
#ifndef BEZIER_IMPLEMENTATION_
#define BEZIER_IMPLEMENTATION_

f32 bezierf(f32 p0, f32 c0, f32 c1, f32 p1, f32 s)
{
    f32 x = 1.0f - s;
    f32 xx = x * x;
    f32 ss = s * s;
    f32 xs3 = (s - ss) * 3.0f;
    f32 a = p0 * xx + c0 * xs3;
    return a + s * (c1 * xs3 + p1 * ss - a);
}

vec2 vec2_bezier(vec2 p0, vec2 c0, vec2 c1, vec2 p1, f32 s)
{
    return vec2_make(
        bezierf(p0.x, c0.x, c1.x, p1.x, s),
        bezierf(p0.y, c0.y, c1.y, p1.y, s)
    );
}

vec3 vec3_bezier(vec3 p0, vec3 c0, vec3 c1, vec3 p1, f32 s)
{
    return vec3_make(
        bezierf(p0.x, c0.x, c1.x, p1.x, s),
        bezierf(p0.y, c0.y, c1.y, p1.y, s),
        bezierf(p0.z, c0.z, c1.z, p1.z, s)
    );
}

#endif /* BEZIER_IMPLEMENTATION_ */
#endif /* BEZIER_IMPLEMENTATION */

#ifdef CATMULL_IMPLEMENTATION
#ifndef CATMULL_IMPLEMENTATION_
#define CATMULL_IMPLEMENTATION_

f32 catmullf(f32 p0, f32 p1, f32 p2, f32 p3, f32 t)
{
    f32 tt = t * t;
    f32 ttt = tt * t;
    return 0.5f * (
        2.0f * p1 +
        (-p0 + p2) * t +
        (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * tt +
        (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * ttt
    );
}

vec2 vec2_catmull(vec2 p0, vec2 p1, vec2 p2, vec2 p3, f32 t)
{
    return vec2_make(
        catmullf(p0.x, p1.x, p2.x, p3.x, t),
        catmullf(p0.y, p1.y, p2.y, p3.y, t)
    );
}

vec3 vec3_catmull(vec3 p0, vec3 p1, vec3 p2, vec3 p3, f32 t)
{
    return vec3_make(
        catmullf(p0.x, p1.x, p2.x, p3.x, t),
        catmullf(p0.y, p1.y, p2.y, p3.y, t),
        catmullf(p0.z, p1.z, p2.z, p3.z, t)
    );
}

#endif /* CATMULL_IMPLEMENTATION_ */
#endif /* CATMULL_IMPLEMENTATION */

#ifdef HERMITE_IMPLEMENTATION
#ifndef HERMITE_IMPLEMENTATION_
#define HERMITE_IMPLEMENTATION_

f32 hermitef(f32 p0, f32 m0, f32 p1, f32 m1, f32 t)
{
    f32 tt = t * t;
    f32 ttt = tt * t;
    f32 h00 = 2.0f * ttt - 3.0f * tt + 1.0f;
    f32 h10 = ttt - 2.0f * tt + t;
    f32 h01 = -2.0f * ttt + 3.0f * tt;
    f32 h11 = ttt - tt;
    return h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
}

vec2 vec2_hermite(vec2 p0, vec2 m0, vec2 p1, vec2 m1, f32 t)
{
    return vec2_make(
        hermitef(p0.x, m0.x, p1.x, m1.x, t),
        hermitef(p0.y, m0.y, p1.y, m1.y, t)
    );
}

vec3 vec3_hermite(vec3 p0, vec3 m0, vec3 p1, vec3 m1, f32 t)
{
    return vec3_make(
        hermitef(p0.x, m0.x, p1.x, m1.x, t),
        hermitef(p0.y, m0.y, p1.y, m1.y, t),
        hermitef(p0.z, m0.z, p1.z, m1.z, t)
    );
}

#endif /* HERMITE_IMPLEMENTATION_ */
#endif /* HERMITE_IMPLEMENTATION */

#ifdef BSPLINE_IMPLEMENTATION
#ifndef BSPLINE_IMPLEMENTATION_
#define BSPLINE_IMPLEMENTATION_

f32 bsplinef(f32 p0, f32 p1, f32 p2, f32 p3, f32 t)
{
    f32 tt = t * t;
    f32 ttt = tt * t;
    f32 one_minus_t = 1.0f - t;
    f32 b0 = (one_minus_t * one_minus_t * one_minus_t) / 6.0f;
    f32 b1 = (3.0f * ttt - 6.0f * tt + 4.0f) / 6.0f;
    f32 b2 = (-3.0f * ttt + 3.0f * tt + 3.0f * t + 1.0f) / 6.0f;
    f32 b3 = ttt / 6.0f;
    return b0 * p0 + b1 * p1 + b2 * p2 + b3 * p3;
}

vec2 vec2_bspline(vec2 p0, vec2 p1, vec2 p2, vec2 p3, f32 t)
{
    return vec2_make(
        bsplinef(p0.x, p1.x, p2.x, p3.x, t),
        bsplinef(p0.y, p1.y, p2.y, p3.y, t)
    );
}

vec3 vec3_bspline(vec3 p0, vec3 p1, vec3 p2, vec3 p3, f32 t)
{
    return vec3_make(
        bsplinef(p0.x, p1.x, p2.x, p3.x, t),
        bsplinef(p0.y, p1.y, p2.y, p3.y, t),
        bsplinef(p0.z, p1.z, p2.z, p3.z, t)
    );
}

#endif /* BSPLINE_IMPLEMENTATION_ */
#endif /* BSPLINE_IMPLEMENTATION */

#ifdef MORTON_IMPLEMENTATION
#ifndef MORTON_IMPLEMENTATION_
#define MORTON_IMPLEMENTATION_

/* spread a 16-bit value's bits out to every other bit of a 32-bit word
 * (bit i moves to bit 2*i), so two spread values can be OR'd together
 * (one of them pre-shifted left by 1) without their bits ever colliding. */
static u32 morton_spread2_32_(u16 v)
{
    u32 x = v;
    x = (x | (x << 8)) & 0x00ff00ffu;
    x = (x | (x << 4)) & 0x0f0f0f0fu;
    x = (x | (x << 2)) & 0x33333333u;
    x = (x | (x << 1)) & 0x55555555u;
    return x;
}

/* inverse of morton_spread2_32_: gather every other bit (bit 2*i) back
 * down into a dense low run (bit i). */
static u16 morton_compact2_32_(u32 x)
{
    x &= 0x55555555u;
    x = (x | (x >> 1)) & 0x33333333u;
    x = (x | (x >> 2)) & 0x0f0f0f0fu;
    x = (x | (x >> 4)) & 0x00ff00ffu;
    x = (x | (x >> 8)) & 0x0000ffffu;
    return (u16)x;
}

/* same idea as morton_spread2_32_, but spreading a 21-bit value's bits
 * out to every 3rd bit of a 64-bit word (bit i moves to bit 3*i), for
 * the 3D case. */
static u64 morton_spread3_64_(u32 v)
{
    u64 x = v & 0x1fffffu; /* keep only the low 21 bits */
    x = (x | (x << 32)) & 0x1f00000000ffffUL;
    x = (x | (x << 16)) & 0x1f0000ff0000ffUL;
    x = (x | (x << 8))  & 0x100f00f00f00f00fUL;
    x = (x | (x << 4))  & 0x10c30c30c30c30c3UL;
    x = (x | (x << 2))  & 0x1249249249249249UL;
    return x;
}

static u32 morton_compact3_64_(u64 x)
{
    x &= 0x1249249249249249UL;
    x = (x | (x >> 2))  & 0x10c30c30c30c30c3UL;
    x = (x | (x >> 4))  & 0x100f00f00f00f00fUL;
    x = (x | (x >> 8))  & 0x1f0000ff0000ffUL;
    x = (x | (x >> 16)) & 0x1f00000000ffffUL;
    x = (x | (x >> 32)) & 0x1fffffUL;
    return (u32)x;
}

u32 morton_encode_2d(u16 x, u16 y)
{
    return morton_spread2_32_(x) | (morton_spread2_32_(y) << 1);
}

void morton_decode_2d(u32 code, u16 *x, u16 *y)
{
    *x = morton_compact2_32_(code);
    *y = morton_compact2_32_(code >> 1);
}

u64 morton_encode_3d(u32 x, u32 y, u32 z)
{
    return morton_spread3_64_(x) | (morton_spread3_64_(y) << 1) | (morton_spread3_64_(z) << 2);
}

void morton_decode_3d(u64 code, u32 *x, u32 *y, u32 *z)
{
    *x = morton_compact3_64_(code);
    *y = morton_compact3_64_(code >> 1);
    *z = morton_compact3_64_(code >> 2);
}

#endif /* MORTON_IMPLEMENTATION_ */
#endif /* MORTON_IMPLEMENTATION */
