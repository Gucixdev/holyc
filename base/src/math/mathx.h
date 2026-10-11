#ifndef HOLYC_MATHX_H_
#define HOLYC_MATHX_H_

/* vec2/vec3/vec4 + Quat + Mat4 - split out of base.h (was base.h's own
 * "math types" section, moved out so base.h
 * stays scoped to base types/bit/print/log, not vector math too). Same
 * per-component IMPLEMENTATION guard names as before the move
 * (VECTOR_IMPLEMENTATION, QUAT_IMPLEMENTATION, MATRIX_IMPLEMENTATION) -
 * every existing `#define X_IMPLEMENTATION` call site keeps working
 * unchanged, consumers just need `#include "math/mathx.h"` in addition to
 * `#include "types/base.h"`. */

#include "types/base.h"

/* ============================================================
 * math types: vector (vector.h)
 * ============================================================ */

typedef struct { f32 x, y; }       vec2;
typedef struct { f32 x, y, z; }    vec3;
typedef struct { f32 x, y, z, w; } vec4;

vec2 vec2_make(f32 x, f32 y);
vec2 vec2_add(vec2 a, vec2 b);
vec2 vec2_sub(vec2 a, vec2 b);
vec2 vec2_scale(vec2 a, f32 s);
f32  vec2_dot(vec2 a, vec2 b);
f32  vec2_len(vec2 a);

vec3 vec3_make(f32 x, f32 y, f32 z);
vec3 vec3_add(vec3 a, vec3 b);
vec3 vec3_sub(vec3 a, vec3 b);
vec3 vec3_scale(vec3 a, f32 s);
f32  vec3_dot(vec3 a, vec3 b);
vec3 vec3_cross(vec3 a, vec3 b);
f32  vec3_len(vec3 a);

vec4 vec4_make(f32 x, f32 y, f32 z, f32 w);
vec4 vec4_add(vec4 a, vec4 b);
vec4 vec4_sub(vec4 a, vec4 b);
vec4 vec4_scale(vec4 a, f32 s);
f32  vec4_dot(vec4 a, vec4 b);
f32  vec4_len(vec4 a);

/* ============================================================
 * math types: quat (quat.h) - depends only on vec3 above, not on Mat4
 * ============================================================ */

typedef struct { f32 x, y, z, w; } Quat; /* w last */

Quat quat_identity(void);
Quat quat_make(f32 x, f32 y, f32 z, f32 w);
Quat quat_from_axis_angle(vec3 axis, f32 angle_rad); /* axis need not be pre-normalized */
Quat quat_mul(Quat a, Quat b);
Quat quat_conj(Quat q);
f32  quat_dot(Quat a, Quat b);
f32  quat_len(Quat q);
Quat quat_normalize(Quat q);
vec3 quat_mul_vec3(Quat q, vec3 v); /* rotate v by q */

/* ============================================================
 * math types: matrix (matrix.h) - COLUMN-MAJOR: m.e[col][row],
 * translation in the last column. Depends on vec3/vec4 and, only for
 * mat4_from_quat, on Quat above.
 * ============================================================ */

typedef struct { f32 e[4][4]; } Mat4; /* e[col][row] */

Mat4 mat4_identity(void);
Mat4 mat4_mul(Mat4 a, Mat4 b);
vec4 mat4_mul_vec4(Mat4 m, vec4 v);
Mat4 mat4_translate(vec3 t);
Mat4 mat4_scale(vec3 s);
Mat4 mat4_rotate(vec3 axis, f32 angle_rad); /* axis need not be pre-normalized */
Mat4 mat4_rotate_x(f32 angle_rad);
Mat4 mat4_rotate_y(f32 angle_rad);
Mat4 mat4_rotate_z(f32 angle_rad);
Mat4 mat4_transpose(Mat4 m);
Mat4 mat4_from_quat(Quat q);

#endif /* HOLYC_MATHX_H_ */

#ifdef VECTOR_IMPLEMENTATION
#ifndef VECTOR_IMPLEMENTATION_
#define VECTOR_IMPLEMENTATION_

/* Newton-Raphson sqrt, replacing math.h's - own local copy (not shared
 * with quat's own copy below) matching this project's standing "no
 * cross-IMPLEMENTATION-guard dependency" rule. Converges quadratically;
 * the iteration cap is a safety net, `next == guess` is the real exit
 * in every real case. */
static f64 vector_sqrt_(f64 x)
{
    f64 guess;
    i32 i;
    if (x <= 0.0) return 0.0; /* sqrt(0)==0; negative input has no real answer, return 0 rather than NaN (no NaN infra without libm) */
    guess = (x > 1.0) ? x : 1.0;
    for (i = 0; i < 40; i++) {
        f64 next = 0.5 * (guess + x / guess);
        if (next == guess) break;
        guess = next;
    }
    return guess;
}

vec2 vec2_make(f32 x, f32 y)
{
    vec2 v;
    v.x = x;
    v.y = y;
    return v;
}

vec2 vec2_add(vec2 a, vec2 b) { return vec2_make(a.x + b.x, a.y + b.y); }
vec2 vec2_sub(vec2 a, vec2 b) { return vec2_make(a.x - b.x, a.y - b.y); }
vec2 vec2_scale(vec2 a, f32 s) { return vec2_make(a.x * s, a.y * s); }
f32 vec2_dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
f32 vec2_len(vec2 a) { return (f32)vector_sqrt_((f64)vec2_dot(a, a)); }

vec3 vec3_make(f32 x, f32 y, f32 z)
{
    vec3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

vec3 vec3_add(vec3 a, vec3 b) { return vec3_make(a.x + b.x, a.y + b.y, a.z + b.z); }
vec3 vec3_sub(vec3 a, vec3 b) { return vec3_make(a.x - b.x, a.y - b.y, a.z - b.z); }
vec3 vec3_scale(vec3 a, f32 s) { return vec3_make(a.x * s, a.y * s, a.z * s); }
f32 vec3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

vec3 vec3_cross(vec3 a, vec3 b)
{
    return vec3_make(
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    );
}

f32 vec3_len(vec3 a) { return (f32)vector_sqrt_((f64)vec3_dot(a, a)); }

vec4 vec4_make(f32 x, f32 y, f32 z, f32 w)
{
    vec4 v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.w = w;
    return v;
}

vec4 vec4_add(vec4 a, vec4 b) { return vec4_make(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
vec4 vec4_sub(vec4 a, vec4 b) { return vec4_make(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); }
vec4 vec4_scale(vec4 a, f32 s) { return vec4_make(a.x * s, a.y * s, a.z * s, a.w * s); }
f32 vec4_dot(vec4 a, vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
f32 vec4_len(vec4 a) { return (f32)vector_sqrt_((f64)vec4_dot(a, a)); }

#endif /* VECTOR_IMPLEMENTATION_ */
#endif /* VECTOR_IMPLEMENTATION */

#ifdef QUAT_IMPLEMENTATION
#ifndef QUAT_IMPLEMENTATION_
#define QUAT_IMPLEMENTATION_

/* Newton-Raphson sqrt + range-reduced Taylor-series sin/cos, replacing
 * math.h's - own local copies (not shared with vector's sqrt or
 * matrix's sin/cos) matching this project's standing "no cross-
 * IMPLEMENTATION-guard dependency" rule. Taylor truncated at the x^13
 * (sin) / x^12 (cos) term after reducing to [-pi,pi] - remainder bound
 * is ~2e-6 at the range edges, well under this project's usual 1e-4
 * test tolerance. */
static f64 quat_sqrt_(f64 x)
{
    f64 guess;
    i32 i;
    if (x <= 0.0) return 0.0;
    guess = (x > 1.0) ? x : 1.0;
    for (i = 0; i < 40; i++) {
        f64 next = 0.5 * (guess + x / guess);
        if (next == guess) break;
        guess = next;
    }
    return guess;
}

static f64 quat_mod_2pi_(f64 x)
{
    const f64 two_pi = 6.283185307179586476925286766559;
    const f64 pi = 3.14159265358979323846;
    f64 n = (f64)(i64)(x / two_pi);
    f64 r = x - n * two_pi;
    if (r > pi) r -= two_pi;
    if (r < -pi) r += two_pi;
    return r;
}

static f64 quat_sin_(f64 x)
{
    f64 r = quat_mod_2pi_(x);
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

static f64 quat_cos_(f64 x)
{
    f64 r = quat_mod_2pi_(x);
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

Quat quat_identity(void) { return quat_make(0.0f, 0.0f, 0.0f, 1.0f); }

Quat quat_make(f32 x, f32 y, f32 z, f32 w)
{
    Quat q;
    q.x = x; q.y = y; q.z = z; q.w = w;
    return q;
}

Quat quat_from_axis_angle(vec3 axis, f32 angle_rad)
{
    f32 len = vec3_len(axis);
    f32 half, s;

    if (len == 0.0f) return quat_identity();

    half = angle_rad * 0.5f;
    s = (f32)quat_sin_((f64)half);
    return quat_make(
        (axis.x / len) * s,
        (axis.y / len) * s,
        (axis.z / len) * s,
        (f32)quat_cos_((f64)half)
    );
}

Quat quat_mul(Quat a, Quat b)
{
    return quat_make(
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    );
}

Quat quat_conj(Quat q) { return quat_make(-q.x, -q.y, -q.z, q.w); }
f32 quat_dot(Quat a, Quat b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
f32 quat_len(Quat q) { return (f32)quat_sqrt_((f64)quat_dot(q, q)); }

Quat quat_normalize(Quat q)
{
    f32 len = quat_len(q);
    if (len == 0.0f) return quat_identity();
    return quat_make(q.x / len, q.y / len, q.z / len, q.w / len);
}

vec3 quat_mul_vec3(Quat q, vec3 v)
{
    vec3 qv = vec3_make(q.x, q.y, q.z);
    vec3 t = vec3_scale(vec3_cross(qv, v), 2.0f);
    vec3 t2 = vec3_cross(qv, t);
    return vec3_add(vec3_add(v, vec3_scale(t, q.w)), t2);
}

#endif /* QUAT_IMPLEMENTATION_ */
#endif /* QUAT_IMPLEMENTATION */

#ifdef MATRIX_IMPLEMENTATION
#ifndef MATRIX_IMPLEMENTATION_
#define MATRIX_IMPLEMENTATION_

/* Range-reduced Taylor-series sin/cos, replacing math.h's - own local
 * copies (not shared with quat's own copies above) matching this
 * project's standing "no cross-IMPLEMENTATION-guard dependency" rule.
 * See quat's own copy for the accuracy/truncation rationale. */
static f64 matrix_mod_2pi_(f64 x)
{
    const f64 two_pi = 6.283185307179586476925286766559;
    const f64 pi = 3.14159265358979323846;
    f64 n = (f64)(i64)(x / two_pi);
    f64 r = x - n * two_pi;
    if (r > pi) r -= two_pi;
    if (r < -pi) r += two_pi;
    return r;
}

static f64 matrix_sin_(f64 x)
{
    f64 r = matrix_mod_2pi_(x);
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

static f64 matrix_cos_(f64 x)
{
    f64 r = matrix_mod_2pi_(x);
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

Mat4 mat4_identity(void)
{
    Mat4 m;
    m.e[0][0] = 1.0f; m.e[0][1] = 0.0f; m.e[0][2] = 0.0f; m.e[0][3] = 0.0f;
    m.e[1][0] = 0.0f; m.e[1][1] = 1.0f; m.e[1][2] = 0.0f; m.e[1][3] = 0.0f;
    m.e[2][0] = 0.0f; m.e[2][1] = 0.0f; m.e[2][2] = 1.0f; m.e[2][3] = 0.0f;
    m.e[3][0] = 0.0f; m.e[3][1] = 0.0f; m.e[3][2] = 0.0f; m.e[3][3] = 1.0f;
    return m;
}

Mat4 mat4_mul(Mat4 a, Mat4 b)
{
    Mat4 result;

    result.e[0][0] = a.e[0][0] * b.e[0][0] + a.e[1][0] * b.e[0][1] + a.e[2][0] * b.e[0][2] + a.e[3][0] * b.e[0][3];
    result.e[0][1] = a.e[0][1] * b.e[0][0] + a.e[1][1] * b.e[0][1] + a.e[2][1] * b.e[0][2] + a.e[3][1] * b.e[0][3];
    result.e[0][2] = a.e[0][2] * b.e[0][0] + a.e[1][2] * b.e[0][1] + a.e[2][2] * b.e[0][2] + a.e[3][2] * b.e[0][3];
    result.e[0][3] = a.e[0][3] * b.e[0][0] + a.e[1][3] * b.e[0][1] + a.e[2][3] * b.e[0][2] + a.e[3][3] * b.e[0][3];

    result.e[1][0] = a.e[0][0] * b.e[1][0] + a.e[1][0] * b.e[1][1] + a.e[2][0] * b.e[1][2] + a.e[3][0] * b.e[1][3];
    result.e[1][1] = a.e[0][1] * b.e[1][0] + a.e[1][1] * b.e[1][1] + a.e[2][1] * b.e[1][2] + a.e[3][1] * b.e[1][3];
    result.e[1][2] = a.e[0][2] * b.e[1][0] + a.e[1][2] * b.e[1][1] + a.e[2][2] * b.e[1][2] + a.e[3][2] * b.e[1][3];
    result.e[1][3] = a.e[0][3] * b.e[1][0] + a.e[1][3] * b.e[1][1] + a.e[2][3] * b.e[1][2] + a.e[3][3] * b.e[1][3];

    result.e[2][0] = a.e[0][0] * b.e[2][0] + a.e[1][0] * b.e[2][1] + a.e[2][0] * b.e[2][2] + a.e[3][0] * b.e[2][3];
    result.e[2][1] = a.e[0][1] * b.e[2][0] + a.e[1][1] * b.e[2][1] + a.e[2][1] * b.e[2][2] + a.e[3][1] * b.e[2][3];
    result.e[2][2] = a.e[0][2] * b.e[2][0] + a.e[1][2] * b.e[2][1] + a.e[2][2] * b.e[2][2] + a.e[3][2] * b.e[2][3];
    result.e[2][3] = a.e[0][3] * b.e[2][0] + a.e[1][3] * b.e[2][1] + a.e[2][3] * b.e[2][2] + a.e[3][3] * b.e[2][3];

    result.e[3][0] = a.e[0][0] * b.e[3][0] + a.e[1][0] * b.e[3][1] + a.e[2][0] * b.e[3][2] + a.e[3][0] * b.e[3][3];
    result.e[3][1] = a.e[0][1] * b.e[3][0] + a.e[1][1] * b.e[3][1] + a.e[2][1] * b.e[3][2] + a.e[3][1] * b.e[3][3];
    result.e[3][2] = a.e[0][2] * b.e[3][0] + a.e[1][2] * b.e[3][1] + a.e[2][2] * b.e[3][2] + a.e[3][2] * b.e[3][3];
    result.e[3][3] = a.e[0][3] * b.e[3][0] + a.e[1][3] * b.e[3][1] + a.e[2][3] * b.e[3][2] + a.e[3][3] * b.e[3][3];

    return result;
}

vec4 mat4_mul_vec4(Mat4 m, vec4 v)
{
    vec4 in;
    in.x = v.x; in.y = v.y; in.z = v.z; in.w = v.w;
    return vec4_make(
        m.e[0][0] * in.x + m.e[1][0] * in.y + m.e[2][0] * in.z + m.e[3][0] * in.w,
        m.e[0][1] * in.x + m.e[1][1] * in.y + m.e[2][1] * in.z + m.e[3][1] * in.w,
        m.e[0][2] * in.x + m.e[1][2] * in.y + m.e[2][2] * in.z + m.e[3][2] * in.w,
        m.e[0][3] * in.x + m.e[1][3] * in.y + m.e[2][3] * in.z + m.e[3][3] * in.w
    );
}

Mat4 mat4_translate(vec3 t)
{
    Mat4 m = mat4_identity();
    m.e[3][0] = t.x;
    m.e[3][1] = t.y;
    m.e[3][2] = t.z;
    return m;
}

Mat4 mat4_scale(vec3 s)
{
    Mat4 m = mat4_identity();
    m.e[0][0] = s.x;
    m.e[1][1] = s.y;
    m.e[2][2] = s.z;
    return m;
}

Mat4 mat4_rotate(vec3 axis, f32 angle_rad)
{
    Mat4 m = mat4_identity();
    f32 len = vec3_len(axis);
    f32 x, y, z, s, c, one_minus_c;

    if (len == 0.0f) return m; /* degenerate axis - identity is the only sane answer */

    x = axis.x / len;
    y = axis.y / len;
    z = axis.z / len;
    s = (f32)matrix_sin_((f64)angle_rad);
    c = (f32)matrix_cos_((f64)angle_rad);
    one_minus_c = 1.0f - c;

    m.e[0][0] = c + x * x * one_minus_c;
    m.e[0][1] = y * x * one_minus_c + z * s;
    m.e[0][2] = z * x * one_minus_c - y * s;

    m.e[1][0] = x * y * one_minus_c - z * s;
    m.e[1][1] = c + y * y * one_minus_c;
    m.e[1][2] = z * y * one_minus_c + x * s;

    m.e[2][0] = x * z * one_minus_c + y * s;
    m.e[2][1] = y * z * one_minus_c - x * s;
    m.e[2][2] = c + z * z * one_minus_c;

    return m;
}

Mat4 mat4_rotate_x(f32 angle_rad)
{
    Mat4 m = mat4_identity();
    f32 s = (f32)matrix_sin_((f64)angle_rad);
    f32 c = (f32)matrix_cos_((f64)angle_rad);
    m.e[1][1] = c; m.e[2][1] = -s;
    m.e[1][2] = s; m.e[2][2] = c;
    return m;
}

Mat4 mat4_rotate_y(f32 angle_rad)
{
    Mat4 m = mat4_identity();
    f32 s = (f32)matrix_sin_((f64)angle_rad);
    f32 c = (f32)matrix_cos_((f64)angle_rad);
    m.e[0][0] = c;  m.e[2][0] = s;
    m.e[0][2] = -s; m.e[2][2] = c;
    return m;
}

Mat4 mat4_rotate_z(f32 angle_rad)
{
    Mat4 m = mat4_identity();
    f32 s = (f32)matrix_sin_((f64)angle_rad);
    f32 c = (f32)matrix_cos_((f64)angle_rad);
    m.e[0][0] = c; m.e[1][0] = -s;
    m.e[0][1] = s; m.e[1][1] = c;
    return m;
}

Mat4 mat4_transpose(Mat4 m)
{
    Mat4 result;
    result.e[0][0] = m.e[0][0]; result.e[0][1] = m.e[1][0]; result.e[0][2] = m.e[2][0]; result.e[0][3] = m.e[3][0];
    result.e[1][0] = m.e[0][1]; result.e[1][1] = m.e[1][1]; result.e[1][2] = m.e[2][1]; result.e[1][3] = m.e[3][1];
    result.e[2][0] = m.e[0][2]; result.e[2][1] = m.e[1][2]; result.e[2][2] = m.e[2][2]; result.e[2][3] = m.e[3][2];
    result.e[3][0] = m.e[0][3]; result.e[3][1] = m.e[1][3]; result.e[3][2] = m.e[2][3]; result.e[3][3] = m.e[3][3];
    return result;
}

Mat4 mat4_from_quat(Quat q)
{
    Mat4 m = mat4_identity();
    f32 x = q.x, y = q.y, z = q.z, w = q.w;
    f32 x2 = x + x, y2 = y + y, z2 = z + z;
    f32 xx = x * x2, xy = x * y2, xz = x * z2;
    f32 yy = y * y2, yz = y * z2, zz = z * z2;
    f32 wx = w * x2, wy = w * y2, wz = w * z2;

    m.e[0][0] = 1.0f - (yy + zz);
    m.e[0][1] = xy + wz;
    m.e[0][2] = xz - wy;

    m.e[1][0] = xy - wz;
    m.e[1][1] = 1.0f - (xx + zz);
    m.e[1][2] = yz + wx;

    m.e[2][0] = xz + wy;
    m.e[2][1] = yz - wx;
    m.e[2][2] = 1.0f - (xx + yy);

    return m;
}

#endif /* MATRIX_IMPLEMENTATION_ */
#endif /* MATRIX_IMPLEMENTATION */
