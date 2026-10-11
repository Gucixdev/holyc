/* search.c - the five ways to search: substring filter, fuzzy, tags, regular expression, and a Monte-Carlo tree search over tag groups. */
#include "lazytui.h"
#include <ctype.h>
#include <math.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

const char *lt_search_mode_name(lt_search_mode m)
{
	static const char *n[LT_SEARCH_COUNT] = {"filter", "fuzzy", "tag", "regex", "mcts"};
	return m >= 0 && m < LT_SEARCH_COUNT ? n[m] : "?";
}

static int lower(int c) { return tolower((unsigned char)c); }

static bool has_sub(const char *hay, const char *needle, size_t nl)
{
	if (nl == 0) return true;
	for (const char *p = hay; *p; p++) {
		size_t i = 0;
		while (i < nl && p[i] && lower(p[i]) == lower(needle[i])) i++;
		if (i == nl) return true;
	}
	return false;
}

/* the words of a query, each a (pointer, length) into a copy */
typedef struct { char *buf; const char *w[24]; size_t len[24]; int n; } words;
static void split(words *W, const char *q)
{
	W->buf = strdup(q ? q : "");
	W->n = 0;
	for (char *p = W->buf; *p && W->n < 24;) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p) break;
		char *s = p;
		while (*p && *p != ' ' && *p != '\t') p++;
		W->w[W->n] = s; W->len[W->n] = (size_t)(p - s); W->n++;
	}
}

/* fuzzy score of one word against text: -1 = no match. Runs, word starts and an early first hit score; gaps cost. */
static int fuzzy_word(const char *text, const char *w, size_t wl)
{
	int score = 0, run = 0, last = -2, first = -1;
	size_t k = 0;
	for (int i = 0; text[i] && k < wl; i++) {
		if (lower(text[i]) != lower(w[k])) continue;
		if (first < 0) first = i;
		int s = 10;
		if (i == last + 1) { run++; s += 8 + run * 2; } else { run = 0; if (last >= 0) s -= (i - last > 6 ? 6 : i - last - 1); }
		if (i == 0 || !isalnum((unsigned char)text[i - 1])) s += 9;
		score += s; last = i; k++;
	}
	if (k < wl) return -1;
	return score + (first >= 0 && first < 8 ? 8 - first : 0);
}

static int fuzzy_score(const char *text, const words *W)
{
	int total = 0;
	for (int i = 0; i < W->n; i++) { int s = fuzzy_word(text, W->w[i], W->len[i]); if (s < 0) return -1; total += s; }
	return total;
}

static bool has_tag(const char *tags, const char *t, size_t tl)
{
	if (!tags) return false;
	for (const char *p = tags; *p;) {
		while (*p == ' ' || *p == ',' || *p == '\t') p++;
		const char *s = p;
		while (*p && *p != ' ' && *p != ',' && *p != '\t') p++;
		size_t l = (size_t)(p - s);
		if (l >= tl && !strncasecmp(s, t, tl) && l > 0) return true;                 /* a tag matches by prefix: tag:net finds "network" */
	}
	return false;
}

typedef struct { int idx, score; } hit;
static int cmp_hit(const void *a, const void *b)
{
	const hit *x = a, *y = b;
	if (x->score != y->score) return y->score - x->score;
	return x->idx - y->idx;
}

static int emit(hit *h, int nh, int *out, int cap)
{
	int n = nh < cap ? nh : cap;
	for (int i = 0; i < n; i++) out[i] = h[i].idx;
	return n;
}

/* ------------------------------------------------------------------ MCTS over tag groups */
typedef struct { int doc; double sum; int visits; } leaf;
typedef struct { int first, count; double sum; int visits; } group;

static uint32_t rng_next(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static int mcts(const char *query, const lt_doc *docs, int n, int *out, int cap)
{
	words W;
	split(&W, query);
	/* level 1: groups by first tag ("" = untagged); level 2: the documents of the group */
	char **gname = malloc((size_t)(n + 1) * sizeof *gname);
	int *gof = malloc((size_t)(n + 1) * sizeof(int)), ng = 0;
	for (int i = 0; i < n; i++) {
		char nm[48] = "";
		const char *t = docs[i].tags;
		if (t) { while (*t == ' ' || *t == ',') t++; size_t l = strcspn(t, " ,"); if (l > 47) l = 47; memcpy(nm, t, l); nm[l] = 0; }
		int g = -1;
		for (int k = 0; k < ng; k++) if (!strcmp(gname[k], nm)) { g = k; break; }
		if (g < 0) { g = ng++; gname[g] = strdup(nm); }
		gof[i] = g;
	}
	int *order = malloc((size_t)(n + 1) * sizeof(int)), *gstart = calloc((size_t)(ng + 2), sizeof(int));
	for (int i = 0; i < n; i++) gstart[gof[i] + 1]++;
	for (int g = 0; g < ng; g++) gstart[g + 1] += gstart[g];
	int *fill = calloc((size_t)(ng + 1), sizeof(int));
	for (int i = 0; i < n; i++) order[gstart[gof[i]] + fill[gof[i]]++] = i;
	leaf *lv = calloc((size_t)(n + 1), sizeof *lv);
	group *gr = calloc((size_t)(ng + 1), sizeof *gr);
	for (int g = 0; g < ng; g++) { gr[g].first = gstart[g]; gr[g].count = gstart[g + 1] - gstart[g]; }
	for (int i = 0; i < n; i++) lv[i].doc = order[i];
	uint32_t seed = 12345;
	int rollouts = 30 + 6 * n;
	if (rollouts > 4000) rollouts = 4000;
	int total = 0;
	for (int it = 0; it < rollouts; it++, total++) {
		int g = 0;
		double best = -1;
		for (int k = 0; k < ng; k++) {                                                   /* UCB1 over the groups, then over the leaves */
			double u = gr[k].visits == 0 ? 1e9 : gr[k].sum / gr[k].visits + 1.2 * sqrt(log(total + 1.0) / gr[k].visits);
			if (u > best) { best = u; g = k; }
		}
		int li = gr[g].first;
		best = -1;
		for (int k = 0; k < gr[g].count; k++) {
			leaf *L = &lv[gr[g].first + k];
			double u = L->visits == 0 ? 1e9 + (rng_next(&seed) % 100) : L->sum / L->visits + 1.2 * sqrt(log(gr[g].visits + 1.0) / L->visits);
			if (u > best) { best = u; li = gr[g].first + k; }
		}
		int sc = fuzzy_score(docs[lv[li].doc].text, &W);
		double reward = sc < 0 ? 0.0 : fmin(1.0, 0.25 + sc / 120.0);
		reward += ((double)(rng_next(&seed) % 1000) / 1000.0 - 0.5) * 0.02;                 /* a little noise: the rollout is stochastic */
		lv[li].sum += reward; lv[li].visits++; gr[g].sum += reward; gr[g].visits++;
	}
	hit *h = malloc((size_t)(n + 1) * sizeof *h);
	int nh = 0;
	for (int i = 0; i < n; i++) {
		if (lv[i].visits == 0 || fuzzy_score(docs[lv[i].doc].text, &W) < 0) continue;      /* only documents that really match are reported */
		h[nh++] = (hit){lv[i].doc, lv[i].visits * 1000 + (int)(lv[i].sum / lv[i].visits * 100)};
	}
	qsort(h, (size_t)nh, sizeof *h, cmp_hit);
	int r = emit(h, nh, out, cap);
	free(h); free(lv); free(gr); free(fill); free(order); free(gstart); free(gof);
	for (int k = 0; k < ng; k++) free(gname[k]);
	free(gname); free(W.buf);
	return r;
}

/* ------------------------------------------------------------------ lt_search_auto: one directive-driven query, no mode selector needed */

/* a leading "filter:"/"fuzzy:"/"regex:"/"mcts:" word at the very start of the query (case-insensitive) picks the mode; anything else
 * (including no directive at all) defaults to fuzzy, and `rest` is left pointing at the query text after the directive. */
static lt_search_mode parse_mode_prefix(const char *q, const char **rest)
{
	static const char *const names[] = {"filter:", "fuzzy:", "regex:", "mcts:"};
	static const lt_search_mode modes[] = {LT_SEARCH_FILTER, LT_SEARCH_FUZZY, LT_SEARCH_REGEX, LT_SEARCH_MCTS};
	while (*q == ' ' || *q == '\t') q++;
	for (int i = 0; i < 4; i++) {
		size_t l = strlen(names[i]);
		if (!strncasecmp(q, names[i], l)) { *rest = q + l; return modes[i]; }
	}
	*rest = q;
	return LT_SEARCH_FUZZY;
}

int lt_search_auto(const char *query, const lt_doc *docs, int n, int *out, int cap, lt_search_mode *mode_out)
{
	if (n <= 0 || cap <= 0 || !out) return 0;
	if (!query) query = "";
	const char *rest;
	lt_search_mode mode = parse_mode_prefix(query, &rest);
	if (mode_out) *mode_out = mode;

	/* tag:x / -tag:x / #x / -#x words are pulled out of the query wherever they are (not just for LT_SEARCH_TAG - a tag filter narrows
	 * the results no matter which mode ranks the rest of the text), leaving the remaining words as the content query for that mode. */
	words W;
	split(&W, rest);
	typedef struct { const char *tag; size_t len; bool neg; } tagf;
	tagf tf[24];
	int ntf = 0;
	char content[512] = "";
	size_t cl = 0;
	for (int i = 0; i < W.n; i++) {
		const char *w = W.w[i];
		size_t l = W.len[i];
		bool neg = l > 1 && w[0] == '-' && (w[1] == '#' || !strncmp(w + 1, "tag:", 4));
		const char *ww = neg ? w + 1 : w;
		size_t wl = neg ? l - 1 : l;
		const char *tag = NULL;
		size_t tl = 0;
		if (wl > 1 && ww[0] == '#') { tag = ww + 1; tl = wl - 1; }
		else if (wl > 4 && !strncmp(ww, "tag:", 4)) { tag = ww + 4; tl = wl - 4; }
		if (tag) { if (ntf < 24) tf[ntf++] = (tagf){tag, tl, neg}; continue; }
		if (cl && cl + 1 < sizeof content) content[cl++] = ' ';
		size_t take = l;
		if (cl + take >= sizeof content) take = sizeof content - cl - 1;
		memcpy(content + cl, w, take); cl += take; content[cl] = 0;
	}
	/* tf[]'s tag pointers point into W.buf, so it stays alive until after the tag-filter pass below uses them */

	int *tmp = malloc((size_t)n * sizeof *tmp);
	int nt = lt_search(mode, content, docs, n, tmp, n);
	int r = 0;
	for (int i = 0; i < nt && r < cap; i++) {
		int idx = tmp[i];
		bool ok = true;
		for (int k = 0; k < ntf && ok; k++) { bool has = has_tag(docs[idx].tags, tf[k].tag, tf[k].len); ok = tf[k].neg ? !has : has; }
		if (ok) out[r++] = idx;
	}
	free(tmp);
	free(W.buf);
	return r;
}

/* ------------------------------------------------------------------ */
int lt_search(lt_search_mode m, const char *query, const lt_doc *docs, int n, int *out, int cap)
{
	if (n <= 0 || cap <= 0 || !out) return 0;
	if (!query) query = "";
	hit *h = malloc((size_t)n * sizeof *h);
	int nh = 0;
	if (m == LT_SEARCH_MCTS) { free(h); return mcts(query, docs, n, out, cap); }
	if (m == LT_SEARCH_REGEX) {
		regex_t re;
		if (!*query) { for (int i = 0; i < n; i++) h[nh++] = (hit){i, 0}; }
		else if (regcomp(&re, query, REG_EXTENDED | REG_ICASE | REG_NOSUB) == 0) {
			for (int i = 0; i < n; i++) if (regexec(&re, docs[i].text, 0, NULL, 0) == 0) h[nh++] = (hit){i, 0};
			regfree(&re);
		}
	} else {
		words W;
		split(&W, query);
		for (int i = 0; i < n; i++) {
			int score = 0;
			bool ok = true;
			if (m == LT_SEARCH_FUZZY) { score = W.n ? fuzzy_score(docs[i].text, &W) : 0; ok = score >= 0; score -= (int)strlen(docs[i].text) / 8; }
			else {
				for (int k = 0; k < W.n && ok; k++) {
					const char *w = W.w[k];
					size_t l = W.len[k];
					bool neg = m == LT_SEARCH_TAG && w[0] == '-' && l > 1 && (w[1] == '#' || !strncmp(w + 1, "tag:", 4));
					if (neg) { w++; l--; }
					size_t tl = 0;
					const char *tag = NULL;
					if (m == LT_SEARCH_TAG && l > 1 && w[0] == '#') { tag = w + 1; tl = l - 1; }
					else if (m == LT_SEARCH_TAG && l > 4 && !strncmp(w, "tag:", 4)) { tag = w + 4; tl = l - 4; }
					if (tag) { bool has = has_tag(docs[i].tags, tag, tl); ok = neg ? !has : has; }
					else ok = has_sub(docs[i].text, w, l);
				}
			}
			if (ok) h[nh++] = (hit){i, score};
		}
		free(W.buf);
	}
	if (m == LT_SEARCH_FUZZY) qsort(h, (size_t)nh, sizeof *h, cmp_hit);
	int r = emit(h, nh, out, cap);
	free(h);
	return r;
}
