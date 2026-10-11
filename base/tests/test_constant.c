#include "test_common.h"
#include "core/base.h"

int main(void)
{
    print_cstr("test_constant\n");

    TEST_CHECK(HOLYC_PI > 3.14159265358 && HOLYC_PI < 3.14159265359, "HOLYC_PI matches the known value to 11 decimal digits");
    TEST_CHECK(HOLYC_TAU > 2.0 * 3.14159265358 && HOLYC_TAU < 2.0 * 3.14159265359, "HOLYC_TAU == 2*pi");
    TEST_CHECK(HOLYC_TAU == 2.0 * HOLYC_PI, "HOLYC_TAU is exactly 2*HOLYC_PI, not an independently-typed constant");

    TEST_CHECK(HOLYC_E > 2.71828182845 && HOLYC_E < 2.71828182846, "HOLYC_E matches the known value of e to 11 decimal digits");

    TEST_CHECK(HOLYC_SQRT2 > 1.41421356237 && HOLYC_SQRT2 < 1.41421356238, "HOLYC_SQRT2 matches sqrt(2) to 11 decimal digits");
    {
        f64 sq = HOLYC_SQRT2 * HOLYC_SQRT2;
        TEST_CHECK(sq > 1.9999999 && sq < 2.0000001, "HOLYC_SQRT2 squared is ~2.0");
    }

    TEST_CHECK(HOLYC_GOLDEN_RATIO > 1.61803398874 && HOLYC_GOLDEN_RATIO < 1.61803398875,
               "HOLYC_GOLDEN_RATIO matches the known value of phi to 11 decimal digits");
    {
        /* phi's own defining property: phi^2 == phi + 1 */
        f64 phi = HOLYC_GOLDEN_RATIO;
        f64 lhs = phi * phi;
        f64 rhs = phi + 1.0;
        TEST_CHECK(lhs > rhs - 0.0000001 && lhs < rhs + 0.0000001, "HOLYC_GOLDEN_RATIO satisfies phi^2 == phi+1");
    }

    {
        f64 deg = HOLYC_DEG_TO_RAD * 180.0;
        TEST_CHECK(deg > HOLYC_PI - 0.0000001 && deg < HOLYC_PI + 0.0000001, "HOLYC_DEG_TO_RAD * 180 == pi");
    }
    {
        f64 rad = HOLYC_RAD_TO_DEG * HOLYC_PI;
        TEST_CHECK(rad > 179.9999999 && rad < 180.0000001, "HOLYC_RAD_TO_DEG * pi == 180");
    }
    TEST_CHECK(HOLYC_DEG_TO_RAD * HOLYC_RAD_TO_DEG > 0.9999999 && HOLYC_DEG_TO_RAD * HOLYC_RAD_TO_DEG < 1.0000001,
               "HOLYC_DEG_TO_RAD and HOLYC_RAD_TO_DEG are reciprocal");

    print_flush();
    return test_failures_ != 0;
}
