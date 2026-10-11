#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define CATMULL_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "curve.h"

static i32 f32_close(f32 a, f32 b, f32 eps)
{
    f32 d = a - b;
    if (d < 0.0f) d = -d;
    return d < eps;
}

int main(void)
{
    print_cstr("test_catmull\n");

    /* t=0 must land exactly on p1, t=1 exactly on p2 - the curve passes
     * through its own control points at the segment endpoints (unlike
     * bezier, where c0/c1 only pull the curve toward them) */
    TEST_CHECK(f32_close(catmullf(0.0f, 10.0f, 20.0f, 30.0f, 0.0f), 10.0f, 0.0001f), "catmullf(t=0) == p1");
    TEST_CHECK(f32_close(catmullf(0.0f, 10.0f, 20.0f, 30.0f, 1.0f), 20.0f, 0.0001f), "catmullf(t=1) == p2");

    /* four collinear, evenly-spaced points -> the curve between p1,p2 is
     * just the straight line between them */
    TEST_CHECK(f32_close(catmullf(0.0f, 10.0f, 20.0f, 30.0f, 0.5f), 15.0f, 0.0001f),
               "evenly-spaced collinear points reduce catmull-rom to a straight lerp at t=0.5");

    {
        vec2 p0 = vec2_make(0.0f, 0.0f);
        vec2 p1 = vec2_make(10.0f, 0.0f);
        vec2 p2 = vec2_make(20.0f, 0.0f);
        vec2 p3 = vec2_make(30.0f, 0.0f);
        vec2 mid = vec2_catmull(p0, p1, p2, p3, 0.5f);
        TEST_CHECK(f32_close(mid.x, 15.0f, 0.0001f) && f32_close(mid.y, 0.0f, 0.0001f),
                   "vec2_catmull matches the scalar formula componentwise");
    }

    print_flush();
    return test_failures_ != 0;
}
