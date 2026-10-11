#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#include "scaffold.h"

SCAFFOLD_ARRAY_DEFINE(i32, I32_Array)

typedef struct { f32 x, y; } Vec2_;
SCAFFOLD_ARRAY_DEFINE(Vec2_, Vec2_Array)

int main(void)
{
    Arena arena = {0};
    I32_Array xs;
    Vec2_Array vs;

    print_cstr("test_scaffold\n");

    I32_Array_init(&xs);
    TEST_CHECK(xs.count == 0, "starts empty");

    I32_Array_push(&arena, &xs, 10);
    I32_Array_push(&arena, &xs, 20);
    I32_Array_push(&arena, &xs, 30);
    TEST_CHECK(xs.count == 3, "count reflects 3 pushes");
    TEST_CHECK(*I32_Array_get(&xs, 0) == 10, "first pushed value is at index 0");
    TEST_CHECK(*I32_Array_get(&xs, 2) == 30, "third pushed value is at index 2");
    TEST_CHECK(I32_Array_get(&xs, 3) == NULL, "out-of-range index returns NULL");

    I32_Array_clear(&xs);
    TEST_CHECK(xs.count == 0, "clear resets count to 0");
    I32_Array_push(&arena, &xs, 99);
    TEST_CHECK(xs.count == 1 && *I32_Array_get(&xs, 0) == 99, "the array is still usable after clear");

    /* a second, differently-typed instantiation from the same macro in
     * the same translation unit - real proof this doesn't collide */
    Vec2_Array_init(&vs);
    {
        Vec2_ a; a.x = 1.0f; a.y = 2.0f;
        Vec2_Array_push(&arena, &vs, a);
    }
    TEST_CHECK(vs.count == 1, "a second SCAFFOLD_ARRAY_DEFINE instantiation works independently");
    TEST_CHECK(Vec2_Array_get(&vs, 0)->x == 1.0f && Vec2_Array_get(&vs, 0)->y == 2.0f, "struct-typed element round-trips both fields");

    /* growth past the initial capacity - real proof arena_da_append's
     * doubling is actually wired through, not just a 1-element stub */
    {
        i32 i;
        I32_Array big;
        I32_Array_init(&big);
        for (i = 0; i < 500; i++) I32_Array_push(&arena, &big, i * 2);
        TEST_CHECK(big.count == 500, "pushes past the initial capacity all land (real growth, not silently dropped)");
        TEST_CHECK(*I32_Array_get(&big, 499) == 998, "the last of 500 pushed values is correct");
    }

    print_flush();
    return test_failures_ != 0;
}
