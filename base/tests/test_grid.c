#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define GRID_IMPLEMENTATION
#include "arena_alloc.h"
#include "grid.h"

int main(void)
{
    Arena a = {0};
    Grid2D g;

    print_cstr("test_grid\n");

    grid2d_init(&a, &g, 10.0f, 10.0f, 8, 8); /* 10x10 world units per cell, 8x8 cells -> 80x80 world */

    /* insert 3 objects into 3 different cells */
    grid2d_insert(&a, &g, 1, 5.0f, 5.0f);   /* cell (0,0) */
    grid2d_insert(&a, &g, 2, 15.0f, 5.0f);  /* cell (1,0) */
    grid2d_insert(&a, &g, 3, 5.0f, 15.0f);  /* cell (0,1) */

    /* query a single cell finds exactly the object placed there */
    {
        i64 ids[8];
        size_t n = grid2d_query_cell(&g, 0, 0, ids, 8);
        TEST_CHECK(n == 1 && ids[0] == 1, "grid2d_query_cell(0,0) finds exactly object 1");
    }
    {
        i64 ids[8];
        size_t n = grid2d_query_cell(&g, 1, 0, ids, 8);
        TEST_CHECK(n == 1 && ids[0] == 2, "grid2d_query_cell(1,0) finds exactly object 2");
    }

    /* an empty cell reports zero results, not a crash */
    {
        i64 ids[8];
        size_t n = grid2d_query_cell(&g, 7, 7, ids, 8);
        TEST_CHECK(n == 0, "grid2d_query_cell on an empty cell reports 0 results");
    }

    /* multiple objects in the same cell are all found */
    grid2d_insert(&a, &g, 4, 6.0f, 6.0f); /* also cell (0,0) */
    {
        i64 ids[8];
        size_t n = grid2d_query_cell(&g, 0, 0, ids, 8);
        i32 saw1 = 0, saw4 = 0;
        size_t i;
        for (i = 0; i < n; i++) {
            if (ids[i] == 1) saw1 = 1;
            if (ids[i] == 4) saw4 = 1;
        }
        TEST_CHECK(n == 2 && saw1 && saw4, "grid2d_query_cell(0,0) finds both objects sharing that cell");
    }

    /* a region query spanning multiple cells finds everything inside it */
    {
        i64 ids[16];
        size_t n = grid2d_query_region(&g, 0.0f, 0.0f, 20.0f, 20.0f, ids, 16);
        TEST_CHECK(n == 4, "grid2d_query_region covering cells (0,0),(1,0),(0,1) finds all 4 objects");
    }

    /* a narrower region query excludes objects outside it */
    {
        i64 ids[16];
        size_t n = grid2d_query_region(&g, 0.0f, 0.0f, 9.0f, 9.0f, ids, 16);
        i32 saw2 = 0, saw3 = 0;
        size_t i;
        for (i = 0; i < n; i++) {
            if (ids[i] == 2) saw2 = 1;
            if (ids[i] == 3) saw3 = 1;
        }
        TEST_CHECK(n == 2 && !saw2 && !saw3, "grid2d_query_region excludes objects in cells outside the queried range");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
