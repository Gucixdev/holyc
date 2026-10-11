/* cloc - count lines of code per language.
 *
 *   cloc [--table | --files] [ROOT]
 *
 * Walks ROOT (default .) recursively, classifies every regular file by its
 * extension, and counts code, comment and blank lines. A line is a comment
 * line when it has comment text and no code; "x = 1; // note" is code.
 * Comment syntax is chosen per language: C-like (// and block comments),
 * hash (#), dash (--) or none. Strings and character literals are skipped
 * so a comment marker inside them does not count. Python docstrings and
 * shell here-documents are counted as code.
 *
 * Hidden entries and node_modules are skipped. Output:
 *   (default)  an aligned table, languages by code lines, then TOTAL
 *   --table    the same as '|' separated rows, for scripts
 *   --files    one row per file: file|lines|code|comment|blank|language
 */

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum style { NONE, CLIKE, HASH, DASH };

struct lang {
    const char *name;
    enum style style;
    const char *exts; /* space separated, with the leading dot */
};

static const struct lang LANGS[] = {
    { "C",          CLIKE, ".c" },
    { "C/C++ hdr",  CLIKE, ".h .hh .hpp .hxx" },
    { "C++",        CLIKE, ".cc .cpp .cxx" },
    { "Rust",       CLIKE, ".rs" },
    { "Go",         CLIKE, ".go" },
    { "Java",       CLIKE, ".java" },
    { "JavaScript", CLIKE, ".js .mjs .cjs" },
    { "TypeScript", CLIKE, ".ts .tsx" },
    { "GDScript",   HASH,  ".gd" },
    { "Python",     HASH,  ".py" },
    { "Shell",      HASH,  ".sh .bash .zsh" },
    { "Perl",       HASH,  ".pl .pm" },
    { "Ruby",       HASH,  ".rb" },
    { "Makefile",   HASH,  ".mk" },
    { "YAML",       HASH,  ".yml .yaml" },
    { "TOML",       HASH,  ".toml" },
    { "Lua",        DASH,  ".lua" },
    { "SQL",        DASH,  ".sql" },
    { "Mojo",       HASH,  ".mojo" },
    { "Shader",     CLIKE, ".glsl .gdshader .gdshaderinc .vert .frag" },
    { "Markdown",   NONE,  ".md" },
    { "Text",       NONE,  ".txt" },
    { "JSON",       NONE,  ".json" },
    { "HTML",       NONE,  ".html .htm" },
    { "CSS",        CLIKE, ".css" },
};
#define NLANGS (int)(sizeof(LANGS) / sizeof(LANGS[0]))

struct tot {
    long files, lines, code, comment, blank;
};

static struct tot per[NLANGS];
static struct tot grand;
static int mode; /* 0 pretty, 1 table, 2 files */

/* language index for a file name, or -1. Makefile and shell-script names
 * without an extension are recognised by name. */
static int lang_of(const char *name) {
    const char *dot = strrchr(name, '.');
    int i;
    if (!strcmp(name, "Makefile") || !strcmp(name, "makefile") || !strcmp(name, "GNUmakefile")) {
        for (i = 0; i < NLANGS; i++)
            if (!strcmp(LANGS[i].name, "Makefile")) return i;
    }
    if (!dot || dot == name) return -1;
    for (i = 0; i < NLANGS; i++) {
        const char *p = LANGS[i].exts;
        size_t n = strlen(dot);
        while (*p) {
            const char *sp = strchr(p, ' ');
            size_t len = sp ? (size_t)(sp - p) : strlen(p);
            if (len == n && !strncmp(p, dot, n)) return i;
            if (!sp) break;
            p = sp + 1;
        }
    }
    return -1;
}

static void scan(const char *path, enum style st, long *lines, long *code, long *comment, long *blank) {
    FILE *f = fopen(path, "rb");
    int c, prev = 0, in_block = 0, in_line = 0, in_str = 0, in_chr = 0;
    int has_code = 0, has_comment = 0, any = 0;
    *lines = *code = *comment = *blank = 0;
    if (!f) return;
#define END_LINE() do { \
        (*lines)++; \
        if (has_code) (*code)++; \
        else if (has_comment) (*comment)++; \
        else (*blank)++; \
        has_code = has_comment = in_line = any = 0; prev = 0; \
        in_str = in_chr = 0; \
    } while (0)
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') { END_LINE(); continue; }
        if (c == '\r') continue;
        if (!isspace(c)) any = 1;
        if (in_line) { has_comment = 1; continue; }
        if (in_block) {
            has_comment = 1;
            if (prev == '*' && c == '/') { in_block = 0; c = 0; }
            prev = c;
            continue;
        }
        if (in_str) {
            has_code = 1;
            if (prev == '\\') { prev = 0; continue; }
            if (c == '"') in_str = 0;
            prev = c;
            continue;
        }
        if (in_chr) {
            has_code = 1;
            if (prev == '\\') { prev = 0; continue; }
            if (c == '\'') in_chr = 0;
            prev = c;
            continue;
        }
        if (st == HASH && c == '#') { in_line = 1; has_comment = 1; continue; }
        if (st == DASH && c == '-' && prev == '-') { in_line = 1; has_comment = 1; prev = 0; continue; }
        if (st == CLIKE && prev == '/' && c == '*') { in_block = 1; has_comment = 1; prev = 0; continue; }
        if (st == CLIKE && prev == '/' && c == '/') { in_line = 1; has_comment = 1; prev = 0; continue; }
        if (c == '"') { in_str = 1; has_code = 1; prev = c; continue; }
        if (c == '\'' && st != NONE) { in_chr = 1; has_code = 1; prev = c; continue; }
        if (!isspace(c) && !(st == CLIKE && c == '/') && !(st == DASH && c == '-')) has_code = 1;
        if (st == NONE && !isspace(c)) has_code = 1;
        prev = c;
    }
    if (any) END_LINE();
#undef END_LINE
    fclose(f);
}

static void walk(const char *dir) {
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL) {
        char path[4096];
        struct stat sb;
        int li;
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "node_modules")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (stat(path, &sb) != 0) continue;
        if (S_ISDIR(sb.st_mode)) { walk(path); continue; }
        if (!S_ISREG(sb.st_mode) || sb.st_size == 0) continue;
        li = lang_of(e->d_name);
        if (li < 0) continue;
        {
            long lines, code, comment, blank;
            scan(path, LANGS[li].style, &lines, &code, &comment, &blank);
            if (mode == 2) printf("%s|%ld|%ld|%ld|%ld|%s\n", path, lines, code, comment, blank, LANGS[li].name);
            per[li].files++; per[li].lines += lines; per[li].code += code;
            per[li].comment += comment; per[li].blank += blank;
            grand.files++; grand.lines += lines; grand.code += code;
            grand.comment += comment; grand.blank += blank;
        }
    }
    closedir(d);
}

static double pct(long a, long b) { return b ? 100.0 * (double)a / (double)b : 0.0; }

int main(int argc, char **argv) {
    const char *root = ".";
    int i, order[NLANGS], n = 0;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--table")) mode = 1;
        else if (!strcmp(argv[i], "--files")) mode = 2;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            puts("usage: cloc [--table | --files] [ROOT]");
            return 0;
        } else root = argv[i];
    }
    {
        struct stat sb;
        if (stat(root, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
            fprintf(stderr, "cloc: %s: not a directory\n", root);
            return 1;
        }
    }
    if (mode == 2) puts("file|lines|code|comment|blank|language");
    walk(root);
    if (mode == 2) return 0;

    for (i = 0; i < NLANGS; i++)
        if (per[i].files) order[n++] = i;
    for (i = 1; i < n; i++) { /* insertion sort by code lines, descending */
        int k = order[i], j = i - 1;
        while (j >= 0 && per[order[j]].code < per[k].code) { order[j + 1] = order[j]; j--; }
        order[j + 1] = k;
    }
    if (mode == 1) {
        puts("language|files|lines|code|comment|blank|code_pct");
        for (i = 0; i < n; i++) {
            struct tot *t = &per[order[i]];
            printf("%s|%ld|%ld|%ld|%ld|%ld|%.1f\n", LANGS[order[i]].name, t->files, t->lines, t->code, t->comment, t->blank, pct(t->code, t->lines));
        }
        printf("TOTAL|%ld|%ld|%ld|%ld|%ld|%.1f\n", grand.files, grand.lines, grand.code, grand.comment, grand.blank, pct(grand.code, grand.lines));
        return 0;
    }
    printf("%-12s %7s %9s %9s %9s %9s %7s\n", "language", "files", "lines", "code", "comment", "blank", "code%");
    puts("---------------------------------------------------------------------");
    for (i = 0; i < n; i++) {
        struct tot *t = &per[order[i]];
        printf("%-12s %7ld %9ld %9ld %9ld %9ld %6.1f%%\n", LANGS[order[i]].name, t->files, t->lines, t->code, t->comment, t->blank, pct(t->code, t->lines));
    }
    puts("---------------------------------------------------------------------");
    printf("%-12s %7ld %9ld %9ld %9ld %9ld %6.1f%%\n", "TOTAL", grand.files, grand.lines, grand.code, grand.comment, grand.blank, pct(grand.code, grand.lines));
    return 0;
}
