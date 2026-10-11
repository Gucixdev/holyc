#ifndef HOLYC_FLAGS_H_
#define HOLYC_FLAGS_H_

/* Cross-cutting runtime flags every real CLI tool ends up wanting
 * (verbose, quiet, no-color, dry-run) - a real, shared, one-place
 * version of the SAME pattern several patches already hand-rolled
 * separately (make's own make_patch_dry_run_enabled, dv's own
 * dv_patch_exclude_enabled), rather than every tool (or every patch
 * within a tool) reinventing its own single global bool.
 *
 * Deliberately NOT built on args.h (unlike each tool's own business-
 * specific flags): args.h isn't universal here - project/doc_format/
 * scan/proof/proof.c is a real, standalone C tool with no args.h
 * dependency at all, and this is meant to work there too ("nasz proof
 * tak samo" - per explicit direction). flags_apply_argv does its own
 * tiny linear argv scan instead, recognizing only these 4 universal
 * spellings and leaving every other token alone - safe to call
 * alongside a tool's own args_parse/hand-rolled argv loop without
 * interfering with it (nothing is consumed or removed from argv).
 *
 * Real suckless/.c+.h split on purpose (per explicit direction),
 * unlike this project's usual single-file stb-style header - copy
 * this pair, unmodified, into a new tool's own directory (matching
 * args.h's own per-tool-copy convention) and call flags_init once at
 * the top of main with that tool's own env-var prefix. */

#include "core/base.h"

typedef struct {
    i32 verbose;
    i32 quiet;
    i32 no_color;
    i32 dry_run;
} Flags;

/* reads TOOLNAME_VERBOSE / TOOLNAME_QUIET / TOOLNAME_DRY_RUN from the
 * environment (env_prefix = "TOOLNAME", any non-empty, non-"0" value
 * counts as set) - no_color ALSO honors the bare NO_COLOR variable
 * (no-color.org's own real, widely-adopted convention) in addition to
 * TOOLNAME_NO_COLOR, so a user's one global NO_COLOR=1 works across
 * every tool without needing to set it per-tool. */
void flags_init(Flags *f, const char *env_prefix);

/* scans argv for --verbose/-v, --quiet/-q, --no-color, --dry-run/-n -
 * each one FOUND sets the matching field to 1 (an env-set flag is
 * never un-set by this - CLI only ever turns a flag further ON, same
 * "most specific wins, nothing silently reverts" spirit as the
 * settings-cascade precedence this project's own doctor/permissions
 * work documents elsewhere). Call after flags_init. */
void flags_apply_argv(Flags *f, int argc, char **argv);

#endif /* HOLYC_FLAGS_H_ */

/* implementation: define FLAGS_IMPLEMENTATION in exactly one translation unit */
#ifdef FLAGS_IMPLEMENTATION
#ifndef FLAGS_IMPLEMENTATION_
#define FLAGS_IMPLEMENTATION_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static i32 flags_getenv_bool_(const char *name)
{
    const char *v = getenv(name);
    return v && v[0] != '\0' && strcmp(v, "0") != 0;
}

static i32 flags_env_bool_(const char *prefix, const char *suffix)
{
    char name[128];
    snprintf(name, sizeof(name), "%s_%s", prefix, suffix);
    return flags_getenv_bool_(name);
}

void flags_init(Flags *f, const char *env_prefix)
{
    f->verbose = flags_env_bool_(env_prefix, "VERBOSE");
    f->quiet = flags_env_bool_(env_prefix, "QUIET");
    f->no_color = flags_env_bool_(env_prefix, "NO_COLOR") || flags_getenv_bool_("NO_COLOR");
    f->dry_run = flags_env_bool_(env_prefix, "DRY_RUN");
}

void flags_apply_argv(Flags *f, int argc, char **argv)
{
    i32 i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) f->verbose = 1;
        else if (strcmp(argv[i], "--quiet") == 0 || strcmp(argv[i], "-q") == 0) f->quiet = 1;
        else if (strcmp(argv[i], "--no-color") == 0) f->no_color = 1;
        else if (strcmp(argv[i], "--dry-run") == 0 || strcmp(argv[i], "-n") == 0) f->dry_run = 1;
    }
}

#endif /* FLAGS_IMPLEMENTATION_ */
#endif /* FLAGS_IMPLEMENTATION */
