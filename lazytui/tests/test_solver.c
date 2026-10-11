/* test_solver.c - the Cassowary constraint solver: basic equalities/inequalities, strengths, edit variables, infeasible REQUIRED
 * constraints reported instead of crashing. */
#include "lazytui.h"
#include <math.h>
#include <stdio.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static bool near(double a, double b) { return fabs(a - b) < 1e-6; }

int main(void)
{
	/* x = 10 (required) */
	lt_solver *s = lt_solver_new();
	lt_var x = lt_var_new(s);
	lt_term tx = {x, 1};
	CHECK(lt_solver_add(s, &tx, 1, -10, LT_EQ, LT_REQUIRED), "x = 10 is satisfiable");
	CHECK(near(lt_solver_value(s, x), 10), "x solves to 10");
	lt_solver_free(s);

	/* x + y = 100, x >= 30 (required), x weakly wants to be 40 */
	s = lt_solver_new();
	x = lt_var_new(s);
	lt_var y = lt_var_new(s);
	lt_term xy[2] = {{x, 1}, {y, 1}};
	CHECK(lt_solver_add(s, xy, 2, -100, LT_EQ, LT_REQUIRED), "x + y = 100");
	lt_term xo = {x, 1};
	CHECK(lt_solver_add(s, &xo, 1, -30, LT_GE, LT_REQUIRED), "x >= 30");
	CHECK(lt_solver_edit(s, x, LT_STRONG), "x is editable");
	CHECK(lt_solver_suggest(s, x, 40), "suggest x = 40");
	CHECK(near(lt_solver_value(s, x), 40) && near(lt_solver_value(s, y), 60), "the pair solves to 40/60");
	CHECK(lt_solver_suggest(s, x, 10), "suggest x = 10 (below the >= 30 floor)");
	CHECK(near(lt_solver_value(s, x), 30) && near(lt_solver_value(s, y), 70), "the REQUIRED floor wins over the suggestion");
	lt_solver_free(s);

	/* a weak preference loses to a strong one on the same variable */
	s = lt_solver_new();
	x = lt_var_new(s);
	lt_term tw = {x, 1};
	CHECK(lt_solver_add(s, &tw, 1, -5, LT_EQ, LT_WEAK), "x == 5 (weak)");
	lt_term ts = {x, 1};
	CHECK(lt_solver_add(s, &ts, 1, -20, LT_EQ, LT_STRONG), "x == 20 (strong)");
	CHECK(near(lt_solver_value(s, x), 20), "the strong constraint wins");
	lt_solver_free(s);

	/* two REQUIRED constraints that contradict: reported false, does not crash */
	s = lt_solver_new();
	x = lt_var_new(s);
	lt_term a1 = {x, 1}, a2 = {x, 1};
	CHECK(lt_solver_add(s, &a1, 1, -10, LT_EQ, LT_REQUIRED), "x = 10");
	bool ok = lt_solver_add(s, &a2, 1, -20, LT_EQ, LT_REQUIRED);
	CHECK(!ok, "x = 20 at the same time is refused, not crashed");
	lt_solver_free(s);

	/* many variables and constraints still solve (a small chain) */
	s = lt_solver_new();
	lt_var v[10];
	for (int i = 0; i < 10; i++) v[i] = lt_var_new(s);
	lt_term root = {v[0], 1};
	lt_solver_add(s, &root, 1, 0, LT_EQ, LT_REQUIRED);
	for (int i = 1; i < 10; i++) { lt_term ch[2] = {{v[i], 1}, {v[i - 1], -1}}; lt_solver_add(s, ch, 2, -1, LT_EQ, LT_REQUIRED); }
	bool chain_ok = true;
	for (int i = 0; i < 10; i++) if (!near(lt_solver_value(s, v[i]), i)) chain_ok = false;
	CHECK(chain_ok, "a chain of ten equalities solves in order");
	lt_solver_free(s);

	printf(fails ? "solver: %d failed\n" : "solver: ok\n", fails);
	return fails != 0;
}
