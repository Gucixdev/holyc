#ifndef HOLYC_GRID_H_
#define HOLYC_GRID_H_

/* Uniform 2D spatial grid - a simple, real alternative/complement to
 * broadphase.h's sweep-and-prune: divide the world into fixed-size
 * cells, each holding the ids of objects whose POINT position falls
 * inside it (not their full extent - a real, documented scope: an
 * object near a cell boundary is only found in the one cell its
 * position point falls into, not every cell its bounding shape might
 * overlap; a caller with real extents wanting exhaustive overlap
 * coverage should insert into every cell the extent touches instead,
 * which this file's single-point `grid2d_insert` doesn't do
 * automatically). Arena-allocated, using arena_alloc.h's own growable-
 * array convention per cell (see arena_da_append). */

#include "base.h"
#include "arena_alloc.h"

typedef struct {
    i64 *items;
    size_t count;
    size_t capacity;
} Grid2D_Cell;

typedef struct {
    f32 cell_w, cell_h;
    i32 cells_x, cells_y;
    Grid2D_Cell *cells; /* [cells_x * cells_y], row-major (cy * cells_x + cx) */
} Grid2D;

void grid2d_init(Arena *a, Grid2D *g, f32 cell_w, f32 cell_h, i32 cells_x, i32 cells_y);

/* inserts object `id` at world position (x,y) - clamped into the grid's
 * own bounds if the position falls outside it, rather than silently
 * dropped or a hard error (a real, common case: an object at the edge
 * of the simulated world shouldn't just vanish from the grid). */
void grid2d_insert(Arena *a, Grid2D *g, i64 id, f32 x, f32 y);

/* writes up to out_capacity object ids found in cell (cx,cy) into
 * out_ids; returns how many were written. Out-of-range (cx,cy) returns
 * 0, not a crash. */
size_t grid2d_query_cell(const Grid2D *g, i32 cx, i32 cy, i64 *out_ids, size_t out_capacity);

/* writes up to out_capacity object ids found in every cell overlapping
 * the world-space rectangle [min_x,max_x] x [min_y,max_y] into out_ids
 * (may contain duplicates if the SAME id happens to be inserted more
 * than once - a single-point insert never causes this on its own, but
 * this function does not itself deduplicate a caller's own repeated
 * inserts of the same id, that's the caller's responsibility if it
 * matters to them); returns how many were written. */
size_t grid2d_query_region(const Grid2D *g, f32 min_x, f32 min_y, f32 max_x, f32 max_y,
                            i64 *out_ids, size_t out_capacity);

#endif /* HOLYC_GRID_H_ */

#ifdef GRID_IMPLEMENTATION
#ifndef GRID_IMPLEMENTATION_
#define GRID_IMPLEMENTATION_

void grid2d_init(Arena *a, Grid2D *g, f32 cell_w, f32 cell_h, i32 cells_x, i32 cells_y)
{
    size_t n = (size_t)cells_x * (size_t)cells_y;
    size_t i;

    g->cell_w = cell_w;
    g->cell_h = cell_h;
    g->cells_x = cells_x;
    g->cells_y = cells_y;
    g->cells = (Grid2D_Cell *)arena_alloc(a, n * sizeof(Grid2D_Cell));
    for (i = 0; i < n; i++) {
        g->cells[i].items = 0;
        g->cells[i].count = 0;
        g->cells[i].capacity = 0;
    }
}

static i32 grid2d_clamp_(i32 v, i32 lo, i32 hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void grid2d_insert(Arena *a, Grid2D *g, i64 id, f32 x, f32 y)
{
    i32 cx = grid2d_clamp_((i32)(x / g->cell_w), 0, g->cells_x - 1);
    i32 cy = grid2d_clamp_((i32)(y / g->cell_h), 0, g->cells_y - 1);
    Grid2D_Cell *cell = &g->cells[(size_t)cy * (size_t)g->cells_x + (size_t)cx];
    arena_da_append(a, cell, id);
}

size_t grid2d_query_cell(const Grid2D *g, i32 cx, i32 cy, i64 *out_ids, size_t out_capacity)
{
    const Grid2D_Cell *cell;
    size_t n, i;

    if (cx < 0 || cx >= g->cells_x || cy < 0 || cy >= g->cells_y) return 0;

    cell = &g->cells[(size_t)cy * (size_t)g->cells_x + (size_t)cx];
    n = cell->count < out_capacity ? cell->count : out_capacity;
    for (i = 0; i < n; i++) out_ids[i] = cell->items[i];
    return n;
}

size_t grid2d_query_region(const Grid2D *g, f32 min_x, f32 min_y, f32 max_x, f32 max_y,
                            i64 *out_ids, size_t out_capacity)
{
    i32 cx0 = grid2d_clamp_((i32)(min_x / g->cell_w), 0, g->cells_x - 1);
    i32 cy0 = grid2d_clamp_((i32)(min_y / g->cell_h), 0, g->cells_y - 1);
    i32 cx1 = grid2d_clamp_((i32)(max_x / g->cell_w), 0, g->cells_x - 1);
    i32 cy1 = grid2d_clamp_((i32)(max_y / g->cell_h), 0, g->cells_y - 1);
    size_t written = 0;
    i32 cy, cx;

    for (cy = cy0; cy <= cy1; cy++) {
        for (cx = cx0; cx <= cx1; cx++) {
            const Grid2D_Cell *cell = &g->cells[(size_t)cy * (size_t)g->cells_x + (size_t)cx];
            size_t i;
            for (i = 0; i < cell->count && written < out_capacity; i++) {
                out_ids[written++] = cell->items[i];
            }
        }
    }

    return written;
}

#endif /* GRID_IMPLEMENTATION_ */
#endif /* GRID_IMPLEMENTATION */
