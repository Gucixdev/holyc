#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define BSPLINE_IMPLEMENTATION
#include "core/base.h"
#include "math/mathx.h"
#include "math/curve.h"

int main(void)
{
    print_cstr("test_bspline\n");

    /* at t=0.5 with 4 evenly-spaced collinear points, the uniform cubic
       B-spline should land exactly on the line (linear precision is a
       real, checkable property of this basis) */
    {
        f32 v = bsplinef(0.0f, 1.0f, 2.0f, 3.0f, 0.5f);
        TEST_CHECK(v > 1.499f && v < 1.501f, "bsplinef through 4 collinear points at t=0.5 lands on the line (1.5)");
    }

    /* the 4 basis weights (b0,b1,b2,b3) always sum to 1 (partition of
       unity) - a real, defining property of any B-spline basis, checked
       here by feeding equal control points (result must equal that value
       regardless of t, since weighted-average-of-equal-values == that value) */
    {
        i32 ok = 1;
        f32 t;
        for (t = 0.0f; t <= 1.001f; t += 0.1f) {
            f32 v = bsplinef(7.0f, 7.0f, 7.0f, 7.0f, t);
            if (v < 6.999f || v > 7.001f) ok = 0;
        }
        TEST_CHECK(ok, "bsplinef basis weights sum to 1 (partition of unity) across many t values");
    }

    /* unlike bezier/hermite, a uniform cubic B-spline does NOT generally
       pass through its own control points at t=0 or t=1 - verify it's
       actually different from p1 at t=0 for a real, non-degenerate case,
       confirming this is genuinely a B-spline basis and not accidentally
       reimplementing bezier/catmull/hermite's interpolating behavior.
       NON-collinear points on purpose: evenly-spaced/collinear points
       exercise the basis's own linear-precision property, which makes
       p(0) coincidentally equal p1 exactly for THAT special case (a real
       test bug caught here, not a hypothetical one) - p2 off the line
       breaks that coincidence so this actually tests what it claims to. */
    {
        f32 v0 = bsplinef(0.0f, 10.0f, 5.0f, 30.0f, 0.0f);
        TEST_CHECK(v0 > 10.001f || v0 < 9.999f, "bsplinef(t=0) does NOT equal p1 for non-collinear points - approximating, not interpolating, basis");
    }

    {
        vec2 p0 = vec2_make(0.0f, 0.0f);
        vec2 p1 = vec2_make(1.0f, 0.0f);
        vec2 p2 = vec2_make(2.0f, 0.0f);
        vec2 p3 = vec2_make(3.0f, 0.0f);
        vec2 mid = vec2_bspline(p0, p1, p2, p3, 0.5f);
        TEST_CHECK(mid.x > 1.499f && mid.x < 1.501f && mid.y == 0.0f,
                   "vec2_bspline through 4 collinear points at t=0.5 lands on the line");
    }

    {
        vec3 p0 = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 p1 = vec3_make(1.0f, 1.0f, 1.0f);
        vec3 p2 = vec3_make(2.0f, 2.0f, 2.0f);
        vec3 p3 = vec3_make(3.0f, 3.0f, 3.0f);
        vec3 mid = vec3_bspline(p0, p1, p2, p3, 0.5f);
        TEST_CHECK(mid.x > 1.499f && mid.x < 1.501f, "vec3_bspline through 4 collinear points at t=0.5 lands on the line");
    }

    print_flush();
    return test_failures_ != 0;
}
