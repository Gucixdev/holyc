#ifndef HOLYC_GRADIENT_H_
#define HOLYC_GRADIENT_H_

/* Classic Perlin-style 2D gradient noise - a DIFFERENT algorithm family
 * from simplex.h (OpenSimplex2), not a redundant duplicate: grid-based
 * (a square lattice, not simplex.h's skewed-simplex lattice), with its
 * own different visual character (simplex noise has no directional grid
 * artifacts at the cost of a more complex per-sample kernel; classic
 * gradient noise is simpler per-sample but can show faint axis-aligned
 * bias) - a legitimate second primitive to have, kept as its own file.
 * simplex.h's own gradient table is `static` (private, internal), so
 * this isn't reaching into or duplicating that file's data at all - a
 * fresh, independent hash-to-gradient scheme below.
 *
 * Algorithm (Ken Perlin's improved 2002 noise, the standard reference
 * shape): hash each of the 4 surrounding lattice corners to one of 8
 * unit gradient directions (0/45/90/.../315 degrees), dot each corner's
 * gradient with the offset vector from that corner to the sample point,
 * then bilinearly blend the 4 dot products using the QUINTIC fade curve
 * `6t^5 - 15t^4 + 10t^3` (the improved version - NOT the original 1985
 * cubic `3t^2 - 2t^3`, which has a discontinuous second derivative at
 * cell boundaries; the quintic curve's first AND second derivatives are
 * both exactly 0 at t=0 and t=1).
 *
 * Defining property, exploited directly by the math rather than special-
 * cased: at any exact integer lattice coordinate, the sample's local
 * offset from ITS OWN containing cell's corner is always (0,0) - dotting
 * any gradient with a zero vector is 0 regardless of direction, and
 * fade(0)==0 exactly means the bilinear blend resolves to exactly that
 * corner's own (zero) contribution - so the noise value at every integer
 * lattice point is exactly 0.0, verified directly in
 * tests/test_gradient.c, not just asserted here. */

#include "core/base.h"

f32 gradient_noise_2d(i32 seed, f32 x, f32 y);

#endif /* HOLYC_GRADIENT_H_ */

#ifdef GRADIENT_IMPLEMENTATION
#ifndef GRADIENT_IMPLEMENTATION_
#define GRADIENT_IMPLEMENTATION_

/* floor-to-i32, replacing math.h's floor() - own local copy, same
 * cast-and-adjust technique noise.h's simplex_floor_ already uses
 * (0 glibc push, no-glibc rule). */
static i32 gradient_floor_i32_(f32 f)
{
    i32 fi = (i32)f;
    return fi - (i32)(f < (f32)fi);
}

/* integer hash of (seed, ix, iy) -> a well-mixed u32, independent of
 * simplex.h's own private hash pipeline (this file's own scheme, not a
 * shared/reused one - see file header). Murmur-style avalanche finalizer
 * after two prime-multiply XOR folds. */
static u32 gradient_hash_(i32 seed, i32 ix, i32 iy)
{
    u32 h = (u32)seed;
    h ^= (u32)ix * 0x27d4eb2du;
    h ^= (u32)iy * 0x165667b1u;
    h = (h ^ (h >> 15)) * 0x85ebca6bu;
    h = (h ^ (h >> 13)) * 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

/* the 8 unit gradient directions (0/45/.../315 degrees) as constant
 * pairs - a fixed, small gradient set (same spirit as Perlin's own
 * classic 12-direction table, just axis/diagonal-only here), selected
 * by the low 3 bits of the hash. 1/sqrt(2) for the 4 diagonals. */
static f32 gradient_dot_(i32 seed, i32 ix, i32 iy, f32 dx, f32 dy)
{
    u32 h = gradient_hash_(seed, ix, iy) & 7u;
    const f32 inv_sqrt2 = 0.70710678118654752440f;

    switch (h) {
    case 0: return dx;
    case 1: return (dx + dy) * inv_sqrt2;
    case 2: return dy;
    case 3: return (-dx + dy) * inv_sqrt2;
    case 4: return -dx;
    case 5: return (-dx - dy) * inv_sqrt2;
    case 6: return -dy;
    case 7: return (dx - dy) * inv_sqrt2;
    default: break; /* unreachable: h is masked to 0..7 above */
    }
    return 0.0f;
}

static f32 gradient_fade_(f32 t)
{
    /* 6t^5 - 15t^4 + 10t^3, Horner-factored: t^3 * (t*(t*6 - 15) + 10) */
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

static f32 gradient_lerp_(f32 a, f32 b, f32 t)
{
    return a + t * (b - a);
}

f32 gradient_noise_2d(i32 seed, f32 x, f32 y)
{
    i32 x0 = gradient_floor_i32_(x);
    i32 y0 = gradient_floor_i32_(y);
    i32 x1 = x0 + 1;
    i32 y1 = y0 + 1;

    f32 sx = x - (f32)x0;
    f32 sy = y - (f32)y0;

    f32 n00 = gradient_dot_(seed, x0, y0, sx, sy);
    f32 n10 = gradient_dot_(seed, x1, y0, sx - 1.0f, sy);
    f32 n01 = gradient_dot_(seed, x0, y1, sx, sy - 1.0f);
    f32 n11 = gradient_dot_(seed, x1, y1, sx - 1.0f, sy - 1.0f);

    f32 u = gradient_fade_(sx);
    f32 v = gradient_fade_(sy);

    f32 nx0 = gradient_lerp_(n00, n10, u);
    f32 nx1 = gradient_lerp_(n01, n11, u);

    return gradient_lerp_(nx0, nx1, v);
}

#endif /* GRADIENT_IMPLEMENTATION_ */
#endif /* GRADIENT_IMPLEMENTATION */
