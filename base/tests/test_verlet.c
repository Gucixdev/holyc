#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define VERLET_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "verlet.h"

int main(void)
{
    print_cstr("test_verlet\n");

    /* a free point under constant gravity, no damping loss (damping=1.0),
       integrated for one step: new_pos = pos + (pos-prev_pos)*damping + accel*dt*dt.
       pos=(0,0,0), prev_pos=(0,0,0) (at rest), accel=(0,-10,0), dt=1.0
       expected new_pos = (0,0,0) + (0,0,0)*1 + (0,-10,0)*1 = (0,-10,0) */
    {
        Verlet_Point p = verlet_point_make(vec3_make(0.0f, 0.0f, 0.0f), 1.0f);
        vec3 gravity = vec3_make(0.0f, -10.0f, 0.0f);
        verlet_integrate(&p, gravity, 1.0f, 1.0f);
        TEST_CHECK(p.pos.y > -10.001f && p.pos.y < -9.999f, "verlet_integrate: point at rest under gravity moves to y=-10 after dt=1");
        TEST_CHECK(p.prev_pos.x == 0.0f && p.prev_pos.y == 0.0f && p.prev_pos.z == 0.0f,
                   "verlet_integrate: prev_pos becomes the OLD position, not the new one");
    }

    /* a point already moving (prev_pos behind pos) continues its implicit
       velocity: pos=(0,1,0), prev_pos=(0,0,0) (moved +1 last step), no
       acceleration, damping=1 -> should keep moving by the same delta */
    {
        Verlet_Point p = verlet_point_make(vec3_make(0.0f, 1.0f, 0.0f), 1.0f);
        p.prev_pos = vec3_make(0.0f, 0.0f, 0.0f);
        verlet_integrate(&p, vec3_make(0.0f, 0.0f, 0.0f), 1.0f, 1.0f);
        TEST_CHECK(p.pos.y > 1.999f && p.pos.y < 2.001f, "verlet_integrate: implicit velocity (pos-prev_pos) carries forward with no acceleration");
    }

    /* a pinned point (inv_mass == 0) must not move at all, even under gravity */
    {
        Verlet_Point p = verlet_point_make(vec3_make(5.0f, 5.0f, 5.0f), 0.0f);
        vec3 gravity = vec3_make(0.0f, -10.0f, 0.0f);
        verlet_integrate(&p, gravity, 1.0f, 1.0f);
        TEST_CHECK(p.pos.x == 5.0f && p.pos.y == 5.0f && p.pos.z == 5.0f, "verlet_integrate: a pinned point (inv_mass=0) does not move under gravity");
    }

    /* damping < 1 should reduce the carried-forward implicit velocity */
    {
        Verlet_Point p_damped = verlet_point_make(vec3_make(0.0f, 2.0f, 0.0f), 1.0f);
        p_damped.prev_pos = vec3_make(0.0f, 0.0f, 0.0f);
        verlet_integrate(&p_damped, vec3_make(0.0f, 0.0f, 0.0f), 1.0f, 0.5f);
        TEST_CHECK(p_damped.pos.y > 2.999f && p_damped.pos.y < 3.001f, "verlet_integrate: damping=0.5 halves the carried-forward delta (2 + 2*0.5=3)");
    }

    print_flush();
    return test_failures_ != 0;
}
