#ifndef HOLYC_INI_DEC_H_
#define HOLYC_INI_DEC_H_

/* Ported from rxi's ini.c (MIT). Same in-place-mutation trick as the
 * original: the whole file is read into one buffer and '\0' bytes are
 * written directly into it to split section headers/keys/values apart -
 * no per-token allocation at all, ini_get() just walks the buffer. Only
 * real change: the file buffer and the Ini struct itself come from an
 * Arena instead of malloc/free (this project's own convention - no
 * scattered malloc, and arena_free() at the end reclaims everything in
 * one call instead of needing a matching ini_free()). */

#include "core/base.h"
#include "core/arena_alloc.h"

typedef struct {
    char *data;
    char *end;
} Ini;

/* returns 0 and leaves *out unspecified on failure (file missing/unreadable) */
i32 ini_load(Arena *a, Ini *out, const char *filename);

/* section may be NULL to search the top-level (before any [section])
 * scope. Returns NULL if the key isn't found. The returned pointer aliases
 * Ini's own buffer - copy it (e.g. arena_strdup) before the Ini's arena
 * gets reset/rewound if it needs to outlive that. */
const char *ini_get(const Ini *ini, const char *section, const char *key);

#endif /* HOLYC_INI_DEC_H_ */

#ifdef INI_DEC_IMPLEMENTATION
#ifndef INI_DEC_IMPLEMENTATION_
#define INI_DEC_IMPLEMENTATION_

#include <ctype.h>
#include <stdio.h>


static i32 ini_strcmpci_(const char *a, const char *b)
{
    for (;;) {
        i32 d = tolower((u8)*a) - tolower((u8)*b);
        if (d != 0 || !*a) return d;
        a++, b++;
    }
}

static char *ini_next_(const Ini *ini, char *p)
{
    p += strlen(p);
    while (p < ini->end && *p == '\0') p++;
    return p;
}

static void ini_trim_back_(const Ini *ini, char *p)
{
    while (p >= ini->data && (*p == ' ' || *p == '\t' || *p == '\r')) *p-- = '\0';
}

static char *ini_discard_line_(const Ini *ini, char *p)
{
    while (p < ini->end && *p != '\n') *p++ = '\0';
    return p;
}

static char *ini_unescape_quoted_value_(const Ini *ini, char *p)
{
    char *q = p;
    p++;
    while (p < ini->end && *p != '"' && *p != '\r' && *p != '\n') {
        switch (*p) {
        case '\\':
            p++;
            switch (*p) {
                default:  *q = *p;   break;
                case 'r': *q = '\r'; break;
                case 'n': *q = '\n'; break;
                case 't': *q = '\t'; break;
                case '\r': case '\n': case '\0':
                    return q;
            }
            break;
        default:
            *q = *p;
            break;
        }
        q++, p++;
    }
    return q;
}

static void ini_split_data_(Ini *ini)
{
    char *value_start, *line_start;
    char *p = ini->data;

    while (p < ini->end) {
        switch (*p) {
        case '\r': case '\n': case '\t': case ' ':
            *p = '\0';
            /* fall through */
        case '\0':
            p++;
            break;

        case '[':
            p += strcspn(p, "]\n");
            *p = '\0';
            break;

        case ';':
            p = ini_discard_line_(ini, p);
            break;

        default:
            line_start = p;
            p += strcspn(p, "=\n");

            if (*p != '=') {
                p = ini_discard_line_(ini, line_start);
                break;
            }
            ini_trim_back_(ini, p - 1);

            do {
                *p++ = '\0';
            } while (*p == ' ' || *p == '\r' || *p == '\t');

            if (*p == '\n' || *p == '\0') {
                p = ini_discard_line_(ini, line_start);
                break;
            }

            switch (*p) {
            case '"':
                value_start = p;
                p = ini_unescape_quoted_value_(ini, p);
                if (p == value_start) {
                    p = ini_discard_line_(ini, line_start);
                    break;
                }
                p = ini_discard_line_(ini, p);
                break;
            default:
                p += strcspn(p, "\n");
                ini_trim_back_(ini, p - 1);
                break;
            }
            break;
        }
    }
}

i32 ini_load(Arena *a, Ini *out, const char *filename)
{
    FILE *fp;
    i64 sz;
    size_t n;

    fp = fopen(filename, "rb");
    if (!fp) return 0;

    fseek(fp, 0, SEEK_END);
    sz = (i64)ftell(fp);
    rewind(fp);

    out->data = (char *)arena_alloc(a, (size_t)sz + 1);
    out->data[sz] = '\0';
    out->end = out->data + sz;
    n = fread(out->data, 1, (size_t)sz, fp);
    fclose(fp);
    if (n != (size_t)sz) return 0;

    ini_split_data_(out);
    return 1;
}

const char *ini_get(const Ini *ini, const char *section, const char *key)
{
    char *current_section = (char *)"";
    char *val;
    char *p = ini->data;

    if (*p == '\0') p = ini_next_(ini, p);

    while (p < ini->end) {
        switch (*p) {
        case '[':
            current_section = p + 1;
            break;
        default:
            val = ini_next_(ini, p);
            if (!section || !ini_strcmpci_(section, current_section)) {
                if (!ini_strcmpci_(p, key)) return val;
            }
            p = val;
            break;
        }
        p = ini_next_(ini, p);
    }

    return (const char *)0;
}

#endif /* INI_DEC_IMPLEMENTATION_ */
#endif /* INI_DEC_IMPLEMENTATION */
