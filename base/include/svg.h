#ifndef HOLYC_SVG_H_
#define HOLYC_SVG_H_

/* SVG parser, from scratch, for a deliberately bounded subset - a real
 * hand-written tokenizer for exactly this subset, not a general XML
 * parser (SVG's full grammar is a whole document format on its own).
 *
 * Supported (stated plainly, matching this project's house style for
 * honest scope cuts - see arena_alloc.h's fp64_mul/fp64_div comment,
 * bit.h's own "not implemented here" list):
 *   - root `<svg width="" height="" viewBox="minx miny w h">` attributes
 *   - `<rect x y width height>`, `<circle cx cy r>`, `<line x1 y1 x2 y2>`,
 *     `<polyline points="x,y x,y ...">`
 *   - `<path d="...">` with ONLY the `M` (moveto) / `L` (lineto) / `C`
 *     (cubic bezier) commands, ABSOLUTE coordinates only
 *
 * Explicitly NOT supported (documented gaps, not silently missing):
 *   - `<g>` transform stacks / any `transform=` attribute at all
 *   - CSS (`style=`, `class=`, `<style>` blocks, presentation attributes
 *     beyond raw geometry)
 *   - gradients, patterns, clipping, masks
 *   - `<text>`
 *   - the rest of the path grammar: arcs (`A`), quadratic curves (`Q`/
 *     `T`), relative lowercase commands (`m`/`l`/`c`/...), shorthand
 *     commands (`H`/`V`/`S`), and IMPLICIT command repetition (a bare
 *     coordinate pair immediately following a command without repeating
 *     its letter, e.g. "M0,0 L10,0 10,10" - the second pair here would
 *     need to be read as an implicit second L, which this parser does
 *     NOT do; every coordinate pair needs its own command letter)
 *
 * Parses into a flat, arena-owned list of drawable primitives - no
 * scene-graph, no nesting, document order only. */

#include "base.h"
#include "arena_alloc.h"

typedef enum {
    SVG_ESUCCESS   = 0,
    SVG_ENOSVG     = -1, /* no <svg ...> root element found */
    SVG_EMALFORMED = -2  /* a recognized element's required attributes couldn't be parsed,
                             or a <path> used an unsupported command */
} Svg_Error;

typedef enum {
    SVG_PRIM_RECT,
    SVG_PRIM_CIRCLE,
    SVG_PRIM_LINE,
    SVG_PRIM_POLYLINE,
    SVG_PRIM_PATH
} Svg_Prim_Kind;

typedef enum {
    SVG_PATH_MOVETO,
    SVG_PATH_LINETO,
    SVG_PATH_CUBICTO
} Svg_Path_Cmd_Kind;

typedef struct {
    Svg_Path_Cmd_Kind kind;
    f32 x1, y1; /* first control point - SVG_PATH_CUBICTO only */
    f32 x2, y2; /* second control point - SVG_PATH_CUBICTO only */
    f32 x, y;   /* endpoint - all three kinds */
} Svg_Path_Cmd;

typedef struct {
    Svg_Prim_Kind kind;
    union {
        struct { f32 x, y, w, h; } rect;
        struct { f32 cx, cy, r; } circle;
        struct { f32 x1, y1, x2, y2; } line;
        struct { f32 *points; size_t point_count; /* point_count = number of (x,y) pairs */ } polyline;
        struct { Svg_Path_Cmd *cmds; size_t cmd_count; } path;
    } as;
} Svg_Prim;

typedef struct {
    f32 width, height;
    f32 vb_min_x, vb_min_y, vb_width, vb_height; /* viewBox - defaults to (0,0,width,height) if the attribute is absent */
    Svg_Prim *prims; /* arena-owned */
    size_t prim_count;
} Svg_Doc;

i32 svg_decode(Arena *a, const char *data, size_t size, Svg_Doc *out);
const char *svg_strerror(i32 err);

#endif /* HOLYC_SVG_H_ */

#ifdef SVG_IMPLEMENTATION
#ifndef SVG_IMPLEMENTATION_
#define SVG_IMPLEMENTATION_

#include <stdlib.h> /* strtod - parsing numeric attribute values, this file's sole real need */

typedef struct { Svg_Prim *items; size_t count; size_t capacity; } Svg_Prim_List_;
typedef struct { f32 *items; size_t count; size_t capacity; } Svg_F32_List_;
typedef struct { Svg_Path_Cmd *items; size_t count; size_t capacity; } Svg_Cmd_List_;

const char *svg_strerror(i32 err)
{
    switch (err) {
    case SVG_ESUCCESS:   return "success";
    case SVG_ENOSVG:     return "no <svg> root element found";
    case SVG_EMALFORMED: return "malformed or unsupported SVG content";
    default: break;
    }
    return "unknown error";
}

static i32 svg_is_space_(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* finds the next '<tagname' start (skipping '</...>' closing tags,
 * '<!--...-->' comments, and '<?...?>' processing instructions), and the
 * span of its attribute area (from just after the name to the '>' or the
 * '/' before a self-closing '/>'). Returns 0 if no more tags exist. */
static i32 svg_next_tag_(const char *p, const char *end,
                          const char **name, size_t *name_len,
                          const char **attrs, const char **attrs_end,
                          const char **after)
{
    for (;;) {
        while (p < end && *p != '<') p++;
        if (p >= end) return 0;

        if (p + 1 < end && p[1] == '/') {
            p = memchr(p, '>', (size_t)(end - p)) ? memchr(p, '>', (size_t)(end - p)) : end;
            if (p < end) p++;
            continue;
        }
        if (p + 3 < end && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            const char *close = p + 4;
            while (close + 2 < end && !(close[0] == '-' && close[1] == '-' && close[2] == '>')) close++;
            p = (close + 2 < end) ? close + 3 : end;
            continue;
        }
        if (p + 1 < end && p[1] == '?') {
            const char *close = memchr(p, '>', (size_t)(end - p));
            p = close ? close + 1 : end;
            continue;
        }

        p++; /* past '<' */
        *name = p;
        while (p < end && !svg_is_space_(*p) && *p != '>' && *p != '/') p++;
        *name_len = (size_t)(p - *name);
        if (*name_len == 0) { p++; continue; }

        *attrs = p;
        while (p < end && *p != '>') p++;
        *attrs_end = (p > *attrs && p[-1] == '/') ? p - 1 : p;
        *after = (p < end) ? p + 1 : end;
        return 1;
    }
}

static i32 svg_tag_is_(const char *name, size_t name_len, const char *literal)
{
    size_t llen = strlen(literal);
    return name_len == llen && memcmp(name, literal, llen) == 0;
}

/* finds attr="value" within [attrs, attrs_end), returns the value span
 * (NOT including the quotes) or 0 if the attribute isn't present. */
static i32 svg_find_attr_(const char *attrs, const char *attrs_end, const char *key,
                           const char **val, size_t *val_len)
{
    size_t klen = strlen(key);
    const char *p = attrs;
    while (p < attrs_end) {
        while (p < attrs_end && svg_is_space_(*p)) p++;
        if (p + klen < attrs_end && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *q = p + klen + 1;
            char quote;
            if (q >= attrs_end || (*q != '"' && *q != '\'')) return 0;
            quote = *q++;
            *val = q;
            while (q < attrs_end && *q != quote) q++;
            *val_len = (size_t)(q - *val);
            return 1;
        }
        while (p < attrs_end && !svg_is_space_(*p)) p++;
    }
    return 0;
}

static f32 svg_attr_f32_(const char *attrs, const char *attrs_end, const char *key, f32 fallback)
{
    const char *val;
    size_t val_len;
    char buf[64];
    if (!svg_find_attr_(attrs, attrs_end, key, &val, &val_len)) return fallback;
    if (val_len >= sizeof(buf)) val_len = sizeof(buf) - 1;
    memcpy(buf, val, val_len);
    buf[val_len] = '\0';
    return (f32)strtod(buf, 0);
}

/* parses the next whitespace/comma-separated f32 token starting at *p
 * (advancing *p past it and any leading separators). Returns 0 on
 * failure (no digits consumed) without advancing *p. */
static i32 svg_parse_f32_(const char **p, const char *end, f32 *out)
{
    const char *s = *p;
    char *stop;
    char buf[64];
    size_t n = 0;

    while (s < end && (svg_is_space_(*s) || *s == ',')) s++;
    while (s < end && n < sizeof(buf) - 1 &&
           !svg_is_space_(*s) && *s != ',' &&
           !((*s == 'M' || *s == 'L' || *s == 'C') && n > 0)) {
        buf[n++] = *s++;
    }
    if (n == 0) return 0;
    buf[n] = '\0';

    *out = (f32)strtod(buf, &stop);
    if (stop == buf) return 0;

    *p = s;
    return 1;
}

static i32 svg_parse_polyline_points_(Arena *a, const char *val, const char *val_end, Svg_F32_List_ *out)
{
    const char *p = val;
    while (p < val_end) {
        f32 v;
        while (p < val_end && (svg_is_space_(*p) || *p == ',')) p++;
        if (p >= val_end) break;
        if (!svg_parse_f32_(&p, val_end, &v)) return 0;
        arena_da_append(a, out, v);
    }
    return out->count > 0 && out->count % 2 == 0;
}

/* maps a tag name to the Svg_Prim_Kind it produces, or -1 if this parser
 * doesn't understand it - a table + loop instead of an else-if chain so
 * the actual per-tag construction logic below can dispatch through a
 * switch (which - unlike a chain of string comparisons - the C89
 * standard's own switch statement can't do directly on a name, hence
 * needing this one indirection step to get to an integer kind first). */
typedef struct { const char *name; i32 kind; } Svg_Tag_Entry_;

static i32 svg_tag_kind_(const char *name, size_t name_len)
{
    static const Svg_Tag_Entry_ table[] = {
        { "rect",     SVG_PRIM_RECT },
        { "circle",   SVG_PRIM_CIRCLE },
        { "line",     SVG_PRIM_LINE },
        { "polyline", SVG_PRIM_POLYLINE },
        { "path",     SVG_PRIM_PATH }
    };
    size_t i;
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (svg_tag_is_(name, name_len, table[i].name)) return table[i].kind;
    }
    return -1;
}

static i32 svg_parse_path_d_(Arena *a, const char *val, const char *val_end, Svg_Cmd_List_ *out)
{
    const char *p = val;
    while (p < val_end) {
        char letter;
        Svg_Path_Cmd cmd;

        while (p < val_end && svg_is_space_(*p)) p++;
        if (p >= val_end) break;

        letter = *p;
        p++;

        memset(&cmd, 0, sizeof(cmd));
        switch (letter) {
        case 'M':
            cmd.kind = SVG_PATH_MOVETO;
            if (!svg_parse_f32_(&p, val_end, &cmd.x)) return 0;
            if (!svg_parse_f32_(&p, val_end, &cmd.y)) return 0;
            break;
        case 'L':
            cmd.kind = SVG_PATH_LINETO;
            if (!svg_parse_f32_(&p, val_end, &cmd.x)) return 0;
            if (!svg_parse_f32_(&p, val_end, &cmd.y)) return 0;
            break;
        case 'C':
            cmd.kind = SVG_PATH_CUBICTO;
            if (!svg_parse_f32_(&p, val_end, &cmd.x1)) return 0;
            if (!svg_parse_f32_(&p, val_end, &cmd.y1)) return 0;
            if (!svg_parse_f32_(&p, val_end, &cmd.x2)) return 0;
            if (!svg_parse_f32_(&p, val_end, &cmd.y2)) return 0;
            if (!svg_parse_f32_(&p, val_end, &cmd.x)) return 0;
            if (!svg_parse_f32_(&p, val_end, &cmd.y)) return 0;
            break;
        default:
            return 0; /* unsupported command - see file header */
        }
        arena_da_append(a, out, cmd);
    }
    return out->count > 0;
}

i32 svg_decode(Arena *a, const char *data, size_t size, Svg_Doc *out)
{
    const char *p = data;
    const char *end = data + size;
    const char *name, *attrs, *attrs_end, *after;
    size_t name_len;
    Svg_Prim_List_ prims;
    i32 found_svg = 0;

    memset(out, 0, sizeof(*out));
    memset(&prims, 0, sizeof(prims));

    while (svg_next_tag_(p, end, &name, &name_len, &attrs, &attrs_end, &after)) {
        if (!found_svg) {
            if (!svg_tag_is_(name, name_len, "svg")) { p = after; continue; }
            found_svg = 1;

            out->width = svg_attr_f32_(attrs, attrs_end, "width", 0.0f);
            out->height = svg_attr_f32_(attrs, attrs_end, "height", 0.0f);

            {
                const char *vb; size_t vb_len;
                if (svg_find_attr_(attrs, attrs_end, "viewBox", &vb, &vb_len)) {
                    const char *vp = vb, *vend = vb + vb_len;
                    f32 vals[4];
                    i32 i, ok = 1;
                    for (i = 0; i < 4; i++) {
                        if (!svg_parse_f32_(&vp, vend, &vals[i])) { ok = 0; break; }
                    }
                    if (ok) {
                        out->vb_min_x = vals[0]; out->vb_min_y = vals[1];
                        out->vb_width = vals[2]; out->vb_height = vals[3];
                    } else {
                        out->vb_width = out->width; out->vb_height = out->height;
                    }
                } else {
                    out->vb_width = out->width; out->vb_height = out->height;
                }
            }

            p = after;
            continue;
        }

        switch (svg_tag_kind_(name, name_len)) {
        case SVG_PRIM_RECT: {
            Svg_Prim prim;
            prim.kind = SVG_PRIM_RECT;
            prim.as.rect.x = svg_attr_f32_(attrs, attrs_end, "x", 0.0f);
            prim.as.rect.y = svg_attr_f32_(attrs, attrs_end, "y", 0.0f);
            prim.as.rect.w = svg_attr_f32_(attrs, attrs_end, "width", 0.0f);
            prim.as.rect.h = svg_attr_f32_(attrs, attrs_end, "height", 0.0f);
            arena_da_append(a, &prims, prim);
            break;
        }
        case SVG_PRIM_CIRCLE: {
            Svg_Prim prim;
            prim.kind = SVG_PRIM_CIRCLE;
            prim.as.circle.cx = svg_attr_f32_(attrs, attrs_end, "cx", 0.0f);
            prim.as.circle.cy = svg_attr_f32_(attrs, attrs_end, "cy", 0.0f);
            prim.as.circle.r = svg_attr_f32_(attrs, attrs_end, "r", 0.0f);
            arena_da_append(a, &prims, prim);
            break;
        }
        case SVG_PRIM_LINE: {
            Svg_Prim prim;
            prim.kind = SVG_PRIM_LINE;
            prim.as.line.x1 = svg_attr_f32_(attrs, attrs_end, "x1", 0.0f);
            prim.as.line.y1 = svg_attr_f32_(attrs, attrs_end, "y1", 0.0f);
            prim.as.line.x2 = svg_attr_f32_(attrs, attrs_end, "x2", 0.0f);
            prim.as.line.y2 = svg_attr_f32_(attrs, attrs_end, "y2", 0.0f);
            arena_da_append(a, &prims, prim);
            break;
        }
        case SVG_PRIM_POLYLINE: {
            const char *val; size_t val_len;
            Svg_F32_List_ pts;
            memset(&pts, 0, sizeof(pts));
            if (!svg_find_attr_(attrs, attrs_end, "points", &val, &val_len)) return SVG_EMALFORMED;
            if (!svg_parse_polyline_points_(a, val, val + val_len, &pts)) return SVG_EMALFORMED;
            {
                Svg_Prim prim;
                prim.kind = SVG_PRIM_POLYLINE;
                prim.as.polyline.points = pts.items;
                prim.as.polyline.point_count = pts.count / 2;
                arena_da_append(a, &prims, prim);
            }
            break;
        }
        case SVG_PRIM_PATH: {
            const char *val; size_t val_len;
            Svg_Cmd_List_ cmds;
            memset(&cmds, 0, sizeof(cmds));
            if (!svg_find_attr_(attrs, attrs_end, "d", &val, &val_len)) return SVG_EMALFORMED;
            if (!svg_parse_path_d_(a, val, val + val_len, &cmds)) return SVG_EMALFORMED;
            {
                Svg_Prim prim;
                prim.kind = SVG_PRIM_PATH;
                prim.as.path.cmds = cmds.items;
                prim.as.path.cmd_count = cmds.count;
                arena_da_append(a, &prims, prim);
            }
            break;
        }
        default:
            /* any other tag (<defs>, <style>, <text>, ...) is silently
               skipped - this parser only understands the 5 primitives
               listed in the file header */
            break;
        }

        p = after;
    }

    if (!found_svg) return SVG_ENOSVG;

    out->prims = prims.items;
    out->prim_count = prims.count;
    return SVG_ESUCCESS;
}

#endif /* SVG_IMPLEMENTATION_ */
#endif /* SVG_IMPLEMENTATION */
