/* pattern.c - items, field selection (--nth / --with-nth / --delimiter), the extended-search query language,
 * ranking (score, tiebreaks) and the multi-threaded search driver. Semantics follow fzf's documented behaviour. */
#include "fzf.h"
#include "opts.h"
#include <pthread.h>

/* ------------------------------------------------------------------ ranges and delimiters */
bool range_parse(const char *s, Range *r)
{
	size_t n = strlen(s);
	char *e;
	long a, b;
	if (!strcmp(s, "..")) { *r = (Range){0, 0}; return true; }
	if (n > 2 && s[0] == '.' && s[1] == '.') {
		b = strtol(s + 2, &e, 10);
		if (*e || b == 0) return false;
		*r = (Range){0, b == -1 ? 0 : (int)b};
		return true;
	}
	if (n > 2 && s[n - 2] == '.' && s[n - 1] == '.') {
		char *t = xstrndup(s, n - 2);
		a = strtol(t, &e, 10);
		bool bad = *e || a == 0;
		free(t);
		if (bad) return false;
		*r = (Range){(int)a, 0};
		if (a == 1) r->begin = 0;
		return true;
	}
	const char *dd = strstr(s, "..");
	if (dd) {
		char *t = xstrndup(s, (size_t)(dd - s));
		a = strtol(t, &e, 10);
		bool bad = *e || a == 0;
		free(t);
		b = strtol(dd + 2, &e, 10);
		if (bad || *e || b == 0 || dd[2] == 0 || (a < 0 && b > 0)) return false;
		*r = (Range){(int)a, (int)b};
		if (r->begin == 1 && r->end != 1) r->begin = 0;
		if (r->end == -1) r->end = 0;
		return true;
	}
	a = strtol(s, &e, 10);
	if (*e || a == 0 || n == 0) return false;
	*r = (Range){(int)a, (int)a};
	if (a == 1) { r->begin = 1; r->end = 1; }
	if (a == -1) { r->begin = -1; r->end = 0; r->end = -1; }
	return true;
}

int ranges_parse(const char *spec, Range **out)
{
	int n = 0, cap = 4;
	Range *v = xmalloc((size_t)cap * sizeof(Range));
	char *copy = xstrdup(spec), *save = NULL;
	for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
		if (n == cap) { cap *= 2; v = xrealloc(v, (size_t)cap * sizeof(Range)); }
		if (!range_parse(t, &v[n])) { free(copy); free(v); return -1; }
		n++;
	}
	free(copy);
	*out = v;
	return n;
}

void delim_set(Delim *d, const char *s0)
{
	Buf b;
	buf_init(&b);
	for (const char *p = s0; *p; p++) {                       /* \t as a tab */
		if (p[0] == '\\' && p[1] == 't') { buf_addc(&b, '\t'); p++; }
		else buf_addc(&b, *p);
	}
	char *s = buf_take(&b);
	memset(d, 0, sizeof *d);
	bool special = strpbrk(s, "\\.+*?()|[]{}^$") != NULL;
	if (utf8_len(s, strlen(s)) == 1 || !special) { d->str = s; d->len = strlen(s); return; }
	if (regcomp(&d->re, s, REG_EXTENDED) == 0) { d->has_re = true; free(s); return; }
	d->str = s; d->len = strlen(s);
}

/* ------------------------------------------------------------------ tokens (rune ranges of an item) */
typedef struct { int start, end; } Tok;

static int byte_to_rune(const char *s, size_t byte_off)
{
	return (int)utf8_len(s, byte_off);
}

/* text as bytes s[0..n) and runes r[0..nr): tokens partition the text, each token keeps its trailing delimiter */
static int tokenize(const char *s, size_t n, const rune *r, int nr, const Delim *d, Tok **out)
{
	int cap = 8, nt = 0;
	Tok *t = xmalloc((size_t)cap * sizeof(Tok));
#define PUSH(a, b) do { if (nt == cap) { cap *= 2; t = xrealloc(t, (size_t)cap * sizeof(Tok)); } t[nt].start = (a); t[nt].end = (b); nt++; } while (0)
	if (!d->str && !d->has_re) {                                    /* awk: runs of blanks separate fields, kept as a suffix */
		int state = 0, begin = 0, end = 0;
		for (int i = 0; i < nr; i++) {
			bool white = r[i] == 9 || r[i] == 32 || r[i] == 10;
			if (state == 0) { if (!white) { state = 1; begin = i; end = i + 1; } }
			else if (state == 1) { end = i + 1; if (white) state = 2; }
			else if (white) end = i + 1;
			else { PUSH(begin, end); state = 1; begin = i; end = i + 1; }
		}
		if (begin < end) PUSH(begin, end);
	} else if (d->str) {
		size_t pos = 0, tokstart = 0;
		while (pos + d->len <= n && d->len) {
			if (!memcmp(s + pos, d->str, d->len)) { PUSH(byte_to_rune(s, tokstart), byte_to_rune(s, pos + d->len)); pos += d->len; tokstart = pos; }
			else pos++;
		}
		PUSH(byte_to_rune(s, tokstart), nr);
	} else {
		char *tmp = xstrndup(s, n);
		regmatch_t m;
		size_t off = 0, begin = 0;
		while (off <= n && regexec(&d->re, tmp + off, 1, &m, off ? REG_NOTBOL : 0) == 0) {
			size_t me = off + (size_t)m.rm_eo;
			if (m.rm_eo == m.rm_so) { off++; continue; }
			PUSH(byte_to_rune(s, begin), byte_to_rune(s, me));
			begin = off = me;
		}
		if (begin < n) PUSH(byte_to_rune(s, begin), nr);
		free(tmp);
	}
#undef PUSH
	*out = t;
	return nt;
}

/* Transform: one merged token per range */
static int transform(const Tok *tk, int ntk, const Range *rg, int nrg, Tok *out)
{
	for (int i = 0; i < nrg; i++) {
		Range r = rg[i];
		int b, e;
		if (r.begin == r.end && r.begin != 0) {
			int k = r.begin < 0 ? r.begin + ntk + 1 : r.begin;
			if (k >= 1 && k <= ntk) { out[i] = tk[k - 1]; continue; }
			out[i] = (Tok){0, 0};
			continue;
		}
		if (r.begin == 0 && r.end == 0) {
			out[i] = ntk ? (Tok){tk[0].start, tk[ntk - 1].end} : (Tok){0, 0};
			continue;
		}
		if (r.begin == 0) { b = 1; e = r.end < 0 ? r.end + ntk + 1 : r.end; }
		else if (r.end == 0) { b = r.begin < 0 ? r.begin + ntk + 1 : r.begin; e = ntk; }
		else { b = r.begin < 0 ? r.begin + ntk + 1 : r.begin; e = r.end < 0 ? r.end + ntk + 1 : r.end; }
		int first = -1, last = -1;
		for (int k = b; k <= e; k++)
			if (k >= 1 && k <= ntk) { if (first < 0) first = k - 1; last = k - 1; }
		out[i] = first < 0 ? (Tok){0, 0} : (Tok){tk[first].start, tk[last].end};
	}
	return nrg;
}

/* ------------------------------------------------------------------ items */
Item *item_new(const Opts *o, const char *line, size_t len)
{
	Item *it = calloc(1, sizeof *it);
	if (!it) die("out of memory");
	it->orig = xstrndup(line, len);
	it->olen = (uint32_t)len;
	Buf plain;
	buf_init(&plain);
	const char *src = line;
	size_t srclen = len;
	Buf joined;
	buf_init(&joined);
	if (o->with_nth && o->nwith > 0) {                             /* the shown text is made of the chosen fields */
		rune *rb = xmalloc((len + 1) * sizeof(rune));
		int nr = (int)utf8_to_runes(line, len, rb);
		Tok *tk;
		int ntk = tokenize(line, len, rb, nr, &o->delim, &tk);
		Tok *tr = xmalloc((size_t)o->nwith * sizeof(Tok));
		transform(tk, ntk, o->with_nth, o->nwith, tr);
		for (int i = 0; i < o->nwith; i++)
			for (int k = tr[i].start; k < tr[i].end; k++) { char e[4]; buf_addn(&joined, e, u8enc(rb[k], e)); }
		free(tr); free(tk); free(rb);
		src = joined.p ? joined.p : ""; srclen = joined.len;
	}
	uint32_t cap = 0;
	if (o->ansi) ansi_extract(src, srclen, &plain, &it->ansi, &it->nansi, &cap);
	else buf_addn(&plain, src, srclen);
	if (o->with_nth && o->nwith > 0) {                             /* trailing blanks are not part of a transformed line */
		while (plain.len && (plain.p[plain.len - 1] == ' ' || plain.p[plain.len - 1] == '\t')) plain.len--;
		if (plain.p) plain.p[plain.len] = 0;
	}
	it->tlen = (uint32_t)plain.len;
	it->text = buf_take(&plain);
	buf_free(&joined);
	it->nrunes = (uint32_t)utf8_len(it->text, it->tlen);
	/* trimmed length in runes */
	rune *rb = xmalloc((it->tlen + 1) * sizeof(rune));
	int n = (int)utf8_to_runes(it->text, it->tlen, rb);
	int a = 0, b = n;
	while (a < b && rune_is_space(rb[a])) a++;
	while (b > a && rune_is_space(rb[b - 1])) b--;
	it->trim = (uint16_t)(b - a > 65535 ? 65535 : b - a);
	free(rb);
	return it;
}
void item_free(Item *it)
{
	if (!it) return;
	free(it->orig); free(it->text); free(it->ansi); free(it->id); free(it);
}

/* ------------------------------------------------------------------ query -> pattern */
enum { T_FUZZY, T_EXACT, T_BOUNDARY, T_PREFIX, T_SUFFIX, T_EQUAL };
typedef struct { int typ; bool inv, cs, norm; rune *t; int n; } Term;
typedef struct { Term *v; int n; } TermSet;

struct Pattern {
	bool extended, fuzzy, cs, norm, fwd, sortable, v1;
	rune *text; int ntext;
	TermSet *sets; int nsets;
	const Opts *o;
	Crit crit[5]; int ncrit;
	Tok *nth_dummy;
};

static bool has_upper(const rune *r, int n) { for (int i = 0; i < n; i++) if (rune_lower(r[i]) != r[i]) return true; return false; }

static void set_push(TermSet *s, Term t) { s->v = xrealloc(s->v, (size_t)(s->n + 1) * sizeof(Term)); s->v[s->n++] = t; }

static rune *dup_runes(const char *s, size_t n, int *out)
{
	rune *r = xmalloc((n + 1) * sizeof(rune));
	*out = (int)utf8_to_runes(s, n, r);
	return r;
}

/* splits on runs of blanks, "\ " keeps a blank inside a term */
static char **split_terms(const char *q, int *nt)
{
	int cap = 8;
	char **v = xmalloc((size_t)cap * sizeof(char *));
	*nt = 0;
	Buf cur;
	buf_init(&cur);
	bool have = false;
	for (const char *p = q; ; p++) {
		if (*p == '\\' && p[1] == ' ') { buf_addc(&cur, '\t'); have = true; p++; continue; }
		if (*p == ' ' || *p == 0) {
			if (have) {
				if (*nt == cap) { cap *= 2; v = xrealloc(v, (size_t)cap * sizeof(char *)); }
				for (size_t i = 0; i < cur.len; i++) if (cur.p[i] == '\t') cur.p[i] = ' ';
				v[(*nt)++] = buf_take(&cur);
				have = false;
			}
			if (!*p) break;
		} else { buf_addc(&cur, *p); have = true; }
	}
	buf_free(&cur);
	return v;
}

Pattern *pattern_new(const Opts *o, const char *query)
{
	Pattern *p = calloc(1, sizeof *p);
	if (!p) die("out of memory");
	p->o = o;
	p->extended = o->extended;
	p->fuzzy = o->fuzzy;
	p->fwd = o->forward;
	p->v1 = o->algo_v1;
	p->norm = o->normalize;
	memcpy(p->crit, o->crit, sizeof p->crit);
	p->ncrit = o->ncrit;
	char *q = xstrdup(query);
	if (!p->extended) {
		char *lower = q;
		int n;
		rune *rn = dup_runes(q, strlen(q), &n);
		bool up = has_upper(rn, n);
		p->cs = o->case_mode == CASE_RESPECT || (o->case_mode == CASE_SMART && up);
		if (!p->cs) for (int i = 0; i < n; i++) rn[i] = rune_lower(rn[i]);
		if (p->norm) { rune *cp = xmalloc((size_t)(n + 1) * sizeof(rune)); memcpy(cp, rn, (size_t)n * sizeof(rune)); normalize_runes(cp, n); if (memcmp(cp, rn, (size_t)n * sizeof(rune))) p->norm = false; free(cp); }
		if (p->norm) normalize_runes(rn, n);
		p->text = rn; p->ntext = n;
		p->sortable = true;
		(void)lower;
		free(q);
		return p;
	}
	/* trim the query: leading blanks and trailing ones (unless escaped) do not count */
	char *s = q;
	while (*s == ' ') s++;
	size_t ln = strlen(s);
	while (ln > 0 && s[ln - 1] == ' ' && !(ln > 1 && s[ln - 2] == '\\')) ln--;
	s[ln] = 0;
	int nt;
	char **tok = split_terms(s, &nt);
	TermSet set = {NULL, 0};
	bool switch_set = false, after_bar = false;
	p->sortable = false;
	for (int i = 0; i < nt; i++) {
		char *t = tok[i];
		int typ = p->fuzzy ? T_FUZZY : T_EXACT;
		bool inv = false;
		int n0;
		rune *r0 = dup_runes(t, strlen(t), &n0);
		bool up = has_upper(r0, n0);
		free(r0);
		bool cs = o->case_mode == CASE_RESPECT || (o->case_mode == CASE_SMART && up);
		if (!cs) { for (char *c = t; *c; c++) if ((unsigned char)*c < 128) *c = (char)tolower((unsigned char)*c); }
		char *text = t;
		if (!cs) {   /* non-ASCII lowercase through runes */
			int n1; rune *r1 = dup_runes(t, strlen(t), &n1);
			for (int k = 0; k < n1; k++) r1[k] = rune_lower(r1[k]);
			Buf b; buf_init(&b);
			for (int k = 0; k < n1; k++) { char e[4]; buf_addn(&b, e, u8enc(r1[k], e)); }
			free(r1); free(t); t = tok[i] = buf_take(&b); text = t;
		}
		if (set.n > 0 && !after_bar && !strcmp(text, "|")) { switch_set = false; after_bar = true; continue; }
		after_bar = false;
		if (text[0] == '!') { inv = true; typ = T_EXACT; text++; }
		size_t tl = strlen(text);
		char *work = xstrndup(text, tl);
		char *w = work;
		size_t wl = tl;
		if (strcmp(w, "$") && wl > 0 && w[wl - 1] == '$') { typ = T_SUFFIX; w[--wl] = 0; }
		if (wl > 2 && w[0] == '\'' && w[wl - 1] == '\'') { typ = T_BOUNDARY; w[wl - 1] = 0; w++; wl -= 2; }
		else if (w[0] == '\'') { typ = (p->fuzzy && !inv) ? T_EXACT : T_FUZZY; w++; wl--; }
		else if (w[0] == '^') { typ = typ == T_SUFFIX ? T_EQUAL : T_PREFIX; w++; wl--; }
		if (wl > 0) {
			if (switch_set) { p->sets = xrealloc(p->sets, (size_t)(p->nsets + 1) * sizeof(TermSet)); p->sets[p->nsets++] = set; set = (TermSet){NULL, 0}; }
			Term tm;
			tm.typ = typ; tm.inv = inv; tm.cs = cs;
			tm.t = dup_runes(w, wl, &tm.n);
			tm.norm = false;
			if (o->normalize) {
				rune *cp = xmalloc((size_t)(tm.n + 1) * sizeof(rune)); memcpy(cp, tm.t, (size_t)tm.n * sizeof(rune)); normalize_runes(cp, tm.n);
				tm.norm = memcmp(cp, tm.t, (size_t)tm.n * sizeof(rune)) == 0;   /* a term with accents asks for the accents */
				free(cp);
				if (tm.norm) normalize_runes(tm.t, tm.n);
			}
			set_push(&set, tm);
			switch_set = true;
			if (!inv) p->sortable = true;
		}
		free(work);
	}
	if (set.n > 0) { p->sets = xrealloc(p->sets, (size_t)(p->nsets + 1) * sizeof(TermSet)); p->sets[p->nsets++] = set; }
	for (int i = 0; i < nt; i++) free(tok[i]);
	free(tok);
	free(q);
	return p;
}

void pattern_free(Pattern *p)
{
	if (!p) return;
	for (int i = 0; i < p->nsets; i++) { for (int j = 0; j < p->sets[i].n; j++) free(p->sets[i].v[j].t); free(p->sets[i].v); }
	free(p->sets); free(p->text); free(p);
}
bool pattern_empty(const Pattern *p) { return p->extended ? p->nsets == 0 : p->ntext == 0; }
bool pattern_sortable(const Pattern *p) { return p->sortable; }

/* ------------------------------------------------------------------ matching one item */
static AlgoFn fn_of(const Pattern *p, int typ)
{
	switch (typ) {
	case T_FUZZY: return p->v1 ? fuzzy_v1 : fuzzy_v2;
	case T_EXACT: return exact_naive;
	case T_BOUNDARY: return exact_boundary;
	case T_PREFIX: return prefix_match;
	case T_SUFFIX: return suffix_match;
	default: return equal_match;
	}
}

static uint16_t clamp16(int v) { return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v); }

/* the fields to match on: the whole text, or the --nth ranges of it (last field without its trailing delimiter) */
static int input_tokens(const Pattern *p, Item *it, Runes t, Tok *out, int maxout)
{
	const Opts *o = p->o;
	if (o->nnth == 0 || o->nth == NULL) { out[0] = (Tok){0, t.n}; return 1; }
	Tok *tk;
	int ntk = tokenize(it->text, it->tlen, t.r, t.n, &o->delim, &tk);
	int n = o->nnth < maxout ? o->nnth : maxout;
	transform(tk, ntk, o->nth, n, out);
	free(tk);
	if (n > 0 && (o->delim.str || o->delim.has_re)) {                  /* strip the last delimiter to allow suffix matches */
		Tok *last = &out[n - 1];
		if (o->delim.str) {
			size_t dl = utf8_len(o->delim.str, o->delim.len);
			if (last->end - last->start >= (int)dl) {
				bool same = true;
				rune *dr = xmalloc((o->delim.len + 1) * sizeof(rune));
				utf8_to_runes(o->delim.str, o->delim.len, dr);
				for (size_t k = 0; k < dl; k++) if (t.r[last->end - (int)dl + (int)k] != dr[k]) same = false;
				free(dr);
				if (same) last->end -= (int)dl;
			}
		}
	}
	return n;
}

static bool iter_tokens(AlgoFn fn, bool cs, bool norm, bool fwd, Runes t, const Tok *tk, int ntk, const rune *pat, int m,
                        bool want_pos, Scratch *sc, int *bs, int *be, int *score, int *pos, int *npos)
{
	for (int i = 0; i < ntk; i++) {
		Runes sub = {t.r + tk[i].start, tk[i].end - tk[i].start};
		int np = 0;
		MRes r = fn(cs, norm, fwd, sub, pat, m, want_pos ? pos : NULL, &np, sc);
		if (r.start >= 0) {
			*bs = r.start + tk[i].start; *be = r.end + tk[i].start; *score = r.score;
			if (want_pos) {
				if (np == 0) { for (int k = *bs; k < *be; k++) pos[np++] = k; }
				else for (int k = 0; k < np; k++) pos[k] += tk[i].start;
			}
			*npos = np;
			return true;
		}
	}
	return false;
}

bool pattern_match(const Pattern *p, Item *it, Scratch *sc, bool want_pos, MatchOut *out)
{
	rune *rb = scratch_rbuf(sc, it->tlen + 1);
	int nr = (int)utf8_to_runes(it->text, it->tlen, rb);
	Runes t = {rb, nr};
	Tok tk[64];
	int ntk = input_tokens(p, it, t, tk, 64);
	int score = 0, minb = 65535, mine = 65535, maxe = 0;
	bool valid = false;
	int npos_total = 0;
	int *posbuf = want_pos ? scratch_ibuf(sc, 0, (size_t)nr + 8) : NULL;
	int *allpos = want_pos ? scratch_ibuf(sc, 1, ((size_t)nr + 8) * 2 + 16) : NULL;
	bool ok = true;
	if (!p->extended) {
		int bs, be, sc1, np = 0;
		AlgoFn fn = p->fuzzy ? fn_of(p, T_FUZZY) : exact_naive;
		if (!iter_tokens(fn, p->cs, p->norm, p->fwd, t, tk, ntk, p->text, p->ntext, want_pos, sc, &bs, &be, &sc1, posbuf, &np)) ok = false;
		else {
			score = sc1;
			if (bs < be) { minb = bs; mine = be; maxe = be; valid = true; }
			if (want_pos) for (int k = 0; k < np; k++) allpos[npos_total++] = posbuf[k];
		}
	} else {
		int matched_sets = 0;
		int *offs = xmalloc((size_t)(p->nsets + 1) * 2 * sizeof(int));
		for (int si = 0; si < p->nsets && ok; si++) {
			bool matched = false;
			int cur_score = 0, os = 0, oe = 0;
			const TermSet *ts = &p->sets[si];
			for (int ti = 0; ti < ts->n; ti++) {
				const Term *tm = &ts->v[ti];
				int bs, be, sc1, np = 0;
				bool hit = iter_tokens(fn_of(p, tm->typ), tm->cs, tm->norm, p->fwd, t, tk, ntk, tm->t, tm->n, want_pos, sc, &bs, &be, &sc1, posbuf, &np);
				if (hit) {
					if (tm->inv) continue;
					os = bs; oe = be; cur_score = sc1; matched = true;
					if (want_pos) for (int k = 0; k < np; k++) allpos[npos_total++] = posbuf[k];
					break;
				} else if (tm->inv) { os = oe = 0; cur_score = 0; matched = true; continue; }
			}
			if (matched) { offs[2 * matched_sets] = os; offs[2 * matched_sets + 1] = oe; matched_sets++; score += cur_score; }
			else ok = false;
		}
		if (ok && matched_sets == p->nsets) {
			for (int i = 0; i < matched_sets; i++) {
				int b = offs[2 * i], e = offs[2 * i + 1];
				if (b < e) { if (b < minb) minb = b; if (e < mine) mine = e; if (e > maxe) maxe = e; valid = true; }
			}
		} else ok = false;
		free(offs);
	}
	if (!ok) return false;

	/* rank: criterion i -> pts[3 - i], smaller is better */
	memset(out->pts, 0, sizeof out->pts);
	for (int idx = 0; idx < p->ncrit; idx++) {
		uint16_t val = 65535;
		switch (p->crit[idx]) {
		case CR_SCORE: val = (uint16_t)(65535 - clamp16(score)); break;
		case CR_LENGTH: val = it->trim; break;
		case CR_CHUNK:
			if (valid) {
				int b = minb, e = maxe;
				for (; b >= 1; b--) if (rune_is_space(rb[b - 1])) break;
				for (; e < nr; e++) if (rune_is_space(rb[e])) break;
				val = clamp16(e - b);
			}
			break;
		case CR_PATHNAME:
			if (valid) {
				int last = -1;
				for (int i = nr - 1; i >= 0; i--) if (rb[i] == '/' || rb[i] == '\\') { last = i; break; }
				if (last <= minb) val = clamp16(minb - last);
			}
			break;
		case CR_BEGIN: case CR_END:
			if (valid) {
				int wp = 0;
				for (int i = 0; i < nr; i++) { wp = i; if (i == minb || !rune_is_space(rb[i])) break; }
				if (p->crit[idx] == CR_BEGIN) val = clamp16(mine - wp);
				else val = clamp16(65535 - 65535 * (maxe - wp) / ((int)it->trim + 1));
			}
			break;
		}
		out->pts[3 - idx] = val;
	}
	out->minb = valid ? minb : -1; out->mine = valid ? mine : -1; out->maxe = maxe;
	if (want_pos) { out->pos = allpos; out->npos = npos_total; } else { out->pos = NULL; out->npos = 0; }   /* points into the scratch: copy it if it must outlive the next call */
	return true;
}

/* ------------------------------------------------------------------ search driver */
typedef struct { const Pattern *p; Item **items; size_t lo, hi; Hit *hits; size_t nhits; bool need_pos; } Job;

static uint64_t key_of(const MatchOut *m)
{
	return (uint64_t)m->pts[0] | ((uint64_t)m->pts[1] << 16) | ((uint64_t)m->pts[2] << 32) | ((uint64_t)m->pts[3] << 48);
}

static void *worker(void *arg)
{
	Job *j = arg;
	Scratch *sc = scratch_new();
	j->hits = xmalloc((j->hi - j->lo + 1) * sizeof(Hit));
	for (size_t i = j->lo; i < j->hi; i++) {
		MatchOut m;
		if (pattern_match(j->p, j->items[i], sc, j->need_pos, &m)) j->hits[j->nhits++] = (Hit){(uint32_t)i, key_of(&m)};
	}
	scratch_free(sc);
	return NULL;
}

static int cmp_hits(const void *a, const void *b)
{
	const Hit *x = a, *y = b;
	if (x->key != y->key) return x->key < y->key ? -1 : 1;
	return x->idx < y->idx ? -1 : x->idx > y->idx;
}
static int cmp_hits_tac(const void *a, const void *b)
{
	const Hit *x = a, *y = b;
	if (x->key != y->key) return x->key < y->key ? -1 : 1;
	return x->idx > y->idx ? -1 : x->idx < y->idx;
}

Hit *search_items(const Opts *o, Pattern *p, Item **items, size_t n, size_t *nhits)
{
	bool need_pos = false;              /* the chunk and pathname tiebreaks need the exact match positions */
	for (int i = 1; i < o->ncrit; i++) if (o->crit[i] == CR_CHUNK || o->crit[i] == CR_PATHNAME) need_pos = true;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	int nt = ncpu < 1 ? 1 : ncpu > 16 ? 16 : (int)ncpu;
	if (n < 2000) nt = 1;
	Job *jobs = xmalloc((size_t)nt * sizeof(Job));
	pthread_t *th = xmalloc((size_t)nt * sizeof(pthread_t));
	size_t per = (n + (size_t)nt - 1) / (size_t)nt;
	for (int i = 0; i < nt; i++) {
		jobs[i] = (Job){p, items, (size_t)i * per, (size_t)(i + 1) * per < n ? (size_t)(i + 1) * per : n, NULL, 0, need_pos};
		if (jobs[i].lo > n) jobs[i].lo = jobs[i].hi = n;
	}
	if (nt == 1) worker(&jobs[0]);
	else {
		for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, worker, &jobs[i]);
		for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
	}
	size_t total = 0;
	for (int i = 0; i < nt; i++) total += jobs[i].nhits;
	Hit *all = xmalloc((total + 1) * sizeof(Hit));
	size_t k = 0;
	for (int i = 0; i < nt; i++) { memcpy(all + k, jobs[i].hits, jobs[i].nhits * sizeof(Hit)); k += jobs[i].nhits; free(jobs[i].hits); }
	free(jobs); free(th);
	bool sorted = o->sort && !pattern_empty(p) && pattern_sortable(p);
	if (sorted) qsort(all, total, sizeof(Hit), o->tac ? cmp_hits_tac : cmp_hits);
	else if (o->tac) for (size_t i = 0; i < total / 2; i++) { Hit t = all[i]; all[i] = all[total - 1 - i]; all[total - 1 - i] = t; }
	*nhits = total;
	return all;
}
