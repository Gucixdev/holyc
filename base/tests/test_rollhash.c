#include "test_common.h"
#define ROLLHASH_IMPLEMENTATION
#include "rollhash.h"

int main(void)
{
    Roll_Hash r;
    Roll_Hash direct;

    print_cstr("test_rollhash\n");

    rollhash_init(&r, (const u8 *)"abcd", 4);

    /* roll the window forward by one: "abcd" -> "bcde" */
    rollhash_roll(&r, 'a', 'e');
    rollhash_init(&direct, (const u8 *)"bcde", 4);
    TEST_CHECK(rollhash_value(&r) == rollhash_value(&direct), "rolling one byte matches hashing the shifted window directly");

    /* keep rolling: "bcde" -> "cdef" -> "defg" */
    rollhash_roll(&r, 'b', 'f');
    rollhash_roll(&r, 'c', 'g');
    rollhash_init(&direct, (const u8 *)"defg", 4);
    TEST_CHECK(rollhash_value(&r) == rollhash_value(&direct), "multiple successive rolls stay in sync with a direct hash");

    {
        Roll_Hash a, b;
        rollhash_init(&a, (const u8 *)"abcd", 4);
        rollhash_init(&b, (const u8 *)"abce", 4);
        TEST_CHECK(rollhash_value(&a) != rollhash_value(&b), "different windows hash differently (in practice, not guaranteed)");
    }

    print_flush();
    return test_failures_ != 0;
}
