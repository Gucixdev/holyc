#include "test_common.h"
#define AVX_MATH_IMPLEMENTATION
#include "base.h"

int main(void)
{
    print_cstr("test_avx_math\n");

    if (!avx_math_available()) {
        print_cstr("  SKIP: AVX2 not available on this CPU\n");
        print_flush();
        return 0;
    }

    /* sum(1..8) == 36 */
    {
        f32 data[8] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };
        f32 s = avx_sum8(data);
        TEST_CHECK(s > 35.999f && s < 36.001f, "avx_sum8({1..8}) == 36");
    }

    /* dot({1,2,3,4,5,6,7,8}, {8,7,6,5,4,3,2,1}) = 8+14+18+20+20+18+14+8 = 120 */
    {
        f32 a[8] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };
        f32 b[8] = { 8.0f, 7.0f, 6.0f, 5.0f, 4.0f, 3.0f, 2.0f, 1.0f };
        f32 d = avx_dot8(a, b);
        TEST_CHECK(d > 119.999f && d < 120.001f, "avx_dot8 of known 8-element vectors == 120");
    }

    /* dot of a vector with itself == sum of squares: 1+4+9+16 = 30 (using
       only 4 nonzero + 4 zero lanes to keep the hand-computed value small) */
    {
        f32 a[8] = { 1.0f, 2.0f, 3.0f, 4.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        f32 d = avx_dot8(a, a);
        TEST_CHECK(d > 29.999f && d < 30.001f, "avx_dot8(a,a) == sum of squares (30)");
    }

    {
        f32 data[8] = { 5.0f, -3.0f, 8.0f, 1.0f, -10.0f, 4.0f, 2.0f, 0.0f };
        f32 mn = avx_min8(data);
        f32 mx = avx_max8(data);
        TEST_CHECK(mn > -10.001f && mn < -9.999f, "avx_min8 finds -10 among 8 values");
        TEST_CHECK(mx > 7.999f && mx < 8.001f, "avx_max8 finds 8 among 8 values");
    }

    {
        f32 same[8] = { 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f, 4.0f };
        TEST_CHECK(avx_sum8(same) > 31.999f && avx_sum8(same) < 32.001f, "avx_sum8 of 8 identical values is 8x that value");
        TEST_CHECK(avx_min8(same) == 4.0f && avx_max8(same) == 4.0f, "avx_min8/avx_max8 of identical values both equal that value");
    }

    print_flush();
    return test_failures_ != 0;
}
