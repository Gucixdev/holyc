#ifndef HOLYC_AST_H_
#define HOLYC_AST_H_

/* Generic tagged expression-tree node - literal/ident/unary/binary/call,
 * the same shape essentially every small expression grammar needs
 * (arithmetic, boolean logic, a config DSL, a formula - see this
 * project's own project/proof/proof.h Proof_Node for an independently
 * arrived-at near-identical shape, built before this generic version
 * existed; not merged into using this file, a real follow-up, not
 * silently done). Deliberately NOT an evaluator - Ast_Node only holds
 * structure, evaluation semantics (what a given AST_BINARY op actually
 * computes) are entirely caller-defined, since that's the one thing
 * that's genuinely different between every consumer (see noast.h, this
 * same directory, for the alternative "skip building the tree at all,
 * evaluate directly while parsing" approach when you don't need the
 * tree itself for anything - re-emitting it, static analysis, multiple
 * evaluation passes - and just want the fastest path to one answer). */

#include "core/arena_alloc.h"

typedef enum {
    AST_LITERAL,  /* a bare number */
    AST_IDENT,    /* a bare name - variable/identifier lookup, meaning caller-defined */
    AST_UNARY,    /* op applied to rhs only */
    AST_BINARY,   /* op applied to lhs and rhs */
    AST_CALL      /* ident(args[0], ..., args[arg_count-1]) */
} Ast_Kind;

#define AST_MAX_CALL_ARGS 8

typedef struct Ast_Node {
    Ast_Kind kind;
    f64 literal_value;        /* AST_LITERAL */
    const char *ident;         /* AST_IDENT/AST_CALL - owned, NUL-terminated, arena copy */
    i32 op;                     /* AST_UNARY/AST_BINARY - caller-defined operator id, this file assigns no meaning to any value */
    struct Ast_Node *lhs, *rhs;  /* AST_BINARY: both; AST_UNARY: rhs only, lhs is NULL */
    struct Ast_Node *args[AST_MAX_CALL_ARGS]; /* AST_CALL */
    i32 arg_count;               /* AST_CALL */
} Ast_Node;

Ast_Node *ast_literal(Arena *a, f64 value);
Ast_Node *ast_ident(Arena *a, const char *name, size_t name_len);
Ast_Node *ast_unary(Arena *a, i32 op, Ast_Node *rhs);
Ast_Node *ast_binary(Arena *a, i32 op, Ast_Node *lhs, Ast_Node *rhs);

/* arg_count must be <= AST_MAX_CALL_ARGS - HOLYC_ASSERT, not a silent
 * truncation, matching signal.h's own "real cap, checkable failure"
 * stance for a different fixed-size structure. */
Ast_Node *ast_call(Arena *a, const char *name, size_t name_len, Ast_Node **args, i32 arg_count);

/* pre-order walk (node visited before its children) - visit returns 0
 * to stop early, matching tree.h's own tree_walk_preorder contract.
 * Returns 0 if visit ever returned 0, else 1. */
i32 ast_walk_preorder(Ast_Node *root, i32 (*visit)(Ast_Node *node, void *ctx), void *ctx);

#endif /* HOLYC_AST_H_ */

#ifdef AST_IMPLEMENTATION
#ifndef AST_IMPLEMENTATION_
#define AST_IMPLEMENTATION_

static Ast_Node *ast_alloc_(Arena *a, Ast_Kind kind)
{
    Ast_Node *n = (Ast_Node *)arena_alloc(a, sizeof(Ast_Node));
    n->kind = kind;
    n->literal_value = 0.0;
    n->ident = NULL;
    n->op = 0;
    n->lhs = NULL;
    n->rhs = NULL;
    n->arg_count = 0;
    return n;
}

static const char *ast_strdup_(Arena *a, const char *s, size_t len)
{
    char *out = (char *)arena_alloc(a, len + 1);
    size_t i;
    for (i = 0; i < len; i++) out[i] = s[i];
    out[len] = '\0';
    return out;
}

Ast_Node *ast_literal(Arena *a, f64 value)
{
    Ast_Node *n = ast_alloc_(a, AST_LITERAL);
    n->literal_value = value;
    return n;
}

Ast_Node *ast_ident(Arena *a, const char *name, size_t name_len)
{
    Ast_Node *n = ast_alloc_(a, AST_IDENT);
    n->ident = ast_strdup_(a, name, name_len);
    return n;
}

Ast_Node *ast_unary(Arena *a, i32 op, Ast_Node *rhs)
{
    Ast_Node *n = ast_alloc_(a, AST_UNARY);
    n->op = op;
    n->rhs = rhs;
    return n;
}

Ast_Node *ast_binary(Arena *a, i32 op, Ast_Node *lhs, Ast_Node *rhs)
{
    Ast_Node *n = ast_alloc_(a, AST_BINARY);
    n->op = op;
    n->lhs = lhs;
    n->rhs = rhs;
    return n;
}

Ast_Node *ast_call(Arena *a, const char *name, size_t name_len, Ast_Node **args, i32 arg_count)
{
    Ast_Node *n = ast_alloc_(a, AST_CALL);
    i32 i;
    HOLYC_ASSERT(arg_count <= AST_MAX_CALL_ARGS);
    n->ident = ast_strdup_(a, name, name_len);
    n->arg_count = arg_count;
    for (i = 0; i < arg_count; i++) n->args[i] = args[i];
    return n;
}

static i32 ast_walk_preorder_(Ast_Node *node, i32 (*visit)(Ast_Node *, void *), void *ctx)
{
    if (!node) return 1;
    if (!visit(node, ctx)) return 0;

    switch (node->kind) {
    case AST_UNARY:
        return ast_walk_preorder_(node->rhs, visit, ctx);
    case AST_BINARY:
        if (!ast_walk_preorder_(node->lhs, visit, ctx)) return 0;
        return ast_walk_preorder_(node->rhs, visit, ctx);
    case AST_CALL: {
        i32 i;
        for (i = 0; i < node->arg_count; i++) {
            if (!ast_walk_preorder_(node->args[i], visit, ctx)) return 0;
        }
        return 1;
    }
    default:
        return 1;
    }
}

i32 ast_walk_preorder(Ast_Node *root, i32 (*visit)(Ast_Node *node, void *ctx), void *ctx)
{
    return ast_walk_preorder_(root, visit, ctx);
}

#endif /* AST_IMPLEMENTATION_ */
#endif /* AST_IMPLEMENTATION */
