#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define AST_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "util/ast.h"

#include <string.h>

#define OP_ADD 1
#define OP_NEG 2

static i32 count_(Ast_Node *node, void *ctx)
{
    (void)node;
    (*(i32 *)ctx)++;
    return 1;
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_ast\n");

    {
        Ast_Node *lit = ast_literal(&a, 42.0);
        TEST_CHECK(lit->kind == AST_LITERAL && lit->literal_value == 42.0, "ast_literal builds a literal node with the right value");
    }

    {
        Ast_Node *id = ast_ident(&a, "hello_world", 5);
        TEST_CHECK(id->kind == AST_IDENT, "ast_ident builds an identifier node");
        TEST_CHECK(strcmp(id->ident, "hello") == 0, "ast_ident copies exactly name_len bytes, not the whole C string past it");
    }

    {
        Ast_Node *lit = ast_literal(&a, 3.0);
        Ast_Node *neg = ast_unary(&a, OP_NEG, lit);
        TEST_CHECK(neg->kind == AST_UNARY && neg->op == OP_NEG && neg->rhs == lit && neg->lhs == NULL, "ast_unary wires rhs, leaves lhs NULL, preserves the op tag");
    }

    {
        Ast_Node *l = ast_literal(&a, 1.0);
        Ast_Node *r = ast_literal(&a, 2.0);
        Ast_Node *add = ast_binary(&a, OP_ADD, l, r);
        TEST_CHECK(add->kind == AST_BINARY && add->lhs == l && add->rhs == r && add->op == OP_ADD, "ast_binary wires both lhs and rhs, preserves the op tag");
    }

    {
        Ast_Node *arg0 = ast_literal(&a, 1.0);
        Ast_Node *arg1 = ast_literal(&a, 2.0);
        Ast_Node *args[2];
        Ast_Node *call;
        args[0] = arg0; args[1] = arg1;
        call = ast_call(&a, "max", 3, args, 2);
        TEST_CHECK(call->kind == AST_CALL && call->arg_count == 2, "ast_call builds a call node with the right arg count");
        TEST_CHECK(strcmp(call->ident, "max") == 0, "ast_call's ident is the function name");
        TEST_CHECK(call->args[0] == arg0 && call->args[1] == arg1, "ast_call's args array holds the given nodes in order");
    }

    /* pre-order walk over a real small tree: (1 + 2) with a call at the root's sibling */
    {
        Ast_Node *l = ast_literal(&a, 1.0);
        Ast_Node *r = ast_literal(&a, 2.0);
        Ast_Node *add = ast_binary(&a, OP_ADD, l, r);
        Ast_Node *args[1];
        Ast_Node *call;
        i32 visits = 0;
        i32 complete;

        args[0] = add;
        call = ast_call(&a, "f", 1, args, 1);

        complete = ast_walk_preorder(call, count_, &visits);
        TEST_CHECK(complete, "a walk with no early stop reports complete");
        TEST_CHECK(visits == 4, "pre-order walk visits all 4 nodes: call, add, its lhs, its rhs");
    }

    {
        Ast_Node *lit = ast_literal(&a, 1.0);
        Ast_Node *neg = ast_unary(&a, OP_NEG, lit);
        i32 visits = 0;
        ast_walk_preorder(neg, count_, &visits);
        TEST_CHECK(visits == 2, "a unary node's walk visits itself and its single rhs child, nothing more");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
