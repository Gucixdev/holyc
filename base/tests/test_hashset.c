#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define HASHTABLE_IMPLEMENTATION
#define HASH_SET_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "data/hash/hashset.h"

int main(void)
{
    Arena a = {0};
    Hash_Set set;

    print_cstr("test_hashset\n");

    hash_set_init(&a, &set, 0);

    TEST_CHECK(hash_set_contains(&set, "apple", 5) == 0, "an empty set contains nothing");
    TEST_CHECK(hash_set_remove(&set, "apple", 5) == 0, "removing from an empty set returns 0");

    hash_set_add(&a, &set, "apple", 5);
    TEST_CHECK(hash_set_contains(&set, "apple", 5) == 1, "a key just added is reported as contained");
    TEST_CHECK(hash_set_contains(&set, "banana", 6) == 0, "a different key is not reported as contained");

    hash_set_add(&a, &set, "banana", 6);
    hash_set_add(&a, &set, "cherry", 6);
    TEST_CHECK(hash_set_contains(&set, "apple", 5) == 1, "apple is still present after adding more keys");
    TEST_CHECK(hash_set_contains(&set, "banana", 6) == 1, "banana is present");
    TEST_CHECK(hash_set_contains(&set, "cherry", 6) == 1, "cherry is present");

    /* adding the same key twice is idempotent, not an error and not a duplicate */
    hash_set_add(&a, &set, "apple", 5);
    TEST_CHECK(hash_set_contains(&set, "apple", 5) == 1, "re-adding an existing key leaves it present");

    TEST_CHECK(hash_set_remove(&set, "banana", 6) == 1, "removing a present key returns 1");
    TEST_CHECK(hash_set_contains(&set, "banana", 6) == 0, "the removed key is no longer contained");
    TEST_CHECK(hash_set_contains(&set, "apple", 5) == 1, "removing one key does not disturb another");
    TEST_CHECK(hash_set_remove(&set, "banana", 6) == 0, "removing an already-removed key returns 0");

    /* binary keys, not just C strings - key_len is the real length, not strlen */
    {
        u8 bin_key[3] = { 0x00, 0xFF, 0x01 };
        TEST_CHECK(hash_set_contains(&set, bin_key, 3) == 0, "a binary key is absent before being added");
        hash_set_add(&a, &set, bin_key, 3);
        TEST_CHECK(hash_set_contains(&set, bin_key, 3) == 1, "a binary key (including an embedded 0x00 byte) works correctly");
    }

    /* stress: many keys, all present, none of the absent ones falsely reported present */
    {
        i32 i;
        i32 all_present = 1;
        i32 no_false_positive = 1;
        char buf[16];

        for (i = 0; i < 200; i++) {
            i32 n = i, len = 0;
            char tmp[8];
            if (n == 0) { tmp[len++] = '0'; }
            while (n > 0) { tmp[len++] = (char)('0' + n % 10); n /= 10; }
            {
                i32 k;
                for (k = 0; k < len; k++) buf[k] = tmp[len - 1 - k];
            }
            hash_set_add(&a, &set, buf, (size_t)len);
        }
        for (i = 0; i < 200; i++) {
            i32 n = i, len = 0;
            char tmp[8];
            if (n == 0) { tmp[len++] = '0'; }
            while (n > 0) { tmp[len++] = (char)('0' + n % 10); n /= 10; }
            {
                i32 k;
                for (k = 0; k < len; k++) buf[k] = tmp[len - 1 - k];
            }
            if (!hash_set_contains(&set, buf, (size_t)len)) all_present = 0;
        }
        if (!hash_set_contains(&set, "not-in-the-set", 14)) { /* expected: absent */ } else { no_false_positive = 0; }

        TEST_CHECK(all_present, "200 stress-added numeric-string keys are all reported present");
        TEST_CHECK(no_false_positive, "a genuinely absent key is not falsely reported present after the stress add");
    }

    print_flush();
    return test_failures_ != 0;
}
