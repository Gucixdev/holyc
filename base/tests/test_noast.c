#include "test_common.h"
#define NOAST_IMPLEMENTATION
#include "noast.h"

static i32 near_(f64 a, f64 b) { f64 d = a - b; if (d < 0) d = -d; return d < 0.0001; }

int main(void)
{
    f64 v;

    print_cstr("test_noast\n");

    TEST_CHECK(noast_eval("2 + 3", &v) && near_(v, 5.0), "simple addition");
    TEST_CHECK(noast_eval("2 + 3 * 4", &v) && near_(v, 14.0), "precedence: multiplication before addition");
    TEST_CHECK(noast_eval("(2 + 3) * 4", &v) && near_(v, 20.0), "parentheses override precedence");
    TEST_CHECK(noast_eval("-5 + 3", &v) && near_(v, -2.0), "unary minus at the start of an expression");
    TEST_CHECK(noast_eval("10 / 4", &v) && near_(v, 2.5), "division produces a real fractional result");
    TEST_CHECK(noast_eval("2 - 3 - 4", &v) && near_(v, -5.0), "left-associative subtraction: (2-3)-4, not 2-(3-4)");
    TEST_CHECK(noast_eval("2.5 * 2", &v) && near_(v, 5.0), "decimal literals parse correctly");
    TEST_CHECK(noast_eval("-(3 + 4)", &v) && near_(v, -7.0), "unary minus applied to a parenthesized subexpression");
    TEST_CHECK(noast_eval("1 + 2 * (3 - 1) / 2", &v) && near_(v, 3.0), "a mixed-precedence expression with nesting evaluates correctly");

    TEST_CHECK(!noast_eval("1 / 0", &v), "division by zero is a reported failure, not inf/nan silently returned");
    TEST_CHECK(!noast_eval("(1 + 2", &v), "an unbalanced open paren is rejected");
    TEST_CHECK(!noast_eval("1 + + ", &v), "a malformed trailing operator is rejected");
    TEST_CHECK(!noast_eval("1 2", &v), "trailing garbage after a complete expression is rejected, not silently truncated");
    TEST_CHECK(!noast_eval("abc", &v), "non-numeric garbage is rejected");
    TEST_CHECK(!noast_eval("", &v), "an empty expression is rejected");

    print_flush();
    return test_failures_ != 0;
}
