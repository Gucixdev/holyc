/* settings_tui.h - the Settings panel of the TUI (key S). Included by tui.h after the row helpers; one translation unit.
 *
 * Data comes from the schema (settings.c): tabs = schema tabs that have something to show, plus a first "Modules" tab that holds every
 * section's `enabled` switch. A module's own tab exists only while the module is enabled. Rows: checkbox for yes/no, value, source
 * (default / system / user / env), the one-line description and the details (default, range, flags) below the list.
 * Keys: j k arrows move, h l tab shift-tab 1-9 change tab, enter/space toggles or cycles or edits, - + step, e edit, d reset to default,
 * / search (all settings), r reload, esc/q/S close. The mouse works (SGR 1006): click a tab or a row, click the selected row again to act,
 * wheel scrolls. Every change is written at once to the user config file (atomic, comments kept) and applied live where it is safe;
 * the schema's `restart` settings say "restart needed" instead.
 */
#ifndef NM_SETTINGS_TUI_H
#define NM_SETTINGS_TUI_H

enum { NM_K_MOUSE = 0x140 };
typedef struct { int b, x, y, rel; } nm_mouse_ev;
static nm_mouse_ev nm__mouse;                        /* the last mouse event read by nm__read_key */

#define NM_SS_MAXTAB 24
#define NM_SS_MAXROW 96
typedef struct {
	netman_config c;                                /* working copy: what the files + environment say right now */
	char path[512];                                 /* the file edits go to */
	int tab, nvt, vt[NM_SS_MAXTAB];                 /* current visible tab (index into vt), visible tab ids (0 = Modules, 1+i = schema tab i) */
	int sel[NM_SS_MAXTAB + 1], top[NM_SS_MAXTAB + 1];
	int rows[NM_SS_MAXROW], nrows, dotted;
	char filter[64];
	int typing;                                     /* '/' prompt open */
	int editing, len; char buf[300];
	char msg[300]; int msgerr;
	int tx0[NM_SS_MAXTAB], tx1[NM_SS_MAXTAB], ntx, tfirst;   /* tab bar hit boxes (screen columns), first drawn tab */
	int lrow0, lrows;                               /* first screen row of the list and its height */
	int mouse_on;
} nm_sset;
static nm_sset nm__ss;
static void nm__apply_cfg(nm_ui *u);                /* defined next to the theme: pushes nm__ss.c into the running UI */

static const char *nm__ss_tabname(int id) { return id == 0 ? "Modules" : nset_tabname(id - 1); }

static int nm__ss_match(const char *hay, const char *needle) { return !*needle || nm__ci_has(hay, needle); }

/* does key k belong on tab id (0 = the Modules tab)? hidden = its module is off */
static int nm__ss_on_tab(const nm_sset *s, int k, int id)
{
	if (nset_hidden(&s->c, k)) return 0;
	if (id == 0) return nset_is_gate(k);
	return !nset_is_gate(k) && nset_tab_index(k) == id - 1;
}

static void nm__ss_tabs(nm_sset *s)
{
	int keep = s->nvt ? s->vt[s->tab] : 0, n = nset_ntabs();
	s->nvt = 0;
	for (int id = 0; id <= n && s->nvt < NM_SS_MAXTAB; id++)
		for (int k = 0; k < NC_N; k++) if (nm__ss_on_tab(s, k, id)) { s->vt[s->nvt++] = id; break; }
	s->tab = 0;
	for (int i = 0; i < s->nvt; i++) if (s->vt[i] == keep) s->tab = i;
}

static int nm__ss_slot(const nm_sset *s) { return s->filter[0] ? NM_SS_MAXTAB : s->tab; }   /* sel/top index: the search view has its own */

static void nm__ss_rows(nm_sset *s)
{
	int id = s->nvt ? s->vt[s->tab] : 0, nsec = 0;
	char first[32] = "";
	s->nrows = 0;
	for (int k = 0; k < NC_N && s->nrows < NM_SS_MAXROW; k++) {
		char full[96], hay[400];
		if (s->filter[0]) {
			if (nset_hidden(&s->c, k)) continue;
			netman_config_fullname(k, full, sizeof full);
			snprintf(hay, sizeof hay, "%s %s", full, netman_config_doc(k));
			if (!nm__ss_match(hay, s->filter)) continue;
		} else if (!nm__ss_on_tab(s, k, id)) continue;
		if (!first[0]) snprintf(first, sizeof first, "%s", netman_config_section(k));
		else if (strcmp(first, netman_config_section(k))) nsec = 1;
		s->rows[s->nrows++] = k;
	}
	s->dotted = s->filter[0] || id == 0 || nsec;
	{
		int sl = nm__ss_slot(s);
		if (s->sel[sl] >= s->nrows) s->sel[sl] = s->nrows - 1;
		if (s->sel[sl] < 0) s->sel[sl] = 0;
	}
}

static void nm__ss_msg(nm_sset *s, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->msg, sizeof s->msg, fmt, ap);
	va_end(ap);
	s->msgerr = err;
}

static void nm__ss_reload(nm_ui *u)
{
	nm_sset *s = &nm__ss;
	char err[400];
	if (netman_config_load(&s->c, err, sizeof err)) { nm__ss_msg(s, 1, "%s", err); return; }
	nm__apply_cfg(u);
	nm__ss_tabs(s);
	nm__ss_rows(s);
}

static void nm__ss_open(nm_ui *u)
{
	nm_sset *s = &nm__ss;
	memset(s, 0, sizeof *s);
	if (!netman_config_user_path(s->path, sizeof s->path)) snprintf(s->path, sizeof s->path, "(no HOME: cannot save)");
	{
		char err[400];
		if (netman_config_load(&s->c, err, sizeof err)) { netman_config_defaults(&s->c); nm__ss_msg(s, 1, "%s", err); }
	}
	nm__ss_tabs(s);
	nm__ss_rows(s);
	u->mode = NM_SETTINGS;
	if (isatty(1) && write(1, "\x1b[?1000h\x1b[?1006h", 16)) { s->mouse_on = 1; }
}

static void nm__ss_close(nm_ui *u)
{
	if (nm__ss.mouse_on && write(1, "\x1b[?1006l\x1b[?1000l", 16)) {}
	nm__ss.mouse_on = 0;
	u->mode = NM_NORMAL;
	if (!u->en[u->tab]) for (int i = NM_NTAB - 1; i >= 0; i--) if (u->en[i]) u->tab = i;
	u->msg[0] = 0;
	nm__clamp(u);
}

/* write KEY = VALUE; reload; say what happened */
static void nm__ss_apply(nm_ui *u, int k, const char *value)
{
	nm_sset *s = &nm__ss;
	char full[96], err[400], was[300];
	snprintf(was, sizeof was, "%s", netman_config_str(&s->c, k));
	netman_config_fullname(k, full, sizeof full);
	if (netman_config_set_file(s->path, full, value, err, sizeof err)) { nm__ss_msg(s, 1, "%s", err); return; }
	nm__ss_reload(u);
	if (!strcmp(nset_source(&s->c, k), "env")) nm__ss_msg(s, 0, "%s saved, but overridden by %s", full, s->c.origin[k] + 4);
	else if (nset_restart(k)) nm__ss_msg(s, 0, "%s saved: restart needed", full);
	else nm__ss_msg(s, 0, "%s = %s", full, netman_config_str(&s->c, k));
}

static void nm__ss_reset(nm_ui *u, int k)
{
	nm_sset *s = &nm__ss;
	char full[96], err[400];
	int rc;
	netman_config_fullname(k, full, sizeof full);
	rc = netman_config_unset_file(s->path, full, err, sizeof err);
	if (rc < 0) { nm__ss_msg(s, 1, "%s", err); return; }
	nm__ss_reload(u);
	if (rc) nm__ss_msg(s, 0, "%s is not set in %s", full, s->path);
	else if (!nset_is_default(&s->c, k)) nm__ss_msg(s, 0, "%s removed from the user file; still %s from %s", full, netman_config_str(&s->c, k), s->c.origin[k]);
	else nm__ss_msg(s, 0, "%s reset to the default (%s)", full, netman_config_default(k));
}

static int nm__ss_cur(const nm_sset *s) { int sl = nm__ss_slot(s); return s->nrows && s->sel[sl] < s->nrows ? s->rows[s->sel[sl]] : -1; }

/* enter / space / click on the selected row */
static void nm__ss_act(nm_ui *u, int dir)
{
	nm_sset *s = &nm__ss;
	int k = nm__ss_cur(s);
	if (k < 0) return;
	switch (nset_kind(k)) {
	case NSET_BOOL: nm__ss_apply(u, k, netman_config_bool(&s->c, k) ? "no" : "yes"); break;
	case NSET_ENUM: {
		char ch[40][24];
		int n = nset_choices(k, ch, 40), at = 0;
		for (int i = 0; i < n; i++) if (!strcmp(ch[i], netman_config_str(&s->c, k))) at = i;
		nm__ss_apply(u, k, ch[(at + dir + n) % n]);
		break;
	}
	default:
		s->editing = 1;
		snprintf(s->buf, sizeof s->buf, "%s", netman_config_str(&s->c, k));
		s->len = (int)strlen(s->buf);
		nm__ss_msg(s, 0, "editing: enter saves, esc cancels");
	}
}

static void nm__ss_step(nm_ui *u, int d)   /* + / - */
{
	nm_sset *s = &nm__ss;
	int k = nm__ss_cur(s);
	long lo, hi, v;
	char b[32];
	if (k < 0) return;
	if (nset_kind(k) == NSET_ENUM || nset_kind(k) == NSET_BOOL) { nm__ss_act(u, d); return; }
	if (!nset_range(k, &lo, &hi)) return;
	v = netman_config_int(&s->c, k) + d;
	if (v < lo || v > hi) { nm__ss_msg(s, 1, "%s must be from %ld to %ld", netman_config_name(k), lo, hi); return; }
	snprintf(b, sizeof b, "%ld", v);
	nm__ss_apply(u, k, b);
}

static void nm__ss_validate(nm_sset *s)   /* live message while typing */
{
	int k = nm__ss_cur(s);
	char norm[300], msg[300];
	if (k < 0) return;
	if (netman_config_check_value(k, s->buf, norm, sizeof norm, msg, sizeof msg)) nm__ss_msg(s, 1, "%s", msg);
	else nm__ss_msg(s, 0, "valid");
}

static void nm__ss_move(nm_sset *s, int d, int rows)
{
	int sl = nm__ss_slot(s);
	s->sel[sl] += d;
	if (s->sel[sl] >= s->nrows) s->sel[sl] = s->nrows - 1;
	if (s->sel[sl] < 0) s->sel[sl] = 0;
	if (s->sel[sl] < s->top[sl]) s->top[sl] = s->sel[sl];
	if (s->sel[sl] >= s->top[sl] + rows) s->top[sl] = s->sel[sl] - rows + 1;
	if (s->top[sl] < 0) s->top[sl] = 0;
	s->msg[0] = 0;
}

static int nm__ss_listrows(const nm_ui *u) { return u->h - 6 > 1 ? u->h - 6 : 1; }   /* rows 3 .. h-4 */

static void nm__ss_tab(nm_ui *u, int to)
{
	nm_sset *s = &nm__ss;
	if (!s->nvt) return;
	s->filter[0] = 0;
	s->tab = ((to % s->nvt) + s->nvt) % s->nvt;
	nm__ss_rows(s);
	nm__ss_move(s, 0, nm__ss_listrows(u));
}

static void nm__ss_mouse(nm_ui *u)
{
	nm_sset *s = &nm__ss;
	const nm_mouse_ev *m = &nm__mouse;
	int rows = nm__ss_listrows(u), sl = nm__ss_slot(s);
	if (m->b == 64 || m->b == 65) { nm__ss_move(s, m->b == 64 ? -3 : 3, rows); return; }
	if ((m->b & 3) != 0 || (m->b & 32) || (m->b & 64) || m->rel) return;   /* plain left press only */
	if (s->editing || s->typing) return;
	if (m->y == 2) {
		for (int i = 0; i < s->ntx; i++) if (m->x >= s->tx0[i] && m->x <= s->tx1[i]) { nm__ss_tab(u, s->tfirst + i); return; }
		return;
	}
	if (m->y >= s->lrow0 && m->y < s->lrow0 + s->lrows) {
		int idx = s->top[sl] + (m->y - s->lrow0);
		if (idx >= s->nrows) return;
		if (idx == s->sel[sl]) nm__ss_act(u, 1);
		else { s->sel[sl] = idx; s->msg[0] = 0; }
	}
}

static void nm_sset_key(nm_ui *u, int key)
{
	nm_sset *s = &nm__ss;
	int rows = nm__ss_listrows(u);
	if (key == NM_K_MOUSE) { nm__ss_mouse(u); return; }
	if (s->editing) {
		int k = nm__ss_cur(s);
		if (key == NM_K_ESC || key == 3) { s->editing = 0; nm__ss_msg(s, 0, "cancelled"); return; }
		if (key == NM_K_ENTER) {
			char norm[300], msg[300];
			if (k < 0 || netman_config_check_value(k, s->buf, norm, sizeof norm, msg, sizeof msg)) { nm__ss_msg(s, 1, "%s", k < 0 ? "nothing selected" : msg); return; }
			s->editing = 0;
			nm__ss_apply(u, k, s->buf);
			return;
		}
		if (key == NM_K_BS) { if (s->len > 0) { do s->len--; while (s->len > 0 && ((unsigned char)s->buf[s->len] & 0xC0) == 0x80); s->buf[s->len] = 0; } }
		else if (key >= 0x20 && key < 0x100 && s->len < (int)sizeof s->buf - 2) { s->buf[s->len++] = (char)key; s->buf[s->len] = 0; }
		nm__ss_validate(s);
		return;
	}
	if (s->typing) {
		if (key == NM_K_ESC) { s->typing = 0; s->filter[0] = 0; nm__ss_rows(s); return; }
		if (key == NM_K_ENTER) { s->typing = 0; return; }
		if (key == NM_K_BS) { size_t l = strlen(s->filter); if (l) s->filter[l - 1] = 0; }
		else if (key >= 0x20 && key < 0x7f && strlen(s->filter) + 1 < sizeof s->filter) { size_t l = strlen(s->filter); s->filter[l] = (char)key; s->filter[l + 1] = 0; }
		nm__ss_rows(s);
		nm__ss_move(s, 0, rows);
		return;
	}
	switch (key) {
	case 'q': case 'S': case 3: nm__ss_close(u); break;
	case NM_K_ESC: if (s->filter[0]) { s->filter[0] = 0; nm__ss_rows(s); } else nm__ss_close(u); break;
	case 'j': case NM_K_DOWN: nm__ss_move(s, 1, rows); break;
	case 'k': case NM_K_UP: nm__ss_move(s, -1, rows); break;
	case NM_K_PGDN: nm__ss_move(s, rows, rows); break;
	case NM_K_PGUP: nm__ss_move(s, -rows, rows); break;
	case 'g': case NM_K_HOME: nm__ss_move(s, -1000, rows); break;
	case 'G': case NM_K_END: nm__ss_move(s, 1000, rows); break;
	case 'l': case NM_K_RIGHT: case NM_K_TAB: nm__ss_tab(u, s->tab + 1); break;
	case 'h': case NM_K_LEFT: case NM_K_BTAB: nm__ss_tab(u, s->tab - 1); break;
	case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': if (key - '1' < s->nvt) nm__ss_tab(u, key - '1'); break;
	case NM_K_ENTER: case ' ': nm__ss_act(u, 1); break;
	case '+': case '=': nm__ss_step(u, 1); break;
	case '-': nm__ss_step(u, -1); break;
	case 'e': {
		int k = nm__ss_cur(s);
		if (k >= 0) { s->editing = 1; snprintf(s->buf, sizeof s->buf, "%s", netman_config_str(&s->c, k)); s->len = (int)strlen(s->buf); nm__ss_msg(s, 0, "editing: enter saves, esc cancels"); }
		break;
	}
	case 'd': { int k = nm__ss_cur(s); if (k >= 0) nm__ss_reset(u, k); break; }
	case '/': s->typing = 1; s->filter[0] = 0; nm__ss_rows(s); break;
	case 'r': nm__ss_reload(u); nm__ss_msg(s, 0, "reloaded"); break;
	default: break;
	}
}

static void nm_sset_render(const nm_ui *u, const nm_theme *t, FILE *o)
{
	nm_sset *s = &nm__ss;
	int w = u->w, h = u->h, rows = nm__ss_listrows(u), sl = nm__ss_slot(s), cur = nm__ss_cur(s), n = s->nrows;
	nm__Row r;
	/* clamp the scroll window for this size (the terminal may have been resized) */
	if (s->sel[sl] < s->top[sl]) s->top[sl] = s->sel[sl];
	if (s->sel[sl] >= s->top[sl] + rows) s->top[sl] = s->sel[sl] - rows + 1;
	if (s->top[sl] < 0) s->top[sl] = 0;
	/* row 1: title */
	nm__row_init(&r, w);
	nm__row_raw(&r, t->title);
	nm__row_add(&r, " netman settings", -1);
	nm__row_raw(&r, t->dim);
	if (w >= 50) { nm__row_add(&r, "  ", -1); nm__row_add(&r, s->path, w - r.col - 1); }
	nm__row_raw(&r, t->reset);
	nm__row_emit(&r, t, o, 1);
	/* row 2: tabs (scrolled so the current one is visible) or the search state */
	nm__row_init(&r, w);
	s->ntx = 0;
	if (s->filter[0] || s->typing) {
		nm__row_raw(&r, t->key);
		nm__row_fmt(&r, -1, " / %s%s", s->filter, s->typing ? "_" : "");
		nm__row_raw(&r, t->dim);
		nm__row_fmt(&r, -1, "   %d match%s in all settings (esc clears)", n, n == 1 ? "" : "es");
	} else {
		int first = 0, used;
		for (;;) {
			used = 0;
			for (int i = first; i <= s->tab; i++) used += nm__cells(nm__ss_tabname(s->vt[i])) + 3;
			if (used <= w - (first > 0 ? 3 : 0) - 3 || first >= s->tab) break;
			first++;
		}
		s->tfirst = first;
		if (first > 0) { nm__row_raw(&r, t->dim); nm__row_add(&r, "< ", -1); nm__row_raw(&r, t->reset); }
		for (int i = first; i < s->nvt; i++) {
			int cw = nm__cells(nm__ss_tabname(s->vt[i])) + 2;
			if (r.col + cw + (i + 1 < s->nvt ? 3 : 0) > w && i != s->tab) { nm__row_raw(&r, t->dim); nm__row_add(&r, ">", -1); nm__row_raw(&r, t->reset); break; }
			if (s->ntx < NM_SS_MAXTAB) { s->tx0[s->ntx] = r.col + 1; s->tx1[s->ntx] = r.col + cw; s->ntx++; }
			nm__row_raw(&r, i == s->tab ? t->tabsel : t->tab);
			nm__row_fmt(&r, -1, " %s ", nm__ss_tabname(s->vt[i]));
			nm__row_raw(&r, t->reset);
			nm__row_add(&r, " ", -1);
		}
	}
	nm__row_raw(&r, t->reset);
	nm__row_emit(&r, t, o, 2);
	/* the list */
	s->lrow0 = 3; s->lrows = rows;
	{
		int nw = w >= 70 ? 26 : w >= 50 ? 22 : 16, tagw = w >= 60 ? 8 : 2, scroll = n > rows, lw = scroll ? w - 1 : w;
		for (int i = 0; i < rows; i++) {
			int idx = s->top[sl] + i, row = 3 + i, sel_row = idx == s->sel[sl] && idx < n;
			nm__row_init(&r, lw);
			if (idx < n) {
				int k = s->rows[idx], isb = nset_kind(k) == NSET_BOOL, vw;
				char name[96], full[96], val[300], src[16];
				const char *vs = netman_config_str(&s->c, k), *sc = nset_source(&s->c, k);
				netman_config_fullname(k, full, sizeof full);
				nm__fit(name, sizeof name, s->dotted ? full : netman_config_name(k), nw, t->ell);
				if (sel_row) nm__row_raw(&r, t->sel);
				if (isb) nm__row_fmt(&r, -1, " [%c] ", !strcmp(vs, "yes") ? 'x' : ' ');
				else nm__row_add(&r, "     ", -1);
				nm__row_fmt(&r, -1, "%-*s ", nw, name);
				vw = lw - r.col - 1 - tagw;
				if (vw < 4) vw = 4;
				if (sel_row && s->editing) {
					char eb[320];
					snprintf(eb, sizeof eb, "%s_", s->buf);
					if (nm__cells(eb) > vw) { const char *p = eb; while (nm__cells(p) > vw && *p) { unsigned cp; p += nm__dec(p, &cp); } nm__row_add(&r, p, vw); }
					else nm__row_add(&r, eb, vw);
				} else {
					if (!sel_row) nm__row_raw(&r, isb ? (!strcmp(vs, "yes") ? t->ok : t->dim) : "");
					snprintf(val, sizeof val, "%s", *vs ? vs : "(empty)");
					nm__fit(val, sizeof val, val, vw, t->ell);
					nm__row_add(&r, val, vw);
					if (!sel_row) nm__row_raw(&r, t->reset);
				}
				nm__row_pad(&r, lw - 1 - tagw);
				if (sel_row) nm__row_raw(&r, t->sel);
				else nm__row_raw(&r, !strcmp(sc, "env") ? t->warn : !strcmp(sc, "default") ? t->dim : t->key);
				if (tagw > 2) snprintf(src, sizeof src, " %-7s", !strcmp(sc, "default") ? "" : sc);
				else snprintf(src, sizeof src, " %c", !strcmp(sc, "default") ? ' ' : sc[0]);
				nm__row_add(&r, src, tagw);
				if (sel_row) nm__row_pad(&r, lw);
			} else if (i == 0 && n == 0) {
				nm__row_raw(&r, t->dim);
				nm__row_add(&r, s->filter[0] ? "  no setting matches (esc clears the search)" : "  nothing to show", -1);
			}
			nm__row_emit(&r, t, o, row);
			if (scroll) {
				int th = rows * rows / n, off, pos;
				if (th < 1) th = 1;
				off = s->top[sl];
				pos = n > rows ? off * (rows - th) / (n - rows) : 0;
				fprintf(o, "\x1b[%d;%dH%s%s%s", row, w, t->dim, i >= pos && i < pos + th ? t->thumb : t->track, t->reset);
			}
		}
	}
	/* description + details of the selected setting */
	nm__row_init(&r, w);
	if (cur >= 0) {
		nm__row_raw(&r, t->reset);
		nm__row_fmt(&r, -1, " %s", netman_config_doc(cur));
	}
	nm__row_emit(&r, t, o, h - 3);
	nm__row_init(&r, w);
	if (cur >= 0) {
		char rng[64] = "", ch[40][24], list[200] = "";
		long lo, hi;
		int nch = nset_choices(cur, ch, 40);
		if (nset_range(cur, &lo, &hi)) snprintf(rng, sizeof rng, "  %ld..%ld", lo, hi);
		if (nset_kind(cur) == NSET_ENUM) for (int i = 0; i < nch; i++) { size_t l = strlen(list); snprintf(list + l, sizeof list - l, "%s%s", i ? "|" : "  ", ch[i]); }
		nm__row_raw(&r, t->dim);
		nm__row_fmt(&r, -1, " default %s%s%s", *netman_config_default(cur) ? netman_config_default(cur) : "(empty)", rng, list);
		if (strcmp(nset_source(&s->c, cur), "default")) nm__row_fmt(&r, -1, "  now from %s (%s)", nset_source(&s->c, cur), s->c.origin[cur]);
		if (nset_restart(cur)) { nm__row_raw(&r, t->warn); nm__row_add(&r, "  restart needed", -1); }
		if (nset_needs_root(cur)) { nm__row_raw(&r, t->dim); nm__row_add(&r, "  acts as root", -1); }
	}
	nm__row_emit(&r, t, o, h - 2);
	/* message / validation line */
	nm__row_init(&r, w);
	if (s->msg[0]) { nm__row_raw(&r, s->msgerr ? t->err : t->ok); nm__row_fmt(&r, -1, " %s", s->msg); }
	nm__row_emit(&r, t, o, h - 1);
	/* key bar */
	nm__row_init(&r, w);
	nm__row_add(&r, " ", 1);
	if (s->editing) { nm__kb(&r, t, "enter", "save"); nm__kb(&r, t, "esc", "cancel"); }
	else if (s->typing) { nm__kb(&r, t, "enter", "keep"); nm__kb(&r, t, "esc", "clear"); }
	else {
		static const nm__Key kk[] = { { "enter", "set" }, { "d", "default" }, { "/", "search" }, { "e", "edit" }, { "+-", "step" }, { "h/l", "tab" } };
		for (unsigned i = 0; i < sizeof kk / sizeof *kk; i++) {
			int need = nm__cells(kk[i].k) + 1 + nm__cells(kk[i].l) + 2;
			if (r.col + need + 9 > w) break;
			nm__kb(&r, t, kk[i].k, kk[i].l);
		}
		nm__kb(&r, t, "esc", "close");
	}
	nm__row_emit(&r, t, o, h);
}
#endif
