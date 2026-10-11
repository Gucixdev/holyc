/* tgates - command-line ternary logic gates over balanced ternary {-1,0,1}:
 * neg, min, max and diff (Kleene/Lukasiewicz definitions, see
 * tenary_gates.h). A TRIT is -1, 0 or 1; a comma separated list is the array
 * form (min, max and diff need two lists of the same length).
 *
 *   tgates neg  TRIT[,TRIT...]
 *   tgates min  TRIT[,TRIT...] TRIT[,TRIT...]
 *   tgates max  TRIT[,TRIT...] TRIT[,TRIT...]
 *   tgates diff TRIT[,TRIT...] TRIT[,TRIT...]
 */

#define BASE_TYPE_IMPLEMENTATION
#include "base.h"
#define TENARY_GATES_IMPLEMENTATION
#include "tenary_gates.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TGATES_MAX_TRITS 256

static i32 tgates_parse_list_(Trit *out, const char *s)
{
    i32 n = 0;
    char buf[2048];
    char *tok, *saveptr;

    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    tok = strtok_r(buf, ",", &saveptr);
    while (tok && n < TGATES_MAX_TRITS) {
        i32 v = atoi(tok);
        if (v < -1 || v > 1) {
            fprintf(stderr, "tgates: '%s' is not a trit (-1, 0, or 1)\n", tok);
            return -1;
        }
        out[n++] = (Trit)v;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    return n;
}

static void tgates_print_(const Trit *v, i32 n)
{
    i32 i;
    for (i = 0; i < n; i++) printf(i == 0 ? "%d" : ",%d", (i32)v[i]);
    putchar('\n');
}

static i32 tgates_neg_(const char *arg)
{
    Trit in[TGATES_MAX_TRITS], out[TGATES_MAX_TRITS];
    i32 n = tgates_parse_list_(in, arg);
    if (n < 0) return 1;
    tneg_array(out, in, (size_t)n);
    tgates_print_(out, n);
    return 0;
}

static i32 tgates_binary_(const char *a, const char *b, void (*fn)(Trit *, const Trit *, const Trit *, size_t))
{
    Trit av[TGATES_MAX_TRITS], bv[TGATES_MAX_TRITS], out[TGATES_MAX_TRITS];
    i32 na = tgates_parse_list_(av, a);
    i32 nb = tgates_parse_list_(bv, b);
    if (na < 0 || nb < 0) return 1;
    if (na != nb) {
        fprintf(stderr, "tgates: operand lists have different lengths (%d vs %d)\n", na, nb);
        return 1;
    }
    fn(out, av, bv, (size_t)na);
    tgates_print_(out, na);
    return 0;
}

static void tgates_usage_(void)
{
    fprintf(stderr, "usage: tgates neg TRIT[,TRIT...]\n"
                     "       tgates min|max|diff TRIT[,TRIT...] TRIT[,TRIT...]\n");
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "neg") == 0) return tgates_neg_(argv[2]);
    if (argc == 4 && strcmp(argv[1], "min") == 0) return tgates_binary_(argv[2], argv[3], tmin_array);
    if (argc == 4 && strcmp(argv[1], "max") == 0) return tgates_binary_(argv[2], argv[3], tmax_array);
    if (argc == 4 && strcmp(argv[1], "diff") == 0) return tgates_binary_(argv[2], argv[3], tdiff_array);
    tgates_usage_();
    return 1;
}
