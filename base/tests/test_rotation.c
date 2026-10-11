#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#define QUAT_IMPLEMENTATION
#define ROTATION_IMPLEMENTATION
#include "rotation.c"

int main(void)
{
    Rotation r;

    print_cstr("test_rotation\n");

    rotation_init(&r, quat_identity());
    TEST_CHECK(r.value.w == 1.0f && r.value.x == 0.0f, "rotation_init sets the initial quaternion");

    {
        Quat q = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), 1.5707963f);
        rotation_set(&r, q);
        TEST_CHECK(r.value.z > 0.7f && r.value.z < 0.71f, "rotation_set replaces the value");
    }

    {
        vec3 v = rotation_apply(&r, vec3_make(1.0f, 0.0f, 0.0f));
        TEST_CHECK(v.y > 0.999f && v.y < 1.001f, "rotation_apply rotates a vector by the stored quaternion (90deg around z sends x to y)");
    }

    print_flush();
    return test_failures_ != 0;
}
