#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define TRIGONOMETRY_IMPLEMENTATION
#define QUAT_IMPLEMENTATION
#define MATRIX_IMPLEMENTATION
#include "types/base.h"
#include "math/mathx.h"
#include "math/trigonometry.h"
#include "math/linear_algebra.h"

static i32 f32_close(f32 a, f32 b, f32 eps)
{
    f32 d = a - b;
    if (d < 0.0f) d = -d;
    return d < eps;
}

static i32 vec3_close(vec3 a, vec3 b, f32 eps)
{
    return f32_close(a.x, b.x, eps) && f32_close(a.y, b.y, eps) && f32_close(a.z, b.z, eps);
}

int main(void)
{
    Mat4 id, m, r;
    vec4 p4, result4;
    vec3 p3, rotated, expected;
    Quat q, q2, qcomposed;
    i32 i, all_identity;

    print_cstr("test_linear_algebra\n");

    id = mat4_identity();
    all_identity = 1;
    for (i = 0; i < 4; i++) {
        i32 j;
        for (j = 0; j < 4; j++) {
            f32 want = (i == j) ? 1.0f : 0.0f;
            if (id.e[i][j] != want) all_identity = 0;
        }
    }
    TEST_CHECK(all_identity, "mat4_identity has 1s on the diagonal, 0s elsewhere");

    m = mat4_mul(id, id);
    TEST_CHECK(f32_close(m.e[0][0], 1.0f, 0.0001f) && f32_close(m.e[3][3], 1.0f, 0.0001f),
               "identity * identity == identity");

    /* translate a homogeneous point */
    m = mat4_translate(vec3_make(10.0f, 20.0f, 30.0f));
    p4 = vec4_make(1.0f, 2.0f, 3.0f, 1.0f);
    result4 = mat4_mul_vec4(m, p4);
    TEST_CHECK(f32_close(result4.x, 11.0f, 0.0001f) && f32_close(result4.y, 22.0f, 0.0001f) &&
               f32_close(result4.z, 33.0f, 0.0001f),
               "mat4_translate moves a homogeneous point (w=1) by the translation vector");

    /* translate a homogeneous DIRECTION (w=0) - must NOT move */
    p4 = vec4_make(1.0f, 2.0f, 3.0f, 0.0f);
    result4 = mat4_mul_vec4(m, p4);
    TEST_CHECK(f32_close(result4.x, 1.0f, 0.0001f) && f32_close(result4.y, 2.0f, 0.0001f),
               "mat4_translate does not move a direction vector (w=0)");

    /* rotate (1,0,0) by 90 degrees around Z -> (0,1,0) */
    r = mat4_rotate_z(deg_to_rad(90.0f));
    p4 = vec4_make(1.0f, 0.0f, 0.0f, 1.0f);
    result4 = mat4_mul_vec4(r, p4);
    TEST_CHECK(f32_close(result4.x, 0.0f, 0.001f) && f32_close(result4.y, 1.0f, 0.001f),
               "mat4_rotate_z(90deg) sends (1,0,0) to (0,1,0)");

    /* same rotation via the general axis-angle mat4_rotate must agree */
    r = mat4_rotate(vec3_make(0.0f, 0.0f, 1.0f), deg_to_rad(90.0f));
    result4 = mat4_mul_vec4(r, p4);
    TEST_CHECK(f32_close(result4.x, 0.0f, 0.001f) && f32_close(result4.y, 1.0f, 0.001f),
               "mat4_rotate(z-axis, 90deg) agrees with mat4_rotate_z(90deg)");

    /* the SAME rotation via quaternions must land on the same answer -
     * this is the real cross-check that both representations are
     * mathematically consistent with each other, not just internally
     * self-consistent */
    q = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), deg_to_rad(90.0f));
    p3 = vec3_make(1.0f, 0.0f, 0.0f);
    rotated = quat_mul_vec3(q, p3);
    expected = vec3_make(0.0f, 1.0f, 0.0f);
    TEST_CHECK(vec3_close(rotated, expected, 0.001f), "quat_mul_vec3 agrees with mat4_rotate_z for the same rotation");

    /* mat4_from_quat must also agree */
    r = mat4_from_quat(q);
    result4 = mat4_mul_vec4(r, vec4_make(1.0f, 0.0f, 0.0f, 1.0f));
    TEST_CHECK(f32_close(result4.x, 0.0f, 0.001f) && f32_close(result4.y, 1.0f, 0.001f),
               "mat4_from_quat produces the same rotation as the quaternion it came from");

    /* quaternion composition: two 45-degree rotations == one 90-degree rotation */
    q = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), deg_to_rad(45.0f));
    q2 = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), deg_to_rad(45.0f));
    qcomposed = quat_mul(q2, q); /* apply q first, then q2 */
    rotated = quat_mul_vec3(qcomposed, p3);
    TEST_CHECK(vec3_close(rotated, expected, 0.001f), "two composed 45deg quat rotations equal one 90deg rotation");

    /* quat_conj inverts a unit quaternion's rotation */
    rotated = quat_mul_vec3(quat_conj(q), quat_mul_vec3(q, p3));
    TEST_CHECK(vec3_close(rotated, p3, 0.001f), "quat_conj(q) undoes q's rotation (round-trip back to the original vector)");

    /* quat_normalize actually normalizes */
    q = quat_make(2.0f, 0.0f, 0.0f, 0.0f);
    q = quat_normalize(q);
    TEST_CHECK(f32_close(quat_len(q), 1.0f, 0.0001f), "quat_normalize produces a unit quaternion");

    print_flush();
    return test_failures_ != 0;
}
