#ifndef HOLYC_FORMULA_H_
#define HOLYC_FORMULA_H_

/* A formula BUILDS a value from an expression tree + a variable
 * context - the concrete-evaluation counterpart to ast.h's
 * deliberately semantics-free node structure (ast.h holds structure
 * only; this file is what actually WALKS an Ast_Node and computes a
 * number from it). Contrast with fabric.h (same project): fabric.h is
 * a compile-time macro that GENERATES a dispatch function from a
 * static list; formula.h is a RUNTIME data structure (an Ast_Node
 * tree, mutable, buildable at runtime from parsed/user-provided input)
 * that produces a value when evaluated - a formula is data, a fabric
 * dispatcher is generated code.
 *
 * Formula_Op deliberately reuses ast.h's Ast_Node.op field (a caller-
 * defined i32) rather than ast.h knowing anything about arithmetic -
 * exactly the separation of concerns ast.h's own header comment
 * describes. */

#include "util/ast.h"

typedef enum {
    FORMULA_OP_ADD,
    FORMULA_OP_SUB,
    FORMULA_OP_MUL,
    FORMULA_OP_DIV,
    FORMULA_OP_NEG /* unary */
} Formula_Op;

#ifndef FORMULA_MAX_BINDINGS
#define FORMULA_MAX_BINDINGS 32
#endif

typedef struct {
    const char *name;
    f64 value;
} Formula_Binding;

typedef struct {
    Formula_Binding bindings[FORMULA_MAX_BINDINGS];
    i32 count;
} Formula_Context;

void formula_context_init(Formula_Context *ctx);

/* overwrites the value if `name` is already bound (same "last set
 * wins, position unchanged" contract hash_map_put documents for the
 * same reason). Returns 0 if already at FORMULA_MAX_BINDINGS and
 * `name` is a genuinely new binding - a real, checkable failure. */
i32 formula_context_set(Formula_Context *ctx, const char *name, f64 value);

/* returns 1 and writes *out if `name` is bound, 0 (leaving *out
 * unspecified) if not. */
i32 formula_context_get(const Formula_Context *ctx, const char *name, f64 *out);

/* evaluates `node` (an Ast_Node tree built with ast_literal/ast_binary/
 * ast_unary using Formula_Op values as the op tag - ast_ident for
 * variable references, resolved against `ctx`) into *out. Returns 0
 * (leaving *out unspecified) on an unbound identifier, division by
 * zero, or an op/kind this evaluator doesn't recognize (AST_CALL is a
 * real, documented gap - no builtin function table exists yet) rather
 * than a silent NaN/garbage result. */
i32 formula_eval(const Ast_Node *node, const Formula_Context *ctx, f64 *out);

#endif /* HOLYC_FORMULA_H_ */

#ifdef FORMULA_IMPLEMENTATION
#ifndef FORMULA_IMPLEMENTATION_
#define FORMULA_IMPLEMENTATION_

static i32 formula_streq_(const char *a, const char *b)
{
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

void formula_context_init(Formula_Context *ctx)
{
    ctx->count = 0;
}

i32 formula_context_set(Formula_Context *ctx, const char *name, f64 value)
{
    i32 i;
    for (i = 0; i < ctx->count; i++) {
        if (formula_streq_(ctx->bindings[i].name, name)) {
            ctx->bindings[i].value = value;
            return 1;
        }
    }
    if (ctx->count >= FORMULA_MAX_BINDINGS) return 0;
    ctx->bindings[ctx->count].name = name;
    ctx->bindings[ctx->count].value = value;
    ctx->count++;
    return 1;
}

i32 formula_context_get(const Formula_Context *ctx, const char *name, f64 *out)
{
    i32 i;
    for (i = 0; i < ctx->count; i++) {
        if (formula_streq_(ctx->bindings[i].name, name)) {
            *out = ctx->bindings[i].value;
            return 1;
        }
    }
    return 0;
}

i32 formula_eval(const Ast_Node *node, const Formula_Context *ctx, f64 *out)
{
    switch (node->kind) {
    case AST_LITERAL:
        *out = node->literal_value;
        return 1;

    case AST_IDENT:
        return formula_context_get(ctx, node->ident, out);

    case AST_UNARY: {
        f64 rhs;
        if (!formula_eval(node->rhs, ctx, &rhs)) return 0;
        if (node->op == FORMULA_OP_NEG) { *out = -rhs; return 1; }
        return 0;
    }

    case AST_BINARY: {
        f64 lhs, rhs;
        if (!formula_eval(node->lhs, ctx, &lhs)) return 0;
        if (!formula_eval(node->rhs, ctx, &rhs)) return 0;
        switch (node->op) {
        case FORMULA_OP_ADD: *out = lhs + rhs; return 1;
        case FORMULA_OP_SUB: *out = lhs - rhs; return 1;
        case FORMULA_OP_MUL: *out = lhs * rhs; return 1;
        case FORMULA_OP_DIV:
            if (rhs == 0.0) return 0;
            *out = lhs / rhs;
            return 1;
        default: return 0;
        }
    }

    default:
        return 0; /* AST_CALL - no builtin function table yet, see this file's own header comment */
    }
}

#endif /* FORMULA_IMPLEMENTATION_ */
#endif /* FORMULA_IMPLEMENTATION */
