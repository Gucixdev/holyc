/* proctable.c - lt_proctable: a process / task table with a header, sortable columns, a process tree (rows with a parent nest under it),
 * a cursor, and scrolling. Columns: NAME (takes the free width), PID, CPU%, MEM (bytes, shown human), DISK (bytes/s, shown human).
 * Ported in spirit from gtop's draw_proc (grouped rows with "├── child [pid]" kids). The widget only DISPLAYS rows you hand it with
 * lt_proctable_set (a copy); collecting them (/proc, ps, ...) is the caller's job. Selection survives a re-set (kept by pid).
 * Keys: Up/Down, PgUp/PgDn, Home/End move; s cycles the sort column, r reverses it; Right/Enter/Space expand a node (Left collapses);
 *   Enter on a leaf = ACTIVATE. Mouse: click a header = sort by it (again = reverse), click a row = select, double click = ACTIVATE,
 *   wheel = scroll. Siblings are sorted by the active column; children stay under their parent.
 * Needs: internal.h (the lib core). */
#include "internal.h"
#include <stdio.h>

typedef struct { char *name; int pid, parent; double cpu, mem, disk; bool open; } prow;
typedef struct { int idx, depth; } vis;

struct lt_proctable {
	prow *rows; int n;
	vis *v; int nv;
	lt_proc_col col; bool desc;
	int cursor, offset;              /* cursor = index into v */
	int sel_pid;                     /* kept across sets */
	lt_ptr ptr;
	int last_body;                   /* body rows of the last draw (for paging) */
};

lt_proctable *lt_proctable_new(void)
{
	lt_proctable *t = calloc(1, sizeof *t);
	if (t) { t->col = LT_PROC_CPU; t->desc = true; t->sel_pid = -1; t->last_body = 10; }
	return t;
}
static void free_rows(lt_proctable *t) { for (int i = 0; i < t->n; i++) free(t->rows[i].name); free(t->rows); t->rows = NULL; t->n = 0; }
void lt_proctable_free(lt_proctable *t) { if (!t) return; free_rows(t); free(t->v); free(t); }

static const prow *g_rows; static lt_proc_col g_col; static bool g_desc;
static int cmp(const void *a, const void *b)
{
	const prow *x = &g_rows[*(const int *)a], *y = &g_rows[*(const int *)b];
	double d = 0;
	switch (g_col) {
	case LT_PROC_NAME: d = strcmp(x->name, y->name); break;
	case LT_PROC_PID: d = x->pid - y->pid; break;
	case LT_PROC_CPU: d = x->cpu - y->cpu; break;
	case LT_PROC_MEM: d = x->mem - y->mem; break;
	case LT_PROC_DISK: d = x->disk - y->disk; break;
	default: break;
	}
	if (g_desc) d = -d;
	if (d != 0) return d < 0 ? -1 : 1;
	return *(const int *)a - *(const int *)b;               /* stable */
}
static void emit(lt_proctable *t, const int *ord, char *seen, int idx, int depth)
{
	if (seen[idx] || depth > 32) return;
	seen[idx] = 1;
	t->v[t->nv++] = (vis){idx, depth};
	if (!t->rows[idx].open) return;
	for (int k = 0; k < t->n; k++) if (t->rows[ord[k]].parent == idx) emit(t, ord, seen, ord[k], depth + 1);
}
static void rebuild(lt_proctable *t)
{
	int keep = t->sel_pid;
	free(t->v); t->v = NULL; t->nv = 0;
	if (t->n == 0) { t->cursor = t->offset = 0; return; }
	int *ord = malloc(sizeof *ord * t->n);
	char *seen = calloc(t->n, 1);
	t->v = malloc(sizeof *t->v * t->n);
	if (!ord || !seen || !t->v) { free(ord); free(seen); free(t->v); t->v = NULL; return; }
	for (int i = 0; i < t->n; i++) ord[i] = i;
	g_rows = t->rows; g_col = t->col; g_desc = t->desc;
	qsort(ord, t->n, sizeof *ord, cmp);
	for (int k = 0; k < t->n; k++) {
		int i = ord[k], p = t->rows[i].parent;
		if (p < 0 || p >= t->n || p == i) emit(t, ord, seen, i, 0);
	}
	for (int k = 0; k < t->n; k++) {                          /* rows in a parent cycle are reachable from no root: show them as roots */
		int i = ord[k], hops = 0;
		if (seen[i]) continue;
		while (hops++ <= t->n && i >= 0 && i < t->n && t->rows[i].parent >= 0 && t->rows[i].parent < t->n && t->rows[i].parent != i) i = t->rows[i].parent;
		if (hops > t->n) emit(t, ord, seen, ord[k], 0);          /* walked in a circle */
	}
	free(ord); free(seen);
	t->cursor = 0;
	for (int i = 0; i < t->nv; i++) if (t->rows[t->v[i].idx].pid == keep) { t->cursor = i; break; }
}

void lt_proctable_set(lt_proctable *t, const lt_proc *rows, int n)
{
	if (n < 0 || (n > 0 && !rows)) n = 0;
	/* remember which pids were expanded, so a refresh does not collapse the tree */
	int *open_pids = malloc(sizeof(int) * (t->n + 1)), no = 0;
	if (open_pids) for (int i = 0; i < t->n; i++) if (t->rows[i].open) open_pids[no++] = t->rows[i].pid;
	free_rows(t);
	t->rows = calloc(n ? n : 1, sizeof *t->rows);
	if (!t->rows) { free(open_pids); return; }
	for (int i = 0; i < n; i++) {
		prow *p = &t->rows[i];
		p->name = strdup(rows[i].name ? rows[i].name : "");
		p->pid = rows[i].pid; p->parent = rows[i].parent;
		p->cpu = lt_finite_or(rows[i].cpu, 0); p->mem = lt_finite_or(rows[i].mem, 0); p->disk = lt_finite_or(rows[i].disk, 0);
		p->open = rows[i].expanded;
		for (int k = 0; k < no; k++) if (open_pids[k] == p->pid) p->open = true;
	}
	t->n = n;
	free(open_pids);
	rebuild(t);
}

void lt_proctable_set_sort(lt_proctable *t, lt_proc_col col, bool desc) { t->col = col; t->desc = desc; rebuild(t); }
lt_proc_col lt_proctable_sort_col(const lt_proctable *t) { return t->col; }
bool lt_proctable_sort_desc(const lt_proctable *t) { return t->desc; }
int lt_proctable_count(const lt_proctable *t) { return t->nv; }
int lt_proctable_selected(const lt_proctable *t) { return t->nv ? t->v[t->cursor].idx : -1; }
int lt_proctable_selected_pid(const lt_proctable *t) { return t->nv ? t->rows[t->v[t->cursor].idx].pid : -1; }
const char *lt_proctable_row_name(const lt_proctable *t, int visible)
{
	return visible >= 0 && visible < t->nv ? t->rows[t->v[visible].idx].name : NULL;
}

/* column geometry shared by draw and event: NAME takes the rest */
typedef struct { int name_x, name_w, pid_x, cpu_x, mem_x, disk_x; } cols;
static cols layout(lt_rect r, int total)
{
	(void)total;
	cols c = {0};
	int fixed = 7 + 7 + 10 + 11;
	if (r.w < fixed + 8) fixed = 7 + 7;                      /* narrow: name + pid + cpu only */
	int right = r.x + r.w;
	c.disk_x = fixed >= 7 + 7 + 10 + 11 ? right - 11 : -1;
	c.mem_x = fixed >= 7 + 7 + 10 + 11 ? right - 21 : -1;
	c.cpu_x = fixed >= 7 + 7 + 10 + 11 ? right - 28 : right - 7;
	c.pid_x = c.cpu_x - 7;
	c.name_x = r.x; c.name_w = c.pid_x - r.x - 1;
	if (c.name_w < 1) c.name_w = 1;
	return c;
}

static void toggle(lt_proctable *t, int vi, int want)    /* want 1 open, 0 close, -1 toggle */
{
	prow *p = &t->rows[t->v[vi].idx];
	bool has = false;
	for (int k = 0; k < t->n; k++) if (t->rows[k].parent == t->v[vi].idx) { has = true; break; }
	if (!has) return;
	p->open = want < 0 ? !p->open : want == 1;
	t->sel_pid = p->pid;
	rebuild(t);
}

void lt_proctable_draw(lt_proctable *t, lt_canvas *c, lt_rect r, const lt_theme *th, bool focused)
{
	lt_rect all = lt_full(c);
	if (r.w < 8 || r.h < 1) return;
	int body = r.h - 1;
	t->last_body = body > 0 ? body : 1;
	lt_rect rr = r;
	if (t->nv > body) rr.w--;                                 /* the scrollbar column */
	cols g = layout(rr, t->nv);
	lt_style hd = lt_slot_style(th, LT_T_ACCENT), tx = lt_slot_style(th, LT_T_TEXT), dim = lt_slot_style(th, LT_T_DIM);
	lt_fill(c, (lt_rect){r.x, r.y, r.w, 1}, ' ', hd);
	static const char *nm[] = {"NAME", "PID", "CPU%", "MEM", "DISK"};
	int hx[5] = {g.name_x, g.pid_x, g.cpu_x, g.mem_x, g.disk_x};
	for (int k = 0; k < 5; k++) {
		if (hx[k] < 0) continue;
		char h[32];
		const char *arrow = t->col == (lt_proc_col)k ? (t->desc ? "\xE2\x96\xBE" : "\xE2\x96\xB4") : "";
		if (k) snprintf(h, sizeof h, "%*s%s", 6, nm[k], arrow); else snprintf(h, sizeof h, "%s%s", nm[k], arrow);
		lt_text(c, all, hx[k], r.y, h, hd, k ? 7 : g.name_w);
	}
	if (body < 1) return;
	t->offset = lt_scroll_to(t->offset, t->cursor, body, t->nv);
	lt_rect view = {r.x, r.y + 1, r.w, body};
	lt_rect content = {r.x, r.y + 1, rr.w, body};
	(void)view;
	for (int row = 0; row < body; row++) {
		int vi = t->offset + row;
		if (vi >= t->nv) break;
		const prow *p = &t->rows[t->v[vi].idx];
		int y = r.y + 1 + row, depth = t->v[vi].depth;
		bool sel = vi == t->cursor;
		lt_style st = sel ? lt_slot_style(th, LT_T_SELECT) : lt_slot_style(th, lt_load_slot(p->cpu));
		if (!sel) st = tx, st.fg = lt_slot_style(th, lt_load_slot(p->cpu)).fg;
		if (sel && !focused) st = lt_slot_style(th, LT_T_ACCENT);
		if (sel) lt_fill(c, (lt_rect){content.x, y, content.w, 1}, ' ', st);
		bool has = false;
		for (int k = 0; k < t->n; k++) if (t->rows[k].parent == t->v[vi].idx) { has = true; break; }
		char label[200];
		bool uni = lt_canvas_caps(c)->unicode;
		snprintf(label, sizeof label, "%*s%s%s", depth * 2, "", has ? (p->open ? (uni ? "\xE2\x96\xBE " : "v ") : (uni ? "\xE2\x96\xB8 " : "> ")) : (depth ? (uni ? "\xE2\x94\x94 " : "` ") : "  "), p->name);
		lt_text(c, all, g.name_x, y, label, st, g.name_w);
		char b[40], m[40], d[40];
		snprintf(b, sizeof b, "%6d", p->pid); lt_text(c, all, g.pid_x, y, b, sel ? st : dim, 6);
		snprintf(b, sizeof b, "%5.1f%%", p->cpu); lt_text(c, all, g.cpu_x, y, b, st, 6);
		if (g.mem_x >= 0) {
			lt_human_bytes(p->mem, m, sizeof m); snprintf(b, sizeof b, "%9s", m); lt_text(c, all, g.mem_x, y, b, st, 9);
			lt_human_rate(p->disk, d, sizeof d); snprintf(b, sizeof b, "%10s", d); lt_text(c, all, g.disk_x, y, b, st, 10);
		}
	}
	if (t->nv > body) {                                       /* scrollbar column */
		int x = r.x + r.w - 1;
		int th_h = body * body / t->nv; if (th_h < 1) th_h = 1;
		int top = (int)((long)(body - th_h) * t->offset / (t->nv - body));
		for (int i = 0; i < body; i++) lt_put(c, all, x, r.y + 1 + i, (i >= top && i < top + th_h) ? 0x2503 : 0x2502, dim);
	}
}

lt_wres lt_proctable_event(lt_proctable *t, lt_rect r, const lt_event *ev)
{
	if (t->nv == 0 && ev->kind == LT_EV_KEY) return LT_W_IGNORED;
	int body = r.h - 1 > 0 ? r.h - 1 : 1;
	int old = t->cursor;
	if (ev->kind == LT_EV_KEY) {
		switch (ev->key) {
		case LT_K_UP: case 'k': t->cursor = lt_clampi(t->cursor - 1, 0, t->nv - 1); break;
		case LT_K_DOWN: case 'j': t->cursor = lt_clampi(t->cursor + 1, 0, t->nv - 1); break;
		case LT_K_PGUP: t->cursor = lt_clampi(t->cursor - body, 0, t->nv - 1); break;
		case LT_K_PGDN: t->cursor = lt_clampi(t->cursor + body, 0, t->nv - 1); break;
		case LT_K_HOME: t->cursor = 0; break;
		case LT_K_END: t->cursor = t->nv - 1; break;
		case 's': t->col = (lt_proc_col)((t->col + 1) % LT_PROC_COLS); t->sel_pid = lt_proctable_selected_pid(t); rebuild(t); return LT_W_CHANGED;
		case 'r': t->desc = !t->desc; t->sel_pid = lt_proctable_selected_pid(t); rebuild(t); return LT_W_CHANGED;
		case LT_K_RIGHT: case ' ': case LT_K_ENTER: {
			int before = t->nv;
			toggle(t, t->cursor, ev->key == LT_K_RIGHT ? 1 : -1);
			if (t->nv != before) return LT_W_CHANGED;
			return ev->key == LT_K_ENTER ? LT_W_ACTIVATE : LT_W_HANDLED;
		}
		case LT_K_LEFT: { int b = t->nv; toggle(t, t->cursor, 0); return t->nv != b ? LT_W_CHANGED : LT_W_HANDLED; }
		default: return LT_W_IGNORED;
		}
		t->sel_pid = lt_proctable_selected_pid(t);
		return t->cursor != old ? LT_W_CHANGED : LT_W_HANDLED;
	}
	if (ev->kind == LT_EV_MOUSE) {
		lt_rect view = {r.x, r.y + 1, r.w, body};
		if (ev->key == LT_K_WHEEL_UP || ev->key == LT_K_WHEEL_DOWN) {
			if (!lt_in(r, ev->x, ev->y)) return LT_W_IGNORED;
			t->cursor = lt_clampi(t->cursor + (ev->key == LT_K_WHEEL_UP ? -3 : 3), 0, t->nv - 1);
			t->sel_pid = lt_proctable_selected_pid(t);
			return LT_W_CHANGED;
		}
		if (ev->key != LT_K_CLICK && ev->key != LT_K_DCLICK) return LT_W_IGNORED;
		if (ev->y == r.y && ev->x >= r.x && ev->x < r.x + r.w) {          /* header: sort by the column under the pointer */
			lt_rect rr = r;
			if (t->nv > body) rr.w--;
			cols g = layout(rr, t->nv);
			int hx[5] = {g.name_x, g.pid_x, g.cpu_x, g.mem_x, g.disk_x}, hit = 0;
			for (int k = 0; k < 5; k++) if (hx[k] >= 0 && ev->x >= hx[k]) hit = k;
			if ((lt_proc_col)hit == t->col) t->desc = !t->desc; else { t->col = (lt_proc_col)hit; t->desc = hit != 0; }
			t->sel_pid = lt_proctable_selected_pid(t);
			rebuild(t);
			return LT_W_CHANGED;
		}
		if (!lt_in(view, ev->x, ev->y)) return LT_W_IGNORED;
		int vi = t->offset + (ev->y - view.y);
		if (vi >= t->nv) return LT_W_HANDLED;
		t->cursor = vi;
		t->sel_pid = lt_proctable_selected_pid(t);
		return (ev->key == LT_K_DCLICK || lt_ptr_double(&t->ptr, vi)) ? LT_W_ACTIVATE : (vi != old ? LT_W_CHANGED : LT_W_HANDLED);
	}
	return LT_W_IGNORED;
}
