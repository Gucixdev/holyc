#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#include "types/base.h"
#include "math/mathx.h"

int main(void)
{
    vec3 v1, v2, v3;

    print_cstr("test_math_type\n");

    v1 = vec3_make(1.0f, 2.0f, 3.0f);
    v2 = vec3_make(4.0f, 5.0f, 6.0f);
    v3 = vec3_add(v1, v2);
    TEST_CHECK(v3.x == 5.0f && v3.y == 7.0f && v3.z == 9.0f, "vec3_add");

    TEST_CHECK(vec3_dot(v1, v2) == 32.0f, "vec3_dot == 32");

    {
        f32 len = vec3_len(vec3_make(3.0f, 4.0f, 0.0f));
        TEST_CHECK(len > 4.999f && len < 5.001f, "vec3_len(3,4,0) ~= 5");
    }

    {
        vec3 cross = vec3_cross(vec3_make(1.0f, 0.0f, 0.0f), vec3_make(0.0f, 1.0f, 0.0f));
        TEST_CHECK(cross.x == 0.0f && cross.y == 0.0f && cross.z == 1.0f, "vec3_cross(x,y) == z");
    }

    {
        vec2 a = vec2_make(3.0f, 4.0f);
        TEST_CHECK(vec2_len(a) > 4.999f && vec2_len(a) < 5.001f, "vec2_len(3,4) ~= 5");
    }

    print_flush();
    return test_failures_ != 0;
}
