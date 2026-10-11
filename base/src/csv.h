#ifndef HOLYC_CSV_H_
#define HOLYC_CSV_H_

/* RFC 4180-shaped CSV parser: comma-separated fields, "..."-quoted
 * fields (comma/newline only legal unquoted outside a quoted field),
 * "" as the escaped-quote-inside-a-quoted-field rule, \r\n or bare \n
 * line endings both accepted.
 *
 * Each field is a String_View (src/core/string_view.h). An UNQUOTED
 * field's view points directly into the caller's input buffer - no
 * copy. A QUOTED field only gets copied into arena-owned memory when
 * unescaping actually changes its bytes (i.e. it contains a "" pair);
 * a quoted field with no embedded "" still points at the original
 * bytes between the quotes, unmodified. This mirrors wav_dec.h's own
 * "don't copy what you don't have to" stance, just applied per-field
 * instead of per-file. */

#include "base.h"
#include "arena_alloc.h"
#include "str.h"

typedef enum {
    CSV_ESUCCESS   = 0,
    CSV_EUNCLOSED  = -1 /* a quoted field's closing " was never found before EOF */
} Csv_Error;

typedef struct {
    String_View *items;
    size_t count;
    size_t capacity;
} Csv_Row;

typedef struct {
    Csv_Row *items;
    size_t count;
    size_t capacity;
} Csv_Table;

i32 csv_parse(Arena *a, const char *data, size_t size, Csv_Table *out);
const char *csv_strerror(i32 err);

#endif /* HOLYC_CSV_H_ */

#ifdef CSV_IMPLEMENTATION
#ifndef CSV_IMPLEMENTATION_
#define CSV_IMPLEMENTATION_

const char *csv_strerror(i32 err)
{
    switch (err) {
    case CSV_ESUCCESS:  return "success";
    case CSV_EUNCLOSED: return "unclosed quoted field";
    default: break;
    }
    return "unknown error";
}

/* parses one field starting at *p (which must be at the field's first
 * byte, not past any leading comma/newline), advances *p to just after
 * the field (at the delimiter or EOF). Returns 0 on an unclosed quote. */
static i32 csv_parse_field_(Arena *a, const char **p, const char *end, String_View *out)
{
    const char *s = *p;

    if (s < end && *s == '"') {
        const char *content_start = s + 1;
        const char *q = content_start;
        i32 has_escape = 0;

        while (q < end) {
            if (*q == '"') {
                if (q + 1 < end && q[1] == '"') {
                    has_escape = 1;
                    q += 2;
                    continue;
                }
                break; /* real closing quote */
            }
            q++;
        }
        if (q >= end) return 0; /* never found the closing quote */

        if (!has_escape) {
            *out = sv_from_parts(content_start, (size_t)(q - content_start));
        } else {
            /* copy + unescape "" -> " into an arena-owned buffer */
            size_t raw_len = (size_t)(q - content_start);
            char *buf = (char *)arena_alloc(a, raw_len); /* unescaped is always <= raw length */
            size_t n = 0;
            const char *r = content_start;
            while (r < q) {
                if (*r == '"' && r + 1 < q && r[1] == '"') {
                    buf[n++] = '"';
                    r += 2;
                } else {
                    buf[n++] = *r++;
                }
            }
            *out = sv_from_parts(buf, n);
        }

        *p = q + 1; /* past the closing quote */
        return 1;
    }

    {
        const char *q = s;
        while (q < end && *q != ',' && *q != '\n' && *q != '\r') q++;
        *out = sv_from_parts(s, (size_t)(q - s));
        *p = q;
        return 1;
    }
}

i32 csv_parse(Arena *a, const char *data, size_t size, Csv_Table *out)
{
    const char *p = data;
    const char *end = data + size;
    Csv_Row row;

    out->items = 0;
    out->count = 0;
    out->capacity = 0;

    row.items = 0;
    row.count = 0;
    row.capacity = 0;

    while (p < end) {
        String_View field;

        if (*p == '\n') { p++; arena_da_append(a, out, row); row.items = 0; row.count = 0; row.capacity = 0; continue; }
        if (*p == '\r') { p++; if (p < end && *p == '\n') p++; arena_da_append(a, out, row); row.items = 0; row.count = 0; row.capacity = 0; continue; }

        if (!csv_parse_field_(a, &p, end, &field)) return CSV_EUNCLOSED;
        arena_da_append(a, &row, field);

        if (p < end && *p == ',') { p++; continue; }
        /* otherwise a line ending (or EOF) follows - the loop head above handles \n/\r,
         * and if this was EOF, fall through to the trailing-row flush below */
    }

    /* a final row with no trailing newline still counts (row.count > 0
     * means at least one field was parsed into it since the last flush) */
    if (row.count > 0) arena_da_append(a, out, row);

    return CSV_ESUCCESS;
}

#endif /* CSV_IMPLEMENTATION_ */
#endif /* CSV_IMPLEMENTATION */
