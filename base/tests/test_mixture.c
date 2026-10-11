#include "test_common.h"
#define MIXTURE_IMPLEMENTATION
#include "mixture.h"

static i32 near_(f64 a, f64 b) { f64 d = a - b; if (d < 0) d = -d; return d < 0.0001; }

int main(void)
{
    Mixture m;
    f64 v;

    print_cstr("test_mixture\n");

    mixture_init(&m);
    TEST_CHECK(!mixture_value(&m, &v), "an empty mixture has no defined value - returns 0, not a silent 0.0/NaN");

    TEST_CHECK(mixture_add(&m, "red", 100.0, 1.0) == 1, "adding the first ingredient succeeds");
    {
        i32 ok = mixture_value(&m, &v) && near_(v, 100.0);
        TEST_CHECK(ok, "a single ingredient's value IS the mixture value regardless of its weight");
    }

    TEST_CHECK(mixture_add(&m, "blue", 0.0, 1.0) == 1, "adding a second ingredient succeeds");
    {
        i32 ok = mixture_value(&m, &v) && near_(v, 50.0);
        TEST_CHECK(ok, "two equally-weighted ingredients (100 and 0) average to exactly 50");
    }

    TEST_CHECK(mixture_set_weight(&m, "blue", 3.0) == 1, "set_weight on a real ingredient succeeds");
    {
        /* (100*1 + 0*3) / (1+3) = 25 */
        i32 ok = mixture_value(&m, &v) && near_(v, 25.0);
        TEST_CHECK(ok, "reweighting shifts the average toward the now-heavier ingredient's value");
    }

    TEST_CHECK(mixture_set_weight(&m, "does-not-exist", 5.0) == 0, "set_weight on a name that was never added fails cleanly");

    {
        Mixture zero_weights;
        mixture_init(&zero_weights);
        mixture_add(&zero_weights, "a", 10.0, 0.0);
        mixture_add(&zero_weights, "b", 20.0, 0.0);
        TEST_CHECK(!mixture_value(&zero_weights, &v), "every ingredient weighted 0 is a real 0/0 case - returns 0, not garbage");
    }

    {
        i32 i;
        Mixture full;
        mixture_init(&full);
        for (i = 0; i < MIXTURE_MAX_INGREDIENTS; i++) {
            TEST_CHECK(mixture_add(&full, "x", (f64)i, 1.0) == 1, "filling up to MIXTURE_MAX_INGREDIENTS all succeed");
        }
        TEST_CHECK(mixture_add(&full, "one-too-many", 0.0, 1.0) == 0, "adding one more past the cap fails cleanly, doesn't overflow");
    }

    print_flush();
    return test_failures_ != 0;
}
