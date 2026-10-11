#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define LERP_IMPLEMENTATION
#include "types/base.h"
#include "math/mathx.h"
#include "math/easing.h"

int main(void)
{
    vec3 a, b, m;

    print_cstr("test_lerp\n");

    TEST_CHECK(lerpf(0.0f, 10.0f, 0.5f) == 5.0f, "lerpf midpoint");
    TEST_CHECK(lerpf(0.0f, 10.0f, 0.0f) == 0.0f, "lerpf t=0 returns from");
    TEST_CHECK(lerpf(0.0f, 10.0f, 1.0f) == 10.0f, "lerpf t=1 returns to");

    TEST_CHECK(lerpcf(0.0f, 10.0f, 2.0f) == 10.0f, "lerpcf clamps t above 1");
    TEST_CHECK(lerpcf(0.0f, 10.0f, -2.0f) == 0.0f, "lerpcf clamps t below 0");

    TEST_CHECK(clampf(5.0f, 0.0f, 10.0f) == 5.0f, "clampf inside range unchanged");
    TEST_CHECK(clampf(-5.0f, 0.0f, 10.0f) == 0.0f, "clampf below range clamps to lo");
    TEST_CHECK(clampf(15.0f, 0.0f, 10.0f) == 10.0f, "clampf above range clamps to hi");

    TEST_CHECK(percentf(0.0f, 10.0f, 5.0f) == 0.5f, "percentf is the inverse of lerpf");

    TEST_CHECK(stepf(5.0f, 4.0f) == 0.0f, "stepf below edge is 0");
    TEST_CHECK(stepf(5.0f, 6.0f) == 1.0f, "stepf at/above edge is 1");

    TEST_CHECK(smoothf(0.0f) == 0.0f && smoothf(1.0f) == 1.0f, "smoothf endpoints are 0 and 1");
    TEST_CHECK(smoothf(0.5f) == 0.5f, "smoothf is symmetric at the midpoint");

    TEST_CHECK(smoothstepf(0.0f, 10.0f, -5.0f) == 0.0f, "smoothstepf clamps below edge0 to 0");
    TEST_CHECK(smoothstepf(0.0f, 10.0f, 15.0f) == 1.0f, "smoothstepf clamps above edge1 to 1");

    a = vec3_make(0.0f, 0.0f, 0.0f);
    b = vec3_make(10.0f, 20.0f, 30.0f);
    m = vec3_lerp(a, b, 0.5f);
    TEST_CHECK(m.x == 5.0f && m.y == 10.0f && m.z == 15.0f, "vec3_lerp componentwise midpoint");

    print_flush();
    return test_failures_ != 0;
}
