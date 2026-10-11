#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define GJK_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "gjk.h"
#include <math.h>

/* a sphere's support function: the point on its surface furthest in a
   given direction is trivially center + radius*normalize(direction) -
   the simplest possible convex shape to test GJK against, since the
   correct overlap/no-overlap answer is unambiguous from the center
   distance vs radius sum, giving a robust ground truth. */
typedef struct { vec3 center; f32 radius; } Test_Sphere_;

static vec3 sphere_support_(void *shape, vec3 dir)
{
    Test_Sphere_ *s = (Test_Sphere_ *)shape;
    f32 len = (f32)sqrt((f64)vec3_dot(dir, dir));
    vec3 n;
    if (len < 0.000001f) return s->center;
    n = vec3_scale(dir, 1.0f / len);
    return vec3_add(s->center, vec3_scale(n, s->radius));
}

/* a box's support function: the point on its surface furthest in a
   given direction is trivially the corner whose sign matches dir's
   sign per axis - the classic AABB support function, another simple
   robust ground truth (box-box and box-sphere overlap are both
   trivially checkable by hand). */
typedef struct { vec3 min, max; } Test_Box_;

static vec3 box_support_(void *shape, vec3 dir)
{
    Test_Box_ *b = (Test_Box_ *)shape;
    vec3 out;
    out.x = dir.x >= 0.0f ? b->max.x : b->min.x;
    out.y = dir.y >= 0.0f ? b->max.y : b->min.y;
    out.z = dir.z >= 0.0f ? b->max.z : b->min.z;
    return out;
}

int main(void)
{
    print_cstr("test_gjk\n");

    /* two clearly overlapping spheres */
    {
        Test_Sphere_ a, b;
        i32 hit;
        a.center = vec3_make(0.0f, 0.0f, 0.0f); a.radius = 2.0f;
        b.center = vec3_make(1.0f, 0.0f, 0.0f); b.radius = 2.0f;
        hit = gjk_intersect(sphere_support_, &a, sphere_support_, &b, NULL);
        TEST_CHECK(hit == 1, "gjk_intersect: two clearly overlapping spheres report a hit");
    }

    /* two clearly separate spheres */
    {
        Test_Sphere_ a, b;
        i32 hit;
        a.center = vec3_make(0.0f, 0.0f, 0.0f); a.radius = 1.0f;
        b.center = vec3_make(10.0f, 0.0f, 0.0f); b.radius = 1.0f;
        hit = gjk_intersect(sphere_support_, &a, sphere_support_, &b, NULL);
        TEST_CHECK(hit == 0, "gjk_intersect: two clearly separate spheres report no hit");
    }

    /* two spheres exactly touching (distance == r1+r2) - boundary case */
    {
        Test_Sphere_ a, b;
        i32 hit;
        a.center = vec3_make(0.0f, 0.0f, 0.0f); a.radius = 3.0f;
        b.center = vec3_make(6.0f, 0.0f, 0.0f); b.radius = 3.0f;
        hit = gjk_intersect(sphere_support_, &a, sphere_support_, &b, NULL);
        TEST_CHECK(hit == 1, "gjk_intersect: two spheres exactly touching report a hit (shared boundary point)");
    }

    /* two overlapping boxes */
    {
        Test_Box_ a, b;
        i32 hit;
        a.min = vec3_make(0.0f, 0.0f, 0.0f); a.max = vec3_make(2.0f, 2.0f, 2.0f);
        b.min = vec3_make(1.0f, 1.0f, 1.0f); b.max = vec3_make(3.0f, 3.0f, 3.0f);
        hit = gjk_intersect(box_support_, &a, box_support_, &b, NULL);
        TEST_CHECK(hit == 1, "gjk_intersect: two overlapping boxes report a hit");
    }

    /* two separate boxes */
    {
        Test_Box_ a, b;
        i32 hit;
        a.min = vec3_make(0.0f, 0.0f, 0.0f); a.max = vec3_make(1.0f, 1.0f, 1.0f);
        b.min = vec3_make(5.0f, 5.0f, 5.0f); b.max = vec3_make(6.0f, 6.0f, 6.0f);
        hit = gjk_intersect(box_support_, &a, box_support_, &b, NULL);
        TEST_CHECK(hit == 0, "gjk_intersect: two separate boxes report no hit");
    }

    /* a small sphere entirely inside a large box (one shape fully containing the other) */
    {
        Test_Sphere_ a;
        Test_Box_ b;
        i32 hit;
        a.center = vec3_make(5.0f, 5.0f, 5.0f); a.radius = 0.5f;
        b.min = vec3_make(0.0f, 0.0f, 0.0f); b.max = vec3_make(10.0f, 10.0f, 10.0f);
        hit = gjk_intersect(sphere_support_, &a, box_support_, &b, NULL);
        TEST_CHECK(hit == 1, "gjk_intersect: one shape entirely containing another still reports a hit");
    }

    print_flush();
    return test_failures_ != 0;
}
