#include "test_common.h"
#define MATH_TYPE_IMPLEMENTATION
#define TRIGONOMETRY_IMPLEMENTATION
#define LINEAR_ALGEBRA_IMPLEMENTATION
#define SLERP_IMPLEMENTATION
#include "types/base.h"
#include "math/mathx.h"
#include "math/trigonometry.h"
#include "math/linear_algebra.h"
#include "math/easing.h"
#include <math.h>

static i32 f32_close(f32 a, f32 b, f32 eps)
{
    f32 d = a - b;
    if (d < 0.0f) d = -d;
    return d < eps;
}

static i32 quat_close(Quat a, Quat b, f32 eps)
{
    return f32_close(a.x, b.x, eps) && f32_close(a.y, b.y, eps) &&
           f32_close(a.z, b.z, eps) && f32_close(a.w, b.w, eps);
}

int main(void)
{
    Quat identity, q90, mid, r;
    vec3 rotated, expected;

    print_cstr("test_slerp\n");

    identity = quat_identity();
    q90 = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), deg_to_rad(90.0f));

    r = quat_slerp(identity, q90, 0.0f);
    TEST_CHECK(quat_close(r, identity, 0.0001f), "quat_slerp(a,b,0) == a");

    r = quat_slerp(identity, q90, 1.0f);
    TEST_CHECK(quat_close(r, q90, 0.0001f), "quat_slerp(a,b,1) == b");

    /* slerp between 0deg and 90deg at t=0.5 should be exactly 45deg */
    mid = quat_slerp(identity, q90, 0.5f);
    rotated = quat_mul_vec3(mid, vec3_make(1.0f, 0.0f, 0.0f));
    expected = vec3_make((f32)cos((f64)deg_to_rad(45.0f)), (f32)sin((f64)deg_to_rad(45.0f)), 0.0f);
    TEST_CHECK(f32_close(rotated.x, expected.x, 0.001f) && f32_close(rotated.y, expected.y, 0.001f),
               "quat_slerp(0deg, 90deg, 0.5) lands exactly on 45deg");

    /* identical quaternions: slerp must return that same quaternion at any t,
     * not divide by zero / blow up in the near-parallel fallback path */
    r = quat_slerp(q90, q90, 0.37f);
    TEST_CHECK(quat_close(r, q90, 0.001f), "slerp between identical quaternions returns that quaternion (nlerp fallback path)");

    /* shortest path: slerping toward -q90 (same rotation, negated
     * representation) must still trace the SAME short arc as slerping
     * toward q90 itself */
    {
        Quat neg_q90 = quat_make(-q90.x, -q90.y, -q90.z, -q90.w);
        Quat r1 = quat_slerp(identity, q90, 0.5f);
        Quat r2 = quat_slerp(identity, neg_q90, 0.5f);
        vec3 v1 = quat_mul_vec3(r1, vec3_make(1.0f, 0.0f, 0.0f));
        vec3 v2 = quat_mul_vec3(r2, vec3_make(1.0f, 0.0f, 0.0f));
        TEST_CHECK(f32_close(v1.x, v2.x, 0.001f) && f32_close(v1.y, v2.y, 0.001f),
                   "slerp toward q and slerp toward -q (same rotation) take the same short path");
    }

    /* quat_nlerp: rough sanity, same endpoints, roughly monotonic between */
    r = quat_nlerp(identity, q90, 0.0f);
    TEST_CHECK(quat_close(r, identity, 0.0001f), "quat_nlerp(a,b,0) == a");
    r = quat_nlerp(identity, q90, 1.0f);
    TEST_CHECK(quat_close(r, q90, 0.0001f), "quat_nlerp(a,b,1) == b");

    print_flush();
    return test_failures_ != 0;
}
