#include "test_common.h"
#define GRADIENT_IMPLEMENTATION
#include "math/gradient.h"
#include <math.h>

int main(void)
{
    print_cstr("test_gradient\n");

    /* THE defining property of classic Perlin gradient noise: at an exact
       integer lattice point, the offset vector from that corner to the
       sample point is (0,0), so the gradient-dot-offset contribution from
       THAT corner is exactly 0 - and since the fade/interpolation weights
       for a point sitting exactly on a lattice point resolve to 1.0 at
       that corner and 0.0 at the other three, the total noise value at
       any integer lattice coordinate is exactly 0.0. This is a real,
       checkable correctness property, not a style preference - if this
       fails, the gradient/interpolation math has a real bug. */
    {
        i32 ok = 1;
        i32 x, y;
        for (x = -5; x <= 5; x++) {
            for (y = -5; y <= 5; y++) {
                f32 v = gradient_noise_2d(1337, (f32)x, (f32)y);
                if (v != 0.0f) ok = 0;
            }
        }
        TEST_CHECK(ok, "gradient_noise_2d is exactly 0.0 at every integer lattice point (defining property)");
    }

    /* determinism: same seed+coords produce identical output */
    {
        f32 a = gradient_noise_2d(42, 1.5f, 2.5f);
        f32 b = gradient_noise_2d(42, 1.5f, 2.5f);
        TEST_CHECK(a == b, "same seed+coords produce identical output (deterministic)");
    }

    /* different seeds produce different output for non-lattice coordinates
       (the seed actually perturbs the gradient hash, not a no-op) */
    {
        f32 a = gradient_noise_2d(1, 1.5f, 2.5f);
        f32 b = gradient_noise_2d(2, 1.5f, 2.5f);
        TEST_CHECK(a != b, "different seeds produce different output at the same non-lattice coordinates");
    }

    /* output stays in a sane bounded range and finite across many points -
       classic 2D Perlin noise is bounded within roughly [-1, 1] (in
       practice usually well inside that for the standard gradient set) */
    {
        i32 ok = 1;
        i32 i;
        for (i = 0; i < 200; i++) {
            f32 x = (f32)i * 0.137f;
            f32 y = (f32)i * 0.593f;
            f32 v = gradient_noise_2d(7, x, y);
            if (v != v) ok = 0; /* NaN check */
            if (v > 1.01f || v < -1.01f) ok = 0;
        }
        TEST_CHECK(ok, "gradient_noise_2d output stays finite and within [-1,1] across many sample points");
    }

    /* not a constant function - real variation across space. Points
       deliberately NOT on a cell's diagonal: (0.25,0.25) vs (0.75,0.75)
       was tried first and is a real, caught test-design bug - those two
       points sit on the diagonal of the same unit cell, and for seed=7
       the (1,0) and (0,1) corners happen to hash to the SAME gradient
       index, making that specific cell symmetric about its diagonal
       (verified by hand: both points genuinely evaluate to the same
       value, not a rounding coincidence) - an accidental structural
       symmetry in the test's own chosen points, not an algorithm bug.
       (0.2,0.6) vs (0.8,0.3) breaks that symmetry. */
    {
        f32 a = gradient_noise_2d(7, 0.2f, 0.6f);
        f32 b = gradient_noise_2d(7, 0.8f, 0.3f);
        TEST_CHECK(a != b, "gradient_noise_2d varies across different non-lattice coordinates (not a constant)");
    }

    print_flush();
    return test_failures_ != 0;
}
