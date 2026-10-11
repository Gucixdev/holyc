#include "test_common.h"
#define EXTRAS_IMPLEMENTATION
#include "math/extras.h"

int main(void)
{
    print_cstr("test_extras\n");

    {
        Ratio r = ratio_from_dimensions(1920, 1080);
        TEST_CHECK(r.num == 16 && r.den == 9, "1920x1080 simplifies to the well-known 16:9 aspect ratio");
    }
    {
        Ratio r = ratio_simplify(8, 4);
        TEST_CHECK(r.num == 2 && r.den == 1, "8:4 simplifies to 2:1");
    }
    {
        Ratio r = ratio_simplify(1, 3);
        TEST_CHECK(r.num == 1 && r.den == 3, "an already-lowest-terms ratio is unchanged");
    }
    {
        Ratio r = ratio_simplify(-4, 2);
        TEST_CHECK(r.num == -2 && r.den == 1, "the sign lands on the numerator, not the denominator");
    }
    {
        Ratio r = ratio_simplify(4, -2);
        TEST_CHECK(r.num == -2 && r.den == 1, "a negative denominator normalizes to a negative numerator instead");
    }

    TEST_CHECK(ratio_to_f64(ratio_simplify(1, 4)) == 0.25, "to_f64 converts a ratio to its real decimal value");

    TEST_CHECK(ratio_eq(ratio_simplify(2, 4), ratio_simplify(1, 2)), "eq compares by simplified value, not raw fields (2:4 == 1:2)");
    TEST_CHECK(!ratio_eq(ratio_simplify(1, 2), ratio_simplify(1, 3)), "eq correctly reports different ratios as unequal");

    print_flush();
    return test_failures_ != 0;
}
