#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define VECTOR_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define HASHTABLE_IMPLEMENTATION
#define SPATIAL_HASH_IMPLEMENTATION
#include "arena_alloc.h"
#include "spatialhash.h"

int main(void)
{
    Arena a = {0};
    Spatial_Hash sh;

    print_cstr("test_spatialhash\n");

    spatial_hash_init(&a, &sh, 10.0f, 4);

    {
        i32 cx, cy, cz;
        spatial_hash_cell_of(&sh, vec3_make(5.0f, 15.0f, -5.0f), &cx, &cy, &cz);
        TEST_CHECK(cx == 0 && cy == 1 && cz == -1, "cell_of buckets a position into the right cell, including negative coordinates");
    }

    spatial_hash_insert(&a, &sh, vec3_make(1.0f, 1.0f, 1.0f), 100);
    spatial_hash_insert(&a, &sh, vec3_make(2.0f, 2.0f, 2.0f), 200); /* same cell as item 100 (cell 0,0,0) */
    spatial_hash_insert(&a, &sh, vec3_make(50.0f, 50.0f, 50.0f), 300); /* a different, far-away cell */

    {
        u32 out[8];
        size_t n = spatial_hash_query_cell(&sh, 0, 0, 0, out, 8);
        TEST_CHECK(n == 2, "querying the shared cell returns both items placed in it");
        TEST_CHECK((out[0] == 100 && out[1] == 200) || (out[0] == 200 && out[1] == 100), "both expected items come back, in either bucket order");
    }

    {
        u32 out[8];
        size_t n = spatial_hash_query_cell(&sh, 5, 5, 5, out, 8);
        TEST_CHECK(n == 1 && out[0] == 300, "querying the far cell returns only the item placed there");
    }

    {
        u32 out[8];
        size_t n = spatial_hash_query_cell(&sh, 99, 99, 99, out, 8);
        TEST_CHECK(n == 0, "querying an empty cell returns 0 items");
    }

    print_flush();
    return test_failures_ != 0;
}
