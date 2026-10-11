#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define BASE_TYPE_IMPLEMENTATION
#define HASHTABLE_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "core/base.h"
#include "data/hash/hashtable.h"

#include <string.h>
#include <stdio.h>

int main(void)
{
    Arena a = {0};
    Ht ht;
    i32 v1 = 1, v2 = 2, v3 = 3;
    void *got;
    i32 i;

    print_cstr("test_hashtable\n");

    ht_init(&a, &ht, 0);
    TEST_CHECK(ht.capacity == HT_DEFAULT_CAPACITY, "ht_init defaults to HT_DEFAULT_CAPACITY when hint is 0");

    ht_put(&a, &ht, "one", 3, &v1);
    ht_put(&a, &ht, "two", 3, &v2);
    ht_put(&a, &ht, "three", 5, &v3);
    TEST_CHECK(ht.count == 3, "ht_put on 3 distinct keys leaves count == 3");

    got = ht_get(&ht, "one", 3);
    TEST_CHECK(got == &v1, "ht_get finds the right value for an existing key");
    got = ht_get(&ht, "two", 3);
    TEST_CHECK(got == &v2, "ht_get finds a different existing key correctly too");
    got = ht_get(&ht, "missing", 7);
    TEST_CHECK(got == NULL, "ht_get returns NULL for a key that was never inserted");

    /* overwrite: same key, new value, count must NOT increase */
    ht_put(&a, &ht, "one", 3, &v3);
    TEST_CHECK(ht.count == 3, "ht_put on an existing key overwrites, does not grow count");
    got = ht_get(&ht, "one", 3);
    TEST_CHECK(got == &v3, "ht_get sees the overwritten value");

    /* delete, then confirm it's gone, then confirm OTHER keys still resolve
     * (this is the real test of tombstone-vs-empty probing: if delete broke
     * the probe chain, "two"/"three" would incorrectly report missing too) */
    TEST_CHECK(ht_delete(&ht, "one", 3) == 1, "ht_delete on an existing key returns 1");
    TEST_CHECK(ht_delete(&ht, "one", 3) == 0, "ht_delete on an already-deleted key returns 0 (not present)");
    got = ht_get(&ht, "one", 3);
    TEST_CHECK(got == NULL, "deleted key no longer resolves");
    got = ht_get(&ht, "two", 3);
    TEST_CHECK(got == &v2, "a DIFFERENT key still resolves after deleting another (tombstone didn't break probing)");
    got = ht_get(&ht, "three", 5);
    TEST_CHECK(got == &v3, "yet another key still resolves after the delete");

    /* binary keys, not just C strings - the whole point of taking key_len explicitly */
    {
        u8 bin_key[4] = { 0x00, 0xff, 0x01, 0xfe };
        i32 bin_val = 99;
        ht_put(&a, &ht, bin_key, 4, &bin_val);
        got = ht_get(&ht, bin_key, 4);
        TEST_CHECK(got == &bin_val, "binary (non-string) keys work, including embedded 0x00");
    }

    /* growth: insert enough keys to force at least one resize past 70%
     * load factor, then verify every single key still resolves correctly
     * afterward - this is the real test that ht_grow_'s rehash is correct */
    {
        Arena ga = {0};
        Ht growing;
        i32 vals[200];
        char keybuf[16];
        i32 all_found;
        size_t initial_capacity;

        ht_init(&ga, &growing, 4); /* tiny initial capacity - guarantees several resizes for 200 keys */
        initial_capacity = growing.capacity;

        for (i = 0; i < 200; i++) {
            vals[i] = i;
            sprintf(keybuf, "key%d", i);
            ht_put(&ga, &growing, keybuf, strlen(keybuf), &vals[i]);
        }

        TEST_CHECK(growing.capacity > initial_capacity, "table actually grew past its tiny initial capacity");
        TEST_CHECK(growing.count == 200, "all 200 distinct keys are present after growth");

        all_found = 1;
        for (i = 0; i < 200; i++) {
            i32 *found;
            sprintf(keybuf, "key%d", i);
            found = (i32 *)ht_get(&growing, keybuf, strlen(keybuf));
            if (!found || *found != i) { all_found = 0; break; }
        }
        TEST_CHECK(all_found, "every key survives one or more resizes with the correct value");

        arena_free(&ga);
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
