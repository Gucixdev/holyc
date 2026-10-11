#include "test_common.h"
#define MEAN_IMPLEMENTATION
#include "mean.h"

int main(void)
{
    print_cstr("test_mean\n");

    {
        f32 data[5] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
        f32 m = mean_f32(data, 5);
        TEST_CHECK(m == 3.0f, "mean_f32([1,2,3,4,5]) == 3");
    }

    {
        f32 data[4] = { 10.0f, 10.0f, 10.0f, 10.0f };
        f32 m = mean_f32(data, 4);
        TEST_CHECK(m == 10.0f, "mean_f32 of identical values is that value");
    }

    {
        f32 data[1] = { 42.0f };
        f32 m = mean_f32(data, 1);
        TEST_CHECK(m == 42.0f, "mean_f32 of a single value is that value");
    }

    {
        f32 data[6] = { -3.0f, -1.0f, 0.0f, 1.0f, 2.0f, 1.0f };
        f32 m = mean_f32(data, 6);
        TEST_CHECK(m == 0.0f, "mean_f32 correctly handles negative values");
    }

    print_flush();
    return test_failures_ != 0;
}
