#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define CAPSULE_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "geometry.h"

int main(void)
{
    print_cstr("test_capsule\n");

    /* closest point on segment: a capsule along the X axis from 0 to 10 */
    {
        Capsule c;
        vec3 closest;
        c.a = vec3_make(0.0f, 0.0f, 0.0f);
        c.b = vec3_make(10.0f, 0.0f, 0.0f);
        c.radius = 1.0f;

        closest = capsule_closest_point_on_segment(c, vec3_make(5.0f, 3.0f, 0.0f));
        TEST_CHECK(closest.x > 4.999f && closest.x < 5.001f && closest.y > -0.001f && closest.y < 0.001f,
                   "closest point directly above the segment's middle is the middle itself");

        /* a point "before" a should clamp to a */
        closest = capsule_closest_point_on_segment(c, vec3_make(-5.0f, 3.0f, 0.0f));
        TEST_CHECK(closest.x > -0.001f && closest.x < 0.001f, "closest point for a query before the segment clamps to endpoint a");

        /* a point "after" b should clamp to b */
        closest = capsule_closest_point_on_segment(c, vec3_make(15.0f, 3.0f, 0.0f));
        TEST_CHECK(closest.x > 9.999f && closest.x < 10.001f, "closest point for a query past the segment clamps to endpoint b");
    }

    /* point containment */
    {
        Capsule c;
        c.a = vec3_make(0.0f, 0.0f, 0.0f);
        c.b = vec3_make(10.0f, 0.0f, 0.0f);
        c.radius = 2.0f;

        TEST_CHECK(capsule_contains_point(c, vec3_make(5.0f, 1.0f, 0.0f)) == 1, "a point close to the middle of the core segment, within radius, is contained");
        TEST_CHECK(capsule_contains_point(c, vec3_make(5.0f, 5.0f, 0.0f)) == 0, "a point far from the core segment (beyond radius) is not contained");
        TEST_CHECK(capsule_contains_point(c, vec3_make(-1.0f, 1.0f, 0.0f)) == 1,
                   "a point near the ROUNDED end-cap (past endpoint a, but within radius of a) is still contained");
    }

    /* capsule-capsule intersection */
    {
        Capsule a, b, c;
        a.a = vec3_make(0.0f, 0.0f, 0.0f);
        a.b = vec3_make(10.0f, 0.0f, 0.0f);
        a.radius = 1.0f;

        /* b is a parallel segment 1.5 units away in y - radii sum to 2, so they overlap */
        b.a = vec3_make(0.0f, 1.5f, 0.0f);
        b.b = vec3_make(10.0f, 1.5f, 0.0f);
        b.radius = 1.0f;

        /* c is a parallel segment 10 units away in y - far apart, no overlap */
        c.a = vec3_make(0.0f, 10.0f, 0.0f);
        c.b = vec3_make(10.0f, 10.0f, 0.0f);
        c.radius = 1.0f;

        TEST_CHECK(capsule_intersects_capsule(a, b) == 1, "two parallel capsules 1.5 apart with radii summing to 2 intersect");
        TEST_CHECK(capsule_intersects_capsule(a, c) == 0, "two parallel capsules 10 apart with radii summing to 2 do not intersect");
    }

    print_flush();
    return test_failures_ != 0;
}
