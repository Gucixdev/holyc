#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#include "base.h"
#include "mathx.h"

int main(void)
{
    vec3 v1, v2, v3;

    print_cstr("test_vector\n");

    v1 = vec3_make(1.0f, 2.0f, 3.0f);
    v2 = vec3_make(4.0f, 5.0f, 6.0f);
    v3 = vec3_add(v1, v2);
    TEST_CHECK(v3.x == 5.0f && v3.y == 7.0f && v3.z == 9.0f, "vec3_add");

    TEST_CHECK(vec3_dot(v1, v2) == 32.0f, "vec3_dot == 32");

    {
        vec3 diff = vec3_sub(v2, v1);
        TEST_CHECK(diff.x == 3.0f && diff.y == 3.0f && diff.z == 3.0f, "vec3_sub");
    }

    {
        vec3 scaled = vec3_scale(vec3_make(1.0f, 2.0f, 3.0f), 2.0f);
        TEST_CHECK(scaled.x == 2.0f && scaled.y == 4.0f && scaled.z == 6.0f, "vec3_scale");
    }

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
        vec2 b = vec2_make(1.0f, 1.0f);
        vec2 sum = vec2_add(a, b);
        TEST_CHECK(sum.x == 4.0f && sum.y == 5.0f, "vec2_add");
        TEST_CHECK(vec2_len(a) > 4.999f && vec2_len(a) < 5.001f, "vec2_len(3,4) ~= 5");
        TEST_CHECK(vec2_dot(a, b) == 7.0f, "vec2_dot");
    }

    {
        vec4 a = vec4_make(1.0f, 2.0f, 3.0f, 4.0f);
        vec4 b = vec4_make(4.0f, 3.0f, 2.0f, 1.0f);
        vec4 sum = vec4_add(a, b);
        TEST_CHECK(sum.x == 5.0f && sum.y == 5.0f && sum.z == 5.0f && sum.w == 5.0f, "vec4_add");
        TEST_CHECK(vec4_dot(a, b) == 4.0f + 6.0f + 6.0f + 4.0f, "vec4_dot");
        {
            f32 len = vec4_len(vec4_make(0.0f, 0.0f, 0.0f, 5.0f));
            TEST_CHECK(len > 4.999f && len < 5.001f, "vec4_len(0,0,0,5) ~= 5");
        }
    }

    print_flush();
    return test_failures_ != 0;
}
