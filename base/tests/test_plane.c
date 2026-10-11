#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define PLANE_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "geometry.h"

int main(void)
{
    print_cstr("test_plane\n");

    /* the XY plane (z=0), built from 3 points on it */
    {
        vec3 a = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 b = vec3_make(1.0f, 0.0f, 0.0f);
        vec3 c = vec3_make(0.0f, 1.0f, 0.0f);
        Plane p = plane_from_points(a, b, c);

        TEST_CHECK(p.normal.x > -0.001f && p.normal.x < 0.001f &&
                   p.normal.y > -0.001f && p.normal.y < 0.001f,
                   "plane_from_points: XY plane normal has zero x,y");
        {
            f32 nz = p.normal.z;
            if (nz < 0.0f) nz = -nz;
            TEST_CHECK(nz > 0.999f && nz < 1.001f, "plane_from_points: XY plane normal z is unit length");
        }
    }

    /* plane from normal+point: the plane z=5, normal (0,0,1) */
    {
        Plane p = plane_from_normal_point(vec3_make(0.0f, 0.0f, 1.0f), vec3_make(3.0f, -2.0f, 5.0f));
        f32 d1 = plane_signed_distance(p, vec3_make(0.0f, 0.0f, 5.0f));
        f32 d2 = plane_signed_distance(p, vec3_make(0.0f, 0.0f, 8.0f));
        f32 d3 = plane_signed_distance(p, vec3_make(0.0f, 0.0f, 2.0f));

        TEST_CHECK(d1 > -0.001f && d1 < 0.001f, "plane_signed_distance: a point ON the plane is ~0");
        TEST_CHECK(d2 > 2.999f && d2 < 3.001f, "plane_signed_distance: a point 3 units in front is +3");
        TEST_CHECK(d3 > -3.001f && d3 < -2.999f, "plane_signed_distance: a point 3 units behind is -3");
    }

    /* classification */
    {
        Plane p = plane_from_normal_point(vec3_make(0.0f, 1.0f, 0.0f), vec3_make(0.0f, 0.0f, 0.0f));
        TEST_CHECK(plane_classify(p, vec3_make(0.0f, 5.0f, 0.0f)) == PLANE_FRONT, "plane_classify: above the plane is FRONT");
        TEST_CHECK(plane_classify(p, vec3_make(0.0f, -5.0f, 0.0f)) == PLANE_BACK, "plane_classify: below the plane is BACK");
        TEST_CHECK(plane_classify(p, vec3_make(3.0f, 0.0f, -2.0f)) == PLANE_ON, "plane_classify: a point ON the plane is ON");
    }

    print_flush();
    return test_failures_ != 0;
}
