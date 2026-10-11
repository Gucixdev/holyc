#include "test_common.h"
#define CASSOWARA_OPTS_IMPLEMENTATION
#include "cassowara_opts.h"

int main(void)
{
    Cassowara_Opts opts;

    print_cstr("test_cassowara_opts\n");

    cassowara_opts_defaults(&opts);
    TEST_CHECK(opts.next_to_dist == 2.0f, "default next_to_dist");
    TEST_CHECK(opts.facing_half_angle_deg == 45.0f, "default facing_half_angle_deg");
    TEST_CHECK(opts.in_range_min == 0.0f, "default in_range_min");
    TEST_CHECK(opts.in_range_max == 10.0f, "default in_range_max");

    print_flush();
    return test_failures_ != 0;
}
