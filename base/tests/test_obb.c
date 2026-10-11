#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define QUAT_IMPLEMENTATION
#define OBB_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "obb.h"

int main(void)
{
    print_cstr("test_obb\n");

    /* two axis-aligned (identity orientation) boxes that overlap */
    {
        Obb a, b;
        a.center = vec3_make(0.0f, 0.0f, 0.0f);
        a.orientation = quat_identity();
        a.half_extents = vec3_make(5.0f, 5.0f, 5.0f);

        b.center = vec3_make(8.0f, 0.0f, 0.0f);
        b.orientation = quat_identity();
        b.half_extents = vec3_make(5.0f, 5.0f, 5.0f);

        TEST_CHECK(obb_intersects_obb(a, b) == 1, "two axis-aligned overlapping boxes (centers 8 apart, half-extents 5) intersect");
    }

    /* two axis-aligned boxes far apart */
    {
        Obb a, b;
        a.center = vec3_make(0.0f, 0.0f, 0.0f);
        a.orientation = quat_identity();
        a.half_extents = vec3_make(5.0f, 5.0f, 5.0f);

        b.center = vec3_make(30.0f, 0.0f, 0.0f);
        b.orientation = quat_identity();
        b.half_extents = vec3_make(5.0f, 5.0f, 5.0f);

        TEST_CHECK(obb_intersects_obb(a, b) == 0, "two axis-aligned far-apart boxes (centers 30 apart, half-extents 5) don't intersect");
    }

    /* a box rotated 45 degrees around Z that just barely reaches another box
       via its corner - a real case where axis-aligned overlap tests alone
       would get the wrong answer, exercising the actual SAT cross-product axes */
    {
        Obb a, b;
        a.center = vec3_make(0.0f, 0.0f, 0.0f);
        a.orientation = quat_identity();
        a.half_extents = vec3_make(5.0f, 5.0f, 5.0f);

        /* b is rotated 45deg around Z, placed along the diagonal direction
           where a's corner would reach - its own axis-aligned half-extents
           projected onto that diagonal are smaller than an unrotated box's
           would be, a case where SAT's rotated-box axes matter */
        b.center = vec3_make(11.5f, 11.5f, 0.0f);
        b.orientation = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), 0.7853981f);
        b.half_extents = vec3_make(5.0f, 5.0f, 5.0f);

        TEST_CHECK(obb_intersects_obb(a, b) == 0, "a rotated box placed just out of reach along the diagonal doesn't intersect");
    }

    /* point containment */
    {
        Obb box;
        box.center = vec3_make(0.0f, 0.0f, 0.0f);
        box.orientation = quat_identity();
        box.half_extents = vec3_make(5.0f, 5.0f, 5.0f);

        TEST_CHECK(obb_contains_point(box, vec3_make(3.0f, 3.0f, 3.0f)) == 1, "obb_contains_point: a point inside an axis-aligned OBB is contained");
        TEST_CHECK(obb_contains_point(box, vec3_make(10.0f, 3.0f, 3.0f)) == 0, "obb_contains_point: a point outside is not contained");
    }

    /* point containment against a rotated box: a point that's outside the
       AXIS-ALIGNED extents but inside once you account for rotation */
    {
        Obb box;
        vec3 p;
        box.center = vec3_make(0.0f, 0.0f, 0.0f);
        box.orientation = quat_from_axis_angle(vec3_make(0.0f, 0.0f, 1.0f), 0.7853981f); /* 45deg */
        box.half_extents = vec3_make(5.0f, 1.0f, 5.0f);

        /* a point along the box's own (rotated) long axis, at distance ~4
           from center in local X - well within half-extent 5 */
        p = vec3_make(2.828f, 2.828f, 0.0f); /* ~4 * (cos45, sin45, 0) */
        TEST_CHECK(obb_contains_point(box, p) == 1, "obb_contains_point correctly accounts for the box's own rotation, not just world axes");
    }

    print_flush();
    return test_failures_ != 0;
}
