/* nmapi.c - the org.freedesktop.NetworkManager D-Bus API, served by netman's daemon over its own dbus.h (no libdbus, no GLib, no libnm).
 *
 * NetworkManager's real foundation for compatibility is this API, not GObject: libnm, nm-applet, plasma-nm and nmcli are clients of it.
 * This file is a faithful SUBSET of it; docs/nm-api-surface.md lists every interface/method/property/signal as tested, untested or absent.
 *
 *   object tree   /org/freedesktop (ObjectManager, as libnm expects) and under /org/freedesktop/NetworkManager: the manager, Devices/N,
 *                 AccessPoint/N, Settings + Settings/N, ActiveConnection/N, IP4Config/N, IP6Config/N, DnsManager, AgentManager
 *   data          links = netlink (net.h), access points + saved networks = the wifi backend (wifi.h, iwd or wpa_supplicant), connections =
 *                 net.h profiles + vpn.h profiles + saved Wi-Fi + runtime ones added over D-Bus, activation = roaming.h / wifi.h / vpn.h
 *   signals       one generic mechanism: after anything changed, the whole object model is serialised and compared with the previous
 *                 serialisation; differences become InterfacesAdded/Removed, PropertiesChanged and the NM-specific signals
 *   introspection the REAL interface XML (nmapi_xml.h, generated from NetworkManager's own files by tools/gen-nmxml)
 *
 * Included into netman.c (after cli.h): it sits on dbus.h's static code and on the other modules of the same translation unit.
 * Not thread-safe; one blocking backend call (a Wi-Fi connect) blocks the service for its duration, like every other netman call.
 * Not done: see docs/nm-api-surface.md (no secret agents, checkpoints, sleep/enable, DHCP*Config objects, VPN plugin API, statistics).
 */
#include <dirent.h>
#include "nmapi.h"
#include "nmapi_xml.h"

#define NMI "org.freedesktop.NetworkManager"
typedef netman_dbus_Buf na_buf;

enum { K_NONE, K_MGR, K_DNS, K_SET, K_AGENT, K_DEV, K_AP, K_CONN, K_ACT, K_IP4, K_IP6 };
enum { SRC_PROFILE = 1, SRC_WIFI, SRC_VPN, SRC_RUNTIME, SRC_SYNTH };
enum { CT_ETH = 1, CT_WIFI, CT_WG, CT_VPN };
enum { SEC_OPEN, SEC_WEP, SEC_WPA, SEC_WPA2, SEC_WPA3, SEC_WPA23, SEC_8021X };

#define NA_DEV 32
#define NA_AP 128
#define NA_CONN 96
#define NA_ACT 32

typedef struct { int live, seen, id, type, state, managed, autoc, ap, speed; char name[16], driver[40]; netman_if x; } na_dev;
typedef struct { int live, seen, id, pct, freq, sec; char ssid[40], bssid[20]; } na_ap;
typedef struct { int live, seen, id, src, type, autoc, sec, dhcp, ndns, unsaved; char key[96], name[80], uuid[40], iface[16], ssid[40], psk[72], addr[64], gw[46], dns[3][46]; } na_conn;
typedef struct { int live, seen, id, conn, dev, state, expl, ext; } na_act;

static na_dev na_devs[NA_DEV];
static na_ap na_aps[NA_AP];
static na_conn na_conns[NA_CONN];
static na_act na_acts[NA_ACT];
static int na_nextid = 1, na_fd = -1, na_wifi_en = 1, na_conncheck = 0, na_state, na_connectivity, na_settings_ver = 1;
static long long na_lastscan;

/* ---- tiny encoders ----------------------------------------------------------------------------------------------- */

static void na_y(na_buf *o, int v) { unsigned char c = (unsigned char)v; netman_dbus_put(o, &c, 1); }
static void na_i64(na_buf *o, long long v) { netman_dbus_align(o, 8); netman_dbus_put(o, &v, 8); }
static void na_arr(na_buf *b, size_t *lp, size_t *st, int al) { netman_dbus_align(b, 4); *lp = b->n; netman_dbus_put_u32(b, 0); if (al == 8) netman_dbus_align(b, 8); *st = b->n; }
static void na_arr_end(na_buf *b, size_t lp, size_t st) { uint32_t l = (uint32_t)(b->n - st); memcpy(b->b + lp, &l, 4); }
static void na_empty_arr(na_buf *b, int al) { size_t lp, st; na_arr(b, &lp, &st, al); na_arr_end(b, lp, st); }
static void na_kv(na_buf *b, const char *k, const char *sig) { netman_dbus_align(b, 8); netman_dbus_put_str(b, k); netman_dbus_put_sig(b, sig); }
static void na_kv_s(na_buf *b, const char *k, const char *v) { na_kv(b, k, "s"); netman_dbus_put_str(b, v); }
static void na_kv_u(na_buf *b, const char *k, uint32_t v) { na_kv(b, k, "u"); netman_dbus_put_u32(b, v); }
static void na_kv_b(na_buf *b, const char *k, int v) { na_kv(b, k, "b"); netman_dbus_put_u32(b, v ? 1 : 0); }

static void na_send_err(int fd, const netman_dbus_Msg *req, const char *name, const char *text)
{
	na_buf out = {0}, b = {0};
	netman_dbus_put_str(&b, text);
	netman_dbus_build(&out, 3, netman_dbus_serial_no++, req->sender, NULL, NULL, NULL, req->serial, name, "s", &b);
	netman_dbus_send_msg(fd, &out);
	free(out.b); free(b.b);
}
static void na_send_signal(int fd, const char *path, const char *iface, const char *member, const char *sig, const na_buf *body)
{
	na_buf out = {0};
	netman_dbus_build(&out, 4, netman_dbus_serial_no++, NULL, path, iface, member, 0, NULL, sig, body);
	netman_dbus_send_msg(fd, &out);
	free(out.b);
}

/* ---- paths ------------------------------------------------------------------------------------------------------- */

static const struct { int kind; const char *prefix; } na_prefix[] = {
	{ K_DEV, NMAPI_PATH "/Devices/" }, { K_AP, NMAPI_PATH "/AccessPoint/" }, { K_CONN, NMAPI_SETTINGS "/" }, { K_ACT, NMAPI_PATH "/ActiveConnection/" },
	{ K_IP4, NMAPI_PATH "/IP4Config/" }, { K_IP6, NMAPI_PATH "/IP6Config/" },
};
static void na_path(char *o, size_t n, int kind, int id)
{
	for (unsigned i = 0; i < sizeof na_prefix / sizeof *na_prefix; i++) if (na_prefix[i].kind == kind) { snprintf(o, n, "%s%d", na_prefix[i].prefix, id); return; }
	snprintf(o, n, "%s", kind == K_MGR ? NMAPI_PATH : kind == K_DNS ? NMAPI_DNS : kind == K_SET ? NMAPI_SETTINGS : kind == K_AGENT ? NMAPI_AGENTMGR : "/");
}
static void na_dpath(char *o, size_t n, const na_dev *d) { na_path(o, n, K_DEV, d->id); }
static const char *na_path_or_root(char *o, size_t n, int kind, int id) { if (id < 0) return "/"; na_path(o, n, kind, id); return o; }

/* ---- lookups ----------------------------------------------------------------------------------------------------- */

static na_dev *na_dev_by_name(const char *n) { for (int i = 0; i < NA_DEV; i++) if (na_devs[i].live && !strcmp(na_devs[i].name, n)) return &na_devs[i]; return NULL; }
static na_dev *na_wifi_dev(void) { for (int i = 0; i < NA_DEV; i++) if (na_devs[i].live && na_devs[i].type == NMAPI_DEV_WIFI) return &na_devs[i]; return NULL; }
static na_act *na_act_of_dev(const na_dev *d) { for (int i = 0; i < NA_ACT; i++) if (na_acts[i].live && na_acts[i].dev == (int)(d - na_devs) && na_acts[i].state != NMAPI_AC_DEACTIVATED) return &na_acts[i]; return NULL; }
static na_conn *na_conn_by_key(const char *k) { for (int i = 0; i < NA_CONN; i++) if (na_conns[i].live && !strcmp(na_conns[i].key, k)) return &na_conns[i]; return NULL; }
static int na_has_ip(const na_dev *d, char family)
{
	for (int j = 0; j < d->x.naddr; j++) {
		int v6 = strchr(d->x.addr[j], ':') != NULL;
		if (family == '6' ? v6 : !v6) return 1;
	}
	return 0;
}

/* find the object behind a path: 1 + kind/slot, 0 = not one of ours (fixed objects return slot 0) */
static int na_find(const char *path, int *kind, int *slot)
{
	*kind = K_NONE; *slot = -1;
	if (!strcmp(path, NMAPI_PATH)) { *kind = K_MGR; return 1; }
	if (!strcmp(path, NMAPI_DNS)) { *kind = K_DNS; return 1; }
	if (!strcmp(path, NMAPI_SETTINGS)) { *kind = K_SET; return 1; }
	if (!strcmp(path, NMAPI_AGENTMGR)) { *kind = K_AGENT; return 1; }
	for (unsigned i = 0; i < sizeof na_prefix / sizeof *na_prefix; i++) {
		size_t l = strlen(na_prefix[i].prefix);
		if (strncmp(path, na_prefix[i].prefix, l) || !path[l]) continue;
		char *end;
		long id = strtol(path + l, &end, 10);
		if (*end) return 0;
		int k = na_prefix[i].kind;
		if (k == K_DEV) { for (int s = 0; s < NA_DEV; s++) if (na_devs[s].live && na_devs[s].id == id) { *kind = k; *slot = s; return 1; } }
		else if (k == K_AP) { for (int s = 0; s < NA_AP; s++) if (na_aps[s].live && na_aps[s].id == id) { *kind = k; *slot = s; return 1; } }
		else if (k == K_CONN) { for (int s = 0; s < NA_CONN; s++) if (na_conns[s].live && na_conns[s].id == id) { *kind = k; *slot = s; return 1; } }
		else if (k == K_ACT) { for (int s = 0; s < NA_ACT; s++) if (na_acts[s].live && na_acts[s].id == id) { *kind = k; *slot = s; return 1; } }
		else for (int s = 0; s < NA_DEV; s++) if (na_devs[s].live && na_devs[s].id == id && na_has_ip(&na_devs[s], k == K_IP4 ? '4' : '6')) { *kind = k; *slot = s; return 1; }
		return 0;
	}
	return 0;
}

/* every live object: cb(path, kind, slot) */
static void na_objs(void (*cb)(void *u, const char *path, int kind, int slot), void *u)
{
	char p[128];
	for (int k = K_MGR; k <= K_AGENT; k++) { na_path(p, sizeof p, k, 0); cb(u, p, k, 0); }
	for (int i = 0; i < NA_DEV; i++) if (na_devs[i].live) {
		na_path(p, sizeof p, K_DEV, na_devs[i].id); cb(u, p, K_DEV, i);
		if (na_has_ip(&na_devs[i], '4')) { na_path(p, sizeof p, K_IP4, na_devs[i].id); cb(u, p, K_IP4, i); }
		if (na_has_ip(&na_devs[i], '6')) { na_path(p, sizeof p, K_IP6, na_devs[i].id); cb(u, p, K_IP6, i); }
	}
	for (int i = 0; i < NA_AP; i++) if (na_aps[i].live) { na_path(p, sizeof p, K_AP, na_aps[i].id); cb(u, p, K_AP, i); }
	for (int i = 0; i < NA_CONN; i++) if (na_conns[i].live) { na_path(p, sizeof p, K_CONN, na_conns[i].id); cb(u, p, K_CONN, i); }
	for (int i = 0; i < NA_ACT; i++) if (na_acts[i].live) { na_path(p, sizeof p, K_ACT, na_acts[i].id); cb(u, p, K_ACT, i); }
}

static int na_ifaces(int kind, int slot, const char **out)
{
	int n = 0;
	switch (kind) {
	case K_MGR: out[n++] = NMI; break;
	case K_DNS: out[n++] = NMI ".DnsManager"; break;
	case K_SET: out[n++] = NMI ".Settings"; break;
	case K_AGENT: out[n++] = NMI ".AgentManager"; break;
	case K_DEV:
		out[n++] = NMI ".Device";
		out[n++] = na_devs[slot].type == NMAPI_DEV_WIFI ? NMI ".Device.Wireless" : na_devs[slot].type == NMAPI_DEV_ETHERNET ? NMI ".Device.Wired" : NMI ".Device.Generic";
		break;
	case K_AP: out[n++] = NMI ".AccessPoint"; break;
	case K_CONN: out[n++] = NMI ".Settings.Connection"; break;
	case K_ACT: out[n++] = NMI ".Connection.Active"; break;
	case K_IP4: out[n++] = NMI ".IP4Config"; break;
	case K_IP6: out[n++] = NMI ".IP6Config"; break;
	}
	return n;
}
static const char *na_ifxml(const char *i)
{
	static const struct { const char *n; const char *x; } t[] = {
		{ NMI, nmx_Manager }, { NMI ".Device", nmx_Device }, { NMI ".Device.Wired", nmx_Wired }, { NMI ".Device.Wireless", nmx_Wireless }, { NMI ".Device.Generic", nmx_Generic },
		{ NMI ".AccessPoint", nmx_AccessPoint }, { NMI ".Connection.Active", nmx_Active }, { NMI ".Settings", nmx_Settings }, { NMI ".Settings.Connection", nmx_Connection },
		{ NMI ".IP4Config", nmx_IP4 }, { NMI ".IP6Config", nmx_IP6 }, { NMI ".DnsManager", nmx_Dns }, { NMI ".AgentManager", nmx_Agent },
	};
	for (unsigned k = 0; k < sizeof t / sizeof *t; k++) if (!strcmp(t[k].n, i)) return t[k].x;
	return "";
}

/* ---- property writer: one code path for GetAll, Get, GetManagedObjects, PropertiesChanged and the change snapshot --------------- */

typedef struct {
	na_buf *real, trash;
	const char *only;       /* comma list of property names to write, NULL = all */
	int bare;               /* Get: just the variant, no key */
	int nmarks;
	struct { char name[40]; size_t off; } marks[72];
	int hit;
} na_ctx;

static int na_inlist(const char *csv, const char *name)
{
	size_t l = strlen(name);
	for (const char *p = csv; *p; ) {
		const char *e = strchr(p, ',');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n == l && !strncmp(p, name, l)) return 1;
		if (!e) break;
		p = e + 1;
	}
	return 0;
}
static na_buf *na_pb(na_ctx *c, const char *name, const char *sig)
{
	if (c->only && !na_inlist(c->only, name)) { c->trash.n = 0; return &c->trash; }
	if (c->nmarks < 72) { snprintf(c->marks[c->nmarks].name, sizeof c->marks[0].name, "%s", name); netman_dbus_align(c->real, 8); c->marks[c->nmarks].off = c->real->n; c->nmarks++; }
	if (!c->bare) { netman_dbus_align(c->real, 8); netman_dbus_put_str(c->real, name); }
	netman_dbus_put_sig(c->real, sig);
	c->hit = 1;
	return c->real;
}
#define P_U(n, v) do { na_buf *o_ = na_pb(c, n, "u"); netman_dbus_put_u32(o_, (uint32_t)(v)); } while (0)
#define P_I(n, v) do { na_buf *o_ = na_pb(c, n, "i"); netman_dbus_put_u32(o_, (uint32_t)(int32_t)(v)); } while (0)
#define P_B(n, v) do { na_buf *o_ = na_pb(c, n, "b"); netman_dbus_put_u32(o_, (v) ? 1 : 0); } while (0)
#define P_S(n, v) do { na_buf *o_ = na_pb(c, n, "s"); netman_dbus_put_str(o_, (v)); } while (0)
#define P_O(n, v) do { na_buf *o_ = na_pb(c, n, "o"); netman_dbus_put_str(o_, (v)); } while (0)
#define P_EMPTY(n, sig, al) do { na_buf *o_ = na_pb(c, n, sig); na_empty_arr(o_, al); } while (0)

/* ---- facts about the system ---------------------------------------------------------------------------------------- */

static void na_rd(const char *dir, const char *name, const char *file, char *out, size_t n)
{
	char p[200]; FILE *f;
	out[0] = 0;
	snprintf(p, sizeof p, "%s/%s/%s", dir, name, file);
	if ((f = fopen(p, "r"))) { if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0; fclose(f); }
}
static int na_classify(const char *name)
{
	char t[64], ue[256] = "", p[200];
	const char *dt;
	na_rd("/sys/class/net", name, "type", t, sizeof t);
	if (!strcmp(t, "772")) return -1;   /* loopback: not exported (documented) */
	snprintf(p, sizeof p, "/sys/class/net/%s/wireless", name);
	if (!access(p, F_OK)) return NMAPI_DEV_WIFI;
	snprintf(p, sizeof p, "/sys/class/net/%s/phy80211", name);
	if (!access(p, F_OK)) return NMAPI_DEV_WIFI;
	{ const char *w = getenv("NETMAN_IFACE"); if (w && !strcmp(w, name)) return NMAPI_DEV_WIFI; }   /* tests: a dummy link standing in for the Wi-Fi card */
	{
		char path[200]; FILE *f;
		snprintf(path, sizeof path, "/sys/class/net/%s/uevent", name);
		if ((f = fopen(path, "r"))) { size_t n = fread(ue, 1, sizeof ue - 1, f); ue[n] = 0; fclose(f); }
	}
	dt = strstr(ue, "DEVTYPE=");
	if (dt) {
		dt += 8;
		if (!strncmp(dt, "bridge", 6)) return NMAPI_DEV_BRIDGE;
		if (!strncmp(dt, "bond", 4)) return NMAPI_DEV_BOND;
		if (!strncmp(dt, "vlan", 4)) return NMAPI_DEV_VLAN;
		if (!strncmp(dt, "wireguard", 9)) return NMAPI_DEV_WIREGUARD;
		if (!strncmp(dt, "wlan", 4)) return NMAPI_DEV_WIFI;
		return NMAPI_DEV_GENERIC;
	}
	return !strcmp(t, "1") ? NMAPI_DEV_ETHERNET : NMAPI_DEV_GENERIC;
}
static void na_uuid(const char *seed, char *out, size_t n)
{
	uint64_t h1 = 1469598103934665603ULL, h2 = 0x9e3779b97f4a7c15ULL;
	for (const unsigned char *p = (const unsigned char *)seed; *p; p++) { h1 = (h1 ^ *p) * 1099511628211ULL; h2 = (h2 + *p) * 0xff51afd7ed558ccdULL; h2 ^= h2 >> 29; }
	snprintf(out, n, "%08x-%04x-5%03x-a%03x-%012llx", (unsigned)(h1 >> 32), (unsigned)(h1 >> 16) & 0xffff, (unsigned)h1 & 0xfff, (unsigned)(h2 >> 52) & 0xfff, (unsigned long long)(h2 & 0xffffffffffffULL));
}
static void na_mac(const na_dev *d, char *o, size_t n) { snprintf(o, n, "%02X:%02X:%02X:%02X:%02X:%02X", d->x.mac[0], d->x.mac[1], d->x.mac[2], d->x.mac[3], d->x.mac[4], d->x.mac[5]); }

typedef struct { char ns4[4][46], ns6[4][46], search[4][64]; int n4, n6, nsearch; } na_dns;
static void na_read_dns(na_dns *d)
{
	char l[300]; FILE *f = fopen(netman_resolv_path ? netman_resolv_path : "/etc/resolv.conf", "r");
	memset(d, 0, sizeof *d);
	if (!f) return;
	while (fgets(l, sizeof l, f)) {
		char a[64], v[64];
		if (sscanf(l, "%63s %63s", a, v) != 2) continue;
		if (!strcmp(a, "nameserver")) { if (strchr(v, ':')) { if (d->n6 < 4) snprintf(d->ns6[d->n6++], 46, "%s", v); } else if (d->n4 < 4) snprintf(d->ns4[d->n4++], 46, "%s", v); }
		else if (!strcmp(a, "search") || !strcmp(a, "domain")) { if (d->nsearch < 4) snprintf(d->search[d->nsearch++], 64, "%s", v); }
	}
	fclose(f);
}

/* ---- properties ---------------------------------------------------------------------------------------------------- */

static na_conn *na_conn_of_act(const na_act *a) { return a->conn >= 0 ? &na_conns[a->conn] : NULL; }
static int na_primary_act(void)   /* the activation that carries the default route */
{
	for (int i = 0; i < NA_ACT; i++) if (na_acts[i].live && na_acts[i].state == NMAPI_AC_ACTIVATED && na_acts[i].dev >= 0 && (*na_devs[na_acts[i].dev].x.gw4 || *na_devs[na_acts[i].dev].x.gw6)) return i;
	return -1;
}
static const char *na_ctype(int t) { return t == CT_ETH ? "802-3-ethernet" : t == CT_WIFI ? "802-11-wireless" : t == CT_WG ? "wireguard" : "vpn"; }

static void na_ao_list(na_ctx *c, const char *name, int kind, int (*pred)(int, int), int arg)
{
	na_buf *o = na_pb(c, name, "ao");
	size_t lp, st;
	char p[128];
	int cap = kind == K_DEV ? NA_DEV : kind == K_AP ? NA_AP : kind == K_CONN ? NA_CONN : NA_ACT;
	na_arr(o, &lp, &st, 4);
	for (int i = 0; i < cap; i++) {
		int live = kind == K_DEV ? na_devs[i].live : kind == K_AP ? na_aps[i].live : kind == K_CONN ? na_conns[i].live : na_acts[i].live;
		int id = kind == K_DEV ? na_devs[i].id : kind == K_AP ? na_aps[i].id : kind == K_CONN ? na_conns[i].id : na_acts[i].id;
		if (!live || (pred && !pred(i, arg))) continue;
		na_path(p, sizeof p, kind, id);
		netman_dbus_put_str(o, p);
	}
	na_arr_end(o, lp, st);
}
static int na_pred_avail(int ci, int di)   /* connection ci can run on device di */
{
	na_conn *cn = &na_conns[ci]; na_dev *d = &na_devs[di];
	if (cn->type == CT_WIFI) return d->type == NMAPI_DEV_WIFI;
	if (cn->type == CT_ETH) return d->type == NMAPI_DEV_ETHERNET && (!cn->iface[0] || !strcmp(cn->iface, d->name));
	if (cn->type == CT_WG) return d->type == NMAPI_DEV_WIREGUARD && (!cn->iface[0] || !strcmp(cn->iface, d->name));
	return 0;
}
static int na_pred_ap_of_wifi(int ai, int unused) { (void)ai; (void)unused; return 1; }
static int na_pred_act_live(int i, int unused) { (void)unused; return na_acts[i].state != NMAPI_AC_DEACTIVATED; }
static int na_pred_dev_eth_or_all(int i, int unused) { (void)i; (void)unused; return 1; }

static void na_pip_addresses(na_buf *o, const na_dev *d)   /* aau: [addr, prefix, gateway] per address */
{
	size_t lp, st;
	struct in_addr gw = {0};
	inet_pton(AF_INET, d->x.gw4, &gw);
	na_arr(o, &lp, &st, 4);
	for (int j = 0; j < d->x.naddr; j++) {
		char a[64]; struct in_addr ia; int pl;
		size_t l2, s2;
		if (strchr(d->x.addr[j], ':') || sscanf(d->x.addr[j], "%63[^/]/%d", a, &pl) != 2 || inet_pton(AF_INET, a, &ia) != 1) continue;
		na_arr(o, &l2, &s2, 4);
		netman_dbus_put_u32(o, ia.s_addr); netman_dbus_put_u32(o, pl); netman_dbus_put_u32(o, gw.s_addr);
		na_arr_end(o, l2, s2);
	}
	na_arr_end(o, lp, st);
}
static void na_pip_addrdata(na_buf *o, const na_dev *d, int v6)   /* aa{sv} */
{
	size_t lp, st;
	na_arr(o, &lp, &st, 4);
	for (int j = 0; j < d->x.naddr; j++) {
		char a[64]; int pl; size_t l2, s2;
		if ((strchr(d->x.addr[j], ':') != NULL) != v6 || sscanf(d->x.addr[j], "%63[^/]/%d", a, &pl) != 2) continue;
		na_arr(o, &l2, &s2, 8);
		na_kv_s(o, "address", a); na_kv_u(o, "prefix", (uint32_t)pl);
		na_arr_end(o, l2, s2);
	}
	na_arr_end(o, lp, st);
}
static void na_pip_routedata(na_buf *o, const na_dev *d, int v6)
{
	size_t lp, st;
	na_arr(o, &lp, &st, 4);
	for (int j = 0; j < d->x.nroute; j++) {
		char t[96], dst[64], *via; int pl = v6 ? 128 : 32; size_t l2, s2;
		snprintf(t, sizeof t, "%s", d->x.route[j]);
		via = strstr(t, " via ");
		if (via) { *via = 0; via += 5; }
		if ((strchr(t, ':') != NULL) != v6) continue;
		snprintf(dst, sizeof dst, "%s", t);
		{ char *sl = strchr(dst, '/'); if (sl) { *sl = 0; pl = atoi(sl + 1); } }
		na_arr(o, &l2, &s2, 8);
		na_kv_s(o, "dest", dst); na_kv_u(o, "prefix", (uint32_t)pl);
		if (via) na_kv_s(o, "next-hop", via);
		na_kv_u(o, "metric", 100);
		na_arr_end(o, l2, s2);
	}
	na_arr_end(o, lp, st);
}
static void na_pip_strs(na_ctx *c, const char *name, char (*v)[64], int n)
{
	na_buf *o = na_pb(c, name, "as"); size_t lp, st;
	na_arr(o, &lp, &st, 4);
	for (int i = 0; i < n; i++) netman_dbus_put_str(o, v[i]);
	na_arr_end(o, lp, st);
}

static void na_props_ip(na_ctx *c, na_dev *d, int v6)
{
	na_dns dn;
	char gw[46];
	na_read_dns(&dn);
	snprintf(gw, sizeof gw, "%s", v6 ? d->x.gw6 : d->x.gw4);
	if (!v6) {
		na_buf *o = na_pb(c, "Addresses", "aau"); na_pip_addresses(o, d);
		o = na_pb(c, "AddressData", "aa{sv}"); na_pip_addrdata(o, d, 0);
		P_S("Gateway", gw);
		P_EMPTY("Routes", "aau", 4);
		o = na_pb(c, "RouteData", "aa{sv}"); na_pip_routedata(o, d, 0);
		o = na_pb(c, "Nameservers", "au");
		{ size_t lp, st; na_arr(o, &lp, &st, 4); for (int i = 0; i < dn.n4; i++) { struct in_addr a; if (inet_pton(AF_INET, dn.ns4[i], &a) == 1) netman_dbus_put_u32(o, a.s_addr); } na_arr_end(o, lp, st); }
		o = na_pb(c, "NameserverData", "aa{sv}");
		{ size_t lp, st; na_arr(o, &lp, &st, 4); for (int i = 0; i < dn.n4; i++) { size_t l2, s2; na_arr(o, &l2, &s2, 8); na_kv_s(o, "address", dn.ns4[i]); na_arr_end(o, l2, s2); } na_arr_end(o, lp, st); }
		na_pip_strs(c, "Domains", dn.search, dn.nsearch);
		na_pip_strs(c, "Searches", dn.search, dn.nsearch);
		P_EMPTY("DnsOptions", "as", 4);
		P_I("DnsPriority", 100);
		P_EMPTY("WinsServers", "au", 4);
		P_EMPTY("WinsServerData", "as", 4);
	} else {
		na_buf *o = na_pb(c, "Addresses", "a(ayuay)");
		{
			size_t lp, st; struct in6_addr g = {0};
			inet_pton(AF_INET6, gw, &g);
			na_arr(o, &lp, &st, 8);
			for (int j = 0; j < d->x.naddr; j++) {
				char a[64]; int pl; struct in6_addr ia; size_t l2, s2;
				if (!strchr(d->x.addr[j], ':') || sscanf(d->x.addr[j], "%63[^/]/%d", a, &pl) != 2 || inet_pton(AF_INET6, a, &ia) != 1) continue;
				netman_dbus_align(o, 8);
				na_arr(o, &l2, &s2, 1); netman_dbus_put(o, &ia, 16); na_arr_end(o, l2, s2);
				netman_dbus_put_u32(o, (uint32_t)pl);
				na_arr(o, &l2, &s2, 1); netman_dbus_put(o, &g, 16); na_arr_end(o, l2, s2);
			}
			na_arr_end(o, lp, st);
		}
		o = na_pb(c, "AddressData", "aa{sv}"); na_pip_addrdata(o, d, 1);
		P_S("Gateway", gw);
		P_EMPTY("Routes", "a(ayuayu)", 8);
		o = na_pb(c, "RouteData", "aa{sv}"); na_pip_routedata(o, d, 1);
		o = na_pb(c, "Nameservers", "aay");
		{ size_t lp, st; na_arr(o, &lp, &st, 4); for (int i = 0; i < dn.n6; i++) { struct in6_addr a; size_t l2, s2; if (inet_pton(AF_INET6, dn.ns6[i], &a) != 1) continue; na_arr(o, &l2, &s2, 1); netman_dbus_put(o, &a, 16); na_arr_end(o, l2, s2); } na_arr_end(o, lp, st); }
		na_pip_strs(c, "Domains", dn.search, dn.nsearch);
		na_pip_strs(c, "Searches", dn.search, dn.nsearch);
		P_EMPTY("DnsOptions", "as", 4);
		P_I("DnsPriority", 100);
	}
}

static unsigned na_ap_wpa(int sec) { return sec == SEC_WPA ? 0x188 : 0; }   /* PAIR_CCMP | GROUP_CCMP | KEY_MGMT_PSK */
static unsigned na_ap_rsn(int sec)
{
	switch (sec) {
	case SEC_WPA2: return 0x188;
	case SEC_WPA3: return 0x8 | 0x80 | 0x400;
	case SEC_WPA23: return 0x188 | 0x400;
	case SEC_8021X: return 0x8 | 0x80 | 0x200;
	}
	return 0;
}

static void na_props(na_ctx *c, int kind, int slot, const char *iface)
{
	char p[128], q[128], mac[24];
	if (kind == K_MGR) {
		int pa = na_primary_act(), act1 = -1;
		for (int i = 0; i < NA_ACT; i++) if (na_acts[i].live && na_acts[i].state == NMAPI_AC_ACTIVATING) { act1 = i; break; }
		na_ao_list(c, "Devices", K_DEV, na_pred_dev_eth_or_all, 0);
		na_ao_list(c, "AllDevices", K_DEV, na_pred_dev_eth_or_all, 0);
		P_EMPTY("Checkpoints", "ao", 4);
		P_B("NetworkingEnabled", 1);
		P_B("WirelessEnabled", na_wifi_en);
		P_B("WirelessHardwareEnabled", 1);
		P_B("WwanEnabled", 0);
		P_B("WwanHardwareEnabled", 0);
		P_B("WimaxEnabled", 0);
		P_B("WimaxHardwareEnabled", 0);
		P_U("RadioFlags", na_wifi_dev() ? 1 : 0);
		na_ao_list(c, "ActiveConnections", K_ACT, na_pred_act_live, 0);
		P_O("PrimaryConnection", na_path_or_root(p, sizeof p, K_ACT, pa >= 0 ? na_acts[pa].id : -1));
		{ na_conn *cn = pa >= 0 ? na_conn_of_act(&na_acts[pa]) : NULL; P_S("PrimaryConnectionType", cn ? na_ctype(cn->type) : ""); }
		P_U("Metered", 0);
		P_O("ActivatingConnection", na_path_or_root(q, sizeof q, K_ACT, act1 >= 0 ? na_acts[act1].id : -1));
		P_B("Startup", 0);
		P_S("Version", "1.58.1");
		{ na_buf *o = na_pb(c, "VersionInfo", "au"); size_t lp, st; na_arr(o, &lp, &st, 4); netman_dbus_put_u32(o, (1u << 16) | (58u << 8) | 1u); netman_dbus_put_u32(o, 0); na_arr_end(o, lp, st); }
		P_EMPTY("Capabilities", "au", 4);
		P_U("State", na_state);
		P_U("Connectivity", na_connectivity);
		P_B("ConnectivityCheckAvailable", 0);
		P_B("ConnectivityCheckEnabled", na_conncheck);
		P_S("ConnectivityCheckUri", "");
		P_EMPTY("GlobalDnsConfiguration", "a{sv}", 8);
	} else if (kind == K_DNS) {
		na_dns dn; na_read_dns(&dn);
		P_S("Mode", "default");
		P_S("RcManager", "file");
		{
			na_buf *o = na_pb(c, "Configuration", "aa{sv}"); size_t lp, st;
			na_arr(o, &lp, &st, 4);
			if (dn.n4 + dn.n6) {
				size_t l2, s2, l3, s3;
				na_arr(o, &l2, &s2, 8);
				na_kv(o, "nameservers", "as"); na_arr(o, &l3, &s3, 4);
				for (int i = 0; i < dn.n4; i++) netman_dbus_put_str(o, dn.ns4[i]);
				for (int i = 0; i < dn.n6; i++) netman_dbus_put_str(o, dn.ns6[i]);
				na_arr_end(o, l3, s3);
				na_kv(o, "priority", "i"); netman_dbus_put_u32(o, 100);
				na_arr_end(o, l2, s2);
			}
			na_arr_end(o, lp, st);
		}
	} else if (kind == K_SET) {
		char hn[80] = "";
		gethostname(hn, sizeof hn - 1);
		na_ao_list(c, "Connections", K_CONN, NULL, 0);
		P_S("Hostname", hn);
		P_B("CanModify", 1);
		{ na_buf *o = na_pb(c, "VersionId", "t"); netman_dbus_align(o, 8); uint64_t v = (uint64_t)na_settings_ver; netman_dbus_put(o, &v, 8); }
	} else if (kind == K_DEV) {
		na_dev *d = &na_devs[slot];
		na_act *a = na_act_of_dev(d);
		na_mac(d, mac, sizeof mac);
		if (!strcmp(iface, NMI ".Device")) {
			char udi[80];
			snprintf(udi, sizeof udi, "/sys/devices/virtual/net/%s", d->name);
			P_S("Udi", udi);
			P_S("Path", "");
			P_S("Interface", d->name);
			P_S("IpInterface", d->name);
			P_S("Driver", d->driver);
			P_S("DriverVersion", "");
			P_S("FirmwareVersion", "");
			P_U("Capabilities", 3);
			P_U("Ip4Address", 0);
			P_U("State", d->state);
			{ na_buf *o = na_pb(c, "StateReason", "(uu)"); netman_dbus_align(o, 8); netman_dbus_put_u32(o, 0); netman_dbus_put_u32(o, 0); }
			P_O("ActiveConnection", na_path_or_root(p, sizeof p, K_ACT, a ? a->id : -1));
			P_O("Ip4Config", na_has_ip(d, '4') ? (na_path(q, sizeof q, K_IP4, d->id), q) : "/");
			P_O("Dhcp4Config", "/");
			{ char r[128]; P_O("Ip6Config", na_has_ip(d, '6') ? (na_path(r, sizeof r, K_IP6, d->id), r) : "/"); }
			P_O("Dhcp6Config", "/");
			P_B("Managed", d->managed);
			P_B("Autoconnect", d->autoc);
			P_B("FirmwareMissing", 0);
			P_B("NmPluginMissing", 0);
			P_U("DeviceType", d->type);
			{
				na_buf *o = na_pb(c, "AvailableConnections", "ao"); size_t lp, st;
				na_arr(o, &lp, &st, 4);
				for (int i = 0; i < NA_CONN; i++) if (na_conns[i].live && na_pred_avail(i, slot)) { na_path(p, sizeof p, K_CONN, na_conns[i].id); netman_dbus_put_str(o, p); }
				na_arr_end(o, lp, st);
			}
			P_S("PhysicalPortId", "");
			P_U("Mtu", d->x.mtu);
			P_U("Metered", 0);
			P_EMPTY("LldpNeighbors", "aa{sv}", 4);
			P_B("Real", 1);
			P_U("Ip4Connectivity", d->state == NMAPI_DS_ACTIVATED && na_has_ip(d, '4') ? NMAPI_CONN_FULL : NMAPI_CONN_NONE);
			P_U("Ip6Connectivity", d->state == NMAPI_DS_ACTIVATED && na_has_ip(d, '6') ? NMAPI_CONN_FULL : NMAPI_CONN_NONE);
			P_U("InterfaceFlags", (d->x.up ? 1 : 0) | (d->x.carrier ? 2 : 0) | (d->x.carrier ? 0x10000 : 0));
			P_S("HwAddress", mac);
			P_EMPTY("Ports", "ao", 4);
		} else if (!strcmp(iface, NMI ".Device.Wired")) {
			P_S("HwAddress", mac);
			P_S("PermHwAddress", mac);
			P_U("Speed", d->speed > 0 ? d->speed : 0);
			P_EMPTY("S390Subchannels", "as", 4);
			P_B("Carrier", d->x.carrier);
		} else if (!strcmp(iface, NMI ".Device.Wireless")) {
			P_S("HwAddress", mac);
			P_S("PermHwAddress", mac);
			P_U("Mode", 2);
			P_U("Bitrate", d->ap >= 0 ? 54000 : 0);
			na_ao_list(c, "AccessPoints", K_AP, na_pred_ap_of_wifi, 0);
			P_O("ActiveAccessPoint", na_path_or_root(p, sizeof p, K_AP, d->ap >= 0 ? na_aps[d->ap].id : -1));
			P_U("WirelessCapabilities", 0x7f);
			{ na_buf *o = na_pb(c, "LastScan", "x"); na_i64(o, na_lastscan); }
		} else {
			P_S("HwAddress", mac);
			P_S("TypeDescription", d->type == NMAPI_DEV_BRIDGE ? "bridge" : d->type == NMAPI_DEV_BOND ? "bond" : d->type == NMAPI_DEV_VLAN ? "vlan" : d->type == NMAPI_DEV_WIREGUARD ? "wireguard" : "generic");
		}
	} else if (kind == K_AP) {
		na_ap *ap = &na_aps[slot];
		P_U("Flags", ap->sec == SEC_OPEN ? 0 : 1);
		P_U("WpaFlags", na_ap_wpa(ap->sec));
		P_U("RsnFlags", na_ap_rsn(ap->sec));
		{ na_buf *o = na_pb(c, "Ssid", "ay"); size_t lp, st, l = strlen(ap->ssid); na_arr(o, &lp, &st, 1); netman_dbus_put(o, ap->ssid, l); na_arr_end(o, lp, st); }
		P_U("Frequency", ap->freq);
		P_S("HwAddress", ap->bssid);
		P_U("Mode", 2);
		P_U("MaxBitrate", 54000);
		P_U("Bandwidth", 20);
		{ na_buf *o = na_pb(c, "Strength", "y"); na_y(o, ap->pct); }
		P_I("LastSeen", (int)(na_lastscan / 1000));
	} else if (kind == K_CONN) {
		na_conn *cn = &na_conns[slot];
		P_B("Unsaved", cn->unsaved);
		P_U("Flags", cn->unsaved ? 1 : 0);
		P_S("Filename", "");
	} else if (kind == K_ACT) {
		na_act *a = &na_acts[slot];
		na_conn *cn = na_conn_of_act(a);
		na_dev *d = a->dev >= 0 ? &na_devs[a->dev] : NULL;
		int primary = slot == na_primary_act();
		P_O("Connection", cn ? (na_path(p, sizeof p, K_CONN, cn->id), p) : "/");
		P_O("SpecificObject", d && d->type == NMAPI_DEV_WIFI && d->ap >= 0 ? (na_path(q, sizeof q, K_AP, na_aps[d->ap].id), q) : "/");
		P_S("Id", cn ? cn->name : "");
		P_S("Uuid", cn ? cn->uuid : "");
		P_S("Type", cn ? na_ctype(cn->type) : "");
		{
			na_buf *o = na_pb(c, "Devices", "ao"); size_t lp, st; char r[128];
			na_arr(o, &lp, &st, 4);
			if (d) { na_dpath(r, sizeof r, d); netman_dbus_put_str(o, r); }
			na_arr_end(o, lp, st);
		}
		P_U("State", a->state);
		P_U("StateFlags", a->state == NMAPI_AC_ACTIVATED ? (4 | 8 | (a->ext ? 0x80 : 0)) : 0);
		P_B("Default", primary && d && d->x.gw4[0]);
		{ char r[128]; P_O("Ip4Config", d && na_has_ip(d, '4') ? (na_path(r, sizeof r, K_IP4, d->id), r) : "/"); }
		P_O("Dhcp4Config", "/");
		P_B("Default6", primary && d && d->x.gw6[0]);
		{ char r[128]; P_O("Ip6Config", d && na_has_ip(d, '6') ? (na_path(r, sizeof r, K_IP6, d->id), r) : "/"); }
		P_O("Dhcp6Config", "/");
		P_B("Vpn", cn && (cn->type == CT_VPN || cn->type == CT_WG));
		P_O("Controller", "/");
		P_O("Master", "/");
	} else if (kind == K_IP4 || kind == K_IP6) na_props_ip(c, &na_devs[slot], kind == K_IP6);
	(void)iface;
}

/* ---- model refresh: links, Wi-Fi, connections, activations ---------------------------------------------------------- */

typedef struct { int kind; char path[100]; uint32_t a, b, c; } na_pend;
static na_pend na_q[64];
static int na_nq;
static void na_queue(int kind, const char *path, uint32_t a, uint32_t b, uint32_t c)
{
	if (na_nq < 64) { na_pend *p = &na_q[na_nq++]; p->kind = kind; snprintf(p->path, sizeof p->path, "%s", path); p->a = a; p->b = b; p->c = c; }
}

static struct { char ssid[40]; int sec; } na_known[64];
static int na_nknown;

static int na_sec_of(const char *s)
{
	return !strcmp(s, "WEP") ? SEC_WEP : !strcmp(s, "WPA") ? SEC_WPA : !strcmp(s, "WPA2") ? SEC_WPA2 : !strcmp(s, "WPA3") ? SEC_WPA3 : !strcmp(s, "WPA2/3") ? SEC_WPA23 : !strcmp(s, "802.1X") ? SEC_8021X : SEC_OPEN;
}

/* read the Wi-Fi backend (netman_wifi_scan's TSV); scan = ask it for a fresh scan first. Returns 0 when it answered. */
static int na_wifi_read(int scan)
{
	na_dev *wd = na_wifi_dev();
	FILE *so = netman_out, *se = netman_err;
	char *ob = NULL, *eb = NULL, *line, *sv = NULL;
	size_t os = 0, es = 0;
	int rc;
	if (!wd) return -1;
	netman_out = open_memstream(&ob, &os); netman_err = open_memstream(&eb, &es);
	netman_wifi_passive = !scan;
	rc = netman_wifi_scan();
	netman_wifi_passive = 0;
	fclose(netman_out); fclose(netman_err);
	netman_out = so; netman_err = se;
	free(eb);
	if (rc) { free(ob); return -1; }
	for (int i = 0; i < NA_AP; i++) na_aps[i].seen = 0;
	wd->ap = -1;
	na_nknown = 0;
	for (line = strtok_r(ob, "\n", &sv); line; line = strtok_r(NULL, "\n", &sv)) {
		char *f[11], *p = line, ssid[40], bssid[20], band[8] = "-";
		int nf = 0, chan = 0, freq = 0, sec, slot = -1, conn, known;
		while (nf < 11) { f[nf++] = p; if (nf == 11) break; char *t = strchr(p, '\t'); if (!t) break; *t = 0; p = t + 1; }
		if (nf < 11) continue;
		conn = f[0][0] == '*'; known = atoi(f[1]); sec = na_sec_of(f[2]);
		snprintf(bssid, sizeof bssid, "%s", f[5]); chan = atoi(f[6]); snprintf(band, sizeof band, "%s", f[7]);
		netman_wifi_unescape(f[10], ssid, sizeof ssid);
		freq = !chan ? 0 : !strcmp(band, "2.4") ? (chan == 14 ? 2484 : 2407 + 5 * chan) : !strcmp(band, "5") ? 5000 + 5 * chan : !strcmp(band, "6") ? 5950 + 5 * chan : 0;
		for (int i = 0; i < NA_AP; i++) if (na_aps[i].live && !strcmp(na_aps[i].ssid, ssid) && !strcmp(na_aps[i].bssid, bssid)) { slot = i; break; }
		if (slot < 0) for (int i = 0; i < NA_AP; i++) if (!na_aps[i].live) { slot = i; memset(&na_aps[i], 0, sizeof na_aps[i]); na_aps[i].live = 1; na_aps[i].id = na_nextid++; break; }
		if (slot < 0) continue;
		na_aps[slot].seen = 1; na_aps[slot].pct = atoi(f[4]); na_aps[slot].freq = freq; na_aps[slot].sec = sec;
		snprintf(na_aps[slot].ssid, sizeof na_aps[slot].ssid, "%s", ssid); snprintf(na_aps[slot].bssid, sizeof na_aps[slot].bssid, "%s", bssid);
		if (conn) wd->ap = slot;
		if (known && ssid[0] && na_nknown < 64) { snprintf(na_known[na_nknown].ssid, 40, "%s", ssid); na_known[na_nknown++].sec = sec; }
	}
	free(ob);
	for (int i = 0; i < NA_AP; i++) if (na_aps[i].live && !na_aps[i].seen) na_aps[i].live = 0;
	{ struct timespec ts; clock_gettime(CLOCK_BOOTTIME, &ts); na_lastscan = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
	return 0;
}

static na_conn *na_conn_upsert(const char *key, int src, int *created)
{
	na_conn *cn = na_conn_by_key(key);
	*created = 0;
	if (!cn) {
		for (int i = 0; i < NA_CONN && !cn; i++) if (!na_conns[i].live) cn = &na_conns[i];
		if (!cn) return NULL;
		memset(cn, 0, sizeof *cn);
		cn->live = 1; cn->id = na_nextid++; cn->src = src; cn->autoc = 1;
		snprintf(cn->key, sizeof cn->key, "%s", key);
		*created = 1;
	}
	cn->seen = 1;
	return cn;
}

static int na_vpn_dir_scan(char names[][64], int max)
{
	DIR *d = opendir(netman_vpn_conf_dir);
	struct dirent *e;
	int n = 0;
	if (!d) return 0;
	while (n < max && (e = readdir(d))) {
		char *dot = strrchr(e->d_name, '.');
		if (!dot || (strcmp(dot, ".conf") && strcmp(dot, ".ovpn")) || (size_t)(dot - e->d_name) >= 64) continue;
		snprintf(names[n], 64, "%.*s", (int)(dot - e->d_name), e->d_name);
		if (netman_vpn_type(names[n])) n++;
	}
	closedir(d);
	return n;
}

static int na_match_conn(const na_dev *d)   /* connection that is active on device d, or -1 */
{
	int di = (int)(d - na_devs);
	for (int i = 0; i < NA_ACT; i++) if (na_acts[i].live && na_acts[i].expl && na_acts[i].dev == di && na_acts[i].state != NMAPI_AC_DEACTIVATED && na_conns[na_acts[i].conn].live) return na_acts[i].conn;
	for (int i = 0; i < NA_CONN; i++) {
		na_conn *cn = &na_conns[i];
		if (!cn->live || cn->src == SRC_SYNTH) continue;
		if (d->type == NMAPI_DEV_ETHERNET && cn->type == CT_ETH && !strcmp(cn->iface, d->name)) return i;
		if (d->type == NMAPI_DEV_WIFI && cn->type == CT_WIFI && d->ap >= 0 && !strcmp(cn->ssid, na_aps[d->ap].ssid)) return i;
		if (d->type == NMAPI_DEV_WIREGUARD && cn->type == CT_WG && !strcmp(cn->name, d->name)) return i;
	}
	return -1;
}

static void na_refresh(int wifi)   /* wifi: 0 keep, 1 read the backend's cache, 2 ask for a fresh scan */
{
	netman_if ifs[NETMAN_MAXIF];
	int n = netman_list(ifs, NETMAN_MAXIF), wifi_ok = 0;
	char names[32][64];
	int np;
	if (n >= 0) {
		for (int i = 0; i < NA_DEV; i++) na_devs[i].seen = 0;
		for (int i = 0; i < n; i++) {
			int t = na_classify(ifs[i].name), slot = -1;
			char l[256] = "", sp[32];
			if (t < 0) continue;
			slot = -1;
			for (int j = 0; j < NA_DEV; j++) if (na_devs[j].live && !strcmp(na_devs[j].name, ifs[i].name)) { slot = j; break; }
			if (slot < 0) for (int j = 0; j < NA_DEV; j++) if (!na_devs[j].live) {
				slot = j; memset(&na_devs[j], 0, sizeof na_devs[j]);
				na_devs[j].live = 1; na_devs[j].id = na_nextid++; na_devs[j].managed = 1; na_devs[j].autoc = 1; na_devs[j].ap = -1; na_devs[j].state = NMAPI_DS_UNAVAILABLE;
				snprintf(na_devs[j].name, sizeof na_devs[j].name, "%s", ifs[i].name);
				break;
			}
			if (slot < 0) continue;
			na_devs[slot].seen = 1; na_devs[slot].type = t; na_devs[slot].x = ifs[i];
			{
				char p[200]; ssize_t k;
				snprintf(p, sizeof p, "/sys/class/net/%s/device/driver", ifs[i].name);
				if ((k = readlink(p, l, sizeof l - 1)) > 0) { l[k] = 0; const char *b = strrchr(l, '/'); snprintf(na_devs[slot].driver, sizeof na_devs[slot].driver, "%s", b ? b + 1 : l); }
				else na_devs[slot].driver[0] = 0;
			}
			na_rd("/sys/class/net", ifs[i].name, "speed", sp, sizeof sp);
			na_devs[slot].speed = atoi(sp);
		}
		for (int i = 0; i < NA_DEV; i++) if (na_devs[i].live && !na_devs[i].seen) {
			na_devs[i].live = 0;
			for (int j = 0; j < NA_ACT; j++) if (na_acts[j].live && na_acts[j].dev == i) na_acts[j].live = 0;
		}
	}
	if (wifi && na_wifi_dev() && na_wifi_en) wifi_ok = na_wifi_read(wifi == 2) == 0;
	if (!na_wifi_dev()) for (int i = 0; i < NA_AP; i++) na_aps[i].live = 0;

	/* base device states */
	for (int i = 0; i < NA_DEV; i++) {
		na_dev *d = &na_devs[i];
		int st;
		if (!d->live) continue;
		if (!d->managed) st = NMAPI_DS_UNMANAGED;
		else if (d->type == NMAPI_DEV_WIFI) st = d->ap >= 0 ? NMAPI_DS_ACTIVATED : d->x.up ? NMAPI_DS_DISCONNECTED : NMAPI_DS_UNAVAILABLE;
		else st = !d->x.carrier ? NMAPI_DS_UNAVAILABLE : na_has_ip(d, '4') ? NMAPI_DS_ACTIVATED : NMAPI_DS_DISCONNECTED;
		d->state = st;   /* provisional; the queue of StateChanged signals is filled below once activations are known */
	}

	/* connections: profiles, saved Wi-Fi, VPN profiles; runtime ones stay */
	for (int i = 0; i < NA_CONN; i++) if (na_conns[i].live && na_conns[i].src != SRC_RUNTIME && (na_conns[i].src != SRC_WIFI || wifi_ok)) na_conns[i].seen = 0; else if (na_conns[i].live) na_conns[i].seen = 1;
	np = netman_profile_list(names, 32);
	for (int i = 0; i < np; i++) {
		netman_profile p; char key[100]; int cr; na_conn *cn;
		if (netman_profile_load(names[i], &p) < 0) continue;
		snprintf(key, sizeof key, "p:%s", p.name);
		if (!(cn = na_conn_upsert(key, SRC_PROFILE, &cr))) break;
		cn->type = CT_ETH; cn->dhcp = p.dhcp; cn->autoc = p.autoconnect; cn->ndns = p.ndns; cn->unsaved = 0;
		snprintf(cn->name, sizeof cn->name, "%s", p.name); snprintf(cn->iface, sizeof cn->iface, "%s", p.iface);
		snprintf(cn->addr, sizeof cn->addr, "%s", p.addr); snprintf(cn->gw, sizeof cn->gw, "%s", p.gateway);
		for (int k = 0; k < 3; k++) snprintf(cn->dns[k], sizeof cn->dns[k], "%s", p.dns[k]);
		na_uuid(key, cn->uuid, sizeof cn->uuid);
	}
	if (wifi_ok) for (int i = 0; i < na_nknown; i++) {
		char key[100]; int cr; na_conn *cn;
		snprintf(key, sizeof key, "w:%s", na_known[i].ssid);
		if (!(cn = na_conn_upsert(key, SRC_WIFI, &cr))) break;
		cn->type = CT_WIFI; cn->sec = na_known[i].sec; cn->dhcp = 1; cn->unsaved = 0;
		snprintf(cn->name, sizeof cn->name, "%s", na_known[i].ssid); snprintf(cn->ssid, sizeof cn->ssid, "%s", na_known[i].ssid);
		na_uuid(key, cn->uuid, sizeof cn->uuid);
	}
	if (nc_enabled("vpn")) {
		char vn[16][64];
		int nv = na_vpn_dir_scan(vn, 16);
		for (int i = 0; i < nv; i++) {
			char key[100]; int cr; na_conn *cn;
			snprintf(key, sizeof key, "v:%s", vn[i]);
			if (!(cn = na_conn_upsert(key, SRC_VPN, &cr))) break;
			cn->type = netman_vpn_type(vn[i]) == NETMAN_VPN_WG ? CT_WG : CT_VPN; cn->unsaved = 0; cn->autoc = 0;
			snprintf(cn->name, sizeof cn->name, "%s", vn[i]);
			na_uuid(key, cn->uuid, sizeof cn->uuid);
		}
	}
	for (int i = 0; i < NA_CONN; i++) if (na_conns[i].live && na_conns[i].src != SRC_SYNTH && !na_conns[i].seen) {
		for (int j = 0; j < NA_ACT; j++) if (na_acts[j].live && na_acts[j].conn == i) na_acts[j].live = 0;
		na_conns[i].live = 0;
	}

	/* activations: derived from the devices; explicit ones (made through D-Bus) stay while their device carries the connection */
	for (int i = 0; i < NA_ACT; i++) if (na_acts[i].live) na_acts[i].seen = (na_acts[i].state == NMAPI_AC_ACTIVATING || na_acts[i].state == NMAPI_AC_DEACTIVATED || (na_acts[i].dev < 0 && na_acts[i].state == NMAPI_AC_ACTIVATED)) ? 1 : 0;
	for (int i = 0; i < NA_CONN; i++) if (na_conns[i].live && na_conns[i].src == SRC_SYNTH) na_conns[i].seen = 0;
	for (int i = 0; i < NA_DEV; i++) {
		na_dev *d = &na_devs[i];
		int m, ai = -1, ext = 0;
		if (!d->live || d->state != NMAPI_DS_ACTIVATED) continue;
		m = na_match_conn(d);
		if (m < 0) {   /* configured outside netman: show it, like NetworkManager's "external" activations */
			char key[100]; int cr; na_conn *cn;
			snprintf(key, sizeof key, "s:%s:%s", d->name, d->type == NMAPI_DEV_WIFI && d->ap >= 0 ? na_aps[d->ap].ssid : "");
			if ((cn = na_conn_upsert(key, SRC_SYNTH, &cr))) {
				cn->type = d->type == NMAPI_DEV_WIFI ? CT_WIFI : d->type == NMAPI_DEV_WIREGUARD ? CT_WG : CT_ETH; cn->unsaved = 1; cn->dhcp = 1;
				snprintf(cn->name, sizeof cn->name, "%s", d->type == NMAPI_DEV_WIFI && d->ap >= 0 ? na_aps[d->ap].ssid : d->name);
				snprintf(cn->iface, sizeof cn->iface, "%s", d->name);
				if (d->type == NMAPI_DEV_WIFI && d->ap >= 0) snprintf(cn->ssid, sizeof cn->ssid, "%s", na_aps[d->ap].ssid);
				na_uuid(key, cn->uuid, sizeof cn->uuid);
				m = (int)(cn - na_conns); ext = 1;
			}
		}
		if (m < 0) continue;
		for (int j = 0; j < NA_ACT; j++) if (na_acts[j].live && na_acts[j].dev == i && na_acts[j].conn == m && na_acts[j].state != NMAPI_AC_DEACTIVATED) { ai = j; break; }
		if (ai < 0) for (int j = 0; j < NA_ACT; j++) if (!na_acts[j].live) {
			ai = j; memset(&na_acts[j], 0, sizeof na_acts[j]);
			na_acts[j].live = 1; na_acts[j].id = na_nextid++; na_acts[j].conn = m; na_acts[j].dev = i; na_acts[j].ext = ext || na_conns[m].src == SRC_SYNTH;
			break;
		}
		if (ai < 0) continue;
		na_acts[ai].seen = 1;
		if (na_acts[ai].state != NMAPI_AC_ACTIVATING) na_acts[ai].state = NMAPI_AC_ACTIVATED;
	}
	for (int i = 0; i < NA_ACT; i++) if (na_acts[i].live && !na_acts[i].seen) na_acts[i].live = 0;
	for (int i = 0; i < NA_CONN; i++) if (na_conns[i].live && na_conns[i].src == SRC_SYNTH && !na_conns[i].seen) na_conns[i].live = 0;

	/* final device states + the signals about them; then the manager's */
	for (int i = 0; i < NA_DEV; i++) {
		na_dev *d = &na_devs[i];
		static int prev[NA_DEV], prev_id[NA_DEV];
		na_act *a;
		if (!d->live) { prev_id[i] = 0; continue; }
		a = na_act_of_dev(d);
		if (a && a->state == NMAPI_AC_ACTIVATING) d->state = NMAPI_DS_PREPARE;
		else if (d->state == NMAPI_DS_ACTIVATED && !a) d->state = NMAPI_DS_DISCONNECTED;
		if (prev_id[i] == d->id && prev[i] != d->state) { char p[100]; na_dpath(p, sizeof p, d); na_queue(2, p, d->state, prev[i], 0); }
		prev[i] = d->state; prev_id[i] = d->id;
	}
	{
		int st = NMAPI_STATE_DISCONNECTED, conn = NMAPI_CONN_NONE;
		static int prev_state = -1;
		for (int i = 0; i < NA_ACT; i++) if (na_acts[i].live && na_acts[i].state == NMAPI_AC_ACTIVATING) st = NMAPI_STATE_CONNECTING;
		if (st != NMAPI_STATE_CONNECTING) for (int i = 0; i < NA_ACT; i++) if (na_acts[i].live && na_acts[i].state == NMAPI_AC_ACTIVATED) {
			int s2 = NMAPI_STATE_CONNECTED_LOCAL;
			if (na_acts[i].dev >= 0 && (na_devs[na_acts[i].dev].x.gw4[0] || na_devs[na_acts[i].dev].x.gw6[0])) s2 = NMAPI_STATE_CONNECTED_GLOBAL;
			if (s2 > st || st == NMAPI_STATE_DISCONNECTED) st = s2;
		}
		conn = st == NMAPI_STATE_CONNECTED_GLOBAL ? NMAPI_CONN_FULL : st == NMAPI_STATE_CONNECTED_LOCAL ? NMAPI_CONN_LIMITED : NMAPI_CONN_NONE;
		na_state = st; na_connectivity = conn;
		if (prev_state >= 0 && prev_state != st) na_queue(1, NMAPI_PATH, (uint32_t)st, 0, 0);
		prev_state = st;
	}
}

/* ---- snapshot + diff -> signals --------------------------------------------------------------------------------------- */

typedef struct { char path[100], iface[60], prop[40]; unsigned char *b; size_t n; } na_ent;
typedef struct { na_ent *e; int n, cap; } na_snap;
static na_snap na_last;

static void na_snap_add(na_snap *s, const char *path, const char *iface, const char *prop, const unsigned char *b, size_t n)
{
	na_ent *e;
	if (s->n == s->cap && !(s->e = realloc(s->e, (size_t)(s->cap = s->cap * 2 + 256) * sizeof *s->e))) exit(1);
	e = &s->e[s->n++];
	snprintf(e->path, sizeof e->path, "%s", path); snprintf(e->iface, sizeof e->iface, "%s", iface); snprintf(e->prop, sizeof e->prop, "%s", prop);
	e->n = n; e->b = n ? malloc(n) : NULL;
	if (n && !e->b) exit(1);
	if (n) memcpy(e->b, b, n);
}
static void na_snap_free(na_snap *s) { for (int i = 0; i < s->n; i++) free(s->e[i].b); free(s->e); memset(s, 0, sizeof *s); }
static void na_snap_cb(void *u, const char *path, int kind, int slot)
{
	const char *ifs[4];
	int ni = na_ifaces(kind, slot, ifs);
	for (int i = 0; i < ni; i++) {
		na_ctx c; na_buf sc = {0};
		memset(&c, 0, sizeof c);
		c.real = &sc;
		na_snap_add(u, path, ifs[i], "", NULL, 0);
		na_props(&c, kind, slot, ifs[i]);
		for (int m = 0; m < c.nmarks; m++) {
			size_t end = m + 1 < c.nmarks ? c.marks[m + 1].off : sc.n;
			na_snap_add(u, path, ifs[i], c.marks[m].name, sc.b + c.marks[m].off, end - c.marks[m].off);
		}
		free(sc.b); free(c.trash.b);
	}
}
static void na_snapshot(na_snap *s) { memset(s, 0, sizeof *s); na_objs(na_snap_cb, s); }
static na_ent *na_snap_find(const na_snap *s, const na_ent *k, int hint)
{
	if (hint < s->n && !strcmp(s->e[hint].path, k->path) && !strcmp(s->e[hint].iface, k->iface) && !strcmp(s->e[hint].prop, k->prop)) return &s->e[hint];
	for (int i = 0; i < s->n; i++) if (!strcmp(s->e[i].path, k->path) && !strcmp(s->e[i].iface, k->iface) && !strcmp(s->e[i].prop, k->prop)) return &s->e[i];
	return NULL;
}
static int na_snap_has_path(const na_snap *s, const char *path) { for (int i = 0; i < s->n; i++) if (!strcmp(s->e[i].path, path)) return 1; return 0; }

static void na_emit_ifaces(na_buf *o, int kind, int slot)   /* a{sa{sv}} of one object */
{
	const char *ifs[4];
	int ni = na_ifaces(kind, slot, ifs);
	size_t lp, st;
	na_arr(o, &lp, &st, 8);
	for (int i = 0; i < ni; i++) {
		na_ctx c; size_t l2, s2;
		memset(&c, 0, sizeof c);
		netman_dbus_align(o, 8); netman_dbus_put_str(o, ifs[i]);
		na_arr(o, &l2, &s2, 8);
		c.real = o;
		na_props(&c, kind, slot, ifs[i]);
		free(c.trash.b);
		na_arr_end(o, l2, s2);
	}
	na_arr_end(o, lp, st);
}
static void na_sig_prop_changed(int fd, const char *path, const char *iface, const char *names, int kind, int slot)
{
	na_buf b = {0}; na_ctx c; size_t lp, st;
	memset(&c, 0, sizeof c);
	netman_dbus_put_str(&b, iface);
	na_arr(&b, &lp, &st, 8);
	c.real = &b; c.only = names;
	na_props(&c, kind, slot, iface);
	free(c.trash.b);
	na_arr_end(&b, lp, st);
	na_empty_arr(&b, 4);
	na_send_signal(fd, path, "org.freedesktop.DBus.Properties", "PropertiesChanged", "sa{sv}as", &b);
	free(b.b);
}
static void na_sig_path(int fd, const char *path, const char *iface, const char *member, const char *target)
{
	na_buf b = {0};
	netman_dbus_put_str(&b, target);
	na_send_signal(fd, path, iface, member, "o", &b);
	free(b.b);
}

static void na_flush(int fd)
{
	na_snap now;
	char wpath[100] = "";
	na_dev *wd = na_wifi_dev();
	if (wd) na_dpath(wpath, sizeof wpath, wd);
	if (fd < 0) { na_nq = 0; return; }
	na_snapshot(&now);
	/* new objects */
	for (int i = 0; i < now.n; i++) {
		na_ent *e = &now.e[i];
		int kind, slot;
		if (e->prop[0] || (i && !strcmp(now.e[i - 1].path, e->path)) || na_snap_has_path(&na_last, e->path)) continue;
		if (!na_find(e->path, &kind, &slot)) continue;
		{
			na_buf b = {0};
			netman_dbus_put_str(&b, e->path);
			na_emit_ifaces(&b, kind, slot);
			na_send_signal(fd, NMAPI_ROOT, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", "oa{sa{sv}}", &b);
			free(b.b);
		}
		if (kind == K_DEV) na_sig_path(fd, NMAPI_PATH, NMI, "DeviceAdded", e->path);
		else if (kind == K_AP && wpath[0]) na_sig_path(fd, wpath, NMI ".Device.Wireless", "AccessPointAdded", e->path);
		else if (kind == K_CONN) na_sig_path(fd, NMAPI_SETTINGS, NMI ".Settings", "NewConnection", e->path);
	}
	/* removed objects */
	for (int i = 0; i < na_last.n; i++) {
		na_ent *e = &na_last.e[i];
		if (e->prop[0] || (i && !strcmp(na_last.e[i - 1].path, e->path)) || na_snap_has_path(&now, e->path)) continue;
		{
			na_buf b = {0}; size_t lp, st;
			netman_dbus_put_str(&b, e->path);
			na_arr(&b, &lp, &st, 4);
			for (int j = i; j < na_last.n && !strcmp(na_last.e[j].path, e->path); j++) if (!na_last.e[j].prop[0]) netman_dbus_put_str(&b, na_last.e[j].iface);
			na_arr_end(&b, lp, st);
			na_send_signal(fd, NMAPI_ROOT, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", "oas", &b);
			free(b.b);
		}
		if (!strncmp(e->path, NMAPI_PATH "/Devices/", strlen(NMAPI_PATH "/Devices/"))) na_sig_path(fd, NMAPI_PATH, NMI, "DeviceRemoved", e->path);
		else if (!strncmp(e->path, NMAPI_PATH "/AccessPoint/", strlen(NMAPI_PATH "/AccessPoint/")) && wpath[0]) na_sig_path(fd, wpath, NMI ".Device.Wireless", "AccessPointRemoved", e->path);
		else if (!strncmp(e->path, NMAPI_SETTINGS "/", strlen(NMAPI_SETTINGS "/"))) na_sig_path(fd, NMAPI_SETTINGS, NMI ".Settings", "ConnectionRemoved", e->path);
	}
	/* changed properties of objects that stayed */
	for (int i = 0; i < now.n; ) {
		int j = i, kind, slot;
		char names[600] = "";
		while (j < now.n && !strcmp(now.e[j].path, now.e[i].path) && !strcmp(now.e[j].iface, now.e[i].iface)) {
			na_ent *e = &now.e[j];
			if (e->prop[0]) {
				na_ent *o = na_snap_find(&na_last, e, j);
				if (o && (o->n != e->n || memcmp(o->b, e->b, e->n))) { size_t l = strlen(names); snprintf(names + l, sizeof names - l, "%s%s", l ? "," : "", e->prop); }
			}
			j++;
		}
		if (names[0] && na_snap_has_path(&na_last, now.e[i].path) && na_find(now.e[i].path, &kind, &slot)) na_sig_prop_changed(fd, now.e[i].path, now.e[i].iface, names, kind, slot);
		i = j;
	}
	/* NM-specific state signals queued by the refresh */
	for (int i = 0; i < na_nq; i++) {
		na_buf b = {0};
		na_pend *p = &na_q[i];
		if (p->kind == 1) { netman_dbus_put_u32(&b, p->a); na_send_signal(fd, NMAPI_PATH, NMI, "StateChanged", "u", &b); }
		else if (p->kind == 2) { netman_dbus_put_u32(&b, p->a); netman_dbus_put_u32(&b, p->b); netman_dbus_put_u32(&b, p->c); na_send_signal(fd, p->path, NMI ".Device", "StateChanged", "uuu", &b); }
		else if (p->kind == 3) { netman_dbus_put_u32(&b, p->a); netman_dbus_put_u32(&b, p->b); na_send_signal(fd, p->path, NMI ".Connection.Active", "StateChanged", "uu", &b); }
		else if (p->kind == 4) na_send_signal(fd, p->path, NMI ".Settings.Connection", "Updated", NULL, NULL);
		free(b.b);
	}
	na_nq = 0;
	na_snap_free(&na_last);
	na_last = now;
}

void nmapi_tick(int fd, int scan)
{
	na_refresh(scan ? 2 : 1);
	na_flush(fd);
}
int nmapi_start(int fd)
{
	na_fd = fd;
	na_refresh(1);
	na_nq = 0;
	na_snapshot(&na_last);
	return 0;
}

/* ---- connection settings: a{sa{sv}} out and in ------------------------------------------------------------------------ */

static void na_grp(na_buf *b, const char *name, size_t *lp, size_t *st) { netman_dbus_align(b, 8); netman_dbus_put_str(b, name); na_arr(b, lp, st, 8); }

static void na_settings_body(na_buf *b, const na_conn *cn, int secrets)
{
	size_t lp, st, l2, s2;
	na_arr(b, &lp, &st, 8);
	if (secrets) {
		if (cn->type == CT_WIFI && cn->psk[0]) { na_grp(b, "802-11-wireless-security", &l2, &s2); na_kv_s(b, "psk", cn->psk); na_arr_end(b, l2, s2); }
		na_arr_end(b, lp, st);
		return;
	}
	na_grp(b, "connection", &l2, &s2);
	na_kv_s(b, "id", cn->name); na_kv_s(b, "uuid", cn->uuid); na_kv_s(b, "type", na_ctype(cn->type));
	if (cn->iface[0] && cn->type != CT_WIFI) na_kv_s(b, "interface-name", cn->iface);
	na_kv_b(b, "autoconnect", cn->autoc);
	na_arr_end(b, l2, s2);
	if (cn->type == CT_ETH) { na_grp(b, "802-3-ethernet", &l2, &s2); na_arr_end(b, l2, s2); }
	else if (cn->type == CT_WIFI) {
		size_t l3, s3;
		na_grp(b, "802-11-wireless", &l2, &s2);
		na_kv(b, "ssid", "ay"); na_arr(b, &l3, &s3, 1); netman_dbus_put(b, cn->ssid, strlen(cn->ssid)); na_arr_end(b, l3, s3);
		na_kv_s(b, "mode", "infrastructure");
		if (cn->sec != SEC_OPEN) na_kv_s(b, "security", "802-11-wireless-security");
		na_arr_end(b, l2, s2);
		if (cn->sec != SEC_OPEN) {
			na_grp(b, "802-11-wireless-security", &l2, &s2);
			na_kv_s(b, "key-mgmt", cn->sec == SEC_WEP ? "none" : cn->sec == SEC_WPA3 ? "sae" : cn->sec == SEC_8021X ? "wpa-eap" : "wpa-psk");
			if (cn->sec != SEC_8021X) na_kv_u(b, cn->sec == SEC_WEP ? "wep-key-flags" : "psk-flags", 0);
			na_arr_end(b, l2, s2);
		}
	} else if (cn->type == CT_WG) { na_grp(b, "wireguard", &l2, &s2); na_arr_end(b, l2, s2); }
	else { na_grp(b, "vpn", &l2, &s2); na_kv_s(b, "service-type", "org.freedesktop.NetworkManager.openvpn"); na_arr_end(b, l2, s2); }
	na_grp(b, "ipv4", &l2, &s2);
	{
		int manual = (cn->type == CT_ETH || cn->type == CT_WIFI) ? !cn->dhcp : cn->type == CT_WG;
		na_kv_s(b, "method", manual ? "manual" : "auto");
		if (manual && cn->addr[0]) {
			char a[64]; int pl = 24; size_t l3, s3, l4, s4;
			snprintf(a, sizeof a, "%s", cn->addr);
			{ char *sl = strchr(a, '/'); if (sl) { *sl = 0; pl = atoi(sl + 1); } }
			na_kv(b, "address-data", "aa{sv}"); na_arr(b, &l3, &s3, 4);
			na_arr(b, &l4, &s4, 8); na_kv_s(b, "address", a); na_kv_u(b, "prefix", (uint32_t)pl); na_arr_end(b, l4, s4);
			na_arr_end(b, l3, s3);
			if (cn->gw[0]) na_kv_s(b, "gateway", cn->gw);
		}
		if (cn->ndns) {
			size_t l3, s3;
			na_kv(b, "dns", "au"); na_arr(b, &l3, &s3, 4);
			for (int i = 0; i < cn->ndns && i < 3; i++) { struct in_addr ia; if (inet_pton(AF_INET, cn->dns[i], &ia) == 1) netman_dbus_put_u32(b, ia.s_addr); }
			na_arr_end(b, l3, s3);
		}
	}
	na_arr_end(b, l2, s2);
	na_grp(b, "ipv6", &l2, &s2);
	na_kv_s(b, "method", cn->type == CT_ETH || cn->type == CT_WIFI ? "auto" : "ignore");
	na_arr_end(b, l2, s2);
	na_arr_end(b, lp, st);
}

typedef struct { char id[80], uuid[40], type[40], iface[16], ssid[40], psk[72], keymgmt[24], method4[16], addr[64], gw[46], dns[3][46]; int ndns, autoc, prefix, have_autoc; } na_set;

static void na_parse_addr_dict(netman_dbus_R *r, na_set *s, int record)
{
	uint32_t len = netman_dbus_r_u32(r);
	size_t end;
	char addr[64] = ""; int pl = -1;
	netman_dbus_ralign(r, 8);
	end = r->pos + len;
	while (!r->bad && r->pos < end) {
		const char *key, *sig;
		netman_dbus_ralign(r, 8);
		key = netman_dbus_r_str(r); sig = netman_dbus_r_sig(r);
		if (*sig == 's' && !strcmp(key, "address")) snprintf(addr, sizeof addr, "%s", netman_dbus_r_str(r));
		else if (*sig == 'u' && !strcmp(key, "prefix")) pl = (int)netman_dbus_r_u32(r);
		else { const char *t = sig; netman_dbus_skip_val(r, &t); }
	}
	if (record && addr[0]) { s->prefix = pl; snprintf(s->addr, sizeof s->addr, "%s", addr); }
}

static int na_parse_set(netman_dbus_R *r, na_set *s)
{
	uint32_t len;
	size_t end;
	memset(s, 0, sizeof *s);
	s->autoc = 1;
	len = netman_dbus_r_u32(r); netman_dbus_ralign(r, 8); end = r->pos + len;
	while (!r->bad && r->pos < end) {
		const char *grp; uint32_t ilen; size_t iend;
		netman_dbus_ralign(r, 8);
		grp = netman_dbus_r_str(r);
		ilen = netman_dbus_r_u32(r); netman_dbus_ralign(r, 8); iend = r->pos + ilen;
		while (!r->bad && r->pos < iend) {
			const char *key, *sig, *t;
			netman_dbus_ralign(r, 8);
			key = netman_dbus_r_str(r); sig = netman_dbus_r_sig(r); t = sig;
			if (*sig == 's' && !sig[1]) {
				const char *v = netman_dbus_r_str(r);
				if (!strcmp(grp, "connection") && !strcmp(key, "id")) snprintf(s->id, sizeof s->id, "%s", v);
				else if (!strcmp(grp, "connection") && !strcmp(key, "uuid")) snprintf(s->uuid, sizeof s->uuid, "%s", v);
				else if (!strcmp(grp, "connection") && !strcmp(key, "type")) snprintf(s->type, sizeof s->type, "%s", v);
				else if (!strcmp(grp, "connection") && !strcmp(key, "interface-name")) snprintf(s->iface, sizeof s->iface, "%s", v);
				else if (!strcmp(grp, "802-11-wireless-security") && !strcmp(key, "psk")) snprintf(s->psk, sizeof s->psk, "%s", v);
				else if (!strcmp(grp, "802-11-wireless-security") && !strcmp(key, "key-mgmt")) snprintf(s->keymgmt, sizeof s->keymgmt, "%s", v);
				else if (!strcmp(grp, "ipv4") && !strcmp(key, "method")) snprintf(s->method4, sizeof s->method4, "%s", v);
				else if (!strcmp(grp, "ipv4") && !strcmp(key, "gateway")) snprintf(s->gw, sizeof s->gw, "%s", v);
			} else if (*sig == 'b' && !sig[1]) {
				uint32_t v = netman_dbus_r_u32(r);
				if (!strcmp(grp, "connection") && !strcmp(key, "autoconnect")) { s->autoc = (int)v; s->have_autoc = 1; }
			} else if (!strcmp(sig, "ay")) {
				uint32_t n = netman_dbus_r_u32(r);
				if (r->bad || r->pos + n > r->len) { r->bad = 1; break; }
				if (!strcmp(grp, "802-11-wireless") && !strcmp(key, "ssid")) { memcpy(s->ssid, r->p + r->pos, n > 32 ? 32 : n); s->ssid[n > 32 ? 32 : n] = 0; }
				r->pos += n;
			} else if (!strcmp(sig, "au") && !strcmp(grp, "ipv4") && !strcmp(key, "dns")) {
				uint32_t n = netman_dbus_r_u32(r);
				for (uint32_t k = 0; k < n / 4 && !r->bad; k++) {
					struct in_addr ia; ia.s_addr = netman_dbus_r_u32(r);
					if (s->ndns < 3) inet_ntop(AF_INET, &ia, s->dns[s->ndns++], 46);
				}
			} else if (!strcmp(sig, "aa{sv}") && !strcmp(grp, "ipv4") && !strcmp(key, "address-data")) {
				uint32_t n = netman_dbus_r_u32(r);
				size_t e2;
				int first = 1;
				netman_dbus_ralign(r, 4);
				e2 = r->pos + n;
				while (!r->bad && r->pos < e2) { na_parse_addr_dict(r, s, first); first = 0; }
			} else netman_dbus_skip_val(r, &t);
		}
	}
	return r->bad ? -1 : 0;
}

/* a parsed setting -> our connection; 0 or -1 (message in err) */
static int na_apply_set(na_conn *cn, const na_set *s, char *err, size_t en)
{
	int type = !strcmp(s->type, "802-3-ethernet") || !strcmp(s->type, "ethernet") ? CT_ETH : !strcmp(s->type, "802-11-wireless") || !strcmp(s->type, "wifi") ? CT_WIFI : !strcmp(s->type, "wireguard") ? CT_WG : !strcmp(s->type, "vpn") ? CT_VPN : 0;
	if (!type) { snprintf(err, en, "connection type '%s' is not supported", s->type); return -1; }
	if (type == CT_WIFI && !s->ssid[0]) { snprintf(err, en, "802-11-wireless.ssid is required"); return -1; }
	cn->type = type;
	snprintf(cn->name, sizeof cn->name, "%s", s->id[0] ? s->id : type == CT_WIFI ? s->ssid : "connection");
	snprintf(cn->uuid, sizeof cn->uuid, "%s", s->uuid);
	if (!cn->uuid[0]) { char seed[200]; snprintf(seed, sizeof seed, "%s:%s:%d", cn->name, s->ssid, cn->id); na_uuid(seed, cn->uuid, sizeof cn->uuid); }
	snprintf(cn->iface, sizeof cn->iface, "%s", s->iface);
	snprintf(cn->ssid, sizeof cn->ssid, "%s", s->ssid);
	if (s->psk[0]) snprintf(cn->psk, sizeof cn->psk, "%s", s->psk);
	cn->autoc = s->autoc;
	cn->dhcp = strcmp(s->method4, "manual") != 0;
	cn->addr[0] = 0;
	if (s->addr[0]) snprintf(cn->addr, sizeof cn->addr, "%s/%d", s->addr, s->prefix >= 0 ? s->prefix : 24);
	snprintf(cn->gw, sizeof cn->gw, "%s", s->gw);
	cn->ndns = s->ndns;
	for (int i = 0; i < 3; i++) snprintf(cn->dns[i], sizeof cn->dns[i], "%s", i < s->ndns ? s->dns[i] : "");
	cn->sec = SEC_OPEN;
	if (type == CT_WIFI) {
		if (!strcmp(s->keymgmt, "sae")) cn->sec = SEC_WPA3;
		else if (!strcmp(s->keymgmt, "wpa-eap")) cn->sec = SEC_8021X;
		else if (!strcmp(s->keymgmt, "none") && s->psk[0]) cn->sec = SEC_WEP;
		else if (s->keymgmt[0] || s->psk[0]) cn->sec = SEC_WPA2;
	}
	return 0;
}

/* ---- backends ---------------------------------------------------------------------------------------------------- */

static FILE *na_null(void) { static FILE *f; if (!f) f = fopen("/dev/null", "w"); return f; }

static int na_profile_of(const na_conn *cn, netman_profile *p)
{
	memset(p, 0, sizeof *p);
	snprintf(p->name, sizeof p->name, "%s", cn->name);
	snprintf(p->iface, sizeof p->iface, "%s", cn->iface);
	p->dhcp = cn->dhcp; p->autoconnect = cn->autoc;
	snprintf(p->addr, sizeof p->addr, "%s", cn->addr);
	snprintf(p->gateway, sizeof p->gateway, "%s", cn->gw);
	p->ndns = cn->ndns;
	for (int i = 0; i < cn->ndns && i < 3; i++) snprintf(p->dns[i], sizeof p->dns[i], "%s", cn->dns[i]);
	return 0;
}

/* bring a connection up; 0 or -1 with a reason text, *wrongpw = 1 for a Wi-Fi password failure */
static int na_backend_up(const na_conn *cn, const na_dev *d, char *err, size_t en, int *wrongpw)
{
	FILE *so = netman_out, *se = netman_err, *vo = netman_vpn_out, *ve = netman_vpn_err;
	char *eb = NULL; size_t es = 0;
	int rc = -1;
	*wrongpw = 0; err[0] = 0;
	netman_out = na_null(); netman_err = open_memstream(&eb, &es); netman_vpn_out = na_null(); netman_vpn_err = netman_err;
	if (cn->type == CT_WIFI) {
		rc = netman_wifi_connect(cn->ssid, cn->psk[0] ? cn->psk : NULL) == 0 ? 0 : -1;
		if (rc) { *wrongpw = netman_wifi_last_wrong_password(); snprintf(err, en, "%s", netman_wifi_last_error()); }
	} else if (cn->type == CT_ETH) {
		netman_profile p; netman_lease l; int r;
		if (cn->src == SRC_PROFILE && netman_profile_load(cn->name, &p) == 0) { if (cn->iface[0] == 0) snprintf(p.iface, sizeof p.iface, "%s", d ? d->name : ""); }
		else na_profile_of(cn, &p);
		if (!p.iface[0] && d) snprintf(p.iface, sizeof p.iface, "%s", d->name);
		r = netman_profile_apply(&p, &l);
		rc = r < 0 ? -1 : 0;
		if (r < 0) snprintf(err, en, "%s%s%s", strerror(-r), netman_error() && *netman_error() ? ": " : "", netman_error() ? netman_error() : "");
	} else {
		rc = netman_vpn_up(cn->name, 15) == 0 ? 0 : -1;
		if (rc) snprintf(err, en, "vpn up failed");
	}
	fflush(netman_err);
	if (rc && !err[0] && eb) snprintf(err, en, "%s", eb);
	fclose(netman_err); free(eb);
	netman_out = so; netman_err = se; netman_vpn_out = vo; netman_vpn_err = ve;
	if (err[strcspn(err, "\n")]) err[strcspn(err, "\n")] = 0;
	return rc;
}
static int na_backend_down(const na_conn *cn, const na_dev *d)
{
	FILE *so = netman_out, *se = netman_err, *vo = netman_vpn_out, *ve = netman_vpn_err;
	int rc;
	netman_out = netman_err = na_null(); netman_vpn_out = netman_vpn_err = na_null();
	if (cn->type == CT_WIFI) rc = netman_wifi_disconnect();
	else if (cn->type == CT_ETH) rc = d ? (netman_iface_reset(d->name) < 0) : 1;
	else rc = netman_vpn_down(cn->name);
	netman_out = so; netman_err = se; netman_vpn_out = vo; netman_vpn_err = ve;
	return rc;
}

/* ---- method handlers ---------------------------------------------------------------------------------------------- */

static void na_reply(int fd, const netman_dbus_Msg *m, const char *sig, na_buf *b) { netman_dbus_reply_to_call(fd, m, sig, b); }
static void na_reply_empty(int fd, const netman_dbus_Msg *m) { netman_dbus_reply_to_call(fd, m, NULL, NULL); }
static void na_reply_o(int fd, const netman_dbus_Msg *m, const char *path) { na_buf b = {0}; netman_dbus_put_str(&b, path); na_reply(fd, m, "o", &b); free(b.b); }

static na_act *na_act_new(int ci, int di)
{
	for (int j = 0; j < NA_ACT; j++) if (!na_acts[j].live) {
		memset(&na_acts[j], 0, sizeof na_acts[j]);
		na_acts[j].live = 1; na_acts[j].id = na_nextid++; na_acts[j].conn = ci; na_acts[j].dev = di; na_acts[j].state = NMAPI_AC_ACTIVATING; na_acts[j].expl = 1;
		return &na_acts[j];
	}
	return NULL;
}

/* mode 0 ActivateConnection (o), 1 AddAndActivateConnection (oo), 2 ...2 (ooa{sv}) */
static void na_activate(int fd, const netman_dbus_Msg *m, int ci, int di, int mode)
{
	na_conn *cn = &na_conns[ci];
	na_act *a;
	char ap[128], cp[128], err[300];
	int rc, wrong;
	if (di < 0 && cn->type != CT_VPN && cn->type != CT_WG) {
		if (cn->type == CT_WIFI) { na_dev *w = na_wifi_dev(); di = w ? (int)(w - na_devs) : -1; }
		else for (int i = 0; i < NA_DEV && di < 0; i++) if (na_devs[i].live && na_pred_avail(ci, i)) di = i;
		if (di < 0) { na_send_err(fd, m, NMI ".UnknownDevice", "no device can activate this connection"); return; }
	}
	if (di >= 0 && !na_pred_avail(ci, di) && cn->type != CT_VPN) { na_send_err(fd, m, NMI ".ConnectionNotAvailable", "the connection does not fit the device"); return; }
	for (int j = 0; j < NA_ACT; j++) if (na_acts[j].live && na_acts[j].conn == ci && na_acts[j].dev == di && na_acts[j].state != NMAPI_AC_DEACTIVATED && mode == 0 && na_acts[j].state == NMAPI_AC_ACTIVATED) { na_path(ap, sizeof ap, K_ACT, na_acts[j].id); na_reply_o(fd, m, ap); return; }
	if (!(a = na_act_new(ci, di))) { na_send_err(fd, m, NMI ".Failed", "too many active connections"); return; }
	na_path(ap, sizeof ap, K_ACT, a->id);
	na_path(cp, sizeof cp, K_CONN, cn->id);
	na_refresh(0);
	na_flush(fd);   /* the activation exists (state activating) before the reply, like NetworkManager */
	{
		na_buf b = {0};
		if (mode) netman_dbus_put_str(&b, cp);
		netman_dbus_put_str(&b, ap);
		if (mode == 2) na_empty_arr(&b, 8);
		na_reply(fd, m, mode == 0 ? "o" : mode == 1 ? "oo" : "ooa{sv}", &b);
		free(b.b);
	}
	rc = na_backend_up(cn, di >= 0 ? &na_devs[di] : NULL, err, sizeof err, &wrong);
	if (rc == 0) {
		a->state = NMAPI_AC_ACTIVATED;
		na_queue(3, ap, NMAPI_AC_ACTIVATED, 0, 0);
		na_refresh(1);
		na_flush(fd);
	} else {
		fprintf(stderr, "netman daemon: activation of '%s' failed: %s\n", cn->name, err);
		a->state = NMAPI_AC_DEACTIVATED;
		na_queue(3, ap, NMAPI_AC_DEACTIVATED, wrong ? 9 : 3, 0);
		na_flush(fd);
		a->live = 0;
		na_refresh(1);
		na_flush(fd);
	}
}

static void na_deactivate(int fd, int slot)
{
	na_act *a = &na_acts[slot];
	char ap[128];
	na_path(ap, sizeof ap, K_ACT, a->id);
	if (a->conn >= 0) na_backend_down(&na_conns[a->conn], a->dev >= 0 ? &na_devs[a->dev] : NULL);
	a->state = NMAPI_AC_DEACTIVATED;
	na_queue(3, ap, NMAPI_AC_DEACTIVATED, 2, 0);
	na_flush(fd);
	a->live = 0;
	na_refresh(1);
	na_flush(fd);
}

static int na_conn_save_profile(const na_conn *cn, char *err, size_t en)
{
	netman_profile p; int r;
	na_profile_of(cn, &p);
	if (!p.iface[0]) for (int i = 0; i < NA_DEV; i++) if (na_devs[i].live && na_devs[i].type == NMAPI_DEV_ETHERNET) { snprintf(p.iface, sizeof p.iface, "%s", na_devs[i].name); break; }
	if (!p.iface[0]) { snprintf(err, en, "no interface-name and no Ethernet device to take it from"); return -1; }
	if ((r = netman_profile_save(&p)) < 0) { snprintf(err, en, "%s%s%s", strerror(-r), netman_error() && *netman_error() ? ": " : "", netman_error() ? netman_error() : ""); return -1; }
	return 0;
}

/* add a connection from a parsed setting; returns its slot or -1 */
static int na_add_conn(const na_set *s, int unsaved, char *err, size_t en)
{
	na_conn tmp, *cn = NULL;
	int created;
	memset(&tmp, 0, sizeof tmp);
	tmp.id = na_nextid;
	if (na_apply_set(&tmp, s, err, en)) return -1;
	if (tmp.type == CT_ETH && !unsaved) {
		char key[100];
		if (na_conn_save_profile(&tmp, err, en)) return -1;
		na_refresh(0);
		snprintf(key, sizeof key, "p:%s", tmp.name);
		cn = na_conn_by_key(key);
		return cn ? (int)(cn - na_conns) : -1;
	}
	{
		char key[100];
		snprintf(key, sizeof key, "r:%s", tmp.uuid);
		if (!(cn = na_conn_upsert(key, SRC_RUNTIME, &created))) { snprintf(err, en, "too many connections"); return -1; }
		tmp.live = 1; tmp.seen = 1; tmp.src = SRC_RUNTIME; tmp.unsaved = 1;
		snprintf(tmp.key, sizeof tmp.key, "%s", key);
		if (created) tmp.id = cn->id; else tmp.id = cn->id;
		*cn = tmp;
	}
	return (int)(cn - na_conns);
}

static void na_set_settings_ver(void) { na_settings_ver++; }

/* ---- introspection ------------------------------------------------------------------------------------------------ */

static const char na_xml_std[] =
	"<interface name=\"org.freedesktop.DBus.Peer\">\n <method name=\"Ping\"/>\n <method name=\"GetMachineId\">\n  <arg type=\"s\" name=\"machine_uuid\" direction=\"out\"/>\n </method>\n</interface>\n"
	"<interface name=\"org.freedesktop.DBus.Introspectable\">\n <method name=\"Introspect\">\n  <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n </method>\n</interface>\n"
	"<interface name=\"org.freedesktop.DBus.Properties\">\n <method name=\"Get\">\n  <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n  <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n  <arg name=\"value\" type=\"v\" direction=\"out\"/>\n </method>\n"
	" <method name=\"GetAll\">\n  <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n  <arg name=\"properties\" type=\"a{sv}\" direction=\"out\"/>\n </method>\n"
	" <method name=\"Set\">\n  <arg name=\"interface_name\" type=\"s\" direction=\"in\"/>\n  <arg name=\"property_name\" type=\"s\" direction=\"in\"/>\n  <arg name=\"value\" type=\"v\" direction=\"in\"/>\n </method>\n"
	" <signal name=\"PropertiesChanged\">\n  <arg type=\"s\" name=\"interface_name\"/>\n  <arg type=\"a{sv}\" name=\"changed_properties\"/>\n  <arg type=\"as\" name=\"invalidated_properties\"/>\n </signal>\n</interface>\n";
static const char na_xml_om[] =
	"<interface name=\"org.freedesktop.DBus.ObjectManager\">\n <method name=\"GetManagedObjects\">\n  <arg type=\"a{oa{sa{sv}}}\" name=\"objpath_interfaces_and_properties\" direction=\"out\"/>\n </method>\n"
	" <signal name=\"InterfacesAdded\">\n  <arg type=\"o\" name=\"object_path\"/>\n  <arg type=\"a{sa{sv}}\" name=\"interfaces_and_properties\"/>\n </signal>\n"
	" <signal name=\"InterfacesRemoved\">\n  <arg type=\"o\" name=\"object_path\"/>\n  <arg type=\"as\" name=\"interfaces\"/>\n </signal>\n</interface>\n";

static void na_child_cb(void *u, const char *path, int kind, int slot)
{
	struct { const char *parent; size_t pl; char seen[40][48]; int n; } *c = u;
	const char *rest;
	size_t l;
	(void)kind; (void)slot;
	if (strncmp(path, c->parent, c->pl) || (c->pl > 1 && path[c->pl] != '/')) return;
	rest = path + c->pl + (c->pl > 1 ? 1 : 0);
	if (!*rest) return;
	l = strcspn(rest, "/");
	if (l >= 48) return;
	for (int i = 0; i < c->n; i++) if (strlen(c->seen[i]) == l && !strncmp(c->seen[i], rest, l)) return;
	if (c->n < 40) { memcpy(c->seen[c->n], rest, l); c->seen[c->n][l] = 0; c->n++; }
}

/* 1 when path has an object or children */
static int na_introspect(const char *path, na_buf *x)
{
	static struct { const char *parent; size_t pl; char seen[40][48]; int n; } c;
	int kind, slot, known;
	const char *ifs[4];
	const char *s = "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n<node>\n";
	known = na_find(path, &kind, &slot);
	c.parent = path; c.pl = strlen(path); c.n = 0;
	na_objs(na_child_cb, &c);
	if (!strcmp(path, "/")) { snprintf(c.seen[0], 48, "org"); c.n = 1; }
	else if (!strcmp(path, "/org")) { snprintf(c.seen[0], 48, "freedesktop"); c.n = 1; }
	else if (!strcmp(path, NMAPI_ROOT)) { snprintf(c.seen[0], 48, "NetworkManager"); c.n = 1; }
	if (!known && !c.n && strcmp(path, NMAPI_ROOT)) return 0;
	netman_dbus_put(x, s, strlen(s));
	netman_dbus_put(x, na_xml_std, strlen(na_xml_std));
	if (!strcmp(path, NMAPI_ROOT)) netman_dbus_put(x, na_xml_om, strlen(na_xml_om));
	if (known) {
		int ni = na_ifaces(kind, slot, ifs);
		for (int i = 0; i < ni; i++) { const char *xi = na_ifxml(ifs[i]); netman_dbus_put(x, xi, strlen(xi)); }
	}
	for (int i = 0; i < c.n; i++) { char l[100]; int n = snprintf(l, sizeof l, "<node name=\"%s\"/>\n", c.seen[i]); netman_dbus_put(x, l, (size_t)n); }
	netman_dbus_put(x, "</node>\n", 8);
	return 1;
}

static void na_mo_cb(void *u, const char *path, int kind, int slot)
{
	na_buf *o = u;
	netman_dbus_align(o, 8);
	netman_dbus_put_str(o, path);
	na_emit_ifaces(o, kind, slot);
}

/* ---- dispatch ------------------------------------------------------------------------------------------------------ */

static int na_slot_by_path(const char *p, int want_kind)
{
	int kind, slot;
	if (!na_find(p, &kind, &slot) || kind != want_kind) return -1;
	return slot;
}
static const char *na_perms[] = { "org.freedesktop.NetworkManager.enable-disable-network", "org.freedesktop.NetworkManager.enable-disable-wifi", "org.freedesktop.NetworkManager.enable-disable-wwan",
	"org.freedesktop.NetworkManager.enable-disable-wimax", "org.freedesktop.NetworkManager.sleep-wake", "org.freedesktop.NetworkManager.network-control", "org.freedesktop.NetworkManager.wifi.share.protected",
	"org.freedesktop.NetworkManager.wifi.share.open", "org.freedesktop.NetworkManager.settings.modify.system", "org.freedesktop.NetworkManager.settings.modify.own",
	"org.freedesktop.NetworkManager.settings.modify.hostname", "org.freedesktop.NetworkManager.settings.modify.global-dns", "org.freedesktop.NetworkManager.reload",
	"org.freedesktop.NetworkManager.checkpoint-rollback", "org.freedesktop.NetworkManager.enable-disable-statistics", "org.freedesktop.NetworkManager.enable-disable-connectivity-check",
	"org.freedesktop.NetworkManager.wifi.scan", NULL };

static int na_dispatch(int fd, const netman_dbus_Msg *m)
{
	const char *iface = m->iface, *mem = m->member;
	int kind, slot, known = na_find(m->path, &kind, &slot);
	netman_dbus_R r = { m->body, 0, m->blen, 0 };
	char err[300];

	if (!strcmp(iface, "org.freedesktop.DBus.Peer")) {
		if (!strcmp(mem, "Ping")) { na_reply_empty(fd, m); return 1; }
		if (!strcmp(mem, "GetMachineId")) { na_buf b = {0}; netman_dbus_put_str(&b, "6e6574206d616e2d6e6574206d616e21"); na_reply(fd, m, "s", &b); free(b.b); return 1; }
	}
	if (!strcmp(iface, "org.freedesktop.DBus.Introspectable") && !strcmp(mem, "Introspect")) {
		na_buf x = {0}, b = {0};
		if (!na_introspect(m->path, &x)) { na_send_err(fd, m, "org.freedesktop.DBus.Error.UnknownObject", "no such object"); free(x.b); return 1; }
		netman_dbus_put_u32(&b, (uint32_t)x.n); netman_dbus_put(&b, x.b, x.n); netman_dbus_put(&b, "", 1);
		na_reply(fd, m, "s", &b);
		free(x.b); free(b.b);
		return 1;
	}
	if (!strcmp(iface, "org.freedesktop.DBus.ObjectManager") && !strcmp(mem, "GetManagedObjects") && !strcmp(m->path, NMAPI_ROOT)) {
		na_buf b = {0}; size_t lp, st;
		na_arr(&b, &lp, &st, 8);
		na_objs(na_mo_cb, &b);
		na_arr_end(&b, lp, st);
		na_reply(fd, m, "a{oa{sa{sv}}}", &b);
		free(b.b);
		return 1;
	}
	if (!known) { na_send_err(fd, m, "org.freedesktop.DBus.Error.UnknownObject", "no such object"); return 1; }

	if (!strcmp(iface, "org.freedesktop.DBus.Properties")) {
		const char *wi = netman_dbus_r_str(&r), *ifs[4];
		int ni = na_ifaces(kind, slot, ifs), ok = 0;
		for (int i = 0; i < ni; i++) if (!strcmp(ifs[i], wi)) ok = 1;
		if (!strcmp(mem, "GetAll")) {
			na_buf b = {0}; na_ctx c; size_t lp, st;
			if (!ok) { na_send_err(fd, m, "org.freedesktop.DBus.Error.UnknownInterface", "no such interface"); return 1; }
			memset(&c, 0, sizeof c);
			na_arr(&b, &lp, &st, 8);
			c.real = &b;
			na_props(&c, kind, slot, wi);
			na_arr_end(&b, lp, st);
			na_reply(fd, m, "a{sv}", &b);
			free(b.b); free(c.trash.b);
			return 1;
		}
		if (!strcmp(mem, "Get")) {
			const char *pn = netman_dbus_r_str(&r);
			na_buf b = {0}; na_ctx c;
			if (!ok) { na_send_err(fd, m, "org.freedesktop.DBus.Error.UnknownInterface", "no such interface"); return 1; }
			memset(&c, 0, sizeof c);
			c.real = &b; c.only = pn; c.bare = 1;
			na_props(&c, kind, slot, wi);
			if (!c.hit) na_send_err(fd, m, "org.freedesktop.DBus.Error.InvalidArgs", "no such property");
			else na_reply(fd, m, "v", &b);
			free(b.b); free(c.trash.b);
			return 1;
		}
		if (!strcmp(mem, "Set")) {
			const char *pn = netman_dbus_r_str(&r), *sig = netman_dbus_r_sig(&r);
			int v = 0;
			if (*sig == 'b') v = netman_dbus_r_u32(&r) ? 1 : 0;
			if (kind == K_MGR && !strcmp(pn, "WirelessEnabled") && *sig == 'b') na_wifi_en = v;
			else if (kind == K_MGR && !strcmp(pn, "ConnectivityCheckEnabled") && *sig == 'b') na_conncheck = v;
			else if (kind == K_DEV && !strcmp(pn, "Autoconnect") && *sig == 'b') na_devs[slot].autoc = v;
			else if (kind == K_DEV && !strcmp(pn, "Managed") && *sig == 'b') na_devs[slot].managed = v;
			else { na_send_err(fd, m, "org.freedesktop.DBus.Error.PropertyReadOnly", "property is read-only or unknown"); return 1; }
			na_reply_empty(fd, m);
			na_refresh(0);
			na_flush(fd);
			return 1;
		}
	}

	if (kind == K_MGR && !strcmp(iface, NMI)) {
		if (!strcmp(mem, "GetDevices") || !strcmp(mem, "GetAllDevices")) {
			na_ctx c; na_buf b = {0};
			memset(&c, 0, sizeof c);
			c.real = &b; c.only = "Devices"; c.bare = 1;
			na_props(&c, K_MGR, 0, NMI);
			/* the bare variant is "ao" signature + array; the reply wants the array alone */
			{ na_buf rb = {0}; size_t off = 1 + 2 + 1; /* sig byte len + "ao" + nul */ netman_dbus_align(&rb, 4); netman_dbus_put(&rb, b.b + off + ((4 - off % 4) % 4), b.n - off - ((4 - off % 4) % 4)); na_reply(fd, m, "ao", &rb); free(rb.b); }
			free(b.b); free(c.trash.b);
			return 1;
		}
		if (!strcmp(mem, "GetDeviceByIpIface")) {
			const char *n = netman_dbus_r_str(&r); na_dev *d = na_dev_by_name(n); char p[128];
			if (!d) { na_send_err(fd, m, NMI ".UnknownDevice", "no such device"); return 1; }
			na_dpath(p, sizeof p, d); na_reply_o(fd, m, p);
			return 1;
		}
		if (!strcmp(mem, "ActivateConnection")) {
			const char *cp = netman_dbus_r_str(&r), *dp = netman_dbus_r_str(&r);
			int ci = na_slot_by_path(cp, K_CONN), di = strcmp(dp, "/") ? na_slot_by_path(dp, K_DEV) : -1;
			if (ci < 0) { na_send_err(fd, m, NMI ".UnknownConnection", "no such connection"); return 1; }
			if (strcmp(dp, "/") && di < 0) { na_send_err(fd, m, NMI ".UnknownDevice", "no such device"); return 1; }
			na_activate(fd, m, ci, di, 0);
			return 1;
		}
		if (!strcmp(mem, "AddAndActivateConnection") || !strcmp(mem, "AddAndActivateConnection2")) {
			na_set s; int ci, di, mode = mem[strlen(mem) - 1] == '2' ? 2 : 1;
			const char *dp, *sp;
			if (na_parse_set(&r, &s)) { na_send_err(fd, m, "org.freedesktop.DBus.Error.InvalidArgs", "cannot parse the connection settings"); return 1; }
			dp = netman_dbus_r_str(&r); sp = netman_dbus_r_str(&r);
			di = strcmp(dp, "/") ? na_slot_by_path(dp, K_DEV) : -1;
			if (strcmp(dp, "/") && di < 0) { na_send_err(fd, m, NMI ".UnknownDevice", "no such device"); return 1; }
			if ((!strcmp(s.type, "802-11-wireless") || !strcmp(s.type, "wifi")) && !s.ssid[0]) {   /* libnm may give only the AP in specific_object */
				int ak, as;
				if (na_find(sp, &ak, &as) && ak == K_AP) snprintf(s.ssid, sizeof s.ssid, "%s", na_aps[as].ssid);
			}
			if ((ci = na_add_conn(&s, 1, err, sizeof err)) < 0) { na_send_err(fd, m, NMI ".Failed", err); return 1; }
			na_set_settings_ver();
			na_refresh(0);
			na_flush(fd);
			na_activate(fd, m, ci, di, mode);
			return 1;
		}
		if (!strcmp(mem, "DeactivateConnection")) {
			int ai = na_slot_by_path(netman_dbus_r_str(&r), K_ACT);
			if (ai < 0) { na_send_err(fd, m, NMI ".ConnectionNotActive", "no such active connection"); return 1; }
			na_reply_empty(fd, m);
			na_deactivate(fd, ai);
			return 1;
		}
		if (!strcmp(mem, "CheckConnectivity")) { na_buf b = {0}; netman_dbus_put_u32(&b, (uint32_t)na_connectivity); na_reply(fd, m, "u", &b); free(b.b); return 1; }
		if (!strcmp(mem, "state")) { na_buf b = {0}; netman_dbus_put_u32(&b, (uint32_t)na_state); na_reply(fd, m, "u", &b); free(b.b); return 1; }
		if (!strcmp(mem, "GetPermissions")) {
			na_buf b = {0}; size_t lp, st;
			na_arr(&b, &lp, &st, 8);
			for (int i = 0; na_perms[i]; i++) { netman_dbus_align(&b, 8); netman_dbus_put_str(&b, na_perms[i]); netman_dbus_put_str(&b, "yes"); }
			na_arr_end(&b, lp, st);
			na_reply(fd, m, "a{ss}", &b); free(b.b);
			return 1;
		}
		if (!strcmp(mem, "GetLogging")) { na_buf b = {0}; netman_dbus_put_str(&b, "INFO"); netman_dbus_put_str(&b, "DEFAULT"); na_reply(fd, m, "ss", &b); free(b.b); return 1; }
		if (!strcmp(mem, "Reload")) { na_reply_empty(fd, m); na_refresh(1); na_flush(fd); return 1; }
		if (!strcmp(mem, "SetLogging")) { na_reply_empty(fd, m); return 1; }
		na_send_err(fd, m, NMI ".NotSupported", "this NetworkManager method is not implemented by netman");
		return 1;
	}
	if (kind == K_AGENT && !strcmp(iface, NMI ".AgentManager")) {   /* secret agents are accepted but never asked: secrets come from the connection itself */
		if (!strcmp(mem, "Register") || !strcmp(mem, "RegisterWithCapabilities") || !strcmp(mem, "Unregister")) { na_reply_empty(fd, m); return 1; }
	}
	if (kind == K_SET && !strcmp(iface, NMI ".Settings")) {
		if (!strcmp(mem, "ListConnections")) {
			na_ctx c; na_buf b = {0};
			memset(&c, 0, sizeof c);
			c.real = &b; c.only = "Connections"; c.bare = 1;
			na_props(&c, K_SET, 0, NMI ".Settings");
			{ size_t off = 1 + 2 + 1, pad = (4 - off % 4) % 4; na_buf rb = {0}; netman_dbus_put(&rb, b.b + off + pad, b.n - off - pad); na_reply(fd, m, "ao", &rb); free(rb.b); }
			free(b.b); free(c.trash.b);
			return 1;
		}
		if (!strcmp(mem, "GetConnectionByUuid")) {
			const char *u = netman_dbus_r_str(&r); char p[128];
			for (int i = 0; i < NA_CONN; i++) if (na_conns[i].live && !strcmp(na_conns[i].uuid, u)) { na_path(p, sizeof p, K_CONN, na_conns[i].id); na_reply_o(fd, m, p); return 1; }
			na_send_err(fd, m, NMI ".Settings.InvalidConnection", "no connection with this uuid");
			return 1;
		}
		if (!strcmp(mem, "AddConnection") || !strcmp(mem, "AddConnectionUnsaved") || !strcmp(mem, "AddConnection2")) {
			na_set s; int ci, v2 = !strcmp(mem, "AddConnection2"), unsaved = !strcmp(mem, "AddConnectionUnsaved");
			char p[128]; na_buf b = {0};
			if (na_parse_set(&r, &s)) { na_send_err(fd, m, "org.freedesktop.DBus.Error.InvalidArgs", "cannot parse the connection settings"); return 1; }
			if (v2) { uint32_t fl = netman_dbus_r_u32(&r); unsaved = !(fl & 1) && (fl & 2); }
			if ((ci = na_add_conn(&s, unsaved, err, sizeof err)) < 0) { na_send_err(fd, m, NMI ".Settings.InvalidSettings", err); return 1; }
			na_set_settings_ver();
			na_path(p, sizeof p, K_CONN, na_conns[ci].id);
			netman_dbus_put_str(&b, p);
			if (v2) na_empty_arr(&b, 8);
			na_reply(fd, m, v2 ? "oa{sv}" : "o", &b);
			free(b.b);
			na_refresh(0);
			na_flush(fd);
			return 1;
		}
		if (!strcmp(mem, "ReloadConnections")) { na_buf b = {0}; netman_dbus_put_u32(&b, 1); na_reply(fd, m, "b", &b); free(b.b); na_refresh(1); na_flush(fd); return 1; }
		if (!strcmp(mem, "SaveHostname")) { na_reply_empty(fd, m); return 1; }
		na_send_err(fd, m, NMI ".NotSupported", "this NetworkManager method is not implemented by netman");
		return 1;
	}
	if (kind == K_CONN && !strcmp(iface, NMI ".Settings.Connection")) {
		na_conn *cn = &na_conns[slot];
		if (!strcmp(mem, "GetSettings") || !strcmp(mem, "GetSecrets")) {
			na_buf b = {0};
			na_settings_body(&b, cn, mem[3] == 'S' && mem[4] == 'e');
			na_reply(fd, m, "a{sa{sv}}", &b); free(b.b);
			return 1;
		}
		if (!strcmp(mem, "ClearSecrets") || !strcmp(mem, "Save")) { na_reply_empty(fd, m); return 1; }
		if (!strcmp(mem, "Update") || !strcmp(mem, "UpdateUnsaved") || !strcmp(mem, "Update2")) {
			na_set s; na_conn tmp = *cn;
			if (na_parse_set(&r, &s)) { na_send_err(fd, m, "org.freedesktop.DBus.Error.InvalidArgs", "cannot parse the connection settings"); return 1; }
			if (cn->src == SRC_VPN || cn->src == SRC_WIFI) { na_send_err(fd, m, NMI ".Settings.Connection.NotSupported", "this connection lives in the VPN directory / the Wi-Fi backend and is not edited here"); return 1; }
			if (na_apply_set(&tmp, &s, err, sizeof err)) { na_send_err(fd, m, NMI ".Settings.InvalidSettings", err); return 1; }
			if (cn->src == SRC_PROFILE) { snprintf(tmp.name, sizeof tmp.name, "%s", cn->name); if (na_conn_save_profile(&tmp, err, sizeof err)) { na_send_err(fd, m, NMI ".Settings.InvalidSettings", err); return 1; } }
			tmp.id = cn->id; tmp.live = 1; tmp.seen = 1; tmp.src = cn->src; tmp.unsaved = cn->unsaved; snprintf(tmp.key, sizeof tmp.key, "%s", cn->key);
			*cn = tmp;
			na_set_settings_ver();
			if (!strcmp(mem, "Update2")) { na_buf b = {0}; na_empty_arr(&b, 8); na_reply(fd, m, "a{sv}", &b); free(b.b); } else na_reply_empty(fd, m);
			na_queue(4, m->path, 0, 0, 0);
			na_refresh(0);
			na_flush(fd);
			return 1;
		}
		if (!strcmp(mem, "Delete")) {
			int rc = 0;
			if (cn->src == SRC_PROFILE) { char p[700]; snprintf(p, sizeof p, "%s/%s.conf", netman_conf_dir, cn->name); rc = unlink(p); }
			else if (cn->src == SRC_WIFI) { FILE *se = netman_err, *so = netman_out; netman_err = netman_out = na_null(); rc = netman_wifi_forget(cn->ssid) == 0 ? 0 : -1; netman_err = se; netman_out = so; }
			else if (cn->src == SRC_VPN) { na_send_err(fd, m, NMI ".Settings.Connection.NotSupported", "VPN profile files are removed with `netman vpn`, not over D-Bus"); return 1; }
			if (rc) { na_send_err(fd, m, NMI ".Settings.Connection.PermissionDenied", "could not delete the connection"); return 1; }
			for (int j = 0; j < NA_ACT; j++) if (na_acts[j].live && na_acts[j].conn == slot) na_acts[j].live = 0;
			cn->live = 0;
			na_set_settings_ver();
			na_reply_empty(fd, m);
			na_refresh(1);
			na_flush(fd);
			return 1;
		}
	}
	if (kind == K_DEV && !strcmp(iface, NMI ".Device")) {
		if (!strcmp(mem, "Disconnect")) {
			na_act *a = na_act_of_dev(&na_devs[slot]);
			if (!a) { na_send_err(fd, m, NMI ".Device.NotActive", "the device is not active"); return 1; }
			na_reply_empty(fd, m);
			na_deactivate(fd, (int)(a - na_acts));
			return 1;
		}
		if (!strcmp(mem, "Delete") || !strcmp(mem, "SetManaged") || !strcmp(mem, "Reapply") || !strcmp(mem, "GetAppliedConnection")) { na_send_err(fd, m, NMI ".NotSupported", "this NetworkManager method is not implemented by netman"); return 1; }
	}
	if (kind == K_DEV && !strcmp(iface, NMI ".Device.Wireless")) {
		if (!strcmp(mem, "GetAccessPoints") || !strcmp(mem, "GetAllAccessPoints")) {
			na_ctx c; na_buf b = {0};
			memset(&c, 0, sizeof c);
			c.real = &b; c.only = "AccessPoints"; c.bare = 1;
			na_props(&c, K_DEV, slot, NMI ".Device.Wireless");
			{ size_t off = 1 + 2 + 1, pad = (4 - off % 4) % 4; na_buf rb = {0}; netman_dbus_put(&rb, b.b + off + pad, b.n - off - pad); na_reply(fd, m, "ao", &rb); free(rb.b); }
			free(b.b); free(c.trash.b);
			return 1;
		}
		if (!strcmp(mem, "RequestScan")) {
			if (!na_wifi_en) { na_send_err(fd, m, NMI ".Device.NotAllowed", "Wi-Fi is disabled"); return 1; }
			na_reply_empty(fd, m);
			na_refresh(2);
			na_flush(fd);
			return 1;
		}
	}
	na_send_err(fd, m, "org.freedesktop.DBus.Error.UnknownMethod", "no such method on this interface");
	return 1;
}

int nmapi_handle(int fd, const netman_dbus_Msg *m0)
{
	netman_dbus_Msg m = *m0;
	unsigned char *copy;
	int rc;
	if (m.type != 1) return 0;
	/* the Wi-Fi backends read their own D-Bus replies through the same static message buffer: keep this call's arguments out of it */
	copy = malloc((size_t)m.blen + 8);
	if (!copy) return 0;
	memcpy(copy, m0->body, m.blen);
	m.body = copy;
	rc = na_dispatch(fd, &m);
	free(copy);
	return rc;
}
