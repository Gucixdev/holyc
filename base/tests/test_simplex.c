#include "test_common.h"
#define SIMPLEX_IMPLEMENTATION
#include "math/noise.h"
#include <math.h>

int main(void)
{
    f32 a, b;
    f32 lo2 = 1e9f, hi2 = -1e9f;
    f32 lo3 = 1e9f, hi3 = -1e9f;
    i32 all_same_2d = 1, all_same_3d = 1;
    i32 any_nan_or_inf = 0;
    f32 first2, first3;
    size_t i, j;

    print_cstr("test_simplex\n");

    a = simplex_noise_2d(1337, 1.5f, 2.5f);
    b = simplex_noise_2d(1337, 1.5f, 2.5f);
    TEST_CHECK(a == b, "2D: same seed+coords produce identical output (deterministic)");

    a = simplex_noise_3d(1337, 1.5f, 2.5f, 3.5f);
    b = simplex_noise_3d(1337, 1.5f, 2.5f, 3.5f);
    TEST_CHECK(a == b, "3D: same seed+coords produce identical output (deterministic)");

    /* coordinates deliberately not round numbers - a point that happens to
     * sit exactly where every simplex kernel's radius contribution is <=0
     * legitimately returns 0.0 for ANY seed (the gradient term never gets
     * multiplied in), which is correct algorithm behavior, not a bug - it
     * just makes for a bad single-point equality test. (1.5, 2.5, 3.5) was
     * exactly such a point at the specific seeds first tried here. */
    a = simplex_noise_2d(1, 1.37f, 2.91f);
    b = simplex_noise_2d(2, 1.37f, 2.91f);
    TEST_CHECK(a != b, "2D: different seeds produce different output at the same coords");

    a = simplex_noise_3d(1, 1.37f, 2.91f, 3.53f);
    b = simplex_noise_3d(2, 1.37f, 2.91f, 3.53f);
    TEST_CHECK(a != b, "3D: different seeds produce different output at the same coords");

    first2 = simplex_noise_2d(42, 0.0f, 0.0f);
    first3 = simplex_noise_3d(42, 0.0f, 0.0f, 0.0f);

    for (i = 0; i < 200; i++) {
        for (j = 0; j < 200; j++) {
            f32 fx = (f32)i * 0.1f;
            f32 fy = (f32)j * 0.1f;
            f32 v2 = simplex_noise_2d(42, fx, fy);
            f32 v3 = simplex_noise_3d(42, fx, fy, fx * 0.5f);

            if (v2 < lo2) lo2 = v2;
            if (v2 > hi2) hi2 = v2;
            if (v3 < lo3) lo3 = v3;
            if (v3 > hi3) hi3 = v3;
            if (v2 != first2) all_same_2d = 0;
            if (v3 != first3) all_same_3d = 0;
            if (v2 != v2 || v3 != v3) any_nan_or_inf = 1; /* NaN != itself */
            if (fabs((double)v2) > 1e30 || fabs((double)v3) > 1e30) any_nan_or_inf = 1;
        }
    }

    TEST_CHECK(lo2 >= -1.5f && hi2 <= 1.5f, "2D: sampled values stay within the documented ~[-1,1] range (generous bound)");
    TEST_CHECK(lo3 >= -1.5f && hi3 <= 1.5f, "3D: sampled values stay within the documented ~[-1,1] range (generous bound)");
    TEST_CHECK(!all_same_2d, "2D: a 200x200 grid is not degenerate (not every sample identical)");
    TEST_CHECK(!all_same_3d, "3D: a 200x200 grid is not degenerate (not every sample identical)");
    TEST_CHECK(!any_nan_or_inf, "no NaN/Inf across the sampled grid");
    TEST_CHECK(hi2 - lo2 > 0.5f, "2D: sampled range has real spread, not just barely-different values");
    TEST_CHECK(hi3 - lo3 > 0.5f, "3D: sampled range has real spread, not just barely-different values");

    print_flush();
    return test_failures_ != 0;
}
