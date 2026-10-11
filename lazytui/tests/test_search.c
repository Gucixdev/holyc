/* test_search.c - the five ranking modes over a small document set: filter (substring), fuzzy (subsequence, scored), tag (tag:/#/-tag:),
 * regex (POSIX ERE), mcts (deterministic, tag-group UCB1). Also lt_list_set_order/reset_order. */
#include "lazytui.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)

static const lt_doc DOCS[] = {
	{"open kernel log", "system log"},
	{"close kernel log", "system log"},
	{"open font size", "ui"},
	{"list open files", "system"},
	{"regex.*special (chars)", "misc"},
};
#define NDOC ((int)(sizeof DOCS / sizeof DOCS[0]))

static bool contains(const int *out, int n, int idx) { for (int i = 0; i < n; i++) if (out[i] == idx) return true; return false; }

int main(void)
{
	int out[16];

	/* filter: every query word must be a substring somewhere */
	int n = lt_search(LT_SEARCH_FILTER, "open log", DOCS, NDOC, out, 16);
	CHECK(n == 1 && out[0] == 0, "filter needs every word present: only doc 0 has both 'open' and 'log'");
	n = lt_search(LT_SEARCH_FILTER, "kernel log", DOCS, NDOC, out, 16);
	CHECK(n == 2 && contains(out, n, 0) && contains(out, n, 1), "filter matches both kernel-log docs");
	n = lt_search(LT_SEARCH_FILTER, "", DOCS, NDOC, out, 16);
	CHECK(n == NDOC, "an empty query matches everything");
	n = lt_search(LT_SEARCH_FILTER, "nonexistentword", DOCS, NDOC, out, 16);
	CHECK(n == 0, "a word nobody has matches nothing");

	/* fuzzy: subsequence match, ranked; an exact substring should still show up near the top */
	n = lt_search(LT_SEARCH_FUZZY, "opnlog", DOCS, NDOC, out, 16);
	CHECK(n >= 1 && (out[0] == 0 || out[0] == 1 || out[0] == 3), "fuzzy finds a subsequence across a plausible doc");
	n = lt_search(LT_SEARCH_FUZZY, "kernellog", DOCS, NDOC, out, 16);
	CHECK(n >= 1, "fuzzy tolerates missing spaces");

	/* tag: tag:x / #x requires the tag, -tag:x forbids it, the rest filters by substring */
	n = lt_search(LT_SEARCH_TAG, "tag:system", DOCS, NDOC, out, 16);
	CHECK(n == 3 && contains(out, n, 0) && contains(out, n, 1) && contains(out, n, 3), "tag:system matches every doc tagged system(-anything)");
	n = lt_search(LT_SEARCH_TAG, "#ui", DOCS, NDOC, out, 16);
	CHECK(n == 1 && out[0] == 2, "#ui is the same as tag:ui");
	n = lt_search(LT_SEARCH_TAG, "-tag:system", DOCS, NDOC, out, 16);
	CHECK(n == 2 && contains(out, n, 2) && contains(out, n, 4), "-tag:system excludes every system-tagged doc");
	n = lt_search(LT_SEARCH_TAG, "tag:system log", DOCS, NDOC, out, 16);
	CHECK(n == 2 && contains(out, n, 0) && contains(out, n, 1), "a tag filter combines with a text filter");

	/* regex: POSIX ERE, case-insensitive, and literal regex metacharacters in a document do not confuse it */
	n = lt_search(LT_SEARCH_REGEX, "^open", DOCS, NDOC, out, 16);
	CHECK(n == 2 && contains(out, n, 0) && contains(out, n, 2), "^open anchors to the start of the text");
	n = lt_search(LT_SEARCH_REGEX, "KERNEL", DOCS, NDOC, out, 16);
	CHECK(n == 2, "regex matching is case-insensitive");
	n = lt_search(LT_SEARCH_REGEX, "[[[bad", DOCS, NDOC, out, 16);
	CHECK(n >= 0, "an invalid regex is handled gracefully, not a crash");

	/* mcts: deterministic given the same input (no randomness leaking through), and returns a plausible ranking */
	int a[16], b[16];
	int na = lt_search(LT_SEARCH_MCTS, "log", DOCS, NDOC, a, 16);
	int nb = lt_search(LT_SEARCH_MCTS, "log", DOCS, NDOC, b, 16);
	CHECK(na == nb && !memcmp(a, b, (size_t)na * sizeof(int)), "mcts search is deterministic for the same query");
	CHECK(na > 0, "mcts returns at least one result for a query that appears in the data");

	/* lt_search_auto: one directive-driven query, no mode-selector needed - a leading "mode:" picks the mode (default fuzzy), and
	 * tag:/#/-tag:/-# words are pulled out and applied as a filter no matter which mode is active */
	lt_search_mode got;
	n = lt_search_auto("kernellog", DOCS, NDOC, out, 16, &got);
	CHECK(got == LT_SEARCH_FUZZY && n >= 1, "no directive at all defaults to fuzzy");
	n = lt_search_auto("filter:kernel log", DOCS, NDOC, out, 16, &got);
	CHECK(got == LT_SEARCH_FILTER && n == 2 && contains(out, n, 0) && contains(out, n, 1), "a leading 'filter:' switches to substring matching");
	n = lt_search_auto("FILTER:kernel log", DOCS, NDOC, out, 16, &got);
	CHECK(got == LT_SEARCH_FILTER && n == 2, "the directive is case-insensitive");
	n = lt_search_auto("regex:^open", DOCS, NDOC, out, 16, &got);
	CHECK(got == LT_SEARCH_REGEX && n == 2 && contains(out, n, 0) && contains(out, n, 2), "'regex:' switches to POSIX ERE matching");
	n = lt_search_auto("mcts:log", DOCS, NDOC, out, 16, &got);
	CHECK(got == LT_SEARCH_MCTS && n > 0, "'mcts:' switches to the tag-group search");
	n = lt_search_auto("tag:system kernel", DOCS, NDOC, out, 16, &got);
	CHECK(got == LT_SEARCH_FUZZY && n == 2 && contains(out, n, 0) && contains(out, n, 1), "a tag: word filters even with no mode directive (default fuzzy)");
	n = lt_search_auto("-tag:ui open", DOCS, NDOC, out, 16, &got);
	CHECK(n == 2 && contains(out, n, 0) && contains(out, n, 3), "-tag: excludes a tag while fuzzy-matching the rest");
	n = lt_search_auto("filter:tag:system log", DOCS, NDOC, out, 16, &got);
	CHECK(got == LT_SEARCH_FILTER && n == 2 && contains(out, n, 0) && contains(out, n, 1), "a tag: filter combines with an explicit mode directive too");
	n = lt_search_auto("#ui", DOCS, NDOC, out, 16, &got);
	CHECK(n == 1 && out[0] == 2, "#ui works the same as tag:ui with no directive");
	n = lt_search_auto("", DOCS, NDOC, out, 16, &got);
	CHECK(got == LT_SEARCH_FUZZY && n == NDOC, "an empty query still defaults to fuzzy and matches everything (fuzzy of nothing is everything)");
	n = lt_search_auto(NULL, DOCS, NDOC, out, 16, NULL);
	CHECK(n == NDOC, "a NULL query and a NULL mode_out are both handled");

	/* mode names exist and are distinct */
	const char *names[LT_SEARCH_COUNT];
	bool dup = false;
	for (int i = 0; i < LT_SEARCH_COUNT; i++) {
		names[i] = lt_search_mode_name((lt_search_mode)i);
		CHECK(names[i] && *names[i], "every mode has a non-empty name");
		for (int j = 0; j < i; j++) if (!strcmp(names[i], names[j])) dup = true;
	}
	CHECK(!dup, "mode names are distinct");

	/* an over-small output cap truncates instead of overflowing */
	int small[1];
	n = lt_search(LT_SEARCH_FILTER, "", DOCS, NDOC, small, 1);
	CHECK(n == 1, "the result count is capped to the buffer size");

	/* lt_list_set_order: a list shows exactly the given items, in that order */
	lt_list *l = lt_list_new();
	for (int i = 0; i < NDOC; i++) lt_list_add(l, DOCS[i].text, i);
	int order[3] = {4, 1, 0};
	lt_list_set_order(l, order, 3);
	CHECK(lt_list_shown(l) == 3, "the list shows only the ordered subset");
	lt_caps caps = {true, true, false, true, 256};
	lt_canvas *lc = lt_canvas_new(30, 3, caps);
	lt_theme lth = lt_theme_default();
	lt_list_draw(l, lc, (lt_rect){0, 0, 30, 3}, &lth, true);
	char *ld = lt_canvas_dump(lc);
	char *p4 = strstr(ld, DOCS[4].text), *p1 = strstr(ld, DOCS[1].text), *p0 = strstr(ld, DOCS[0].text);
	CHECK(p4 && p1 && p0 && p4 < p1 && p1 < p0, "the ordered subset is drawn in the requested order");
	free(ld); lt_canvas_free(lc);
	lt_list_reset_order(l);
	CHECK(lt_list_shown(l) == NDOC, "resetting the order shows everything again");
	lt_list_free(l);

	printf(fails ? "search: %d failed\n" : "search: ok\n", fails);
	return fails != 0;
}
