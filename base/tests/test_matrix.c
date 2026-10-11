#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define MATRIX_IMPLEMENTATION
#define QUAT_IMPLEMENTATION
#include "core/base.h"
#include "math/mathx.h"

int main(void)
{
    print_cstr("test_matrix\n");

    {
        Mat4 id = mat4_identity();
        i32 ok = 1;
        i32 r, c;
        for (c = 0; c < 4; c++) {
            for (r = 0; r < 4; r++) {
                f32 expected = (r == c) ? 1.0f : 0.0f;
                if (id.e[c][r] != expected) ok = 0;
            }
        }
        TEST_CHECK(ok, "mat4_identity has 1s on the diagonal, 0s elsewhere");
    }

    {
        Mat4 id = mat4_identity();
        Mat4 result = mat4_mul(id, id);
        i32 ok = 1;
        i32 r, c;
        for (c = 0; c < 4; c++)
            for (r = 0; r < 4; r++)
                if (result.e[c][r] != id.e[c][r]) ok = 0;
        TEST_CHECK(ok, "identity * identity == identity");
    }

    {
        Mat4 t = mat4_translate(vec3_make(1.0f, 2.0f, 3.0f));
        vec4 p = mat4_mul_vec4(t, vec4_make(0.0f, 0.0f, 0.0f, 1.0f));
        TEST_CHECK(p.x == 1.0f && p.y == 2.0f && p.z == 3.0f,
                   "mat4_translate moves a homogeneous point (w=1)");

        {
            vec4 d = mat4_mul_vec4(t, vec4_make(5.0f, 5.0f, 5.0f, 0.0f));
            TEST_CHECK(d.x == 5.0f && d.y == 5.0f && d.z == 5.0f,
                       "mat4_translate does not move a direction vector (w=0)");
        }
    }

    {
        Mat4 s = mat4_scale(vec3_make(2.0f, 3.0f, 4.0f));
        vec4 p = mat4_mul_vec4(s, vec4_make(1.0f, 1.0f, 1.0f, 1.0f));
        TEST_CHECK(p.x == 2.0f && p.y == 3.0f && p.z == 4.0f, "mat4_scale scales each axis independently");
    }

    {
        Mat4 rz = mat4_rotate_z(1.5707963f); /* 90 degrees */
        vec4 p = mat4_mul_vec4(rz, vec4_make(1.0f, 0.0f, 0.0f, 1.0f));
        TEST_CHECK(p.x > -0.001f && p.x < 0.001f && p.y > 0.999f && p.y < 1.001f,
                   "mat4_rotate_z(90deg) sends (1,0,0) to (0,1,0)");
    }

    {
        Mat4 rz1 = mat4_rotate_z(1.5707963f);
        Mat4 rz2 = mat4_rotate(vec3_make(0.0f, 0.0f, 1.0f), 1.5707963f);
        vec4 p1 = mat4_mul_vec4(rz1, vec4_make(1.0f, 0.0f, 0.0f, 1.0f));
        vec4 p2 = mat4_mul_vec4(rz2, vec4_make(1.0f, 0.0f, 0.0f, 1.0f));
        f32 dx = p1.x - p2.x, dy = p1.y - p2.y;
        TEST_CHECK(dx > -0.001f && dx < 0.001f && dy > -0.001f && dy < 0.001f,
                   "mat4_rotate(z-axis, 90deg) agrees with mat4_rotate_z(90deg)");
    }

    {
        Mat4 m;
        Mat4 t;
        i32 ok;
        i32 r, c;

        m = mat4_identity();
        m.e[3][0] = 7.0f; /* translation lives in the last column */
        t = mat4_transpose(m);

        ok = 1;
        for (c = 0; c < 4; c++)
            for (r = 0; r < 4; r++)
                if (t.e[c][r] != m.e[r][c]) ok = 0;
        TEST_CHECK(ok, "mat4_transpose swaps row/column indices correctly");
    }

    {
        Quat q = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), 1.5707963f);
        Mat4 m = mat4_from_quat(q);
        vec4 p = mat4_mul_vec4(m, vec4_make(1.0f, 0.0f, 0.0f, 1.0f));
        TEST_CHECK(p.x > -0.001f && p.x < 0.001f && p.y > 0.999f && p.y < 1.001f,
                   "mat4_from_quat produces the same rotation as the quaternion it came from");
    }

    print_flush();
    return test_failures_ != 0;
}
