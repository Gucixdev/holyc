/* dv - disk view: recursively size a directory, print the biggest
 * entries first (a non-interactive slice of what ncdu shows). */

#define BASE_TYPE_IMPLEMENTATION
#include "base.h"
#define ARGS_IMPLEMENTATION
#include "args.h"
/* Command-line flags, typed: one struct filled from argv instead of
 * scattered args_get/args_has lookups. The -exclude pattern list is
 * per-invocation data, not a flag, so it lives outside this struct. */

typedef struct {
    i32 top;      /* how many biggest entries to print - dv's own -n/--top */
    i32 verbose;  /* print each directory as dv_scan enters it */
    i32 quiet;    /* suppress the final entry listing entirely */
} Dv_Flags;

/* reads dv's own -n/--top from `r` (args_parse's own result) plus
 * -v/--verbose and -q/--quiet directly from argv (args.h's own
 * SPECS table only needs to know about -top - verbose/quiet are
 * simple presence-only flags this file checks for itself, the same
 * "doesn't need args.h's own machinery for something this small"
 * reasoning dv's own -exclude patch already documents for itself). */
void dv_flags_init(Dv_Flags *f, const Args_Result *r, int argc, char **argv);

#include <stdlib.h>
#include <string.h>

void dv_flags_init(Dv_Flags *f, const Args_Result *r, int argc, char **argv)
{
    i32 i;

    f->top = args_has(r, "top") ? atoi(args_get(r, "top")) : 20;
    f->verbose = 0;
    f->quiet = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) f->verbose = 1;
        else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) f->quiet = 1;
    }
}
/* ---- end flags.h/flags.c ---- */
#define DV_PATCH_EXCLUDE 1
/* ---- inlined from patches/exclude.h (was a separate opt-in patch file) ---- */
/* patch: exclude - real -exclude PATTERN (repeatable), built from
 * this project's own reusable src/patch_templates/inexclude.c pattern
 * (adapted from a fixed compile-time table to runtime-configurable
 * patterns, same approach already used for find/patches/exclude.h).
 * Real, honest motivation: dv walks an ENTIRE directory tree to size
 * it - without this, sizing a real project directory always includes
 * .git/node_modules/target/build in the "biggest entries" output,
 * which is rarely what anyone actually wants to see. Like find's own
 * -exclude, this PRUNES matching directories (dv_scan never
 * recurses into them at all) rather than just hiding them from the
 * final listing - a real difference for a disk-usage tool
 * specifically, since walking into a huge excluded tree is real,
 * wasted stat() work even if its size were discarded afterward. */
#ifndef DV_PATCH_EXCLUDE_H_
#define DV_PATCH_EXCLUDE_H_

#include <fnmatch.h>

#define DV_MAX_EXCLUDE 64

static const char *g_dv_exclude[DV_MAX_EXCLUDE];
static i32 g_dv_exclude_count = 0;
static i32 dv_patch_exclude_enabled = 0;

static void dv_exclude_add(const char *pattern)
{
    if (g_dv_exclude_count < DV_MAX_EXCLUDE) g_dv_exclude[g_dv_exclude_count++] = pattern;
}

static i32 dv_excluded(const char *base)
{
    i32 i;
    for (i = 0; i < g_dv_exclude_count; i++) {
        if (fnmatch(g_dv_exclude[i], base, 0) == 0) return 1;
    }
    return 0;
}

#endif

/* ---- end patches/exclude.h ---- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const Args_Spec SPECS[] = {
    { "top", 'n', 1 },
};
#define N_SPECS ((i32)(sizeof(SPECS) / sizeof(SPECS[0])))

#define DV_MAX_ENTRIES 65536

typedef struct { char path[4096]; i64 bytes; } Dv_Entry;

static Dv_Entry dv_entries[DV_MAX_ENTRIES];
static i32 dv_count = 0;

static Dv_Flags dv_flags;

static i64 dv_scan(const char *path)
{
    struct stat st;
    i64 total = 0;

    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *ent;
        if (!d) return 0;
        if (dv_flags.verbose) fprintf(stderr, "scanning: %s\n", path);
        while ((ent = readdir(d)) != NULL) {
            char child[4096];
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
#if DV_PATCH_EXCLUDE
            if (dv_patch_exclude_enabled && dv_excluded(ent->d_name)) continue;
#endif
            snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
            total += dv_scan(child);
        }
        closedir(d);
    } else {
        total = (i64)st.st_size;
    }

    if (dv_count < DV_MAX_ENTRIES) {
        strncpy(dv_entries[dv_count].path, path, sizeof(dv_entries[dv_count].path) - 1);
        dv_entries[dv_count].path[sizeof(dv_entries[dv_count].path) - 1] = '\0';
        dv_entries[dv_count].bytes = total;
        dv_count++;
    }
    return total;
}

static i32 dv_cmp(const void *pa, const void *pb)
{
    const Dv_Entry *a = pa, *b = pb;
    if (a->bytes < b->bytes) return 1;
    if (a->bytes > b->bytes) return -1;
    return 0;
}

static void dv_human(i64 bytes, char *out, size_t cap)
{
    const char *units[] = { "B", "K", "M", "G", "T" };
    double v = (double)bytes;
    i32 u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    snprintf(out, cap, "%.1f%s", v, units[u]);
}

int main(int argc, char **argv)
{
    Args_Result r;
    const char *root;
    i32 i;
#if DV_PATCH_EXCLUDE
    static char *filtered_argv[64];
    i32 filtered_argc = 0;
#endif

#if DV_PATCH_EXCLUDE
    /* args.h's own args_get is single-value (last-wins), not a
     * repeatable-flag collector - a real, manual pre-scan for every
     * -exclude/--exclude occurrence, same approach find's own
     * (args.h-free) -exclude parsing takes. The -exclude/PATTERN pair
     * is also removed from what args_parse itself sees below -
     * leaving it in would make args_parse treat "-exclude" as an
     * unrecognized short option (matching only its own 'e') and then
     * treat the untouched PATTERN token on the NEXT loop iteration as
     * a real positional argument, corrupting dv's own root-path
     * selection - a real bug this patch's first draft actually had,
     * caught before shipping by tracing args_parse's own token-by-
     * token loop, not by testing alone. */
    filtered_argv[filtered_argc++] = argv[0];
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-exclude") == 0 || strcmp(argv[i], "--exclude") == 0) {
            if (i + 1 < argc) { dv_patch_exclude_enabled = 1; dv_exclude_add(argv[i + 1]); i++; }
            continue;
        }
        if (filtered_argc < 64) filtered_argv[filtered_argc++] = argv[i];
    }
    argc = filtered_argc;
    argv = filtered_argv;
#endif

    args_parse(argc, argv, SPECS, N_SPECS, &r);
    dv_flags_init(&dv_flags, &r, argc, argv);
    root = r.positional_count > 0 ? r.positional[0] : ".";

    dv_scan(root);
    qsort(dv_entries, (size_t)dv_count, sizeof(Dv_Entry), dv_cmp);

    if (!dv_flags.quiet) {
        for (i = 0; i < dv_count && i < dv_flags.top; i++) {
            char h[32];
            dv_human(dv_entries[i].bytes, h, sizeof(h));
            printf("%8s  %s\n", h, dv_entries[i].path);
        }
    }
    return 0;
}
