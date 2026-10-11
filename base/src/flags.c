/* real .c file, but - like every other component this project's own
 * tools #include directly (gravity.c, position.c, ...) - safe to
 * #include more than once thanks to this guard, since this project's
 * own single-TU-per-tool build (see automake's own header comment)
 * has no separate object-file-linking step: a consumer just does
 * #include "flags.c" (which pulls in flags.h transitively) rather
 * than compiling+linking this as its own .o. */
#ifndef HOLYC_FLAGS_C_
#define HOLYC_FLAGS_C_

#include "flags.h"
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

#endif /* HOLYC_FLAGS_C_ */
