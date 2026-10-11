#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define TRIANGLE_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "geometry.h"

int main(void)
{
    print_cstr("test_triangle\n");

    /* a right triangle in the XY plane: (0,0), (4,0), (0,3) - known area 6 */
    {
        Triangle t;
        t.a = vec3_make(0.0f, 0.0f, 0.0f);
        t.b = vec3_make(4.0f, 0.0f, 0.0f);
        t.c = vec3_make(0.0f, 3.0f, 0.0f);

        {
            f32 area = triangle_area(t);
            TEST_CHECK(area > 5.999f && area < 6.001f, "triangle_area of a 4x3 right triangle is 6");
        }

        {
            vec3 centroid = triangle_centroid(t);
            TEST_CHECK(centroid.x > 1.332f && centroid.x < 1.334f &&
                       centroid.y > 0.999f && centroid.y < 1.001f,
                       "triangle_centroid is the average of the 3 vertices (4/3, 1, 0)");
        }

        {
            vec3 n = triangle_normal(t);
            f32 nz = n.z;
            if (nz < 0.0f) nz = -nz;
            TEST_CHECK(nz > 0.999f && nz < 1.001f, "triangle_normal of an XY-plane triangle points along Z, unit length");
        }
    }

    /* barycentric coordinates: at each vertex, that vertex's own weight is 1 */
    {
        Triangle t;
        f32 u, v, w;
        t.a = vec3_make(0.0f, 0.0f, 0.0f);
        t.b = vec3_make(4.0f, 0.0f, 0.0f);
        t.c = vec3_make(0.0f, 4.0f, 0.0f);

        triangle_barycentric(t, t.a, &u, &v, &w);
        TEST_CHECK(u > 0.999f && u < 1.001f && v > -0.001f && v < 0.001f && w > -0.001f && w < 0.001f,
                   "barycentric of vertex a is (1,0,0)");

        triangle_barycentric(t, t.b, &u, &v, &w);
        TEST_CHECK(v > 0.999f && v < 1.001f, "barycentric of vertex b has v~=1");

        /* the centroid has equal barycentric weights (1/3 each) */
        {
            vec3 centroid = triangle_centroid(t);
            triangle_barycentric(t, centroid, &u, &v, &w);
            TEST_CHECK(u > 0.332f && u < 0.334f && v > 0.332f && v < 0.334f && w > 0.332f && w < 0.334f,
                       "barycentric of the centroid is (1/3, 1/3, 1/3)");
        }
    }

    /* point-in-triangle */
    {
        Triangle t;
        t.a = vec3_make(0.0f, 0.0f, 0.0f);
        t.b = vec3_make(4.0f, 0.0f, 0.0f);
        t.c = vec3_make(0.0f, 4.0f, 0.0f);

        TEST_CHECK(triangle_contains_point(t, vec3_make(1.0f, 1.0f, 0.0f)) == 1,
                   "triangle_contains_point: a point well inside the triangle is contained");
        TEST_CHECK(triangle_contains_point(t, vec3_make(10.0f, 10.0f, 0.0f)) == 0,
                   "triangle_contains_point: a point well outside is not contained");
    }

    print_flush();
    return test_failures_ != 0;
}
