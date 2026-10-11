#ifndef HOLYC_CLI_H_
#define HOLYC_CLI_H_

/* Ported from xgetopt (public domain, "free and unencumbered software").
 * Picked over the sibling getopt.h in the same repo specifically because
 * it has no global state (optind/optopt/optarg live in a caller-owned
 * struct, not static globals) - fits this project's "explicit over
 * hidden state" stance better, and means two independent argv parses
 * (e.g. a test harness parsing its own args while also parsing a
 * sub-command's) don't stomp on each other.
 *
 * Behaves as though the standard getopt() had opterr=0 and its optstring
 * always started with ':' - a missing required argument returns ':',
 * not '?'; an unrecognized option returns '?'. Same algorithm as the
 * original, just i32 instead of int and CLI_GETOPT_INIT zero-init
 * instead of relying on aggregate-init shorthand the caller might get
 * wrong. */

#include "core/base.h"

typedef struct {
    char *optarg;
    i32 optind;
    i32 optopt;
    i32 optpos;
} Cli_Getopt;

#define CLI_GETOPT_INIT { (char *)0, 0, 0, 0 }

/* optstring: getopt-style spec, e.g. "ab:c" means -a and -c are flags,
 * -b takes an argument. Returns the option character, ':' if an option
 * needing an argument didn't get one, '?' for an unrecognized option, or
 * -1 once argv is exhausted (x->optind then indexes the first non-option
 * argument, same convention as standard getopt's optind). */
i32 cli_getopt(Cli_Getopt *x, i32 argc, char **argv, const char *optstring);

#endif /* HOLYC_CLI_H_ */

#ifdef CLI_IMPLEMENTATION
#ifndef CLI_IMPLEMENTATION_
#define CLI_IMPLEMENTATION_

i32 cli_getopt(Cli_Getopt *x, i32 argc, char **argv, const char *optstring)
{
    char *arg;

    arg = argv[!x->optind ? (x->optind += (argc != 0)) : x->optind];

    if (arg && arg[0] == '-' && arg[1] == '-' && !arg[2]) {
        x->optind++;
        return -1;
    } else if (!arg || arg[0] != '-' ||
               ((arg[1] < '0' || arg[1] > '9') &&
                (arg[1] < 'A' || arg[1] > 'Z') &&
                (arg[1] < 'a' || arg[1] > 'z'))) {
        return -1;
    } else {
        const char *spec = optstring;
        while (*spec && arg[x->optpos + 1] != *spec) {
            spec++;
        }
        x->optopt = arg[x->optpos + 1];
        if (!*spec) {
            return '?';
        } else if (spec[1] == ':') {
            if (arg[x->optpos + 2]) {
                x->optarg = arg + x->optpos + 2;
                x->optind++;
                x->optpos = 0;
                return x->optopt;
            } else if (argv[x->optind + 1]) {
                x->optarg = argv[x->optind + 1];
                x->optind += 2;
                x->optpos = 0;
                return x->optopt;
            } else {
                return ':';
            }
        } else {
            if (!arg[++x->optpos + 1]) {
                x->optind++;
                x->optpos = 0;
            }
            return x->optopt;
        }
    }
}

#endif /* CLI_IMPLEMENTATION_ */
#endif /* CLI_IMPLEMENTATION */
