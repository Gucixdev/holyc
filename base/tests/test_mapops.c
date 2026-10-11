#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "core/arena_alloc.h"
#define ARRAY_IMPLEMENTATION
#define MAPOPS_IMPLEMENTATION
#include "data/mapops.h"

static i32 is_even_(const void *elem, void *userdata)
{
    (void)userdata;
    return (*(const i32 *)elem) % 2 == 0;
}

static void double_it_(void *elem, void *userdata)
{
    (void)userdata;
    *(i32 *)elem *= 2;
}

static void sum_(void *acc, const void *elem, void *userdata)
{
    (void)userdata;
    *(i32 *)acc += *(const i32 *)elem;
}

int main(void)
{
    Arena a = {0};
    Array src, dst;
    i32 i;

    print_cstr("test_mapops\n");

    array_dynamic_init(&src, sizeof(i32));
    for (i = 1; i <= 6; i++) array_push(&a, &src, &i);

    array_dynamic_init(&dst, sizeof(i32));
    {
        size_t n = mapops_filter(&a, &dst, &src, is_even_, NULL);
        TEST_CHECK(n == 3, "filter keeps exactly the elements matching the predicate (evens out of 1..6)");
        TEST_CHECK(*(i32 *)array_get(&dst, 0) == 2 && *(i32 *)array_get(&dst, 2) == 6, "filtered elements preserve their original relative order");
    }

    mapops_foreach(&src, double_it_, NULL);
    TEST_CHECK(*(i32 *)array_get(&src, 0) == 2 && *(i32 *)array_get(&src, 5) == 12, "foreach mutates every element in place");

    {
        i32 total = 0;
        mapops_fold(&src, &total, sum_, NULL);
        TEST_CHECK(total == 2+4+6+8+10+12, "fold accumulates across every element in order");
    }

    print_flush();
    return test_failures_ != 0;
}
