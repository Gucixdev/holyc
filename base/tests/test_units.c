#include "test_common.h"
#define UNITS_IMPLEMENTATION
#include "util/units.h"

int main(void)
{
    print_cstr("test_units\n");

    TEST_CHECK(units_length_convert(1000.0, UNITS_MM, UNITS_M) == 1.0, "1000mm converts to exactly 1m");
    TEST_CHECK(units_length_convert(1.0, UNITS_KM, UNITS_M) == 1000.0, "1km converts to exactly 1000m");
    TEST_CHECK(units_length_convert(250.0, UNITS_CM, UNITS_MM) == 2500.0, "250cm converts to exactly 2500mm");
    TEST_CHECK(units_length_convert(5.0, UNITS_M, UNITS_M) == 5.0, "converting a unit to itself is a no-op");

    TEST_CHECK(units_weight_convert(1000.0, UNITS_G, UNITS_KG) == 1.0, "1000g converts to exactly 1kg");
    TEST_CHECK(units_weight_convert(1.0, UNITS_T, UNITS_KG) == 1000.0, "1 tonne converts to exactly 1000kg");
    TEST_CHECK(units_weight_convert(1.0, UNITS_G, UNITS_MG) == 1000.0, "1g converts to exactly 1000mg");

    print_flush();
    return test_failures_ != 0;
}
