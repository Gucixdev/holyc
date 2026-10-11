#include "test_common.h"
#define MEAN_IMPLEMENTATION
#define VARIANCE_IMPLEMENTATION
#include "math/mean.h"
#include "math/variance.h"

int main(void)
{
    print_cstr("test_variance\n");

    {
        /* [2,4,4,4,5,5,7,9], a textbook population-variance example -
           mean=5, population variance=4, population stddev=2 */
        f32 data[8] = { 2.0f, 4.0f, 4.0f, 4.0f, 5.0f, 5.0f, 7.0f, 9.0f };
        f32 m = mean_f32(data, 8);
        f32 v = variance_f32(data, 8, m);
        f32 sd = stddev_f32(data, 8, m);

        TEST_CHECK(m == 5.0f, "mean of the textbook dataset is 5");
        TEST_CHECK(v > 3.999f && v < 4.001f, "variance_f32 of the textbook dataset is 4");
        TEST_CHECK(sd > 1.999f && sd < 2.001f, "stddev_f32 of the textbook dataset is 2");
    }

    {
        f32 data[5] = { 7.0f, 7.0f, 7.0f, 7.0f, 7.0f };
        f32 m = mean_f32(data, 5);
        f32 v = variance_f32(data, 5, m);
        TEST_CHECK(v == 0.0f, "variance of identical values is exactly 0");
    }

    {
        f32 data[2] = { 0.0f, 10.0f };
        f32 m = mean_f32(data, 2);
        f32 v = variance_f32(data, 2, m);
        /* population variance of {0,10}: mean=5, squared devs = 25,25, avg = 25 */
        TEST_CHECK(v > 24.999f && v < 25.001f, "variance_f32 of {0,10} is 25 (population, not sample, variance)");
    }

    print_flush();
    return test_failures_ != 0;
}
