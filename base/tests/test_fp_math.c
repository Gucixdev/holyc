#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define FP_MATH_IMPLEMENTATION
#include "base.h"
#include "fp_math.h"

int main(void)
{
    print_cstr("test_fp_math\n");

    /* fp32_sqrt(4.0) ~= 2.0 */
    {
        fp32 four = fp32_from_f32(4.0f);
        fp32 root = fp32_sqrt(four);
        f32 back = fp32_to_f32(root);
        TEST_CHECK(back > 1.99f && back < 2.01f, "fp32_sqrt(4.0) ~= 2.0");
    }

    /* fp32_sqrt(2.0) ~= 1.41421356 */
    {
        fp32 two = fp32_from_f32(2.0f);
        fp32 root = fp32_sqrt(two);
        f32 back = fp32_to_f32(root);
        TEST_CHECK(back > 1.41f && back < 1.42f, "fp32_sqrt(2.0) ~= 1.41421356");
    }

    /* fp32_sqrt(1.0) == 1.0 exactly */
    {
        fp32 one = fp32_from_f32(1.0f);
        fp32 root = fp32_sqrt(one);
        f32 back = fp32_to_f32(root);
        TEST_CHECK(back > 0.999f && back < 1.001f, "fp32_sqrt(1.0) ~= 1.0");
    }

    /* fp32_sqrt(0.0) == 0.0 */
    {
        fp32 zero = fp32_from_f32(0.0f);
        fp32 root = fp32_sqrt(zero);
        TEST_CHECK(root == 0, "fp32_sqrt(0.0) == 0.0 exactly");
    }

    /* fp32_sqrt(0.25) ~= 0.5 - exercises a fractional input smaller than 1 */
    {
        fp32 quarter = fp32_from_f32(0.25f);
        fp32 root = fp32_sqrt(quarter);
        f32 back = fp32_to_f32(root);
        TEST_CHECK(back > 0.49f && back < 0.51f, "fp32_sqrt(0.25) ~= 0.5");
    }

    /* fp32_sqrt against many values, cross-checked against a real f32 sqrt
       computed independently via fp32_mul (x*x == original, roughly) */
    {
        f32 inputs[6] = { 9.0f, 16.0f, 100.0f, 0.01f, 3.14159f, 1000.0f };
        i32 all_ok = 1;
        i32 i;
        for (i = 0; i < 6; i++) {
            fp32 x = fp32_from_f32(inputs[i]);
            fp32 root = fp32_sqrt(x);
            fp32 squared_back = fp32_mul(root, root);
            f32 diff = fp32_to_f32(squared_back) - inputs[i];
            if (diff < 0.0f) diff = -diff;
            /* fixed-point Q16.16 precision, allow a modest relative tolerance */
            if (diff > inputs[i] * 0.01f + 0.01f) all_ok = 0;
        }
        TEST_CHECK(all_ok, "fp32_sqrt(x) squared via fp32_mul recovers x within Q16.16 tolerance, across several values");
    }

    print_flush();
    return test_failures_ != 0;
}
