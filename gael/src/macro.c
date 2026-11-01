/* macro.c - hygienic macros, in the style of Scheme's syntax-rules.
 *
 *   (defmacro swap
 *     ((_ a b) (let ((tmp a)) (set a b) (set b tmp))))
 *
 * A macro is a list of rules, each (pattern template); the first rule whose pattern matches a use rewrites it, and the
 * compiler goes on with what came out. A pattern is a list whose first element stands for the macro's name and is not
 * matched (write _). In it a symbol is a pattern variable, _ matches any one form, a keyword, a number or a string
 * matches only itself, a list matches a list, and ... after an element matches it zero or more times (one ... per
 * list, other elements may follow it). In the template a pattern variable becomes a copy of what it matched, x ...
 * repeats x once for each form the variables in x matched, and everything else is copied as it is. In what comes out,
 * (string-of x) becomes a string and (name-of "print-" x) a name, as # and ## do in C (text_forms).
 *
 * Hygiene. Every expansion has a stamp, and the symbols its template brings in carry it; the forms a use hands in keep
 * theirs (0 for the program's own text). The compiler binds a local under a name and a stamp, and a symbol sees only
 * the locals bound with its own stamp. So a name a template binds (the tmp of swap) never captures one from the use,
 * and a name a template uses without binding it (let, set, a global function, another macro) finds no local with its
 * stamp and means what it means at the top level, where every macro is defined - whatever the use has bound under that
 * name.
 *
 * Rules are copied into a table of their own when they are defined, because a unit's nodes are gone when the next unit
 * is read; an expansion copies its template back into the unit's nodes. Checking, matching and copying keep their own
 * stacks: no recursion in C. */

#define MAX_MACROS (1 << 14)
#define MAX_RULES (1 << 16)
#define MAX_MNODES (1 << 20)
#define MTEXT_BYTES ((size_t)64 << 20)
#define MAX_PVARS 63            /* pattern variables in one rule: one bit each of a mask in an i64 */
#define MAX_BINDS (1 << 15)     /* what one use matched */
#define MAX_MSTACK 1024         /* lists nested in one pattern, template or copy */
#define MAX_EXPANSIONS 100000   /* in one unit, so a macro that expands into itself forever stops */

typedef struct {
	const char *name;
	i32 rule0, nrules; /* its rules in g_rules */
	const char *file;  /* where it was defined */
	i32 line;
	i32 gem;           /* the gem it belongs to, 0 for the program */
} Macro;

typedef struct {
	i32 pattern, tmpl; /* in g_mnodes */
	i32 nvars;
	u8 depth[MAX_PVARS]; /* how many ... each pattern variable is under */
} Rule;

/* what a pattern variable matched: a form (node), or a list of binds one level down, one for each repetition */
typedef struct {
	i32 node;
	i32 first, last, next, n;
} Bind;

/* a list being walked by the checks, the matcher or a copy */
typedef struct {
	i32 cur;     /* its next element */
	i32 list;    /* the list itself; in a copy, the list being built */
	i32 last;    /* in a copy: the last element built so far */
	i32 x;       /* matching: the use's next element */
	i32 dotted;  /* matching: the pattern element followed by ..., -1 when there is none */
	i32 depth;   /* checks: how many ... its elements are under */
	i32 rep;     /* the element being repeated, -1 when none is */
	i32 left;    /* repetitions left */
	i64 vars;    /* checks: the pattern variables in it; expanding: the ones that step with each repetition */
	u8 seen;     /* checks: a ... was seen in this list */
	u8 busy;     /* a repetition is running, to be closed when control comes back to this list */
} MFrame;

static Node *g_mnodes; /* these three and the text: reserved address space (compile.c's tables_init) */
static i32 g_nmnodes;
static Rule *g_rules;
static i32 g_nrules;
static Macro *g_macros;
static i32 g_nmacros;
static Arena g_mtext = { 0, 0, 0, "macro text" };
static u32 g_stamps;     /* the last stamp an expansion got */
static i32 g_expansions; /* in this unit */

static MFrame g_mf[MAX_MSTACK];
static Bind g_binds[MAX_BINDS];
static i32 g_nbinds;
static i32 g_ctx[MAX_PVARS];      /* matching: where each variable's next match goes; copying: what it stands for */
static i32 g_rem[MAX_PVARS];      /* copying: how many ... each variable is still under */
static i32 g_saved[8192][3];      /* a variable, its bind and its rem, while a repetition runs */
static i32 g_nsaved;
static const char *g_pvname[MAX_PVARS]; /* the variables of the rule being checked */
static u8 g_pvdepth[MAX_PVARS];
static i32 g_npv;

static int is_reserved(const char *s); /* compile.c */

static int dots(const Node *a, i32 n) { return n >= 0 && a[n].kind == N_SYM && !strcmp(a[n].text, "..."); }

static i64 bit(i32 v) { return (i64)1 << v; }

/* find_macro NAME: the macro of that name, the newest one when the REPL defined it again; -1 when there is none */
static i32 find_macro(const char *name)
{
	const NameSlot *sl = names_get(name);
	return sl ? sl->macro : -1;
}

/* mcopy N: the unit's form N, everything in it and its text, copied into the macro table */
static i32 mnode(i32 n)
{
	Node *d;
	if (g_nmnodes == MAX_MNODES) die("macros too large (MAX_MNODES)");
	d = &g_mnodes[g_nmnodes];
	*d = g_nodes[n];
	d->first = d->next = -1;
	if (d->text) d->text = arena_strndup(&g_mtext, d->text, d->len);
	return g_nmnodes++;
}

static i32 mcopy(i32 n)
{
	i32 sp = 0, root = mnode(n);
	if (g_nodes[n].kind == N_LIST) {
		g_mf[0].cur = g_nodes[n].first;
		g_mf[0].list = root;
		g_mf[0].last = -1;
		sp = 1;
	}
	while (sp > 0) {
		MFrame *f = &g_mf[sp - 1];
		i32 c = f->cur, d;
		if (c < 0) { sp--; continue; }
		f->cur = g_nodes[c].next;
		d = mnode(c);
		if (f->last < 0) g_mnodes[f->list].first = d; else g_mnodes[f->last].next = d;
		f->last = d;
		if (g_nodes[c].kind == N_LIST) {
			if (sp == MAX_MSTACK) die("a macro nested too deep (MAX_MSTACK)");
			g_mf[sp].cur = g_nodes[c].first;
			g_mf[sp].list = d;
			g_mf[sp].last = -1;
			sp++;
		}
	}
	return root;
}

/* ucopy N: a copy of the unit's form N in the unit, each node keeping its stamp and its place in the source */
static i32 unode(i32 n)
{
	i32 d = new_node((NodeKind)g_nodes[n].kind, g_nodes[n].line, g_nodes[n].col);
	g_nodes[d] = g_nodes[n];
	g_nodes[d].first = g_nodes[d].next = -1;
	return d;
}

static i32 ucopy(i32 n)
{
	static MFrame st[MAX_MSTACK];
	i32 sp = 0, root = unode(n);
	if (g_nodes[n].kind == N_LIST) {
		st[0].cur = g_nodes[n].first;
		st[0].list = root;
		st[0].last = -1;
		sp = 1;
	}
	while (sp > 0) {
		MFrame *f = &st[sp - 1];
		i32 c = f->cur, d;
		if (c < 0) { sp--; continue; }
		f->cur = g_nodes[c].next;
		d = unode(c);
		if (f->last < 0) g_nodes[f->list].first = d; else g_nodes[f->last].next = d;
		f->last = d;
		if (g_nodes[c].kind == N_LIST) {
			if (sp == MAX_MSTACK) die("a macro's expansion nested too deep (MAX_MSTACK)");
			st[sp].cur = g_nodes[c].first;
			st[sp].list = d;
			st[sp].last = -1;
			sp++;
		}
	}
	return root;
}

/* check_pattern P: a rule's pattern - give each pattern variable a number (a symbol's ival becomes it + 1, a list's
 * ival the mask of the variables in it) and find how many ... each one is under; 0 after an error it reported */
static int check_pattern(i32 p)
{
	i32 sp = 1, kw = p >= 0 && g_nodes[p].kind == N_LIST ? g_nodes[p].first : -1;
	g_npv = 0;
	if (kw < 0 || g_nodes[kw].kind != N_SYM) {
		error_at(g_nodes[p].line, g_nodes[p].col, "a pattern is a list that starts with _, like (_ a b)");
		return 0;
	}
	g_nodes[kw].ival = 0;
	g_mf[0].cur = g_nodes[kw].next;
	g_mf[0].list = p;
	g_mf[0].depth = 0;
	g_mf[0].vars = 0;
	g_mf[0].seen = 0;
	while (sp > 0) {
		MFrame *f = &g_mf[sp - 1];
		i32 c = f->cur, d;
		int rep;
		const char *s;
		if (c < 0) {
			i64 vars = f->vars;
			g_nodes[f->list].ival = vars;
			if (--sp > 0) g_mf[sp - 1].vars |= vars;
			continue;
		}
		f->cur = g_nodes[c].next;
		if (dots(g_nodes, c)) {
			error_at(g_nodes[c].line, g_nodes[c].col, "... comes right after the part of the pattern it repeats");
			return 0;
		}
		rep = dots(g_nodes, g_nodes[c].next);
		if (rep) {
			if (f->seen) {
				error_at(g_nodes[g_nodes[c].next].line, g_nodes[g_nodes[c].next].col, "a list in a pattern can have one ...");
				return 0;
			}
			f->seen = 1;
			f->cur = g_nodes[g_nodes[c].next].next;
		}
		d = f->depth + rep;
		if (g_nodes[c].kind == N_LIST) {
			if (sp == MAX_MSTACK) { error_at(g_nodes[c].line, g_nodes[c].col, "pattern nested too deep"); return 0; }
			g_mf[sp].cur = g_nodes[c].first;
			g_mf[sp].list = c;
			g_mf[sp].depth = d;
			g_mf[sp].vars = 0;
			g_mf[sp].seen = 0;
			sp++;
			continue;
		}
		if (g_nodes[c].kind != N_SYM) continue; /* a keyword, a number or a string: matches itself */
		s = g_nodes[c].text;
		g_nodes[c].ival = 0;
		if (!strcmp(s, "_")) continue;
		if (strchr(s, '.')) {
			error_at(g_nodes[c].line, g_nodes[c].col, "%.64s cannot be a pattern variable: a pattern variable is a plain name", s);
			return 0;
		}
		if (is_reserved(s)) { /* a built-in name matches only itself, like the : of (name : type) */
			g_nodes[c].ival = -1;
			continue;
		}
		{
			i32 v;
			for (v = 0; v < g_npv; v++) {
				if (!strcmp(g_pvname[v], s)) {
					error_at(g_nodes[c].line, g_nodes[c].col, "%.64s is in this pattern twice", s);
					return 0;
				}
			}
		}
		if (g_npv == MAX_PVARS) {
			error_at(g_nodes[c].line, g_nodes[c].col, "a pattern has at most %d variables", MAX_PVARS);
			return 0;
		}
		g_pvname[g_npv] = s;
		g_pvdepth[g_npv] = (u8)(d > 255 ? 255 : d);
		f->vars |= bit(g_npv);
		g_nodes[c].ival = ++g_npv;
	}
	return 1;
}

static i32 pattern_var(const char *s)
{
	i32 v;
	for (v = 0; v < g_npv; v++) {
		if (!strcmp(g_pvname[v], s)) return v;
	}
	return -1;
}

/* drives VARS DEPTH: one of the variables VARS was matched under DEPTH ... or more, so it can drive a ... at DEPTH */
static int drives(i64 vars, i32 depth)
{
	i32 v;
	for (v = 0; v < g_npv; v++) {
		if ((vars & bit(v)) && g_pvdepth[v] >= depth) return 1;
	}
	return 0;
}

/* check_template T: mark the pattern variables in a rule's template (ival as in a pattern) and check its ...: each one
 * repeats a part with a variable that was matched under as many ..., and each variable stands under at least as many
 * ... as it was matched under; 0 after an error it reported */
static int check_template(i32 t)
{
	i32 sp = 0, v;
	if (g_nodes[t].kind == N_SYM) {
		g_nodes[t].ival = 0;
		if ((v = pattern_var(g_nodes[t].text)) < 0) return 1;
		g_nodes[t].ival = v + 1;
		if (g_pvdepth[v] > 0) {
			error_at(g_nodes[t].line, g_nodes[t].col, "%.64s matched a sequence (it is under ... in the pattern), so it needs ... after it here", g_nodes[t].text);
			return 0;
		}
		return 1;
	}
	if (g_nodes[t].kind != N_LIST) return 1;
	g_mf[0].cur = g_nodes[t].first;
	g_mf[0].list = t;
	g_mf[0].depth = 0;
	g_mf[0].vars = 0;
	g_mf[0].rep = 0;
	sp = 1;
	while (sp > 0) {
		MFrame *f = &g_mf[sp - 1];
		i32 c = f->cur, d;
		int rep;
		if (c < 0) {
			i64 vars = f->vars;
			g_nodes[f->list].ival = vars;
			if (f->rep && !drives(vars, f->depth)) {
				error_at(g_nodes[f->list].line, g_nodes[f->list].col,
				         "the ... after this repeats nothing: no variable in it was matched under ... in the pattern");
				return 0;
			}
			if (--sp > 0) g_mf[sp - 1].vars |= vars;
			continue;
		}
		f->cur = g_nodes[c].next;
		if (dots(g_nodes, c)) {
			error_at(g_nodes[c].line, g_nodes[c].col, "... comes right after the part of the template it repeats");
			return 0;
		}
		rep = dots(g_nodes, g_nodes[c].next);
		if (rep) f->cur = g_nodes[g_nodes[c].next].next;
		d = f->depth + rep;
		if (g_nodes[c].kind == N_LIST) {
			if (sp == MAX_MSTACK) { error_at(g_nodes[c].line, g_nodes[c].col, "template nested too deep"); return 0; }
			g_mf[sp].cur = g_nodes[c].first;
			g_mf[sp].list = c;
			g_mf[sp].depth = d;
			g_mf[sp].vars = 0;
			g_mf[sp].rep = rep;
			sp++;
			continue;
		}
		if (g_nodes[c].kind == N_SYM) g_nodes[c].ival = 0;
		v = g_nodes[c].kind == N_SYM ? pattern_var(g_nodes[c].text) : -1;
		if (v < 0) {
			if (rep) {
				error_at(g_nodes[c].line, g_nodes[c].col, "the ... after this repeats nothing: it is not a pattern variable");
				return 0;
			}
			continue;
		}
		g_nodes[c].ival = v + 1;
		f->vars |= bit(v);
		if (g_pvdepth[v] > d) {
			error_at(g_nodes[c].line, g_nodes[c].col, "%.64s is under %d ... in the pattern and %d here; it needs as many here",
			         g_nodes[c].text, (int)g_pvdepth[v], (int)d);
			return 0;
		}
		if (rep && g_pvdepth[v] < d) {
			error_at(g_nodes[c].line, g_nodes[c].col, "the ... after %.64s repeats nothing: it was matched once, not under ...", g_nodes[c].text);
			return 0;
		}
	}
	return 1;
}

/* macro_define N: (defmacro name (pattern template)...) - check its rules and keep them, as a new macro (in the REPL
 * one of the same name stays behind it, unseen). Its name was checked by the caller. 0 after an error it reported */
static int macro_define(i32 n)
{
	i32 name = g_nodes[g_nodes[n].first].next, r, k = 0, rule0 = g_nrules, v;
	for (r = g_nodes[name].next; r >= 0; r = g_nodes[r].next) {
		i32 p = g_nodes[r].kind == N_LIST ? g_nodes[r].first : -1, t = p >= 0 ? g_nodes[p].next : -1;
		if (t < 0 || g_nodes[t].next >= 0) {
			error_at(g_nodes[r].line, g_nodes[r].col, "a rule is (pattern template), like ((_ a b) (+ a b))");
			return 0;
		}
		if (!check_pattern(p) || !check_template(t)) return 0;
		if (g_nrules == MAX_RULES) die("too many macro rules (MAX_RULES)");
		g_rules[g_nrules].pattern = mcopy(p);
		g_rules[g_nrules].tmpl = mcopy(t);
		g_rules[g_nrules].nvars = g_npv;
		for (v = 0; v < g_npv; v++) g_rules[g_nrules].depth[v] = g_pvdepth[v];
		g_nrules++;
		k++;
	}
	if (!k) {
		error_at(g_nodes[n].line, g_nodes[n].col, "defmacro needs a rule: (defmacro %.64s ((_ x) template))", g_nodes[name].text);
		return 0;
	}
	if (g_nmacros == MAX_MACROS) die("too many macros (MAX_MACROS)");
	g_macros[g_nmacros].name = arena_strndup(&g_mtext, g_nodes[name].text, g_nodes[name].len);
	g_macros[g_nmacros].rule0 = rule0;
	g_macros[g_nmacros].nrules = k;
	g_macros[g_nmacros].file = g_unit.file;
	g_macros[g_nmacros].line = g_nodes[n].line;
	g_macros[g_nmacros].gem = g_unit_gem;
	names_put(g_macros[g_nmacros].name)->macro = g_nmacros;
	g_nmacros++;
	return 1;
}

static i32 new_bind(void)
{
	Bind *b;
	if (g_nbinds == MAX_BINDS) die("a macro use matched too much (MAX_BINDS)");
	b = &g_binds[g_nbinds];
	b->node = b->first = b->last = b->next = -1;
	b->n = 0;
	return g_nbinds++;
}

/* list_start F P X: F walks the pattern list's elements from P against the use's from X; 0 when their counts cannot
 * match. The element followed by ..., if there is one, takes as many as the others leave. */
static int list_start(MFrame *f, i32 p, i32 x)
{
	i32 k = 0, n = 0, c;
	f->cur = p;
	f->x = x;
	f->dotted = -1;
	f->rep = -1;
	f->left = 0;
	f->busy = 0;
	for (c = p; c >= 0; c = g_mnodes[c].next) {
		if (dots(g_mnodes, c)) continue;
		k++;
		if (dots(g_mnodes, g_mnodes[c].next)) f->dotted = c;
	}
	for (c = x; c >= 0; c = g_nodes[c].next) n++;
	if (f->dotted < 0) return n == k;
	f->left = n - (k - 1);
	return f->left >= 0;
}

/* match_one P X: the pattern element P against the use's form X: a variable takes it, a list goes on the stack */
static int match_one(i32 p, i32 x, i32 *sp)
{
	const Node *pn = &g_mnodes[p], *xn = &g_nodes[x];
	switch (pn->kind) {
	case N_SYM:
		if (pn->ival < 0) return xn->kind == N_SYM && !strcmp(pn->text, xn->text); /* a built-in name */
		if (pn->ival > 0) g_binds[g_ctx[pn->ival - 1]].node = x;
		return 1;
	case N_KEY: return xn->kind == N_KEY && !strcmp(pn->text, xn->text);
	case N_INT: return xn->kind == N_INT && pn->ival == xn->ival && pn->big == xn->big;
	case N_FLOAT: return xn->kind == N_FLOAT && pn->fval == xn->fval;
	case N_STR: return xn->kind == N_STR && pn->len == xn->len && !memcmp(pn->text, xn->text, pn->len);
	default:
		if (xn->kind != N_LIST) return 0;
		if (*sp == MAX_MSTACK) return 0;
		if (!list_start(&g_mf[*sp], pn->first, xn->first)) return 0;
		(*sp)++;
		return 1;
	}
}

/* rep_vars P: the pattern variables in the pattern element P */
static i64 rep_vars(const Node *a, i32 p)
{
	if (a[p].kind == N_LIST) return a[p].ival;
	return a[p].kind == N_SYM && a[p].ival > 0 ? bit((i32)a[p].ival - 1) : 0;
}

/* match_rule R CALL: R's pattern against the use CALL; on a match each variable's bind is in g_ctx */
static int match_rule(const Rule *r, i32 call)
{
	i32 sp = 1, v;
	g_nbinds = 0;
	g_nsaved = 0;
	for (v = 0; v < r->nvars; v++) g_ctx[v] = new_bind();
	if (!list_start(&g_mf[0], g_mnodes[g_mnodes[r->pattern].first].next, g_nodes[g_nodes[call].first].next)) return 0;
	while (sp > 0) {
		MFrame *f = &g_mf[sp - 1];
		i32 x;
		if (f->busy) { /* the repetition that just ran: its variables go back to their lists */
			i64 vars = rep_vars(g_mnodes, f->rep);
			f->busy = 0;
			for (v = 0; v < r->nvars; v++) {
				if (vars & bit(v)) g_ctx[v] = g_saved[--g_nsaved][1];
			}
		}
		if (f->rep >= 0) {
			if (f->left > 0) {
				i64 vars = rep_vars(g_mnodes, f->rep);
				f->left--;
				for (v = r->nvars - 1; v >= 0; v--) { /* each variable in it gets one more bind for this repetition */
					if (vars & bit(v)) {
						i32 b = new_bind(), seq = g_ctx[v];
						if (g_nsaved == (i32)GAEL_COUNT(g_saved)) return 0;
						g_saved[g_nsaved][0] = v;
						g_saved[g_nsaved++][1] = seq;
						if (g_binds[seq].last < 0) g_binds[seq].first = b; else g_binds[g_binds[seq].last].next = b;
						g_binds[seq].last = b;
						g_binds[seq].n++;
						g_ctx[v] = b;
					}
				}
				f->busy = 1;
				x = f->x;
				f->x = g_nodes[x].next;
				if (!match_one(f->rep, x, &sp)) return 0;
				continue;
			}
			f->cur = g_mnodes[g_mnodes[f->rep].next].next; /* past the ... */
			f->rep = -1;
			f->dotted = -1;
		}
		if (f->cur < 0) { sp--; continue; }
		if (f->cur == f->dotted) { f->rep = f->cur; continue; }
		x = f->x;
		f->x = g_nodes[x].next;
		{
			i32 p = f->cur;
			f->cur = g_mnodes[p].next;
			if (!match_one(p, x, &sp)) return 0;
		}
	}
	return 1;
}

/* var_name R V: the name of R's pattern variable V, for messages: its pattern's nodes come right before its template */
static const char *var_name(const Rule *r, i32 v)
{
	i32 i;
	for (i = r->pattern; i < r->tmpl; i++) {
		if (g_mnodes[i].kind == N_SYM && g_mnodes[i].ival == v + 1) return g_mnodes[i].text;
	}
	return "?";
}

/* telem SP F T: the expansion of the template element T, put after the elements of the list F builds (F 0: the
 * root); a list's elements come next, from the stack */
static i32 telem(i32 *sp, MFrame *f, i32 t, u32 stamp, i32 line, i32 col)
{
	const Node *s = &g_mnodes[t];
	i32 d;
	if (s->kind == N_SYM && s->ival > 0) {
		d = ucopy(g_binds[g_ctx[s->ival - 1]].node); /* what the use handed in, with its own stamp and place */
	} else {
		Node *nd;
		d = new_node((NodeKind)s->kind, line, col); /* the template's own: this expansion's stamp, the use's place */
		nd = &g_nodes[d];
		nd->ival = s->kind == N_INT ? s->ival : 0;
		nd->big = s->big;
		nd->fval = s->fval;
		nd->text = s->text;
		nd->len = s->len;
		nd->stamp = stamp;
	}
	if (f) {
		if (f->last < 0) g_nodes[f->list].first = d; else g_nodes[f->last].next = d;
		f->last = d;
	}
	if (s->kind == N_LIST && s->first >= 0) {
		if (*sp == MAX_MSTACK) die("a macro's expansion nested too deep (MAX_MSTACK)");
		g_mf[*sp].cur = s->first;
		g_mf[*sp].list = d;
		g_mf[*sp].last = -1;
		g_mf[*sp].rep = -1;
		g_mf[*sp].busy = 0;
		(*sp)++;
	}
	return d;
}

/* expand_rule R CALL: R's template with what its variables matched in CALL (in g_ctx); -1 after an error it reported */
static i32 expand_rule(const Rule *r, i32 call)
{
	i32 sp = 0, root, v, line = g_nodes[call].line, col = g_nodes[call].col;
	u32 stamp = ++g_stamps;
	for (v = 0; v < r->nvars; v++) g_rem[v] = r->depth[v];
	g_nsaved = 0;
	root = telem(&sp, 0, r->tmpl, stamp, line, col);
	while (sp > 0) {
		MFrame *f = &g_mf[sp - 1];
		i32 c;
		if (f->busy) { /* a repetition just ran: its variables step to their next match */
			f->busy = 0;
			for (v = 0; v < r->nvars; v++) {
				if (f->vars & bit(v)) g_ctx[v] = g_binds[g_ctx[v]].next;
			}
			if (f->left > 0) {
				f->left--;
				f->busy = 1;
				telem(&sp, f, f->rep, stamp, line, col);
				continue;
			}
			for (v = r->nvars - 1; v >= 0; v--) { /* all done: back to the lists they were stepping through */
				if (f->vars & bit(v)) {
					g_nsaved--;
					g_ctx[v] = g_saved[g_nsaved][1];
					g_rem[v] = g_saved[g_nsaved][2];
				}
			}
			f->cur = g_mnodes[g_mnodes[f->rep].next].next;
			f->rep = -1;
		}
		c = f->cur;
		if (c < 0) { sp--; continue; }
		if (dots(g_mnodes, g_mnodes[c].next)) { /* c ...: once for each match of the variables in c still under ... */
			i64 vars = rep_vars(g_mnodes, c);
			i32 n = -1, first = -1;
			f->vars = 0;
			for (v = 0; v < r->nvars; v++) {
				if (!(vars & bit(v)) || g_rem[v] <= 0) continue;
				f->vars |= bit(v);
				if (first < 0) {
					first = v;
					n = g_binds[g_ctx[v]].n;
				} else if (g_binds[g_ctx[v]].n != n) {
					error_at(line, col, "%.64s matched %ld forms and %.64s %ld, so one ... cannot repeat them together",
					         var_name(r, first), (long)n, var_name(r, v), (long)g_binds[g_ctx[v]].n);
					return -1;
				}
			}
			if (n <= 0) {
				f->cur = g_mnodes[g_mnodes[c].next].next;
				continue;
			}
			for (v = 0; v < r->nvars; v++) {
				if (!(f->vars & bit(v))) continue;
				if (g_nsaved == (i32)GAEL_COUNT(g_saved)) die("a macro's expansion nested too deep (g_saved)");
				g_saved[g_nsaved][0] = v;
				g_saved[g_nsaved][1] = g_ctx[v];
				g_saved[g_nsaved++][2] = g_rem[v];
				g_ctx[v] = g_binds[g_ctx[v]].first;
				g_rem[v]--;
			}
			f->rep = c;
			f->left = n - 1;
			f->busy = 1;
			telem(&sp, f, c, stamp, line, col);
			continue;
		}
		f->cur = g_mnodes[c].next;
		telem(&sp, f, c, stamp, line, col);
	}
	return root;
}

static o putn(char *buf, size_t cap, size_t *at, const char *s, size_t n)
{
	if (*at + n >= cap) n = cap - 1 - *at;
	memcpy(buf + *at, s, n);
	*at += n;
	buf[*at] = 0;
}

/* nprint NODES ROOT BUF CAP AT: the form ROOT of NODES (the unit's or the macro table's) as source text, added at *AT;
 * cut short to fit in CAP. A number is written as it was read, a string between quotes */
static o nprint(const Node *a, i32 root, char *buf, size_t cap, size_t *at)
{
	static i32 st[MAX_MSTACK]; /* the open lists: what comes after each */
	i32 sp = 0, c = root;
	int first = 1;
	char num[40];
	for (;;) {
		const Node *nd;
		i32 next;
		if (c < 0) { /* a list ends, or the root was an atom */
			if (sp == 0) return;
			putn(buf, cap, at, ")", 1);
			c = st[--sp];
			first = 0;
			continue;
		}
		nd = &a[c];
		next = c == root ? -1 : nd->next;
		if (!first) putn(buf, cap, at, " ", 1);
		first = 0;
		if (nd->kind == N_LIST) {
			putn(buf, cap, at, "(", 1);
			if (sp == MAX_MSTACK) { putn(buf, cap, at, "...)", 4); c = next; continue; }
			st[sp++] = next;
			c = nd->first;
			first = 1;
			continue;
		}
		if (nd->kind == N_STR) {
			putn(buf, cap, at, "\"", 1);
			putn(buf, cap, at, nd->text, nd->len);
			putn(buf, cap, at, "\"", 1);
		} else if (nd->text) {
			putn(buf, cap, at, nd->text, nd->len);
		} else { /* a number some code made, with no text of its own */
			if (nd->kind == N_FLOAT) sprintf(num, "%g", nd->fval);
			else if (nd->big) sprintf(num, "%lu", (unsigned long)(u64)nd->ival);
			else sprintf(num, "%ld", (long)nd->ival);
			putn(buf, cap, at, num, strlen(num));
		}
		c = next;
	}
}

/* mprint P NAME BUF CAP: the pattern P as text, with NAME for its first element; cut short to fit in CAP */
static o mprint(i32 p, const char *name, char *buf, size_t cap)
{
	size_t at = 0;
	i32 c;
	buf[0] = 0;
	putn(buf, cap, &at, "(", 1);
	putn(buf, cap, &at, name, strlen(name));
	for (c = g_mnodes[g_mnodes[p].first].next; c >= 0; c = g_mnodes[c].next) {
		putn(buf, cap, &at, " ", 1);
		nprint(g_mnodes, c, buf, cap, &at);
	}
	putn(buf, cap, &at, ")", 1);
}

/* The x-macro helpers, C's # and ##: (string-of part...) is a string and (name-of part...) a name, each the parts'
 * text joined - a string gives what it holds, a name or a number itself, and string-of writes a list as source. Both
 * are worked out where they stand: in what a macro expands to, right after it expands (a defmacro in it keeps its own
 * for its own uses), and in code written by hand when the compiler meets them. A name made this way is in the scope
 * of the first name among its parts, as if the use had written it; with no name among them it is the template's own. */
#define TEXT_FORM_BYTES 4096

/* text_form N: the list N, when it is (string-of ...) or (name-of ...), becomes the string or the name; 0 after an
 * error it reported */
static int text_form(i32 n)
{
	static char buf[TEXT_FORM_BYTES + 1];
	i32 head = g_nodes[n].first, p, scope = -1;
	size_t at = 0;
	int name;
	Node *nd;
	if (head < 0 || g_nodes[head].kind != N_SYM) return 1;
	if (!strcmp(g_nodes[head].text, "string-of")) name = 0;
	else if (!strcmp(g_nodes[head].text, "name-of")) name = 1;
	else return 1;
	buf[0] = 0;
	if (g_nodes[head].next < 0) {
		error_at(g_nodes[n].line, g_nodes[n].col, name ? "name-of joins the parts of a name: (name-of \"print-\" x)"
		                                               : "string-of takes what to write as a string: (string-of x)");
		return 0;
	}
	for (p = g_nodes[head].next; p >= 0; p = g_nodes[p].next) {
		const Node *pn = &g_nodes[p];
		if (name && (pn->kind == N_LIST || pn->kind == N_FLOAT)) {
			error_at(pn->line, pn->col, "name-of joins names, strings and ints; %s cannot be part of a name",
			         pn->kind == N_LIST ? "a list" : "a float");
			return 0;
		}
		if (pn->kind == N_SYM && scope < 0) scope = p;
		if (pn->kind == N_STR) putn(buf, sizeof(buf), &at, pn->text, pn->len);
		else nprint(g_nodes, p, buf, sizeof(buf), &at);
	}
	if (at >= TEXT_FORM_BYTES) {
		error_at(g_nodes[n].line, g_nodes[n].col, "%s: the text is longer than %d bytes", name ? "name-of" : "string-of", TEXT_FORM_BYTES);
		return 0;
	}
	nd = &g_nodes[n];
	if (name) {
		size_t i;
		NodeKind kind = at ? atom_kind(buf, at) : N_INT;
		for (i = 0; i < at && !is_delim((unsigned char)buf[i]); i++) {}
		if (!at || i < at || kind == N_INT || kind == N_FLOAT || (kind == N_SYM && like_number(buf, at))) {
			error_at(nd->line, nd->col, "name-of made \"%.64s\", which is not a name: a name has no spaces, parentheses, quotes or ;, and does not start like a number", buf);
			return 0;
		}
		nd->kind = (u8)kind;
		nd->stamp = scope >= 0 ? g_nodes[scope].stamp : g_nodes[head].stamp;
	} else {
		nd->kind = N_STR;
	}
	nd->text = arena_strndup(&g_unit_arena, buf, at);
	nd->len = (u32)at;
	nd->first = -1;
	nd->ival = 0;
	return 1;
}

/* text_forms ROOT: every (string-of ...) and (name-of ...) in the form ROOT worked out in place, the outer one first: a
 * string-of writes the parts it is given as they are, as # does; 0 after an error it reported */
static int text_forms(i32 root)
{
	static i32 st[MAX_MSTACK]; /* the lists being walked: what comes after each */
	i32 sp = 0, c = root, next, h;
	for (;;) {
		if (c < 0) {
			if (sp == 0) return 1;
			c = st[--sp];
			continue;
		}
		next = c == root ? -1 : g_nodes[c].next;
		if (g_nodes[c].kind == N_LIST && (h = g_nodes[c].first) >= 0) {
			const char *s = g_nodes[h].kind == N_SYM ? g_nodes[h].text : "";
			if (!strcmp(s, "string-of") || !strcmp(s, "name-of")) {
				if (!text_form(c)) return 0;
			} else if (strcmp(s, "defmacro")) { /* a defmacro's template is its own, for its own uses */
				if (sp == MAX_MSTACK) die("a form nested too deep (MAX_MSTACK)");
				st[sp++] = next;
				c = h;
				continue;
			}
		}
		c = next;
	}
}

/* macro_expand M CALL: the use CALL of macro M, rewritten by the first rule that matches it; -1 after an error it
 * reported */
static i32 macro_expand(i32 m, i32 call)
{
	const Macro *mc = &g_macros[m];
	i32 k, line = g_nodes[call].line, col = g_nodes[call].col;
	char list[600], one[200];
	size_t at = 0;
	if (++g_expansions > MAX_EXPANSIONS || g_nnodes > MAX_NODES - MAX_NODES / 8) {
		error_at(line, col, "macros expanded too often in this file: does %.64s expand into itself forever?", mc->name);
		return -1;
	}
	for (k = 0; k < mc->nrules; k++) {
		const Rule *r = &g_rules[mc->rule0 + k];
		if (match_rule(r, call)) {
			i32 x = expand_rule(r, call);
			return x >= 0 && !text_forms(x) ? -1 : x;
		}
	}
	list[0] = 0;
	for (k = 0; k < mc->nrules; k++) {
		mprint(g_rules[mc->rule0 + k].pattern, mc->name, one, sizeof(one));
		if (k) putn(list, sizeof(list), &at, k + 1 == mc->nrules ? " or " : ", ", k + 1 == mc->nrules ? 4 : 2);
		putn(list, sizeof(list), &at, one, strlen(one));
	}
	error_at(line, col, "no rule of %.64s matches this; it takes %s", mc->name, list);
	return -1;
}
