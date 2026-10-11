#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define ARRAY_IMPLEMENTATION
#include "arena_alloc.h"
#include "array.h"

int main(void)
{
    Arena arena = {0};

    print_cstr("test_array\n");

    /* static mode: fixed caller buffer, never grows */
    {
        i32 buffer[4];
        Array a;
        i32 v;

        array_static_init(&a, buffer, 4, sizeof(i32));
        TEST_CHECK(a.count == 0, "static: starts empty");

        v = 10; TEST_CHECK(array_push(NULL, &a, &v) == 1, "static: push into a non-full array succeeds");
        v = 20; array_push(NULL, &a, &v);
        v = 30; array_push(NULL, &a, &v);
        v = 40; TEST_CHECK(array_push(NULL, &a, &v) == 1, "static: push #4 fills exact capacity");
        TEST_CHECK(a.count == 4, "static: count reflects all 4 pushes");

        v = 50;
        TEST_CHECK(array_push(NULL, &a, &v) == 0, "static: push on a full array fails (does not grow)");
        TEST_CHECK(a.count == 4, "static: a failed push does not change count");

        TEST_CHECK(*(i32 *)array_get(&a, 0) == 10, "static: get(0) returns the first pushed value");
        TEST_CHECK(*(i32 *)array_get(&a, 3) == 40, "static: get(3) returns the last pushed value");
        TEST_CHECK(array_get(&a, 4) == NULL, "static: get() past count returns NULL");
        TEST_CHECK(buffer[2] == 30, "static: the array writes directly into the caller's own buffer");
    }

    /* dynamic mode: no buffer up front, grows via the arena */
    {
        Array a;
        i32 i;
        i32 all_ok = 1;

        array_dynamic_init(&a, sizeof(i32));
        TEST_CHECK(a.count == 0 && a.capacity == 0, "dynamic: starts with no capacity at all");

        for (i = 0; i < 100; i++) {
            if (!array_push(&arena, &a, &i)) { all_ok = 0; break; }
        }
        TEST_CHECK(all_ok, "dynamic: 100 pushes all succeed (grows past its initial capacity several times)");
        TEST_CHECK(a.count == 100, "dynamic: count reaches 100 after 100 pushes");
        TEST_CHECK(a.capacity >= 100, "dynamic: capacity grew to at least fit everything pushed");

        for (i = 0; i < 100; i++) {
            if (*(i32 *)array_get(&a, (size_t)i) != i) { all_ok = 0; break; }
        }
        TEST_CHECK(all_ok, "dynamic: every one of the 100 values reads back correctly after growth");

        {
            i32 v = 999;
            TEST_CHECK(array_push(NULL, &a, &v) == 0, "dynamic: pushing with a NULL arena fails cleanly instead of crashing");
        }
    }

    /* remove_swap: O(1) removal, order not preserved, shrinks count */
    {
        i32 buffer[5];
        Array a;
        i32 i;

        array_static_init(&a, buffer, 5, sizeof(i32));
        for (i = 0; i < 5; i++) array_push(NULL, &a, &i); /* [0,1,2,3,4] */

        TEST_CHECK(array_remove_swap(&a, 1) == 1, "remove_swap: removing a valid index succeeds");
        TEST_CHECK(a.count == 4, "remove_swap: count shrinks by one");
        TEST_CHECK(*(i32 *)array_get(&a, 1) == 4, "remove_swap: the last element moved into the removed slot");

        TEST_CHECK(array_remove_swap(&a, 4) == 0, "remove_swap: an out-of-range index (== new count) fails");

        TEST_CHECK(array_remove_swap(&a, 3) == 1, "remove_swap: removing the actual last element still works (no swap needed, just shrinks)");
        TEST_CHECK(a.count == 3, "remove_swap: count is 3 after two real removals");
    }

    {
        i32 buffer[2];
        Array a;
        i32 v = 7;
        array_static_init(&a, buffer, 2, sizeof(i32));
        array_push(NULL, &a, &v);
        array_clear(&a);
        TEST_CHECK(a.count == 0, "clear: count resets to 0");
        TEST_CHECK(a.capacity == 2, "clear: capacity/buffer are untouched, only count resets");
    }

    print_flush();
    return test_failures_ != 0;
}
