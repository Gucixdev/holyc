#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define PREFIX_IMPLEMENTATION
#include "data/prefix.h"

int main(void)
{
    Arena arena = {0};
    Prefix_Tree t;
    i32 v1 = 1, v2 = 2, v3 = 3;

    print_cstr("test_prefix\n");

    prefix_init(&t, &arena);
    TEST_CHECK(t.count == 0, "starts empty");

    prefix_insert(&t, "cat", 3, &v1);
    prefix_insert(&t, "car", 3, &v2);
    prefix_insert(&t, "card", 4, &v3);
    TEST_CHECK(t.count == 3, "count reflects 3 distinct inserted keys");

    TEST_CHECK(prefix_get(&t, "cat", 3) == &v1, "exact key lookup finds the right value");
    TEST_CHECK(prefix_get(&t, "car", 3) == &v2, "a key that's a prefix of another still looks up on its own");
    TEST_CHECK(prefix_get(&t, "card", 4) == &v3, "a key that extends another still looks up on its own");

    TEST_CHECK(prefix_get(&t, "ca", 2) == NULL, "a prefix that was never itself inserted returns NULL");
    TEST_CHECK(prefix_get(&t, "dog", 3) == NULL, "a completely absent key returns NULL");

    TEST_CHECK(prefix_has_prefix(&t, "ca", 2) == 1, "a prefix of real keys reports has_prefix true");
    TEST_CHECK(prefix_has_prefix(&t, "car", 3) == 1, "a key that's also a real prefix of another reports true");
    TEST_CHECK(prefix_has_prefix(&t, "xyz", 3) == 0, "an unrelated prefix reports false");

    {
        i32 v4 = 99;
        prefix_insert(&t, "cat", 3, &v4);
        TEST_CHECK(t.count == 3, "re-inserting an existing key does not grow count");
        TEST_CHECK(prefix_get(&t, "cat", 3) == &v4, "re-inserting an existing key overwrites its value");
    }

    print_flush();
    return test_failures_ != 0;
}
