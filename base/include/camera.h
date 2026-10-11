#ifndef HOLYC_CAMERA_H_
#define HOLYC_CAMERA_H_

/* First-person-style 3D camera: position + orientation (Quat, from
 * quat.h), OpenGL convention (looking down -Z by default, +X right,
 * +Y up - matching matrix.h/quat.h's own established convention, see
 * their file comments).
 *
 * camera_view_matrix computes the INVERSE of the camera's own world
 * transform directly rather than via a general matrix inverse: for a
 * pure rotation+translation, inverse(R * T(p)) == inverse(T(p)) *
 * inverse(R) == T(-p) * R^T (R's inverse is its transpose since
 * rotation matrices are orthonormal - mat4_transpose from matrix.h
 * does this exactly). view = R^T * T(-p): translate by -position
 * first, then rotate by the transposed orientation - matches
 * mat4_mul's own "right operand applied first" convention (see
 * matrix.h/linear_algebra.h's own mat4_mul semantics).
 *
 * camera_perspective is the standard OpenGL-style right-handed
 * perspective projection (NDC z in [-1,1]) - the exact, published
 * formula, not approximated: given vertical fov (radians), aspect
 * (width/height), near, far:
 *   f = 1/tan(fovy/2)
 *   e[0][0]=f/aspect  e[1][1]=f  e[2][2]=(far+near)/(near-far)
 *   e[2][3]=-1        e[3][2]=(2*far*near)/(near-far) */

#include "base.h"
#include "mathx.h"

typedef struct {
    vec3 position;
    Quat orientation;
} Camera;

void camera_init(Camera *cam, vec3 position, Quat orientation);

Mat4 camera_view_matrix(const Camera *cam);
Mat4 camera_perspective(f32 fovy_rad, f32 aspect, f32 near_, f32 far_);

vec3 camera_forward(const Camera *cam); /* -Z rotated by orientation */
vec3 camera_right(const Camera *cam);   /* +X rotated by orientation */
vec3 camera_up(const Camera *cam);      /* +Y rotated by orientation */

/* moves position along the camera's OWN basis vectors (forward/right/up),
 * not world axes - e.g. camera_move(cam, 1,0,0) always moves "forward"
 * from the camera's current facing, whatever that currently is. */
void camera_move(Camera *cam, f32 forward_amount, f32 right_amount, f32 up_amount);

/* yaw around world +Y (applied first, so left/right look stays level
 * regardless of current pitch - the standard FPS-camera technique),
 * then pitch around the camera's own local +X (applied after, so
 * up/down look happens relative to the current facing). Both in
 * radians. */
void camera_look(Camera *cam, f32 yaw_delta_rad, f32 pitch_delta_rad);

#endif /* HOLYC_CAMERA_H_ */

#ifdef CAMERA_IMPLEMENTATION
#ifndef CAMERA_IMPLEMENTATION_
#define CAMERA_IMPLEMENTATION_

/* Range-reduced Taylor-series sin/cos (tan = sin/cos), replacing
 * math.h's tan() - own local copy, matching base.h's own quat_sin_/
 * quat_cos_ pattern (0 glibc push, no-glibc rule). */
static f64 camera_mod_2pi_(f64 x)
{
    const f64 two_pi = 6.283185307179586476925286766559;
    const f64 pi = 3.14159265358979323846;
    f64 n = (f64)(i64)(x / two_pi);
    f64 r = x - n * two_pi;
    if (r > pi) r -= two_pi;
    if (r < -pi) r += two_pi;
    return r;
}

static f64 camera_sin_(f64 x)
{
    f64 r = camera_mod_2pi_(x);
    f64 x2 = r * r;
    f64 term = r;
    f64 sum = r;
    i32 n;
    for (n = 1; n <= 6; n++) {
        term *= -x2 / ((f64)(2 * n) * (f64)(2 * n + 1));
        sum += term;
    }
    return sum;
}

static f64 camera_cos_(f64 x)
{
    f64 r = camera_mod_2pi_(x);
    f64 x2 = r * r;
    f64 term = 1.0;
    f64 sum = 1.0;
    i32 n;
    for (n = 1; n <= 6; n++) {
        term *= -x2 / ((f64)(2 * n - 1) * (f64)(2 * n));
        sum += term;
    }
    return sum;
}

static f64 camera_tan_(f64 x)
{
    return camera_sin_(x) / camera_cos_(x);
}

void camera_init(Camera *cam, vec3 position, Quat orientation)
{
    cam->position = position;
    cam->orientation = orientation;
}

Mat4 camera_view_matrix(const Camera *cam)
{
    Mat4 rot_t = mat4_transpose(mat4_from_quat(cam->orientation));
    Mat4 trans = mat4_translate(vec3_scale(cam->position, -1.0f));
    return mat4_mul(rot_t, trans);
}

Mat4 camera_perspective(f32 fovy_rad, f32 aspect, f32 near_, f32 far_)
{
    Mat4 m;
    f32 f = 1.0f / (f32)camera_tan_((f64)fovy_rad / 2.0);
    i32 r, c;

    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++)
            m.e[c][r] = 0.0f;

    m.e[0][0] = f / aspect;
    m.e[1][1] = f;
    m.e[2][2] = (far_ + near_) / (near_ - far_);
    m.e[2][3] = -1.0f;
    m.e[3][2] = (2.0f * far_ * near_) / (near_ - far_);

    return m;
}

vec3 camera_forward(const Camera *cam)
{
    return quat_mul_vec3(cam->orientation, vec3_make(0.0f, 0.0f, -1.0f));
}

vec3 camera_right(const Camera *cam)
{
    return quat_mul_vec3(cam->orientation, vec3_make(1.0f, 0.0f, 0.0f));
}

vec3 camera_up(const Camera *cam)
{
    return quat_mul_vec3(cam->orientation, vec3_make(0.0f, 1.0f, 0.0f));
}

void camera_move(Camera *cam, f32 forward_amount, f32 right_amount, f32 up_amount)
{
    vec3 fwd = vec3_scale(camera_forward(cam), forward_amount);
    vec3 right = vec3_scale(camera_right(cam), right_amount);
    vec3 up = vec3_scale(camera_up(cam), up_amount);
    cam->position = vec3_add(cam->position, vec3_add(fwd, vec3_add(right, up)));
}

void camera_look(Camera *cam, f32 yaw_delta_rad, f32 pitch_delta_rad)
{
    Quat yaw_q = quat_from_axis_angle(vec3_make(0.0f, 1.0f, 0.0f), yaw_delta_rad);
    Quat pitch_q = quat_from_axis_angle(vec3_make(1.0f, 0.0f, 0.0f), pitch_delta_rad);
    cam->orientation = quat_mul(yaw_q, quat_mul(cam->orientation, pitch_q));
}

#endif /* CAMERA_IMPLEMENTATION_ */
#endif /* CAMERA_IMPLEMENTATION */
