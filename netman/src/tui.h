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
#define NM_MAXDL 120
#define NM_MAXENT 96

typedef struct { char name[96], sec[8], bssid[18], band[4]; int dbm, pct, chan, nbss, hidden, connected, known, autoc; } nm_net;   /* autoc: -1 unknown, 0 off, 1 on; dbm 0 = unknown */
typedef struct { char addr[18], alias[64]; int c, p, t, dbm; } nm_dev;                                                          /* dbm 0 = no reading */
typedef struct { netman_if f; netman_ifstat st; int hasst; } nm_if;
typedef struct { char name[48], kind[16], state[12], ep[80]; int up, active; long long hs, rx, tx; } nm_vpn;   /* up: state "up"; active: up or connecting; hs seconds since the handshake, -1 unknown; rx/tx -1 unknown */
typedef struct { char kind[8], name[48], state[12], detail[132], prof[16]; int lvl; } nm_ent;   /* Enterprise row: lvl 0 ok, 1 attention, 2 bad; prof = saved .ent profile it came from or "" */
typedef struct { char host[64], iface[16], gw[40], ip[64], ipkind[8], dns[96], word[64]; int score, ncheck; } nm_hdr;   /* the one-line header: score -1 = not measured yet */
typedef struct { char key[96]; time_t first, last; } nm_seen;                                                          /* when this session first / last saw a Wi-Fi network */
typedef struct { time_t t; char ssid[48], bssid[18]; } nm_roam;                                                       /* the access point the connected network used, newest last */

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

enum { NM_WIFI, NM_ETH, NM_BT, NM_VPN, NM_ENT, NM_TOR, NM_SSH, NM_MESH, NM_BOND, NM_TUN, NM_NTAB };
enum { NM_NORMAL, NM_FILTER, NM_PROMPT, NM_CONFIRM, NM_HELP, NM_WPS, NM_INFO };
enum { NM_K_UP = 0x101, NM_K_DOWN, NM_K_RIGHT, NM_K_LEFT, NM_K_BTAB, NM_K_PGUP, NM_K_PGDN, NM_K_HOME, NM_K_END, NM_K_TAB = 9, NM_K_ENTER = 13, NM_K_ESC = 27, NM_K_BS = 127 };
enum { NM_SORT_SIGNAL, NM_SORT_NAME, NM_SORT_SECURED, NM_SORT_OPEN, NM_NSORT };

typedef struct {
	int tab, sel[NM_NTAB], top[NM_NTAB], loaded[NM_NTAB], en[NM_NTAB], w, h, mode, quit;
	nm_net nets[NM_MAXNET]; int nnets;
	nm_dev devs[NM_MAXDEV]; int ndevs;
	nm_if ifs[NETMAN_MAXIF]; int nifs; netman_metrics met;
	nm_ent ents[NM_MAXENT]; int nent;
	nm_vpn vpns[NM_MAXVPN]; int nvpn, ks;   /* ks: kill switch table loaded 1, absent 0, cannot tell -1 */
	nm_hdr hdr;
	int big, bigoff, bigdirty, bigtab; char bigkey[112];   /* I: the "all info" view replaces the list; scroll offset; what it was built for */
	char sadp[160];                         /* bluetooth adapter: powered / discoverable / pairable, from the adapter object */
	nm_seen seen[NM_MAXNET]; int nseen;
	nm_roam roam[8]; int nroam;
	char vpndir[200];                       /* where the profiles are expected (for the empty-list hint) */
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

int nm_parse_vpn(const char *text, nm_vpn *o, int max)   /* `vpn list`: "NAME TYPE STATE [ep=HOST:PORT hs=SECS rx=BYTES tx=BYTES]" */
{
	char line[400];
	int n = 0;
	while (n < max && nm__next_line(&text, line, sizeof line)) {
		char a[48], b[16], c[16];
		const char *x;
		if (sscanf(line, "%47s %15s %15s", a, b, c) != 3) continue;
		if (strcmp(c, "up") && strcmp(c, "down") && strcmp(c, "connecting") && strcmp(c, "failed") && strcmp(c, "locked") && strcmp(c, "untrusted")) continue;
		memset(&o[n], 0, sizeof o[n]);
		nm__copy(o[n].name, sizeof o[n].name, a); nm__copy(o[n].kind, sizeof o[n].kind, b); nm__copy(o[n].state, sizeof o[n].state, c);
		o[n].up = !strcmp(c, "up");
		o[n].active = o[n].up || !strcmp(c, "connecting");
		o[n].hs = o[n].rx = o[n].tx = -1;
		if ((x = strstr(line, " ep="))) { sscanf(x + 4, "%79s", o[n].ep); }
		if ((x = strstr(line, " hs="))) o[n].hs = atoll(x + 4);
		if ((x = strstr(line, " rx="))) o[n].rx = atoll(x + 4);
		if ((x = strstr(line, " tx="))) o[n].tx = atoll(x + 4);
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

/* the Tor tab shows its status as read-only rows in the same list as the Enterprise tab */
static int nm__entlike(const nm_ui *u) { return u->tab >= NM_ENT; }   /* Enterprise, Tor, SSH, Mesh, Bond, Tunnel share the row list */
void nm_ui_init(nm_ui *u, int w, int h)
{
	memset(u, 0, sizeof *u);
	u->w = w < 20 ? 20 : w;
	u->h = h < 6 ? 6 : h;
	for (int i = 0; i < NM_NTAB; i++) u->en[i] = 1;
	u->en[NM_ENT] = 0; u->en[NM_TOR] = 0; u->en[NM_SSH] = 0; u->en[NM_MESH] = 0; u->en[NM_BOND] = 0; u->en[NM_TUN] = 0;                      /* the Enterprise tab exists only when enterprise.enabled (nc_tui) */
	u->rssi_good = -55; u->rssi_ok = -65; u->rssi_weak = -75;
	u->hdr.score = -1; u->ks = -1;
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
	int n = 0, total = u->tab == NM_WIFI ? u->nnets : u->tab == NM_BT ? u->ndevs : u->tab == NM_ETH ? u->nifs : u->tab == NM_VPN ? u->nvpn : nm__entlike(u) ? u->nent : 0;
	for (int i = 0; i < total && n < max; i++) {
		const char *a, *b = "";
		if (u->tab == NM_WIFI) {
			const nm_net *e = &u->nets[i];
			if (u->secf == 1 && !nm__net_open(e)) continue;
			if (u->secf == 2 && nm__net_open(e)) continue;
			a = nm__net_label(e); b = e->bssid;
		} else if (u->tab == NM_BT) { a = u->devs[i].alias; b = u->devs[i].addr; }
		else if (u->tab == NM_ETH) { a = u->ifs[i].f.name; b = u->ifs[i].f.naddr ? u->ifs[i].f.addr[0] : ""; }
		else if (nm__entlike(u)) { a = u->ents[i].name; b = u->ents[i].kind; }
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
	static const char *full[] = { "Wi-Fi", "Ethernet/IP", "Bluetooth", "VPN", "Enterprise", "Tor", "SSH", "Mesh", "Bond", "Tunnel" }, *sh[] = { "Wi-Fi", "Eth", "BT", "VPN", "Ent", "Tor", "SSH", "Mesh", "Bond", "Tun" };
	return shortn ? sh[i] : full[i];
}

typedef struct { const char *k, *l; } nm__Key;

static void nm__kb(nm__Row *r, const nm_theme *t, const char *k, const char *l) { nm__row_raw(r, t->key); nm__row_add(r, k, -1); nm__row_raw(r, t->reset); nm__row_fmt(r, -1, " %s  ", l); }

static void nm__keybar(const nm_ui *u, const nm_theme *t, nm__Row *r)
{
	static const nm__Key wifi[] = { { "enter", "connect" }, { "i", "info" }, { "I", "all" }, { "s", "sort" }, { "o", "open/sec" }, { "d", "disc" }, { "x", "forget" }, { "a", "auto" }, { "w", "wps" }, { "H", "hotspot" }, { "r", "rescan" }, { "/", "filter" } };
	static const nm__Key bt[] = { { "enter", "conn" }, { "i", "info" }, { "I", "all" }, { "p", "pair" }, { "t", "trust" }, { "x", "remove" }, { "s", "scan" }, { "o", "power" }, { "/", "filter" } };
	static const nm__Key eth[] = { { "i", "info" }, { "I", "all" }, { "r", "refresh" }, { "/", "filter" } };
	static const nm__Key vpn[] = { { "enter", "up/down" }, { "K", "killsw" }, { "i", "info" }, { "I", "all" }, { "r", "refresh" }, { "/", "filter" } };
	static const nm__Key ent[] = { { "i", "info" }, { "I", "all" }, { "r", "refresh" }, { "/", "filter" } };
	const nm__Key *k = u->tab == NM_WIFI ? wifi : u->tab == NM_BT ? bt : u->tab == NM_ETH ? eth : nm__entlike(u) ? ent : vpn;
	int n = u->tab == NM_WIFI ? 12 : u->tab == NM_BT ? 9 : u->tab == NM_ETH ? 4 : nm__entlike(u) ? 4 : 6, fixed = 14;
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
	/* row 2: the header - what is on every tab: wifi / bt status, hostname, default route, ip, dns, network score.
	 * Segments are placed by priority (the current tab's own status first); one that does not fit whole is dropped. */
	nm__row_init(&r, w);
	{
		struct { char l[220], sh[80]; int prio, put; const char *col; } sg[7];
		static const char *const order = "wbhdinx";
		char wf[260], hs[80] = "";
		const nm_net *cn = NULL;
		const nm_hdr *H = &u->hdr;
		int placed = 0, used = 1;
		for (int i = 0; i < u->nnets; i++) if (u->nets[i].connected) { cn = &u->nets[i]; break; }
		memset(sg, 0, sizeof sg);
		snprintf(wf, sizeof wf, "%s", u->swifi[0] ? u->swifi : "-");
		if (cn && cn->dbm && w >= 50) {
			nm_hist_text(u, t, cn, hs, sizeof hs);
			{ size_t l = strlen(wf); snprintf(wf + l, sizeof wf - l, "  %d dBm %d%%%s%s", cn->dbm, cn->pct < 0 ? 0 : cn->pct, hs[0] ? "  " : "", hs); }
		}
		if (u->en[NM_WIFI]) snprintf(sg[0].l, sizeof sg[0].l, "wifi: %s", wf);
		if (u->en[NM_BT]) snprintf(sg[1].l, sizeof sg[1].l, "bt: %s", u->sbt[0] ? u->sbt : "-");
		if (H->host[0]) snprintf(sg[2].l, sizeof sg[2].l, "%s", H->host);
		if (H->host[0]) {
			if (H->iface[0]) { snprintf(sg[3].l, sizeof sg[3].l, "default %s via %s", H->iface, H->gw[0] ? H->gw : "-"); snprintf(sg[3].sh, sizeof sg[3].sh, "%s", H->iface); }
			else snprintf(sg[3].l, sizeof sg[3].l, "default -");
			if (H->ip[0]) snprintf(sg[4].l, sizeof sg[4].l, "%s %s", H->ipkind, H->ip);
			snprintf(sg[5].l, sizeof sg[5].l, "dns %s", H->dns[0] ? H->dns : "-");
			if (H->score >= 0) {
				static const char *const cls[] = { "bad", "weak", "ok", "good" };
				int c = H->score >= 70 ? 3 : H->score >= 40 ? 2 : H->score >= 20 ? 1 : 0;
				snprintf(sg[6].l, sizeof sg[6].l, "net %d %s%s%s%s", H->score, cls[c], H->word[0] ? " (" : "", H->word, H->word[0] ? ")" : "");
				snprintf(sg[6].sh, sizeof sg[6].sh, "net %d %s", H->score, cls[c]);
				sg[6].col = c == 3 ? t->ok : c == 2 ? t->warn : t->err;
			} else snprintf(sg[6].l, sizeof sg[6].l, "net: %s", H->word[0] ? H->word : "measuring...");
		}
		{
			static const int pw[7] = { 1, 7, 5, 4, 3, 6, 2 }, pb[7] = { 7, 1, 5, 4, 3, 6, 2 }, pe[7] = { 6, 7, 4, 3, 2, 5, 1 };
			const int *pp = u->tab == NM_WIFI ? pw : u->tab == NM_BT ? pb : pe;
			for (int i = 0; i < 7; i++) sg[i].prio = pp[i];
		}
		for (int pr = 1; pr <= 7; pr++)
			for (int i = 0; i < 7; i++) {
				int sep = placed ? 3 : 0;
				if (sg[i].prio != pr || !sg[i].l[0]) continue;
				if (!placed) {   /* the first segment is always shown, cut to the window if needed */
					sg[i].put = 1; placed = 1; used += nm__cells(sg[i].l) < w - 2 ? nm__cells(sg[i].l) : w - 2;
				} else if (used + sep + nm__cells(sg[i].l) <= w - 1) { sg[i].put = 1; placed++; used += sep + nm__cells(sg[i].l); }
				else if (sg[i].sh[0] && used + sep + nm__cells(sg[i].sh) <= w - 1) { sg[i].put = 2; placed++; used += sep + nm__cells(sg[i].sh); }
			}
		nm__row_raw(&r, t->dim);
		nm__row_add(&r, " ", 1);
		{
			int first = 1;
			for (int k = 0; k < 7; k++) {
				int i = (int)(strchr(order, "wbhdinx"[k]) - order);
				char fitb[260];
				if (!sg[i].put) continue;
				if (!first) nm__row_add(&r, " | ", -1);
				first = 0;
				if (sg[i].col) nm__row_raw(&r, sg[i].col);
				nm__fit(fitb, sizeof fitb, sg[i].put == 2 ? sg[i].sh : sg[i].l, w - r.col - 1, t->ell);
				nm__row_add(&r, fitb, -1);
				if (sg[i].col) nm__row_raw(&r, t->dim);
			}
		}
	}
	nm__row_emit(&r, t, o, 2);
	/* row 3: column header (+ the sort / filter state at the right edge) */
	nm__row_init(&r, w);
	{
		char f[140] = "";
		static const char *sn[] = { "signal", "name", "secured first", "open first" }, *sf[] = { "", " only open", " only secured" };
		const char *hd = u->tab == NM_WIFI ? (w >= 56 ? "   sec  network" : "   sec  network") : u->tab == NM_BT ? " cpt  address            name"
		               : u->tab == NM_ETH ? " iface        state  speed    rx/s     tx/s  address / gateway" : nm__entlike(u) ? " kind    name               state     detail" : w >= 70 ? " profile              type      state     endpoint               handshake / transfer" : " profile       type      state";
		if (u->tab == NM_WIFI) snprintf(f, sizeof f, " sort:%s%s%s%s ", sn[u->sortm], sf[u->secf], u->filter[0] ? " /" : "", u->filter);
		else if (u->filter[0] || u->mode == NM_FILTER) snprintf(f, sizeof f, " /%s ", u->filter);
		nm__row_raw(&r, t->head);
		if (u->big && nt) { char bh[200]; snprintf(bh, sizeof bh, w >= 60 ? " all info  (I closes, j/k picks, PgUp/PgDn scrolls)" : " all info  (I closes)"); hd = bh; f[0] = 0; nm__row_add(&r, hd, -1); nm__row_pad(&r, w); nm__row_raw(&r, t->reset); goto hdr_done; }
		nm__row_add(&r, hd, f[0] && w > nm__cells(f) + 20 ? w - nm__cells(f) : -1);
		nm__row_pad(&r, w - (f[0] && w > nm__cells(f) + 20 ? nm__cells(f) : 0));
		nm__row_raw(&r, t->reset);
		if (f[0] && w > nm__cells(f) + 20) { nm__row_raw(&r, t->key); nm__row_add(&r, f, -1); }
	}
	hdr_done:
	nm__row_emit(&r, t, o, 3);
	/* list area; a scrollbar takes the last column when the list is longer than the window */
	for (int i = 0; i < rows; i++) {
		int row = 4 + i, k = u->top[u->tab] + i, selected = k == u->sel[u->tab] && k < n;
		int scroll = n > rows, lw = scroll ? w - 1 : w;
		nm__row_init(&r, lw);
		if (u->big && nt && u->mode != NM_INFO) {   /* the "all info" view: the selected row's details instead of the list */
			int li = u->bigoff + i;
			char bl[200];
			nm__row_init(&r, w);
			if (li < u->ndl) { nm__fit(bl, sizeof bl, u->dl[li], w - 2, t->ell); nm__row_fmt(&r, -1, " %s", bl); }
			if (li == 0 || li >= u->ndl) {}
			nm__row_emit(&r, t, o, row);
			continue;
		}
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
			} else if (nm__entlike(u)) {
				const nm_ent *e = &u->ents[idx[k]];
				char nm[60];
				int nw = lw < 50 ? 12 : 18;
				nm__fit(nm, sizeof nm, e->name, nw, t->ell);
				nm__row_fmt(&r, -1, " %-7s %-*s ", e->kind, nw, nm);
				if (!selected) nm__row_raw(&r, e->lvl == 2 ? t->err : e->lvl == 1 ? t->warn : t->ok);
				nm__row_fmt(&r, -1, "%-9s", e->state);
				if (!selected) nm__row_raw(&r, t->reset);
				nm__row_fmt(&r, -1, " %s", e->detail);
			} else {
				const nm_vpn *e = &u->vpns[idx[k]];
				char nm[60], hs[24] = "-", rx[24] = "-", tx[24] = "-", det[200] = "";
				int nw = lw < 50 ? 12 : lw < 70 ? 16 : 20;
				nm__fit(nm, sizeof nm, e->name, nw, t->ell);
				nm__row_fmt(&r, -1, " %-*s ", nw, nm);
				if (lw >= 44) nm__row_fmt(&r, -1, "%-9s ", e->kind);
				if (!selected) nm__row_raw(&r, e->up ? t->ok : !strcmp(e->state, "down") ? t->dim : t->warn);
				nm__row_fmt(&r, -1, "%-9s", e->state);
				if (!selected) nm__row_raw(&r, t->reset);
				if (e->hs >= 0) snprintf(hs, sizeof hs, "%llds", e->hs);
				if (e->rx >= 0) netman_metrics_fmt_bytes(rx, sizeof rx, (uint64_t)e->rx);
				if (e->tx >= 0) netman_metrics_fmt_bytes(tx, sizeof tx, (uint64_t)e->tx);
				if (lw >= 70) snprintf(det, sizeof det, " %-22s hs %-6s rx %s tx %s", e->ep[0] ? e->ep : "-", hs, rx, tx);
				else if (lw >= 44) snprintf(det, sizeof det, " %s", e->ep[0] ? e->ep : "-");
				nm__row_add(&r, det, -1);
			}
			if (selected) nm__row_pad(&r, lw);
		} else if (i == 0 && n == 0) {
			nm__row_raw(&r, t->dim);
			if (u->tab == NM_VPN && u->loaded[NM_VPN] && !u->filter[0]) nm__row_fmt(&r, -1, "  no VPN profiles in %s", u->vpndir);
			else if (nm__entlike(u) && u->loaded[u->tab] && !u->filter[0]) nm__row_add(&r, u->tab == NM_TOR ? "  tor: no status (netman tor status)" : u->tab == NM_SSH ? "  no ssh profiles (netman ssh save NAME host=...)" : u->tab == NM_MESH ? "  no mesh profiles (netman mesh save NAME kind=...)" : u->tab == NM_BOND ? "  no bonds (netman bond add NAME SLAVE...)" : u->tab == NM_TUN ? "  no tunnels (netman tunnel save NAME kind=...)" : "  no bonds, bridges, vlans, uplinks or licences (netman ent save ...)", -1);
			else nm__row_add(&r, u->filter[0] || u->secf ? "  nothing matches the filter" : u->loaded[u->tab] ? "  nothing found (r to refresh)" : "  loading...", -1);
		} else if (n == 0 && i <= 3 && u->tab == NM_VPN && u->loaded[NM_VPN] && !u->filter[0]) {
			static const char *const hint[] = { "", "  import one:  netman vpn import FILE   (WireGuard .conf / OpenVPN .ovpn)", "  or copy it there yourself (root-owned, mode 0600)", "  other directory: env NETMAN_VPN_CONF_DIR" };
			nm__row_raw(&r, t->dim);
			nm__row_add(&r, hint[i], -1);
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
	} else if (u->tab == NM_VPN && u->mode == NM_NORMAL && u->loaded[NM_VPN]) {   /* kill switch state, and profiles netman cannot use */
		int bad = 0;
		for (int i = 0; i < u->nvpn; i++) if (!strcmp(u->vpns[i].state, "locked") || !strcmp(u->vpns[i].state, "untrusted")) bad++;
		nm__row_raw(&r, bad ? t->warn : t->dim);
		if (bad) nm__row_fmt(&r, -1, " %d profile%s unusable (locked/untrusted: i)  kill switch %s", bad, bad == 1 ? "" : "s", u->ks > 0 ? "on" : u->ks == 0 ? "off" : "?");
		else nm__row_fmt(&r, -1, " kill switch: %s  (K toggles)", u->ks > 0 ? "ON" : u->ks == 0 ? "off" : "unknown (needs root + nft)");
	} else if (nm__entlike(u) && u->mode == NM_NORMAL && u->loaded[u->tab]) {   /* read-only summary */
		int bad = 0;
		for (int i = 0; i < u->nent; i++) if (u->ents[i].lvl) bad++;
		nm__row_raw(&r, bad ? t->warn : t->dim);
		if (bad) nm__row_fmt(&r, -1, " %d of %d need attention (i)  read-only: change with netman ent / licence / key", bad, u->nent);
		else nm__row_fmt(&r, -1, " %d item%s, all fine  (read-only: netman ent / licence / key)", u->nent, u->nent == 1 ? "" : "s");
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

/* ---- Enterprise tab: read-only rows from the saved .ent profiles, the live kernel state and the licence store ---- */

static const char *const nm__ent_order[] = { "bond", "bridge", "vlan", "uplink", "tether", "licence" };

static void nm__ent_add(nm_ui *u, const char *kind, const char *name, const char *state, int lvl, const char *prof, const char *fmt, ...)
{
	nm_ent *e;
	va_list ap;
	if (u->nent >= NM_MAXENT) return;
	e = &u->ents[u->nent++];
	nm__copy(e->kind, sizeof e->kind, kind); nm__copy(e->name, sizeof e->name, name); nm__copy(e->state, sizeof e->state, state); nm__copy(e->prof, sizeof e->prof, prof);
	e->lvl = lvl;
	va_start(ap, fmt);
	vsnprintf(e->detail, sizeof e->detail, fmt, ap);
	va_end(ap);
}
static int nm__ent_rank(const char *kind) { for (unsigned i = 0; i < sizeof nm__ent_order / sizeof *nm__ent_order; i++) if (!strcmp(kind, nm__ent_order[i])) return (int)i; return 99; }
static int nm__ent_has(const nm_ui *u, const char *kind, const char *name) { for (int i = 0; i < u->nent; i++) if (!strcmp(u->ents[i].kind, kind) && !strcmp(u->ents[i].name, name)) return 1; return 0; }

/* link state of an interface as the uplink / tether rows show it: up, nolink, down, missing */
static const char *nm__ent_link(const char *ifn, int *lvl)
{
	netman_if x;
	*lvl = 0;
	if (netman_find(ifn, &x) < 0) { *lvl = 2; return "missing"; }
	if (!x.up) { *lvl = 1; return "down"; }
	if (!x.carrier) { *lvl = 1; return "nolink"; }
	return "up";
}

static void nm__ent_members(char *out, size_t n, const char *dev, const netman_ent *prof)
{
	char m[16][16];
	int k = dev ? netman_ent_members(dev, m, 16) : 0;
	size_t o = 0;
	out[0] = 0;
	if (k > 0) { for (int i = 0; i < k && o + 20 < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", i ? " " : "", m[i]); return; }
	if (prof) for (int i = 0; i < prof->nmember && o + 20 < n; i++) o += (size_t)snprintf(out + o, n - o, "%s%s", i ? " " : "", prof->member[i]);
	if (!out[0]) snprintf(out, n, "none");
}

static void nm__ent_dev_row(nm_ui *u, const char *kind, const char *name, const netman_ent *prof)   /* kind: bond bridge vlan */
{
	netman_ent_info in;
	char mem[120] = "";
	int live = netman_ent_getinfo(name, &in) == 0;
	const char *state = !live ? "absent" : in.up ? "up" : "down";
	int lvl = !live ? 1 : in.up ? 0 : 1;
	if (!strcmp(kind, "bond")) {
		nm__ent_members(mem, sizeof mem, live ? name : NULL, prof);
		nm__ent_add(u, kind, name, state, lvl, prof ? prof->name : "", "%s%s%s members %s%s", prof && prof->mode[0] ? "mode " : "", prof ? prof->mode : "", prof && prof->mode[0] ? " " : "", mem, live ? "" : "  (profile not applied)");
	} else if (!strcmp(kind, "bridge")) {
		nm__ent_members(mem, sizeof mem, live ? name : NULL, prof);
		nm__ent_add(u, kind, name, state, lvl, prof ? prof->name : "", "stp %s%s  ports %s%s", live && in.stp >= 0 ? (in.stp ? "on" : "off") : prof && prof->stp ? "on" : "off", live && in.vlan_filtering > 0 ? "  vlan-filtering" : "", mem, live ? "" : "  (profile not applied)");
	} else {
		char vid[24] = "";
		if (live && in.vid > 0) snprintf(vid, sizeof vid, "vid %d", in.vid); else if (prof && prof->vid > 0) snprintf(vid, sizeof vid, "vid %d", prof->vid);
		nm__ent_add(u, kind, name, state, lvl, prof ? prof->name : "", "%s%s%s%s%s", live && in.kind[0] ? in.kind : prof ? prof->kind : "vlan", vid[0] ? "  " : "", vid, prof && prof->parent[0] ? " on " : "", prof ? prof->parent : "");
	}
}

/* Tor / SSH / Mesh / Bond / Tunnel tabs: each mod prints rows "key state rest" (its `status`); they become list rows. Read-only: the CLI changes things. */
static void nm__status_rows(nm_ui *u, const char *kind, char *buf)
{
	char *sv = NULL;
	u->nent = 0;
	if (buf)
		for (char *ln = strtok_r(buf, "\n", &sv); ln; ln = strtok_r(NULL, "\n", &sv)) {
			char key[40]; size_t k = strcspn(ln, " ");
			if (k >= sizeof key) k = sizeof key - 1;
			memcpy(key, ln, k); key[k] = 0;
			const char *rest = ln + k; while (*rest == ' ') rest++;
			char st[12]; size_t w = strcspn(rest, " "); if (w >= sizeof st) w = sizeof st - 1; memcpy(st, rest, w); st[w] = 0;
			int lvl = !strcmp(st, "down") || !strcmp(st, "degraded") || !strcmp(st, "connecting") ? 1 : 0;
			if (!strcmp(kind, "tor")) lvl = !strcmp(key, "tor") && strcmp(st, "running") ? 1 : 0;
			nm__ent_add(u, kind, key, st, lvl, "", "%s", rest);
		}
}
static void nm__tor_refresh(nm_ui *u)
{
	netman_tor_cfg c; char *buf = NULL; size_t bl = 0;
	FILE *m = open_memstream(&buf, &bl);
	nc_tor_cfg(&c);
	if (m) { c.out = m; c.err = m; netman_tor_status(&c); fclose(m); }
	nm__status_rows(u, "tor", buf);
	free(buf);
	u->loaded[NM_TOR] = 1;
}
static void nm__mod_refresh(nm_ui *u)
{
	char *buf = NULL; size_t bl = 0;
	FILE *m = open_memstream(&buf, &bl);
	const char *kind = u->tab == NM_SSH ? "ssh" : u->tab == NM_MESH ? "mesh" : u->tab == NM_BOND ? "bond" : "tunnel";
	nc_mod_globals();
	if (m) {
		if (u->tab == NM_SSH) { netman_ssh_cfg c; nc_ssh_cfg(&c); netman_ssh_status(&c, m); }
		else if (u->tab == NM_MESH) netman_mesh_status(m);
		else if (u->tab == NM_BOND) netman_bond_status(m);
		else netman_tunnel_status(m);
		fclose(m);
	}
	nm__status_rows(u, kind, buf);
	free(buf);
	u->loaded[u->tab] = 1;
}
static void nm__ent_refresh(nm_ui *u)
{
	if (u->tab == NM_TOR) { nm__tor_refresh(u); return; }
	if (u->tab >= NM_SSH) { nm__mod_refresh(u); return; }
	char names[64][64];
	int k = netman_ent_list(names, 64);
	netman_if t[NETMAN_MAXIF];
	int nif;
	u->nent = 0;
	for (int i = 0; i < k; i++) {
		netman_ent e;
		if (netman_ent_load(names[i], &e) != 0) { nm__ent_add(u, "profile", names[i], "invalid", 2, names[i], "%s", netman_ent_error()); continue; }
		if (!strcmp(e.kind, "bond") || !strcmp(e.kind, "team")) nm__ent_dev_row(u, "bond", e.name, &e);
		else if (!strcmp(e.kind, "bridge")) nm__ent_dev_row(u, "bridge", e.name, &e);
		else if (!strcmp(e.kind, "vlan") || !strcmp(e.kind, "macvlan") || !strcmp(e.kind, "ipvlan")) nm__ent_dev_row(u, "vlan", e.name, &e);
		else if (!strcmp(e.kind, "ecmp")) {
			for (int j = 0; j < e.nup; j++) {
				char nm[64];
				int lvl;
				const char *st = nm__ent_link(e.up[j].ifn, &lvl);
				snprintf(nm, sizeof nm, "%s:%s", e.name, e.up[j].ifn);
				nm__ent_add(u, "uplink", nm, st, lvl, e.name, "gw %s  weight %d%s%s", e.up[j].gw[0] ? e.up[j].gw : "-", e.up[j].weight, e.up[j].probe[0] ? "  probe " : "", e.up[j].probe);
			}
		} else if (!strcmp(e.kind, "tether") && nc_enabled("tether")) {
			int lvl;
			const char *st = nm__ent_link(e.name, &lvl);
			nm__ent_add(u, "tether", e.name, st, lvl, e.name, "%s%s%s%s", e.mode[0] ? e.mode : "share", e.upstream[0] ? "  upstream " : "", e.upstream, "  (profile)");
		}
	}
	nif = netman_list(t, NETMAN_MAXIF);   /* live bonds / bridges / vlans no profile of ours made */
	for (int i = 0; i < nif; i++) {
		netman_ent_info in;
		const char *kind;
		if (netman_ent_getinfo(t[i].name, &in) != 0) continue;
		kind = !strcmp(in.kind, "bond") ? "bond" : !strcmp(in.kind, "bridge") ? "bridge" : !strcmp(in.kind, "vlan") || !strcmp(in.kind, "macvlan") || !strcmp(in.kind, "ipvlan") ? "vlan" : NULL;
		if (kind && !nm__ent_has(u, kind, t[i].name)) nm__ent_dev_row(u, kind, t[i].name, NULL);
	}
	if (nc_enabled("tether")) {
		netman_ent_tether_dev d[16];
		int nd = netman_ent_tether_detect(d, 16);
		for (int i = 0; i < nd; i++) {
			int lvl;
			const char *st;
			if (nm__ent_has(u, "tether", d[i].ifn)) continue;
			st = nm__ent_link(d[i].ifn, &lvl);
			nm__ent_add(u, "tether", d[i].ifn, st, lvl, "", "%s %s  (detected)", d[i].kind, d[i].driver);
		}
	}
	if (nc_enabled("licence")) {
		netman_lic l[32];
		int nl = netman_lic_list(l, 32);
		for (int i = 0; i < nl; i++) {
			char when[40];
			struct tm tm;
			time_t ex = l[i].expires;
			if (ex) { ex -= 1; gmtime_r(&ex, &tm); strftime(when, sizeof when, "%Y-%m-%d", &tm); } else snprintf(when, sizeof when, "never");
			nm__ent_add(u, "licence", l[i].name, l[i].state == 2 ? "expired" : l[i].state == 1 ? "expiring" : "valid", l[i].state == 2 ? 2 : l[i].state == 1 ? 1 : 0, "", "%s%s%s  expires %s%s", l[i].holder, l[i].holder[0] ? "  " : "", l[i].kind, when, l[i].days == 99999 ? "" : l[i].days < 0 ? "  (expired)" : "");
		}
	}
	for (int i = 1; i < u->nent; i++) {   /* stable insertion sort by kind, then name */
		nm_ent v = u->ents[i];
		int j = i - 1;
		while (j >= 0 && (nm__ent_rank(u->ents[j].kind) > nm__ent_rank(v.kind) || (nm__ent_rank(u->ents[j].kind) == nm__ent_rank(v.kind) && strcmp(u->ents[j].name, v.name) > 0))) { u->ents[j + 1] = u->ents[j]; j--; }
		u->ents[j + 1] = v;
	}
	u->loaded[u->tab] = 1;
}

static void nm__keep_sel(nm_ui *u, const char *keep)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET);
	if (keep && *keep)
		for (int i = 0; i < n; i++) {
			const char *nm = u->tab == NM_WIFI ? nm__hkey(&u->nets[idx[i]]) : u->tab == NM_BT ? u->devs[idx[i]].addr : u->tab == NM_ETH ? u->ifs[idx[i]].f.name : nm__entlike(u) ? u->ents[idx[i]].name : u->vpns[idx[i]].name;
			if (!strcmp(nm, keep)) { u->sel[u->tab] = i; break; }
		}
	nm__clamp(u);
}

static void nm__keyof(const nm_ui *u, char *keep, size_t n)
{
	int idx[NM_MAXNET], c = nm_ui_visible(u, idx, NM_MAXNET), s = u->sel[u->tab];
	keep[0] = 0;
	if (s < c) nm__copy(keep, n, u->tab == NM_WIFI ? nm__hkey(&u->nets[idx[s]]) : u->tab == NM_BT ? u->devs[idx[s]].addr : u->tab == NM_ETH ? u->ifs[idx[s]].f.name : nm__entlike(u) ? u->ents[idx[s]].name : u->vpns[idx[s]].name);
}

static int nm__scan_req;   /* set only by the `r` key: everything else reads cached scan results */

static void nm__seen_note(nm_ui *u);
static void nm__roam_note(nm_ui *u, const char *ssid, const char *bssid);

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
		nm__seen_note(u);
		for (int i = 0; i < u->nnets; i++) if (u->nets[i].connected) nm__roam_note(u, u->nets[i].name, u->nets[i].bssid);
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
	} else if (nm__entlike(u)) {
		nm__ent_refresh(u);
	} else if (u->tab == NM_VPN) {
		int rc = NM_CALL(u, "vpn profiles", netman_vpn_list());
		if (rc) nm__report(u, "vpn", rc);
		else {
			u->nvpn = nm_parse_vpn(nm__o(), u->vpns, NM_MAXVPN); u->msg[0] = 0;
			u->ks = strstr(nm__o(), "killswitch on") ? 1 : strstr(nm__o(), "killswitch off") ? 0 : -1;
		}
		nm__copy(u->vpndir, sizeof u->vpndir, netman_vpn_conf_dir);
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

/* value of KEY in "key   value" backend text (the key field is KW cells wide); 0 when the backend did not say */
static int nm__kv(const char *text, int kw, const char *key, char *out, size_t n)
{
	char line[300];
	const char *p = text;
	out[0] = 0;
	while (nm__next_line(&p, line, sizeof line)) {
		char k[48];
		if ((int)strlen(line) <= kw || kw >= (int)sizeof k) continue;
		memcpy(k, line, (size_t)kw); k[kw] = 0;
		nm__rstrip(k);
		if (strcmp(k, key)) continue;
		nm__copy(out, n, line + kw + strspn(line + kw, " "));
		return out[0] != 0;
	}
	return 0;
}

static void nm__dlv(nm_ui *u, const char *label, const char *val) { nm__dl(u, "%-12s %s", label, val && *val ? val : "-"); }

static void nm__ago(time_t t, char *out, size_t n)
{
	long d = (long)(time(NULL) - t);
	struct tm tm;
	char hm[16];
	if (!t) { snprintf(out, n, "-"); return; }
	localtime_r(&t, &tm);
	strftime(hm, sizeof hm, "%H:%M:%S", &tm);
	if (d < 0) d = 0;
	if (d < 90) snprintf(out, n, "%s (%lds ago)", hm, d);
	else if (d < 5400) snprintf(out, n, "%s (%ldm ago)", hm, d / 60);
	else snprintf(out, n, "%s (%ldh ago)", hm, d / 3600);
}

static void nm__sysfs(const char *ifn, const char *file, char *out, size_t n)   /* first line of /sys/class/net/IFN/FILE, "" when unreadable */
{
	char p[160];
	FILE *f;
	out[0] = 0;
	snprintf(p, sizeof p, "/sys/class/net/%s/%s", ifn, file);
	if ((f = fopen(p, "r"))) { if (fgets(out, (int)n, f)) nm__rstrip(out); fclose(f); }
}

static void nm__seen_note(nm_ui *u)
{
	time_t now = time(NULL);
	for (int i = 0; i < u->nnets; i++) {
		const char *k = nm__hkey(&u->nets[i]);
		nm_seen *s = NULL;
		for (int j = 0; j < u->nseen; j++) if (!strcmp(u->seen[j].key, k)) { s = &u->seen[j]; break; }
		if (!s) { if (u->nseen >= NM_MAXNET) continue; s = &u->seen[u->nseen++]; nm__copy(s->key, sizeof s->key, k); s->first = now; }
		s->last = now;
	}
}

static void nm__roam_note(nm_ui *u, const char *ssid, const char *bssid)
{
	nm_roam *l = u->nroam ? &u->roam[u->nroam - 1] : NULL;
	if (!bssid || strlen(bssid) != 17) return;
	if (l && !strcmp(l->ssid, ssid) && !strcasecmp(l->bssid, bssid)) return;
	if (u->nroam == 8) { memmove(u->roam, u->roam + 1, 7 * sizeof u->roam[0]); u->nroam = 7; }
	l = &u->roam[u->nroam++];
	l->t = time(NULL);
	nm__copy(l->ssid, sizeof l->ssid, ssid); nm__copy(l->bssid, sizeof l->bssid, bssid);
}

static const char *nm__wifi_iface(void);
static void nm__ip_lines(nm_ui *u, const char *ifname, const char *leasekey);
static void nm__speed_lines(nm_ui *u, const char *ifname);

static void nm__info_wifi(nm_ui *u, const nm_net *e, int full)
{
	char raw[96], text[4096], v[300], hs[80], tm[64];
	int rc, haveinfo = 0;
	const nm_seen *sn = NULL;
	netman_wifi_unescape(e->name, raw, sizeof raw);
	rc = NM_CALL(u, "network details", netman_wifi_info(e->hidden ? NULL : raw));
	nm__copy(text, sizeof text, rc ? "" : nm__o());
	haveinfo = !rc;
	if (rc && e->connected) nm__dl(u, "(backend gave no extra details: %s)", nm__e());
	nm__cap_free();
	nm__dlv(u, "network", e->hidden ? "<hidden>" : e->name);
	if (haveinfo && nm__kv(text, 14, "bssid", v, sizeof v)) {   /* every access point of this SSID */
		char *sv = NULL, *tok;
		int first = 1;
		for (tok = strtok_r(v, " ", &sv); tok; tok = strtok_r(NULL, " ", &sv)) { nm__dlv(u, first ? "bssids" : "", tok); first = 0; }
	} else nm__dlv(u, "bssids", e->bssid);
	nm__dl(u, "%-12s %d", "access points", e->nbss);
	if (haveinfo && nm__kv(text, 14, "bssid (now)", v, sizeof v)) { nm__dlv(u, "connected to", v); if (e->connected) nm__roam_note(u, e->name, v); }
	if (haveinfo && nm__kv(text, 14, "security", v, sizeof v)) nm__dlv(u, "security", v); else nm__dlv(u, "security", e->sec);
	nm__dlv(u, "  pairwise", haveinfo && nm__kv(text, 14, "cipher", v, sizeof v) ? v : "");
	nm__dlv(u, "  group", haveinfo && nm__kv(text, 14, "group cipher", v, sizeof v) ? v : "");
	nm__dlv(u, "  akm", haveinfo && nm__kv(text, 14, "auth", v, sizeof v) ? v : "");
	nm__dlv(u, "  wps", haveinfo && nm__kv(text, 14, "wps", v, sizeof v) ? v : "");
	{   /* channel + width, signal + average, rx/tx bitrate share a row each: the details window is short */
		char ch[200] = "", wd[80] = "", sg[200] = "", av[80] = "", rx[80] = "", tx[80] = "", no[80] = "";
		if (haveinfo) {
			nm__kv(text, 14, "channel", ch, sizeof ch); nm__kv(text, 14, "width", wd, sizeof wd);
			if (!nm__kv(text, 14, "signal now", sg, sizeof sg)) nm__kv(text, 14, "signal", sg, sizeof sg);
			nm__kv(text, 14, "signal avg", av, sizeof av);
			if (!nm__kv(text, 14, "rx rate", rx, sizeof rx)) nm__kv(text, 14, "link speed", rx, sizeof rx);
			if (!nm__kv(text, 14, "tx rate", tx, sizeof tx)) nm__kv(text, 14, "link speed", tx, sizeof tx);
			if (!nm__kv(text, 14, "noise", no, sizeof no)) nm__kv(text, 14, "noise / SNR", no, sizeof no);
		}
		if (!ch[0] && e->chan) snprintf(ch, sizeof ch, "%d (%s GHz)", e->chan, e->band[0] ? e->band : "?");
		if (!sg[0] && e->dbm) snprintf(sg, sizeof sg, "%d dBm  %d%%", e->dbm, e->pct < 0 ? 0 : e->pct);
		snprintf(v, sizeof v, "%s   width %s", ch[0] ? ch : "-", wd[0] ? wd : "-"); nm__dlv(u, "channel", v);
		snprintf(v, sizeof v, "%s   average %s", sg[0] ? sg : "-", av[0] ? av : "-"); nm__dlv(u, "signal", v);
		nm__dlv(u, "noise", no);
		snprintf(v, sizeof v, "rx %s   tx %s", rx[0] ? rx : "-", tx[0] ? tx : "-"); nm__dlv(u, "bitrate", v);
	}
	snprintf(v, sizeof v, "%s  autoconnect %s", e->known ? "yes" : "no", e->autoc == 1 ? "on" : e->autoc == 0 ? "off" : "-");
	nm__dlv(u, "saved", v);
	nm__dlv(u, "connected", e->connected ? "yes" : "no");
	if (haveinfo) nm__dl_text(u, text, "ssid bssid security channel signal cipher group auth wps width noise rx tx link access");   /* what the backend said beyond the rows above */
	for (int j = 0; j < u->nseen; j++) if (!strcmp(u->seen[j].key, nm__hkey(e))) sn = &u->seen[j];
	nm__ago(sn ? sn->first : 0, tm, sizeof tm); nm__dlv(u, "first seen", sn ? tm : "");
	nm__ago(sn ? sn->last : 0, tm, sizeof tm); nm__dlv(u, "last seen", sn ? tm : "");
	{   /* roaming between access points of this SSID, as netman saw it since it started */
		int shown = 0;
		for (int j = u->nroam - 1; j >= 0 && shown < 4; j--) {
			if (strcmp(u->roam[j].ssid, e->name)) continue;
			nm__ago(u->roam[j].t, tm, sizeof tm);
			snprintf(v, sizeof v, "%s on %s", u->roam[j].bssid, tm);
			nm__dlv(u, shown ? "" : "roam history", v);
			shown++;
		}
		if (!shown) nm__dlv(u, "roam history", e->connected ? "no access point change seen yet" : "");
	}
	if (nm_hist_text(u, &(nm_theme){ .up = "^", .down = "v", .flat = "=" }, e, hs, sizeof hs)) nm__dlv(u, "history", hs);
	if (e->connected) { const char *ifn = nm__wifi_iface(); nm__ip_lines(u, ifn, raw); nm__speed_lines(u, ifn); }
	if (full && haveinfo) { nm__dl(u, "--- backend, as reported ---"); nm__dl_text(u, text, ""); }
}

static void nm__info_eth(nm_ui *u, const nm_if *e, int full)
{
	char v[200], w[100], key[64] = "";
	int any = 0;
	netman_profile pr;
	const netman_ifstat *st = e->hasst ? &e->st : NULL;
	nm__dlv(u, "interface", e->f.name);
	snprintf(v, sizeof v, "%s", !e->f.up ? "down (admin)" : e->f.carrier ? "up" : "up, no carrier");
	nm__dlv(u, "state", v);
	nm__sysfs(e->f.name, "device/driver", w, sizeof w);   /* a directory symlink on real hardware; fopen fails on it, so read the link */
	{
		char lp[160], tgt[200];
		ssize_t n;
		snprintf(lp, sizeof lp, "/sys/class/net/%s/device/driver", e->f.name);
		n = readlink(lp, tgt, sizeof tgt - 1);
		if (n > 0) { tgt[n] = 0; nm__dlv(u, "driver", strrchr(tgt, '/') ? strrchr(tgt, '/') + 1 : tgt); }
		else nm__dlv(u, "driver", "");
	}
	snprintf(v, sizeof v, "%02x:%02x:%02x:%02x:%02x:%02x", e->f.mac[0], e->f.mac[1], e->f.mac[2], e->f.mac[3], e->f.mac[4], e->f.mac[5]);
	nm__dlv(u, "mac", v);
	snprintf(v, sizeof v, "%d", e->f.mtu); nm__dlv(u, "mtu", e->f.mtu > 0 ? v : "");
	{
		char sp[32] = "", du[32] = "";
		if (st && st->speed_mbps > 0) snprintf(sp, sizeof sp, "%d Mbit/s", st->speed_mbps);
		nm__sysfs(e->f.name, "duplex", du, sizeof du);
		snprintf(v, sizeof v, "%s%s%s", sp[0] ? sp : "-", sp[0] ? "  " : "", du[0] && strcmp(du, "unknown") ? du : "");
		nm__dlv(u, "speed/duplex", sp[0] || (du[0] && strcmp(du, "unknown")) ? v : "");
	}
	nm__dlv(u, "link detected", e->f.carrier ? "yes" : "no");
	for (int a = 0; a < e->f.naddr; a++) if (!strchr(e->f.addr[a], ':')) { nm__dlv(u, any ? "" : "ipv4", e->f.addr[a]); any = 1; }
	if (!any) nm__dlv(u, "ipv4", "");
	any = 0;
	for (int a = 0; a < e->f.naddr; a++) if (strchr(e->f.addr[a], ':')) { nm__dlv(u, any ? "" : "ipv6", e->f.addr[a]); any = 1; }
	if (!any) nm__dlv(u, "ipv6", "");
	nm__dlv(u, "gateway", e->f.gw4); nm__dlv(u, "gateway v6", e->f.gw6);
	{
		FILE *fp = fopen(netman_resolv_path, "r");
		char l[200], dns[160] = "";
		while (fp && fgets(l, sizeof l, fp)) if (!strncmp(l, "nameserver", 10)) { nm__rstrip(l); snprintf(dns + strlen(dns), sizeof dns - strlen(dns), "%s%s", dns[0] ? " " : "", l + 10 + strspn(l + 10, " \t")); }
		if (fp) fclose(fp);
		nm__dlv(u, "dns", dns);
	}
	{   /* the DHCP lease netman cached for a profile on this interface (the profile name is the cache key) */
		char names[16][64];
		int np = netman_profile_list(names, 16), found = 0;
		for (int i = 0; i < np && !found; i++)
			if (netman_profile_load(names[i], &pr) == 0 && !strcmp(pr.iface, e->f.name) && pr.dhcp) { nm__copy(key, sizeof key, names[i]); found = 1; }
		if (found) {
			netman_lease le;
			if (netman_lease_load(key, e->f.name, &le) == 0) {
				char ip[32] = "", sv[32] = "";
				inet_ntop(AF_INET, &le.server, sv, sizeof sv); inet_ntop(AF_INET, &le.ip, ip, sizeof ip);
				nm__dlv(u, "dhcp server", le.server ? sv : "");
				snprintf(v, sizeof v, "%s  %us  (renew %us, rebind %us)", ip, (unsigned)le.lease_s, (unsigned)le.t1, (unsigned)le.t2);
				nm__dlv(u, "dhcp lease", v);
				nm__dlv(u, "domain", le.domain);
			} else nm__dlv(u, "dhcp lease", "none cached (expired or not acquired by netman)");
		} else nm__dlv(u, "dhcp lease", "- (no netman profile with dhcp for this interface)");
	}
	if (st) {
		char rx[24], tx[24], b1[24], b2[24];
		netman_metrics_fmt_rate(rx, sizeof rx, st->rx_bps); netman_metrics_fmt_rate(tx, sizeof tx, st->tx_bps);
		netman_metrics_fmt_bytes(b1, sizeof b1, st->rx_bytes); netman_metrics_fmt_bytes(b2, sizeof b2, st->tx_bytes);
		snprintf(v, sizeof v, "%s now, %s total", rx, b1); nm__dlv(u, "rx", v);
		snprintf(v, sizeof v, "%s now, %s total", tx, b2); nm__dlv(u, "tx", v);
		snprintf(v, sizeof v, "rx %llu tx %llu", (unsigned long long)st->rx_err, (unsigned long long)st->tx_err); nm__dlv(u, "errors", v);
		snprintf(v, sizeof v, "rx %llu tx %llu", (unsigned long long)st->rx_drop, (unsigned long long)st->tx_drop); nm__dlv(u, "drops", v);
	} else { nm__dlv(u, "rx", ""); nm__dlv(u, "tx", ""); }
	for (int i = 0; i < e->f.nroute; i++) nm__dlv(u, i ? "" : "routes", e->f.route[i]);
	if (full) {
		static const char *const f[] = { "operstate", "address", "broadcast", "tx_queue_len", "type", "flags", "carrier_changes", "dev_id" };
		nm__dl(u, "--- sysfs ---");
		for (unsigned i = 0; i < sizeof f / sizeof *f; i++) { nm__sysfs(e->f.name, f[i], v, sizeof v); if (v[0]) nm__dl(u, "%-16s %s", f[i], v); }
	}
}

static void nm__info_bt(nm_ui *u, const nm_dev *d, int full)
{
	int rc = NM_CALL(u, "device details", netman_bt_info(d->addr));
	nm__dl(u, "%s", d->alias[0] ? d->alias : d->addr);
	if (!rc) nm__dl_text(u, nm__o(), ""); else nm__dl(u, "(%s)", nm__e());
	if (d->dbm) nm__dl(u, "rssi         %d dBm", d->dbm);
	nm__cap_free();
	rc = NM_CALL(u, "adapter details", netman_bt_adapter());
	nm__dl(u, "--- adapter ---");
	if (!rc) nm__dl_text(u, nm__o(), ""); else nm__dl(u, "(%s)", nm__e());
	nm__cap_free();
	(void)full;
}

static void nm__info_vpn(nm_ui *u, const nm_vpn *e, int full)
{
	int rc;
	char name[48];
	nm__copy(name, sizeof name, e->name);
	if (!strcmp(e->state, "locked") || !strcmp(e->state, "untrusted")) {
		nm__dlv(u, "profile", name);
		nm__dlv(u, "state", e->state);
		nm__dl(u, !strcmp(e->state, "locked") ? "the file is not readable by this user: run netman as root" : "the file must be owned by root (or you) and not writable by group/others (chmod 600)");
		return;
	}
	rc = NM_CALL(u, "vpn details", netman_vpn_info(name));
	nm__dlv(u, "name", name);
	if (!rc) nm__dl_text(u, nm__o(), ""); else nm__dl(u, "(%s)", nm__e());
	if (rc || !strstr(nm__o(), "kill switch")) nm__dlv(u, "kill switch", u->ks > 0 ? "on (nft table inet netman_vpn_ks)" : u->ks == 0 ? "off" : "unknown (needs root and nft)");
	nm__cap_free();
	if (full) {
		rc = NM_CALL(u, "vpn status", netman_vpn_status(name));
		nm__dl(u, "--- status ---");
		nm__dl_text(u, nm__o(), ""); nm__dl_text(u, nm__e(), "");
		nm__cap_free();
	}
}

static void nm__info_ent(nm_ui *u, const nm_ent *e, int full)
{
	nm__dlv(u, "kind", e->kind); nm__dlv(u, "name", e->name); nm__dlv(u, "state", e->state);
	nm__dlv(u, "detail", e->detail);
	if (!strcmp(e->kind, "licence")) {
		netman_lic l;
		if (netman_lic_load(e->name, &l) == 0) {
			nm__dlv(u, "holder", l.holder); nm__dlv(u, "licence kind", l.kind);
			if (l.days == 99999) nm__dlv(u, "expires", "never"); else { char v[64]; snprintf(v, sizeof v, "%d day%s %s", l.days < 0 ? -l.days : l.days, (l.days == 1 || l.days == -1) ? "" : "s", l.days < 0 ? "ago" : "left"); nm__dlv(u, "expires in", v); }
			nm__dlv(u, "warn window", netman_config_str(&nc_cfg, NC_LIC_WARN));
		} else nm__dlv(u, "licence file", netman_ent_error());
		nm__dlv(u, "directory", netman_lic_dir);
		nm__dl(u, "(keys are never shown; renew: netman licence renew NAME)");
		return;
	}
	if (e->prof[0]) {
		netman_ent p;
		char *buf = NULL;
		size_t sz = 0;
		FILE *m;
		nm__dlv(u, "profile", e->prof);
		if (netman_ent_load(e->prof, &p) == 0 && (m = open_memstream(&buf, &sz))) {
			nm__dl(u, "--- live state ---");
			netman_ent_show(&p, m);
			fclose(m);
			if (buf) nm__dl_text(u, buf, "");
			free(buf);
			buf = NULL;
			if (full) {
				char raw[8192];
				if (netman_ent_format(&p, raw, sizeof raw) >= 0) { nm__dl(u, "--- profile (%s.ent) ---", e->prof); nm__dl_text(u, raw, "password"); }
			}
		}
	} else if (full || strcmp(e->kind, "tether")) nm__dl(u, "not created by a netman profile");
	{
		netman_ent_info in;
		if (strcmp(e->kind, "uplink") && netman_ent_getinfo(e->name, &in) == 0) {
			char v[80];
			snprintf(v, sizeof v, "index %d, mtu %d, master %d", in.idx, in.mtu, in.master);
			nm__dl(u, "--- kernel ---"); nm__dlv(u, "link", v);
		}
	}
}

/* fills u->dl for the selected row of the current tab; full = the "all info" view (adds the raw backend / sysfs dump) */
static int nm__info_build(nm_ui *u, int full)
{
	int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET), s = u->sel[u->tab];
	u->ndl = 0;
	if (s >= n) return 0;
	if (u->tab == NM_WIFI) nm__info_wifi(u, &u->nets[idx[s]], full);
	else if (u->tab == NM_BT) nm__info_bt(u, &u->devs[idx[s]], full);
	else if (u->tab == NM_ETH) nm__info_eth(u, &u->ifs[idx[s]], full);
	else if (nm__entlike(u)) nm__info_ent(u, &u->ents[idx[s]], full);
	else if (u->tab == NM_VPN) nm__info_vpn(u, &u->vpns[idx[s]], full);
	if (!u->ndl) nm__dl(u, "no details");
	return 1;
}

void nm_info_open(nm_ui *u)
{
	u->hoff = 0;
	if (!nm__info_build(u, 0)) { nm__say(u, 1, "details", "nothing selected"); return; }
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

/* vpn up, plus the kill switch when vpn.killswitch is set - the same rule `netman vpn up` follows */
static int nm__vpn_up_ks(const char *name)
{
	int rc = netman_vpn_up(name, 30);
	if (!rc && netman_config_bool(&nc_cfg, NC_VPN_KILLSWITCH)) rc = netman_vpn_killswitch(name, 1);
	return rc;
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
		if (!strcmp(u->vpns[idx[u->sel[NM_VPN]]].state, "locked")) { nm__say(u, 1, "vpn", "profile not readable by this user: run netman as root (sudo)"); return; }
		if (!strcmp(u->vpns[idx[u->sel[NM_VPN]]].state, "untrusted")) { nm__say(u, 1, "vpn", "profile must be root- or self-owned and not group/world writable (chmod 600)"); return; }
		rc = NM_CALL(u, up ? "vpn down" : "vpn up", up ? netman_vpn_down(name) : nm__vpn_up_ks(name));
		nm__report(u, up ? "vpn down" : "vpn up", rc);
		nm__cap_free();
		{ char m[sizeof u->msg]; int me = u->msgerr; memcpy(m, u->msg, sizeof m); nm_refresh(u); memcpy(u->msg, m, sizeof m); u->msgerr = me; }   /* the refresh must not wipe the result */
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

static void nm__ui_key0(nm_ui *u, int key)
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
	case '1': case '2': case '3': case '4': case '5': {
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
		} else if (u->tab == NM_VPN) {
			int idx[NM_MAXNET], n = nm_ui_visible(u, idx, NM_MAXNET);
			if (key == 'K') {
				if (u->sel[NM_VPN] >= n) nm__say(u, 1, "kill switch", "no profile selected");
				else {
					char name[48];
					int on = u->ks != 1, rc;
					nm__copy(name, sizeof name, u->vpns[idx[u->sel[NM_VPN]]].name);
					rc = NM_CALL(u, on ? "kill switch on" : "kill switch off", netman_vpn_killswitch(name, on));
					nm__report(u, on ? "kill switch on" : "kill switch off", rc);
					nm__cap_free();
					{ char m[sizeof u->msg]; int me = u->msgerr; memcpy(m, u->msg, sizeof m); nm_refresh(u); memcpy(u->msg, m, sizeof m); u->msgerr = me; }
				}
			}
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

static void nm__big_build(nm_ui *u)
{
	char keep[100];
	if (!nm__info_build(u, 1)) { u->ndl = 0; nm__dl(u, "nothing selected"); }
	nm__keyof(u, keep, sizeof keep);
	snprintf(u->bigkey, sizeof u->bigkey, "%d:%s", u->tab, keep);
	if (u->bigoff > u->ndl - 1) u->bigoff = u->ndl > 0 ? u->ndl - 1 : 0;
}

/* I switches the list for the all-info view of the selected row; j/k still move the selection (the view follows) */
void nm_ui_key(nm_ui *u, int key)
{
	int was = u->mode;
	if (was == NM_NORMAL && key == 'I') { u->big = !u->big; u->bigoff = 0; if (u->big) nm__big_build(u); return; }
	if (u->big && was == NM_NORMAL) {
		int inner = nm__rows(u), max = u->ndl - inner > 0 ? u->ndl - inner : 0;
		if (key == NM_K_ESC) { u->big = 0; return; }
		if (key == NM_K_PGDN || key == ' ') { u->bigoff += inner - 1; if (u->bigoff > max) u->bigoff = max; return; }
		if (key == NM_K_PGUP) { u->bigoff -= inner - 1; if (u->bigoff < 0) u->bigoff = 0; return; }
	}
	nm__ui_key0(u, key);
	if (u->big && u->mode == NM_NORMAL) {
		char keep[100], now[110];
		nm__keyof(u, keep, sizeof keep);
		snprintf(now, sizeof now, "%d:%s", u->tab, keep);
		if (was != NM_NORMAL || key == 'r' || key == NM_K_ENTER || strcmp(now, u->bigkey)) { if (strcmp(now, u->bigkey)) u->bigoff = 0; nm__big_build(u); }
	}
}

/* ---- header data + the async network score ---------------------------------------------------------------------- */

static int nm__is_private(const char *ip)
{
	unsigned a, b;
	if (strchr(ip, ':')) return !strncasecmp(ip, "fe80", 4) || !strncasecmp(ip, "fc", 2) || !strncasecmp(ip, "fd", 2) || !strcmp(ip, "::1");
	if (sscanf(ip, "%u.%u", &a, &b) != 2) return 1;
	return a == 10 || a == 127 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 169 && b == 254) || (a == 100 && b >= 64 && b < 128);
}

/* hostname, default route, address, dns: local reads only (rtnetlink + one file), cheap enough for every tick */
static void nm__hdr_refresh(nm_ui *u)
{
	netman_if t[NETMAN_MAXIF];
	int n = netman_list(t, NETMAN_MAXIF), best = -1;
	nm_hdr *H = &u->hdr;
	FILE *fp;
	char l[200];
	if (gethostname(H->host, sizeof H->host - 1) != 0 || !H->host[0]) snprintf(H->host, sizeof H->host, "?");
	H->iface[0] = H->gw[0] = H->ip[0] = H->ipkind[0] = 0;
	for (int i = 0; i < n; i++) if (t[i].up && t[i].gw4[0] && strcmp(t[i].name, "lo")) { best = i; break; }
	if (best < 0) for (int i = 0; i < n; i++) if (t[i].up && t[i].gw6[0] && strcmp(t[i].name, "lo")) { best = i; break; }
	if (best >= 0) {
		const netman_if *f = &t[best];
		nm__copy(H->iface, sizeof H->iface, f->name);
		nm__copy(H->gw, sizeof H->gw, f->gw4[0] ? f->gw4 : f->gw6);
		for (int a = 0; a < f->naddr && !H->ip[0]; a++) if (!strchr(f->addr[a], ':')) nm__copy(H->ip, sizeof H->ip, f->addr[a]);
		for (int a = 0; a < f->naddr && !H->ip[0]; a++) nm__copy(H->ip, sizeof H->ip, f->addr[a]);
		{ char *sl = strchr(H->ip, '/'); if (sl) *sl = 0; }
		snprintf(H->ipkind, sizeof H->ipkind, "%s", H->ip[0] && !nm__is_private(H->ip) ? "pub" : "ip");
	}
	H->dns[0] = 0;
	if ((fp = fopen(netman_resolv_path, "r"))) {
		while (fgets(l, sizeof l, fp)) {
			char *x = l + 10;
			if (strncmp(l, "nameserver", 10)) continue;
			nm__rstrip(l);
			x += strspn(x, " \t");
			if (strlen(H->dns) + strlen(x) + 2 < sizeof H->dns) snprintf(H->dns + strlen(H->dns), sizeof H->dns - strlen(H->dns), "%s%s", H->dns[0] ? " " : "", x);
		}
		fclose(fp);
	}
	if (!H->iface[0]) { H->score = -1; nm__copy(H->word, sizeof H->word, "no default route"); }
}

static int nm__sc_fd = -1, nm__sc_pid;
static time_t nm__sc_t;

/* the score is measured by a forked child (3 pings at 400 ms would stall the UI); it writes "SCORE|words\n" to a pipe */
static void nm__score_start(nm_ui *u)
{
	int pf[2];
	pid_t pid;
	char ifn[16], gw[40];
	if (nm__sc_fd >= 0 || !u->hdr.iface[0] || !u->hdr.gw[0] || strchr(u->hdr.gw, ':')) return;
	nm__copy(ifn, sizeof ifn, u->hdr.iface); nm__copy(gw, sizeof gw, u->hdr.gw);
	if (pipe(pf) < 0) return;
	if ((pid = fork()) < 0) { close(pf[0]); close(pf[1]); return; }
	if (pid == 0) {
		netman_score_in in;
		netman_gwstat st;
		char out[160], w[120] = "";
		int sc, noping = 0;
		long best = -1;
		close(pf[0]);
		netman_score_in_init(&in);
		in.wired = !netman_is_wifi(ifn);
		if (!in.wired) { int d = netman_signal_dbm(ifn); if (d != NETMAN_UNKNOWN) in.signal_dbm = d; }
		netman_gwstat_init(&st);
		for (int i = 0; i < 3; i++) {
			long us = netman_gw_ping(ifn, gw, 400);
			if (us == -EACCES || us == -EPERM || us == -EAFNOSUPPORT) { noping = 1; break; }
			netman_gwstat_add(&st, us < 0 ? -1 : (int)((us + 500) / 1000));
			if (us >= 0 && (best < 0 || us < best)) best = us;
		}
		if (st.n) { in.loss_pct = netman_gwstat_loss(&st); in.rtt_ms = st.rtt_ms == NETMAN_UNKNOWN ? -1 : st.rtt_ms; }
		sc = netman_score(&in);
		if (st.n) {
			if (in.rtt_ms >= 0) snprintf(w, sizeof w, "rtt %dms, loss %d%%", in.rtt_ms, in.loss_pct); else snprintf(w, sizeof w, "gateway silent, loss %d%%", in.loss_pct);
		} else if (noping) snprintf(w, sizeof w, "ping n/a");
		if (in.signal_dbm != NETMAN_UNKNOWN) snprintf(w + strlen(w), sizeof w - strlen(w), "%s%d dBm", w[0] ? ", " : "", in.signal_dbm);
		snprintf(out, sizeof out, "%d|%s\n", sc, w);
		if (write(pf[1], out, strlen(out))) {}
		_exit(0);
	}
	close(pf[1]);
	nm__sc_fd = pf[0]; nm__sc_pid = (int)pid; nm__sc_t = time(NULL);
}

static void nm__score_collect(nm_ui *u)
{
	char b[200];
	ssize_t n = read(nm__sc_fd, b, sizeof b - 1);
	int status;
	close(nm__sc_fd); nm__sc_fd = -1;
	while (waitpid(nm__sc_pid, &status, 0) < 0 && errno == EINTR) {}
	if (n <= 0) return;
	b[n] = 0;
	{
		char *bar = strchr(b, '|');
		if (!bar) return;
		*bar = 0;
		u->hdr.score = atoi(b);
		nm__copy(u->hdr.word, sizeof u->hdr.word, bar + 1);
		nm__rstrip(u->hdr.word);
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
	nm__ui.en[NM_WIFI] = nc_enabled("wifi"); nm__ui.en[NM_ETH] = nc_enabled("net"); nm__ui.en[NM_BT] = nc_enabled("bluetooth"); nm__ui.en[NM_VPN] = nc_enabled("vpn"); nm__ui.en[NM_ENT] = nc_enabled("enterprise"); nm__ui.en[NM_TOR] = nc_enabled("tor"); nm__ui.en[NM_SSH] = nc_enabled("ssh"); nm__ui.en[NM_MESH] = nc_enabled("mesh"); nm__ui.en[NM_BOND] = nc_enabled("bonding"); nm__ui.en[NM_TUN] = nc_enabled("tunnel");
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
	nm__hdr_refresh(&nm__ui);
	nm__score_start(&nm__ui);
	while (!nm__ui.quit) {
		int k;
		if (nm__winch) { nm__winch = 0; nm__size(&nm__ui); nm__clamp(&nm__ui); if (write(1, "\x1b[2J", 4)) {} }
		nm__paint(&nm__ui);
		/* wake up every second: re-read the header, the Ethernet counters, and collect a finished score child */
		{
			struct pollfd pf[2] = { { 0, POLLIN, 0 }, { nm__sc_fd, POLLIN, 0 } };
			int pr = poll(pf, nm__sc_fd >= 0 ? 2 : 1, 1000);
			if (pr < 0) continue;
			if (nm__sc_fd >= 0 && (pf[1].revents & (POLLIN | POLLHUP))) { nm__score_collect(&nm__ui); continue; }
			if (pr == 0) {
				if (nm__ui.mode == NM_NORMAL && !nm__ui.busy) {
					nm__hdr_refresh(&nm__ui);
					if (nm__sc_fd < 0 && time(NULL) - nm__sc_t >= 10) nm__score_start(&nm__ui);
					if (nm__ui.tab == NM_ETH || nm__entlike(&nm__ui)) {
						char keep[100];
						nm__keyof(&nm__ui, keep, sizeof keep); if (nm__ui.tab == NM_ETH) nm__eth_refresh(&nm__ui); else nm__ent_refresh(&nm__ui); nm__keep_sel(&nm__ui, keep);
						if (nm__ui.big) nm__big_build(&nm__ui);
					}
				}
				continue;
			}
		}
		k = nm__read_key();
		if (k == -1) break;
		if (k > 0) nm_ui_key(&nm__ui, k);
	}
	nm__redraw = NULL;
	if (nm__sc_fd >= 0) { close(nm__sc_fd); nm__sc_fd = -1; kill(nm__sc_pid, SIGKILL); waitpid(nm__sc_pid, NULL, 0); }
	nm__restore();
	if (netman_in) { fclose(netman_in); netman_in = NULL; }
	return 0;
}

#endif /* NM_TUI_H */
