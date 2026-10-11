#include "test_common.h"
#define XORSHIFT_IMPLEMENTATION
#define DISTRIBUTION_IMPLEMENTATION
#include "xorshift.h"
#include "distribution.h"

int main(void)
{
    print_cstr("test_distribution\n");

    /* Box-Muller: known input/output relationship - u2=0 makes theta=0,
       so z0 = r*cos(0) = r, z1 = r*sin(0) = 0 */
    {
        f32 z0, z1;
        f32 expected_r;
        dist_gaussian_box_muller(0.1353352832f /* e^-2, so -2*ln(u1) == 4, r == 2 */, 0.0f, &z0, &z1);
        expected_r = 2.0f;
        TEST_CHECK(z0 > expected_r - 0.01f && z0 < expected_r + 0.01f, "Box-Muller: known input produces r on z0 when theta=0");
        TEST_CHECK(z1 > -0.01f && z1 < 0.01f, "Box-Muller: z1 is ~0 when theta=0 (sin(0)=0)");
    }

    /* Box-Muller: statistical sanity over many samples - mean should be
       near 0, and values should spread out (not collapse to one value) */
    {
        Xorshift32 rng;
        f64 sum = 0.0;
        f64 sum_sq = 0.0;
        i32 n = 20000;
        i32 i;
        f64 mean, variance;

        xorshift32_seed(&rng, 999);
        for (i = 0; i < n; i += 2) {
            f32 u1 = xorshift32_f32(&rng);
            f32 u2 = xorshift32_f32(&rng);
            f32 z0, z1;
            if (u1 <= 0.0f) u1 = 0.0001f; /* Box-Muller's real precondition u1 > 0 */
            dist_gaussian_box_muller(u1, u2, &z0, &z1);
            sum += (f64)z0 + (f64)z1;
            sum_sq += (f64)z0 * (f64)z0 + (f64)z1 * (f64)z1;
        }
        mean = sum / (f64)n;
        variance = sum_sq / (f64)n - mean * mean;

        TEST_CHECK(mean > -0.05 && mean < 0.05, "Box-Muller: sample mean over 20000 draws is close to 0");
        TEST_CHECK(variance > 0.9 && variance < 1.1, "Box-Muller: sample variance over 20000 draws is close to 1");
    }

    /* exponential: known inverse-CDF value - u=0 always gives 0 regardless of rate */
    {
        f32 v = dist_exponential(0.0f, 1.0f);
        TEST_CHECK(v == 0.0f, "exponential: u=0 gives exactly 0 (-ln(1-0) == 0)");
    }

    /* exponential: known value - u = 1 - e^-1 with rate=1 should give exactly 1.0 */
    {
        f32 v = dist_exponential(0.6321205588f /* 1 - e^-1 */, 1.0f);
        TEST_CHECK(v > 0.99f && v < 1.01f, "exponential: known input (1-e^-1, rate=1) gives ~1.0");
    }

    /* exponential: higher rate produces smaller values for the same u */
    {
        f32 v_slow = dist_exponential(0.5f, 1.0f);
        f32 v_fast = dist_exponential(0.5f, 10.0f);
        TEST_CHECK(v_fast < v_slow, "exponential: a higher rate produces a smaller value for the same u");
    }

    print_flush();
    return test_failures_ != 0;
}
