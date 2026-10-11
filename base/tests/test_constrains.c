#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define VERLET_IMPLEMENTATION
#define CONSTRAINS_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "verlet.h"
#include "constrains.h"

static f32 dist3_(vec3 a, vec3 b)
{
    return vec3_len(vec3_sub(a, b));
}

int main(void)
{
    print_cstr("test_constrains\n");

    /* two equal-mass points too far apart converge toward rest_length
       symmetrically after several satisfy iterations */
    {
        Verlet_Point a = verlet_point_make(vec3_make(0.0f, 0.0f, 0.0f), 1.0f);
        Verlet_Point b = verlet_point_make(vec3_make(10.0f, 0.0f, 0.0f), 1.0f);
        Verlet_Distance_Constraint c = verlet_distance_constraint_make(&a, &b, 2.0f);
        i32 i;

        for (i = 0; i < 50; i++) constraint_satisfy(&c);

        {
            f32 d = dist3_(a.pos, b.pos);
            TEST_CHECK(d > 1.99f && d < 2.01f, "constraint_satisfy: distance converges to rest_length after 50 iterations");
        }
        TEST_CHECK(a.pos.x > 3.99f && a.pos.x < 4.01f, "constraint_satisfy: equal masses move symmetrically (a stops at x~4)");
        TEST_CHECK(b.pos.x > 5.99f && b.pos.x < 6.01f, "constraint_satisfy: equal masses move symmetrically (b stops at x~6)");
    }

    /* a pinned point (inv_mass=0) stays fixed; the other point does all the moving */
    {
        Verlet_Point a = verlet_point_make(vec3_make(0.0f, 0.0f, 0.0f), 0.0f); /* pinned */
        Verlet_Point b = verlet_point_make(vec3_make(10.0f, 0.0f, 0.0f), 1.0f);
        Verlet_Distance_Constraint c = verlet_distance_constraint_make(&a, &b, 2.0f);
        i32 i;

        for (i = 0; i < 50; i++) constraint_satisfy(&c);

        TEST_CHECK(a.pos.x == 0.0f && a.pos.y == 0.0f && a.pos.z == 0.0f, "constraint_satisfy: pinned point a does not move at all");
        {
            f32 d = dist3_(a.pos, b.pos);
            TEST_CHECK(d > 1.99f && d < 2.01f, "constraint_satisfy: distance still converges to rest_length even with one point pinned");
        }
        TEST_CHECK(b.pos.x > 1.99f && b.pos.x < 2.01f, "constraint_satisfy: unpinned point b moves all the way to satisfy the constraint");
    }

    /* both points pinned: constraint_satisfy must be a safe no-op (not a
       divide-by-zero or NaN), positions stay exactly where they started */
    {
        Verlet_Point a = verlet_point_make(vec3_make(0.0f, 0.0f, 0.0f), 0.0f);
        Verlet_Point b = verlet_point_make(vec3_make(10.0f, 0.0f, 0.0f), 0.0f);
        Verlet_Distance_Constraint c = verlet_distance_constraint_make(&a, &b, 2.0f);
        constraint_satisfy(&c);
        TEST_CHECK(a.pos.x == 0.0f && b.pos.x == 10.0f, "constraint_satisfy: two pinned points is a safe no-op, not a crash/NaN");
    }

    print_flush();
    return test_failures_ != 0;
}
