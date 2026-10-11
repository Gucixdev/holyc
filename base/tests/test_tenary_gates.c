/* test_tenary_gates.c - balanced ternary logic: min/max are Kleene AND/OR, neg is NOT, diff is the
 * three-way compare. Checks the full 3x3 tables, De Morgan, double negation and the array forms. */
#define TENARY_GATES_IMPLEMENTATION
#include "math/tenary_gates.h"
#include <stdio.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, m); fails++; } } while (0)

int main(void)
{
	static const Trit v[3] = {-1, 0, 1};
	/* diff(a,b) = sign(a-b) */
	static const Trit diff_tab[3][3] = {{0, -1, -1}, {1, 0, -1}, {1, 1, 0}};
	int i, j;
	Trit a[5] = {-1, 0, 1, 1, -1}, b[5] = {1, 0, -1, 1, -1}, out[5];

	for (i = 0; i < 3; i++) {
		CHECK(tneg(tneg(v[i])) == v[i], "double negation");
		CHECK(tmin(v[i], 1) == v[i] && tmax(v[i], -1) == v[i], "1 is the AND identity, -1 the OR identity");
		CHECK(tmin(v[i], -1) == -1 && tmax(v[i], 1) == 1, "-1 absorbs AND, 1 absorbs OR");
		for (j = 0; j < 3; j++) {
			CHECK(tneg(tmin(v[i], v[j])) == tmax(tneg(v[i]), tneg(v[j])), "De Morgan");
			CHECK(tmin(v[i], v[j]) == tmin(v[j], v[i]) && tmax(v[i], v[j]) == tmax(v[j], v[i]), "commutative");
			CHECK(tdiff(v[i], v[j]) == diff_tab[i][j], "diff table");
			CHECK(tdiff(v[i], v[j]) == -tdiff(v[j], v[i]), "diff is antisymmetric");
		}
	}
	tmin_array(out, a, b, 5);
	CHECK(out[0] == -1 && out[1] == 0 && out[2] == -1 && out[3] == 1 && out[4] == -1, "tmin_array");
	tmax_array(out, a, b, 5);
	CHECK(out[0] == 1 && out[1] == 0 && out[2] == 1 && out[3] == 1 && out[4] == -1, "tmax_array");
	tdiff_array(out, a, b, 5);
	CHECK(out[0] == -1 && out[1] == 0 && out[2] == 1 && out[3] == 0 && out[4] == 0, "tdiff_array");
	tneg_array(a, a, 5); /* output may alias input */
	CHECK(a[0] == 1 && a[1] == 0 && a[2] == -1, "tneg_array in place");

	if (fails) { printf("%d FAILED\n", fails); return 1; }
	printf("test_tenary_gates: ok\n");
	return 0;
}
