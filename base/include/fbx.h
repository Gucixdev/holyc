#ifndef HOLYC_FBX_H_
#define HOLYC_FBX_H_

/* FBX ASCII (the older, plain-text FBX 6.1-era format - a nested
 * `Name: value, value, ... {` block grammar) mesh reader, from scratch.
 *
 * Scope, deliberately cut and stated plainly (matching this project's
 * house style for honest gaps - see e.g. arena_alloc.h's fp64_mul/
 * fp64_div comment, bit.h's own "not implemented here" list): this file
 * does NOT read modern binary FBX (FBX 7.x and later, Autodesk's current
 * default export format) at all - that is a completely different,
 * undocumented-by-Autodesk binary node-tree format, not a variant of
 * this text grammar, and reverse-engineering it is out of scope here.
 * Within FBX ASCII itself, this reads exactly one thing: the FIRST
 * `Model: "Model::<name>", "Mesh" {` block's `Vertices:` and
 * `PolygonVertexIndex:` arrays, fan-triangulated into a flat mesh.
 * Materials, UVs, normals, animation, multiple meshes, and the rest of
 * FBX's enormous scene-graph vocabulary are not read - if a file needs
 * any of that, this is not the tool.
 *
 * Real FBX quirk this file gets right: PolygonVertexIndex does not carry
 * a separate polygon-boundary marker - the LAST vertex index of each
 * polygon is stored bitwise-complemented (stored as ~x, i.e. -x-1) so a
 * negative value in the stream means both "this is a real vertex index"
 * (after un-complementing) AND "a polygon just ended". */

#include "base.h"
#include "arena_alloc.h"

typedef enum {
    FBX_ESUCCESS   = 0,
    FBX_ENOMESH    = -1, /* no Model:...,"Mesh" block found */
    FBX_EMALFORMED = -2  /* Vertices:/PolygonVertexIndex: present but unparsable, or a
                             face exceeded FBX_MAX_FACE_VERTS */
} Fbx_Error;

#ifndef FBX_MAX_FACE_VERTS
#define FBX_MAX_FACE_VERTS 256
#endif

typedef struct {
    f32 *positions;        /* [position_count * 3] xyz, arena-owned */
    size_t position_count;
    i64 *triangle_indices; /* [triangle_count * 3], indices into positions, arena-owned */
    size_t triangle_count;
} Fbx_Mesh;

i32 fbx_decode(Arena *a, const char *data, size_t size, Fbx_Mesh *out);
const char *fbx_strerror(i32 err);

#endif /* HOLYC_FBX_H_ */

#ifdef FBX_IMPLEMENTATION
#ifndef FBX_IMPLEMENTATION_
#define FBX_IMPLEMENTATION_

#include <stdlib.h> /* strtod/strtol - parsing numeric tokens, this file's sole real need */

typedef struct { f32 *items; size_t count; size_t capacity; } Fbx_F32_List_;
typedef struct { i64 *items; size_t count; size_t capacity; } Fbx_I64_List_;

const char *fbx_strerror(i32 err)
{
    switch (err) {
    case FBX_ESUCCESS:   return "success";
    case FBX_ENOMESH:    return "no Model:...,\"Mesh\" block found";
    case FBX_EMALFORMED: return "malformed FBX ASCII input";
    default: break;
    }
    return "unknown error";
}

static const char *fbx_find_(const char *p, const char *end, const char *needle)
{
    size_t nlen = strlen(needle);
    while (p + nlen <= end) {
        if (memcmp(p, needle, nlen) == 0) return p;
        p++;
    }
    return end;
}

/* finds the matching '}' for the '{' at *p (which must point AT the '{'),
 * accounting for nested braces so a sibling block's own '{'/'}' pair
 * doesn't end the outer block early. Returns end if unbalanced. */
static const char *fbx_match_brace_(const char *p, const char *end)
{
    i32 depth = 0;
    while (p < end) {
        switch (*p) {
        case '{':
            depth++;
            break;
        case '}':
            depth--;
            if (depth == 0) return p;
            break;
        default:
            break;
        }
        p++;
    }
    return end;
}

/* parses a comma-separated list of f64 numbers starting just after a
 * ':' up to line_end, appending each as f32 to `out`. */
static i32 fbx_parse_f32_list_(Arena *a, const char *p, const char *end, Fbx_F32_List_ *out)
{
    while (p < end) {
        char *stop;
        char buf[64];
        size_t n = 0;
        f32 v;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
        if (p >= end) break;

        while (p < end && n < sizeof(buf) - 1 &&
               *p != ',' && *p != '\r' && *p != '\n') {
            buf[n++] = *p++;
        }
        if (n == 0) break;
        buf[n] = '\0';

        v = (f32)strtod(buf, &stop);
        if (stop == buf) return 0;

        arena_da_append(a, out, v);
    }
    return 1;
}

static i32 fbx_parse_i64_list_(Arena *a, const char *p, const char *end, Fbx_I64_List_ *out)
{
    while (p < end) {
        char *stop;
        i64 v;

        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',')) p++;
        if (p >= end) break;

        v = (i64)strtol(p, &stop, 10);
        if (stop == p) return 0;
        p = stop;

        arena_da_append(a, out, (i64)v);
    }
    return 1;
}

i32 fbx_decode(Arena *a, const char *data, size_t size, Fbx_Mesh *out)
{
    const char *end = data + size;
    const char *model, *mesh_open, *mesh_close;
    const char *verts_key, *verts_colon, *verts_line_end;
    const char *pvi_key, *pvi_colon, *pvi_line_end;
    Fbx_F32_List_ positions;
    Fbx_I64_List_ pvi;
    Fbx_I64_List_ triangles;
    size_t k;

    memset(out, 0, sizeof(*out));
    memset(&positions, 0, sizeof(positions));
    memset(&pvi, 0, sizeof(pvi));
    memset(&triangles, 0, sizeof(triangles));

    /* find the first Model block whose type string is "Mesh" - scan for
     * "Model:" occurrences and check each one's second quoted arg. */
    model = data;
    for (;;) {
        const char *quote2_start, *quote2_end;
        model = fbx_find_(model, end, "Model:");
        if (model == end) return FBX_ENOMESH;

        quote2_start = fbx_find_(model, end, "\"");
        if (quote2_start == end) return FBX_ENOMESH;
        quote2_start = fbx_find_(quote2_start + 1, end, "\"");
        if (quote2_start == end) return FBX_ENOMESH;
        quote2_start = fbx_find_(quote2_start + 1, end, "\"");
        if (quote2_start == end) return FBX_ENOMESH;
        quote2_end = fbx_find_(quote2_start + 1, end, "\"");
        if (quote2_end == end) return FBX_ENOMESH;

        if ((size_t)(quote2_end - (quote2_start + 1)) == 4 &&
            memcmp(quote2_start + 1, "Mesh", 4) == 0) {
            break;
        }
        model = quote2_end + 1;
    }

    mesh_open = fbx_find_(model, end, "{");
    if (mesh_open == end) return FBX_ENOMESH;
    mesh_close = fbx_match_brace_(mesh_open, end);
    if (mesh_close == end) return FBX_EMALFORMED;

    verts_key = fbx_find_(mesh_open, mesh_close, "Vertices:");
    if (verts_key == mesh_close) return FBX_EMALFORMED;
    verts_colon = verts_key + strlen("Vertices:");
    verts_line_end = fbx_find_(verts_colon, mesh_close, "\n");
    if (!fbx_parse_f32_list_(a, verts_colon, verts_line_end, &positions)) return FBX_EMALFORMED;
    if (positions.count == 0 || positions.count % 3 != 0) return FBX_EMALFORMED;

    pvi_key = fbx_find_(mesh_open, mesh_close, "PolygonVertexIndex:");
    if (pvi_key == mesh_close) return FBX_EMALFORMED;
    pvi_colon = pvi_key + strlen("PolygonVertexIndex:");
    pvi_line_end = fbx_find_(pvi_colon, mesh_close, "\n");
    if (!fbx_parse_i64_list_(a, pvi_colon, pvi_line_end, &pvi)) return FBX_EMALFORMED;
    if (pvi.count == 0) return FBX_EMALFORMED;

    /* un-complement + fan-triangulate one polygon at a time */
    {
        i64 corners[FBX_MAX_FACE_VERTS];
        size_t n = 0;

        for (k = 0; k < pvi.count; k++) {
            i64 raw = pvi.items[k];
            i32 last = raw < 0;
            i64 idx = last ? ~raw : raw;

            if ((size_t)idx * 3 + 2 >= positions.count) return FBX_EMALFORMED;
            if (n >= FBX_MAX_FACE_VERTS) return FBX_EMALFORMED;
            corners[n++] = idx;

            if (last) {
                size_t i;
                if (n < 3) return FBX_EMALFORMED;
                for (i = 1; i + 1 < n; i++) {
                    arena_da_append(a, &triangles, corners[0]);
                    arena_da_append(a, &triangles, corners[i]);
                    arena_da_append(a, &triangles, corners[i + 1]);
                }
                n = 0;
            }
        }
        if (n != 0) return FBX_EMALFORMED; /* stream ended mid-polygon */
    }

    out->positions = positions.items;
    out->position_count = positions.count / 3;
    out->triangle_indices = triangles.items;
    out->triangle_count = triangles.count / 3;

    return FBX_ESUCCESS;
}

#endif /* FBX_IMPLEMENTATION_ */
#endif /* FBX_IMPLEMENTATION */
