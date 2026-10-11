#ifndef HOLYC_OBB_H_
#define HOLYC_OBB_H_

/* Oriented Bounding Box - center + orientation (a Quat, since
 * quat_mul_vec3 already exists to rotate the local axes/points on
 * demand, no need to also cache a 3x3 rotation matrix) + half-extents
 * along the box's own local axes.
 *
 * obb_intersects_obb is the Separating Axis Theorem (SAT) test for two
 * OBBs (Gottschalk et al. / Ericson, Real-Time Collision Detection
 * section 4.4) - 15 candidate separating axes total: each box's own 3
 * local axes (6), plus the 9 cross products of one box's axis with the
 * other's. If no candidate axis separates the two boxes' projected
 * intervals, they overlap. The exact per-axis formulas (which
 * half-extents get multiplied by which |R[i][j]| term) are the
 * standard, well-documented ones from that algorithm - not derived or
 * approximated here, transcribed carefully and cross-checked against
 * the test cases in tests/test_obb.c, including a genuinely rotated
 * case that specifically exercises the 9 cross-product axes (an
 * axis-aligned-only test would get that case wrong). */

#include "base.h"
#include "mathx.h"

typedef struct {
    vec3 center;
    Quat orientation;
    vec3 half_extents;
} Obb;

/* builds an OBB from an axis-aligned box (given as a center + half-
 * extents in its own local space, e.g. from an existing Aabb's
 * midpoint/half-size) plus a world transform (translation + rotation) -
 * deliberately takes plain vec3/Quat parameters rather than depending
 * on aabb_collision.h's Aabb type, keeping this file independent of
 * it (same "minimal, independent" split this project already applies
 * to vector.h/matrix.h/quat.h). */
Obb obb_from_aabb_transform(vec3 aabb_center, vec3 aabb_half_extents, vec3 translation, Quat rotation);

i32 obb_contains_point(Obb box, vec3 p);
i32 obb_intersects_obb(Obb a, Obb b);

#endif /* HOLYC_OBB_H_ */

#ifdef OBB_IMPLEMENTATION
#ifndef OBB_IMPLEMENTATION_
#define OBB_IMPLEMENTATION_

#ifndef OBB_EPSILON
#define OBB_EPSILON 0.000001f
#endif

static f32 obb_fabsf_(f32 x) { return x < 0.0f ? -x : x; }

Obb obb_from_aabb_transform(vec3 aabb_center, vec3 aabb_half_extents, vec3 translation, Quat rotation)
{
    Obb box;
    box.center = vec3_add(translation, quat_mul_vec3(rotation, aabb_center));
    box.orientation = rotation;
    box.half_extents = aabb_half_extents;
    return box;
}

i32 obb_contains_point(Obb box, vec3 p)
{
    Quat inv = quat_conj(box.orientation); /* conjugate of a unit quaternion is its inverse */
    vec3 local = quat_mul_vec3(inv, vec3_sub(p, box.center));

    return (obb_fabsf_(local.x) <= box.half_extents.x &&
            obb_fabsf_(local.y) <= box.half_extents.y &&
            obb_fabsf_(local.z) <= box.half_extents.z) ? 1 : 0;
}

i32 obb_intersects_obb(Obb a, Obb b)
{
    vec3 a_axis[3], b_axis[3];
    f32 r[3][3], abs_r[3][3];
    vec3 t_world;
    f32 t[3];
    f32 ea[3], eb[3];
    f32 ra, rb;
    i32 i, j;

    a_axis[0] = quat_mul_vec3(a.orientation, vec3_make(1.0f, 0.0f, 0.0f));
    a_axis[1] = quat_mul_vec3(a.orientation, vec3_make(0.0f, 1.0f, 0.0f));
    a_axis[2] = quat_mul_vec3(a.orientation, vec3_make(0.0f, 0.0f, 1.0f));
    b_axis[0] = quat_mul_vec3(b.orientation, vec3_make(1.0f, 0.0f, 0.0f));
    b_axis[1] = quat_mul_vec3(b.orientation, vec3_make(0.0f, 1.0f, 0.0f));
    b_axis[2] = quat_mul_vec3(b.orientation, vec3_make(0.0f, 0.0f, 1.0f));

    ea[0] = a.half_extents.x; ea[1] = a.half_extents.y; ea[2] = a.half_extents.z;
    eb[0] = b.half_extents.x; eb[1] = b.half_extents.y; eb[2] = b.half_extents.z;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            r[i][j] = vec3_dot(a_axis[i], b_axis[j]);

    t_world = vec3_sub(b.center, a.center);
    for (i = 0; i < 3; i++) t[i] = vec3_dot(t_world, a_axis[i]);

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            abs_r[i][j] = obb_fabsf_(r[i][j]) + OBB_EPSILON;

    /* A's own 3 axes */
    for (i = 0; i < 3; i++) {
        ra = ea[i];
        rb = eb[0] * abs_r[i][0] + eb[1] * abs_r[i][1] + eb[2] * abs_r[i][2];
        if (obb_fabsf_(t[i]) > ra + rb) return 0;
    }

    /* B's own 3 axes */
    for (j = 0; j < 3; j++) {
        f32 t_proj;
        ra = ea[0] * abs_r[0][j] + ea[1] * abs_r[1][j] + ea[2] * abs_r[2][j];
        rb = eb[j];
        t_proj = obb_fabsf_(t[0] * r[0][j] + t[1] * r[1][j] + t[2] * r[2][j]);
        if (t_proj > ra + rb) return 0;
    }

    /* 9 cross-product axes: Ai x Bj */
    ra = ea[1] * abs_r[2][0] + ea[2] * abs_r[1][0];
    rb = eb[1] * abs_r[0][2] + eb[2] * abs_r[0][1];
    if (obb_fabsf_(t[2] * r[1][0] - t[1] * r[2][0]) > ra + rb) return 0;

    ra = ea[1] * abs_r[2][1] + ea[2] * abs_r[1][1];
    rb = eb[0] * abs_r[0][2] + eb[2] * abs_r[0][0];
    if (obb_fabsf_(t[2] * r[1][1] - t[1] * r[2][1]) > ra + rb) return 0;

    ra = ea[1] * abs_r[2][2] + ea[2] * abs_r[1][2];
    rb = eb[0] * abs_r[0][1] + eb[1] * abs_r[0][0];
    if (obb_fabsf_(t[2] * r[1][2] - t[1] * r[2][2]) > ra + rb) return 0;

    ra = ea[0] * abs_r[2][0] + ea[2] * abs_r[0][0];
    rb = eb[1] * abs_r[1][2] + eb[2] * abs_r[1][1];
    if (obb_fabsf_(t[0] * r[2][0] - t[2] * r[0][0]) > ra + rb) return 0;

    ra = ea[0] * abs_r[2][1] + ea[2] * abs_r[0][1];
    rb = eb[0] * abs_r[1][2] + eb[2] * abs_r[1][0];
    if (obb_fabsf_(t[0] * r[2][1] - t[2] * r[0][1]) > ra + rb) return 0;

    ra = ea[0] * abs_r[2][2] + ea[2] * abs_r[0][2];
    rb = eb[0] * abs_r[1][1] + eb[1] * abs_r[1][0];
    if (obb_fabsf_(t[0] * r[2][2] - t[2] * r[0][2]) > ra + rb) return 0;

    ra = ea[0] * abs_r[1][0] + ea[1] * abs_r[0][0];
    rb = eb[1] * abs_r[2][2] + eb[2] * abs_r[2][1];
    if (obb_fabsf_(t[1] * r[0][0] - t[0] * r[1][0]) > ra + rb) return 0;

    ra = ea[0] * abs_r[1][1] + ea[1] * abs_r[0][1];
    rb = eb[0] * abs_r[2][2] + eb[2] * abs_r[2][0];
    if (obb_fabsf_(t[1] * r[0][1] - t[0] * r[1][1]) > ra + rb) return 0;

    ra = ea[0] * abs_r[1][2] + ea[1] * abs_r[0][2];
    rb = eb[0] * abs_r[2][1] + eb[1] * abs_r[2][0];
    if (obb_fabsf_(t[1] * r[0][2] - t[0] * r[1][2]) > ra + rb) return 0;

    return 1; /* no separating axis found */
}

#endif /* OBB_IMPLEMENTATION_ */
#endif /* OBB_IMPLEMENTATION */
