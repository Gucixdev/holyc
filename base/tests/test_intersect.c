#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define INTERSECT_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "intersect.h"

int main(void)
{
    print_cstr("test_intersect\n");

    /* 2D segment-segment: an X crossing at a known point */
    {
        vec2 a = vec2_make(0.0f, 0.0f);
        vec2 b = vec2_make(4.0f, 4.0f);
        vec2 c = vec2_make(0.0f, 4.0f);
        vec2 d = vec2_make(4.0f, 0.0f);
        vec2 out;
        i32 hit = intersect_segment_segment_2d(a, b, c, d, &out);
        TEST_CHECK(hit == 1, "intersect_segment_segment_2d: crossing X segments intersect");
        TEST_CHECK(out.x > 1.999f && out.x < 2.001f && out.y > 1.999f && out.y < 2.001f,
                   "intersect_segment_segment_2d: crossing point is (2,2)");
    }

    /* 2D segment-segment: parallel, non-intersecting */
    {
        vec2 a = vec2_make(0.0f, 0.0f);
        vec2 b = vec2_make(4.0f, 0.0f);
        vec2 c = vec2_make(0.0f, 1.0f);
        vec2 d = vec2_make(4.0f, 1.0f);
        vec2 out;
        i32 hit = intersect_segment_segment_2d(a, b, c, d, &out);
        TEST_CHECK(hit == 0, "intersect_segment_segment_2d: parallel segments don't intersect");
    }

    /* 2D segment-segment: would cross if extended, but the actual segments don't reach */
    {
        vec2 a = vec2_make(0.0f, 0.0f);
        vec2 b = vec2_make(1.0f, 1.0f);
        vec2 c = vec2_make(0.0f, 4.0f);
        vec2 d = vec2_make(4.0f, 0.0f);
        vec2 out;
        i32 hit = intersect_segment_segment_2d(a, b, c, d, &out);
        TEST_CHECK(hit == 0, "intersect_segment_segment_2d: segments too short to actually reach the crossing point");
    }

    /* 2D circle-circle: clearly overlapping */
    TEST_CHECK(intersect_circle_circle_2d(vec2_make(0.0f, 0.0f), 5.0f, vec2_make(3.0f, 0.0f), 5.0f) == 1,
               "intersect_circle_circle_2d: overlapping circles");

    /* 2D circle-circle: clearly separate */
    TEST_CHECK(intersect_circle_circle_2d(vec2_make(0.0f, 0.0f), 1.0f, vec2_make(10.0f, 0.0f), 1.0f) == 0,
               "intersect_circle_circle_2d: separate circles don't intersect");

    /* 2D circle-circle: exactly touching (distance == r1+r2) */
    TEST_CHECK(intersect_circle_circle_2d(vec2_make(0.0f, 0.0f), 2.0f, vec2_make(5.0f, 0.0f), 3.0f) == 1,
               "intersect_circle_circle_2d: exactly touching circles count as intersecting");

    /* ray-plane: straight hit */
    {
        vec3 origin = vec3_make(0.0f, 0.0f, -5.0f);
        vec3 dir = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 plane_point = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 plane_normal = vec3_make(0.0f, 0.0f, 1.0f);
        f32 t;
        i32 hit = intersect_ray_plane(origin, dir, plane_point, plane_normal, &t);
        TEST_CHECK(hit == 1, "intersect_ray_plane: ray heading straight at the plane hits");
        TEST_CHECK(t > 4.999f && t < 5.001f, "intersect_ray_plane: hit distance is 5");
    }

    /* ray-plane: parallel to the plane, never hits */
    {
        vec3 origin = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 dir = vec3_make(1.0f, 0.0f, 0.0f);
        vec3 plane_point = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 plane_normal = vec3_make(0.0f, 0.0f, 1.0f);
        f32 t;
        i32 hit = intersect_ray_plane(origin, dir, plane_point, plane_normal, &t);
        TEST_CHECK(hit == 0, "intersect_ray_plane: a ray parallel to the plane never hits");
    }

    /* ray-plane: plane behind the ray origin, direction pointing away */
    {
        vec3 origin = vec3_make(0.0f, 0.0f, 5.0f);
        vec3 dir = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 plane_point = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 plane_normal = vec3_make(0.0f, 0.0f, 1.0f);
        f32 t;
        i32 hit = intersect_ray_plane(origin, dir, plane_point, plane_normal, &t);
        TEST_CHECK(hit == 0, "intersect_ray_plane: plane behind the ray, heading away, is a miss");
    }

    print_flush();
    return test_failures_ != 0;
}
