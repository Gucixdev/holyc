#ifndef HOLYC_ARGS_H_
#define HOLYC_ARGS_H_

/* getopt-style command-line argument parser - declarative (an Args_Spec
 * table describes every recognized flag/option up front, matching how
 * this project's own project/<name>_cli.c tools would otherwise hand-
 * parse argv one token at a time) rather than a stateful getopt()-style
 * cursor. No allocation at all - fixed-capacity result arrays
 * (ARGS_MAX_MATCHES/ARGS_MAX_POSITIONAL), since a real CLI's own
 * argument count is always small and known at compile time.
 *
 * Supported forms:
 *   --name          long flag (no value)
 *   --name=value    long option with an attached value
 *   --name value    long option with a separate-token value
 *   -x              short flag (no value)
 *   -xvalue         short option with an attached value
 *   -x value        short option with a separate-token value
 *   anything else   a positional argument
 *
 * NOT supported (real, deliberate scope cuts): combined short flags
 * (`-abc` meaning `-a -b -c`), `--` to force everything after it
 * positional, option repetition (the last matching occurrence simply
 * overwrites the ones before it in Args_Result, same as most getopt
 * implementations default to). */

#include "types/base.h"

#ifndef ARGS_MAX_MATCHES
#define ARGS_MAX_MATCHES 32
#endif
#ifndef ARGS_MAX_POSITIONAL
#define ARGS_MAX_POSITIONAL 32
#endif

typedef struct {
    const char *name;   /* long form, without "--" - e.g. "verbose" */
    char short_name;      /* short form character, without "-" - 0 if none */
    i32 takes_value;       /* 0 = flag, 1 = expects a value */
} Args_Spec;

typedef struct {
    const char *name;    /* points at the matching Args_Spec's own name */
    const char *value;    /* NULL for a flag (takes_value==0) */
} Args_Match;

typedef struct {
    Args_Match matches[ARGS_MAX_MATCHES];
    i32 match_count;

    const char *positional[ARGS_MAX_POSITIONAL];
    i32 positional_count;

    const char *error; /* NULL on success; otherwise a static, human-readable message about the FIRST parse error hit */
} Args_Result;

/* argv[0] (the program name) is skipped automatically - pass the real
 * argc/argv straight from main. */
void args_parse(i32 argc, char **argv, const Args_Spec *specs, i32 spec_count, Args_Result *out);

/* the value of the last occurrence of `name` (a spec's own long name),
 * or NULL if it was never given, or if it's a flag with no value. */
const char *args_get(const Args_Result *r, const char *name);

/* 1 if `name` was given at all (flag or option, with or without a
 * value), 0 otherwise. */
i32 args_has(const Args_Result *r, const char *name);

#endif /* HOLYC_ARGS_H_ */

#ifdef ARGS_IMPLEMENTATION
#ifndef ARGS_IMPLEMENTATION_
#define ARGS_IMPLEMENTATION_

static i32 args_streq_(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static i32 args_streq_n_(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
        if (a[i] == '\0') return 1;
    }
    return 1;
}

static const Args_Spec *args_find_long_(const Args_Spec *specs, i32 spec_count, const char *name, size_t name_len)
{
    i32 i;
    for (i = 0; i < spec_count; i++) {
        if (args_streq_n_(specs[i].name, name, name_len) && specs[i].name[name_len] == '\0') return &specs[i];
    }
    return NULL;
}

static const Args_Spec *args_find_short_(const Args_Spec *specs, i32 spec_count, char c)
{
    i32 i;
    for (i = 0; i < spec_count; i++) {
        if (specs[i].short_name == c) return &specs[i];
    }
    return NULL;
}

static void args_record_(Args_Result *out, const char *name, const char *value)
{
    i32 i;
    for (i = 0; i < out->match_count; i++) {
        if (args_streq_(out->matches[i].name, name)) {
            out->matches[i].value = value; /* last occurrence wins */
            return;
        }
    }
    if (out->match_count < ARGS_MAX_MATCHES) {
        out->matches[out->match_count].name = name;
        out->matches[out->match_count].value = value;
        out->match_count++;
    }
}

void args_parse(i32 argc, char **argv, const Args_Spec *specs, i32 spec_count, Args_Result *out)
{
    i32 i;

    out->match_count = 0;
    out->positional_count = 0;
    out->error = NULL;

    for (i = 1; i < argc; i++) {
        const char *tok = argv[i];

        if (tok[0] == '-' && tok[1] == '-' && tok[2] != '\0') {
            const char *name = tok + 2;
            const char *eq = name;
            size_t name_len = 0;
            const Args_Spec *spec;

            while (*eq && *eq != '=') { eq++; name_len++; }
            spec = args_find_long_(specs, spec_count, name, name_len);
            if (!spec) { out->error = "unknown long option"; continue; }

            if (!spec->takes_value) {
                args_record_(out, spec->name, NULL);
                continue;
            }

            if (*eq == '=') {
                args_record_(out, spec->name, eq + 1);
            } else if (i + 1 < argc) {
                i++;
                args_record_(out, spec->name, argv[i]);
            } else {
                out->error = "option requires a value";
            }
            continue;
        }

        if (tok[0] == '-' && tok[1] != '\0') {
            char c = tok[1];
            const Args_Spec *spec = args_find_short_(specs, spec_count, c);

            if (!spec) { out->error = "unknown short option"; continue; }

            if (!spec->takes_value) {
                args_record_(out, spec->name, NULL);
                continue;
            }

            if (tok[2] != '\0') {
                args_record_(out, spec->name, tok + 2);
            } else if (i + 1 < argc) {
                i++;
                args_record_(out, spec->name, argv[i]);
            } else {
                out->error = "option requires a value";
            }
            continue;
        }

        if (out->positional_count < ARGS_MAX_POSITIONAL) {
            out->positional[out->positional_count++] = tok;
        }
    }
}

const char *args_get(const Args_Result *r, const char *name)
{
    i32 i;
    for (i = 0; i < r->match_count; i++) {
        if (args_streq_(r->matches[i].name, name)) return r->matches[i].value;
    }
    return NULL;
}

i32 args_has(const Args_Result *r, const char *name)
{
    i32 i;
    for (i = 0; i < r->match_count; i++) {
        if (args_streq_(r->matches[i].name, name)) return 1;
    }
    return 0;
}

#endif /* ARGS_IMPLEMENTATION_ */
#endif /* ARGS_IMPLEMENTATION */
