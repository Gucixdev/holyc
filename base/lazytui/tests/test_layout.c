/* test_layout.c - boxes on top of the solver: rows/cols split space, FIXED/FILL/MIN/MAX/PCT sizes, gaps, padding, nested trees,
 * anchors by name, and re-solving cheaply when only the window size changes. */
#include "lazytui.h"
#include <stdio.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static bool req(lt_rect r, int x, int y, int w, int h) { return r.x == x && r.y == y && r.w == w && r.h == h; }

int main(void)
{
	/* a row of three fixed-width children fills a 30-wide root exactly */
	lt_layout *l = lt_layout_new();
	lt_node *root = lt_layout_root(l, LT_ROW);
	lt_node *a = lt_node_add(root, "a", LT_COL, LT_FIXED(10));
	lt_node *b = lt_node_add(root, "b", LT_COL, LT_FILL(1));
	lt_node *c = lt_node_add(root, "c", LT_COL, LT_FIXED(5));
	CHECK(lt_layout_solve(l, 30, 10), "solves");
	CHECK(req(lt_node_rect(a), 0, 0, 10, 10), "fixed child a is 10 wide at x=0");
	CHECK(req(lt_node_rect(b), 10, 0, 15, 10), "the fill child takes the remainder");
	CHECK(req(lt_node_rect(c), 25, 0, 5, 10), "fixed child c is 5 wide at the end");
	lt_layout_free(l);

	/* two FILL children with weights 1:3 split the remainder proportionally */
	l = lt_layout_new();
	root = lt_layout_root(l, LT_ROW);
	a = lt_node_add(root, "a", LT_COL, LT_FILL(1));
	b = lt_node_add(root, "b", LT_COL, LT_FILL(3));
	CHECK(lt_layout_solve(l, 40, 10), "solves");
	lt_rect ra = lt_node_rect(a), rb = lt_node_rect(b);
	CHECK(ra.w + rb.w == 40, "the fill children use the whole width");
	CHECK(rb.w > ra.w * 2, "the 3x-weighted child gets substantially more");
	lt_layout_free(l);

	/* MIN and MAX */
	l = lt_layout_new();
	root = lt_layout_root(l, LT_COL);
	a = lt_node_add(root, "a", LT_ROW, LT_MIN(4));
	b = lt_node_add(root, "b", LT_ROW, LT_MAX(3));
	c = lt_node_add(root, "c", LT_ROW, LT_FILL(1));
	CHECK(lt_layout_solve(l, 8, 20), "solves with little height");
	CHECK(lt_node_rect(a).h >= 4, "MIN(4) never shrinks below 4");
	CHECK(lt_node_rect(b).h <= 3, "MAX(3) never grows past 3");
	lt_layout_free(l);

	/* PCT: 25% of a 200-wide row */
	l = lt_layout_new();
	root = lt_layout_root(l, LT_ROW);
	a = lt_node_add(root, "a", LT_COL, LT_PCT(25));
	b = lt_node_add(root, "b", LT_COL, LT_FILL(1));
	CHECK(lt_layout_solve(l, 200, 10), "solves");
	CHECK(lt_node_rect(a).w == 50, "25%% of 200 is 50");
	lt_layout_free(l);

	/* gap and padding */
	l = lt_layout_new();
	root = lt_layout_root(l, LT_ROW);
	lt_node_gap(root, 2);
	a = lt_node_add(root, "a", LT_COL, LT_FIXED(5));
	b = lt_node_add(root, "b", LT_COL, LT_FIXED(5));
	CHECK(lt_layout_solve(l, 30, 10), "solves");
	CHECK(lt_node_rect(b).x == lt_node_rect(a).x + lt_node_rect(a).w + 2, "a 2-cell gap separates the children");
	lt_layout_free(l);

	l = lt_layout_new();
	root = lt_layout_root(l, LT_COL);
	lt_node_pad(root, 1, 2, 1, 2);
	a = lt_node_add(root, "a", LT_ROW, LT_FILL(1));
	CHECK(lt_layout_solve(l, 20, 10), "solves");
	lt_rect pr = lt_node_rect(a);
	CHECK(pr.x == 2 && pr.y == 1 && pr.w == 20 - 4 && pr.h == 10 - 2, "padding shrinks the content rect on all sides");
	lt_layout_free(l);

	/* a STACK: children occupy the same rectangle, on top of one another */
	l = lt_layout_new();
	root = lt_layout_root(l, LT_STACK);
	a = lt_node_add(root, "a", LT_COL, LT_FILL(1));
	b = lt_node_add(root, "b", LT_COL, LT_FILL(1));
	CHECK(lt_layout_solve(l, 12, 6), "solves");
	CHECK(req(lt_node_rect(a), 0, 0, 12, 6) && req(lt_node_rect(b), 0, 0, 12, 6), "stacked children share the same rect");
	lt_layout_free(l);

	/* nested rows/cols, and lookup by name */
	l = lt_layout_new();
	root = lt_layout_root(l, LT_COL);
	lt_node *header = lt_node_add(root, "header", LT_ROW, LT_FIXED(1));
	lt_node *body = lt_node_add(root, "body", LT_ROW, LT_FILL(1));
	lt_node *sidebar = lt_node_add(body, "sidebar", LT_COL, LT_FIXED(12));
	lt_node *main_ = lt_node_add(body, "main", LT_COL, LT_FILL(1));
	(void)header;
	CHECK(lt_layout_solve(l, 60, 20), "solves a nested tree");
	CHECK(lt_node_rect(sidebar).h == lt_node_rect(body).h, "the sidebar fills the body's height");
	CHECK(lt_node_rect(main_).w == 60 - 12, "main takes what the sidebar left");
	CHECK(lt_layout_find(l, "sidebar") == sidebar && lt_layout_find(l, "main") == main_, "nodes are found by name");
	CHECK(lt_layout_find(l, "nope") == NULL, "an unknown name is not found");

	/* re-solving after only the window size changed is cheap and still correct */
	CHECK(lt_layout_solve(l, 100, 30), "re-solves at a new size");
	CHECK(lt_node_rect(main_).w == 100 - 12, "the new size propagates");
	lt_layout_free(l);

	/* a child with no size defaults sanely (does not crash, gets some rect) */
	l = lt_layout_new();
	root = lt_layout_root(l, LT_ROW);
	a = lt_node_add(root, NULL, LT_COL, LT_FILL(1));
	CHECK(lt_layout_solve(l, 10, 10), "an unnamed child solves too");
	CHECK(lt_node_rect(a).w == 10, "and fills the root");
	lt_layout_free(l);

	printf(fails ? "layout: %d failed\n" : "layout: ok\n", fails);
	return fails != 0;
}
