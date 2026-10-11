#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define RAYCAST_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "raycast.h"

int main(void)
{
    print_cstr("test_raycast\n");

    /* ray-AABB: straight hit through a unit box centered at origin */
    {
        vec3 origin = vec3_make(0.0f, 0.0f, -5.0f);
        vec3 dir = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 bmin = vec3_make(-1.0f, -1.0f, -1.0f);
        vec3 bmax = vec3_make(1.0f, 1.0f, 1.0f);
        f32 t;
        i32 hit = ray_aabb(origin, dir, bmin, bmax, &t);
        TEST_CHECK(hit == 1, "ray_aabb: straight-through ray hits the box");
        TEST_CHECK(t > 3.999f && t < 4.001f, "ray_aabb: hit distance is 4 (from z=-5 to z=-1)");
    }

    /* ray-AABB: miss, box far off to the side */
    {
        vec3 origin = vec3_make(0.0f, 0.0f, -5.0f);
        vec3 dir = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 bmin = vec3_make(10.0f, 10.0f, 10.0f);
        vec3 bmax = vec3_make(11.0f, 11.0f, 11.0f);
        f32 t;
        i32 hit = ray_aabb(origin, dir, bmin, bmax, &t);
        TEST_CHECK(hit == 0, "ray_aabb: ray missing the box entirely reports no hit");
    }

    /* ray-AABB: origin already inside the box still reports a hit at t=0 (or the exit) */
    {
        vec3 origin = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 dir = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 bmin = vec3_make(-1.0f, -1.0f, -1.0f);
        vec3 bmax = vec3_make(1.0f, 1.0f, 1.0f);
        f32 t;
        i32 hit = ray_aabb(origin, dir, bmin, bmax, &t);
        TEST_CHECK(hit == 1, "ray_aabb: ray starting inside the box reports a hit");
    }

    /* ray-sphere: straight hit through a unit sphere at origin */
    {
        vec3 origin = vec3_make(0.0f, 0.0f, -5.0f);
        vec3 dir = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 center = vec3_make(0.0f, 0.0f, 0.0f);
        f32 t = -1.0f; /* sentinel: stays put (and cleanly fails the check below) if ray_sphere ever wrongly reports no hit */
        i32 hit = ray_sphere(origin, dir, center, 1.0f, &t);
        TEST_CHECK(hit == 1, "ray_sphere: straight-through ray hits the sphere");
        TEST_CHECK(t > 3.999f && t < 4.001f, "ray_sphere: hit distance is 4 (from z=-5 to the near surface at z=-1)");
    }

    /* ray-sphere: miss, sphere far off to the side */
    {
        vec3 origin = vec3_make(0.0f, 0.0f, -5.0f);
        vec3 dir = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 center = vec3_make(10.0f, 10.0f, 10.0f);
        f32 t;
        i32 hit = ray_sphere(origin, dir, center, 1.0f, &t);
        TEST_CHECK(hit == 0, "ray_sphere: ray missing the sphere reports no hit");
    }

    /* ray-sphere: sphere entirely behind the ray origin (both roots negative) is a miss */
    {
        vec3 origin = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 dir = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 center = vec3_make(0.0f, 0.0f, -5.0f);
        f32 t;
        i32 hit = ray_sphere(origin, dir, center, 1.0f, &t);
        TEST_CHECK(hit == 0, "ray_sphere: sphere entirely behind the ray origin is a miss, not a backward hit");
    }

    /* ray-triangle (Moller-Trumbore): straight hit through the triangle's interior */
    {
        vec3 origin = vec3_make(0.25f, 0.25f, -5.0f);
        vec3 dir = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 v0 = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 v1 = vec3_make(1.0f, 0.0f, 0.0f);
        vec3 v2 = vec3_make(0.0f, 1.0f, 0.0f);
        f32 t;
        i32 hit = ray_triangle(origin, dir, v0, v1, v2, &t);
        TEST_CHECK(hit == 1, "ray_triangle: ray through the triangle's interior hits");
        TEST_CHECK(t > 4.999f && t < 5.001f, "ray_triangle: hit distance is 5 (from z=-5 to z=0)");
    }

    /* ray-triangle: miss, ray passes outside the triangle's footprint */
    {
        vec3 origin = vec3_make(5.0f, 5.0f, -5.0f);
        vec3 dir = vec3_make(0.0f, 0.0f, 1.0f);
        vec3 v0 = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 v1 = vec3_make(1.0f, 0.0f, 0.0f);
        vec3 v2 = vec3_make(0.0f, 1.0f, 0.0f);
        f32 t;
        i32 hit = ray_triangle(origin, dir, v0, v1, v2, &t);
        TEST_CHECK(hit == 0, "ray_triangle: ray passing outside the triangle's footprint misses");
    }

    /* ray-triangle: parallel ray (in the triangle's own plane) never hits */
    {
        vec3 origin = vec3_make(0.25f, 0.25f, 0.0f);
        vec3 dir = vec3_make(1.0f, 0.0f, 0.0f);
        vec3 v0 = vec3_make(0.0f, 0.0f, 0.0f);
        vec3 v1 = vec3_make(1.0f, 0.0f, 0.0f);
        vec3 v2 = vec3_make(0.0f, 1.0f, 0.0f);
        f32 t;
        i32 hit = ray_triangle(origin, dir, v0, v1, v2, &t);
        TEST_CHECK(hit == 0, "ray_triangle: a ray parallel to the triangle's own plane never hits");
    }

    print_flush();
    return test_failures_ != 0;
}
