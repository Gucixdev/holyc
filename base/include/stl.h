#ifndef HOLYC_STL_H_
#define HOLYC_STL_H_

/* STL (STereoLithography) mesh parser, from scratch - the format is
 * simple enough (binary: fixed 50-byte-per-triangle records; ASCII: a
 * flat keyword grammar) not to need a vendored reference decoder, same
 * reasoning wav_dec.h/obj.h give for their own formats.
 *
 * Auto-detects binary vs ASCII. The naive check ("does it start with the
 * literal bytes 'solid'?") is a known-bad heuristic: some binary STL
 * exporters put arbitrary text in the 80-byte header, and that text can
 * itself start with "solid" - which would misdetect a real binary file
 * as ASCII and then fail to parse it as text. This reader disambiguates
 * by checking whether the total file size EXACTLY matches the binary
 * format's expected size for the triangle count binary encodes at bytes
 * 80..83 (80 header + 4 count + count*50) - only treated as ASCII if
 * that arithmetic doesn't check out.
 *
 * Output shares the same flat-mesh shape as obj.h's Obj_Mesh in spirit
 * (positions + triangle list, arena-owned) but is its own independent
 * struct, not a shared type - STL's one-normal-per-triangle (not
 * per-vertex-corner like OBJ's optional vn) is a genuinely different
 * shape, and duplicating two small structs is cheaper to reason about
 * than forcing both formats through one abstraction. */

#include "base.h"
#include "arena_alloc.h"

typedef enum {
    STL_ESUCCESS   = 0,
    STL_EBADINPUT  = -1, /* too short to be any valid STL, or truncated mid-record */
    STL_EMALFORMED = -2  /* ASCII input didn't match the expected facet/vertex grammar */
} Stl_Error;

typedef struct {
    f32 *positions;        /* [triangle_count * 3 * 3] xyz per corner, arena-owned */
    f32 *normals;           /* [triangle_count * 3] xyz, one per triangle, arena-owned */
    size_t triangle_count;
} Stl_Mesh;

i32 stl_decode(Arena *a, const u8 *data, size_t size, Stl_Mesh *out);
const char *stl_strerror(i32 err);

#endif /* HOLYC_STL_H_ */

#ifdef STL_IMPLEMENTATION
#ifndef STL_IMPLEMENTATION_
#define STL_IMPLEMENTATION_

#include <stdlib.h> /* strtod - ASCII vertex/normal float parsing */

#define STL_BINARY_HEADER_SIZE_ 80
#define STL_BINARY_RECORD_SIZE_ 50

const char *stl_strerror(i32 err)
{
    switch (err) {
    case STL_ESUCCESS:   return "success";
    case STL_EBADINPUT:  return "input too short or truncated";
    case STL_EMALFORMED: return "malformed ASCII STL input";
    default: break;
    }
    return "unknown error";
}

static u32 stl_read_u32le_(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static f32 stl_read_f32le_(const u8 *p)
{
    u32 bits = stl_read_u32le_(p);
    f32 out;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

/* returns 1 if `size` is exactly consistent with the binary format for
 * the triangle count the layout itself claims at data[80..83] - see
 * file header for why this, not a text-prefix check, decides binary vs
 * ASCII. */
static i32 stl_looks_binary_(const u8 *data, size_t size)
{
    u32 tri_count;
    size_t expected;

    if (size < STL_BINARY_HEADER_SIZE_ + 4) return 0;
    tri_count = stl_read_u32le_(data + STL_BINARY_HEADER_SIZE_);
    expected = (size_t)STL_BINARY_HEADER_SIZE_ + 4 + (size_t)tri_count * STL_BINARY_RECORD_SIZE_;
    return expected == size;
}

static i32 stl_decode_binary_(Arena *a, const u8 *data, size_t size, Stl_Mesh *out)
{
    u32 tri_count, i;
    const u8 *p;

    tri_count = stl_read_u32le_(data + STL_BINARY_HEADER_SIZE_);
    p = data + STL_BINARY_HEADER_SIZE_ + 4;

    out->triangle_count = tri_count;
    out->normals = (f32 *)arena_alloc(a, (size_t)tri_count * 3 * sizeof(f32));
    out->positions = (f32 *)arena_alloc(a, (size_t)tri_count * 9 * sizeof(f32));

    for (i = 0; i < tri_count; i++) {
        size_t j;
        if ((size_t)(p - data) + STL_BINARY_RECORD_SIZE_ > size) return STL_EBADINPUT;

        for (j = 0; j < 3; j++) out->normals[i * 3 + j] = stl_read_f32le_(p + j * 4);
        p += 12;
        for (j = 0; j < 9; j++) out->positions[i * 9 + j] = stl_read_f32le_(p + j * 4);
        p += 36;
        p += 2; /* attribute byte count - ignored */
    }

    return STL_ESUCCESS;
}

static i32 stl_is_space_(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *stl_skip_ws_(const char *p, const char *end)
{
    while (p < end && stl_is_space_(*p)) p++;
    return p;
}

static const char *stl_skip_word_(const char *p, const char *end)
{
    while (p < end && !stl_is_space_(*p)) p++;
    return p;
}

static i32 stl_match_word_(const char **p, const char *end, const char *word)
{
    size_t n = arena_strlen(word);
    const char *s = stl_skip_ws_(*p, end);
    if ((size_t)(end - s) < n || memcmp(s, word, n) != 0) return 0;
    *p = s + n;
    return 1;
}

static i32 stl_parse_f32_(const char **p, const char *end, f32 *out)
{
    const char *s = stl_skip_ws_(*p, end);
    const char *tok = s;
    char buf[64];
    size_t n = 0;
    char *stop;

    while (tok < end && !stl_is_space_(*tok) && n < sizeof(buf) - 1) buf[n++] = *tok++;
    if (n == 0) return 0;
    buf[n] = '\0';

    *out = (f32)strtod(buf, &stop);
    if (stop == buf) return 0;

    *p = tok;
    return 1;
}

typedef struct { f32 *items; size_t count; size_t capacity; } Stl_F32_List_;

static i32 stl_decode_ascii_(Arena *a, const u8 *data, size_t size, Stl_Mesh *out)
{
    const char *p = (const char *)data;
    const char *end = (const char *)data + size;
    Stl_F32_List_ positions, normals;
    size_t triangle_count = 0;

    memset(&positions, 0, sizeof(positions));
    memset(&normals, 0, sizeof(normals));

    if (!stl_match_word_(&p, end, "solid")) return STL_EMALFORMED;
    p = stl_skip_word_(stl_skip_ws_(p, end), end); /* skip the (optional) solid name */

    for (;;) {
        const char *lookahead = stl_skip_ws_(p, end);
        if (stl_match_word_(&lookahead, end, "endsolid")) break;

        if (!stl_match_word_(&p, end, "facet")) return STL_EMALFORMED;
        if (!stl_match_word_(&p, end, "normal")) return STL_EMALFORMED;
        {
            f32 nx, ny, nz;
            if (!stl_parse_f32_(&p, end, &nx)) return STL_EMALFORMED;
            if (!stl_parse_f32_(&p, end, &ny)) return STL_EMALFORMED;
            if (!stl_parse_f32_(&p, end, &nz)) return STL_EMALFORMED;
            arena_da_append(a, &normals, nx);
            arena_da_append(a, &normals, ny);
            arena_da_append(a, &normals, nz);
        }

        if (!stl_match_word_(&p, end, "outer")) return STL_EMALFORMED;
        if (!stl_match_word_(&p, end, "loop")) return STL_EMALFORMED;

        {
            i32 v;
            for (v = 0; v < 3; v++) {
                f32 x, y, z;
                if (!stl_match_word_(&p, end, "vertex")) return STL_EMALFORMED;
                if (!stl_parse_f32_(&p, end, &x)) return STL_EMALFORMED;
                if (!stl_parse_f32_(&p, end, &y)) return STL_EMALFORMED;
                if (!stl_parse_f32_(&p, end, &z)) return STL_EMALFORMED;
                arena_da_append(a, &positions, x);
                arena_da_append(a, &positions, y);
                arena_da_append(a, &positions, z);
            }
        }

        if (!stl_match_word_(&p, end, "endloop")) return STL_EMALFORMED;
        if (!stl_match_word_(&p, end, "endfacet")) return STL_EMALFORMED;

        triangle_count++;
    }

    out->positions = positions.items;
    out->normals = normals.items;
    out->triangle_count = triangle_count;
    return STL_ESUCCESS;
}

i32 stl_decode(Arena *a, const u8 *data, size_t size, Stl_Mesh *out)
{
    memset(out, 0, sizeof(*out));
    /* stl_looks_binary_ already bounds-checks internally (false for
     * anything under a binary header+count) - NOT gated on a blanket
     * minimum size here, since a genuinely short but valid ASCII STL
     * (e.g. an empty "solid \nendsolid\n") is well under 84 bytes and
     * must still get a real shot at the ASCII path, not an automatic
     * reject just for being short. */
    if (size == 0) return STL_EBADINPUT;

    if (stl_looks_binary_(data, size)) {
        return stl_decode_binary_(a, data, size, out);
    }
    return stl_decode_ascii_(a, data, size, out);
}

#endif /* STL_IMPLEMENTATION_ */
#endif /* STL_IMPLEMENTATION */
