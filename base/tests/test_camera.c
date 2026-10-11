#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define QUAT_IMPLEMENTATION
#define MATRIX_IMPLEMENTATION
#define CAMERA_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "camera.h"

int main(void)
{
    print_cstr("test_camera\n");

    /* identity camera (at origin, identity orientation) produces an
       identity view matrix - the defining base case */
    {
        Camera cam;
        Mat4 view;
        i32 ok = 1;
        i32 r, c;

        camera_init(&cam, vec3_make(0.0f, 0.0f, 0.0f), quat_identity());
        view = camera_view_matrix(&cam);

        for (c = 0; c < 4; c++) {
            for (r = 0; r < 4; r++) {
                f32 expected = (r == c) ? 1.0f : 0.0f;
                f32 diff = view.e[c][r] - expected;
                if (diff < -0.0001f || diff > 0.0001f) ok = 0;
            }
        }
        TEST_CHECK(ok, "identity camera (origin, identity orientation) produces an identity view matrix");
    }

    /* a camera translated to (5,0,0) with identity orientation should
       have a view matrix that moves world-space point (5,0,0) to
       camera-space origin (0,0,0) */
    {
        Camera cam;
        Mat4 view;
        vec4 world_p, cam_p;

        camera_init(&cam, vec3_make(5.0f, 0.0f, 0.0f), quat_identity());
        view = camera_view_matrix(&cam);
        world_p = vec4_make(5.0f, 0.0f, 0.0f, 1.0f);
        cam_p = mat4_mul_vec4(view, world_p);

        TEST_CHECK(cam_p.x > -0.001f && cam_p.x < 0.001f &&
                   cam_p.y > -0.001f && cam_p.y < 0.001f &&
                   cam_p.z > -0.001f && cam_p.z < 0.001f,
                   "the camera's own world position transforms to the camera-space origin");
    }

    /* perspective projection: known structure for fov=90deg, aspect=1,
       near=1, far=100 - tan(45deg)==1, so f==1, e[0][0]==e[1][1]==1 */
    {
        Mat4 proj = camera_perspective(1.5707963267948966f /* 90 deg in radians */, 1.0f, 1.0f, 100.0f);
        TEST_CHECK(proj.e[0][0] > 0.999f && proj.e[0][0] < 1.001f, "perspective e[0][0] == 1 for 90deg fov, aspect=1");
        TEST_CHECK(proj.e[1][1] > 0.999f && proj.e[1][1] < 1.001f, "perspective e[1][1] == 1 for 90deg fov");
        TEST_CHECK(proj.e[2][3] > -1.001f && proj.e[2][3] < -0.999f, "perspective e[2][3] == -1 (the standard w-divide row)");
    }

    /* camera_forward/right/up for an identity-orientation camera match
       the standard OpenGL convention: looking down -Z, +X right, +Y up */
    {
        Camera cam;
        vec3 fwd, right, up;

        camera_init(&cam, vec3_make(0.0f, 0.0f, 0.0f), quat_identity());
        fwd = camera_forward(&cam);
        right = camera_right(&cam);
        up = camera_up(&cam);

        TEST_CHECK(fwd.z < -0.999f && fwd.z > -1.001f, "camera_forward for identity orientation points down -Z");
        TEST_CHECK(right.x > 0.999f && right.x < 1.001f, "camera_right for identity orientation points along +X");
        TEST_CHECK(up.y > 0.999f && up.y < 1.001f, "camera_up for identity orientation points along +Y");
    }

    /* camera_move translates along the camera's own basis, not world axes */
    {
        Camera cam;
        camera_init(&cam, vec3_make(0.0f, 0.0f, 0.0f), quat_identity());
        camera_move(&cam, 2.0f, 3.0f, 1.0f); /* forward, right, up */
        /* forward is -Z, right is +X, up is +Y for identity orientation */
        TEST_CHECK(cam.position.x > 2.999f && cam.position.x < 3.001f, "camera_move's 'right' component moves along +X");
        TEST_CHECK(cam.position.y > 0.999f && cam.position.y < 1.001f, "camera_move's 'up' component moves along +Y");
        TEST_CHECK(cam.position.z < -1.999f && cam.position.z > -2.001f, "camera_move's 'forward' component moves along -Z");
    }

    print_flush();
    return test_failures_ != 0;
}
