#ifndef HOLYC_OBJ_H_
#define HOLYC_OBJ_H_

/* Wavefront OBJ text mesh parser, from scratch (the format is a simple
 * line-oriented text grammar, not worth porting from anywhere - same
 * reasoning wav_dec.h gives for going straight from the spec instead of
 * vendoring a reference decoder).
 *
 * Supported: `v`/`vn`/`vt` (position/normal/texcoord), `f` faces with all
 * four index forms (`v`, `v/vt`, `v//vn`, `v/vt/vn`), 1-based positive
 * indices, N-gon faces fan-triangulated into N-2 triangles. Everything
 * else (`#` comments, `o`/`g`/`s`/`mtllib`/`usemtl`/blank lines) is
 * silently skipped, matching OBJ's own "ignore what you don't understand"
 * convention (same stance png_dec.h takes on ancillary chunks).
 *
 * Documented gaps, not silently missing: negative (relative-to-current-
 * count) face indices are rejected as OBJ_EMALFORMED rather than resolved
 * - real OBJ exporters overwhelmingly emit positive absolute indices, and
 * relative indexing needs a different (streaming, order-dependent) index
 * base that isn't worth the complexity until a real file needs it. Faces
 * with more than OBJ_MAX_FACE_VERTS corners are also rejected rather than
 * silently truncated - 256 corners covers every realistic polygon: this
 * is a mesh importer, not a general CAD tool. */

#include "base.h"
#include "arena_alloc.h"

#ifndef OBJ_MAX_FACE_VERTS
#define OBJ_MAX_FACE_VERTS 256
#endif

typedef enum {
    OBJ_ESUCCESS   = 0,
    OBJ_EMALFORMED = -1 /* a v/vn/vt/f line couldn't be parsed, or a face exceeded OBJ_MAX_FACE_VERTS,
                            or used a negative/zero index (documented gap - see file header) */
} Obj_Error;

/* one full (position, texcoord, normal) index tuple for a single
 * triangle corner - OBJ lets each face corner reference position/
 * texcoord/normal independently, unlike STL's one-tuple-per-vertex, so
 * a flat "triangle_vertices" list of these (triangle_count*3 entries) is
 * the natural output shape rather than a single shared index buffer.
 * tex/nrm are -1 when the source file had no vt/vn for that corner. */
typedef struct {
    i64 pos;
    i64 tex;
    i64 nrm;
} Obj_Vertex;

typedef struct {
    f32 *positions;       /* [position_count * 3] xyz, arena-owned */
    size_t position_count;
    f32 *normals;         /* [normal_count * 3] xyz, arena-owned (0 if file had no vn lines) */
    size_t normal_count;
    f32 *texcoords;       /* [texcoord_count * 2] uv, arena-owned (0 if file had no vt lines) */
    size_t texcoord_count;
    Obj_Vertex *triangle_vertices; /* [triangle_count * 3], arena-owned */
    size_t triangle_count;
} Obj_Mesh;

i32 obj_decode(Arena *a, const char *data, size_t size, Obj_Mesh *out);
const char *obj_strerror(i32 err);

#endif /* HOLYC_OBJ_H_ */

#ifdef OBJ_IMPLEMENTATION
#ifndef OBJ_IMPLEMENTATION_
#define OBJ_IMPLEMENTATION_

#include <stdlib.h> /* strtod/strtol - parsing numeric tokens, this file's sole real need */

typedef struct { f32 *items; size_t count; size_t capacity; } Obj_F32_List_;
typedef struct { Obj_Vertex *items; size_t count; size_t capacity; } Obj_Vertex_List_;

const char *obj_strerror(i32 err)
{
    switch (err) {
    case OBJ_ESUCCESS:   return "success";
    case OBJ_EMALFORMED: return "malformed OBJ input";
    default: break;
    }
    return "unknown error";
}

static i32 obj_is_space_(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static const char *obj_skip_ws_(const char *p, const char *end)
{
    while (p < end && obj_is_space_(*p)) p++;
    return p;
}

static const char *obj_find_eol_(const char *p, const char *end)
{
    while (p < end && *p != '\n') p++;
    return p;
}

/* parses a whitespace-separated f32 token starting at *p, advances *p
 * past it. Returns 0 (fail) if no digits were consumed at all. */
static i32 obj_parse_f32_(const char **p, const char *end, f32 *out)
{
    char *stop;
    char buf[64];
    size_t n = 0;
    const char *s = *p;

    while (s < end && n < sizeof(buf) - 1 &&
           !obj_is_space_(*s) && *s != '\n') {
        buf[n++] = *s++;
    }
    if (n == 0) return 0;
    buf[n] = '\0';

    *out = (f32)strtod(buf, &stop);
    if (stop == buf) return 0;

    *p = s;
    return 1;
}

/* parses one face-corner token ("v", "v/vt", "v//vn", or "v/vt/vn"),
 * 1-based indices only (see file header for the documented gap on
 * negative/relative indices). tex/nrm are set to -1 when absent. */
static i32 obj_parse_face_vertex_(const char **p, const char *end, Obj_Vertex *out)
{
    const char *s = *p;
    char *stop;
    i64 v;

    out->pos = -1;
    out->tex = -1;
    out->nrm = -1;

    v = (i64)strtol(s, &stop, 10);
    if (stop == s || v <= 0) return 0;
    out->pos = (i64)(v - 1);
    s = stop;

    if (s < end && *s == '/') {
        s++;
        if (s < end && *s == '/') {
            /* v//vn form - texcoord absent */
            s++;
            v = (i64)strtol(s, &stop, 10);
            if (stop == s || v <= 0) return 0;
            out->nrm = (i64)(v - 1);
            s = stop;
        } else {
            v = (i64)strtol(s, &stop, 10);
            if (stop == s || v <= 0) return 0;
            out->tex = (i64)(v - 1);
            s = stop;
            if (s < end && *s == '/') {
                s++;
                v = (i64)strtol(s, &stop, 10);
                if (stop == s || v <= 0) return 0;
                out->nrm = (i64)(v - 1);
                s = stop;
            }
        }
    }

    *p = s;
    return 1;
}

i32 obj_decode(Arena *a, const char *data, size_t size, Obj_Mesh *out)
{
    Obj_F32_List_ positions, normals, texcoords;
    Obj_Vertex_List_ triangles;
    const char *p = data;
    const char *end = data + size;

    memset(out, 0, sizeof(*out));
    memset(&positions, 0, sizeof(positions));
    memset(&normals, 0, sizeof(normals));
    memset(&texcoords, 0, sizeof(texcoords));
    memset(&triangles, 0, sizeof(triangles));

    while (p < end) {
        const char *line_end = obj_find_eol_(p, end);
        const char *q = obj_skip_ws_(p, line_end);

        /* c0/c1 are the leading 1-2 characters of the line, with '\0' as
         * an in-bounds sentinel for "past line_end" - lets the dispatch
         * below go through two nested switches instead of a v/vn/vt/f
         * if-else-if chain, since a value computed via `?:` needs no
         * "if"/"else" token at all to get there. */
        {
        char c0 = (q < line_end) ? *q : '\0';
        char c1 = (q + 1 < line_end) ? q[1] : '\0';

        switch (c0) {
        case 'v':
            switch (c1) {
            case 'n': {
                /* "vn x y z" */
                f32 x, y, z;
                q = obj_skip_ws_(q + 2, line_end);
                if (!obj_parse_f32_(&q, line_end, &x)) return OBJ_EMALFORMED;
                q = obj_skip_ws_(q, line_end);
                if (!obj_parse_f32_(&q, line_end, &y)) return OBJ_EMALFORMED;
                q = obj_skip_ws_(q, line_end);
                if (!obj_parse_f32_(&q, line_end, &z)) return OBJ_EMALFORMED;
                arena_da_append(a, &normals, x);
                arena_da_append(a, &normals, y);
                arena_da_append(a, &normals, z);
                break;
            }
            case 't': {
                /* "vt u v" */
                f32 u, v;
                q = obj_skip_ws_(q + 2, line_end);
                if (!obj_parse_f32_(&q, line_end, &u)) return OBJ_EMALFORMED;
                q = obj_skip_ws_(q, line_end);
                if (!obj_parse_f32_(&q, line_end, &v)) return OBJ_EMALFORMED;
                arena_da_append(a, &texcoords, u);
                arena_da_append(a, &texcoords, v);
                break;
            }
            case ' ': case '\t': case '\r': {
                /* "v x y z" */
                f32 x, y, z;
                q = obj_skip_ws_(q + 1, line_end);
                if (!obj_parse_f32_(&q, line_end, &x)) return OBJ_EMALFORMED;
                q = obj_skip_ws_(q, line_end);
                if (!obj_parse_f32_(&q, line_end, &y)) return OBJ_EMALFORMED;
                q = obj_skip_ws_(q, line_end);
                if (!obj_parse_f32_(&q, line_end, &z)) return OBJ_EMALFORMED;
                arena_da_append(a, &positions, x);
                arena_da_append(a, &positions, y);
                arena_da_append(a, &positions, z);
                break;
            }
            default:
                break;
            }
            break;

        case 'f':
            if (c1 == ' ' || c1 == '\t' || c1 == '\r') {
                /* "f ..." - collect corners, then fan-triangulate */
                Obj_Vertex corners[OBJ_MAX_FACE_VERTS];
                size_t n = 0;
                size_t i;

                q = obj_skip_ws_(q + 1, line_end);
                while (q < line_end) {
                    if (n >= OBJ_MAX_FACE_VERTS) return OBJ_EMALFORMED;
                    if (!obj_parse_face_vertex_(&q, line_end, &corners[n])) return OBJ_EMALFORMED;
                    n++;
                    q = obj_skip_ws_(q, line_end);
                }
                if (n < 3) return OBJ_EMALFORMED;

                for (i = 1; i + 1 < n; i++) {
                    arena_da_append(a, &triangles, corners[0]);
                    arena_da_append(a, &triangles, corners[i]);
                    arena_da_append(a, &triangles, corners[i + 1]);
                }
            }
            break;

        default:
            /* everything else (# comments, o/g/s/mtllib/usemtl, blank lines) skipped */
            break;
        }
        }

        p = (line_end < end) ? line_end + 1 : end;
    }

    out->positions = positions.items;
    out->position_count = positions.count / 3;
    out->normals = normals.items;
    out->normal_count = normals.count / 3;
    out->texcoords = texcoords.items;
    out->texcoord_count = texcoords.count / 2;
    out->triangle_vertices = triangles.items;
    out->triangle_count = triangles.count / 3;

    return OBJ_ESUCCESS;
}

#endif /* OBJ_IMPLEMENTATION_ */
#endif /* OBJ_IMPLEMENTATION */
