#include "test_common.h"
#define MATH_HASH_IMPLEMENTATION
#include "hash.h"

int main(void)
{
    print_cstr("test_math_hash\n");

    TEST_CHECK(math_hash_u32(42) == math_hash_u32(42), "hash_u32 is deterministic for the same input");
    TEST_CHECK(math_hash_u32(42) != math_hash_u32(43), "hash_u32 differs for different inputs (in practice, not guaranteed)");
    TEST_CHECK(math_hash_u32(1) != 1, "hash_u32 is not the identity function");

    TEST_CHECK(math_hash2_u32(1, 2) == math_hash2_u32(1, 2), "hash2_u32 is deterministic");
    TEST_CHECK(math_hash2_u32(1, 2) != math_hash2_u32(2, 1), "hash2_u32 is order-sensitive (x,y) != (y,x)");

    TEST_CHECK(math_hash3_u32(1, 2, 3) == math_hash3_u32(1, 2, 3), "hash3_u32 is deterministic");
    TEST_CHECK(math_hash3_u32(1, 2, 3) != math_hash2_u32(1, 2), "hash3_u32 differs from hash2_u32 given related inputs (in practice)");

    TEST_CHECK(math_hash_seeded(5, 1) != math_hash_seeded(5, 2), "the same coordinate with a different seed produces a different stream");
    TEST_CHECK(math_hash_seeded(5, 1) == math_hash_seeded(5, 1), "the same coordinate+seed pair is deterministic");

    /* rough avalanche sanity check: hashing 0..999 shouldn't collide much */
    {
        u32 i;
        u32 collisions = 0;
        u32 prev = math_hash_u32(0);
        for (i = 1; i < 1000; i++) {
            u32 h = math_hash_u32(i);
            if (h == prev) collisions++;
            prev = h;
        }
        TEST_CHECK(collisions == 0, "no adjacent-input collisions across 1000 consecutive integers (a real avalanche property)");
    }

    print_flush();
    return test_failures_ != 0;
}
