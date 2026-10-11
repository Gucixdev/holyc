#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define AST_IMPLEMENTATION
#define FORMULA_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "util/ast.h"
#include "util/formula.h"

static i32 near_(f64 a, f64 b) { f64 d = a - b; if (d < 0) d = -d; return d < 0.0001; }

int main(void)
{
    Arena a = {0};
    Formula_Context ctx;
    f64 result;

    print_cstr("test_formula\n");

    formula_context_init(&ctx);
    TEST_CHECK(formula_context_set(&ctx, "x", 3.0) == 1, "setting a new binding succeeds");
    TEST_CHECK(formula_context_set(&ctx, "y", 4.0) == 1, "setting a second new binding succeeds");

    {
        f64 v;
        TEST_CHECK(formula_context_get(&ctx, "x", &v) && v == 3.0, "context_get retrieves exactly what was set");
        TEST_CHECK(!formula_context_get(&ctx, "nope", &v), "context_get on an unbound name returns 0");
    }

    TEST_CHECK(formula_context_set(&ctx, "x", 30.0) == 1, "re-setting an existing binding still succeeds (overwrite)");
    {
        f64 v;
        formula_context_get(&ctx, "x", &v);
        TEST_CHECK(v == 30.0, "re-setting a binding actually overwrites the value");
    }

    /* formula: (x + y) * 2  -  x=30, y=4 -> 68 */
    {
        Ast_Node *x = ast_ident(&a, "x", 1);
        Ast_Node *y = ast_ident(&a, "y", 1);
        Ast_Node *sum = ast_binary(&a, FORMULA_OP_ADD, x, y);
        Ast_Node *two = ast_literal(&a, 2.0);
        Ast_Node *formula = ast_binary(&a, FORMULA_OP_MUL, sum, two);

        TEST_CHECK(formula_eval(formula, &ctx, &result) && near_(result, 68.0), "a real (x+y)*2 formula evaluates to the correct value against the context");
    }

    /* unbound identifier in the formula fails cleanly */
    {
        Ast_Node *unknown = ast_ident(&a, "z", 1);
        TEST_CHECK(!formula_eval(unknown, &ctx, &result), "a formula referencing an unbound identifier fails cleanly, not a silent 0");
    }

    /* division by zero fails cleanly */
    {
        Ast_Node *one = ast_literal(&a, 1.0);
        Ast_Node *zero = ast_literal(&a, 0.0);
        Ast_Node *div = ast_binary(&a, FORMULA_OP_DIV, one, zero);
        TEST_CHECK(!formula_eval(div, &ctx, &result), "division by zero in a formula fails cleanly, not inf/nan");
    }

    /* unary negation */
    {
        Ast_Node *five = ast_literal(&a, 5.0);
        Ast_Node *neg = ast_unary(&a, FORMULA_OP_NEG, five);
        TEST_CHECK(formula_eval(neg, &ctx, &result) && near_(result, -5.0), "unary negation evaluates correctly");
    }

    /* an unset FORMULA_MAX_BINDINGS cap is respected */
    {
        Formula_Context full;
        i32 i;
        char names[FORMULA_MAX_BINDINGS][8];
        formula_context_init(&full);
        for (i = 0; i < FORMULA_MAX_BINDINGS; i++) {
            names[i][0] = 'v'; names[i][1] = (char)('0' + i / 10); names[i][2] = (char)('0' + i % 10); names[i][3] = '\0';
            TEST_CHECK(formula_context_set(&full, names[i], (f64)i) == 1, "filling up to FORMULA_MAX_BINDINGS all succeed");
        }
        TEST_CHECK(formula_context_set(&full, "one_too_many", 0.0) == 0, "setting one more brand-new binding past the cap fails cleanly");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
