#include "test_common.h"
#define SSE_MATH_IMPLEMENTATION
#include "base.h"

int main(void)
{
    print_cstr("test_sse_math\n");

    if (!sse_math_available()) {
        print_cstr("  SKIP: SSE2 not available on this CPU\n");
        print_flush();
        return 0;
    }

    /* sum(1,2,3,4) == 10 */
    {
        f32 data[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
        f32 s = sse_sum4(data);
        TEST_CHECK(s > 9.999f && s < 10.001f, "sse_sum4({1,2,3,4}) == 10");
    }

    /* dot({1,2,3,4}, {4,3,2,1}) = 4+6+6+4 = 20 */
    {
        f32 a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
        f32 b[4] = { 4.0f, 3.0f, 2.0f, 1.0f };
        f32 d = sse_dot4(a, b);
        TEST_CHECK(d > 19.999f && d < 20.001f, "sse_dot4 of known 4-element vectors == 20");
    }

    /* dot of a vector with itself == sum of squares: 1+4+9+16 = 30 */
    {
        f32 a[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
        f32 d = sse_dot4(a, a);
        TEST_CHECK(d > 29.999f && d < 30.001f, "sse_dot4(a,a) == sum of squares (30)");
    }

    {
        f32 data[4] = { 5.0f, -3.0f, 8.0f, 1.0f };
        f32 mn = sse_min4(data);
        f32 mx = sse_max4(data);
        TEST_CHECK(mn > -3.001f && mn < -2.999f, "sse_min4 finds -3 among 4 values");
        TEST_CHECK(mx > 7.999f && mx < 8.001f, "sse_max4 finds 8 among 4 values");
    }

    {
        f32 same[4] = { 7.0f, 7.0f, 7.0f, 7.0f };
        TEST_CHECK(sse_sum4(same) > 27.999f && sse_sum4(same) < 28.001f, "sse_sum4 of 4 identical values is 4x that value");
        TEST_CHECK(sse_min4(same) == 7.0f && sse_max4(same) == 7.0f, "sse_min4/sse_max4 of identical values both equal that value");
    }

    print_flush();
    return test_failures_ != 0;
}
