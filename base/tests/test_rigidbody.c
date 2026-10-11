#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define RIGIDBODY_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "rigidbody.h"

int main(void)
{
    print_cstr("test_rigidbody\n");

    /* semi-implicit Euler, known hand-computable case: mass=1 (inv_mass=1),
       force=(0,-10,0) constant, dt=0.1, for 3 steps.
       step: v += F*inv_mass*dt; pos += v*dt (velocity updated BEFORE position)
       step1: v=(0,-1,0);   pos=(0,-0.1,0)
       step2: v=(0,-2,0);   pos=(0,-0.3,0)
       step3: v=(0,-3,0);   pos=(0,-0.6,0) */
    {
        Rigidbody b = rigidbody_make(vec3_make(0.0f, 0.0f, 0.0f), 1.0f);
        vec3 gravity = vec3_make(0.0f, -10.0f, 0.0f);
        i32 i;

        for (i = 0; i < 3; i++) {
            rigidbody_apply_force(&b, gravity, 0.1f);
            rigidbody_integrate(&b, 0.1f);
        }

        TEST_CHECK(b.velocity.y > -3.001f && b.velocity.y < -2.999f, "rigidbody: velocity after 3 steps of F=-10, dt=0.1 is -3.0 (semi-implicit Euler)");
        TEST_CHECK(b.position.y > -0.601f && b.position.y < -0.599f, "rigidbody: position after 3 steps matches hand-computed semi-implicit Euler result (-0.6)");
    }

    /* an immovable body (inv_mass=0, mass=0 sentinel) does not accelerate under force */
    {
        Rigidbody b = rigidbody_make(vec3_make(1.0f, 2.0f, 3.0f), 0.0f);
        rigidbody_apply_force(&b, vec3_make(100.0f, 100.0f, 100.0f), 1.0f);
        rigidbody_integrate(&b, 1.0f);
        TEST_CHECK(b.velocity.x == 0.0f && b.velocity.y == 0.0f && b.velocity.z == 0.0f, "rigidbody: an immovable body (mass=0) gains no velocity from force");
        TEST_CHECK(b.position.x == 1.0f && b.position.y == 2.0f && b.position.z == 3.0f, "rigidbody: an immovable body does not move");
    }

    /* a body with no force applied, existing velocity, still integrates position */
    {
        Rigidbody b = rigidbody_make(vec3_make(0.0f, 0.0f, 0.0f), 1.0f);
        b.velocity = vec3_make(5.0f, 0.0f, 0.0f);
        rigidbody_integrate(&b, 2.0f);
        TEST_CHECK(b.position.x > 9.999f && b.position.x < 10.001f, "rigidbody: constant velocity with no force integrates position correctly (5*2=10)");
    }

    print_flush();
    return test_failures_ != 0;
}
