#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define QUAT_IMPLEMENTATION
#include "core/base.h"
#include "math/mathx.h"

int main(void)
{
    print_cstr("test_quat\n");

    {
        Quat id = quat_identity();
        TEST_CHECK(id.x == 0.0f && id.y == 0.0f && id.z == 0.0f && id.w == 1.0f,
                   "quat_identity is (0,0,0,1)");
    }

    {
        Quat q = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), 1.5707963f); /* 90 deg around z */
        vec3 v = quat_mul_vec3(q, vec3_make(1.0f, 0.0f, 0.0f));
        TEST_CHECK(v.x > -0.001f && v.x < 0.001f && v.y > 0.999f && v.y < 1.001f,
                   "quat_mul_vec3: 90deg around z sends (1,0,0) to (0,1,0)");
    }

    {
        Quat q1 = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), 0.7853981f); /* 45 deg */
        Quat q2 = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), 0.7853981f);
        Quat composed = quat_mul(q2, q1); /* apply q1 then q2 */
        vec3 v = quat_mul_vec3(composed, vec3_make(1.0f, 0.0f, 0.0f));
        TEST_CHECK(v.x > -0.001f && v.x < 0.001f && v.y > 0.999f && v.y < 1.001f,
                   "two composed 45deg rotations equal one 90deg rotation");
    }

    {
        Quat q = quat_from_axis_angle(vec3_make(1.0f, 1.0f, 1.0f), 1.2345f);
        Quat conj = quat_conj(q);
        vec3 original = vec3_make(3.0f, -1.0f, 2.0f);
        vec3 rotated = quat_mul_vec3(q, original);
        vec3 back = quat_mul_vec3(conj, rotated);
        f32 dx = back.x - original.x, dy = back.y - original.y, dz = back.z - original.z;
        TEST_CHECK(dx > -0.001f && dx < 0.001f && dy > -0.001f && dy < 0.001f && dz > -0.001f && dz < 0.001f,
                   "quat_conj undoes q's rotation (round-trip back to the original vector)");
    }

    {
        Quat q = quat_make(1.0f, 2.0f, 3.0f, 4.0f);
        Quat n = quat_normalize(q);
        f32 len = quat_len(n);
        TEST_CHECK(len > 0.999f && len < 1.001f, "quat_normalize produces a unit quaternion");
    }

    {
        Quat a = quat_make(1.0f, 0.0f, 0.0f, 0.0f);
        Quat b = quat_make(0.0f, 1.0f, 0.0f, 0.0f);
        TEST_CHECK(quat_dot(a, a) == 1.0f, "quat_dot(a,a) == 1 for a unit quaternion");
        TEST_CHECK(quat_dot(a, b) == 0.0f, "quat_dot of two orthogonal quaternions is 0");
    }

    {
        Quat degenerate = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 0.0f), 1.0f);
        Quat id = quat_identity();
        TEST_CHECK(degenerate.x == id.x && degenerate.y == id.y &&
                   degenerate.z == id.z && degenerate.w == id.w,
                   "quat_from_axis_angle with a zero-length axis returns identity, not NaN");
    }

    print_flush();
    return test_failures_ != 0;
}
