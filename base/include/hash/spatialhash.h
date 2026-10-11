#ifndef HOLYC_SPATIALHASH_H_
#define HOLYC_SPATIALHASH_H_

/* Sparse 3D uniform-grid spatial index: world position -> cell coords
 * -> a hashtable.h Ht bucket (built on Ht the same way hashset.h/
 * hashmap.h are, not reimplementing probing). "Sparse" is the point -
 * an unbounded world only pays for cells that actually have something
 * in them, unlike spatial.h's dense array-indexed grid which needs
 * fixed known bounds up front. Payload is a plain u32 (an entity id,
 * array index, whatever the caller's own id type is - same
 * deliberately-generic stance ecs.h/equipment.c take).
 *
 * Same two-#define requirement as hashset.h/hashmap.h: needs
 * HASHTABLE_IMPLEMENTATION defined too wherever
 * SPATIAL_HASH_IMPLEMENTATION is. */

#include "hashtable.h"
#include "mathx.h"

typedef struct Spatial_Hash_Node Spatial_Hash_Node;
struct Spatial_Hash_Node {
    u32 item;
    Spatial_Hash_Node *next;
};

typedef struct {
    Ht ht; /* packed (cx,cy,cz) cell key -> Spatial_Hash_Node* bucket head */
    f32 cell_size;
} Spatial_Hash;

void spatial_hash_init(Arena *a, Spatial_Hash *sh, f32 cell_size, size_t capacity_hint);

void spatial_hash_cell_of(const Spatial_Hash *sh, vec3 pos, i32 *cx, i32 *cy, i32 *cz);
void spatial_hash_insert(Arena *a, Spatial_Hash *sh, vec3 pos, u32 item);

/* items in exactly the one cell (cx,cy,cz) - a caller wanting a
 * radius query calls this once per neighboring cell it cares about
 * (typically the 3x3x3 block around the query point's own cell). */
size_t spatial_hash_query_cell(const Spatial_Hash *sh, i32 cx, i32 cy, i32 cz, u32 *out, size_t max_out);

#endif /* HOLYC_SPATIALHASH_H_ */

#ifdef SPATIAL_HASH_IMPLEMENTATION
#ifndef SPATIAL_HASH_IMPLEMENTATION_
#define SPATIAL_HASH_IMPLEMENTATION_

void spatial_hash_init(Arena *a, Spatial_Hash *sh, f32 cell_size, size_t capacity_hint)
{
    ht_init(a, &sh->ht, capacity_hint);
    sh->cell_size = cell_size;
}

void spatial_hash_cell_of(const Spatial_Hash *sh, vec3 pos, i32 *cx, i32 *cy, i32 *cz)
{
    /* floor-toward-negative-infinity division so negative coordinates
     * bucket correctly instead of jumping toward 0 like plain
     * truncating (i32) division would */
    f32 fx = pos.x / sh->cell_size;
    f32 fy = pos.y / sh->cell_size;
    f32 fz = pos.z / sh->cell_size;
    *cx = (i32)fx - (fx < 0.0f && fx != (f32)(i32)fx ? 1 : 0);
    *cy = (i32)fy - (fy < 0.0f && fy != (f32)(i32)fy ? 1 : 0);
    *cz = (i32)fz - (fz < 0.0f && fz != (f32)(i32)fz ? 1 : 0);
}

static void spatial_hash_pack_key_(i32 cx, i32 cy, i32 cz, i32 key[3])
{
    key[0] = cx; key[1] = cy; key[2] = cz;
}

void spatial_hash_insert(Arena *a, Spatial_Hash *sh, vec3 pos, u32 item)
{
    i32 cx, cy, cz, key[3];
    Spatial_Hash_Node *node;

    spatial_hash_cell_of(sh, pos, &cx, &cy, &cz);
    spatial_hash_pack_key_(cx, cy, cz, key);

    node = (Spatial_Hash_Node *)arena_alloc(a, sizeof(Spatial_Hash_Node));
    node->item = item;
    node->next = (Spatial_Hash_Node *)ht_get(&sh->ht, key, sizeof(key));

    ht_put(a, &sh->ht, key, sizeof(key), node);
}

size_t spatial_hash_query_cell(const Spatial_Hash *sh, i32 cx, i32 cy, i32 cz, u32 *out, size_t max_out)
{
    i32 key[3];
    Spatial_Hash_Node *node;
    size_t n = 0;

    spatial_hash_pack_key_(cx, cy, cz, key);
    node = (Spatial_Hash_Node *)ht_get(&sh->ht, key, sizeof(key));

    while (node && n < max_out) {
        out[n++] = node->item;
        node = node->next;
    }
    return n;
}

#endif /* SPATIAL_HASH_IMPLEMENTATION_ */
#endif /* SPATIAL_HASH_IMPLEMENTATION */
