#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define CASSOWARA_IMPLEMENTATION
#define CASSOWARA_PERCENTAGE_IMPLEMENTATION
#include "cassowara_percentage.h"

static vec3 v3(f32 x, f32 y, f32 z) { vec3 v; v.x = x; v.y = y; v.z = z; return v; }

int main(void)
{
    vec3 origin, forward_x;

    print_cstr("test_cassowara_percentage\n");

    origin = v3(0, 0, 0);
    forward_x = v3(1, 0, 0);

    TEST_CHECK(cassowara_is_within_arc(origin, forward_x, v3(-5, 5, 0), CASSOWARA_ARC_FULL) == 1, "the full arc (1.0) contains everything, even dead behind-ish");
    TEST_CHECK(cassowara_is_within_arc(origin, forward_x, v3(5, 0, 0), CASSOWARA_ARC_HALF) == 1, "dead ahead is within the half arc");
    TEST_CHECK(cassowara_is_within_arc(origin, forward_x, v3(-5, 0, 0), CASSOWARA_ARC_HALF) == 0, "dead behind is NOT within the half (front) arc");
    TEST_CHECK(cassowara_is_within_arc(origin, forward_x, v3(5, 0, 0), CASSOWARA_ARC_QUARTER) == 1, "dead ahead is within the quarter arc too");
    TEST_CHECK(cassowara_is_within_arc(origin, forward_x, v3(1, 1.5f, 0), CASSOWARA_ARC_QUARTER) == 0, "56 degrees off-axis is NOT within the quarter (90-degree-wide) arc");
    TEST_CHECK(cassowara_is_within_arc(origin, forward_x, v3(1, 1, 0), CASSOWARA_ARC_HALF) == 1, "45 degrees off-axis IS within the half (180-degree-wide) arc");

    TEST_CHECK(cassowara_arc_bucket(origin, forward_x, v3(5, 0, 0)) == CASSOWARA_ARC_EIGHTH, "dead-on-axis buckets into the smallest (eighth) arc");
    TEST_CHECK(cassowara_arc_bucket(origin, forward_x, v3(1, 1.5f, 0)) == CASSOWARA_ARC_HALF, "56 degrees off-axis buckets into the half arc, not smaller");
    TEST_CHECK(cassowara_arc_bucket(origin, forward_x, v3(-5, 0, 0)) == CASSOWARA_ARC_FULL, "dead behind only fits the full-circle bucket");

    print_flush();
    return test_failures_ != 0;
}
