#include "test_common.h"
#define RNG_IMPLEMENTATION
#include "math/rng.h"

int main(void)
{
    Rng r1, r2;
    u32 vals[1000];
    i32 i, all_distinct, distinct_count;
    u64 sum;
    f32 f;
    i32 all_in_range;
    i32 range_ok;

    print_cstr("test_rng\n");

    /* same seed -> same sequence, deterministic */
    pcg32_seed(&r1, 12345, 0);
    pcg32_seed(&r2, 12345, 0);
    all_distinct = 1; /* misnamed here, reused below for the actual distinctness check */
    for (i = 0; i < 100; i++) {
        if (pcg32_u32(&r1) != pcg32_u32(&r2)) all_distinct = 0;
    }
    TEST_CHECK(all_distinct, "same seed produces the identical sequence (deterministic)");

    /* different seed -> different sequence */
    pcg32_seed(&r1, 1, 0);
    pcg32_seed(&r2, 2, 0);
    TEST_CHECK(pcg32_u32(&r1) != pcg32_u32(&r2), "different seeds produce different first outputs");

    /* rough distribution sanity: 1000 draws shouldn't collapse to a
     * handful of distinct values (would indicate a badly broken generator,
     * not testing for true uniformity here, just "not obviously broken") */
    pcg32_seed(&r1, 999, 0);
    for (i = 0; i < 1000; i++) vals[i] = pcg32_u32(&r1);
    distinct_count = 0;
    for (i = 0; i < 1000; i++) {
        i32 j, dup = 0;
        for (j = 0; j < i; j++) if (vals[j] == vals[i]) { dup = 1; break; }
        if (!dup) distinct_count++;
    }
    TEST_CHECK(distinct_count > 990, "1000 draws are (almost) all distinct - no obvious collapse");

    /* pcg32_f32 stays in [0, 1) */
    pcg32_seed(&r1, 42, 0);
    all_in_range = 1;
    for (i = 0; i < 1000; i++) {
        f = pcg32_f32(&r1);
        if (f < 0.0f || f >= 1.0f) all_in_range = 0;
    }
    TEST_CHECK(all_in_range, "pcg32_f32 always lands in [0, 1)");

    /* pcg32_range stays in [lo, hi) */
    pcg32_seed(&r1, 7, 0);
    range_ok = 1;
    sum = 0;
    for (i = 0; i < 1000; i++) {
        u32 v = pcg32_range(&r1, 10, 20);
        if (v < 10 || v >= 20) range_ok = 0;
        sum += v;
    }
    TEST_CHECK(range_ok, "pcg32_range always lands in [lo, hi)");
    TEST_CHECK(sum / 1000 >= 12 && sum / 1000 <= 17, "pcg32_range mean is roughly centered (not wildly skewed)");

    print_flush();
    return test_failures_ != 0;
}
