#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define SPHERE_IMPLEMENTATION
#define AABB_COLLISION_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "geometry.h"
#include "aabb_collision.h"

int main(void)
{
    print_cstr("test_aabb_collision\n");

    /* aabb_from_points */
    {
        vec3 pts[3];
        Aabb box;
        pts[0] = vec3_make(1.0f, -2.0f, 3.0f);
        pts[1] = vec3_make(-5.0f, 4.0f, 0.0f);
        pts[2] = vec3_make(2.0f, 1.0f, -6.0f);

        box = aabb_from_points(pts, 3);
        TEST_CHECK(box.min.x == -5.0f && box.min.y == -2.0f && box.min.z == -6.0f, "aabb_from_points: min is the component-wise minimum");
        TEST_CHECK(box.max.x == 2.0f && box.max.y == 4.0f && box.max.z == 3.0f, "aabb_from_points: max is the component-wise maximum");
    }

    /* aabb-aabb intersection */
    {
        Aabb a, b, c;
        a.min = vec3_make(0.0f, 0.0f, 0.0f); a.max = vec3_make(10.0f, 10.0f, 10.0f);
        b.min = vec3_make(5.0f, 5.0f, 5.0f); b.max = vec3_make(15.0f, 15.0f, 15.0f); /* overlaps a */
        c.min = vec3_make(20.0f, 20.0f, 20.0f); c.max = vec3_make(30.0f, 30.0f, 30.0f); /* separate from a */

        TEST_CHECK(aabb_intersects_aabb(a, b) == 1, "aabb_intersects_aabb: overlapping boxes intersect");
        TEST_CHECK(aabb_intersects_aabb(a, c) == 0, "aabb_intersects_aabb: far-apart boxes don't intersect");
    }

    /* aabb-point containment */
    {
        Aabb box;
        box.min = vec3_make(0.0f, 0.0f, 0.0f);
        box.max = vec3_make(10.0f, 10.0f, 10.0f);

        TEST_CHECK(aabb_contains_point(box, vec3_make(5.0f, 5.0f, 5.0f)) == 1, "aabb_contains_point: a point inside is contained");
        TEST_CHECK(aabb_contains_point(box, vec3_make(0.0f, 0.0f, 0.0f)) == 1, "aabb_contains_point: a point exactly on the boundary is contained");
        TEST_CHECK(aabb_contains_point(box, vec3_make(15.0f, 5.0f, 5.0f)) == 0, "aabb_contains_point: a point outside is not contained");
    }

    /* aabb-sphere intersection */
    {
        Aabb box;
        Sphere near_sphere, far_sphere;
        box.min = vec3_make(0.0f, 0.0f, 0.0f);
        box.max = vec3_make(10.0f, 10.0f, 10.0f);

        near_sphere.center = vec3_make(12.0f, 5.0f, 5.0f);
        near_sphere.radius = 3.0f; /* closest point on box is (10,5,5), distance 2 < radius 3 */

        far_sphere.center = vec3_make(30.0f, 5.0f, 5.0f);
        far_sphere.radius = 3.0f; /* closest point on box is (10,5,5), distance 20 > radius 3 */

        TEST_CHECK(aabb_intersects_sphere(box, near_sphere) == 1, "aabb_intersects_sphere: sphere close enough to the box surface intersects");
        TEST_CHECK(aabb_intersects_sphere(box, far_sphere) == 0, "aabb_intersects_sphere: sphere far from the box doesn't intersect");
    }

    print_flush();
    return test_failures_ != 0;
}
