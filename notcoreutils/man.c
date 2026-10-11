/* man - a minimal manual-page viewer: finds NAME's page under the
 * usual manN/ tree (honoring $MANPATH, falling back to the standard
 * FHS locations), gunzips it if needed (the bundled deflate.h,
 * manually skipping the gzip wrapper - deflate_decode itself only
 * speaks raw DEFLATE/zlib, not the gzip container), and renders the
 * common troff/man(7) macros as plain readable text.
 *
 * Deliberately not a groff reimplementation - no macro expansion
 * language, no tables (.TS/.TE), no real fill/adjust (line wrapping),
 * no font metrics. Handles what the overwhelming majority of real man
 * pages actually use: .TH/.SH/.SS headers, .PP/.P/.LP/.TP paragraph
 * breaks, .B/.I/.BR/.IR/.RB/.RI alternating bold/italic (rendered as
 * ANSI bold/underline), \fB/\fI/\fR/\fP inline escapes, .nf/.fi
 * literal blocks, .RS/.RE indent, and \- / \(em / \*(-- style escapes
 * in running text. Unknown macros are skipped rather than printed
 * raw - honest omission beats dumping troff source.
 *
 * usage: man [SECTION] NAME
 */

#define BASE_TYPE_IMPLEMENTATION
#include "base.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define DEFLATE_IMPLEMENTATION
#include "deflate.h"
#include "patches.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *MAN_DEFAULT_PATH[] = {
    "/usr/share/man", "/usr/local/share/man", "/usr/man", NULL
};

/* ---- locate the page ---- */

static i32 man_find_(char *out, size_t outcap, const char *name, const char *section)
{
    char pathbuf[4096];
    const char *mp;
    char *dir, *saveptr;
    i32 sec;
    struct stat st;

    mp = getenv("MANPATH");
    if (mp && mp[0]) {
        strncpy(pathbuf, mp, sizeof(pathbuf) - 1);
        pathbuf[sizeof(pathbuf) - 1] = '\0';
        for (dir = strtok_r(pathbuf, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
            for (sec = section ? atoi(section) : 1; sec <= (section ? atoi(section) : 9); sec++) {
                snprintf(out, outcap, "%s/man%d/%s.%d", dir, sec, name, sec);
                if (stat(out, &st) == 0) return 1;
                snprintf(out, outcap, "%s/man%d/%s.%d.gz", dir, sec, name, sec);
                if (stat(out, &st) == 0) return 1;
            }
        }
        return 0;
    }

    {
        i32 di;
        for (di = 0; MAN_DEFAULT_PATH[di]; di++) {
            for (sec = section ? atoi(section) : 1; sec <= (section ? atoi(section) : 9); sec++) {
                snprintf(out, outcap, "%s/man%d/%s.%d", MAN_DEFAULT_PATH[di], sec, name, sec);
                if (stat(out, &st) == 0) return 1;
                snprintf(out, outcap, "%s/man%d/%s.%d.gz", MAN_DEFAULT_PATH[di], sec, name, sec);
                if (stat(out, &st) == 0) return 1;
            }
        }
    }
    return 0;
}

/* ---- load, gunzip if needed ---- */

static u8 *man_load_(Arena *a, const char *path, size_t *out_len)
{
    FILE *f;
    u8 *raw;
    size_t raw_len, gz_pos;
    long fsize;
    u8 flg;

    f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 0) { fclose(f); return NULL; }

    raw = (u8 *)arena_alloc(a, (size_t)fsize);
    if (fread(raw, 1, (size_t)fsize, f) != (size_t)fsize) { fclose(f); return NULL; }
    fclose(f);
    raw_len = (size_t)fsize;

    if (raw_len < 10 || raw[0] != 0x1f || raw[1] != 0x8b) {
        *out_len = raw_len;
        return raw; /* not gzip - already plain troff source */
    }

    flg = raw[3];
    gz_pos = 10;
    if (flg & 0x04) { /* FEXTRA */
        u32 xlen = raw[gz_pos] | (raw[gz_pos + 1] << 8);
        gz_pos += 2 + xlen;
    }
    if (flg & 0x08) while (gz_pos < raw_len && raw[gz_pos++]) ; /* FNAME */
    if (flg & 0x10) while (gz_pos < raw_len && raw[gz_pos++]) ; /* FCOMMENT */
    if (flg & 0x02) gz_pos += 2; /* FHCRC */

    {
        u8 *decoded;
        size_t decoded_len;
        if (gz_pos >= raw_len ||
            !deflate_decode(a, raw + gz_pos, raw_len - gz_pos, 0, &decoded, &decoded_len)) {
            return NULL;
        }
        *out_len = decoded_len;
        return decoded;
    }
}

/* ---- rendering ---- */

#define ANSI_BOLD "\033[1m"
#define ANSI_UNDERLINE "\033[4m"
#define ANSI_RESET "\033[0m"

static i32 man_indent_ = 0;
static i32 man_in_literal_ = 0;

static void man_print_indent_(void)
{
    i32 i;
    for (i = 0; i < man_indent_; i++) putchar(' ');
}

/* maps a troff special-character/string-register NAME (from either the
 * classic \(XX / \*(XX two-letter forms or the pandoc-style bracketed
 * \[NAME] form) to its plain-text rendering. len is the name length
 * (2 for the classic forms, variable for \[NAME]). Unknown names are
 * dropped silently - honest omission over garbage. */
static void man_emit_named_char_(const char *name, size_t len)
{
    if (len == 2 && name[0] == 'e' && name[1] == 'm') fputs("--", stdout);
    else if (len == 2 && name[0] == 'e' && name[1] == 'n') fputs("-", stdout);
    else if (len == 2 && (name[0] == 'a' || name[0] == 'A' || name[0] == 'o' || name[0] == 'c') && (name[1] == 'q' || name[1] == 'Q')) fputs("'", stdout);
    else if (len == 2 && name[0] == 'l' && name[1] == 'q') fputs("\"", stdout);
    else if (len == 2 && name[0] == 'r' && name[1] == 'q') fputs("\"", stdout);
    else if (len == 2 && name[0] == 'b' && name[1] == 'u') fputs("*", stdout);
    else if (len == 2 && name[0] == 'c' && name[1] == 'o') fputs("(C)", stdout);
    else if (len == 2 && name[0] == 'r' && name[1] == 'g') fputs("(R)", stdout);
    else if (len == 2 && name[0] == 't' && name[1] == 'm') fputs("(TM)", stdout);
    /* else: unrecognized name - drop silently */
}

/* converts \fB \fI \fR \fP \f[B] \f[] \- \(em \[aq] and drops other
 * backslash escapes, writing ANSI codes for bold/italic as it goes. */
static void man_emit_text_(const char *s)
{
    i32 bold_on = 0, ital_on = 0;

    while (*s) {
        if (s[0] == '\\' && s[1] == 'f' && s[2]) {
            if (bold_on) { fputs(ANSI_RESET, stdout); bold_on = 0; }
            if (ital_on) { fputs(ANSI_RESET, stdout); ital_on = 0; }
            if (s[2] == 'B') { fputs(ANSI_BOLD, stdout); bold_on = 1; s += 3; }
            else if (s[2] == 'I') { fputs(ANSI_UNDERLINE, stdout); ital_on = 1; s += 3; }
            else if (s[2] == '(' && s[3] && s[4]) { s += 5; } /* \f(XX two-letter font name (e.g. CW = constant-width) - no distinct ANSI rendering, just reset to roman */
            else if (s[2] == '[') { /* \f[B] \f[I] \f[] pandoc-style bracketed font name */
                const char *close = strchr(s + 3, ']');
                if (close && close - (s + 3) == 1 && s[3] == 'B') { fputs(ANSI_BOLD, stdout); bold_on = 1; }
                else if (close && close - (s + 3) == 1 && s[3] == 'I') { fputs(ANSI_UNDERLINE, stdout); ital_on = 1; }
                s = close ? close + 1 : s + 3;
            }
            else s += 3; /* \fR, \fP, or any other one-letter font name */
            continue;
        }
        if (s[0] == '\\' && (s[1] == '-' || s[1] == '_')) { putchar('-'); s += 2; continue; }
        if (s[0] == '\\' && s[1] == '(' && s[2] && s[3]) { /* \(XX two-letter special character */
            man_emit_named_char_(s + 2, 2);
            s += 4;
            continue;
        }
        if (s[0] == '\\' && s[1] == '[') { /* \[NAME] pandoc-style bracketed special character */
            const char *close = strchr(s + 2, ']');
            if (close) {
                man_emit_named_char_(s + 2, (size_t)(close - (s + 2)));
                s = close + 1;
            } else {
                s += 2;
            }
            continue;
        }
        if (s[0] == '\\' && s[1] == '&') { s += 2; continue; } /* zero-width */
        if (s[0] == '\\' && s[1] == '*' && s[2] == '(' && s[3] && s[4]) { /* \*(XX string-register interpolation */
            man_emit_named_char_(s + 3, 2);
            s += 5;
            continue;
        }
        if (s[0] == '\\' && s[1] == 'X' && s[2] == '\'') { /* device-control string (e.g. PDF/HTML link metadata) - invisible in plain text */
            s += 3;
            while (*s && *s != '\'') s++;
            if (*s == '\'') s++;
            continue;
        }
        if (s[0] == '\\' && s[1]) { s += 2; continue; } /* unknown escape - drop it */
        putchar(*s);
        s++;
    }
    if (bold_on || ital_on) fputs(ANSI_RESET, stdout);
}

/* .BR/.IR/.RB/.RI/.BI - alternate bold/italic/roman across args, no
 * space between them (real troff behavior for these macros). */
static void man_emit_alternating_(char **args, i32 nargs, const char *style)
{
    i32 i;
    for (i = 0; i < nargs; i++) {
        char c = style[i % (i32)strlen(style)];
        if (c == 'B') fputs(ANSI_BOLD, stdout);
        else if (c == 'I') fputs(ANSI_UNDERLINE, stdout);
        man_emit_text_(args[i]);
        fputs(ANSI_RESET, stdout);
    }
    putchar('\n');
}

static i32 man_split_args_(char *line, char **args, i32 maxargs)
{
    i32 n = 0;
    char *p = line;
    while (*p && n < maxargs) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            args[n++] = p;
            while (*p && *p != '"') p++;
        } else {
            args[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        if (*p) *p++ = '\0';
    }
    return n;
}

static void man_render_(char *text)
{
    char *line, *saveptr, *args[16];
    i32 nargs;

    for (line = strtok_r(text, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        if ((line[0] == '.' || line[0] == '\'') && line[1] == '\\' && line[2] == '"') continue; /* comment */

        if (line[0] != '.' && line[0] != '\'') {
            if (line[0] == '\0') { putchar('\n'); continue; }
            man_print_indent_();
            man_emit_text_(line); /* .nf/.fi only disable line-fill/reflow (which we never do anyway) - escapes are still processed inside a literal block */
            putchar('\n');
            continue;
        }

        {
            char macrobuf[4096];
            strncpy(macrobuf, line + 1, sizeof(macrobuf) - 1);
            macrobuf[sizeof(macrobuf) - 1] = '\0';
            nargs = man_split_args_(macrobuf, args, 16);
            if (nargs == 0) continue;

            if (strcmp(args[0], "TH") == 0) {
                if (nargs > 1) printf(ANSI_BOLD "%s" ANSI_RESET "(%s)\n", args[1], nargs > 2 ? args[2] : "");
            } else if (strcmp(args[0], "SH") == 0) {
                man_indent_ = 0;
                putchar('\n');
                fputs(ANSI_BOLD, stdout);
                { i32 i; for (i = 1; i < nargs; i++) { man_emit_text_(args[i]); if (i + 1 < nargs) putchar(' '); } }
                fputs(ANSI_RESET "\n", stdout);
                man_indent_ = 3;
            } else if (strcmp(args[0], "SS") == 0) {
                putchar('\n');
                man_indent_ = 3;
                fputs(ANSI_UNDERLINE, stdout);
                { i32 i; for (i = 1; i < nargs; i++) { man_emit_text_(args[i]); if (i + 1 < nargs) putchar(' '); } }
                fputs(ANSI_RESET "\n", stdout);
                man_indent_ = 6;
            } else if (strcmp(args[0], "PP") == 0 || strcmp(args[0], "P") == 0 || strcmp(args[0], "LP") == 0) {
                putchar('\n');
            } else if (strcmp(args[0], "TP") == 0) {
                putchar('\n');
            } else if (strcmp(args[0], "B") == 0) {
                man_print_indent_();
                { i32 i; for (i = 1; i < nargs; i++) { fputs(ANSI_BOLD, stdout); man_emit_text_(args[i]); fputs(ANSI_RESET, stdout); if (i + 1 < nargs) putchar(' '); } }
                putchar('\n');
            } else if (strcmp(args[0], "I") == 0) {
                man_print_indent_();
                { i32 i; for (i = 1; i < nargs; i++) { fputs(ANSI_UNDERLINE, stdout); man_emit_text_(args[i]); fputs(ANSI_RESET, stdout); if (i + 1 < nargs) putchar(' '); } }
                putchar('\n');
            } else if (strcmp(args[0], "BR") == 0) {
                man_print_indent_(); man_emit_alternating_(args + 1, nargs - 1, "BR");
            } else if (strcmp(args[0], "IR") == 0) {
                man_print_indent_(); man_emit_alternating_(args + 1, nargs - 1, "IR");
            } else if (strcmp(args[0], "RB") == 0) {
                man_print_indent_(); man_emit_alternating_(args + 1, nargs - 1, "RB");
            } else if (strcmp(args[0], "RI") == 0) {
                man_print_indent_(); man_emit_alternating_(args + 1, nargs - 1, "RI");
            } else if (strcmp(args[0], "BI") == 0) {
                man_print_indent_(); man_emit_alternating_(args + 1, nargs - 1, "BI");
            } else if (strcmp(args[0], "RS") == 0) {
                man_indent_ += 4;
            } else if (strcmp(args[0], "RE") == 0) {
                man_indent_ -= 4;
                if (man_indent_ < 0) man_indent_ = 0;
            } else if (strcmp(args[0], "nf") == 0) {
                man_in_literal_ = 1;
            } else if (strcmp(args[0], "fi") == 0) {
                man_in_literal_ = 0;
            }
            /* everything else (.ds, .if, .TS/.TE, custom macros...) silently skipped */
        }
    }
}

int main(int argc, char **argv)
{
    Arena a = {0};
    char path[4096];
    const char *name, *section = NULL;
    u8 *data;
    size_t len;

    if (argc == 3) { section = argv[1]; name = argv[2]; }
    else if (argc == 2) { name = argv[1]; }
    else { fprintf(stderr, "usage: man [SECTION] NAME\n"); return 1; }

    if (!man_find_(path, sizeof(path), name, section)) {
        fprintf(stderr, "man: no manual entry for %s\n", name);
        return 1;
    }

    data = man_load_(&a, path, &len);
    if (!data) {
        fprintf(stderr, "man: could not read/decompress %s\n", path);
        return 1;
    }

    {
        char *text = (char *)arena_alloc(&a, len + 1);
        memcpy(text, data, len);
        text[len] = '\0';
        man_render_(text);
    }

    arena_free(&a);
    return 0;
}
