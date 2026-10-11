#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define LERP_IMPLEMENTATION
#define PARTICLE_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "easing.h"
#include "particle.h"

int main(void)
{
    print_cstr("test_particle\n");

    /* a freshly spawned particle is not dead and starts at age 0 */
    {
        Particle p = particle_spawn(vec3_make(0.0f, 0.0f, 0.0f), vec3_make(1.0f, 0.0f, 0.0f), 2.0f,
                                     vec3_make(1.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 1.0f), 1.0f, 5.0f);
        TEST_CHECK(!particle_is_dead(&p), "particle_spawn: a fresh particle is not dead");
        TEST_CHECK(p.age == 0.0f, "particle_spawn: age starts at 0");
    }

    /* update integrates position via velocity, and ages the particle */
    {
        Particle p = particle_spawn(vec3_make(0.0f, 0.0f, 0.0f), vec3_make(2.0f, 0.0f, 0.0f), 10.0f,
                                     vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 0.0f), 1.0f, 1.0f);
        particle_update(&p, vec3_make(0.0f, 0.0f, 0.0f), 0.5f);
        TEST_CHECK(p.position.x > 0.999f && p.position.x < 1.001f, "particle_update: position integrates via velocity (2 * 0.5 = 1.0)");
        TEST_CHECK(p.age > 0.499f && p.age < 0.501f, "particle_update: age advances by dt");
    }

    /* a particle dies exactly when age reaches max_lifetime */
    {
        Particle p = particle_spawn(vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 0.0f), 1.0f,
                                     vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 0.0f), 1.0f, 1.0f);
        particle_update(&p, vec3_make(0.0f, 0.0f, 0.0f), 0.5f);
        TEST_CHECK(!particle_is_dead(&p), "particle: not yet dead at age=0.5 of max_lifetime=1.0");
        particle_update(&p, vec3_make(0.0f, 0.0f, 0.0f), 0.6f);
        TEST_CHECK(particle_is_dead(&p), "particle: dead once age exceeds max_lifetime");
    }

    /* a lerped property (color/size) reaches its END value at end-of-life */
    {
        Particle p = particle_spawn(vec3_make(0.0f, 0.0f, 0.0f), vec3_make(0.0f, 0.0f, 0.0f), 1.0f,
                                     vec3_make(1.0f, 0.0f, 0.0f), vec3_make(0.0f, 1.0f, 0.0f), 2.0f, 10.0f);
        vec3 c;
        f32 s;

        c = particle_current_color(&p);
        TEST_CHECK(c.x == 1.0f && c.y == 0.0f, "particle_current_color: at age=0, color equals color_start exactly");
        s = particle_current_size(&p);
        TEST_CHECK(s == 2.0f, "particle_current_size: at age=0, size equals size_start exactly");

        particle_update(&p, vec3_make(0.0f, 0.0f, 0.0f), 1.0f); /* age == max_lifetime now */
        c = particle_current_color(&p);
        TEST_CHECK(c.x < 0.001f && c.y > 0.999f, "particle_current_color: at end of life, color equals color_end");
        s = particle_current_size(&p);
        TEST_CHECK(s > 9.999f && s < 10.001f, "particle_current_size: at end of life, size equals size_end");
    }

    print_flush();
    return test_failures_ != 0;
}
