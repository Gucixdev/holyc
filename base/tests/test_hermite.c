#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define HERMITE_IMPLEMENTATION
#include "types/base.h"
#include "math/mathx.h"
#include "math/curve.h"

int main(void)
{
    print_cstr("test_hermite\n");

    /* at t=0, the curve passes exactly through p0 (defining property) */
    TEST_CHECK(hermitef(1.0f, 5.0f, 4.0f, -3.0f, 0.0f) == 1.0f, "hermitef(t=0) == p0 exactly");

    /* at t=1, the curve passes exactly through p1 (defining property) */
    TEST_CHECK(hermitef(1.0f, 5.0f, 4.0f, -3.0f, 1.0f) == 4.0f, "hermitef(t=1) == p1 exactly");

    /* zero tangents at both ends, straight line case: at t=0.5, halfway
       between p0 and p1 for a flat (ease-in-ease-out) interpolation */
    {
        f32 mid = hermitef(0.0f, 0.0f, 10.0f, 0.0f, 0.5f);
        TEST_CHECK(mid > 4.99f && mid < 5.01f, "hermitef with zero tangents at t=0.5 is the midpoint (ease-in-ease-out symmetry)");
    }

    /* vec2/vec3 wrappers apply the same interpolation per-component */
    {
        vec2 p0 = vec2_make(0.0f, 0.0f);
        vec2 m0 = vec2_make(1.0f, 0.0f);
        vec2 p1 = vec2_make(1.0f, 1.0f);
        vec2 m1 = vec2_make(0.0f, 1.0f);
        vec2 start = vec2_hermite(p0, m0, p1, m1, 0.0f);
        vec2 end = vec2_hermite(p0, m0, p1, m1, 1.0f);
        TEST_CHECK(start.x == 0.0f && start.y == 0.0f, "vec2_hermite(t=0) == p0 exactly");
        TEST_CHECK(end.x == 1.0f && end.y == 1.0f, "vec2_hermite(t=1) == p1 exactly");
    }

    {
        vec3 p0 = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 m0 = vec3_make(1.0f, 0.0f, 0.0f);
        vec3 p1 = vec3_make(2.0f, 2.0f, 2.0f);
        vec3 m1 = vec3_make(0.0f, 1.0f, 0.0f);
        vec3 start = vec3_hermite(p0, m0, p1, m1, 0.0f);
        vec3 end = vec3_hermite(p0, m0, p1, m1, 1.0f);
        TEST_CHECK(start.x == 0.0f && start.y == 0.0f && start.z == 0.0f, "vec3_hermite(t=0) == p0 exactly");
        TEST_CHECK(end.x == 2.0f && end.y == 2.0f && end.z == 2.0f, "vec3_hermite(t=1) == p1 exactly");
    }

    print_flush();
    return test_failures_ != 0;
}
