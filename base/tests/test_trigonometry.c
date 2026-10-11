#include "test_common.h"
#define TRIGONOMETRY_IMPLEMENTATION
#include "math/trigonometry.h"

int main(void)
{
    f32 r;

    print_cstr("test_trigonometry\n");

    r = deg_to_rad(180.0f);
    TEST_CHECK(r > 3.1415f && r < 3.1416f, "deg_to_rad(180) ~= pi");

    r = rad_to_deg((f32)HOLYC_PI);
    TEST_CHECK(r > 179.99f && r < 180.01f, "rad_to_deg(pi) ~= 180");

    r = turn_to_rad(0.5f);
    TEST_CHECK(r > 3.1415f && r < 3.1416f, "turn_to_rad(0.5) ~= pi (half turn)");

    r = rad_to_turn((f32)(2.0 * HOLYC_PI));
    TEST_CHECK(r > 0.999f && r < 1.001f, "rad_to_turn(2*pi) ~= 1 (full turn)");

    print_flush();
    return test_failures_ != 0;
}
