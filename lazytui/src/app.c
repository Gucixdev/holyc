/* app.c - the data-driven, event-driven UI layer. A spec (indented text) is parsed into a tree of nodes; containers become Cassowary layout
 * nodes, widgets are instantiated (searchbar, list, tree, dropdown, buttonbar, ...) and drawn from a key/value model; terminal events are routed
 * to the widget under the mouse or the focused one and come out as named events for the program's handlers. See lazytui.h for the spec language. */
#include "appint.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ model */
static mval *find_m(const lt_app *a, const char *k) { for (int i = 0; i < a->nm; i++) if (!strcmp(a->m[i].key, k)) return &a->m[i]; return NULL; }
static mval *get_m(lt_app *a, const char *k)
{
	mval *v = find_m(a, k);
	if (v) return v;
	if (a->nm == a->capm) { a->capm = a->capm ? a->capm * 2 : 32; a->m = realloc(a->m, (size_t)a->capm * sizeof *a->m); }
	a->m[a->nm] = (mval){strdup(k), 0, strdup("")};
	return &a->m[a->nm++];
}
void lt_app_set(lt_app *a, const char *key, double value)
{
	mval *v = get_m(a, key);
	if (v->n != value) a->dirty = true;
	v->n = value;
	char b[48];
	snprintf(b, sizeof b, "%.10g", value);
	free(v->s); v->s = strdup(b);
}
void lt_app_set_text(lt_app *a, const char *key, const char *text)
{
	mval *v = get_m(a, key);
	if (strcmp(v->s, text ? text : "")) a->dirty = true;
	free(v->s); v->s = strdup(text ? text : "");
	v->n = atof(v->s);
}
double lt_app_number(const lt_app *a, const char *key) { mval *v = find_m(a, key); return v ? v->n : 0; }
const char *lt_app_text(const lt_app *a, const char *key) { mval *v = find_m(a, key); return v ? v->s : ""; }

/* ------------------------------------------------------------------ nodes and attributes */
const char *app_attr(const anode *n, const char *k)
{
	for (int i = 0; i < n->nattr; i++) if (!strcmp(n->ak[i], k)) return n->av[i];
	return NULL;
}
const char *app_attr_or(const anode *n, const char *k, const char *d) { const char *v = app_attr(n, k); return v ? v : d; }
double app_attr_num(const anode *n, const char *k, double d) { const char *v = app_attr(n, k); return v ? atof(v) : d; }
#define attr app_attr
#define attr_or app_attr_or
#define attr_num app_attr_num

static void node_free(anode *n)
{
	for (int i = 0; i < n->nkid; i++) node_free(n->kid[i]);
	free(n->kid); free(n->kind); free(n->id);
	for (int i = 0; i < n->nattr; i++) { free(n->ak[i]); free(n->av[i]); }
	free(n->ak); free(n->av);
	if (n->ops && n->ops->destroy) n->ops->destroy(n);
	lt_buttonbar_free(n->bb);
	if (n->has_in) lt_input_free(&n->in);
	free(n->last_text);
	lt_list_free(n->list);
	lt_tree_free(n->tree);
	lt_dropdown_free(n->dd);
	lt_keybar_free(n->keys);
	free(n->hint_ids); free(n->zones);
	for (int i = 0; i < n->nzones; i++) free(n->zkeys[i]);
	free(n->zkeys);
	for (int i = 0; i < n->nitems; i++) free(n->items[i]);
	free(n->items);
	if (n->own) lt_layout_free(n->own);
	free(n);
}

lt_app *lt_app_new(void) { return calloc(1, sizeof(lt_app)); }
void lt_app_free(lt_app *a)
{
	if (!a) return;
	if (a->root) node_free(a->root);
	lt_layout_free(a->lay);
	for (int i = 0; i < a->nm; i++) { free(a->m[i].key); free(a->m[i].s); }
	free(a->m);
	for (int i = 0; i < a->nh; i++) { free(a->h[i].id); free(a->h[i].kind); }
	free(a->h);
	for (int i = 0; i < a->nhk; i++) { free(a->hk[i].key); free(a->hk[i].event); }
	free(a->hk);
	for (int i = 0; i < a->ntm; i++) free(a->tm[i].event);
	free(a->tm);
	for (int i = 0; i < a->ncu; i++) free(a->cu[i].type);
	free(a->cu); free(a->foc); free(a->hit);
	free(a);
}

/* ------------------------------------------------------------------ the parser */
static char *dupn(const char *s, size_t n) { char *p = malloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }

static void add_attr(anode *n, char *k, char *v)
{
	n->ak = realloc(n->ak, (size_t)(n->nattr + 1) * sizeof(char *));
	n->av = realloc(n->av, (size_t)(n->nattr + 1) * sizeof(char *));
	n->ak[n->nattr] = k; n->av[n->nattr] = v; n->nattr++;
}
static void add_kid(anode *p, anode *k)
{
	p->kid = realloc(p->kid, (size_t)(p->nkid + 1) * sizeof(anode *));
	p->kid[p->nkid++] = k;
	k->parent = p;
}

static const char *KINDS[] = {"row", "col", "stack", "panel", "scroll", "pages", "spacer", "text", "textbar", "progressbar", "ringbar", "zonebar", "pacmanbar",
                              "searchbar", "buttonbar", "dropdown", "list", "tree", "keybar", "rain", "custom", NULL};
static bool is_container(const char *k) { return !strcmp(k, "row") || !strcmp(k, "col") || !strcmp(k, "stack") || !strcmp(k, "panel") || !strcmp(k, "pages") || !strcmp(k, "scroll"); }

static bool parse_line(const char *p, anode *n, char *err, size_t errn, int line)
{
	while (*p == ' ') p++;
	const char *s = p;
	while (*p && !isspace((unsigned char)*p)) p++;
	n->kind = dupn(s, (size_t)(p - s));
	bool known = false;
	for (int i = 0; KINDS[i]; i++) if (!strcmp(KINDS[i], n->kind)) known = true;
	if (app_kind_ops(n->kind)) known = true;
	if (!known) { snprintf(err, errn, "line %d: unknown widget kind '%s'", line, n->kind); return false; }
	for (;;) {
		while (*p == ' ' || *p == '\t') p++;
		if (!*p || *p == '#') break;
		const char *ks = p;
		while (*p && *p != '=' && !isspace((unsigned char)*p)) p++;
		char *k = dupn(ks, (size_t)(p - ks)), *v;
		if (*p == '=') {
			p++;
			if (*p == '"') {
				p++;
				size_t cap = 32, len = 0;
				v = malloc(cap);
				while (*p && *p != '"') {
					if (*p == '\\' && p[1]) p++;
					if (len + 2 > cap) { cap *= 2; v = realloc(v, cap); }
					v[len++] = *p++;
				}
				if (*p != '"') { snprintf(err, errn, "line %d: unterminated quote in attribute '%s'", line, k); free(k); free(v); return false; }
				p++;
				v[len] = 0;
			} else { const char *vs = p; while (*p && !isspace((unsigned char)*p)) p++; v = dupn(vs, (size_t)(p - vs)); }
		} else v = strdup("1");
		if (!strcmp(k, "id")) { free(n->id); n->id = v; free(k); continue; }
		add_attr(n, k, v);
	}
	return true;
}

static bool parse_size(const char *v, lt_size *out)
{
	if (!v || !*v || !strcmp(v, "fill")) { *out = LT_FILL(1); return true; }
	if (!strncmp(v, "fill:", 5)) { *out = LT_FILL(atof(v + 5)); return true; }
	if (!strncmp(v, "pct:", 4)) { *out = LT_PCT(atof(v + 4)); return true; }
	if (!strncmp(v, "min:", 4)) { *out = LT_MIN(atof(v + 4)); return true; }
	if (!strncmp(v, "max:", 4)) { *out = LT_MAX(atof(v + 4)); return true; }
	char *e;
	double d = strtod(v, &e);
	if (*e) return false;
	*out = LT_FIXED(d);
	return true;
}

/* builds the layout for the subtree under `n`, whose layout node is n->ln */
static bool build(lt_app *a, anode *n, lt_layout *lay, lt_node *ln, anode *scroll, char *err, size_t errn, bool in_scroll_root)
{
	n->ln = ln;
	n->scroll = scroll;
	bool panel = !strcmp(n->kind, "panel");
	if (panel) { lt_node_pad(ln, 1, 2, 1, 2); }
	const char *gap = attr(n, "gap");
	if (gap) lt_node_gap(ln, atoi(gap));
	const char *pad = attr(n, "pad");
	if (pad) { int t = 0, r = 0, b = 0, l = 0; if (sscanf(pad, "%d,%d,%d,%d", &t, &r, &b, &l) == 4) lt_node_pad(ln, t, r, b, l); else { t = atoi(pad); lt_node_pad(ln, t, t, t, t); } }
	if (!strcmp(n->kind, "scroll")) {
		n->own = lt_layout_new();
		lt_node *root = lt_layout_root(n->own, LT_COL);
		n->content_h = 0;
		for (int i = 0; i < n->nkid; i++) {
			anode *k = n->kid[i];
			lt_size sz;
			const char *sv = attr(k, "size");
			if (!sv) sz = LT_FIXED(3);
			else if (!parse_size(sv, &sz) || sz.kind != LT_SZ_FIXED) { snprintf(err, errn, "line %d: children of a scroll need a fixed size (size=N)", k->line); return false; }
			n->content_h += (int)sz.v;
			lt_node *kn = lt_node_add(root, k->id, is_container(k->kind) && strcmp(k->kind, "scroll") ? (!strcmp(k->kind, "row") ? LT_ROW : !strcmp(k->kind, "stack") || !strcmp(k->kind, "pages") ? LT_STACK : LT_COL) : LT_COL, sz);
			if (!build(a, k, n->own, kn, n, err, errn, true)) return false;
		}
		(void)lay; (void)in_scroll_root;
		return true;
	}
	for (int i = 0; i < n->nkid; i++) {
		anode *k = n->kid[i];
		lt_size sz;
		const char *sv = attr(k, "size");
		if (sv && !parse_size(sv, &sz)) { snprintf(err, errn, "line %d: bad size '%s'", k->line, sv); return false; }
		if (!sv) sz = LT_FILL(1);
		lt_dir d = !strcmp(k->kind, "row") ? LT_ROW : (!strcmp(k->kind, "stack") || !strcmp(k->kind, "pages")) ? LT_STACK : LT_COL;
		lt_node *kn = lt_node_add(ln, k->id, d, sz);
		if (!build(a, k, lay, kn, scroll, err, errn, false)) return false;
	}
	return true;
}

static void collect(lt_app *a, anode *n)
{
	const char *k = n->kind;
	n->focusable = !strcmp(k, "searchbar") || !strcmp(k, "list") || !strcmp(k, "tree") || !strcmp(k, "dropdown") || !strcmp(k, "buttonbar");
	if (n->ops) n->focusable = n->ops->focusable;
	if (!strcmp(k, "custom")) { const char *t = attr(n, "type"); for (int i = 0; t && i < a->ncu; i++) if (!strcmp(a->cu[i].type, t) && a->cu[i].event) n->focusable = true; }
	if (attr(n, "nofocus")) n->focusable = false;
	if (n->focusable) { a->foc = realloc(a->foc, (size_t)(a->nfoc + 1) * sizeof(anode *)); a->foc[a->nfoc++] = n; }
	for (int i = 0; i < n->nkid; i++) collect(a, n->kid[i]);
}

static char **split_csv(const char *s, int *n)
{
	char **out = NULL;
	*n = 0;
	for (const char *p = s; p && *p;) {
		const char *e = strchr(p, ',');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		out = realloc(out, (size_t)(*n + 1) * sizeof(char *));
		out[(*n)++] = dupn(p, l);
		if (!e) break;
		p = e + 1;
	}
	return out;
}

static lt_slot slot_of(const char *s)
{
	static const struct { const char *n; lt_slot s; } t[] = {{"text", LT_T_TEXT}, {"dim", LT_T_DIM}, {"accent", LT_T_ACCENT}, {"ok", LT_T_OK}, {"warn", LT_T_WARN}, {"err", LT_T_ERR},
	                                                          {"bar", LT_T_BAR}, {"label", LT_T_LABEL}, {"select", LT_T_SELECT}, {"title", LT_T_TITLE}};
	for (size_t i = 0; s && i < sizeof t / sizeof t[0]; i++) if (!strcmp(t[i].n, s)) return t[i].s;
	return LT_T_TEXT;
}

static void instantiate(lt_app *a, anode *n)
{
	const char *k = n->kind;
	n->ops = app_kind_ops(k);
	if (n->ops && n->ops->init) n->ops->init(a, n);
	if (n->ops) { for (int i = 0; i < n->nkid; i++) instantiate(a, n->kid[i]); return; }
	if (!strcmp(k, "searchbar")) { lt_input_init(&n->in); n->has_in = true; }
	else if (!strcmp(k, "buttonbar")) {
		int cnt;
		char **it = split_csv(attr_or(n, "items", ""), &cnt);
		n->bb = lt_buttonbar_new((const char *const *)it, cnt);
		for (int i = 0; i < cnt; i++) free(it[i]);
		free(it);
		const char *p = attr(n, "prompt");
		if (p) lt_buttonbar_set_prompt(n->bb, p);
	} else if (!strcmp(k, "dropdown") && attr(n, "tree")) {
		n->tree = lt_tree_new();                                   /* a dropdown whose popup is a tree: fill it through lt_app_tree() */
		n->dd = lt_dropdown_from_tree(n->tree);
	} else if (!strcmp(k, "dropdown")) {
		int cnt;
		n->items = split_csv(attr_or(n, "items", ""), &cnt);
		n->nitems = cnt;
		n->dd = lt_dropdown_new((const char *const *)n->items, cnt);
	} else if (!strcmp(k, "list")) { n->list = lt_list_new(); lt_list_set_multi(n->list, attr(n, "multi") != NULL); }
	else if (!strcmp(k, "tree")) n->tree = lt_tree_new();
	else if (!strcmp(k, "keybar")) {
		lt_hint hints[32];
		char *copy = strdup(attr_or(n, "hints", "")), *save = NULL;
		int cnt = 0;
		n->hint_ids = calloc(32, sizeof(int));
		for (char *t = strtok_r(copy, ";", &save); t && cnt < 32; t = strtok_r(NULL, ";", &save)) {
			char *c1 = strchr(t, ':');
			if (!c1) continue;
			*c1 = 0;
			hints[cnt] = (lt_hint){strdup(t), strdup(c1 + 1), cnt + 1};
			cnt++;
		}
		n->keys = lt_keybar_new(hints, cnt);
		n->nhints = cnt;
		for (int i = 0; i < cnt; i++) { free((char *)hints[i].key); free((char *)hints[i].label); }
		free(copy);
	} else if (!strcmp(k, "zonebar")) {
		char *copy = strdup(attr_or(n, "zones", "")), *save = NULL;
		for (char *t = strtok_r(copy, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
			char *eq = strchr(t, '='), *co = eq ? strchr(eq, ':') : NULL;
			if (!eq) continue;
			*eq = 0;
			if (co) *co = 0;
			n->zones = realloc(n->zones, (size_t)(n->nzones + 1) * sizeof(lt_zone));
			n->zkeys = realloc(n->zkeys, (size_t)(n->nzones + 1) * sizeof(char *));
			n->zones[n->nzones] = (lt_zone){NULL, 1, co ? slot_of(co + 1) : LT_T_OK};
			n->zones[n->nzones].label = NULL;
			n->zkeys[n->nzones] = strdup(eq + 1);
			/* the label is kept in the item list so the pointer stays valid */
			n->items = realloc(n->items, (size_t)(n->nitems + 1) * sizeof(char *));
			n->items[n->nitems] = strdup(t);
			n->nitems++;
			n->nzones++;
		}
		for (int i = 0; i < n->nzones; i++) n->zones[i].label = n->items[i];
		free(copy);
	}
	(void)a;
	for (int i = 0; i < n->nkid; i++) instantiate(a, n->kid[i]);
}

static void assign_ids(anode *n, int *counter)
{
	if (!n->id) { char b[32]; snprintf(b, sizeof b, "_%d", (*counter)++); n->id = strdup(b); }
	for (int i = 0; i < n->nkid; i++) assign_ids(n->kid[i], counter);
}

bool lt_app_load(lt_app *a, const char *spec, char *err, size_t errn)
{
	char dummy[160];
	if (!err) { err = dummy; errn = sizeof dummy; }
	err[0] = 0;
	anode *stack[64] = {0};
	int indents[64] = {0}, depth = 0;
	anode *root = NULL;
	int line = 0;
	for (const char *p = spec; *p;) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		char *ln = dupn(p, len);
		p += len + (e ? 1 : 0);
		line++;
		int ind = 0;
		while (ln[ind] == ' ') ind++;
		if (!ln[ind] || ln[ind] == '#') { free(ln); continue; }
		if (ln[ind] == '\t') { snprintf(err, errn, "line %d: tabs are not allowed, indent with spaces", line); free(ln); if (root) node_free(root); return false; }
		anode *n = calloc(1, sizeof *n);
		n->line = line;
		if (!parse_line(ln, n, err, errn, line)) { free(ln); node_free(n); if (root) node_free(root); return false; }
		free(ln);
		while (depth > 0 && indents[depth - 1] >= ind) depth--;
		if (depth == 0) {
			if (root) { snprintf(err, errn, "line %d: a second top-level node (the spec has one root)", line); node_free(n); node_free(root); return false; }
			root = n;
		} else {
			if (!is_container(stack[depth - 1]->kind)) { snprintf(err, errn, "line %d: '%s' cannot have children", line, stack[depth - 1]->kind); node_free(n); node_free(root); return false; }
			add_kid(stack[depth - 1], n);
		}
		if (depth < 63) { stack[depth] = n; indents[depth] = ind; depth++; }
	}
	if (!root) { snprintf(err, errn, "the spec is empty"); return false; }
	int counter = 0;
	assign_ids(root, &counter);
	lt_layout *lay = lt_layout_new();
	lt_node *rn = lt_layout_root(lay, !strcmp(root->kind, "row") ? LT_ROW : !strcmp(root->kind, "stack") || !strcmp(root->kind, "pages") ? LT_STACK : LT_COL);
	if (!build(a, root, lay, rn, NULL, err, errn, false)) { lt_layout_free(lay); node_free(root); return false; }
	if (a->root) node_free(a->root);
	lt_layout_free(a->lay);
	free(a->foc); a->foc = NULL; a->nfoc = 0; a->focus = NULL;
	a->root = root; a->lay = lay;
	instantiate(a, root);
	collect(a, root);
	return true;
}

static void set_focus_hook(lt_app *a, anode *n);

/* ------------------------------------------------------------------ lookups */
static anode *find(anode *n, const char *id)
{
	if (n->id && !strcmp(n->id, id)) return n;
	for (int i = 0; i < n->nkid; i++) { anode *f = find(n->kid[i], id); if (f) return f; }
	return NULL;
}
static anode *nodeof(const lt_app *a, const char *id) { return a->root && id ? find(a->root, id) : NULL; }
lt_list *lt_app_list(lt_app *a, const char *id) { anode *n = nodeof(a, id); return n ? n->list : NULL; }
lt_tree *lt_app_tree(lt_app *a, const char *id) { anode *n = nodeof(a, id); return n ? n->tree : NULL; }
lt_dropdown *lt_app_dropdown(lt_app *a, const char *id) { anode *n = nodeof(a, id); return n ? n->dd : NULL; }
bool lt_app_rect(const lt_app *a, const char *id, lt_rect *out) { anode *n = nodeof(a, id); if (!n || !n->drawn) return false; if (out) *out = n->rect; return true; }
void lt_app_focus(lt_app *a, const char *id) { anode *n = nodeof(a, id); if (!id || !*id) a->focus = NULL; else if (n && n->focusable) set_focus_hook(a, n); }
const char *lt_app_focused(const lt_app *a) { return a->focus ? a->focus->id : ""; }
void lt_app_invalidate(lt_app *a) { a->dirty = true; }
void lt_app_quit(lt_app *a) { a->quit = true; if (a->ui) lt_ui_quit(a->ui); }
lt_ui *lt_app_ui(const lt_app *a) { return a->ui; }

void lt_app_custom(lt_app *a, const char *type, lt_custom_draw draw, lt_custom_event event, void *ctx)
{
	a->cu = realloc(a->cu, (size_t)(a->ncu + 1) * sizeof *a->cu);
	a->cu[a->ncu++] = (custom){strdup(type), draw, event, ctx};
	if (a->root) { free(a->foc); a->foc = NULL; a->nfoc = 0; collect(a, a->root); }
}
static custom *custom_of(lt_app *a, const anode *n)
{
	const char *t = attr(n, "type");
	for (int i = 0; t && i < a->ncu; i++) if (!strcmp(a->cu[i].type, t)) return &a->cu[i];
	return NULL;
}

/* ------------------------------------------------------------------ events out */
void lt_app_on(lt_app *a, const char *id, const char *kind, lt_app_fn fn, void *ctx)
{
	a->h = realloc(a->h, (size_t)(a->nh + 1) * sizeof *a->h);
	a->h[a->nh++] = (handler){strdup(id), strdup(kind), fn, ctx};
}
void lt_app_key(lt_app *a, const char *key, const char *event)
{
	a->hk = realloc(a->hk, (size_t)(a->nhk + 1) * sizeof *a->hk);
	a->hk[a->nhk++] = (hotkey){strdup(key), strdup(event)};
}
void lt_app_every(lt_app *a, double s, const char *event)
{
	a->tm = realloc(a->tm, (size_t)(a->ntm + 1) * sizeof *a->tm);
	a->tm[a->ntm++] = (timer){s, a->time + s, strdup(event)};
}
void lt_app_emit(lt_app *a, const char *id, const char *kind, const char *text, double number)
{
	lt_appevent ev = {id, kind, text, (int)number, number, 0};
	for (int i = 0; i < a->nh; i++)
		if ((!strcmp(a->h[i].id, "*") || !strcmp(a->h[i].id, id)) && (!strcmp(a->h[i].kind, "*") || !strcmp(a->h[i].kind, kind))) a->h[i].fn(a, &ev, a->h[i].ctx);
}

/* ------------------------------------------------------------------ drawing */
void app_mark(lt_app *a, anode *n)
{
	if (a->nhit == a->caphit) { a->caphit = a->caphit ? a->caphit * 2 : 32; a->hit = realloc(a->hit, (size_t)a->caphit * sizeof(anode *)); }
	a->hit[a->nhit++] = n;
}

#define mark app_mark
int app_canvas_h(const lt_app *a) { return a->canvas_h; }

static bool visible_rect(lt_rect r, lt_rect view) { return r.w > 0 && r.h > 0 && r.y + r.h > view.y && r.y < view.y + view.h && r.x + r.w > view.x && r.x < view.x + view.w; }

static void sync_text(lt_app *a, anode *n, const char *key)
{
	const char *m = lt_app_text(a, key);
	if (!n->last_text || strcmp(n->last_text, m)) {            /* the model changed behind the widget's back: take it over */
		if (strcmp(n->in.text, m)) lt_input_set(&n->in, m);
		free(n->last_text); n->last_text = strdup(m);
	}
}

typedef struct { lt_rect view; int ox, oy; } frame;

static void draw_node(lt_app *a, anode *n, lt_canvas *c, const lt_theme *t, frame f, bool active_page);

static lt_rect node_rect(const anode *n, frame f) { lt_rect r = lt_node_rect(n->ln); r.x += f.ox; r.y += f.oy; return r; }

static void draw_children(lt_app *a, anode *n, lt_canvas *c, const lt_theme *t, frame f)
{
	for (int i = 0; i < n->nkid; i++) draw_node(a, n->kid[i], c, t, f, true);
}

static void draw_node(lt_app *a, anode *n, lt_canvas *c, const lt_theme *t, frame f, bool active_page)
{
	(void)active_page;
	lt_rect r = node_rect(n, f);
	n->rect = r; n->inner = r; n->drawn = false;
	const char *k = n->kind;
	if (!visible_rect(r, f.view) && strcmp(k, "pages")) { for (int i = 0; i < n->nkid; i++) n->kid[i]->drawn = false; return; }
	n->drawn = true;
	n->content_y = r.y - f.oy;
	lt_rect all = lt_full(c);
	bool focused = a->focus == n;
	const char *bind = attr(n, "bind");
	lt_rect w = r;                                          /* the widget's own area (after the optional name column) */
	const char *name = attr(n, "name");
	if (name && strcmp(k, "panel")) {
		int nw = (int)attr_num(n, "namew", 12);
		lt_text(c, all, r.x, r.y, name, t->s[LT_T_DIM], nw);
		w.x += nw + 1; w.w -= nw + 1;
	}
	n->inner = w;

	if (n->ops) {
		if (n->ops->draw) n->ops->draw(a, n, c, t, w, focused);
		if (n->ops->focusable) mark(a, n);
	} else if (!strcmp(k, "row") || !strcmp(k, "col") || !strcmp(k, "stack")) draw_children(a, n, c, t, f);
	else if (!strcmp(k, "panel")) {
		lt_theme tt = *t;
		bool inside = false;
		for (anode *q = a->focus; q; q = q->parent) if (q == n) inside = true;
		if (inside) tt.s[LT_T_BORDER] = t->s[LT_T_ACCENT];
		lt_panel_ex(c, r, &tt, attr(n, "title"), LT_PANEL_HUD);
		n->inner = lt_panel_inner(r);
		draw_children(a, n, c, t, f);
	} else if (!strcmp(k, "pages")) {
		int idx = bind ? (int)lt_app_number(a, bind) : 0;
		for (int i = 0; i < n->nkid; i++) if (i != idx) { n->kid[i]->drawn = false; }
		if (idx >= 0 && idx < n->nkid) draw_node(a, n->kid[idx], c, t, f, true);
	} else if (!strcmp(k, "scroll")) {
		lt_rect content = lt_scrollview_begin(&n->sv, c, r, n->content_h);
		lt_layout_solve(n->own, content.w, n->content_h);
		frame sub = {(lt_rect){r.x, r.y, r.w, r.h}, content.x, content.y};
		n->inner = (lt_rect){r.x, r.y, content.w, r.h};
		mark(a, n);
		for (int i = 0; i < n->nkid; i++) {
			anode *kid = n->kid[i];
			lt_rect kr = lt_node_rect(kid->ln);
			kr.x += sub.ox; kr.y += sub.oy;
			kid->rect = kr;
			draw_node(a, kid, c, t, sub, true);
		}
		lt_scrollview_end(&n->sv, c, t);
	} else if (!strcmp(k, "text")) {
		const char *s = bind ? lt_app_text(a, bind) : attr_or(n, "text", "");
		lt_text(c, all, w.x, w.y, s, t->s[slot_of(attr(n, "style"))], w.w);
	} else if (!strcmp(k, "textbar")) {
		lt_textbar(c, w, t, bind ? lt_app_text(a, bind) : attr_or(n, "text", ""), slot_of(attr_or(n, "style", "select")));
	} else if (!strcmp(k, "progressbar")) {
		double mx = attr_num(n, "max", 1);
		lt_progressbar(c, w, t, bind && mx > 0 ? lt_app_number(a, bind) / mx : 0, attr(n, "label"), attr(n, "nopercent") == NULL);
	} else if (!strcmp(k, "ringbar")) {
		lt_color col = LT_DEFAULT;
		const char *cs = attr(n, "color");
		unsigned rr, gg, bb;
		if (cs && cs[0] == '#' && sscanf(cs + 1, "%2x%2x%2x", &rr, &gg, &bb) == 3) col = LT_RGB(rr, gg, bb);
		a->animated = true;
		lt_ringbar(c, w, t, a->time * attr_num(n, "speed", 9), bind ? lt_app_text(a, bind) : attr_or(n, "text", ""), col);
	} else if (!strcmp(k, "zonebar")) {
		for (int i = 0; i < n->nzones; i++) n->zones[i].weight = lt_app_number(a, n->zkeys[i]);
		lt_zonebar_ex(c, w, t, n->zones, n->nzones, attr(n, "legend") != NULL);
	} else if (!strcmp(k, "pacmanbar")) {
		double done = lt_app_number(a, attr_or(n, "done", "done")), total = lt_app_number(a, attr_or(n, "total", "total")), rate = lt_app_number(a, attr_or(n, "rate", "rate"));
		a->animated = true;
		if (done < n->last_done) memset(&n->pst, 0, sizeof n->pst);
		n->last_done = done;
		int eta = rate > 0 ? (int)((total - done) / rate) : -1;
		lt_pacmanbar(c, w, t, attr_or(n, "pkg", ""), done, total, rate, attr(n, "noeta") ? -1 : eta, &n->pst);
	} else if (!strcmp(k, "searchbar")) {
		if (bind) sync_text(a, n, bind);
		lt_searchbar(c, w, t, attr_or(n, "prompt", "> "), n->in.text, focused ? n->in.cursor : -1);
		mark(a, n);
	} else if (!strcmp(k, "buttonbar")) {
		const char *act = attr(n, "active");
		if (act) lt_buttonbar_set_active(n->bb, (int)lt_app_number(a, act));
		lt_buttonbar_draw(n->bb, c, w, t, focused);
		mark(a, n);
	} else if (!strcmp(k, "dropdown")) {
		if (bind) lt_dropdown_set_selected(n->dd, (int)lt_app_number(a, bind));
		lt_rect box = {w.x, w.y, w.w, 1};
		lt_dropdown_draw(n->dd, c, box, t, focused);
		n->inner = box;
		mark(a, n);
		if (lt_dropdown_is_open(n->dd)) a->modal = n;
	} else if (!strcmp(k, "list")) {
		lt_list_draw(n->list, c, w, t, focused);
		mark(a, n);
	} else if (!strcmp(k, "tree")) {
		lt_tree_draw(n->tree, c, w, t, focused);
		mark(a, n);
	} else if (!strcmp(k, "keybar")) {
		lt_keybar_draw(n->keys, c, w, t, a->held, a->caps && a->caps->kitty);
		mark(a, n);
	} else if (!strcmp(k, "rain")) {
		a->animated = true;
		lt_rain(c, w, t, a->time, (uint32_t)attr_num(n, "seed", 7));
	} else if (!strcmp(k, "custom")) {
		custom *cu = custom_of(a, n);
		if (attr(n, "animated")) a->animated = true;
		if (cu && cu->draw) cu->draw(a, n->id, c, w, t, focused, cu->ctx);
		if (cu && cu->event) mark(a, n);
	}
}

void lt_app_render(lt_app *a, lt_canvas *c, const lt_theme *t)
{
	if (!a->root) return;
	a->canvas_w = lt_canvas_w(c); a->canvas_h = lt_canvas_h(c);
	a->caps = lt_canvas_caps(c);
	a->nhit = 0;
	a->modal = NULL;
	a->animated = false;
	lt_layout_solve(a->lay, a->canvas_w, a->canvas_h);
	frame f = {{0, 0, a->canvas_w, a->canvas_h}, 0, 0};
	draw_node(a, a->root, c, t, f, true);
	if (a->modal) lt_dropdown_draw_popup(a->modal->dd, c, a->modal->inner, t);            /* the open popup goes over everything */
}

/* ------------------------------------------------------------------ events in */
static void report(lt_app *a, anode *n, lt_wres r, const lt_event *ev)
{
	(void)ev;
	const char *k = n->kind, *bind = attr(n, "bind");
	if (r == LT_W_IGNORED || r == LT_W_HANDLED) return;
	if (n->ops) { if (n->ops->report) n->ops->report(a, n, r); return; }
	if (!strcmp(k, "buttonbar")) {
		if (r == LT_W_ACTIVATE) {
			int i = lt_buttonbar_cursor(n->bb);
			const char *act = attr(n, "active");
			if (act) lt_app_set(a, act, i);
			lt_app_emit(a, n->id, "activate", attr_or(n, "items", ""), i);
		}
	} else if (!strcmp(k, "dropdown") && n->tree) {
		if (r == LT_W_CHANGED) {
			int node = lt_dropdown_selected_node(n->dd);
			if (bind) lt_app_set(a, bind, node);
			lt_app_emit(a, n->id, "change", lt_tree_label(n->tree, node), node);
		}
	} else if (!strcmp(k, "dropdown")) {
		if (r == LT_W_CHANGED) {
			int i = lt_dropdown_selected(n->dd);
			if (bind) lt_app_set(a, bind, i);
			lt_app_emit(a, n->id, "change", n->items && i >= 0 && i < n->nitems ? n->items[i] : "", i);
		}
	} else if (!strcmp(k, "list")) {
		int cur = lt_list_cursor(n->list);
		if (bind && r == LT_W_CHANGED) lt_app_set(a, bind, cur);
		lt_app_emit(a, n->id, r == LT_W_ACTIVATE ? "activate" : "change", lt_list_label(n->list, cur), cur);
	} else if (!strcmp(k, "tree")) {
		int sel = lt_tree_selected(n->tree);
		if (bind) lt_app_set(a, bind, sel);
		lt_app_emit(a, n->id, r == LT_W_ACTIVATE ? "activate" : r == LT_W_TOGGLE ? "toggle" : "change", lt_tree_label(n->tree, sel), sel);
	}
}

static anode *scroll_of(anode *n) { for (anode *q = n; q; q = q->parent) if (!strcmp(q->kind, "scroll")) return q; return NULL; }

static void show_focus(anode *n)
{
	anode *sc = scroll_of(n->parent ? n->parent : n);
	if (!sc || sc == n) return;
	anode *top = n;                                           /* the child of the scroll that contains n */
	while (top->parent && top->parent != sc) top = top->parent;
	lt_rect r = lt_node_rect(top->ln);
	lt_scrollview_show(&sc->sv, r.y, r.h);
}

static void set_focus(lt_app *a, anode *n) { a->focus = n; if (n) show_focus(n); }
static void set_focus_hook(lt_app *a, anode *n) { set_focus(a, n); }

static lt_wres widget_event(lt_app *a, anode *n, const lt_event *ev)
{
	const char *k = n->kind, *bind = attr(n, "bind");
	int ch = a->canvas_h;
	if (n->ops) return n->ops->event ? n->ops->event(a, n, ev) : LT_W_IGNORED;
	if (!strcmp(k, "searchbar")) {
		lt_wres r;
		bool changed = lt_searchbar_event(&n->in, attr_or(n, "prompt", "> "), n->inner, ev, &r);
		if (changed) {
			if (bind) lt_app_set_text(a, bind, n->in.text);
			free(n->last_text); n->last_text = strdup(n->in.text);
			lt_app_emit(a, n->id, "change", n->in.text, 0);
		}
		if (ev->kind == LT_EV_KEY && ev->key == LT_K_ENTER && a->focus == n) { lt_app_emit(a, n->id, "activate", n->in.text, 0); return LT_W_HANDLED; }
		return r;
	}
	if (!strcmp(k, "buttonbar")) return lt_buttonbar_event(n->bb, n->inner, ev);
	if (!strcmp(k, "dropdown")) return lt_dropdown_event(n->dd, n->inner, ev, ch);
	if (!strcmp(k, "list")) return lt_list_event(n->list, n->inner, ev);
	if (!strcmp(k, "tree")) return lt_tree_event(n->tree, n->inner, ev);
	if (!strcmp(k, "keybar")) {
		if (ev->kind == LT_EV_MOUSE && (ev->key & 0xFFFFFF) == LT_K_CLICK) { int id = lt_keybar_hit(n->keys, ev->x, ev->y); if (id > 0) { lt_app_emit(a, n->id, "activate", "hint", id - 1); return LT_W_ACTIVATE; } }
		return LT_W_IGNORED;
	}
	if (!strcmp(k, "custom")) { custom *cu = custom_of(a, n); return cu && cu->event ? cu->event(a, n->id, n->inner, ev, cu->ctx) : LT_W_IGNORED; }
	if (!strcmp(k, "scroll")) return lt_scrollview_event(&n->sv, ev, true);
	return LT_W_IGNORED;
}

static bool key_matches(const char *spec, int key) { int k = lt_key_parse(spec); return k >= 0 && k == key; }

static bool hotkey_fire(lt_app *a, int key, bool plain_ok)
{
	bool plain = (key & ~0xFFFFFF) == 0 && (key & 0xFFFFFF) >= 32 && (key & 0xFFFFFF) < LT_K_ENTER;
	for (int i = 0; i < a->nhk; i++)
		if (key_matches(a->hk[i].key, key) && (!plain || plain_ok)) { lt_app_emit(a, "app", a->hk[i].event, a->hk[i].key, 0); return true; }
	return false;
}

static void cycle_focus(lt_app *a, int dir)
{
	if (a->nfoc == 0) return;
	int cur = -1;
	for (int i = 0; i < a->nfoc; i++) if (a->foc[i] == a->focus) cur = i;
	for (int step = 1; step <= a->nfoc; step++) {
		int i = ((cur < 0 ? (dir > 0 ? -1 : 0) : cur) + dir * step % a->nfoc + a->nfoc) % a->nfoc;
		if (a->foc[i]->drawn) { set_focus(a, a->foc[i]); return; }
	}
}

/* buttonbars with the same group= behave as one bar: Left/Right at the edge of one moves into the next */
static bool bar_neighbour(lt_app *a, int dir)
{
	anode *f = a->focus;
	const char *g = f && !strcmp(f->kind, "buttonbar") ? attr(f, "group") : NULL;
	if (!g) return false;
	int cur = -1;
	for (int i = 0; i < a->nfoc; i++) if (a->foc[i] == f) cur = i;
	for (int i = cur + dir; i >= 0 && i < a->nfoc; i += dir) {
		anode *q = a->foc[i];
		if (strcmp(q->kind, "buttonbar") || !q->drawn) continue;
		const char *qg = attr(q, "group");
		if (!qg || strcmp(qg, g)) continue;
		set_focus(a, q);
		lt_buttonbar_set_cursor(q->bb, dir > 0 ? 0 : lt_buttonbar_count(q->bb) - 1);
		return true;
	}
	return false;
}

bool lt_app_dispatch(lt_app *a, const lt_event *ev, const lt_caps *caps, int held)
{
	a->held = held;
	if (caps) a->caps = caps;
	if (ev->kind == LT_EV_MODS) { a->held = ev->mods; return true; }
	if (a->modal && a->modal->dd && lt_dropdown_is_open(a->modal->dd)) {                                  /* an open dropdown is modal */
		anode *m = a->modal;
		lt_wres r = widget_event(a, m, ev);
		report(a, m, r, ev);
		return true;
	}
	int base = ev->key & 0xFFFFFF;
	if (ev->kind == LT_EV_MOUSE && a->cap && (base == LT_K_DRAG || base == LT_K_RELEASE)) {              /* the widget that took the press keeps the drag */
		anode *n = a->cap;
		if (base == LT_K_RELEASE) a->cap = NULL;
		lt_wres r = widget_event(a, n, ev);
		if (r != LT_W_IGNORED) report(a, n, r, ev);
		return true;
	}
	if (ev->kind == LT_EV_MOUSE) {
		bool over_any = false;                                              /* a click landed on SOME focusable widget's own area, even if that
		                                                                      * widget's own logic didn't claim it (e.g. the blank part of a panel) */
		for (int i = a->nhit - 1; i >= 0; i--) {
			anode *n = a->hit[i];
			if (!n->drawn) continue;
			bool over = lt_in(n->inner.w ? n->inner : n->rect, ev->x, ev->y) || !strcmp(n->kind, "scroll");
			if (!strcmp(n->kind, "scroll")) {
				if (!(lt_in(n->rect, ev->x, ev->y) || n->sv.dragging)) continue;
			} else if (!over && !(base == LT_K_DRAG || base == LT_K_RELEASE)) continue;
			if (base == LT_K_CLICK && over && n->focusable) over_any = true;
			if (!strcmp(n->kind, "scroll")) {                                                            /* the page itself: wheel, bar, drag */
				lt_wres r = lt_scrollview_event(&n->sv, ev, false);
				if (r != LT_W_IGNORED) return true;
				continue;
			}
			if (base == LT_K_CLICK && n->focusable) set_focus(a, n);
			lt_wres r = widget_event(a, n, ev);
			if (r != LT_W_IGNORED) { if (base == LT_K_CLICK && !strcmp(n->kind, "custom")) a->cap = n; report(a, n, r, ev); return true; }
		}
		if (base == LT_K_CLICK) { anode *kb = NULL; for (int i = 0; i < a->nhit; i++) if (!strcmp(a->hit[i]->kind, "keybar") && lt_in(a->hit[i]->rect, ev->x, ev->y)) kb = a->hit[i]; if (kb) { widget_event(a, kb, ev); return true; } }
		if (base == LT_K_CLICK && !over_any) a->focus = NULL;               /* only a click that hit nothing focusable clears focus - a click that
		                                                                      * landed on the focused widget's own area but wasn't "claimed" by it
		                                                                      * (e.g. its header text, not a day cell) still keeps it focused */
		lt_app_emit(a, "*", "mouse", "", base);
		return false;
	}
	if (ev->kind == LT_EV_PASTE) {
		if (a->focus && a->focus->has_in) {
			lt_wres r;
			bool ch = lt_searchbar_event(&a->focus->in, attr_or(a->focus, "prompt", "> "), a->focus->inner, ev, &r);
			if (ch) { const char *bind = attr(a->focus, "bind"); if (bind) lt_app_set_text(a, bind, a->focus->in.text); lt_app_emit(a, a->focus->id, "change", a->focus->in.text, 0); }
			return true;
		}
		return false;
	}
	if (ev->kind != LT_EV_KEY) return false;
	bool text_focus = a->focus && a->focus->has_in;
	if (hotkey_fire(a, ev->key, !text_focus) && !(text_focus && !((ev->key & ~0xFFFFFF) || base >= LT_K_ENTER))) return true;
	if (a->focus) {
		lt_wres r = widget_event(a, a->focus, ev);
		if (r != LT_W_IGNORED) { report(a, a->focus, r, ev); return true; }
		if ((ev->key == LT_K_LEFT || ev->key == LT_K_RIGHT) && bar_neighbour(a, ev->key == LT_K_RIGHT ? 1 : -1)) return true;
	}
	if (ev->key == LT_K_F6) { cycle_focus(a, 1); return true; }
	if (ev->key == LT_K_TAB) { cycle_focus(a, 1); return true; }
	if (ev->key == LT_K_BTAB) { cycle_focus(a, -1); return true; }
	if (ev->key == LT_K_ESC && a->focus) { a->focus = NULL; return true; }
	for (int i = 0; i < a->nhit; i++) if (!strcmp(a->hit[i]->kind, "scroll") && a->hit[i]->drawn) { if (lt_scrollview_event(&a->hit[i]->sv, ev, true) != LT_W_IGNORED) return true; }
	char nm[32];
	lt_key_name(ev->key, nm, sizeof nm);
	lt_app_emit(a, "*", "key", nm, ev->key);
	return false;
}

void lt_app_tick(lt_app *a, double dt)
{
	a->time += dt;
	for (int i = 0; i < a->ntm; i++) if (a->time >= a->tm[i].next) { a->tm[i].next = a->time + a->tm[i].every; lt_app_emit(a, "app", a->tm[i].event, "", a->time); }
	lt_app_emit(a, "app", "tick", "", dt);
}

/* ------------------------------------------------------------------ attaching to a lt_ui loop */
static void ui_draw(lt_ui *ui, lt_canvas *c, void *ctx) { lt_app_render(ctx, c, lt_ui_theme(ui)); }
static bool ui_event(lt_ui *ui, const lt_event *ev, void *ctx)
{
	lt_app *a = ctx;
	lt_tty *t = lt_ui_term(ui);
	lt_caps caps = lt_tty_caps(t);
	static lt_caps keep;
	keep = caps;
	bool used = lt_app_dispatch(a, ev, &keep, lt_tty_mods(t));
	return used;
}
static void ui_tick(lt_ui *ui, double dt, void *ctx)
{
	lt_app *a = ctx;
	lt_app_tick(a, dt);
	if (a->dirty || a->animated) lt_ui_redraw(ui);                     /* an idle screen is not redrawn 30 times a second */
	a->dirty = false;
}

void lt_app_attach(lt_app *a, lt_ui *ui)
{
	a->ui = ui;
	lt_ui_on_draw(ui, ui_draw, a);
	lt_ui_on_event(ui, ui_event, a);
	lt_ui_on_tick(ui, ui_tick, a);
	lt_ui_set_tick_redraw(ui, false);
}
