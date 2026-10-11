#ifndef HOLYC_NOAST_H_
#define HOLYC_NOAST_H_

/* "No AST" arithmetic expression evaluator - recursive-descent parsing
 * that computes the result DIRECTLY as it parses, never materializing
 * an Ast_Node tree at all (contrast with ast.h, same directory, which
 * exists for exactly the cases that DO need the tree - re-emitting it,
 * static analysis, evaluating the same expression many times with
 * different variable bindings). For a one-shot "parse this string, get
 * one number back" calculator, skipping tree construction is strictly
 * less work: no arena allocation per node, one pass over the input
 * instead of parse-then-walk. The real cost: no reuse (re-evaluating
 * the same expression means re-parsing the string from scratch) and no
 * way to inspect/transform the expression's structure - a real,
 * accepted tradeoff for the "just evaluate it" use case, not a
 * strictly-better replacement for ast.h.
 *
 * Grammar (standard arithmetic precedence, left-associative):
 *   expr   := term (('+'|'-') term)*
 *   term   := factor (('*'|'/') factor)*
 *   factor := '-' factor | primary
 *   primary := number | '(' expr ')'
 * Division by zero and a malformed expression (unexpected character,
 * unbalanced parens, trailing garbage after a complete expression) are
 * both real, reported failures (noast_eval returns 0), not NaN/silent
 * garbage. */

#include "base.h"

/* returns 1 and writes the result to *out on success, 0 on any parse
 * error or division by zero (*out is left unspecified on failure -
 * don't use it, matching this project's own "unspecified on failure"
 * out-param convention, e.g. bdf_dec.h's bdf_parse). */
i32 noast_eval(const char *expr, f64 *out);

#endif /* HOLYC_NOAST_H_ */

#ifdef NOAST_IMPLEMENTATION
#ifndef NOAST_IMPLEMENTATION_
#define NOAST_IMPLEMENTATION_

typedef struct {
    const char *p;
    i32 ok;
} Noast_State_;

static void noast_skip_space_(Noast_State_ *s)
{
    while (*s->p == ' ' || *s->p == '\t') s->p++;
}

static f64 noast_expr_(Noast_State_ *s);

static f64 noast_primary_(Noast_State_ *s)
{
    f64 v;
    noast_skip_space_(s);

    if (*s->p == '(') {
        s->p++;
        v = noast_expr_(s);
        noast_skip_space_(s);
        if (*s->p != ')') { s->ok = 0; return 0.0; }
        s->p++;
        return v;
    }

    if ((*s->p >= '0' && *s->p <= '9') || *s->p == '.') {
        f64 whole = 0.0, frac = 0.0, frac_scale = 1.0;
        i32 any_digit = 0;
        while (*s->p >= '0' && *s->p <= '9') { whole = whole * 10.0 + (f64)(*s->p - '0'); s->p++; any_digit = 1; }
        if (*s->p == '.') {
            s->p++;
            while (*s->p >= '0' && *s->p <= '9') { frac_scale *= 10.0; frac = frac * 10.0 + (f64)(*s->p - '0'); s->p++; any_digit = 1; }
        }
        if (!any_digit) { s->ok = 0; return 0.0; }
        return whole + frac / frac_scale;
    }

    s->ok = 0;
    return 0.0;
}

static f64 noast_factor_(Noast_State_ *s)
{
    noast_skip_space_(s);
    if (*s->p == '-') { s->p++; return -noast_factor_(s); }
    if (*s->p == '+') { s->p++; return noast_factor_(s); }
    return noast_primary_(s);
}

static f64 noast_term_(Noast_State_ *s)
{
    f64 v = noast_factor_(s);
    for (;;) {
        noast_skip_space_(s);
        if (*s->p == '*') { s->p++; v *= noast_factor_(s); }
        else if (*s->p == '/') {
            f64 rhs;
            s->p++;
            rhs = noast_factor_(s);
            if (rhs == 0.0) { s->ok = 0; return 0.0; }
            v /= rhs;
        } else {
            return v;
        }
    }
}

static f64 noast_expr_(Noast_State_ *s)
{
    f64 v = noast_term_(s);
    for (;;) {
        noast_skip_space_(s);
        if (*s->p == '+') { s->p++; v += noast_term_(s); }
        else if (*s->p == '-') { s->p++; v -= noast_term_(s); }
        else return v;
    }
}

i32 noast_eval(const char *expr, f64 *out)
{
    Noast_State_ s;
    f64 result;

    s.p = expr;
    s.ok = 1;

    result = noast_expr_(&s);
    if (!s.ok) return 0;

    noast_skip_space_(&s);
    if (*s.p != '\0') return 0; /* trailing garbage after a complete expression */

    *out = result;
    return 1;
}

#endif /* NOAST_IMPLEMENTATION_ */
#endif /* NOAST_IMPLEMENTATION */
