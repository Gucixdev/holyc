#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define BEZIER_IMPLEMENTATION
#include "core/base.h"
#include "math/mathx.h"
#include "math/curve.h"

static i32 f32_close(f32 a, f32 b, f32 eps)
{
    f32 d = a - b;
    if (d < 0.0f) d = -d;
    return d < eps;
}

int main(void)
{
    vec2 p0, c0, c1, p1, mid;

    print_cstr("test_bezier\n");

    TEST_CHECK(f32_close(bezierf(0.0f, 1.0f, 2.0f, 3.0f, 0.0f), 0.0f, 0.0001f), "bezierf(s=0) == p0");
    TEST_CHECK(f32_close(bezierf(0.0f, 1.0f, 2.0f, 3.0f, 1.0f), 3.0f, 0.0001f), "bezierf(s=1) == p1");

    /* a "straight line" bezier (control points on the p0->p1 line) must
     * be linear - bezierf(s) == lerp(p0,p1,s) for every s, not just the endpoints */
    TEST_CHECK(f32_close(bezierf(0.0f, 3.333f, 6.667f, 10.0f, 0.5f), 5.0f, 0.01f),
               "collinear control points reduce cubic bezier to a straight lerp at s=0.5");

    p0 = vec2_make(0.0f, 0.0f);
    c0 = vec2_make(0.0f, 10.0f);
    c1 = vec2_make(10.0f, 10.0f);
    p1 = vec2_make(10.0f, 0.0f);
    mid = vec2_bezier(p0, c0, c1, p1, 0.5f);
    TEST_CHECK(f32_close(mid.x, 5.0f, 0.0001f), "vec2_bezier is symmetric at s=0.5 for a symmetric control cage");
    TEST_CHECK(mid.y > 0.0f, "vec2_bezier midpoint is pulled toward the control points (curve bulges upward)");

    print_flush();
    return test_failures_ != 0;
}
