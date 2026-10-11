#include "test_common.h"
#define XORSHIFT_IMPLEMENTATION
#include "math/xorshift.h"

int main(void)
{
    Xorshift32 r1, r2;

    print_cstr("test_xorshift\n");

    /* determinism: same seed produces the same stream */
    xorshift32_seed(&r1, 42);
    xorshift32_seed(&r2, 42);
    TEST_CHECK(xorshift32_u32(&r1) == xorshift32_u32(&r2), "same seed produces the same first output");
    TEST_CHECK(xorshift32_u32(&r1) == xorshift32_u32(&r2), "same seed produces the same second output too");

    /* different seeds diverge */
    xorshift32_seed(&r1, 1);
    xorshift32_seed(&r2, 2);
    TEST_CHECK(xorshift32_u32(&r1) != xorshift32_u32(&r2), "different seeds produce different output");

    /* seed 0 is remapped, not a fixed point (0 never advances if left as-is) */
    {
        Xorshift32 r;
        u32 first;
        xorshift32_seed(&r, 0);
        TEST_CHECK(r.state == 1, "seed 0 is silently remapped to 1");
        first = xorshift32_u32(&r);
        TEST_CHECK(first != 0, "output after seeding with 0 is not stuck at 0");
    }

    /* f32 output lands in [0, 1) */
    {
        Xorshift32 r;
        i32 all_in_range = 1;
        i32 i;
        xorshift32_seed(&r, 123);
        for (i = 0; i < 1000; i++) {
            f32 v = xorshift32_f32(&r);
            if (v < 0.0f || v >= 1.0f) all_in_range = 0;
        }
        TEST_CHECK(all_in_range, "xorshift32_f32 output always lands in [0, 1) across 1000 draws");
    }

    /* range output respects [lo, hi_exclusive) */
    {
        Xorshift32 r;
        i32 all_in_range = 1;
        i32 saw_variety = 0;
        u32 first_val;
        i32 i;
        xorshift32_seed(&r, 7);
        first_val = xorshift32_range(&r, 10, 20);
        for (i = 0; i < 200; i++) {
            u32 v = xorshift32_range(&r, 10, 20);
            if (v < 10 || v >= 20) all_in_range = 0;
            if (v != first_val) saw_variety = 1;
        }
        TEST_CHECK(all_in_range, "xorshift32_range output always lands in [10, 20) across 200 draws");
        TEST_CHECK(saw_variety, "xorshift32_range doesn't always return the same value (real variety, not stuck)");
    }

    print_flush();
    return test_failures_ != 0;
}
