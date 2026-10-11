#ifndef HOLYC_SPATIAL_H_
#define HOLYC_SPATIAL_H_

/* Dense 2D uniform grid spatial index: fixed known world bounds
 * (grid_w x grid_h cells, decided at init) indexed directly by a flat
 * array - no hashing, no probing, just cy*grid_w+cx. Cheaper than
 * hash/spatialhash.h's sparse Ht-backed grid whenever the world's
 * bounds really are known and bounded (tilemaps, minimaps, 2D level
 * grids - the kind of thing gamedev/map/'s heatmap.h/worldmap.h
 * neighbors this file suggest), at the cost of needing that upfront
 * bound and eagerly allocating grid_w*grid_h bucket-head slots even
 * for empty cells. Bucket-per-cell linked list, same shape as
 * hash/spatialhash.h's Spatial_Hash_Node (deliberately not shared -
 * this project's "own local copy, not cross-file dependency" rule). */

#include "base.h"
#include "arena_alloc.h"

typedef struct Spatial_Node Spatial_Node;
struct Spatial_Node {
    u32 item;
    Spatial_Node *next;
};

typedef struct {
    Spatial_Node **cells; /* arena-owned, [grid_w*grid_h] bucket heads */
    i32 grid_w, grid_h;
    f32 origin_x, origin_y; /* world-space position of cell (0,0)'s corner */
    f32 cell_size;
} Spatial_Grid;

void spatial_grid_init(Arena *a, Spatial_Grid *g, f32 origin_x, f32 origin_y, i32 grid_w, i32 grid_h, f32 cell_size);

/* returns 0 (and leaves cx and cy untouched) if (x,y) falls outside
 * the grid's bounds - callers MUST check this before insert/query,
 * out-of-bounds coordinates are a caller error, not silently clamped. */
i32 spatial_grid_cell_of(const Spatial_Grid *g, f32 x, f32 y, i32 *cx, i32 *cy);

void spatial_grid_insert(Arena *a, Spatial_Grid *g, i32 cx, i32 cy, u32 item);
size_t spatial_grid_query_cell(const Spatial_Grid *g, i32 cx, i32 cy, u32 *out, size_t max_out);

#endif /* HOLYC_SPATIAL_H_ */

#ifdef SPATIAL_IMPLEMENTATION
#ifndef SPATIAL_IMPLEMENTATION_
#define SPATIAL_IMPLEMENTATION_

void spatial_grid_init(Arena *a, Spatial_Grid *g, f32 origin_x, f32 origin_y, i32 grid_w, i32 grid_h, f32 cell_size)
{
    i32 i;
    size_t n = (size_t)grid_w * (size_t)grid_h;

    g->cells = (Spatial_Node **)arena_alloc(a, n * sizeof(Spatial_Node *));
    for (i = 0; i < grid_w * grid_h; i++) g->cells[i] = 0;

    g->grid_w = grid_w;
    g->grid_h = grid_h;
    g->origin_x = origin_x;
    g->origin_y = origin_y;
    g->cell_size = cell_size;
}

i32 spatial_grid_cell_of(const Spatial_Grid *g, f32 x, f32 y, i32 *cx, i32 *cy)
{
    f32 fx = (x - g->origin_x) / g->cell_size;
    f32 fy = (y - g->origin_y) / g->cell_size;
    i32 icx, icy;

    if (fx < 0.0f || fy < 0.0f) return 0;

    icx = (i32)fx;
    icy = (i32)fy;
    if (icx >= g->grid_w || icy >= g->grid_h) return 0;

    *cx = icx;
    *cy = icy;
    return 1;
}

void spatial_grid_insert(Arena *a, Spatial_Grid *g, i32 cx, i32 cy, u32 item)
{
    size_t idx = (size_t)cy * (size_t)g->grid_w + (size_t)cx;
    Spatial_Node *node = (Spatial_Node *)arena_alloc(a, sizeof(Spatial_Node));
    node->item = item;
    node->next = g->cells[idx];
    g->cells[idx] = node;
}

size_t spatial_grid_query_cell(const Spatial_Grid *g, i32 cx, i32 cy, u32 *out, size_t max_out)
{
    size_t idx = (size_t)cy * (size_t)g->grid_w + (size_t)cx;
    Spatial_Node *node = g->cells[idx];
    size_t n = 0;

    while (node && n < max_out) {
        out[n++] = node->item;
        node = node->next;
    }
    return n;
}

#endif /* SPATIAL_IMPLEMENTATION_ */
#endif /* SPATIAL_IMPLEMENTATION */
