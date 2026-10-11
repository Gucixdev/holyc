#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define SPHERE_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "geometry.h"

int main(void)
{
    print_cstr("test_sphere\n");

    /* sphere_from_points: 4 points around the origin, radius should be
       at least the max distance from the centroid to any point */
    {
        vec3 pts[4];
        Sphere s;
        f32 dist_to_farthest;
        pts[0] = vec3_make(10.0f, 0.0f, 0.0f);
        pts[1] = vec3_make(-10.0f, 0.0f, 0.0f);
        pts[2] = vec3_make(0.0f, 10.0f, 0.0f);
        pts[3] = vec3_make(0.0f, -10.0f, 0.0f);

        s = sphere_from_points(pts, 4);
        TEST_CHECK(s.center.x > -0.001f && s.center.x < 0.001f &&
                   s.center.y > -0.001f && s.center.y < 0.001f,
                   "sphere_from_points: centroid of 4 symmetric points is the origin");
        TEST_CHECK(s.radius > 9.999f && s.radius < 10.001f, "sphere_from_points: radius covers the farthest point exactly (10)");

        dist_to_farthest = vec3_len(vec3_sub(pts[0], s.center));
        TEST_CHECK(dist_to_farthest <= s.radius + 0.001f, "sphere_from_points: every input point is within radius of center");
    }

    /* point containment */
    {
        Sphere s;
        s.center = vec3_make(0.0f, 0.0f, 0.0f);
        s.radius = 5.0f;

        TEST_CHECK(sphere_contains_point(s, vec3_make(3.0f, 0.0f, 0.0f)) == 1, "sphere_contains_point: a point well inside is contained");
        TEST_CHECK(sphere_contains_point(s, vec3_make(5.0f, 0.0f, 0.0f)) == 1, "sphere_contains_point: a point exactly on the surface is contained");
        TEST_CHECK(sphere_contains_point(s, vec3_make(10.0f, 0.0f, 0.0f)) == 0, "sphere_contains_point: a point well outside is not contained");
    }

    /* sphere-sphere intersection */
    {
        Sphere a, b, c;
        a.center = vec3_make(0.0f, 0.0f, 0.0f); a.radius = 5.0f;
        b.center = vec3_make(8.0f, 0.0f, 0.0f); b.radius = 5.0f; /* distance 8 < 5+5=10, overlapping */
        c.center = vec3_make(20.0f, 0.0f, 0.0f); c.radius = 5.0f; /* distance 20 > 5+5=10, separate */

        TEST_CHECK(sphere_intersects_sphere(a, b) == 1, "sphere_intersects_sphere: overlapping spheres (dist 8 < sum of radii 10)");
        TEST_CHECK(sphere_intersects_sphere(a, c) == 0, "sphere_intersects_sphere: far-apart spheres (dist 20 > sum of radii 10) don't intersect");
    }

    print_flush();
    return test_failures_ != 0;
}
