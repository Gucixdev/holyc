#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define SPATIAL_IMPLEMENTATION
#include "spatial.h"

int main(void)
{
    Arena a = {0};
    Spatial_Grid g;

    print_cstr("test_spatial\n");

    spatial_grid_init(&a, &g, 0.0f, 0.0f, 4, 4, 10.0f);

    {
        i32 cx, cy;
        TEST_CHECK(spatial_grid_cell_of(&g, 15.0f, 25.0f, &cx, &cy) == 1, "an in-bounds position resolves to a cell");
        TEST_CHECK(cx == 1 && cy == 2, "the resolved cell coordinates are correct");

        TEST_CHECK(spatial_grid_cell_of(&g, -5.0f, 5.0f, &cx, &cy) == 0, "a position left of the grid's origin is out of bounds");
        TEST_CHECK(spatial_grid_cell_of(&g, 999.0f, 5.0f, &cx, &cy) == 0, "a position past the grid's far edge is out of bounds");
    }

    spatial_grid_insert(&a, &g, 1, 2, 111);
    spatial_grid_insert(&a, &g, 1, 2, 222); /* same cell */
    spatial_grid_insert(&a, &g, 3, 3, 333); /* different cell */

    {
        u32 out[8];
        size_t n = spatial_grid_query_cell(&g, 1, 2, out, 8);
        TEST_CHECK(n == 2, "querying the shared cell returns both items placed in it");
    }
    {
        u32 out[8];
        size_t n = spatial_grid_query_cell(&g, 3, 3, out, 8);
        TEST_CHECK(n == 1 && out[0] == 333, "querying the other cell returns only its own item");
    }
    {
        u32 out[8];
        size_t n = spatial_grid_query_cell(&g, 0, 0, out, 8);
        TEST_CHECK(n == 0, "querying an untouched cell returns 0 items");
    }

    print_flush();
    return test_failures_ != 0;
}
