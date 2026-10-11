/* solver.c - the Cassowary algorithm (Badros, Borning, Stuckey): an incremental simplex over linear constraints
 * with strengths, plus edit variables so that changing one input (the terminal size) re-solves with a few dual
 * pivots. The structure follows the well known kiwi solver: symbols (external/slack/error/dummy), rows keyed by
 * their basic symbol, an objective row that sums the weighted errors.
 *
 * Problem sizes here are a few dozen variables, so rows are small arrays with linear search: simple and fast enough.
 * Removing constraints is not supported (a layout is rebuilt instead when its structure changes). */
#include "lazytui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define EPS 1e-8

typedef enum { S_INVALID = 0, S_EXTERNAL, S_SLACK, S_ERROR, S_DUMMY } stype;
typedef struct { int sym; double c; } cell;
typedef struct { cell *v; int n, cap; double constant; } row;
typedef struct { int marker, other; double constant; } edit;

struct lt_solver {
	int nsym, capsym;
	unsigned char *type;            /* per symbol id */
	row **rows;                     /* per symbol id: the row it is basic in, or NULL */
	row objective;
	row *artificial;
	int *infeasible; int ninf, capinf;
	edit *edits; int *editvar; int nedit, capedit;   /* edits[i] belongs to variable editvar[i] */
	bool broken;
};

static bool near_zero(double v) { return fabs(v) < EPS; }

/* ------------------------------------------------------------------ rows */
static void row_init(row *r) { r->v = NULL; r->n = r->cap = 0; r->constant = 0; }
static void row_free(row *r) { free(r->v); r->v = NULL; r->n = r->cap = 0; }
static void row_copy(row *d, const row *s)
{
	d->n = s->n; d->cap = s->n; d->constant = s->constant;
	d->v = s->n ? malloc((size_t)s->n * sizeof(cell)) : NULL;
	if (s->n) memcpy(d->v, s->v, (size_t)s->n * sizeof(cell));
}
static int row_find(const row *r, int sym) { for (int i = 0; i < r->n; i++) if (r->v[i].sym == sym) return i; return -1; }
static double row_coeff(const row *r, int sym) { int i = row_find(r, sym); return i < 0 ? 0 : r->v[i].c; }
static void row_remove_at(row *r, int i) { r->v[i] = r->v[--r->n]; }
static void row_insert_sym(row *r, int sym, double c)
{
	int i = row_find(r, sym);
	if (i >= 0) { r->v[i].c += c; if (near_zero(r->v[i].c)) row_remove_at(r, i); return; }
	if (near_zero(c)) return;
	if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 8; r->v = realloc(r->v, (size_t)r->cap * sizeof(cell)); }
	r->v[r->n++] = (cell){sym, c};
}
static void row_insert_row(row *r, const row *o, double k)
{
	r->constant += o->constant * k;
	for (int i = 0; i < o->n; i++) row_insert_sym(r, o->v[i].sym, o->v[i].c * k);
}
static void row_remove_sym(row *r, int sym) { int i = row_find(r, sym); if (i >= 0) row_remove_at(r, i); }
static void row_reverse(row *r) { r->constant = -r->constant; for (int i = 0; i < r->n; i++) r->v[i].c = -r->v[i].c; }
static void row_solve_sym(row *r, int sym)          /* the row says 0 = ... + c*sym: make it sym = ... */
{
	int i = row_find(r, sym);
	double k = -1.0 / r->v[i].c;
	row_remove_at(r, i);
	r->constant *= k;
	for (int j = 0; j < r->n; j++) r->v[j].c *= k;
}
static void row_solve_for(row *r, int lhs, int rhs) { row_insert_sym(r, lhs, -1.0); row_solve_sym(r, rhs); }
static void row_substitute(row *r, int sym, const row *o)
{
	int i = row_find(r, sym);
	if (i < 0) return;
	double k = r->v[i].c;
	row_remove_at(r, i);
	row_insert_row(r, o, k);
}

/* ------------------------------------------------------------------ solver */
lt_solver *lt_solver_new(void)
{
	lt_solver *s = calloc(1, sizeof *s);
	row_init(&s->objective);
	s->nsym = 0; s->capsym = 64;
	s->type = calloc((size_t)s->capsym, 1);
	s->rows = calloc((size_t)s->capsym, sizeof(row *));
	return s;
}

void lt_solver_free(lt_solver *s)
{
	if (!s) return;
	for (int i = 0; i <= s->nsym; i++) if (s->rows[i]) { row_free(s->rows[i]); free(s->rows[i]); }
	row_free(&s->objective);
	free(s->type); free(s->rows); free(s->infeasible); free(s->edits); free(s->editvar);
	free(s);
}

static int sym_new(lt_solver *s, stype t)
{
	if (s->nsym + 2 > s->capsym) {
		int nc = s->capsym * 2;
		s->type = realloc(s->type, (size_t)nc);
		s->rows = realloc(s->rows, (size_t)nc * sizeof(row *));
		memset(s->type + s->capsym, 0, (size_t)(nc - s->capsym));
		memset(s->rows + s->capsym, 0, (size_t)(nc - s->capsym) * sizeof(row *));
		s->capsym = nc;
	}
	s->type[++s->nsym] = (unsigned char)t;
	return s->nsym;
}
lt_var lt_var_new(lt_solver *s) { return sym_new(s, S_EXTERNAL); }

static void inf_push(lt_solver *s, int sym)
{
	if (s->ninf == s->capinf) { s->capinf = s->capinf ? s->capinf * 2 : 16; s->infeasible = realloc(s->infeasible, (size_t)s->capinf * sizeof(int)); }
	s->infeasible[s->ninf++] = sym;
}

/* replaces sym by row everywhere; rows whose basic symbol is restricted and went negative are queued for the dual pass */
static void substitute(lt_solver *s, int sym, const row *r)
{
	for (int i = 1; i <= s->nsym; i++) {
		row *o = s->rows[i];
		if (!o) continue;
		row_substitute(o, sym, r);
		if (s->type[i] != S_EXTERNAL && o->constant < 0) inf_push(s, i);
	}
	row_substitute(&s->objective, sym, r);
	if (s->artificial) row_substitute(s->artificial, sym, r);
}

static int entering_symbol(const row *obj, const lt_solver *s)
{
	for (int i = 0; i < obj->n; i++)
		if (s->type[obj->v[i].sym] != S_DUMMY && obj->v[i].c < 0) return obj->v[i].sym;
	return 0;
}
static int dual_entering(const lt_solver *s, const row *r)
{
	int best = 0;
	double ratio = 1e300;
	for (int i = 0; i < r->n; i++) {
		if (r->v[i].c > 0 && s->type[r->v[i].sym] != S_DUMMY) {
			double q = row_coeff(&s->objective, r->v[i].sym) / r->v[i].c;
			if (q < ratio) { ratio = q; best = r->v[i].sym; }
		}
	}
	return best;
}
static int leaving_row(const lt_solver *s, int entering)
{
	double ratio = 1e300;
	int found = 0;
	for (int i = 1; i <= s->nsym; i++) {
		const row *r = s->rows[i];
		if (!r || s->type[i] == S_EXTERNAL) continue;
		double c = row_coeff(r, entering);
		if (c >= 0) continue;
		double q = -r->constant / c;
		if (q < ratio) { ratio = q; found = i; }
	}
	return found;
}

static bool optimize(lt_solver *s, row *obj)
{
	for (int guard = 0; guard < 10000; guard++) {
		int enter = entering_symbol(obj, s);
		if (!enter) return true;
		int leave = leaving_row(s, enter);
		if (!leave) return false;                       /* unbounded: cannot happen with error variables */
		row *r = s->rows[leave];
		s->rows[leave] = NULL;
		row_solve_for(r, leave, enter);
		substitute(s, enter, r);
		s->rows[enter] = r;
	}
	return false;
}

static bool dual_optimize(lt_solver *s)
{
	while (s->ninf > 0) {
		int leave = s->infeasible[--s->ninf];
		row *r = s->rows[leave];
		if (r && r->constant < 0) {
			int enter = dual_entering(s, r);
			if (!enter) return false;
			s->rows[leave] = NULL;
			row_solve_for(r, leave, enter);
			substitute(s, enter, r);
			s->rows[enter] = r;
		}
	}
	return true;
}

static bool all_dummies(const lt_solver *s, const row *r)
{
	for (int i = 0; i < r->n; i++) if (s->type[r->v[i].sym] != S_DUMMY) return false;
	return true;
}

static int choose_subject(const lt_solver *s, const row *r, int marker, int other)
{
	for (int i = 0; i < r->n; i++) if (s->type[r->v[i].sym] == S_EXTERNAL) return r->v[i].sym;
	if (marker && (s->type[marker] == S_SLACK || s->type[marker] == S_ERROR) && row_coeff(r, marker) < 0) return marker;
	if (other && (s->type[other] == S_SLACK || s->type[other] == S_ERROR) && row_coeff(r, other) < 0) return other;
	return 0;
}

static bool add_with_artificial(lt_solver *s, const row *r)
{
	int art = sym_new(s, S_SLACK);
	row *copy = malloc(sizeof *copy);
	row_copy(copy, r);
	s->rows[art] = copy;
	row *a = malloc(sizeof *a);
	row_copy(a, r);
	s->artificial = a;
	optimize(s, a);
	bool ok = near_zero(a->constant);
	row_free(a); free(a);
	s->artificial = NULL;
	row *basic = s->rows[art];
	if (basic) {
		s->rows[art] = NULL;
		if (basic->n == 0) { row_free(basic); free(basic); return ok; }
		int enter = 0;
		for (int i = 0; i < basic->n; i++) if (s->type[basic->v[i].sym] == S_SLACK || s->type[basic->v[i].sym] == S_ERROR) { enter = basic->v[i].sym; break; }
		if (!enter) { row_free(basic); free(basic); return false; }
		row_solve_for(basic, art, enter);
		substitute(s, enter, basic);
		s->rows[enter] = basic;
	}
	for (int i = 1; i <= s->nsym; i++) if (s->rows[i]) row_remove_sym(s->rows[i], art);
	row_remove_sym(&s->objective, art);
	return ok;
}

/* builds the row of the constraint; returns marker/other symbols through the out parameters */
static void create_row(lt_solver *s, row *r, const lt_term *t, int n, double constant, lt_op op, double strength, int *marker, int *other)
{
	row_init(r);
	r->constant = constant;
	for (int i = 0; i < n; i++) {
		if (near_zero(t[i].coeff)) continue;
		int sym = t[i].var;
		if (s->rows[sym]) row_insert_row(r, s->rows[sym], t[i].coeff);
		else row_insert_sym(r, sym, t[i].coeff);
	}
	*marker = *other = 0;
	if (op == LT_LE || op == LT_GE) {
		double k = op == LT_LE ? 1.0 : -1.0;
		int slack = sym_new(s, S_SLACK);
		*marker = slack;
		row_insert_sym(r, slack, k);
		if (strength < LT_REQUIRED) {
			int err = sym_new(s, S_ERROR);
			*other = err;
			row_insert_sym(r, err, -k);
			row_insert_sym(&s->objective, err, strength);
		}
	} else if (strength < LT_REQUIRED) {
		int ep = sym_new(s, S_ERROR), em = sym_new(s, S_ERROR);
		*marker = ep; *other = em;
		row_insert_sym(r, ep, -1.0);
		row_insert_sym(r, em, 1.0);
		row_insert_sym(&s->objective, ep, strength);
		row_insert_sym(&s->objective, em, strength);
	} else {
		int d = sym_new(s, S_DUMMY);
		*marker = d;
		row_insert_sym(r, d, 1.0);        /* the dummy only tags the constraint; it can never enter the basis */
	}
	if (r->constant < 0) row_reverse(r);
}

static bool add_constraint(lt_solver *s, const lt_term *t, int n, double constant, lt_op op, double strength, int *marker_out, int *other_out)
{
	if (s->broken) return false;
	row r;
	int marker, other;
	create_row(s, &r, t, n, constant, op, strength, &marker, &other);
	if (marker_out) *marker_out = marker;
	if (other_out) *other_out = other;
	int subject = choose_subject(s, &r, marker, other);
	if (!subject && all_dummies(s, &r)) {
		if (!near_zero(r.constant)) { row_free(&r); s->broken = true; return false; }
		subject = marker;
	}
	if (!subject) {
		bool ok = add_with_artificial(s, &r);
		row_free(&r);
		if (!ok) { s->broken = true; return false; }
	} else {
		row *nr = malloc(sizeof *nr);
		*nr = r;
		row_solve_sym(nr, subject);
		substitute(s, subject, nr);
		s->rows[subject] = nr;
	}
	if (!optimize(s, &s->objective)) { s->broken = true; return false; }
	return true;
}

bool lt_solver_add(lt_solver *s, const lt_term *terms, int n, double constant, lt_op op, double strength)
{
	return add_constraint(s, terms, n, constant, op, strength, NULL, NULL);
}

bool lt_solver_edit(lt_solver *s, lt_var v, double strength)
{
	if (strength >= LT_REQUIRED) return false;
	for (int i = 0; i < s->nedit; i++) if (s->editvar[i] == v) return true;
	lt_term t = {v, 1.0};
	int marker, other;
	if (!add_constraint(s, &t, 1, 0, LT_EQ, strength, &marker, &other)) return false;
	if (s->nedit == s->capedit) {
		s->capedit = s->capedit ? s->capedit * 2 : 8;
		s->edits = realloc(s->edits, (size_t)s->capedit * sizeof(edit));
		s->editvar = realloc(s->editvar, (size_t)s->capedit * sizeof(int));
	}
	s->edits[s->nedit] = (edit){marker, other, 0};
	s->editvar[s->nedit++] = v;
	return true;
}

bool lt_solver_suggest(lt_solver *s, lt_var v, double value)
{
	if (s->broken) return false;
	int i;
	for (i = 0; i < s->nedit; i++) if (s->editvar[i] == v) break;
	if (i == s->nedit) return false;
	edit *e = &s->edits[i];
	double delta = value - e->constant;
	e->constant = value;
	row *r = s->rows[e->marker];
	if (r) {
		r->constant -= delta;
		if (r->constant < 0) inf_push(s, e->marker);
	} else if ((r = s->rows[e->other]) != NULL) {
		r->constant += delta;
		if (r->constant < 0) inf_push(s, e->other);
	} else {
		for (int k = 1; k <= s->nsym; k++) {
			row *o = s->rows[k];
			if (!o) continue;
			double c = row_coeff(o, e->marker);
			if (c != 0) {
				o->constant += delta * c;
				if (s->type[k] != S_EXTERNAL && o->constant < 0) inf_push(s, k);
			}
		}
	}
	return dual_optimize(s);
}

double lt_solver_value(const lt_solver *s, lt_var v)
{
	if (v <= 0 || v > s->nsym) return 0;
	const row *r = s->rows[v];
	return r ? r->constant : 0;
}
