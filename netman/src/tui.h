/* tui.h - the netman terminal UI. Included by netman.c after the mods, cli.h and help.c (one translation unit); tests include netman.c
 * with NETMAN_NO_MAIN and call the plain functions (parsers, nm_ui_render, nm_ui_key) without a terminal.
 *
 * Tabs: Wi-Fi | Ethernet/IP | Bluetooth | VPN (a tab of a module switched off in the config is hidden).
 *   Wi-Fi      netman_wifi_scan() rows (TSV): hidden networks as "<hidden> BSSID", lock glyph + security tag, signal as dBm and percent.
 *   Ethernet   netman_list() + netman_metrics_poll() in this process: address, gateway, link speed, live rx/tx.
 *   Bluetooth  netman_bt_list() text.      VPN  netman_vpn_list() text, enter = up/down.
 * Every call that can block (scan, connect, pair, vpn up) runs in a forked worker that ships its captured output back over a pipe, so the
 * spinner keeps turning and esc cancels. Colours and glyphs come only from nm_theme.
 */
#ifndef NM_TUI_H
#define NM_TUI_H

#include <ctype.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>

#define NM_MAXNET 128
#define NM_MAXDEV 64
#define NM_MAXVPN 32
#define NM_MAXDL 64

typedef struct { char name[96], sec[8], bssid[18], band[4]; int dbm, pct, chan, nbss, hidden, connected, known, autoc; } nm_net;   /* autoc: -1 unknown, 0 off, 1 on; dbm 0 = unknown */
typedef struct { char addr[18], alias[64]; int c, p, t, dbm; } nm_dev;                                                          /* dbm 0 = no reading */
typedef struct { netman_if f; netman_ifstat st; int hasst; } nm_if;
typedef struct { char name[48], kind[16]; int up; } nm_vpn;

typedef struct {
	const char *reset, *title, *tab, *tabsel, *dim, *head, *sel, *ok, *warn, *err, *key, *modal;
	const char *spin[10]; int nspin;
	const char *bx[6];                 /* box: top-left, top-right, bottom-left, bottom-right, horizontal, vertical */
	const char *track, *thumb;         /* scrollbar */
	const char *lock_s, *lock_o;       /* secured / open marker, padded to 4 cells by the renderer */
	const char *ell, *up, *down, *flat;/* ellipsis and trend arrows */
	int unicode;
} nm_theme;

typedef struct { char key[96]; signed char v[8]; int n; } nm_hist;   /* last signal readings of one network */

enum { NM_WIFI, NM_ETH, NM_BT, NM_VPN, NM_NTAB };
enum { NM_NORMAL, NM_FILTER, NM_PROMPT, NM_CONFIRM, NM_HELP, NM_WPS, NM_INFO };
enum { NM_K_UP = 0x101, NM_K_DOWN, NM_K_RIGHT, NM_K_LEFT, NM_K_BTAB, NM_K_PGUP, NM_K_PGDN, NM_K_HOME, NM_K_END, NM_K_TAB = 9, NM_K_ENTER = 13, NM_K_ESC = 27, NM_K_BS = 127 };
enum { NM_SORT_SIGNAL, NM_SORT_NAME, NM_SORT_SECURED, NM_SORT_OPEN, NM_NSORT };

typedef struct {
	int tab, sel[NM_NTAB], top[NM_NTAB], loaded[NM_NTAB], en[NM_NTAB], w, h, mode, quit;
	nm_net nets[NM_MAXNET]; int nnets;
	nm_dev devs[NM_MAXDEV]; int ndevs;
	nm_if ifs[NETMAN_MAXIF]; int nifs; netman_metrics met;
	nm_vpn vpns[NM_MAXVPN]; int nvpn;
	char swifi[200], sbt[200], filter[64], msg[300];
	int msgerr, sortm, secf, rssi_good, rssi_ok, rssi_weak;   /* secf: 0 all, 1 only open, 2 only secured */
	char in[128], label[96], pa[128];       /* prompt: input buffer, label, value saved from an earlier stage (the SSID) */
	char ptitle[128], pinfo[128], perr[300], pident[128];   /* modal: title, "Security: WPA2", the last failure, 802.1X identity */
	char psec[8];
	int inlen, masked, pshow, pkind, ckind;
	char cwhat[96], cssid[96];              /* what a confirmation is about */
	int hoff;                               /* help / info popup scroll offset */
	int busy, spin;                         /* a worker is running; spinner frame counter */
	char blabel[96];
	char dl[NM_MAXDL][160]; int ndl;        /* lines of the details popup */
	nm_hist hist[NM_MAXNET]; int nhist;
} nm_ui;

/* ---- text width --------------------------------------------------------------------------------------------------- */

static int nm__cw(unsigned cp)   /* display cells of one code point: 0 combining/control, 2 wide (CJK, emoji), else 1 */
{
	if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 0;
	if ((cp >= 0x300 && cp <= 0x36f) || (cp >= 0x200b && cp <= 0x200f) || (cp >= 0xfe00 && cp <= 0xfe0f)) return 0;
	if (cp >= 0x1100 && (cp <= 0x115f || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
	    (cp >= 0xfe30 && cp <= 0xfe6f) || (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1faff) ||
	    (cp >= 0x20000 && cp <= 0x3fffd))) return 2;
	return 1;
}
static int nm__dec(const char *s, unsigned *cp)   /* bytes of the sequence at s (>= 1); invalid bytes decode as U+FFFD, 1 byte */
{
	const unsigned char *p = (const unsigned char *)s;
	int n = p[0] < 0x80 ? 1 : (p[0] & 0xE0) == 0xC0 ? 2 : (p[0] & 0xF0) == 0xE0 ? 3 : (p[0] & 0xF8) == 0xF0 ? 4 : 0;
	unsigned c;
	if (!n) { *cp = 0xfffd; return 1; }
	c = n == 1 ? p[0] : p[0] & (0xFF >> (n + 1));
	for (int i = 1; i < n; i++) { if ((p[i] & 0xC0) != 0x80) { *cp = 0xfffd; return 1; } c = c << 6 | (p[i] & 0x3F); }
	*cp = c;
	return n;
}
static int nm__cells(const char *s) { int w = 0; unsigned cp; while (*s) { s += nm__dec(s, &cp); w += nm__cw(cp); } return w; }

/* copy s into out (n bytes) so that it takes at most `cells` display cells; when it does not fit the end becomes `ell` ("..." or the one-cell ellipsis) */
static int nm__fit(char *out, size_t n, const char *s, int cells, const char *ell)
{
	int total = nm__cells(s), ec = nm__cells(ell), w = 0, lim = total <= cells ? cells : cells - ec;
	size_t o = 0;
	unsigned cp;
	if (cells <= 0) { out[0] = 0; return 0; }
	if (lim < 0) lim = 0;
	while (*s) {
		int l = nm__dec(s, &cp), cw = nm__cw(cp);
		if (w + cw > lim || o + (size_t)l + 8 > n) break;
		if (cp == 0xfffd || (cw == 0 && cp < 0x20)) { out[o++] = '?'; w++; s += l; continue; }
		memcpy(out + o, s, (size_t)l); o += (size_t)l; w += cw; s += l;
	}
	if (*s) { size_t el = strlen(ell); if (o + el + 1 <= n) { memcpy(out + o, ell, el); o += el; w += ec; } }
	out[o] = 0;
	return w;
}

static char *nm__rstrip(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = 0; return s; }
static void nm__copy(char *d, size_t n, const char *s)   /* truncating copy that never cuts a UTF-8 sequence in half */
{
	size_t l = strlen(s);
	if (l >= n) { l = n - 1; while (l && ((unsigned char)s[l] & 0xC0) == 0x80) l--; }
	memcpy(d, s, l);
	d[l] = 0;
}
static int nm__next_line(const char **p, char *buf, size_t n)
{
	const char *s = *p, *e;
	size_t l;
	if (!s || !*s) return 0;
	e = strchr(s, '\n');
	l = e ? (size_t)(e - s) : strlen(s);
	if (l >= n) l = n - 1;
	memcpy(buf, s, l);
	buf[l] = 0;
	*p = e ? e + 1 : s + strlen(s);
	while (l && buf[l - 1] == '\r') buf[--l] = 0;
	return 1;
}

/* ---- parsing what the library prints ------------------------------------------------------------------------------ */

/* `netman wifi scan` TSV: FLAG KNOWN SEC DBM PCT BSSID CHAN BAND NBSS HIDDEN NAME (NAME may be empty) */
int nm_parse_scan(const char *text, nm_net *o, int max)
{
	char line[600];
	int n = 0;
	while (n < max && nm__next_line(&text, line, sizeof line)) {
		char *f[11], *p = line;
		int k = 0;
		nm_net *e = &o[n];
		f[k++] = p;
		while (k < 11 && (p = strchr(p, '\t'))) { *p++ = 0; f[k++] = p; }
		if (k < 11) continue;
		memset(e, 0, sizeof *e);
		e->connected = f[0][0] == '*';
		e->known = atoi(f[1]) != 0;
		nm__copy(e->sec, sizeof e->sec, f[2]);
		e->dbm = !strcmp(f[3], "-") ? 0 : atoi(f[3]);
		e->pct = !strcmp(f[4], "-") ? -1 : atoi(f[4]);
		nm__copy(e->bssid, sizeof e->bssid, f[5]);
		e->chan = atoi(f[6]);
		nm__copy(e->band, sizeof e->band, !strcmp(f[7], "-") ? "" : f[7]);
		e->nbss = atoi(f[8]);
		e->hidden = atoi(f[9]) != 0 || !f[10][0];
		nm__copy(e->name, sizeof e->name, f[10]);
		e->autoc = -1;
		n++;
	}
	return n;
}

static int nm__is_type(const char *s) { return !strcmp(s, "open") || !strcmp(s, "psk") || !strcmp(s, "8021x") || !strcmp(s, "wep"); }

int nm_parse_known(const char *text, nm_net *o, int max)   /* `wifi known` output */
{
	char line[512];
	int n = 0;
	while (n < max && nm__next_line(&text, line, sizeof line)) {
		char *p = strstr(line, "autoconnect="), *sp;
		nm_net *e = &o[n];
		if (line[0] != ' ' || line[1] != ' ' || !p) continue;
		memset(e, 0, sizeof *e);
		e->autoc = !strncmp(p + 12, "on", 2);
		*p = 0;
		nm__rstrip(line);
		if ((sp = strrchr(line, ' ')) && nm__is_type(sp + 1)) { *sp = 0; nm__rstrip(line); }
		nm__copy(e->name, sizeof e->name, line + 2);
		e->known = 1;
		n++;
	}
	return n;
}

int nm_parse_bt(const char *text, nm_dev *o, int max)   /* `bt list` output */
{
	char line[512];
	int n = 0;
	while (n < max && nm__next_line(&text, line, sizeof line)) {
		char *p, *sp;
		nm_dev *e = &o[n];
		size_t l = strlen(line);
		if (l < 24 || line[3] != ' ' || line[6] != ':' || line[9] != ':' || line[12] != ':' || line[15] != ':' || line[18] != ':' || line[21] != ' ') continue;
		if (!strchr("C-", line[0]) || !strchr("P-", line[1]) || !strchr("T-", line[2])) continue;
		if (strcmp(line + l - 4, " dBm")) continue;
		line[l - 4] = 0;
		memset(e, 0, sizeof *e);
		e->c = line[0] == 'C'; e->p = line[1] == 'P'; e->t = line[2] == 'T';
		memcpy(e->addr, line + 4, 17);
		p = line + 23;
		if (!(sp = strrchr(p, ' '))) sp = p - 1;
		e->dbm = sp[1] == '-' && !sp[2] ? 0 : atoi(sp + 1);
		if (sp >= p) *sp = 0; else *p = 0;
		nm__rstrip(p);
		nm__copy(e->alias, sizeof e->alias, p);
		n++;
	}
	return n;
}

int nm_parse_vpn(const char *text, nm_vpn *o, int max)   /* `vpn list`: "NAME TYPE up|down ..." */
{
	char line[300];
	int n = 0;
	while (n < max && nm__next_line(&text, line, sizeof line)) {
		char a[48], b[16], c[16];
		if (sscanf(line, "%47s %15s %15s", a, b, c) != 3 || (strcmp(c, "up") && strcmp(c, "down"))) continue;
		memset(&o[n], 0, sizeof o[n]);
		nm__copy(o[n].name, sizeof o[n].name, a); nm__copy(o[n].kind, sizeof o[n].kind, b);
		o[n].up = !strcmp(c, "up");
		n++;
	}
	return n;
}

/* ---- signal: quality colour, history with a trend arrow ------------------------------------------------------------- */

static const char *nm__sigcol(const nm_ui *u, const nm_theme *t, int dbm)
{
	if (!dbm) return t->dim;
	return dbm >= u->rssi_good ? t->ok : dbm >= u->rssi_ok ? t->warn : t->err;
}

static const char *nm__hkey(const nm_net *e) { return e->hidden ? e->bssid : e->name; }

void nm_hist_push(nm_ui *u)
{
	for (int i = 0; i < u->nnets; i++) {
		nm_hist *h = NULL;
		const char *k = nm__hkey(&u->nets[i]);
		if (!u->nets[i].dbm) continue;
		for (int j = 0; j < u->nhist; j++) if (!strcmp(u->hist[j].key, k)) { h = &u->hist[j]; break; }
		if (!h) {
			if (u->nhist >= NM_MAXNET) continue;
			h = &u->hist[u->nhist++];
			memset(h, 0, sizeof *h);
			nm__copy(h->key, sizeof h->key, k);
		}
		if (h->n == 8) { memmove(h->v, h->v + 1, 7); h->n = 7; }
		h->v[h->n++] = (signed char)(u->nets[i].dbm < -127 ? -127 : u->nets[i].dbm > 0 ? 0 : u->nets[i].dbm);
	}
}

/* "-70 -68 -66 -64 ^": the last readings as numbers and a trend arrow (last two against the first two, 3 dBm hysteresis); returns the count */
int nm_hist_text(const nm_ui *u, const nm_theme *t, const nm_net *e, char *out, size_t n)
{
	out[0] = 0;
	for (int j = 0; j < u->nhist; j++) {
		const nm_hist *h = &u->hist[j];
		size_t used = 0;
		int a, b;
		if (strcmp(h->key, nm__hkey(e))) continue;
		for (int i = 0; i < h->n && used + 6 < n; i++) used += (size_t)snprintf(out + used, n - used, "%s%d", i ? " " : "", h->v[i]);
		a = h->n > 1 ? (h->v[0] + (h->n > 2 ? h->v[1] : h->v[0])) / 2 : h->v[0];
		b = h->n > 1 ? (h->v[h->n - 1] + (h->n > 2 ? h->v[h->n - 2] : h->v[h->n - 1])) / 2 : h->v[0];
		if (h->n > 1) snprintf(out + used, n - used, " %s", b - a > 3 ? t->up : a - b > 3 ? t->down : t->flat);
		return h->n;
	}
	return 0;
}

/* ---- theme + state ------------------------------------------------------------------------------------------------ */

void nm_theme_make(nm_theme *t, int unicode, const char *name)
{
	memset(t, 0, sizeof *t);
	t->unicode = unicode;
	t->reset = "\x1b[0m"; t->title = "\x1b[1m"; t->tab = "\x1b[2m"; t->tabsel = "\x1b[7;1m"; t->dim = "\x1b[2m"; t->head = "\x1b[4;2m";
	t->sel = "\x1b[7m"; t->ok = "\x1b[32m"; t->warn = "\x1b[33;1m"; t->err = "\x1b[31;1m"; t->key = "\x1b[1;36m"; t->modal = "\x1b[36m";
	if (name && !strcmp(name, "light")) { t->ok = "\x1b[32;1m"; t->warn = "\x1b[35;1m"; t->key = "\x1b[1;34m"; t->modal = "\x1b[34m"; }
	if (name && !strcmp(name, "mono")) { t->ok = ""; t->warn = "\x1b[1m"; t->err = "\x1b[1;4m"; t->key = "\x1b[1m"; t->modal = ""; }
	if (unicode) {
		static const char *sp[10] = { "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8", "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7", "\xe2\xa0\x87", "\xe2\xa0\x8f" };
		static const char *bx[6] = { "\xe2\x94\x8c", "\xe2\x94\x90", "\xe2\x94\x94", "\xe2\x94\x98", "\xe2\x94\x80", "\xe2\x94\x82" };
		memcpy(t->spin, sp, sizeof sp); t->nspin = 10; memcpy(t->bx, bx, sizeof bx);
		t->track = "\xe2\x94\x82"; t->thumb = "\xe2\x96\x88";
		t->lock_s = "\xf0\x9f\x94\x92"; t->lock_o = "\xf0\x9f\x94\x93";
		t->ell = "\xe2\x80\xa6"; t->up = "\xe2\x86\x91"; t->down = "\xe2\x86\x93"; t->flat = "\xe2\x86\x92";
	} else {
		static const char *sp[10] = { "|", "/", "-", "\\", "|", "/", "-", "\\", "|", "/" };
		static const char *bx[6] = { "+", "+", "+", "+", "-", "|" };
		memcpy(t->spin, sp, sizeof sp); t->nspin = 4; memcpy(t->bx, bx, sizeof bx);
		t->track = "|"; t->thumb = "#";
		t->lock_s = "[*]"; t->lock_o = "open";
		t->ell = "..."; t->up = "^"; t->down = "v"; t->flat = "=";
	}
}

void nm_ui_init(nm_ui *u, int w, int h)
{
	memset(u, 0, sizeof *u);
	u->w = w < 20 ? 20 : w;
	u->h = h < 6 ? 6 : h;
	for (int i = 0; i < NM_NTAB; i++) u->en[i] = 1;
	u->rssi_good = -55; u->rssi_ok = -65; u->rssi_weak = -75;
}

static int nm__ci_has(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	if (!n) return 1;
	for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return 1;
	return 0;
}

static int nm__net_open(const nm_net *e) { return !strcmp(e->sec, "OPEN"); }
static const char *nm__net_label(const nm_net *e) { return e->hidden ? "<hidden>" : e->name; }

/* sort key of the Wi-Fi list: the connected network is always first; then by the chosen order */
static int nm__net_cmp(const nm_ui *u, const nm_net *a, const nm_net *b)
{
	int r;
	if (a->connected != b->connected) return b->connected - a->connected;
	switch (u->sortm) {
	case NM_SORT_NAME: r = strcasecmp(nm__net_label(a), nm__net_label(b)); if (r) return r; break;
	case NM_SORT_SECURED: if (nm__net_open(a) != nm__net_open(b)) return nm__net_open(a) - nm__net_open(b); break;
	case NM_SORT_OPEN: if (nm__net_open(a) != nm__net_open(b)) return nm__net_open(b) - nm__net_open(a); break;
	}
	if (a->dbm != b->dbm) return b->dbm - a->dbm;
	return strcmp(a->bssid, b->bssid);
}

int nm_ui_visible(const nm_ui *u, int *idx, int max)   /* rows of the current tab that pass the filter, in display order */
{
	int n = 0, total = u->tab == NM_WIFI ? u->nnets : u->tab == NM_BT ? u->ndevs : u->tab == NM_ETH ? u->nifs : u->tab == NM_VPN ? u->nvpn : 0;
	for (int i = 0; i < total && n < max; i++) {
		const char *a, *b = "";
		if (u->tab == NM_WIFI) {
			const nm_net *e = &u->nets[i];
			if (u->secf == 1 && !nm__net_open(e)) continue;
			if (u->secf == 2 && nm__net_open(e)) continue;
			a = nm__net_label(e); b = e->bssid;
		} else if (u->tab == NM_BT) { a = u->devs[i].alias; b = u->devs[i].addr; }
		else if (u->tab == NM_ETH) { a = u->ifs[i].f.name; b = u->ifs[i].f.naddr ? u->ifs[i].f.addr[0] : ""; }
		else a = u->vpns[i].name;
		if (nm__ci_has(a, u->filter) || nm__ci_has(b, u->filter)) idx[n++] = i;
	}
	if (u->tab == NM_WIFI)   /* insertion sort: stable and the list is short */
		for (int i = 1; i < n; i++) {
			int v = idx[i], j = i - 1;
			while (j >= 0 && nm__net_cmp(u, &u->nets[idx[j]], &u->nets[v]) > 0) { idx[j + 1] = idx[j]; j--; }
			idx[j + 1] = v;
		}
	return n;
}

static int nm__rows(const nm_ui *u) { return u->h - 5 > 1 ? u->h - 5 : 1; }

static void nm__clamp(nm_ui *u)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET), r = nm__rows(u), *s = &u->sel[u->tab], *t = &u->top[u->tab];
	if (*s >= n) *s = n - 1;
	if (*s < 0) *s = 0;
	if (*s < *t) *t = *s;
	if (*s >= *t + r) *t = *s - r + 1;
	if (*t < 0) *t = 0;
}

/* ---- rendering ---------------------------------------------------------------------------------------------------- */

typedef struct { char b[4096]; int n, col, w; } nm__Row;

static void nm__row_init(nm__Row *r, int w) { r->n = r->col = 0; r->w = w; r->b[0] = 0; }
static void nm__row_raw(nm__Row *r, const char *s) { int l = (int)strlen(s); if (r->n + l < (int)sizeof r->b - 1) { memcpy(r->b + r->n, s, (size_t)l); r->n += l; r->b[r->n] = 0; } }
static void nm__row_add(nm__Row *r, const char *s, int maxcells)
{
	int lim = r->w - r->col;
	unsigned cp;
	if (maxcells >= 0 && maxcells < lim) lim = maxcells;
	while (*s && lim > 0) {
		int l = nm__dec(s, &cp), cw = nm__cw(cp);
		char tmp[8] = {0};
		if (cp < 0x20 || cp == 0x7f || cp == 0xfffd) { nm__row_raw(r, "?"); s += l; r->col++; lim--; continue; }
		if (cw > lim) break;
		memcpy(tmp, s, (size_t)l);
		nm__row_raw(r, tmp);
		s += l; r->col += cw; lim -= cw;
	}
}
static void nm__row_fmt(nm__Row *r, int maxcells, const char *fmt, ...)
{
	char tmp[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	nm__row_add(r, tmp, maxcells);
}
static void nm__row_pad(nm__Row *r, int col) { while (r->col < col && r->col < r->w) { nm__row_raw(r, " "); r->col++; } }
static void nm__row_emit(const nm__Row *r, const nm_theme *t, FILE *o, int row) { fprintf(o, "\x1b[%d;1H%s%s\x1b[K", row, r->b, t->reset); }

/* visible tabs, in order */
static int nm__tabs(const nm_ui *u, int *out) { int n = 0; for (int i = 0; i < NM_NTAB; i++) if (u->en[i]) out[n++] = i; return n; }

static const char *nm__tabname(int i, int shortn)
{
	static const char *full[] = { "Wi-Fi", "Ethernet/IP", "Bluetooth", "VPN" }, *sh[] = { "Wi-Fi", "Eth", "BT", "VPN" };
	return shortn ? sh[i] : full[i];
}

typedef struct { const char *k, *l; } nm__Key;

static void nm__kb(nm__Row *r, const nm_theme *t, const char *k, const char *l) { nm__row_raw(r, t->key); nm__row_add(r, k, -1); nm__row_raw(r, t->reset); nm__row_fmt(r, -1, " %s  ", l); }

static void nm__keybar(const nm_ui *u, const nm_theme *t, nm__Row *r)
{
	static const nm__Key wifi[] = { { "enter", "connect" }, { "i", "info" }, { "s", "sort" }, { "o", "open/sec" }, { "d", "disc" }, { "x", "forget" }, { "a", "auto" }, { "w", "wps" }, { "H", "hotspot" }, { "r", "rescan" }, { "/", "filter" } };
	static const nm__Key bt[] = { { "enter", "conn" }, { "i", "info" }, { "p", "pair" }, { "t", "trust" }, { "x", "remove" }, { "s", "scan" }, { "o", "power" }, { "/", "filter" } };
	static const nm__Key eth[] = { { "i", "info" }, { "r", "refresh" }, { "/", "filter" } };
	static const nm__Key vpn[] = { { "enter", "up/down" }, { "r", "refresh" }, { "/", "filter" } };
	const nm__Key *k = u->tab == NM_WIFI ? wifi : u->tab == NM_BT ? bt : u->tab == NM_ETH ? eth : vpn;
	int n = u->tab == NM_WIFI ? 11 : u->tab == NM_BT ? 8 : u->tab == NM_ETH ? 3 : 3, fixed = 14;
	if (u->mode == NM_HELP || u->mode == NM_INFO) { nm__kb(r, t, "j/k", "scroll"); nm__kb(r, t, "esc", "close"); return; }
	if (u->mode == NM_PROMPT) { nm__kb(r, t, "enter", "ok"); if (u->masked) nm__kb(r, t, "tab", "show/hide"); nm__kb(r, t, "esc", "cancel"); return; }
	if (u->mode == NM_CONFIRM) { nm__kb(r, t, "y", "yes"); nm__kb(r, t, "n", "no"); return; }
	for (int i = 0; i < n; i++) {
		int need = nm__cells(k[i].k) + 1 + nm__cells(k[i].l) + 2;
		if (r->col + need + fixed > r->w) break;
		nm__kb(r, t, k[i].k, k[i].l);
	}
	nm__kb(r, t, "?", "help"); nm__kb(r, t, "q", "quit");
}

/* ---- modal: a bordered box centred over the list area ------------------------------------------------------------------ */

typedef struct { char text[200]; const char *style; } nm__ML;

static void nm__modal(const nm_ui *u, const nm_theme *t, FILE *o, const char *title, const nm__ML *l, int nl)
{
	int bw = 0, top, left, ah = u->h - 5 > 1 ? u->h - 5 : 1, bh = nl + 2;
	char tt[160];
	for (int i = 0; i < nl; i++) { int c = nm__cells(l[i].text); if (c > bw) bw = c; }
	bw += 4;
	if (bw < 34) bw = 34;
	if (bw > u->w - 2) bw = u->w - 2;
	if (bw < 12) bw = 12;
	top = 4 + (ah - bh) / 2;
	if (top < 4) top = 4;
	left = (u->w - bw) / 2 + 1;
	snprintf(tt, sizeof tt, " %s ", title);
	nm__fit(tt, sizeof tt, tt, bw - 4, t->ell);
	fprintf(o, "\x1b[%d;%dH%s%s%s%s%s%s%s", top, left, t->modal, t->bx[0], t->bx[4], t->title, tt, t->modal, "");
	for (int i = 1 + nm__cells(tt); i < bw - 2; i++) fputs(t->bx[4], o);
	fprintf(o, "%s%s", t->bx[1], t->reset);
	for (int i = 0; i < nl; i++) {
		char s[200];
		int c = nm__fit(s, sizeof s, l[i].text, bw - 4, t->ell);
		fprintf(o, "\x1b[%d;%dH%s%s%s %s%s", top + 1 + i, left, t->modal, t->bx[5], t->reset, l[i].style ? l[i].style : "", s);
		for (int k = c; k < bw - 4; k++) fputc(' ', o);
		fprintf(o, "%s %s%s%s", t->reset, t->modal, t->bx[5], t->reset);
	}
	fprintf(o, "\x1b[%d;%dH%s%s", top + 1 + nl, left, t->modal, t->bx[2]);
	for (int i = 0; i < bw - 2; i++) fputs(t->bx[4], o);
	fprintf(o, "%s%s", t->bx[3], t->reset);
}

/* scrollable text popup: the help (nc_tui_help) or the details (u->dl) */
static int nm__ptext_n(const nm_ui *u) { return u->mode == NM_INFO ? u->ndl : NC_NTUIHELP; }
static const char *nm__ptext_line(const nm_ui *u, int i) { return u->mode == NM_INFO ? u->dl[i] : nc_tui_help[i]; }
static int nm__help_inner(const nm_ui *u) { int tot = nm__ptext_n(u), ah = u->h - 3; int in = (ah > tot + 2 ? tot + 2 : ah) - 2; return in < 1 ? 1 : in; }

static void nm__text_modal(const nm_ui *u, const nm_theme *t, FILE *o)
{
	int tot = nm__ptext_n(u), bw = u->w - 2 < 78 ? u->w - 2 : 78, inner = nm__help_inner(u), bh = inner + 2, top = 3 + (u->h - 3 - bh) / 2, left = (u->w - bw) / 2 + 1;
	int off = u->hoff, th, pos;
	char tt[96];
	if (bw < 20) bw = 20;
	if (off > tot - inner) off = tot - inner;
	if (off < 0) off = 0;
	snprintf(tt, sizeof tt, " %s %d-%d/%d ", u->mode == NM_INFO ? "details" : "help", off + 1, off + inner < tot ? off + inner : tot, tot);
	nm__fit(tt, sizeof tt, tt, bw - 4, t->ell);
	fprintf(o, "\x1b[%d;%dH%s%s%s", top, left, t->modal, t->bx[0], t->bx[4]);
	fprintf(o, "%s%s%s", t->title, tt, t->modal);
	for (int i = 1 + nm__cells(tt); i < bw - 2; i++) fputs(t->bx[4], o);
	fprintf(o, "%s%s", t->bx[1], t->reset);
	th = tot > inner ? inner * inner / tot : inner;
	if (th < 1) th = 1;
	pos = tot > inner ? off * (inner - th) / (tot - inner) : 0;
	for (int i = 0; i < inner; i++) {
		char s[200] = "";
		const char *ln = off + i < tot ? nm__ptext_line(u, off + i) : "";
		int head = u->mode == NM_HELP && ln[0] && ln[0] != ' ', c = nm__fit(s, sizeof s, ln, bw - 5, t->ell);
		fprintf(o, "\x1b[%d;%dH%s%s%s %s%s", top + 1 + i, left, t->modal, t->bx[5], t->reset, head ? t->key : "", s);
		for (int k = c; k < bw - 5; k++) fputc(' ', o);
		fprintf(o, "%s ", t->reset);
		if (tot > inner) fprintf(o, "%s%s%s", t->modal, i >= pos && i < pos + th ? t->thumb : t->bx[5], t->reset);
		else fprintf(o, "%s%s%s", t->modal, t->bx[5], t->reset);
	}
	fprintf(o, "\x1b[%d;%dH%s%s", top + 1 + inner, left, t->modal, t->bx[2]);
	for (int i = 0; i < bw - 2; i++) fputs(t->bx[4], o);
	fprintf(o, "%s%s", t->bx[3], t->reset);
}

static void nm__overlay(const nm_ui *u, const nm_theme *t, FILE *o)
{
	nm__ML l[8];
	int k = 0;
	memset(l, 0, sizeof l);
	if (u->mode == NM_HELP || u->mode == NM_INFO) {
		nm__text_modal(u, t, o);
	} else if (u->busy) {
		snprintf(l[0].text, sizeof l[0].text, "%s %s", t->spin[u->spin % t->nspin], u->blabel);
		l[0].style = t->key;
		snprintf(l[1].text, sizeof l[1].text, "esc cancels");
		l[1].style = t->dim;
		nm__modal(u, t, o, "working", l, 2);
	} else if (u->mode == NM_PROMPT) {
		int maxin = u->w - 16, shown = u->inlen;
		char in[200] = "";
		if (maxin > 60) maxin = 60;
		if (shown > maxin && maxin > 0) shown = maxin;
		if (u->masked && !u->pshow) { for (int i = 0; i < shown && i < (int)sizeof in - 4; i++) strcat(in, t->unicode ? "\xe2\x80\xa2" : "*"); }
		else snprintf(in, sizeof in, "%.*s", shown, u->in + (u->inlen - shown));
		if (u->pinfo[0]) { snprintf(l[k].text, sizeof l[k].text, "%s", u->pinfo); l[k++].style = t->dim; }
		if (u->perr[0]) { nm__copy(l[k].text, sizeof l[k].text, u->perr); l[k++].style = t->err; }
		snprintf(l[k++].text, sizeof l[0].text, "%s%s_", u->label, in);
		snprintf(l[k].text, sizeof l[k].text, u->masked ? "enter ok   tab show/hide   esc cancel" : "enter ok   esc cancel");
		l[k++].style = t->dim;
		nm__modal(u, t, o, u->ptitle[0] ? u->ptitle : u->masked ? "hidden input" : "input", l, k);
	} else if (u->mode == NM_CONFIRM) {
		snprintf(l[0].text, sizeof l[0].text, u->ckind == 3 ? "%s is OPEN: nothing is encrypted. Connect?" : "%s %s?", u->ckind == 3 ? u->cwhat : u->ckind == 1 ? "forget" : "remove", u->cwhat);
		l[0].style = u->ckind == 3 ? t->warn : t->err;
		snprintf(l[1].text, sizeof l[1].text, "y yes   n no");
		l[1].style = t->dim;
		nm__modal(u, t, o, "confirm", l, 2);
	} else if (u->mode == NM_WPS) {
		snprintf(l[0].text, sizeof l[0].text, "p push-button   i pin   c cancel");
		snprintf(l[1].text, sizeof l[1].text, "esc back");
		l[1].style = t->dim;
		nm__modal(u, t, o, "WPS", l, 2);
	}
}

/* the right-hand part of a Wi-Fi row: tag, dBm, percent, saved flags - fixed width so the columns line up */
static void nm__wifi_row(const nm_ui *u, const nm_theme *t, nm__Row *r, const nm_net *e, int selected, int lw)
{
	char flags[24] = "", dbms[16], pcts[16], nm[200], bs[40] = "";
	int wide = lw >= 56, open = nm__net_open(e), rl, nw;
	if (e->known) snprintf(flags, sizeof flags, "saved%s", e->autoc == 1 ? " auto" : "");
	if (e->dbm) snprintf(dbms, sizeof dbms, "%4d dBm", e->dbm); else snprintf(dbms, sizeof dbms, "     ---");
	if (e->pct >= 0 && e->dbm) snprintf(pcts, sizeof pcts, "%3d%%", e->pct); else snprintf(pcts, sizeof pcts, " --%%");
	rl = (wide ? 11 : 0) + 1 + 7 + 1 + 4 + 1 + (lw >= 70 ? 11 : 0);   /* glyph is on the left; here: tag(7) dbm(8) pct(4) [flags] */
	rl = 7 + 1 + 8 + 1 + 4 + (lw >= 70 ? 1 + 10 : 0);
	nm__row_add(r, e->connected ? "*" : " ", 1);
	nm__row_add(r, " ", 1);
	if (!selected) nm__row_raw(r, open ? t->warn : t->dim);
	nm__row_add(r, open ? t->lock_o : t->lock_s, 4);
	if (!selected) nm__row_raw(r, t->reset);
	nm__row_pad(r, 2 + 4 + 1);
	if (e->hidden && e->bssid[0]) snprintf(bs, sizeof bs, " %s", e->bssid);
	nw = lw - r->col - rl - 1;
	if (nw < 4) nw = 4;
	if (e->hidden) {   /* "<hidden>" + BSSID, the BSSID dimmed; when narrow the BSSID is what gets cut */
		int hl = nm__cells("<hidden>");
		nm__row_add(r, "<hidden>", nw);
		if (nw > hl + 4) { if (!selected) nm__row_raw(r, t->dim); nm__fit(nm, sizeof nm, bs, nw - hl, t->ell); nm__row_add(r, nm, nw - hl); if (!selected) nm__row_raw(r, t->reset); }
	} else { nm__fit(nm, sizeof nm, e->name, nw, t->ell); nm__row_add(r, nm, nw); }
	nm__row_pad(r, lw - rl);
	if (!selected) nm__row_raw(r, open ? t->warn : "");
	nm__row_fmt(r, 7, "%-7s", e->sec[0] ? e->sec : "?");
	if (!selected) nm__row_raw(r, t->reset);
	nm__row_add(r, " ", 1);
	if (!selected) nm__row_raw(r, nm__sigcol(u, t, e->dbm));
	nm__row_fmt(r, 8, "%8s", dbms + (dbms[0] == ' ' && strlen(dbms) > 8 ? 0 : 0));
	nm__row_add(r, " ", 1);
	nm__row_fmt(r, 4, "%4s", pcts);
	if (!selected) nm__row_raw(r, t->reset);
	if (lw >= 70) nm__row_fmt(r, 11, " %-10s", flags);
}

void nm_ui_render(const nm_ui *u, const nm_theme *t, FILE *o)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET), rows = nm__rows(u), w = u->w, h = u->h, tabs[NM_NTAB], nt = nm__tabs(u, tabs);
	nm__Row r;
	/* row 1: tabs */
	nm__row_init(&r, w);
	{
		int shortn = w < 60;
		for (int i = 0; i < nt; i++) {
			nm__row_raw(&r, tabs[i] == u->tab ? t->tabsel : t->tab);
			nm__row_fmt(&r, -1, " %d %s ", i + 1, nm__tabname(tabs[i], shortn));
			nm__row_raw(&r, t->reset);
			nm__row_add(&r, " ", -1);
		}
		if (w - r.col >= 11) { nm__row_pad(&r, w - 10); nm__row_raw(&r, t->title); nm__row_add(&r, "netman", -1); }
	}
	nm__row_emit(&r, t, o, 1);
	/* row 2: summary - the connected network with its numbers and history, | bluetooth */
	nm__row_init(&r, w);
	{
		char wf[260], hs[80] = "";
		const nm_net *cn = NULL;
		int wcol = w >= 70 ? w - 24 : w >= 50 ? w * 2 / 5 : -1;
		for (int i = 0; i < u->nnets; i++) if (u->nets[i].connected) { cn = &u->nets[i]; break; }
		snprintf(wf, sizeof wf, "%s", u->swifi[0] ? u->swifi : "-");
		if (cn && cn->dbm && wcol > 0) {
			nm_hist_text(u, t, cn, hs, sizeof hs);
			{ size_t l = strlen(wf); snprintf(wf + l, sizeof wf - l, "  %d dBm %d%%%s%s", cn->dbm, cn->pct < 0 ? 0 : cn->pct, hs[0] ? "  " : "", hs); }
		}
		nm__row_raw(&r, t->dim);
		{ char wfit[300]; nm__row_add(&r, " wifi: ", wcol); nm__fit(wfit, sizeof wfit, wf, (wcol > 0 ? wcol : w) - r.col, t->ell); nm__row_add(&r, wfit, wcol); }
		if (wcol > 0) { char bt[200]; nm__row_pad(&r, wcol + 1); nm__row_add(&r, " | bt: ", -1); nm__fit(bt, sizeof bt, u->sbt[0] ? u->sbt : "-", w - r.col, t->ell); nm__row_add(&r, bt, -1); }
	}
	nm__row_emit(&r, t, o, 2);
	/* row 3: column header (+ the sort / filter state at the right edge) */
	nm__row_init(&r, w);
	{
		char f[140] = "";
		static const char *sn[] = { "signal", "name", "secured first", "open first" }, *sf[] = { "", " only open", " only secured" };
		const char *hd = u->tab == NM_WIFI ? (w >= 56 ? "   sec  network" : "   sec  network") : u->tab == NM_BT ? " cpt  address            name"
		               : u->tab == NM_ETH ? " iface        state  speed    rx/s     tx/s  address / gateway" : " profile                  type       state";
		if (u->tab == NM_WIFI) snprintf(f, sizeof f, " sort:%s%s%s%s ", sn[u->sortm], sf[u->secf], u->filter[0] ? " /" : "", u->filter);
		else if (u->filter[0] || u->mode == NM_FILTER) snprintf(f, sizeof f, " /%s ", u->filter);
		nm__row_raw(&r, t->head);
		nm__row_add(&r, hd, f[0] && w > nm__cells(f) + 20 ? w - nm__cells(f) : -1);
		nm__row_pad(&r, w - (f[0] && w > nm__cells(f) + 20 ? nm__cells(f) : 0));
		nm__row_raw(&r, t->reset);
		if (f[0] && w > nm__cells(f) + 20) { nm__row_raw(&r, t->key); nm__row_add(&r, f, -1); }
	}
	nm__row_emit(&r, t, o, 3);
	/* list area; a scrollbar takes the last column when the list is longer than the window */
	for (int i = 0; i < rows; i++) {
		int row = 4 + i, k = u->top[u->tab] + i, selected = k == u->sel[u->tab] && k < n;
		int scroll = n > rows, lw = scroll ? w - 1 : w;
		nm__row_init(&r, lw);
		if (nt == 0) {
			nm__row_raw(&r, t->dim);
			if (i == 0) nm__row_add(&r, "  every module is switched off in the config (netman config show)", -1);
		} else if (k < n) {
			if (selected) nm__row_raw(&r, t->sel);
			if (u->tab == NM_WIFI) nm__wifi_row(u, t, &r, &u->nets[idx[k]], selected, lw);
			else if (u->tab == NM_BT) {
				const nm_dev *e = &u->devs[idx[k]];
				char nm[100], rs[16] = "";
				if (e->dbm) snprintf(rs, sizeof rs, "%4d dBm ", e->dbm);
				nm__row_fmt(&r, -1, " %c%c%c  %s  ", e->c ? 'C' : '-', e->p ? 'P' : '-', e->t ? 'T' : '-', e->addr);
				nm__fit(nm, sizeof nm, e->alias[0] ? e->alias : "?", lw - r.col - (int)strlen(rs) - 1, t->ell);
				nm__row_add(&r, nm, -1);
				if (rs[0]) { nm__row_pad(&r, lw - (int)strlen(rs)); nm__row_add(&r, rs, -1); }
			} else if (u->tab == NM_ETH) {
				const nm_if *e = &u->ifs[idx[k]];
				char rx[24] = "-", tx[24] = "-", sp[16] = "-", ad[200] = "", nm[40];
				if (e->hasst) { netman_metrics_fmt_rate(rx, sizeof rx, e->st.rx_bps); netman_metrics_fmt_rate(tx, sizeof tx, e->st.tx_bps); if (e->st.speed_mbps > 0) snprintf(sp, sizeof sp, "%dM", e->st.speed_mbps); }
				for (int a = 0; a < e->f.naddr && !ad[0]; a++) if (!strchr(e->f.addr[a], ':')) snprintf(ad, sizeof ad, "%s", e->f.addr[a]);
				if (!ad[0] && e->f.naddr) snprintf(ad, sizeof ad, "%s", e->f.addr[0]);
				if (e->f.gw4[0]) { size_t l = strlen(ad); snprintf(ad + l, sizeof ad - l, "  gw %s", e->f.gw4); }
				nm__fit(nm, sizeof nm, e->f.name, 12, t->ell);
				nm__row_fmt(&r, -1, " %-12s %-6s %5s %8s %8s  %s%s", nm, !e->f.up ? "down" : e->f.carrier ? "up" : "nolink", sp, rx, tx, ad, "");
			} else {
				const nm_vpn *e = &u->vpns[idx[k]];
				char nm[60];
				nm__fit(nm, sizeof nm, e->name, 24, t->ell);
				nm__row_fmt(&r, -1, " %-24s %-10s ", nm, e->kind);
				if (!selected) nm__row_raw(&r, e->up ? t->ok : t->dim);
				nm__row_add(&r, e->up ? "up" : "down", -1);
			}
			if (selected) nm__row_pad(&r, lw);
		} else if (i == 0 && n == 0) {
			nm__row_raw(&r, t->dim);
			nm__row_add(&r, u->filter[0] || u->secf ? "  nothing matches the filter" : u->loaded[u->tab] ? (u->tab == NM_VPN ? "  no VPN profiles (put WireGuard / OpenVPN configs in the vpn conf dir)" : "  nothing found (r to refresh)") : "  loading...", -1);
		}
		nm__row_emit(&r, t, o, row);
		if (scroll) {
			int th = rows * rows / n, off, pos;
			if (th < 1) th = 1;
			off = u->top[u->tab];
			pos = n > rows ? off * (rows - th) / (n - rows) : 0;
			fprintf(o, "\x1b[%d;%dH%s%s%s", row, w, t->dim, i >= pos && i < pos + th ? t->thumb : t->track, t->reset);
		}
	}
	/* message / prompt row */
	nm__row_init(&r, w);
	if (u->mode == NM_FILTER) {
		int shown = u->inlen, maxin = w - 4;
		nm__row_raw(&r, t->key); nm__row_add(&r, "/", -1); nm__row_raw(&r, t->reset);
		if (shown > maxin && maxin > 0) shown = maxin;
		nm__row_add(&r, u->in + (u->inlen - shown), shown);
		nm__row_raw(&r, t->sel); nm__row_add(&r, " ", 1);
	} else if (u->msg[0] && u->mode != NM_PROMPT && u->mode != NM_CONFIRM && u->mode != NM_WPS && !u->busy) {
		nm__row_raw(&r, u->msgerr ? t->err : t->ok);
		nm__row_fmt(&r, -1, " %s", u->msg);
	} else if (u->tab == NM_WIFI && u->mode == NM_NORMAL && n > 0 && u->sel[NM_WIFI] < n) {   /* the selected network's numbers: history + trend */
		const nm_net *e = &u->nets[idx[u->sel[NM_WIFI]]];
		char hs[80];
		if (nm_hist_text(u, t, e, hs, sizeof hs)) { nm__row_raw(&r, t->dim); nm__row_fmt(&r, -1, " %s  %d dBm  %s", nm__net_label(e), e->dbm, hs); }
	}
	nm__row_emit(&r, t, o, h - 1);
	/* key bar */
	nm__row_init(&r, w);
	nm__row_raw(&r, t->dim);
	nm__row_add(&r, " ", 1);
	nm__row_raw(&r, t->reset);
	nm__keybar(u, t, &r);
	nm__row_emit(&r, t, o, h);
	nm__overlay(u, t, o);
}

/* ---- worker: every library call that can block runs in a forked child that sends rc + captured stdout/stderr back over a pipe ---- */
/* The parent keeps drawing (spinner) and watches the keyboard; esc / ctrl-c kills the child. Without a tty (tests) it simply waits. */

static void (*nm__redraw)(nm_ui *) = NULL;
static char *nm__co, *nm__ce;
static size_t nm__cos, nm__ces;
static int nm__raw_on, nm__pfd = -1, nm__cancelled;
static pid_t nm__kid;
static const char *nm__klabel;
static nm_ui *nm__kui;
static int nm__read_key(void);

static void nm__cap_free(void) { free(nm__co); free(nm__ce); nm__co = nm__ce = NULL; }
static const char *nm__o(void) { return nm__co ? nm__co : ""; }
static const char *nm__e(void) { return nm__ce ? nm__ce : ""; }

static void nm__lastline(const char *text, char *out, size_t n)
{
	const char *e = text + strlen(text), *s;
	out[0] = 0;
	while (e > text && (e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\r')) e--;
	s = e;
	while (s > text && s[-1] != '\n') s--;
	snprintf(out, n, "%.*s", (int)(e - s), s);
}

static void nm__say(nm_ui *u, int err, const char *what, const char *detail)
{
	snprintf(u->msg, sizeof u->msg, "%s%s%s", what, detail && *detail ? ": " : "", detail ? detail : "");
	u->msgerr = err;
}

static void nm__wr(int fd, const void *b, size_t n)
{
	const char *p = b;
	while (n) { ssize_t w = write(fd, p, n); if (w < 0 && errno == EINTR) continue; if (w <= 0) return; p += w; n -= (size_t)w; }
}

static int nm__fork(nm_ui *u, const char *label)   /* 0 in the child (capture on), 1 in the parent */
{
	int fd[2];
	nm__cancelled = 0; nm__kui = u; nm__klabel = label;
	if (pipe(fd) < 0) { nm__pfd = -1; return 1; }
	fflush(NULL);
	nm__kid = fork();
	if (nm__kid < 0) { close(fd[0]); close(fd[1]); nm__pfd = -1; return 1; }
	if (nm__kid == 0) {
		close(fd[0]);
		nm__pfd = fd[1];
		signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL); signal(SIGWINCH, SIG_DFL);
		nm__raw_on = 0;
		nm__co = nm__ce = NULL;
		netman_out = netman_vpn_out = open_memstream(&nm__co, &nm__cos);
		netman_err = netman_vpn_err = open_memstream(&nm__ce, &nm__ces);
		return 0;
	}
	close(fd[1]);
	nm__pfd = fd[0];
	return 1;
}

static int nm__child_exit(int rc)
{
	unsigned hdr[3];
	fflush(netman_out); fflush(netman_err);
	hdr[0] = (unsigned)rc; hdr[1] = (unsigned)(nm__co ? strlen(nm__co) : 0); hdr[2] = (unsigned)(nm__ce ? strlen(nm__ce) : 0);
	nm__wr(nm__pfd, hdr, sizeof hdr);
	if (hdr[1]) nm__wr(nm__pfd, nm__co, hdr[1]);
	if (hdr[2]) nm__wr(nm__pfd, nm__ce, hdr[2]);
	_exit(0);
}

static int nm__wait(void)
{
	nm_ui *u = nm__kui;
	char *buf = NULL;
	size_t len = 0, cap = 0;
	int status, rc = 1, killed = 0;
	nm__co = nm__ce = NULL;
	if (nm__pfd < 0) { nm__ce = strdup("could not start a worker"); return 1; }
	u->busy = 1; u->spin = 0;
	snprintf(u->blabel, sizeof u->blabel, "%s", nm__klabel);
	if (nm__redraw && nm__raw_on) nm__redraw(u);
	for (;;) {
		struct pollfd p[2] = { { nm__pfd, POLLIN, 0 }, { 0, POLLIN, 0 } };
		int pr = poll(p, nm__raw_on ? 2 : 1, 80);
		if (pr > 0 && (p[0].revents & (POLLIN | POLLHUP))) {
			char tmp[4096];
			ssize_t n = read(nm__pfd, tmp, sizeof tmp);
			if (n > 0) {
				if (len + (size_t)n + 1 > cap) { cap = (len + (size_t)n + 1) * 2; buf = realloc(buf, cap); }
				memcpy(buf + len, tmp, (size_t)n);
				len += (size_t)n;
				continue;
			}
			if (n == 0 || (errno != EINTR && errno != EAGAIN)) break;
		}
		if (pr > 0 && nm__raw_on && (p[1].revents & POLLIN) && !killed) {
			int k = nm__read_key();
			if (k == NM_K_ESC || k == 3) { kill(nm__kid, SIGKILL); killed = nm__cancelled = 1; }
		}
		if (pr <= 0) { u->spin++; if (nm__redraw && nm__raw_on) nm__redraw(u); }
	}
	close(nm__pfd);
	nm__pfd = -1;
	while (waitpid(nm__kid, &status, 0) < 0 && errno == EINTR) {}
	u->busy = 0;
	if (killed) { free(buf); nm__ce = strdup("cancelled"); return 1; }
	if (len >= 3 * sizeof(unsigned)) {
		unsigned hdr[3];
		memcpy(hdr, buf, sizeof hdr);
		if (len >= sizeof hdr + hdr[1] + hdr[2]) {
			nm__co = malloc(hdr[1] + 1); memcpy(nm__co, buf + sizeof hdr, hdr[1]); nm__co[hdr[1]] = 0;
			nm__ce = malloc(hdr[2] + 1); memcpy(nm__ce, buf + sizeof hdr + hdr[1], hdr[2]); nm__ce[hdr[2]] = 0;
			rc = (int)hdr[0];
			free(buf);
			return rc;
		}
	}
	free(buf);
	nm__ce = strdup("the worker died");
	return rc;
}

/* run a library call in a worker; evaluates to its return code; nm__co / nm__ce then hold what it printed (nm__cap_free releases them) */
#define NM_CALL(u, label, expr) (nm__fork((u), (label)) ? nm__wait() : nm__child_exit((expr)))

#define NM_WRONGPW "\x01WRONGPW"
static int nm__c_connect(const char *ssid, const char *pass)
{
	int rc = netman_wifi_connect(ssid, pass);
	if (rc && netman_wifi_last_wrong_password()) fprintf(netman_err, "%s\n", NM_WRONGPW);
	return rc;
}
static int nm__c_connect_eap(const char *ssid, const char *ident, const char *pass)
{
	int rc = netman_wifi_connect_eap(ssid, ident, pass);
	if (rc && netman_wifi_last_wrong_password()) fprintf(netman_err, "%s\n", NM_WRONGPW);
	return rc;
}

static void nm__report(nm_ui *u, const char *what, int rc)
{
	char l[200];
	nm__lastline(rc ? (nm__ce && *nm__ce ? nm__ce : nm__o()) : nm__o(), l, sizeof l);
	nm__say(u, rc != 0, what, l[0] ? l : rc ? "failed" : "ok");
}

/* ---- refresh -------------------------------------------------------------------------------------------------------- */

static void nm__first_line(const char *text, char *out, size_t n)   /* "wifi     connected to X" -> "connected to X" */
{
	const char *e = strchr(text, '\n');
	char tmp[300];
	size_t l = e ? (size_t)(e - text) : strlen(text);
	const char *p;
	if (l >= sizeof tmp) l = sizeof tmp - 1;
	memcpy(tmp, text, l); tmp[l] = 0;
	p = tmp;
	while (*p && *p != ' ') p++;
	while (*p == ' ') p++;
	nm__copy(out, n, *p ? p : tmp);
}

static void nm__refresh_status(nm_ui *u)
{
	if (u->en[NM_WIFI]) { (void)NM_CALL(u, "status", netman_wifi_status()); nm__first_line(nm__o(), u->swifi, sizeof u->swifi); nm__cap_free(); }
	if (u->en[NM_BT]) {
		(void)NM_CALL(u, "status", netman_bt_status());
		if (strstr(nm__o(), "adapter off")) nm__copy(u->sbt, sizeof u->sbt, "adapter off"); else nm__first_line(nm__o(), u->sbt, sizeof u->sbt);
		nm__cap_free();
	}
}

static void nm__eth_refresh(nm_ui *u)
{
	netman_if t[NETMAN_MAXIF];
	int n = netman_list(t, NETMAN_MAXIF), m = 0;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	if (nc_enabled("metrics")) netman_metrics_poll(&u->met, (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	for (int i = 0; i < n && m < NETMAN_MAXIF; i++) {
		const netman_ifstat *st;
		if (!strcmp(t[i].name, "lo")) continue;
		u->ifs[m].f = t[i];
		st = nc_enabled("metrics") ? netman_metrics_find(&u->met, t[i].name) : NULL;
		u->ifs[m].hasst = st != NULL;
		if (st) u->ifs[m].st = *st;
		m++;
	}
	u->nifs = m;
	u->loaded[NM_ETH] = 1;
}

static void nm__keep_sel(nm_ui *u, const char *keep)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET);
	if (keep && *keep)
		for (int i = 0; i < n; i++) {
			const char *nm = u->tab == NM_WIFI ? nm__hkey(&u->nets[idx[i]]) : u->tab == NM_BT ? u->devs[idx[i]].addr : u->tab == NM_ETH ? u->ifs[idx[i]].f.name : u->vpns[idx[i]].name;
			if (!strcmp(nm, keep)) { u->sel[u->tab] = i; break; }
		}
	nm__clamp(u);
}

static void nm__keyof(const nm_ui *u, char *keep, size_t n)
{
	int idx[NM_MAXNET], c = nm_ui_visible(u, idx, NM_MAXNET), s = u->sel[u->tab];
	keep[0] = 0;
	if (s < c) nm__copy(keep, n, u->tab == NM_WIFI ? nm__hkey(&u->nets[idx[s]]) : u->tab == NM_BT ? u->devs[idx[s]].addr : u->tab == NM_ETH ? u->ifs[idx[s]].f.name : u->vpns[idx[s]].name);
}

static int nm__scan_req;   /* set only by the `r` key: everything else reads cached scan results */

void nm_refresh(nm_ui *u)
{
	char keep[100];
	nm__keyof(u, keep, sizeof keep);
	if (u->tab == NM_WIFI) {
		nm_net known[NM_MAXNET];
		int nk, rc;
		netman_wifi_passive = !nm__scan_req;
		rc = NM_CALL(u, nm__scan_req ? "scanning networks" : "reading networks", netman_wifi_scan());
		netman_wifi_passive = 0;
		if (rc) nm__report(u, "wifi scan", rc);
		else { u->nnets = nm_parse_scan(nm__o(), u->nets, NM_MAXNET); u->msg[0] = 0; }
		nm__cap_free();
		NM_CALL(u, "saved networks", netman_wifi_known());
		nk = nm_parse_known(nm__o(), known, NM_MAXNET);
		nm__cap_free();
		nm_hist_push(u);
		for (int i = 0; i < u->nnets; i++)
			for (int j = 0; j < nk; j++)
				if (!u->nets[i].hidden && !strcmp(u->nets[i].name, known[j].name)) { u->nets[i].known = 1; u->nets[i].autoc = known[j].autoc; }
	} else if (u->tab == NM_BT) {
		int rc = NM_CALL(u, "bluetooth", netman_bt_list());
		if (rc) nm__report(u, "bluetooth", rc);
		else { u->ndevs = nm_parse_bt(nm__o(), u->devs, NM_MAXDEV); u->msg[0] = 0; }
		nm__cap_free();
	} else if (u->tab == NM_ETH) {
		nm__eth_refresh(u);
	} else if (u->tab == NM_VPN) {
		int rc = NM_CALL(u, "vpn profiles", netman_vpn_list());
		if (rc) nm__report(u, "vpn", rc);
		else { u->nvpn = nm_parse_vpn(nm__o(), u->vpns, NM_MAXVPN); u->msg[0] = 0; }
		nm__cap_free();
	}
	if (u->tab == NM_WIFI || u->tab == NM_BT) nm__refresh_status(u);
	u->loaded[u->tab] = 1;
	nm__keep_sel(u, keep);
}

static const nm_net *nm__cur_net(const nm_ui *u)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET);
	return u->tab == NM_WIFI && u->sel[NM_WIFI] < n ? &u->nets[idx[u->sel[NM_WIFI]]] : NULL;
}
static const nm_dev *nm__cur_dev(const nm_ui *u)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET);
	return u->tab == NM_BT && u->sel[NM_BT] < n ? &u->devs[idx[u->sel[NM_BT]]] : NULL;
}

/* ---- details popup -------------------------------------------------------------------------------------------------- */

static void nm__dl(nm_ui *u, const char *fmt, ...)
{
	va_list ap;
	if (u->ndl >= NM_MAXDL) return;
	va_start(ap, fmt);
	vsnprintf(u->dl[u->ndl], sizeof u->dl[0], fmt, ap);
	va_end(ap);
	nm__rstrip(u->dl[u->ndl]);
	u->ndl++;
}

static void nm__dl_text(nm_ui *u, const char *text, const char *skip)   /* each line of captured output; lines starting with a key in skip (space separated) are left out */
{
	char line[200];
	while (nm__next_line(&text, line, sizeof line)) {
		char key[40] = "";
		const char *s = skip;
		int drop = 0;
		sscanf(line, "%39s", key);
		while (s && *s && key[0]) {
			size_t l = strcspn(s, " ");
			if (l == strlen(key) && !strncmp(s, key, l)) { drop = 1; break; }
			s += l; while (*s == ' ') s++;
		}
		if (!drop && line[0]) nm__dl(u, "%s", line);
	}
}

static void nm__ip_lines(nm_ui *u, const char *ifname, const char *leasekey)
{
	netman_if t[NETMAN_MAXIF];
	int n = netman_list(t, NETMAN_MAXIF);
	const netman_if *f = NULL;
	for (int i = 0; i < n; i++) if (!strcmp(t[i].name, ifname)) f = &t[i];
	if (!f) { nm__dl(u, "interface    %s (not found)", ifname); return; }
	nm__dl(u, "interface    %s  %s  mtu %d", f->name, !f->up ? "down" : f->carrier ? "up" : "no carrier", f->mtu);
	for (int a = 0; a < f->naddr; a++) nm__dl(u, "%s%s", a ? "             " : "address      ", f->addr[a]);
	if (f->gw4[0]) nm__dl(u, "gateway      %s", f->gw4);
	if (f->gw6[0]) nm__dl(u, "gateway v6   %s", f->gw6);
	{
		FILE *fp = fopen(netman_resolv_path, "r");
		char l[200], dns[160] = "";
		while (fp && fgets(l, sizeof l, fp)) if (!strncmp(l, "nameserver", 10)) { nm__rstrip(l); snprintf(dns + strlen(dns), sizeof dns - strlen(dns), "%s%s", dns[0] ? " " : "", l + 10 + strspn(l + 10, " \t")); }
		if (fp) fclose(fp);
		nm__dl(u, "dns          %s", dns[0] ? dns : "-");
	}
	if (leasekey && *leasekey) {
		netman_lease le;
		if (netman_lease_load(leasekey, f->name, &le) == 0) {
			char ip[32] = "", rt[32] = "";
			inet_ntop(AF_INET, &le.ip, ip, sizeof ip); inet_ntop(AF_INET, &le.router, rt, sizeof rt);
			nm__dl(u, "lease        %s via %s, %us", ip, rt, (unsigned)le.lease_s);
		} else nm__dl(u, "lease        none cached");
	}
}

static void nm__speed_lines(nm_ui *u, const char *ifname)
{
	netman_metrics m;
	struct timespec ts;
	const netman_ifstat *st;
	char rx[24], tx[24], b1[24], b2[24];
	if (!nc_enabled("metrics")) return;
	memset(&m, 0, sizeof m);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	netman_metrics_poll(&m, (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
	if (!(st = netman_metrics_find(&m, ifname))) return;
	netman_metrics_fmt_bytes(b1, sizeof b1, st->rx_bytes); netman_metrics_fmt_bytes(b2, sizeof b2, st->tx_bytes);
	netman_metrics_fmt_rate(rx, sizeof rx, st->rx_bps); netman_metrics_fmt_rate(tx, sizeof tx, st->tx_bps);
	nm__dl(u, "link speed   %s", st->speed_mbps > 0 ? "" : "unknown");
	if (st->speed_mbps > 0) snprintf(u->dl[u->ndl - 1], sizeof u->dl[0], "link speed   %d Mbit/s", st->speed_mbps);
	nm__dl(u, "rx / tx      %s / %s total (%s, %s now)", b1, b2, rx, tx);
}

static const char *nm__wifi_iface(void)
{
	static char nm[16];
	netman_if t[NETMAN_MAXIF];
	int n;
	const char *e = getenv("NETMAN_IFACE");
	if (e && *e) return e;
	n = netman_list(t, NETMAN_MAXIF);
	for (int i = 0; i < n; i++) if (!strncmp(t[i].name, "wl", 2)) { nm__copy(nm, sizeof nm, t[i].name); return nm; }
	return "wlan0";
}

void nm_info_open(nm_ui *u)
{
	u->ndl = 0; u->hoff = 0;
	if (u->tab == NM_WIFI) {
		const nm_net *e = nm__cur_net(u);
		char raw[96];
		if (!e) { nm__say(u, 1, "details", "nothing selected"); return; }
		netman_wifi_unescape(e->name, raw, sizeof raw);
		nm__dl(u, "network      %s", e->hidden ? "<hidden>" : e->name);
		nm__dl(u, "bssid        %s%s", e->bssid, e->nbss > 1 ? "  (and others)" : "");
		nm__dl(u, "access points %d", e->nbss);
		nm__dl(u, "security     %s", e->sec[0] ? e->sec : "?");
		nm__dl(u, "channel      %d  band %s GHz", e->chan, e->band[0] ? e->band : "?");
		if (e->dbm) nm__dl(u, "signal       %d dBm  %d%%", e->dbm, e->pct < 0 ? 0 : e->pct);
		nm__dl(u, "saved        %s  autoconnect %s", e->known ? "yes" : "no", e->autoc == 1 ? "on" : e->autoc == 0 ? "off" : "-");
		nm__dl(u, "connected    %s", e->connected ? "yes" : "no");
		{
			char hs[80];
			if (nm_hist_text(u, &(nm_theme){ .up = "^", .down = "v", .flat = "=" }, e, hs, sizeof hs)) nm__dl(u, "history      %s", hs);
		}
		{
			int rc = NM_CALL(u, "network details", netman_wifi_info(e->hidden ? NULL : raw));
			if (e->connected || !e->hidden) { if (!rc) nm__dl_text(u, nm__o(), "ssid bssid security channel signal"); else if (e->connected) nm__dl(u, "(no extra details: %s)", nm__e()); }
			nm__cap_free();
		}
		if (e->connected) { const char *ifn = nm__wifi_iface(); nm__ip_lines(u, ifn, raw); nm__speed_lines(u, ifn); }
	} else if (u->tab == NM_BT) {
		const nm_dev *d = nm__cur_dev(u);
		int rc;
		if (!d) { nm__say(u, 1, "details", "nothing selected"); return; }
		rc = NM_CALL(u, "device details", netman_bt_info(d->addr));
		nm__dl(u, "%s", d->alias[0] ? d->alias : d->addr);
		if (!rc) nm__dl_text(u, nm__o(), ""); else nm__dl(u, "(%s)", nm__e());
		if (d->dbm) nm__dl(u, "rssi         %d dBm", d->dbm);
		nm__cap_free();
	} else if (u->tab == NM_ETH) {
		int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET);
		const nm_if *e;
		if (u->sel[NM_ETH] >= n) return;
		e = &u->ifs[idx[u->sel[NM_ETH]]];
		nm__ip_lines(u, e->f.name, NULL);
		nm__dl(u, "mac          %02x:%02x:%02x:%02x:%02x:%02x", e->f.mac[0], e->f.mac[1], e->f.mac[2], e->f.mac[3], e->f.mac[4], e->f.mac[5]);
		for (int i = 0; i < e->f.nroute; i++) nm__dl(u, "route        %s", e->f.route[i]);
		nm__speed_lines(u, e->f.name);
	} else if (u->tab == NM_VPN) {
		int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET);
		char name[48];
		if (u->sel[NM_VPN] >= n) return;
		nm__copy(name, sizeof name, u->vpns[idx[u->sel[NM_VPN]]].name);
		(void)NM_CALL(u, "vpn status", netman_vpn_status(name));
		nm__dl(u, "profile      %s", name);
		nm__dl_text(u, nm__o(), "");
		nm__dl_text(u, nm__e(), "");
		nm__cap_free();
	}
	if (!u->ndl) nm__dl(u, "no details");
	u->mode = NM_INFO;
}

/* ---- connect flow --------------------------------------------------------------------------------------------------- */

enum { PK_PASS = 1, PK_IDENT, PK_EAPPASS, PK_HIDNAME, PK_WPSPIN, PK_HSSID, PK_HSPASS, PK_BTPIN };

static void nm__prompt(nm_ui *u, int kind, const char *label, int masked)
{
	u->mode = NM_PROMPT; u->pkind = kind; u->masked = masked; u->pshow = 0; u->inlen = 0; u->in[0] = 0;
	nm__copy(u->label, sizeof u->label, label);
}

static void nm__pass_prompt(nm_ui *u)   /* the masked login modal for u->cssid / u->psec */
{
	char shown[96];
	nm__fit(shown, sizeof shown, u->cssid, 40, "...");
	snprintf(u->ptitle, sizeof u->ptitle, "Connect to %s", shown);
	snprintf(u->pinfo, sizeof u->pinfo, "Security: %s", u->psec[0] ? u->psec : "WPA2");
	if (!strcmp(u->psec, "802.1X")) nm__prompt(u, PK_EAPPASS, "password: ", 1);
	else nm__prompt(u, PK_PASS, "passphrase: ", 1);
}

static void nm__do_connect(nm_ui *u, const char *pass)   /* connects u->cssid; on failure of a secured network the login modal comes back with the reason */
{
	char l[200];
	int rc, wrong, secured = strcmp(u->psec, "OPEN") != 0;
	if (u->pident[0]) rc = NM_CALL(u, "connecting", nm__c_connect_eap(u->cssid, u->pident, pass));
	else rc = NM_CALL(u, "connecting", nm__c_connect(u->cssid, pass));
	wrong = strstr(nm__e(), NM_WRONGPW) != NULL;
	if (nm__ce) { char *p = strstr(nm__ce, NM_WRONGPW); if (p) memmove(p, p + sizeof NM_WRONGPW - 1 + (p[sizeof NM_WRONGPW - 1] == '\n'), strlen(p + sizeof NM_WRONGPW - 1) + 1); }
	nm__lastline(rc ? (*nm__e() ? nm__e() : nm__o()) : nm__o(), l, sizeof l);
	if (rc && nm__cancelled) { nm__say(u, 1, "connect", "cancelled"); u->perr[0] = 0; }
	else if (rc && secured) {
		nm__pass_prompt(u);
		snprintf(u->perr, sizeof u->perr, "%s", wrong ? "Wrong password - try again (enter retries, esc cancels)" : l[0] ? l : "connection failed");
		if (u->pident[0]) u->pkind = PK_EAPPASS;
		u->msg[0] = 0;
	} else if (rc) nm__say(u, 1, "connect", l[0] ? l : "failed");
	else { u->perr[0] = 0; nm__say(u, 0, "connected to", u->cssid); }
	nm__cap_free();
	nm_refresh(u);
	if (!rc && !nm__cancelled) nm__say(u, 0, "connected to", u->cssid);
}

static void nm__start_connect(nm_ui *u, const nm_net *e)
{
	u->pident[0] = 0; u->perr[0] = 0;
	nm__copy(u->psec, sizeof u->psec, e->sec);
	if (e->hidden && !e->name[0]) {   /* hidden network: the name is the one thing we do not know */
		nm__prompt(u, PK_HIDNAME, "network name (SSID): ", 0);
		snprintf(u->ptitle, sizeof u->ptitle, "Hidden network %s", e->bssid);
		u->pinfo[0] = 0;
		return;
	}
	netman_wifi_unescape(e->name, u->cssid, sizeof u->cssid);
	if (e->connected) { nm__say(u, 0, "already connected to", e->name); return; }
	if (e->known) { nm__do_connect(u, NULL); return; }
	if (nm__net_open(e)) { nm__copy(u->cwhat, sizeof u->cwhat, e->name); u->ckind = 3; u->mode = NM_CONFIRM; return; }
	if (!strcmp(e->sec, "802.1X")) {
		char shown[96];
		nm__fit(shown, sizeof shown, e->name, 40, "...");
		snprintf(u->ptitle, sizeof u->ptitle, "Connect to %s", shown);
		snprintf(u->pinfo, sizeof u->pinfo, "Security: 802.1X (enterprise)");
		nm__prompt(u, PK_IDENT, "identity: ", 0);
		return;
	}
	nm__pass_prompt(u);
}

static void nm__prompt_done(nm_ui *u)
{
	int kind = u->pkind;
	char val[128];
	nm__copy(val, sizeof val, u->in);
	if ((kind == PK_PASS || kind == PK_EAPPASS) && !val[0]) { snprintf(u->perr, sizeof u->perr, "the passphrase is empty"); return; }
	if ((kind == PK_IDENT || kind == PK_HIDNAME) && !val[0]) { snprintf(u->perr, sizeof u->perr, "required"); return; }
	u->mode = NM_NORMAL;
	memset(u->in, 0, sizeof u->in);
	u->inlen = 0;
	u->perr[0] = 0;
	if (kind == PK_PASS || kind == PK_EAPPASS) nm__do_connect(u, val);
	else if (kind == PK_IDENT) { nm__copy(u->pident, sizeof u->pident, val); nm__pass_prompt(u); u->pkind = PK_EAPPASS; }
	else if (kind == PK_HIDNAME) {
		nm__copy(u->cssid, sizeof u->cssid, val);
		u->pident[0] = 0;
		nm__pass_prompt(u);
		snprintf(u->pinfo, sizeof u->pinfo, "Hidden network (security assumed WPA2)");
	} else if (kind == PK_WPSPIN) {
		int rc = NM_CALL(u, "WPS pin", netman_wifi_wps_pin(val[0] ? val : NULL));
		nm__report(u, "wps pin", rc);
		nm__cap_free();
	} else if (kind == PK_HSSID) {
		if (!val[0]) { int rc = NM_CALL(u, "stopping hotspot", netman_wifi_hotspot_stop()); nm__report(u, "hotspot stop", rc); nm__cap_free(); }
		else { nm__copy(u->pa, sizeof u->pa, val); nm__prompt(u, PK_HSPASS, "hotspot passphrase (8-63, empty = open): ", 1); u->ptitle[0] = u->pinfo[0] = 0; }
	} else if (kind == PK_HSPASS) {
		int rc = NM_CALL(u, "starting hotspot", netman_wifi_hotspot_start(u->pa, val[0] ? val : NULL));
		nm__report(u, "hotspot", rc);
		nm__cap_free();
	} else if (kind == PK_BTPIN) {
		const nm_dev *d = nm__cur_dev(u);
		char addr[18];
		int rc;
		if (!d) return;
		nm__copy(addr, sizeof addr, d->addr);
		netman_bt_set_pin(val[0] ? val : "0000");
		rc = NM_CALL(u, "bt pair", netman_bt_action("pair", addr));
		nm__report(u, "pair", rc);
		nm__cap_free();
		netman_bt_set_pin(NULL);
		nm_refresh(u);
	}
	memset(val, 0, sizeof val);
}

static void nm__confirmed(nm_ui *u)
{
	int rc;
	u->mode = NM_NORMAL;
	if (u->ckind == 3) { nm__do_connect(u, NULL); return; }
	if (u->ckind == 1) {
		char raw[96];
		netman_wifi_unescape(u->cwhat, raw, sizeof raw);
		rc = NM_CALL(u, "forgetting", netman_wifi_forget(raw));
		nm__report(u, "forget", rc);
		nm__cap_free();
	} else {
		const nm_dev *d = nm__cur_dev(u);
		char addr[18];
		if (!d) return;
		nm__copy(addr, sizeof addr, d->addr);
		rc = NM_CALL(u, "bt remove", netman_bt_action("remove", addr));
		nm__report(u, "remove", rc);
		nm__cap_free();
	}
	nm_refresh(u);
}

static void nm__wps(nm_ui *u, int key)
{
	int rc;
	u->mode = NM_NORMAL;
	if (key == 'p') {
		if (!getenv("NETMAN_WPS_SECS")) setenv("NETMAN_WPS_SECS", "30", 1);
		rc = NM_CALL(u, "WPS: press the button on the router", netman_wifi_wps_pbc());
		nm__report(u, "wps", rc);
		nm__cap_free();
		if (nm__cancelled) { (void)NM_CALL(u, "cancelling WPS", netman_wifi_wps_cancel()); nm__cap_free(); nm__say(u, 1, "wps", "cancelled"); }
		nm_refresh(u);
	} else if (key == 'i') { u->ptitle[0] = u->pinfo[0] = u->perr[0] = 0; nm__prompt(u, PK_WPSPIN, "WPS pin (empty = generate): ", 0); }
	else if (key == 'c') { rc = NM_CALL(u, "cancelling WPS", netman_wifi_wps_cancel()); nm__report(u, "wps cancel", rc); nm__cap_free(); }
}

static void nm__enter(nm_ui *u)
{
	if (u->tab == NM_WIFI) {
		const nm_net *e = nm__cur_net(u);
		if (e) nm__start_connect(u, e);
	} else if (u->tab == NM_BT) {
		const nm_dev *d = nm__cur_dev(u);
		char addr[18];
		int rc, c;
		if (!d) return;
		nm__copy(addr, sizeof addr, d->addr);
		c = d->c;
		rc = NM_CALL(u, c ? "disconnecting" : "connecting", netman_bt_action(c ? "disconnect" : "connect", addr));
		nm__report(u, c ? "disconnect" : "connect", rc);
		nm__cap_free();
		nm_refresh(u);
	} else if (u->tab == NM_VPN) {
		int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET), up, rc;
		char name[48];
		if (u->sel[NM_VPN] >= n) return;
		nm__copy(name, sizeof name, u->vpns[idx[u->sel[NM_VPN]]].name);
		up = u->vpns[idx[u->sel[NM_VPN]]].up;
		rc = NM_CALL(u, up ? "vpn down" : "vpn up", up ? netman_vpn_down(name) : netman_vpn_up(name, 30));
		nm__report(u, up ? "vpn down" : "vpn up", rc);
		nm__cap_free();
		nm_refresh(u);
	}
}

static void nm__simple_bt(nm_ui *u, const char *action)
{
	const nm_dev *d = nm__cur_dev(u);
	char addr[18];
	int rc;
	if (!d) return;
	nm__copy(addr, sizeof addr, d->addr);
	rc = NM_CALL(u, action, netman_bt_action(action, addr));
	nm__report(u, action, rc);
	nm__cap_free();
	nm_refresh(u);
}

static void nm__move(nm_ui *u, int d) { u->sel[u->tab] += d; nm__clamp(u); }

static void nm__switch(nm_ui *u, int tab)
{
	int tabs[NM_NTAB], nt = nm__tabs(u, tabs), at = 0;
	if (!nt) return;
	if (tab >= 0 && tab < NM_NTAB && u->en[tab] && tab == u->tab) return;
	u->filter[0] = 0;
	u->msg[0] = 0;
	u->tab = tab;
	(void)at;
	if (!u->loaded[u->tab]) nm_refresh(u);
	nm__clamp(u);
}

static void nm__cycle(nm_ui *u, int d)   /* next / previous visible tab */
{
	int tabs[NM_NTAB], nt = nm__tabs(u, tabs), at = 0;
	if (!nt) return;
	for (int i = 0; i < nt; i++) if (tabs[i] == u->tab) at = i;
	nm__switch(u, tabs[(at + d + nt) % nt]);
}

static void nm__text_key(nm_ui *u, int key, int is_filter)
{
	if (key == NM_K_ESC) {
		if (is_filter) u->filter[0] = 0; else { u->perr[0] = 0; u->msg[0] = 0; }
		u->mode = NM_NORMAL; u->inlen = 0; memset(u->in, 0, sizeof u->in); nm__clamp(u);
		return;
	}
	if (key == NM_K_ENTER) { if (is_filter) { u->mode = NM_NORMAL; nm__clamp(u); } else nm__prompt_done(u); return; }
	if (key == NM_K_TAB && !is_filter && u->masked) { u->pshow = !u->pshow; return; }
	if (key == NM_K_BS) {
		if (u->inlen > 0) { do u->inlen--; while (u->inlen > 0 && ((unsigned char)u->in[u->inlen] & 0xC0) == 0x80); u->in[u->inlen] = 0; }
	} else if (key >= 0x20 && key < 0x100 && u->inlen < (int)sizeof u->in - 1) {
		u->in[u->inlen++] = (char)key; u->in[u->inlen] = 0;
	}
	if (is_filter) { nm__copy(u->filter, sizeof u->filter, u->in); u->sel[u->tab] = 0; u->top[u->tab] = 0; nm__clamp(u); }
}

void nm_ui_key(nm_ui *u, int key)
{
	if (u->mode == NM_HELP || u->mode == NM_INFO) {
		int inner = nm__help_inner(u), tot = nm__ptext_n(u), max = tot - inner > 0 ? tot - inner : 0;
		if (key == NM_K_ESC || key == 'q' || key == '?' || key == 3 || (key == 'i' && u->mode == NM_INFO)) { u->mode = NM_NORMAL; return; }
		if (key == 'j' || key == NM_K_DOWN) u->hoff++;
		else if (key == 'k' || key == NM_K_UP) u->hoff--;
		else if (key == NM_K_PGDN || key == ' ') u->hoff += inner - 1;
		else if (key == NM_K_PGUP) u->hoff -= inner - 1;
		else if (key == 'g' || key == NM_K_HOME) u->hoff = 0;
		else if (key == 'G' || key == NM_K_END) u->hoff = max;
		if (u->hoff > max) u->hoff = max;
		if (u->hoff < 0) u->hoff = 0;
		return;
	}
	if (u->mode == NM_FILTER) { nm__text_key(u, key, 1); return; }
	if (u->mode == NM_PROMPT) { nm__text_key(u, key, 0); return; }
	if (u->mode == NM_CONFIRM) { if (key == 'y' || key == 'Y') nm__confirmed(u); else u->mode = NM_NORMAL; return; }
	if (u->mode == NM_WPS) { nm__wps(u, key); return; }
	switch (key) {
	case 'q': case 3: u->quit = 1; break;
	case '?': u->mode = NM_HELP; u->hoff = 0; break;
	case 9: case 'l': case NM_K_RIGHT: nm__cycle(u, 1); break;
	case NM_K_BTAB: case 'h': case NM_K_LEFT: nm__cycle(u, -1); break;
	case '1': case '2': case '3': case '4': {
		int tabs[NM_NTAB], nt = nm__tabs(u, tabs);
		if (key - '1' < nt) nm__switch(u, tabs[key - '1']);
		break;
	}
	case 'j': case NM_K_DOWN: nm__move(u, 1); break;
	case 'k': case NM_K_UP: nm__move(u, -1); break;
	case NM_K_PGDN: nm__move(u, nm__rows(u)); break;
	case NM_K_PGUP: nm__move(u, -nm__rows(u)); break;
	case 'g': case NM_K_HOME: u->sel[u->tab] = 0; nm__clamp(u); break;
	case 'G': case NM_K_END: u->sel[u->tab] = 1 << 20; nm__clamp(u); break;
	case '/': u->mode = NM_FILTER; u->masked = 0; u->inlen = (int)strlen(u->filter); nm__copy(u->in, sizeof u->in, u->filter); break;
	case 'r': nm__scan_req = 1; nm_refresh(u); nm__scan_req = 0; break;
	case 'i': nm_info_open(u); break;
	case NM_K_ENTER: nm__enter(u); break;
	default:
		if (u->tab == NM_WIFI) {
			const nm_net *e = nm__cur_net(u);
			int rc;
			if (key == 's') { char keep[100]; nm__keyof(u, keep, sizeof keep); u->sortm = (u->sortm + 1) % NM_NSORT; nm__keep_sel(u, keep); }
			else if (key == 'o') { char keep[100]; nm__keyof(u, keep, sizeof keep); u->secf = (u->secf + 1) % 3; u->sel[NM_WIFI] = 0; nm__keep_sel(u, keep); }
			else if (key == 'd') { rc = NM_CALL(u, "disconnecting", netman_wifi_disconnect()); nm__report(u, "disconnect", rc); nm__cap_free(); nm_refresh(u); }
			else if (key == 'x' && e) { nm__copy(u->cwhat, sizeof u->cwhat, e->name); u->ckind = 1; u->mode = NM_CONFIRM; }
			else if (key == 'a') {
				if (!e || !e->known) nm__say(u, 1, "autoconnect", "not a saved network");
				else {
					char ssid[96];
					int on = e->autoc != 1;
					netman_wifi_unescape(e->name, ssid, sizeof ssid);
					rc = NM_CALL(u, "autoconnect", netman_wifi_autoconnect(ssid, on));
					nm__report(u, on ? "autoconnect on" : "autoconnect off", rc);
					nm__cap_free();
					nm_refresh(u);
				}
			}
			else if (key == 'w') u->mode = NM_WPS;
			else if (key == 'H') { u->ptitle[0] = u->pinfo[0] = u->perr[0] = 0; nm__prompt(u, PK_HSSID, "hotspot SSID (empty = stop): ", 0); }
		} else if (u->tab == NM_BT) {
			const nm_dev *d = nm__cur_dev(u);
			if (key == 'p' && d) { u->ptitle[0] = u->pinfo[0] = u->perr[0] = 0; nm__prompt(u, PK_BTPIN, "pairing PIN (empty = 0000): ", 1); }
			else if (key == 't') nm__simple_bt(u, "trust");
			else if (key == 'x' && d) { nm__copy(u->cwhat, sizeof u->cwhat, d->alias[0] ? d->alias : d->addr); u->ckind = 2; u->mode = NM_CONFIRM; }
			else if (key == 's') {
				int rc = NM_CALL(u, "scanning bluetooth", netman_bt_scan(5));
				if (rc) nm__report(u, "scan", rc);
				else { u->ndevs = nm_parse_bt(nm__o(), u->devs, NM_MAXDEV); u->msg[0] = 0; }
				nm__cap_free();
				nm__refresh_status(u);
				nm__clamp(u);
			} else if (key == 'o') {
				int on = strstr(u->sbt, "adapter off") != NULL, rc = NM_CALL(u, "adapter power", netman_bt_power(on));
				nm__report(u, on ? "power on" : "power off", rc);
				nm__cap_free();
				nm_refresh(u);
			}
		}
	}
}

/* ---- terminal ----------------------------------------------------------------------------------------------------- */

static struct termios nm__saved;
static volatile sig_atomic_t nm__winch;

static void nm__restore(void)
{
	static const char off[] = "\x1b[0m\x1b[?25h\x1b[?1049l";
	if (!nm__raw_on) return;
	if (write(1, off, sizeof off - 1)) {}
	tcsetattr(0, TCSAFLUSH, &nm__saved);
	nm__raw_on = 0;
}
static void nm__on_term(int s) { nm__restore(); signal(s, SIG_DFL); raise(s); }
static void nm__on_winch(int s) { (void)s; nm__winch = 1; }

static int nm__read_key(void)
{
	unsigned char c;
	ssize_t n;
	struct pollfd p = { 0, POLLIN, 0 };
	if ((n = read(0, &c, 1)) <= 0) return n < 0 && errno == EINTR ? -2 : -1;
	if (c != 27) return c;
	if (poll(&p, 1, 30) <= 0) return NM_K_ESC;
	if (read(0, &c, 1) != 1) return NM_K_ESC;
	if (c != '[' && c != 'O') return NM_K_ESC;
	{
		char seq[8];
		int k = 0;
		while (k < 6 && poll(&p, 1, 30) > 0 && read(0, &seq[k], 1) == 1) { k++; if (seq[k - 1] >= 0x40 && seq[k - 1] <= 0x7e) break; }
		if (!k) return NM_K_ESC;
		switch (seq[k - 1]) {
		case 'A': return NM_K_UP;
		case 'B': return NM_K_DOWN;
		case 'C': return NM_K_RIGHT;
		case 'D': return NM_K_LEFT;
		case 'Z': return NM_K_BTAB;
		case 'H': return NM_K_HOME;
		case 'F': return NM_K_END;
		case '~': return seq[0] == '5' ? NM_K_PGUP : seq[0] == '6' ? NM_K_PGDN : seq[0] == '1' || seq[0] == '7' ? NM_K_HOME : seq[0] == '4' || seq[0] == '8' ? NM_K_END : 0;
		}
	}
	return 0;
}

static void nm__size(nm_ui *u)
{
	struct winsize ws;
	int w = 80, h = 24;
	if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) { w = ws.ws_col; h = ws.ws_row; }
	u->w = w < 20 ? 20 : w;
	u->h = h < 6 ? 6 : h;
}

static nm_theme nm__theme;
static nm_ui nm__ui;

static void nm__paint(nm_ui *u)
{
	char *buf = NULL;
	size_t len = 0;
	FILE *m = open_memstream(&buf, &len);
	if (!m) return;
	fputs("\x1b[?25l", m);
	nm_ui_render(u, &nm__theme, m);
	fclose(m);
	for (size_t off = 0; off < len;) {
		ssize_t w = write(1, buf + off, len - off);
		if (w <= 0) { if (errno == EINTR) continue; break; }
		off += (size_t)w;
	}
	free(buf);
}

static int nc_tui(void)
{
	struct termios raw;
	struct sigaction sa;
	const char *loc, *uni = netman_config_str(&nc_cfg, NC_TUI_UNICODE), *th = netman_config_str(&nc_cfg, NC_TUI_THEME);
	int utf, ntab = 0;
	if (!isatty(0) || !isatty(1)) { fputs("netman: the TUI needs a terminal\n", stderr); return 1; }
	loc = getenv("LC_ALL"); if (!loc || !*loc) loc = getenv("LC_CTYPE"); if (!loc || !*loc) loc = getenv("LANG");
	utf = !strcmp(uni, "yes") || (!strcmp(uni, "auto") && loc && (strcasestr(loc, "utf-8") || strcasestr(loc, "utf8")));
	nm_theme_make(&nm__theme, utf, th);
	nm_ui_init(&nm__ui, 80, 24);
	nm__ui.en[NM_WIFI] = nc_enabled("wifi"); nm__ui.en[NM_ETH] = nc_enabled("net"); nm__ui.en[NM_BT] = nc_enabled("bluetooth"); nm__ui.en[NM_VPN] = nc_enabled("vpn");
	nm__ui.rssi_good = netman_config_int(&nc_cfg, NC_WIFI_RSSI_GOOD); nm__ui.rssi_ok = netman_config_int(&nc_cfg, NC_WIFI_RSSI_OK); nm__ui.rssi_weak = netman_config_int(&nc_cfg, NC_WIFI_RSSI_WEAK);
	for (int i = 0; i < NM_NTAB; i++) if (nm__ui.en[i]) { if (!ntab) nm__ui.tab = i; ntab++; }
	tcgetattr(0, &nm__saved);
	raw = nm__saved;
	cfmakeraw(&raw);
	raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSAFLUSH, &raw);
	nm__raw_on = 1;
	if (write(1, "\x1b[?1049h\x1b[2J", 12)) {}
	memset(&sa, 0, sizeof sa);
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = nm__on_winch;
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = nm__on_term;
	sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL); sigaction(SIGINT, &sa, NULL);
	netman_in = fopen("/dev/null", "r");   /* the pairing agent must never read the terminal the UI owns */
	nm__size(&nm__ui);
	nm__redraw = nm__paint;
	if (ntab) nm_refresh(&nm__ui);
	while (!nm__ui.quit) {
		int k;
		struct pollfd p = { 0, POLLIN, 0 };
		if (nm__winch) { nm__winch = 0; nm__size(&nm__ui); nm__clamp(&nm__ui); if (write(1, "\x1b[2J", 4)) {} }
		nm__paint(&nm__ui);
		/* the Ethernet tab is live: wake up every second to re-read counters */
		if (nm__ui.tab == NM_ETH && !nm__ui.busy && nm__ui.mode == NM_NORMAL) {
			int pr = poll(&p, 1, 1000);
			if (pr == 0) { char keep[100]; nm__keyof(&nm__ui, keep, sizeof keep); nm__eth_refresh(&nm__ui); nm__keep_sel(&nm__ui, keep); continue; }
			if (pr < 0) continue;
		}
		k = nm__read_key();
		if (k == -1) break;
		if (k > 0) nm_ui_key(&nm__ui, k);
	}
	nm__redraw = NULL;
	nm__restore();
	if (netman_in) { fclose(netman_in); netman_in = NULL; }
	return 0;
}

#endif /* NM_TUI_H */
