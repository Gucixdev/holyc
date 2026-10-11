#ifndef HOLYC_FRACTAL_MATH_H_
#define HOLYC_FRACTAL_MATH_H_

/* Classic fractal-generation primitives, distinct from noise.h's fBm
 * (that's fractal-layered NOISE for terrain/texture; this is the
 * escape-time / iterated-function-system / box-counting family):
 *
 *   fractal_mandelbrot_escape / fractal_julia_escape - escape-time
 *   iteration for the two best-known escape-time fractals.
 *
 *   fractal_ifs_step - one chaos-game step of an Iterated Function
 *   System (a weighted set of affine transforms, one picked per step
 *   by a caller-supplied random draw) - the Sierpinski triangle,
 *   Barnsley fern, etc. are all just a specific transform set fed
 *   through this same primitive.
 *
 *   fractal_box_count_dimension - box-counting dimension estimate over
 *   a 2D boolean occupancy grid (real fractals scale their box count
 *   as a power law in box size; this fits that power law via a linear
 *   regression on the log-log points and returns the slope). */

#include "base.h"
#include "mathx.h"
#include <math.h> /* log - trigonometry.h's own precedent (see its header comment): call libm directly */

typedef struct {
    f32 x, y; /* complex plane: x = real, y = imaginary */
} Fractal_Complex;

/* iterates z = z^2 + c starting from z=(0,0), c = the point being
 * tested - the standard Mandelbrot definition. Returns the iteration
 * count at which |z| first exceeds 2 (the standard escape radius,
 * proven sufficient: once |z|>2 it provably diverges), or max_iter if
 * it never escapes within that budget (a strong hint - not a proof -
 * that c is in the set). */
i32 fractal_mandelbrot_escape(Fractal_Complex c, i32 max_iter);

/* same escape-time iteration, but z starts at the given point and c is
 * a FIXED constant for the whole image (the Julia set definition -
 * one fixed c, iterate every starting z0). */
i32 fractal_julia_escape(Fractal_Complex z0, Fractal_Complex c, i32 max_iter);

typedef struct {
    f32 a, b, c, d, e, f; /* affine: x' = a*x + b*y + e, y' = c*x + d*y + f */
    f32 weight;           /* relative selection probability - NOT required to sum to 1, see fractal_ifs_step */
} Fractal_Ifs_Transform;

#define FRACTAL_IFS_MAX_TRANSFORMS 16

typedef struct {
    Fractal_Ifs_Transform transforms[FRACTAL_IFS_MAX_TRANSFORMS];
    i32 count;
} Fractal_Ifs;

/* one chaos-game step: picks a transform weighted by `weight` (using
 * random_draw, a caller-supplied value in [0,1) - this file has no
 * opinion on which RNG produces it) and applies it to `point`,
 * returning the new point. Calling this repeatedly from any starting
 * point converges onto the IFS's attractor (the fractal shape itself)
 * after a short burn-in - same algorithm real Sierpinski-triangle/
 * Barnsley-fern generators use. */
vec2 fractal_ifs_step(const Fractal_Ifs *ifs, vec2 point, f32 random_draw);

/* box-counting dimension over a `size`x`size` boolean occupancy grid
 * (occupied[y*size+x] != 0 means that cell is part of the shape).
 * Counts occupied boxes at box sizes 1,2,4,8,... up to size/2, fits
 * log(count) vs log(1/box_size) with a least-squares line, and returns
 * the slope - the standard box-counting fractal dimension estimate
 * (a filled square converges to ~2.0, a single point to ~0.0, a real
 * fractal boundary like a Sierpinski triangle to its known ~1.585). */
f32 fractal_box_count_dimension(const u8 *occupied, i32 size);

#endif /* HOLYC_FRACTAL_MATH_H_ */

#ifdef FRACTAL_MATH_IMPLEMENTATION
#ifndef FRACTAL_MATH_IMPLEMENTATION_
#define FRACTAL_MATH_IMPLEMENTATION_

i32 fractal_mandelbrot_escape(Fractal_Complex c, i32 max_iter)
{
    Fractal_Complex z0;
    z0.x = 0.0f; z0.y = 0.0f;
    return fractal_julia_escape(z0, c, max_iter);
}

i32 fractal_julia_escape(Fractal_Complex z0, Fractal_Complex c, i32 max_iter)
{
    f32 zx = z0.x, zy = z0.y;
    i32 i;
    for (i = 0; i < max_iter; i++) {
        f32 zx2 = zx * zx, zy2 = zy * zy;
        if (zx2 + zy2 > 4.0f) return i; /* |z|^2 > 4 == |z| > 2 */
        {
            f32 new_zy = 2.0f * zx * zy + c.y;
            zx = zx2 - zy2 + c.x;
            zy = new_zy;
        }
    }
    return max_iter;
}

vec2 fractal_ifs_step(const Fractal_Ifs *ifs, vec2 point, f32 random_draw)
{
    f32 total_weight = 0.0f;
    f32 target;
    f32 running = 0.0f;
    i32 i;
    i32 chosen = ifs->count - 1; /* fallback: last transform, covers float rounding at the top of the range */

    for (i = 0; i < ifs->count; i++) total_weight += ifs->transforms[i].weight;
    target = random_draw * total_weight;

    for (i = 0; i < ifs->count; i++) {
        running += ifs->transforms[i].weight;
        if (target < running) { chosen = i; break; }
    }

    {
        const Fractal_Ifs_Transform *t = &ifs->transforms[chosen];
        vec2 out;
        out.x = t->a * point.x + t->b * point.y + t->e;
        out.y = t->c * point.x + t->d * point.y + t->f;
        return out;
    }
}

f32 fractal_box_count_dimension(const u8 *occupied, i32 size)
{
    f32 log_inv_box_size[16];
    f32 log_count[16];
    i32 n_scales = 0;
    i32 box_size;

    for (box_size = 1; box_size <= size / 2 && n_scales < 16; box_size *= 2) {
        i32 bx, by;
        i32 count = 0;
        for (by = 0; by < size; by += box_size) {
            for (bx = 0; bx < size; bx += box_size) {
                i32 x, y;
                i32 hit = 0;
                for (y = by; y < by + box_size && y < size && !hit; y++) {
                    for (x = bx; x < bx + box_size && x < size; x++) {
                        if (occupied[y * size + x]) { hit = 1; break; }
                    }
                }
                if (hit) count++;
            }
        }
        if (count > 0) {
            /* natural log via log2-free approach: use the identity
             * ln(x) = log2(x) * ln(2) would need log2 too - simplest
             * to just use the standard library's log here (matches
             * easing.h's own precedent of calling libm directly for
             * transcendental functions this project doesn't hand-roll). */
            log_count[n_scales] = (f32)log((f64)count);
            log_inv_box_size[n_scales] = (f32)log(1.0 / (f64)box_size);
            n_scales++;
        }
    }

    if (n_scales < 2) return 0.0f;

    /* least-squares slope of log_count vs log_inv_box_size - the
     * box-counting dimension is exactly that slope. */
    {
        f32 sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f, sum_xx = 0.0f;
        i32 i;
        f32 n = (f32)n_scales;
        f32 denom;
        for (i = 0; i < n_scales; i++) {
            sum_x += log_inv_box_size[i];
            sum_y += log_count[i];
            sum_xy += log_inv_box_size[i] * log_count[i];
            sum_xx += log_inv_box_size[i] * log_inv_box_size[i];
        }
        denom = n * sum_xx - sum_x * sum_x;
        if (denom == 0.0f) return 0.0f;
        return (n * sum_xy - sum_x * sum_y) / denom;
    }
}

#endif /* FRACTAL_MATH_IMPLEMENTATION_ */
#endif /* FRACTAL_MATH_IMPLEMENTATION */
