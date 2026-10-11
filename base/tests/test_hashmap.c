#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define HASHTABLE_IMPLEMENTATION
#define HASH_MAP_IMPLEMENTATION
#include "alloc/arena_alloc.h"
#include "data/hash/hashmap.h"

int main(void)
{
    Arena a = {0};
    Hash_Map m;
    i32 va = 1, vb = 2, vc = 3;

    print_cstr("test_hashmap\n");

    hash_map_init(&a, &m, 4);
    TEST_CHECK(hash_map_count(&m) == 0, "a freshly initialized map is empty");
    TEST_CHECK(hash_map_get(&m, "a", 1) == NULL, "getting a missing key returns NULL");

    hash_map_put(&a, &m, "a", 1, &va);
    hash_map_put(&a, &m, "b", 1, &vb);
    hash_map_put(&a, &m, "c", 1, &vc);
    TEST_CHECK(hash_map_count(&m) == 3, "count reflects 3 distinct inserted keys");
    TEST_CHECK(*(i32 *)hash_map_get(&m, "b", 1) == 2, "get returns the value stored for that key");

    TEST_CHECK(*(const char *)hash_map_key_at(&m, 0)->key == 'a', "iteration order preserves the order keys were first inserted");
    TEST_CHECK(*(const char *)hash_map_key_at(&m, 2)->key == 'c', "the last-inserted-so-far key is last in iteration order");

    {
        i32 va2 = 100;
        hash_map_put(&a, &m, "a", 1, &va2);
        TEST_CHECK(hash_map_count(&m) == 3, "overwriting an existing key does not grow the count");
        TEST_CHECK(*(i32 *)hash_map_get(&m, "a", 1) == 100, "overwriting an existing key replaces its value");
        TEST_CHECK(*(const char *)hash_map_key_at(&m, 0)->key == 'a', "overwriting an existing key does not change its insertion position");
    }

    TEST_CHECK(hash_map_remove(&m, "b", 1) == 1, "removing a present key succeeds");
    TEST_CHECK(hash_map_count(&m) == 2, "count drops after a successful remove");
    TEST_CHECK(hash_map_get(&m, "b", 1) == NULL, "a removed key is no longer found");
    TEST_CHECK(hash_map_remove(&m, "z", 1) == 0, "removing an absent key fails cleanly");

    print_flush();
    return test_failures_ != 0;
}
