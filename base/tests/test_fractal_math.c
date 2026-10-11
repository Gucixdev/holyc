#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define FRACTAL_MATH_IMPLEMENTATION
#include "math/fractal_math.h"

static Fractal_Complex fc(f32 x, f32 y) { Fractal_Complex c; c.x = x; c.y = y; return c; }

/* deterministic [0,1) sequence for the IFS chaos-game test - a simple
 * xorshift-style generator is overkill for "just needs to look
 * uniform enough to visit every transform" - a low-discrepancy
 * additive sequence (golden-ratio increments) does that with zero
 * state beyond a running accumulator. */
static f32 pseudo_random_(f32 *state)
{
    *state += 0.6180339887f;
    if (*state >= 1.0f) *state -= 1.0f;
    return *state;
}

int main(void)
{
    print_cstr("test_fractal_math\n");

    /* Mandelbrot escape times, cross-checked against a plain-Python
     * complex-number reference implementation of the same algorithm. */
    TEST_CHECK(fractal_mandelbrot_escape(fc(0.0f, 0.0f), 50) == 50, "c=0 (the set's own center) never escapes within budget");
    TEST_CHECK(fractal_mandelbrot_escape(fc(-1.0f, 0.0f), 50) == 50, "c=-1 (period-2 bulb center, exactly periodic) never escapes");
    TEST_CHECK(fractal_mandelbrot_escape(fc(5.0f, 5.0f), 50) == 1, "c=5+5i (far outside) escapes almost immediately");
    TEST_CHECK(fractal_mandelbrot_escape(fc(0.3f, 0.5f), 50) == 50, "c=0.3+0.5i (inside the main cardioid) stays bounded");

    /* Julia set: fixed c, varying starting z0 */
    TEST_CHECK(fractal_julia_escape(fc(0.5f, 0.5f), fc(-0.4f, 0.6f), 50) == 3, "Julia escape time matches the Python reference implementation exactly");

    /* IFS chaos game: the classic 3-transform Sierpinski triangle
     * (vertices (0,0),(1,0),(0.5,~0.866), each transform contracts
     * halfway toward one vertex) - real generative use, not just a
     * roundtrip. Every point after the walk should stay inside the
     * triangle's own bounding box, since an affine contraction toward
     * a point inside a convex region can never leave that region. */
    {
        Fractal_Ifs ifs;
        vec2 p;
        f32 rand_state = 0.0f;
        i32 i;
        i32 all_in_bounds = 1;

        ifs.count = 3;
        ifs.transforms[0].a = 0.5f; ifs.transforms[0].b = 0.0f; ifs.transforms[0].c = 0.0f; ifs.transforms[0].d = 0.5f;
        ifs.transforms[0].e = 0.0f; ifs.transforms[0].f = 0.0f; ifs.transforms[0].weight = 1.0f;
        ifs.transforms[1].a = 0.5f; ifs.transforms[1].b = 0.0f; ifs.transforms[1].c = 0.0f; ifs.transforms[1].d = 0.5f;
        ifs.transforms[1].e = 0.5f; ifs.transforms[1].f = 0.0f; ifs.transforms[1].weight = 1.0f;
        ifs.transforms[2].a = 0.5f; ifs.transforms[2].b = 0.0f; ifs.transforms[2].c = 0.0f; ifs.transforms[2].d = 0.5f;
        ifs.transforms[2].e = 0.25f; ifs.transforms[2].f = 0.433f; ifs.transforms[2].weight = 1.0f;

        p.x = 0.0f; p.y = 0.0f;
        for (i = 0; i < 1000; i++) {
            p = fractal_ifs_step(&ifs, p, pseudo_random_(&rand_state));
            if (p.x < -0.01f || p.x > 1.01f || p.y < -0.01f || p.y > 0.87f) all_in_bounds = 0;
        }
        TEST_CHECK(all_in_bounds, "1000 chaos-game steps all stay within the Sierpinski triangle's bounding box");

        /* real distribution check: with all 3 transforms equally
         * weighted, points should visibly spread across the whole
         * triangle, not collapse to one corner - check the x-range
         * actually spans a real fraction of [0,1], not just a sliver. */
        {
            f32 min_x = 2.0f, max_x = -1.0f;
            rand_state = 0.0f;
            p.x = 0.0f; p.y = 0.0f;
            for (i = 0; i < 2000; i++) {
                p = fractal_ifs_step(&ifs, p, pseudo_random_(&rand_state));
                if (i > 20) { /* skip burn-in */
                    if (p.x < min_x) min_x = p.x;
                    if (p.x > max_x) max_x = p.x;
                }
            }
            TEST_CHECK(max_x - min_x > 0.5f, "points spread across most of the attractor's x-range, not collapsed to one region");
        }
    }

    /* box-counting dimension: a fully-filled grid is a solid 2D
     * region - its box-counting dimension should land close to 2.0
     * (count scales as box_size^-2, the definition of a plane). */
    {
        u8 grid[32 * 32];
        i32 i;
        f32 dim;
        for (i = 0; i < 32 * 32; i++) grid[i] = 1;
        dim = fractal_box_count_dimension(grid, 32);
        TEST_CHECK(dim > 1.8f && dim < 2.2f, "a fully-filled grid's box-counting dimension is close to 2.0 (a real 2D region)");
    }

    /* a single occupied cell (dimension 0 - a point doesn't grow with
     * scale) */
    {
        u8 grid[32 * 32];
        i32 i;
        f32 dim;
        for (i = 0; i < 32 * 32; i++) grid[i] = 0;
        grid[0] = 1;
        dim = fractal_box_count_dimension(grid, 32);
        TEST_CHECK(dim > -0.3f && dim < 0.3f, "a single occupied cell's box-counting dimension is close to 0.0 (a point)");
    }

    print_flush();
    return test_failures_ != 0;
}
