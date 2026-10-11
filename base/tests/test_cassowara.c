#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define CASSOWARA_IMPLEMENTATION
#include "cassowara.h"

static vec3 v3(f32 x, f32 y, f32 z) { vec3 v; v.x = x; v.y = y; v.z = z; return v; }

int main(void)
{
    vec3 origin, forward_x, candidates[3];

    print_cstr("test_cassowara\n");

    origin = v3(0, 0, 0);
    forward_x = v3(1, 0, 0);

    TEST_CHECK(cassowara_is_next_to(origin, v3(1, 0, 0), 2.0f) == 1, "a point within max_dist is next_to");
    TEST_CHECK(cassowara_is_next_to(origin, v3(5, 0, 0), 2.0f) == 0, "a point beyond max_dist is not next_to");

    TEST_CHECK(cassowara_is_in_range(origin, v3(3, 0, 0), 1.0f, 5.0f) == 1, "a point inside [min,max] is in_range");
    TEST_CHECK(cassowara_is_in_range(origin, v3(0.5f, 0, 0), 1.0f, 5.0f) == 0, "a point closer than min is not in_range");
    TEST_CHECK(cassowara_is_in_range(origin, v3(10, 0, 0), 1.0f, 5.0f) == 0, "a point farther than max is not in_range");
    TEST_CHECK(cassowara_is_out_of_range(origin, v3(10, 0, 0), 1.0f, 5.0f) == 1, "out_of_range is the exact negation of in_range");

    TEST_CHECK(cassowara_is_in_front_of(origin, forward_x, v3(5, 0, 0), 45.0f) == 1, "dead ahead is in_front_of");
    TEST_CHECK(cassowara_is_in_front_of(origin, forward_x, v3(-5, 0, 0), 45.0f) == 0, "dead behind is NOT in_front_of");
    TEST_CHECK(cassowara_is_behind(origin, forward_x, v3(-5, 0, 0), 45.0f) == 1, "dead behind IS behind");
    TEST_CHECK(cassowara_is_behind(origin, forward_x, v3(5, 0, 0), 45.0f) == 0, "dead ahead is NOT behind");
    TEST_CHECK(cassowara_is_facing(origin, forward_x, v3(5, 0, 0), 45.0f) == 1, "facing is the same as in_front_of");

    /* a point exactly on the 45-degree boundary of a 90-degree-wide
     * (45 half-angle) cone should count as inside it */
    TEST_CHECK(cassowara_is_in_front_of(origin, forward_x, v3(1, 1, 0), 45.0f) == 1, "a point exactly at the cone edge counts as inside");
    /* just past the boundary should not */
    TEST_CHECK(cassowara_is_in_front_of(origin, forward_x, v3(1, 2, 0), 45.0f) == 0, "a point past the cone edge is excluded");

    candidates[0] = v3(10, 0, 0);
    candidates[1] = v3(2, 0, 0);
    candidates[2] = v3(20, 0, 0);
    TEST_CHECK(cassowara_closest_to(origin, candidates, 3) == 1, "closest_to picks the nearest candidate's index");
    TEST_CHECK(cassowara_farthest_to(origin, candidates, 3) == 2, "farthest_to picks the farthest candidate's index");
    TEST_CHECK(cassowara_closest_to(origin, candidates, 0) == -1, "closest_to on an empty candidate list returns -1");

    print_flush();
    return test_failures_ != 0;
}
