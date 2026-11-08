/* tui.h - terminal UI for netmanager (stb-style, one header).
 *
 *   #define NM_TUI_IMPLEMENTATION   in exactly one .c file, after including netmanager_blue.h (declarations only is enough)
 *   int nm_tui(void);               run the UI until the user quits; returns 0, or 1 when stdin/stdout is not a terminal
 *
 * It drives the library through its public API only (nmb_status / nmb_wifi_* / nmb_bt_*): every call is made with nmb_out / nmb_err
 * pointed at memory, and the text the library printed is parsed back into rows. The parsers (nm_parse_*) and the renderer
 * (nm_ui_render) are plain functions so they can be tested without a terminal and without a network.
 *
 * Tabs: Wi-Fi | Ethernet/IP | Bluetooth | VPN. Ethernet/IP shows links from `nmb_status`; its profile editor and the VPN tab are placeholders
 * until nm_net / nm_vpn are wired in (the tab structure is already here). Colours come only from nm_theme.
 */
#ifndef NM_TUI_H
#define NM_TUI_H

#include <stdio.h>
#include "netmanager_blue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NM_MAXNET 128
#define NM_MAXDEV 64
#define NM_MAXIF 16

typedef struct { char name[64], type[8]; int dbm, connected, known, autoc; } nm_net;   /* autoc: -1 unknown, 0 off, 1 on */
typedef struct { char addr[18], alias[64]; int c, p, t, dbm; } nm_dev;                  /* dbm 0 = no reading */
typedef struct { char name[16], kind[8], state[16], mac[18], ip[48]; int nolink; } nm_if;

typedef struct {
	const char *reset, *title, *tab, *tabsel, *dim, *head, *sel, *ok, *err, *key;
	const char *bar_on[4], *bar_off;
} nm_theme;

enum { NM_WIFI, NM_ETH, NM_BT, NM_VPN, NM_NTAB };
enum { NM_NORMAL, NM_FILTER, NM_PROMPT, NM_CONFIRM, NM_HELP, NM_WPS };
enum { NM_K_UP = 0x101, NM_K_DOWN, NM_K_RIGHT, NM_K_LEFT, NM_K_BTAB, NM_K_PGUP, NM_K_PGDN, NM_K_HOME, NM_K_END, NM_K_ENTER = 13, NM_K_ESC = 27, NM_K_BS = 127 };

typedef struct {
	int tab, sel[NM_NTAB], top[NM_NTAB], loaded[NM_NTAB], w, h, mode, quit;
	nm_net nets[NM_MAXNET]; int nnets;
	nm_dev devs[NM_MAXDEV]; int ndevs;
	nm_if ifs[NM_MAXIF]; int nifs;
	char swifi[200], sbt[200], filter[64], msg[240];
	int msgerr;
	char in[128], label[96], pa[128];   /* prompt: input buffer, label, value saved from an earlier stage */
	int inlen, masked, pkind, ckind;
	char cwhat[96];                     /* what a confirmation is about */
} nm_ui;

int nm_parse_wifi(const char *text, nm_net *o, int max);                     /* `wifi list` output (iwd and wpa_supplicant forms) */
int nm_parse_known(const char *text, nm_net *o, int max);                    /* `wifi known` output */
int nm_parse_bt(const char *text, nm_dev *o, int max);                       /* `bt list` output */
int nm_parse_status(const char *text, nm_if *ifs, int max, char *wifi, char *bt, size_t n);   /* `status` output; returns the link count */
int nm_bars(int dbm);                                                        /* 0..4 */
void nm_theme_default(nm_theme *t, int unicode);
void nm_ui_init(nm_ui *u, int w, int h);
int nm_ui_visible(const nm_ui *u, int *idx, int max);                        /* rows of the current tab that pass the filter */
void nm_ui_render(const nm_ui *u, const nm_theme *t, FILE *o);
void nm_ui_key(nm_ui *u, int key);                                           /* one key press; may call the library */
int nm_tui(void);

#ifdef __cplusplus
}
#endif
#endif /* NM_TUI_H */

#ifdef NM_TUI_IMPLEMENTATION
#ifndef NM_TUI_IMPLEMENTATION_ONCE
#define NM_TUI_IMPLEMENTATION_ONCE

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ---- parsing what the library prints ------------------------------------------------------------------------------ */

static char *nm__rstrip(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == '\t')) s[--n] = 0; return s; }
static int nm__is_type(const char *s) { return !strcmp(s, "open") || !strcmp(s, "psk") || !strcmp(s, "8021x") || !strcmp(s, "wep"); }
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
	nm__rstrip(buf);
	return 1;
}

int nm_parse_wifi(const char *text, nm_net *o, int max)
{
	char line[512];
	int n = 0;
	while (n < max && nm__next_line(&text, line, sizeof line)) {
		char *p, *sp;
		nm_net *e = &o[n];
		int known = 0;
		size_t l = strlen(line);
		if (l < 4 || (line[0] != ' ' && line[0] != '*') || line[1] != ' ') continue;
		if ((p = strstr(line, "  (known)")) && !p[9]) { known = 1; *p = 0; }
		l = strlen(line);
		if (l < 4 || strcmp(line + l - 4, " dBm")) continue;
		line[l - 4] = 0;
		if (!(sp = strrchr(line, ' '))) continue;
		memset(e, 0, sizeof *e);
		e->dbm = atoi(sp + 1);
		*sp = 0;
		nm__rstrip(line);
		if (!(sp = strrchr(line, ' '))) continue;
		nm__copy(e->type, sizeof e->type, sp + 1);
		if (!nm__is_type(e->type) && strcmp(e->type, "?")) continue;
		*sp = 0;
		nm__rstrip(line);
		nm__copy(e->name, sizeof e->name, line + 2);
		e->connected = line[0] == '*';
		e->known = known;
		e->autoc = -1;
		n++;
	}
	return n;
}

int nm_parse_known(const char *text, nm_net *o, int max)
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
		if ((sp = strrchr(line, ' ')) && nm__is_type(sp + 1)) { nm__copy(e->type, sizeof e->type, sp + 1); *sp = 0; nm__rstrip(line); }
		nm__copy(e->name, sizeof e->name, line + 2);
		e->known = 1;
		n++;
	}
	return n;
}

int nm_parse_bt(const char *text, nm_dev *o, int max)
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

static int nm__is_mac(const char *s)
{
	if (strlen(s) != 17) return 0;
	for (int i = 0; i < 17; i++) if (i % 3 == 2 ? s[i] != ':' : !isxdigit((unsigned char)s[i])) return 0;
	return 1;
}

int nm_parse_status(const char *text, nm_if *ifs, int max, char *wifi, char *bt, size_t n)
{
	char line[512];
	int k = 0;
	if (wifi && n) wifi[0] = 0;
	if (bt && n) bt[0] = 0;
	while (nm__next_line(&text, line, sizeof line)) {
		char t[6][160] = {{0}};
		int nt = 0;
		const char *s = line;
		while (nt < 6) {
			int l = 0;
			while (*s == ' ') s++;
			if (!*s) break;
			while (s[l] && s[l] != ' ') l++;
			snprintf(t[nt++], sizeof t[0], "%.*s", l, s);
			s += l;
		}
		if (nt >= 4 && (!strcmp(t[1], "eth") || !strcmp(t[1], "wifi")) && nm__is_mac(t[3])) {
			nm_if *e;
			if (k >= max) continue;
			e = &ifs[k++];
			memset(e, 0, sizeof *e);
			nm__copy(e->name, sizeof e->name, t[0]); nm__copy(e->kind, sizeof e->kind, t[1]);
			nm__copy(e->state, sizeof e->state, t[2]); nm__copy(e->mac, sizeof e->mac, t[3]);
			if (nt > 4 && isdigit((unsigned char)t[4][0])) nm__copy(e->ip, sizeof e->ip, t[4]);
			e->nolink = strstr(line, "no cable/link") != NULL;
		} else if (!strncmp(line, "wifi ", 5) || !strncmp(line, "bt ", 3)) {
			char *dst = line[0] == 'w' ? wifi : bt;
			const char *v = line + (line[0] == 'w' ? 4 : 2);
			size_t used;
			while (*v == ' ') v++;
			if (!dst || !n) continue;
			used = strlen(dst);
			if (dst == bt && !strncmp(v, "adapter", 7) && used) {   /* power state first: it is what the rest of the line gets cut for */
				char old[400];
				nm__copy(old, sizeof old, dst);
				snprintf(dst, n, "%s; %s", v, old);
			} else snprintf(dst + used, n - used, "%s%s", used ? "; " : "", v);
		}
	}
	return k;
}

int nm_bars(int dbm)
{
	if (dbm >= -55) return 4;
	if (dbm >= -65) return 3;
	if (dbm >= -75) return 2;
	if (dbm >= -85) return 1;
	return 0;
}

/* ---- theme + state ------------------------------------------------------------------------------------------------ */

void nm_theme_default(nm_theme *t, int unicode)
{
	t->reset = "\x1b[0m"; t->title = "\x1b[1m"; t->tab = "\x1b[2m"; t->tabsel = "\x1b[7;1m"; t->dim = "\x1b[2m"; t->head = "\x1b[4;2m";
	t->sel = "\x1b[7m"; t->ok = "\x1b[32m"; t->err = "\x1b[31;1m"; t->key = "\x1b[1;36m";
	if (unicode) { t->bar_on[0] = "\xe2\x96\x82"; t->bar_on[1] = "\xe2\x96\x84"; t->bar_on[2] = "\xe2\x96\x86"; t->bar_on[3] = "\xe2\x96\x88"; t->bar_off = "\xc2\xb7"; }
	else { t->bar_on[0] = "."; t->bar_on[1] = ":"; t->bar_on[2] = "|"; t->bar_on[3] = "#"; t->bar_off = " "; }
}

void nm_ui_init(nm_ui *u, int w, int h)
{
	memset(u, 0, sizeof *u);
	u->w = w < 20 ? 20 : w;
	u->h = h < 6 ? 6 : h;
}

static int nm__ci_has(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	if (!n) return 1;
	for (; *hay; hay++) if (!strncasecmp(hay, needle, n)) return 1;
	return 0;
}

int nm_ui_visible(const nm_ui *u, int *idx, int max)
{
	int n = 0, total = u->tab == NM_WIFI ? u->nnets : u->tab == NM_BT ? u->ndevs : u->tab == NM_ETH ? u->nifs : 0;
	for (int i = 0; i < total && n < max; i++) {
		const char *a = u->tab == NM_WIFI ? u->nets[i].name : u->tab == NM_BT ? u->devs[i].alias : u->ifs[i].name;
		const char *b = u->tab == NM_BT ? u->devs[i].addr : u->tab == NM_ETH ? u->ifs[i].ip : "";
		if (nm__ci_has(a, u->filter) || nm__ci_has(b, u->filter)) idx[n++] = i;
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
static void nm__row_raw(nm__Row *r, const char *s) { int l = strlen(s); if (r->n + l < (int)sizeof r->b - 1) { memcpy(r->b + r->n, s, l); r->n += l; r->b[r->n] = 0; } }
static void nm__row_add(nm__Row *r, const char *s, int maxcells)
{
	int lim = r->w - r->col;
	if (maxcells >= 0 && maxcells < lim) lim = maxcells;
	while (*s && lim > 0) {
		unsigned char c = (unsigned char)*s;
		int len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
		if (c < 0x20 || c == 0x7f) { nm__row_raw(r, "?"); s++; }
		else {
			char tmp[5] = {0};
			int k = 0;
			while (k < len && s[k]) { tmp[k] = s[k]; k++; }
			nm__row_raw(r, tmp);
			s += k;
		}
		r->col++; lim--;
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
static int nm__cells(const char *s) { int n = 0; for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++; return n; }
static void nm__row_emit(const nm__Row *r, const nm_theme *t, FILE *o, int row) { fprintf(o, "\x1b[%d;1H%s%s\x1b[K", row, r->b, t->reset); }

static const char *nm__tabname(int i, int shortn)
{
	static const char *full[] = { "Wi-Fi", "Ethernet/IP", "Bluetooth", "VPN" }, *sh[] = { "Wi-Fi", "Eth", "BT", "VPN" };
	return shortn ? sh[i] : full[i];
}

typedef struct { const char *k, *l; } nm__Key;

static void nm__keybar(const nm_ui *u, const nm_theme *t, nm__Row *r)
{
	static const nm__Key wifi[] = { { "enter", "connect" }, { "d", "disc" }, { "x", "forget" }, { "a", "auto" }, { "w", "wps" }, { "H", "hotspot" }, { "r", "rescan" }, { "/", "filter" } };
	static const nm__Key bt[] = { { "enter", "conn" }, { "p", "pair" }, { "t", "trust" }, { "x", "remove" }, { "s", "scan" }, { "o", "power" }, { "/", "filter" } };
	static const nm__Key oth[] = { { "r", "refresh" }, { "/", "filter" } };
	const nm__Key *k = u->tab == NM_WIFI ? wifi : u->tab == NM_BT ? bt : oth;
	int n = u->tab == NM_WIFI ? 8 : u->tab == NM_BT ? 7 : 2, fixed = 14;
	if (u->mode == NM_PROMPT) { nm__row_raw(r, t->key); nm__row_add(r, "enter", -1); nm__row_raw(r, t->reset); nm__row_add(r, " ok  ", -1); nm__row_raw(r, t->key); nm__row_add(r, "esc", -1); nm__row_raw(r, t->reset); nm__row_add(r, " cancel", -1); return; }
	if (u->mode == NM_CONFIRM) { nm__row_raw(r, t->key); nm__row_add(r, "y", -1); nm__row_raw(r, t->reset); nm__row_add(r, " yes  ", -1); nm__row_raw(r, t->key); nm__row_add(r, "n", -1); nm__row_raw(r, t->reset); nm__row_add(r, " no", -1); return; }
	for (int i = 0; i < n; i++) {
		int need = nm__cells(k[i].k) + 1 + nm__cells(k[i].l) + 2;
		if (r->col + need + fixed > r->w) break;
		nm__row_raw(r, t->key); nm__row_add(r, k[i].k, -1); nm__row_raw(r, t->reset);
		nm__row_fmt(r, -1, " %s  ", k[i].l);
	}
	nm__row_raw(r, t->key); nm__row_add(r, "?", -1); nm__row_raw(r, t->reset); nm__row_add(r, " help  ", -1);
	nm__row_raw(r, t->key); nm__row_add(r, "q", -1); nm__row_raw(r, t->reset); nm__row_add(r, " quit", -1);
}

static const char *nm__help[] = {
	"  tab / shift-tab, h / l, 1-4    switch tab",
	"  j k  or  arrows, g G, pgup/dn  move",
	"  /                              filter (esc clears)",
	"  r                              refresh the current tab",
	"  Wi-Fi:  enter connect (asks for the passphrase if needed)",
	"          d disconnect   x forget   a toggle autoconnect",
	"          w WPS (p button / i pin / c cancel)   H hotspot (empty SSID stops it)",
	"  Bluetooth: enter connect/disconnect   p pair (asks a PIN, enter = 0000)",
	"          t trust   x remove   s scan 5s   o adapter power",
	"  Ethernet/IP, VPN: read-only for now (backends not wired yet)",
	"  q  quit        press any key to close this help",
};

void nm_ui_render(const nm_ui *u, const nm_theme *t, FILE *o)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET), rows = nm__rows(u), w = u->w, h = u->h;
	nm__Row r;
	/* row 1: tabs */
	nm__row_init(&r, w);
	{
		int shortn = w < 60;
		for (int i = 0; i < NM_NTAB; i++) {
			nm__row_raw(&r, i == u->tab ? t->tabsel : t->tab);
			nm__row_fmt(&r, -1, " %d %s ", i + 1, nm__tabname(i, shortn));
			nm__row_raw(&r, t->reset);
			nm__row_add(&r, " ", -1);
		}
		if (w - r.col >= 11) { nm__row_pad(&r, w - 10); nm__row_raw(&r, t->title); nm__row_add(&r, "netmanager", -1); }
	}
	nm__row_emit(&r, t, o, 1);
	/* row 2: summary */
	nm__row_init(&r, w);
	nm__row_raw(&r, t->dim);
	nm__row_fmt(&r, w >= 50 ? w * 2 / 5 : -1, " wifi: %s", u->swifi[0] ? u->swifi : "-");
	if (w >= 50) { nm__row_pad(&r, w * 2 / 5 + 1); nm__row_fmt(&r, -1, " | bt: %s", u->sbt[0] ? u->sbt : "-"); }
	nm__row_emit(&r, t, o, 2);
	/* row 3: column header (+ the active filter at the right edge) */
	nm__row_init(&r, w);
	{
		char f[100] = "";
		const char *hd = u->tab == NM_WIFI ? "   signal  network" : u->tab == NM_BT ? " cpt  address            name"
		               : u->tab == NM_ETH ? " link     kind  state     mac               address" : " vpn";
		if (u->filter[0] || u->mode == NM_FILTER) snprintf(f, sizeof f, " /%s ", u->filter);
		nm__row_raw(&r, t->head);
		nm__row_add(&r, hd, f[0] && w > nm__cells(f) + 20 ? w - nm__cells(f) : -1);
		nm__row_pad(&r, w - (f[0] && w > nm__cells(f) + 20 ? nm__cells(f) : 0));
		nm__row_raw(&r, t->reset);
		if (f[0] && w > nm__cells(f) + 20) { nm__row_raw(&r, t->key); nm__row_add(&r, f, -1); }
	}
	nm__row_emit(&r, t, o, 3);
	/* list area */
	for (int i = 0; i < rows; i++) {
		int row = 4 + i, k = u->top[u->tab] + i, selected = k == u->sel[u->tab] && k < n;
		nm__row_init(&r, w);
		if (u->mode == NM_HELP) {
			nm__row_raw(&r, t->dim);
			if (i < (int)(sizeof nm__help / sizeof *nm__help)) nm__row_add(&r, nm__help[i], -1);
		} else if (u->tab == NM_VPN) {
			static const char *vpn[] = { "", "  VPN: backend not wired yet.", "  WireGuard / OpenVPN profiles and providers (Mullvad, NordVPN) will list here once nm_vpn is plugged in.", "  The CLI side is built separately; this tab keeps its place so nothing else moves." };
			nm__row_raw(&r, t->dim);
			if (i < 4) nm__row_add(&r, vpn[i], -1);
		} else if (k < n) {
			if (selected) nm__row_raw(&r, t->sel);
			if (u->tab == NM_WIFI) {
				const nm_net *e = &u->nets[idx[k]];
				char right[64], flags[24] = "";
				int lvl = nm_bars(e->dbm), rl;
				if (e->known) snprintf(flags, sizeof flags, "saved%s", e->autoc == 1 ? " auto" : "");
				if (w >= 50) snprintf(right, sizeof right, "%-5s %4d dBm  %-10s", e->type, e->dbm, flags);   /* fixed width: columns line up */
				else snprintf(right, sizeof right, "%-5s %4d", e->type, e->dbm);
				rl = nm__cells(right);
				nm__row_add(&r, e->connected ? "*" : " ", 1);
				nm__row_add(&r, " ", 1);
				for (int b = 0; b < 4; b++) {
					if (!selected) nm__row_raw(&r, b < lvl ? (lvl >= 3 ? t->ok : t->title) : t->dim);
					nm__row_add(&r, b < lvl ? t->bar_on[b] : t->bar_off, 1);
				}
				if (!selected) nm__row_raw(&r, t->reset);
				nm__row_add(&r, "  ", -1);
				{ int nw = w - r.col - rl - 1; if (nw < 4) nw = 4; nm__row_fmt(&r, nw, "%s", e->name[0] ? e->name : "(hidden)"); }
				nm__row_pad(&r, w - rl);
				nm__row_add(&r, right, -1);
			} else if (u->tab == NM_BT) {
				const nm_dev *e = &u->devs[idx[k]];
				nm__row_fmt(&r, -1, " %c%c%c  %s  %s", e->c ? 'C' : '-', e->p ? 'P' : '-', e->t ? 'T' : '-', e->addr, e->alias[0] ? e->alias : "?");
				if (e->dbm) { char s[16]; snprintf(s, sizeof s, "%d dBm ", e->dbm); nm__row_pad(&r, w - nm__cells(s)); nm__row_add(&r, s, -1); }
			} else {
				const nm_if *e = &u->ifs[idx[k]];
				nm__row_fmt(&r, -1, " %-8s %-5s %-9s %-17s %s%s", e->name, e->kind, e->state, e->mac, e->ip, e->nolink ? "  no link" : "");
			}
			if (selected) nm__row_pad(&r, w);
		} else if (i == 0 && n == 0) {
			nm__row_raw(&r, t->dim);
			nm__row_add(&r, u->filter[0] ? "  nothing matches the filter" : u->loaded[u->tab] ? "  nothing found (r to refresh)" : "  loading...", -1);
		} else if (u->tab == NM_ETH && i == rows - 1 && rows > n + 1) {
			nm__row_raw(&r, t->dim);
			nm__row_add(&r, "  profiles / DHCP / static IP / DNS: backend not wired yet", -1);
		}
		nm__row_emit(&r, t, o, row);
	}
	/* message / prompt row */
	nm__row_init(&r, w);
	if (u->mode == NM_PROMPT || u->mode == NM_FILTER) {
		const char *lab = u->mode == NM_FILTER ? "/" : u->label;
		int shown = u->inlen, maxin = w - nm__cells(lab) - 3;
		nm__row_raw(&r, t->key); nm__row_add(&r, lab, -1); nm__row_raw(&r, t->reset);
		if (shown > maxin && maxin > 0) shown = maxin;
		if (u->masked && u->mode == NM_PROMPT) { for (int i = 0; i < shown; i++) nm__row_add(&r, "*", 1); }
		else nm__row_add(&r, u->in + (u->inlen - shown), shown);
		nm__row_raw(&r, t->sel); nm__row_add(&r, " ", 1);
	} else if (u->mode == NM_CONFIRM) {
		nm__row_raw(&r, t->err); nm__row_fmt(&r, -1, " %s %s? ", u->ckind == 1 ? "forget" : "remove", u->cwhat);
	} else if (u->mode == NM_WPS) {
		nm__row_raw(&r, t->key); nm__row_add(&r, " WPS: ", -1); nm__row_raw(&r, t->reset);
		nm__row_add(&r, "p push-button   i pin   c cancel   esc back", -1);
	} else if (u->msg[0]) {
		nm__row_raw(&r, u->msgerr ? t->err : t->ok);
		nm__row_fmt(&r, -1, " %s", u->msg);
	}
	nm__row_emit(&r, t, o, h - 1);
	/* key bar */
	nm__row_init(&r, w);
	nm__row_raw(&r, t->dim);
	nm__row_add(&r, " ", 1);
	nm__row_raw(&r, t->reset);
	nm__keybar(u, t, &r);
	nm__row_emit(&r, t, o, h);
}

/* ---- actions (the only place the library is called) --------------------------------------------------------------- */

static void (*nm__redraw)(nm_ui *) = NULL;
static char *nm__co, *nm__ce;
static size_t nm__cos, nm__ces;

static void nm__cap_begin(void) { nm__co = nm__ce = NULL; nmb_out = open_memstream(&nm__co, &nm__cos); nmb_err = open_memstream(&nm__ce, &nm__ces); }
static void nm__cap_end(void) { fclose(nmb_out); fclose(nmb_err); nmb_out = nmb_err = NULL; }
static void nm__cap_free(void) { free(nm__co); free(nm__ce); nm__co = nm__ce = NULL; }

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

static void nm__busy(nm_ui *u, const char *what)
{
	nm__say(u, 0, what, "...");
	if (nm__redraw) nm__redraw(u);
}

/* report the outcome of a library call that has just been captured */
static void nm__report(nm_ui *u, const char *what, int rc)
{
	char l[200];
	nm__lastline(rc ? (nm__ce && *nm__ce ? nm__ce : nm__co ? nm__co : "") : (nm__co ? nm__co : ""), l, sizeof l);
	if (rc) nm__say(u, 1, what, l[0] ? l : "failed");
	else nm__say(u, 0, what, l[0] ? l : "ok");
}

static void nm__refresh_status(nm_ui *u)
{
	nm__cap_begin();
	nmb_status();
	nm__cap_end();
	u->nifs = nm_parse_status(nm__co ? nm__co : "", u->ifs, NM_MAXIF, u->swifi, u->sbt, sizeof u->swifi < sizeof u->sbt ? sizeof u->swifi : sizeof u->sbt);
	nm__cap_free();
}

static void nm__keep_sel(nm_ui *u, const char *keep)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET);
	if (keep && *keep)
		for (int i = 0; i < n; i++) {
			const char *nm = u->tab == NM_WIFI ? u->nets[idx[i]].name : u->tab == NM_BT ? u->devs[idx[i]].addr : u->ifs[idx[i]].name;
			if (!strcmp(nm, keep)) { u->sel[u->tab] = i; break; }
		}
	nm__clamp(u);
}

static void nm__refresh(nm_ui *u)
{
	char keep[64] = "";
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET), s = u->sel[u->tab];
	if (s < n) nm__copy(keep, sizeof keep, u->tab == NM_WIFI ? u->nets[idx[s]].name : u->tab == NM_BT ? u->devs[idx[s]].addr : u->ifs[idx[s]].name);
	if (u->tab == NM_WIFI) {
		nm_net known[NM_MAXNET];
		int nk, rc;
		nm__busy(u, "scanning");
		nm__cap_begin(); rc = nmb_wifi_list(); nm__cap_end();
		if (rc) nm__report(u, "wifi list", rc);
		else { u->nnets = nm_parse_wifi(nm__co ? nm__co : "", u->nets, NM_MAXNET); u->msg[0] = 0; }
		nm__cap_free();
		nm__cap_begin(); nmb_wifi_known(); nm__cap_end();
		nk = nm_parse_known(nm__co ? nm__co : "", known, NM_MAXNET);
		nm__cap_free();
		for (int i = 0; i < u->nnets; i++)
			for (int j = 0; j < nk; j++)
				if (!strcmp(u->nets[i].name, known[j].name)) { u->nets[i].known = 1; u->nets[i].autoc = known[j].autoc; }
	} else if (u->tab == NM_BT) {
		int rc;
		nm__cap_begin(); rc = nmb_bt_list(); nm__cap_end();
		if (rc) nm__report(u, "bluetooth", rc);
		else { u->ndevs = nm_parse_bt(nm__co ? nm__co : "", u->devs, NM_MAXDEV); u->msg[0] = 0; }
		nm__cap_free();
	}
	nm__refresh_status(u);
	u->loaded[u->tab] = 1;
	nm__keep_sel(u, keep);
}

static const nm_net *nm__cur_net(const nm_ui *u)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET);
	return u->sel[NM_WIFI] < n ? &u->nets[idx[u->sel[NM_WIFI]]] : NULL;
}
static const nm_dev *nm__cur_dev(const nm_ui *u)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET);
	return u->sel[NM_BT] < n ? &u->devs[idx[u->sel[NM_BT]]] : NULL;
}

enum { PK_PASS = 1, PK_WPSPIN, PK_HSSID, PK_HSPASS, PK_BTPIN };

static void nm__prompt(nm_ui *u, int kind, const char *label, int masked)
{
	u->mode = NM_PROMPT; u->pkind = kind; u->masked = masked; u->inlen = 0; u->in[0] = 0;
	nm__copy(u->label, sizeof u->label, label);
}

static void nm__do_connect(nm_ui *u, const char *ssid, const char *pass)
{
	int rc;
	nm__busy(u, "connecting");
	nm__cap_begin(); rc = nmb_wifi_connect(ssid, pass); nm__cap_end();
	nm__report(u, "connect", rc);
	if (!rc && !u->msg[0]) nm__say(u, 0, "connected to", ssid);
	nm__cap_free();
	nm__refresh(u);
	if (!rc) nm__say(u, 0, "connect", "ok");
}

static void nm__prompt_done(nm_ui *u)
{
	int kind = u->pkind;
	char val[128];
	nm__copy(val, sizeof val, u->in);
	u->mode = NM_NORMAL;
	memset(u->in, 0, sizeof u->in);   /* the passphrase does not stay in the struct */
	u->inlen = 0;
	if (kind == PK_PASS) nm__do_connect(u, u->pa, val[0] ? val : NULL);
	else if (kind == PK_WPSPIN) {
		int rc;
		nm__busy(u, "WPS pin");
		nm__cap_begin(); rc = nmb_wifi_wps_pin(val[0] ? val : NULL); nm__cap_end();
		nm__report(u, "wps pin", rc);
		nm__cap_free();
	} else if (kind == PK_HSSID) {
		int rc;
		if (!val[0]) {
			nm__cap_begin(); rc = nmb_wifi_hotspot_stop(); nm__cap_end();
			nm__report(u, "hotspot stop", rc);
			nm__cap_free();
		} else { nm__copy(u->pa, sizeof u->pa, val); nm__prompt(u, PK_HSPASS, "hotspot passphrase (8-63, empty = open): ", 1); }
	} else if (kind == PK_HSPASS) {
		int rc;
		nm__busy(u, "starting hotspot");
		nm__cap_begin(); rc = nmb_wifi_hotspot_start(u->pa, val[0] ? val : NULL); nm__cap_end();
		nm__report(u, "hotspot", rc);
		nm__cap_free();
	} else if (kind == PK_BTPIN) {
		const nm_dev *d = nm__cur_dev(u);
		int rc;
		char addr[18];
		if (!d) return;
		nm__copy(addr, sizeof addr, d->addr);
		nmb_bt_set_pin(val[0] ? val : "0000");
		nm__busy(u, "pairing");
		nm__cap_begin(); rc = nmb_bt_action("pair", addr); nm__cap_end();
		nm__report(u, "pair", rc);
		nm__cap_free();
		nmb_bt_set_pin(NULL);
		nm__refresh(u);
	}
	memset(val, 0, sizeof val);
}

static void nm__confirmed(nm_ui *u)
{
	int rc;
	u->mode = NM_NORMAL;
	if (u->ckind == 1) {
		nm__cap_begin(); rc = nmb_wifi_forget(u->cwhat); nm__cap_end();
		nm__report(u, "forget", rc);
		nm__cap_free();
	} else {
		const nm_dev *d = nm__cur_dev(u);
		char addr[18];
		if (!d) return;
		nm__copy(addr, sizeof addr, d->addr);
		nm__cap_begin(); rc = nmb_bt_action("remove", addr); nm__cap_end();
		nm__report(u, "remove", rc);
		nm__cap_free();
	}
	nm__refresh(u);
}

static void nm__wps(nm_ui *u, int key)
{
	int rc;
	u->mode = NM_NORMAL;
	if (key == 'p') {
		if (!getenv("NMB_WPS_SECS")) setenv("NMB_WPS_SECS", "30", 1);   /* the TUI cannot be interrupted while this waits */
		nm__busy(u, "WPS: press the button on the router");
		nm__cap_begin(); rc = nmb_wifi_wps_pbc(); nm__cap_end();
		nm__report(u, "wps", rc);
		nm__cap_free();
		nm__refresh(u);
	} else if (key == 'i') nm__prompt(u, PK_WPSPIN, "WPS pin (empty = generate): ", 0);
	else if (key == 'c') {
		nm__cap_begin(); rc = nmb_wifi_wps_cancel(); nm__cap_end();
		nm__report(u, "wps cancel", rc);
		nm__cap_free();
	}
}

static void nm__enter(nm_ui *u)
{
	if (u->tab == NM_WIFI) {
		const nm_net *e = nm__cur_net(u);
		if (!e) return;
		if (e->connected) { nm__say(u, 0, "already connected to", e->name); return; }
		if (e->known || !strcmp(e->type, "open")) nm__do_connect(u, e->name, NULL);
		else { nm__copy(u->pa, sizeof u->pa, e->name); nm__prompt(u, PK_PASS, "passphrase: ", 1); }
	} else if (u->tab == NM_BT) {
		const nm_dev *d = nm__cur_dev(u);
		int rc;
		char addr[18];
		if (!d) return;
		nm__copy(addr, sizeof addr, d->addr);
		nm__busy(u, d->c ? "disconnecting" : "connecting");
		nm__cap_begin(); rc = nmb_bt_action(d->c ? "disconnect" : "connect", addr); nm__cap_end();
		nm__report(u, d->c ? "disconnect" : "connect", rc);
		nm__cap_free();
		nm__refresh(u);
	}
}

static void nm__simple_bt(nm_ui *u, const char *action)
{
	const nm_dev *d = nm__cur_dev(u);
	int rc;
	char addr[18];
	if (!d) return;
	nm__copy(addr, sizeof addr, d->addr);
	nm__cap_begin(); rc = nmb_bt_action(action, addr); nm__cap_end();
	nm__report(u, action, rc);
	nm__cap_free();
	nm__refresh(u);
}

static void nm__move(nm_ui *u, int d)
{
	u->sel[u->tab] += d;
	nm__clamp(u);
}

static void nm__switch(nm_ui *u, int tab)
{
	u->tab = (tab + NM_NTAB) % NM_NTAB;
	u->filter[0] = 0;
	u->msg[0] = 0;
	if (!u->loaded[u->tab]) nm__refresh(u);
	nm__clamp(u);
}

static void nm__text_key(nm_ui *u, int key, int is_filter)
{
	if (key == NM_K_ESC) { if (is_filter) u->filter[0] = 0; u->mode = NM_NORMAL; u->inlen = 0; memset(u->in, 0, sizeof u->in); nm__clamp(u); return; }
	if (key == NM_K_ENTER) { if (is_filter) { u->mode = NM_NORMAL; nm__clamp(u); } else nm__prompt_done(u); return; }
	if (key == NM_K_BS) {
		if (u->inlen > 0) { do u->inlen--; while (u->inlen > 0 && ((unsigned char)u->in[u->inlen] & 0xC0) == 0x80); u->in[u->inlen] = 0; }
	} else if (key >= 0x20 && key < 0x100 && u->inlen < (int)sizeof u->in - 1) {
		u->in[u->inlen++] = (char)key; u->in[u->inlen] = 0;
	}
	if (is_filter) { nm__copy(u->filter, sizeof u->filter, u->in); u->sel[u->tab] = 0; u->top[u->tab] = 0; nm__clamp(u); }
}

void nm_ui_key(nm_ui *u, int key)
{
	if (u->mode == NM_HELP) { u->mode = NM_NORMAL; return; }
	if (u->mode == NM_FILTER) { nm__text_key(u, key, 1); return; }
	if (u->mode == NM_PROMPT) { nm__text_key(u, key, 0); return; }
	if (u->mode == NM_CONFIRM) { if (key == 'y' || key == 'Y') nm__confirmed(u); else u->mode = NM_NORMAL; return; }
	if (u->mode == NM_WPS) { nm__wps(u, key); return; }
	switch (key) {
	case 'q': case 3: u->quit = 1; break;
	case '?': u->mode = NM_HELP; break;
	case 9: case 'l': case NM_K_RIGHT: nm__switch(u, u->tab + 1); break;
	case NM_K_BTAB: case 'h': case NM_K_LEFT: nm__switch(u, u->tab - 1); break;
	case '1': case '2': case '3': case '4': nm__switch(u, key - '1'); break;
	case 'j': case NM_K_DOWN: nm__move(u, 1); break;
	case 'k': case NM_K_UP: nm__move(u, -1); break;
	case NM_K_PGDN: nm__move(u, nm__rows(u)); break;
	case NM_K_PGUP: nm__move(u, -nm__rows(u)); break;
	case 'g': case NM_K_HOME: u->sel[u->tab] = 0; nm__clamp(u); break;
	case 'G': case NM_K_END: u->sel[u->tab] = 1 << 20; nm__clamp(u); break;
	case '/': u->mode = NM_FILTER; u->masked = 0; u->inlen = (int)strlen(u->filter); nm__copy(u->in, sizeof u->in, u->filter); break;
	case 'r': nm__refresh(u); break;
	case NM_K_ENTER: nm__enter(u); break;
	default:
		if (u->tab == NM_WIFI) {
			const nm_net *e = nm__cur_net(u);
			int rc;
			if (key == 'd') { nm__cap_begin(); rc = nmb_wifi_disconnect(); nm__cap_end(); nm__report(u, "disconnect", rc); nm__cap_free(); nm__refresh(u); }
			else if (key == 'x' && e) { nm__copy(u->cwhat, sizeof u->cwhat, e->name); u->ckind = 1; u->mode = NM_CONFIRM; }
			else if (key == 'a') {
				if (!e || !e->known) nm__say(u, 1, "autoconnect", "not a saved network");
				else {
					char ssid[64];
					int on = e->autoc != 1;
					nm__copy(ssid, sizeof ssid, e->name);
					nm__cap_begin(); rc = nmb_wifi_autoconnect(ssid, on); nm__cap_end();
					nm__report(u, on ? "autoconnect on" : "autoconnect off", rc);
					nm__cap_free();
					nm__refresh(u);
				}
			}
			else if (key == 'w') u->mode = NM_WPS;
			else if (key == 'H') nm__prompt(u, PK_HSSID, "hotspot SSID (empty = stop): ", 0);
		} else if (u->tab == NM_BT) {
			const nm_dev *d = nm__cur_dev(u);
			if (key == 'p' && d) nm__prompt(u, PK_BTPIN, "pairing PIN (empty = 0000): ", 1);
			else if (key == 't') nm__simple_bt(u, "trust");
			else if (key == 'x' && d) { nm__copy(u->cwhat, sizeof u->cwhat, d->alias[0] ? d->alias : d->addr); u->ckind = 2; u->mode = NM_CONFIRM; }
			else if (key == 's') {
				int rc;
				nm__busy(u, "scanning");
				nm__cap_begin(); rc = nmb_bt_scan(5); nm__cap_end();
				if (rc) nm__report(u, "scan", rc);
				else { u->ndevs = nm_parse_bt(nm__co ? nm__co : "", u->devs, NM_MAXDEV); u->msg[0] = 0; }
				nm__cap_free();
				nm__refresh_status(u);
				nm__clamp(u);
			} else if (key == 'o') {
				int rc, on = strstr(u->sbt, "adapter off") != NULL;
				nm__cap_begin(); rc = nmb_bt_power(on); nm__cap_end();
				nm__report(u, on ? "power on" : "power off", rc);
				nm__cap_free();
				nm__refresh(u);
			}
		}
	}
}

/* ---- terminal ----------------------------------------------------------------------------------------------------- */

static struct termios nm__saved;
static int nm__raw_on;
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

int nm_tui(void)
{
	struct termios raw;
	struct sigaction sa;
	const char *loc;
	int utf;
	if (!isatty(0) || !isatty(1)) { fputs("netmanager: the TUI needs a terminal\n", stderr); return 1; }
	loc = getenv("LC_ALL"); if (!loc || !*loc) loc = getenv("LC_CTYPE"); if (!loc || !*loc) loc = getenv("LANG");
	utf = loc && (strcasestr(loc, "utf-8") || strcasestr(loc, "utf8"));
	nm_theme_default(&nm__theme, utf);
	tcgetattr(0, &nm__saved);
	raw = nm__saved;
	cfmakeraw(&raw);
	raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSAFLUSH, &raw);
	nm__raw_on = 1;
	if (write(1, "\x1b[?1049h\x1b[2J", 12)) {}
	memset(&sa, 0, sizeof sa);
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = nm__on_winch;   /* no SA_RESTART: a resize interrupts the key read so the frame is redrawn */
	sigaction(SIGWINCH, &sa, NULL);
	sa.sa_handler = nm__on_term;
	sigaction(SIGTERM, &sa, NULL); sigaction(SIGHUP, &sa, NULL); sigaction(SIGINT, &sa, NULL);
	nmb_in = fopen("/dev/null", "r");   /* the pairing agent must never read the terminal the UI owns */
	nm_ui_init(&nm__ui, 80, 24);
	nm__size(&nm__ui);
	nm__redraw = nm__paint;
	nm__refresh(&nm__ui);
	while (!nm__ui.quit) {
		int k;
		if (nm__winch) { nm__winch = 0; nm__size(&nm__ui); nm__clamp(&nm__ui); if (write(1, "\x1b[2J", 4)) {} }
		nm__paint(&nm__ui);
		k = nm__read_key();
		if (k == -1) break;   /* stdin closed */
		if (k > 0) nm_ui_key(&nm__ui, k);
	}
	nm__redraw = NULL;
	nm__restore();
	if (nmb_in) { fclose(nmb_in); nmb_in = NULL; }
	return 0;
}

#endif /* NM_TUI_IMPLEMENTATION_ONCE */
#endif /* NM_TUI_IMPLEMENTATION */
