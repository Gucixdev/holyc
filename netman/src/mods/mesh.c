/* mesh.c - see mesh.h. */
#include "modkit.h"
#include "net.h"
#include "vpn.h"
#include "wireguard.h"
#include "mesh.h"
#include <arpa/inet.h>
#include <linux/if_link.h>
#include <net/if.h>

const char *netman_mesh_dir = "/etc/netman/mesh";
const char *netman_mesh_ip = "ip";
const char *netman_mesh_batctl = "batctl";

static int addr_ok(const char *a) { unsigned char b[16]; return a[0] && (inet_pton(AF_INET, a, b) == 1 || inet_pton(AF_INET6, a, b) == 1); }
static int is_v6(const char *a) { return strchr(a, ':') != NULL; }
/* "10.0.0.1/24" or "fd00::1/64" */
static int cidr_ok(const char *c)
{
	char a[64], *s;
	int p;
	mk_cpy(a, sizeof a, c);
	if (!(s = strchr(a, '/'))) return 0;
	*s++ = 0;
	return addr_ok(a) && !mk_int(s, 0, is_v6(a) ? 128 : 32, &p);
}
static int ifaces_ok(const char *l)
{
	char b[96], *sv = NULL, *t;
	int n = 0;
	mk_cpy(b, sizeof b, l);
	for (t = strtok_r(b, ",", &sv); t; t = strtok_r(NULL, ",", &sv), n++)
		if (!mk_name_ok(t) || strlen(t) > 15) return 0;
	return n > 0;
}

static void mesh_init(netman_mesh *m) { memset(m, 0, sizeof *m); m->hop_penalty = -1; }

static int set_kv(netman_mesh *m, const char *k, const char *v, char *err, size_t en)
{
	int x;
#define BAD(...) do { snprintf(err, en, __VA_ARGS__); return -1; } while (0)
	if (!strcmp(k, "kind")) { if (strcmp(v, "batman") && strcmp(v, "wg")) BAD("kind must be batman or wg"); mk_cpy(m->kind, sizeof m->kind, v); }
	else if (!strcmp(k, "iface")) { if (!mk_name_ok(v) || strlen(v) > 15) BAD("bad iface '%s'", v); mk_cpy(m->iface, sizeof m->iface, v); }
	else if (!strcmp(k, "addr")) { if (!cidr_ok(v)) BAD("addr must be a CIDR, got '%s'", v); mk_cpy(m->addr, sizeof m->addr, v); }
	else if (!strcmp(k, "auto")) { if (!mk_bool_valid(v)) BAD("auto must be yes or no"); m->autostart = mk_bool(v); }
	else if (!strcmp(k, "ifaces")) { if (!ifaces_ok(v)) BAD("ifaces: comma list of interface names, got '%s'", v); mk_cpy(m->ifaces, sizeof m->ifaces, v); }
	else if (!strcmp(k, "orig_interval")) { if (mk_int(v, 0, 60000, &m->orig_interval)) BAD("orig_interval 0..60000 ms"); }
	else if (!strcmp(k, "hop_penalty")) { if (mk_int(v, 0, 255, &m->hop_penalty)) BAD("hop_penalty 0..255"); }
	else if (!strcmp(k, "gw_mode")) { if (strcmp(v, "off") && strcmp(v, "client") && strcmp(v, "server")) BAD("gw_mode off|client|server"); mk_cpy(m->gw_mode, sizeof m->gw_mode, v); }
	else if (!strcmp(k, "mtu")) { if (mk_int(v, 68, 65535, &m->mtu)) BAD("mtu 68..65535"); }
	else if (!strcmp(k, "self")) { if (!mk_name_ok(v)) BAD("bad node name '%s'", v); mk_cpy(m->self, sizeof m->self, v); }
	else if (!strcmp(k, "port")) { if (mk_int(v, 1, 65535, &x)) BAD("port 1..65535"); m->port = x; }
	else if (!strcmp(k, "prefix")) { if (mk_int(v, 1, 128, &x)) BAD("prefix 1..128"); m->prefix = x; }
	else if (!strcmp(k, "keepalive")) { if (mk_int(v, 0, 65535, &x)) BAD("keepalive 0..65535"); m->keepalive = x; }
	else if (!strcmp(k, "key")) { if (!mk_token_ok(v, sizeof m->key) && strcmp(v, "")) BAD("bad key path"); mk_cpy(m->key, sizeof m->key, v); }
	else BAD("unknown key '%s'", k);
#undef BAD
	return 0;
}

int netman_mesh_parse(const char *text, netman_mesh *m, char *err, size_t en)
{
	char *c = strdup(text), *sv = NULL, *ln;
	mesh_init(m);
	if (!c) return -1;
	for (ln = strtok_r(c, "\n", &sv); ln; ln = strtok_r(NULL, "\n", &sv)) {
		char *p = ent_trim(ln), *eq;
		if (!*p || *p == '#') continue;
		if (!(eq = strchr(p, '='))) { snprintf(err, en, "not key = value: '%s'", p); free(c); return -1; }
		*eq = 0;
		if (set_kv(m, ent_trim(p), ent_trim(eq + 1), err, en)) { free(c); return -1; }
	}
	free(c);
	return 0;
}

int netman_mesh_format(const netman_mesh *m, char *b, size_t n)
{
	int l = snprintf(b, n, "kind = %s\n", m->kind);
#define S(k, f) if (m->f[0]) l += snprintf(b + l, l < (int)n ? n - l : 0, k " = %s\n", m->f)
#define N(k, f, z) if (m->f != z) l += snprintf(b + l, l < (int)n ? n - l : 0, k " = %d\n", m->f)
	S("iface", iface); S("addr", addr); S("ifaces", ifaces); S("gw_mode", gw_mode); S("self", self); S("key", key);
	N("orig_interval", orig_interval, 0); N("hop_penalty", hop_penalty, -1); N("mtu", mtu, 0); N("port", port, 0); N("prefix", prefix, 0); N("keepalive", keepalive, 0);
	l += snprintf(b + l, l < (int)n ? n - l : 0, "auto = %s\n", m->autostart ? "yes" : "no");
#undef S
#undef N
	return l;
}

int netman_mesh_check(netman_mesh *m, char *err, size_t en)
{
	if (!m->kind[0]) { snprintf(err, en, "kind is required (batman or wg)"); return -1; }
	if (!m->iface[0]) mk_cpy(m->iface, sizeof m->iface, !strcmp(m->kind, "wg") ? "wgmesh0" : "bat0");
	if (!strcmp(m->kind, "batman")) {
		if (!m->ifaces[0]) { snprintf(err, en, "batman needs ifaces = eth1,wlan1 (the ports to join the mesh)"); return -1; }
		if (m->self[0] || m->port || m->prefix || m->keepalive || m->key[0]) { snprintf(err, en, "self/port/prefix/keepalive/key are WireGuard-mesh keys"); return -1; }
	} else {
		if (!m->self[0]) { snprintf(err, en, "wg mesh needs self = <this node's name in the peer list>"); return -1; }
		if (m->ifaces[0] || m->gw_mode[0] || m->orig_interval || m->hop_penalty >= 0) { snprintf(err, en, "ifaces/gw_mode/orig_interval/hop_penalty are batman keys"); return -1; }
		if (!m->port) m->port = 51820;
		if (m->keepalive == 0) m->keepalive = 25;
	}
	return 0;
}

/* ---- peer list ---- */
int netman_mesh_nodes_parse(const char *text, netman_mesh_node *n, int max, char *err, size_t en)
{
	char *c = strdup(text), *sv = NULL, *ln;
	int cnt = 0;
	if (!c) return -1;
	for (ln = strtok_r(c, "\n", &sv); ln; ln = strtok_r(NULL, "\n", &sv)) {
		char *p = ent_trim(ln), nm[32], pub[48], ep[80], ad[46], extra[8];
		unsigned char k[32];
		if (!*p || *p == '#') continue;
		if (sscanf(p, "%31s %47s %79s %45s %7s", nm, pub, ep, ad, extra) != 4) { snprintf(err, en, "peer line needs: name public-key endpoint|- address (got '%.60s')", p); free(c); return -1; }
		if (cnt >= max) { snprintf(err, en, "more than %d nodes", max); free(c); return -1; }
		if (!mk_name_ok(nm) || strlen(nm) > 31) { snprintf(err, en, "bad node name '%s'", nm); free(c); return -1; }
		if (netman_vpn_b64_decode32(pub, k)) { snprintf(err, en, "node %s: public key is not a 32-byte base64 key", nm); free(c); return -1; }
		if (strcmp(ep, "-") && (!mk_token_ok(ep, 80) || !strchr(ep, ':'))) { snprintf(err, en, "node %s: endpoint must be host:port or -", nm); free(c); return -1; }
		if (!addr_ok(ad)) { snprintf(err, en, "node %s: bad mesh address '%s'", nm, ad); free(c); return -1; }
		for (int i = 0; i < cnt; i++)
			if (!strcmp(n[i].name, nm) || !strcmp(n[i].addr, ad) || !strcmp(n[i].pub, pub)) { snprintf(err, en, "node %s duplicates %s (name, key or address)", nm, n[i].name); free(c); return -1; }
		mk_cpy(n[cnt].name, sizeof n[cnt].name, nm); mk_cpy(n[cnt].pub, sizeof n[cnt].pub, pub);
		mk_cpy(n[cnt].ep, sizeof n[cnt].ep, ep); mk_cpy(n[cnt].addr, sizeof n[cnt].addr, ad);
		cnt++;
	}
	free(c);
	return cnt;
}

static int find_node(const netman_mesh_node *n, int cnt, const char *name) { for (int i = 0; i < cnt; i++) if (!strcmp(n[i].name, name)) return i; return -1; }

int netman_mesh_gen(const netman_mesh *m, const netman_mesh_node *n, int cnt, const char *node, const char *priv, char *b, size_t len)
{
	int me = find_node(n, cnt, node), l = 0;
	int pre = m->prefix ? m->prefix : (me >= 0 && is_v6(n[me].addr) ? 64 : 24);
#define P(...) l += snprintf(b + l, l < (int)len ? len - l : 0, __VA_ARGS__)
	if (me < 0) return -ENOENT;
	P("# netman mesh %s: node %s (generated; the same peer list gives every node its own file)\n[Interface]\n", m->name, node);
	P("Address = %s/%d\nListenPort = %d\nPrivateKey = %s\n", n[me].addr, pre, m->port ? m->port : 51820, priv ? priv : "<private key of this node>");
	for (int i = 0; i < cnt; i++) {
		if (i == me) continue;
		P("\n# %s\n[Peer]\nPublicKey = %s\nAllowedIPs = %s/%d\n", n[i].name, n[i].pub, n[i].addr, is_v6(n[i].addr) ? 128 : 32);
		if (strcmp(n[i].ep, "-")) P("Endpoint = %s\n", n[i].ep);
		P("PersistentKeepalive = %d\n", m->keepalive ? m->keepalive : 25);
	}
#undef P
	return l;
}

/* ---- files ---- */
static int load(const char *name, netman_mesh *m, char *err, size_t en)
{
	char p[400], txt[4096];
	if (!mk_name_ok(name)) { snprintf(err, en, "bad mesh name '%s'", name); return -1; }
	mk_path(p, sizeof p, netman_mesh_dir, name, "mesh");
	if (ent_slurp(p, txt, sizeof txt) < 0) { snprintf(err, en, "no mesh profile '%s' in %s (netman mesh save)", name, netman_mesh_dir); return -1; }
	if (netman_mesh_parse(txt, m, err, en) < 0) return -1;
	snprintf(m->name, sizeof m->name, "%s", name);
	return netman_mesh_check(m, err, en);
}
static int nodes_load(const char *name, netman_mesh_node *n, int max, char *err, size_t en)
{
	char p[400], txt[16384];
	mk_path(p, sizeof p, netman_mesh_dir, name, "peers");
	if (ent_slurp(p, txt, sizeof txt) < 0) { snprintf(err, en, "no peer list %s (netman mesh node %s add ...)", p, name); return -1; }
	return netman_mesh_nodes_parse(txt, n, max, err, en);
}
static void keypath(const netman_mesh *m, char *o, size_t n)
{
	if (m->key[0]) mk_cpy(o, n, m->key); else mk_path(o, n, netman_mesh_dir, m->name, "key");
}
static int key_load(const netman_mesh *m, unsigned char k[32], char *b64, size_t bn, char *err, size_t en)
{
	char p[400], t[128];
	keypath(m, p, sizeof p);
	if (ent_slurp(p, t, sizeof t) < 0) { snprintf(err, en, "no private key %s (netman mesh init %s)", p, m->name); return -1; }
	{ char *e = ent_trim(t); if (netman_vpn_b64_decode32(e, k)) { snprintf(err, en, "%s is not a 32-byte base64 key", p); return -1; } if (b64) snprintf(b64, bn, "%s", e); }
	return 0;
}

/* ---- up / down ---- */
static int ip_run(const char *a1, const char *a2, const char *a3, const char *a4, const char *a5, const char *a6, const char *a7, char *err, size_t en)
{
	char *a[10] = { (char *)netman_mesh_ip, (char *)a1, (char *)a2, (char *)a3, (char *)a4, (char *)a5, (char *)a6, (char *)a7, NULL, NULL };
	char o[300];
	int r = mk_run(a, o, sizeof o);
	if (r == -ENOENT) { snprintf(err, en, "%s not found (batman mode needs iproute2)", netman_mesh_ip); return r; }
	if (r < 0) { char *nl = strchr(o, '\n'); if (nl) *nl = 0; snprintf(err, en, "%s %s %s: %.200s", netman_mesh_ip, a1, a2, o); return r; }
	return 0;
}
static void batctl(const netman_mesh *m, const char *a1, const char *a2, const char *a3)
{
	char *a[8] = { (char *)netman_mesh_batctl, "meshif", (char *)m->iface, (char *)a1, (char *)a2, (char *)a3, NULL, NULL };
	if (mk_have(netman_mesh_batctl)) mk_run(a, NULL, 0);
}

static int up_batman(const netman_mesh *m, char *err, size_t en)
{
	char b[96], *sv = NULL, *t, num[16];
	int r;
	if (ent_idx(m->iface) >= 0) return -EEXIST;
	if ((r = ip_run("link", "add", "name", m->iface, "type", "batadv", NULL, err, en)) < 0) return r;
	mk_cpy(b, sizeof b, m->ifaces);
	for (t = strtok_r(b, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
		if (mk_have(netman_mesh_batctl)) { char *a[8] = { (char *)netman_mesh_batctl, "meshif", (char *)m->iface, "interface", "add", t, NULL, NULL }; char o[200]; if (mk_run(a, o, sizeof o) == 0) { ip_run("link", "set", "dev", t, "up", NULL, NULL, err, en); continue; } }
		if ((r = ip_run("link", "set", "dev", t, "master", m->iface, NULL, err, en)) < 0) return r;
		ip_run("link", "set", "dev", t, "up", NULL, NULL, err, en);
	}
	err[0] = 0;
	if (m->orig_interval) { snprintf(num, sizeof num, "%d", m->orig_interval); batctl(m, "orig_interval", num, NULL); }
	if (m->hop_penalty >= 0) { snprintf(num, sizeof num, "%d", m->hop_penalty); batctl(m, "hop_penalty", num, NULL); }
	if (m->gw_mode[0]) batctl(m, "gw_mode", m->gw_mode, NULL);
	if (m->mtu) { snprintf(num, sizeof num, "%d", m->mtu); ip_run("link", "set", "dev", m->iface, "mtu", num, NULL, err, en); err[0] = 0; }
	if (m->addr[0] && (r = netman_addr_add(m->iface, m->addr)) < 0) { snprintf(err, en, "address %s: %s", m->addr, strerror(-r)); return r; }
	if ((r = netman_link_set(m->iface, 1)) < 0) snprintf(err, en, "cannot bring %s up: %s", m->iface, strerror(-r));
	return r;
}

static int up_wg(const netman_mesh *m, char *err, size_t en)
{
	netman_mesh_node n[128];
	netman_wg_peer peers[128];
	netman_wg_ip allow[128];
	netman_wg_conf c;
	int cnt = nodes_load(m->name, n, 128, err, en), me, np = 0, r, pre;
	char a[100];
	if (cnt < 0) return -EINVAL;
	if ((me = find_node(n, cnt, m->self)) < 0) { snprintf(err, en, "self '%s' is not in %s.peers", m->self, m->name); return -ENOENT; }
	memset(&c, 0, sizeof c);
	if (key_load(m, c.priv, NULL, 0, err, en) < 0) return -ENOENT;
	c.has_priv = 1; c.listen_port = m->port;
	for (int i = 0; i < cnt; i++) {
		netman_wg_peer *p = &peers[np];
		if (i == me) continue;
		memset(p, 0, sizeof *p);
		netman_vpn_b64_decode32(n[i].pub, p->pub);
		if (strcmp(n[i].ep, "-")) { if (netman_wg_resolve(n[i].ep, &p->ep) < 0) { snprintf(err, en, "node %s: cannot resolve %s", n[i].name, n[i].ep); return -EHOSTUNREACH; } p->has_ep = 1; }
		p->keepalive = m->keepalive;
		memset(&allow[np], 0, sizeof allow[np]);
		allow[np].family = is_v6(n[i].addr) ? AF_INET6 : AF_INET;
		inet_pton(allow[np].family, n[i].addr, allow[np].addr);
		allow[np].prefix = allow[np].family == AF_INET6 ? 128 : 32;
		p->allowed = &allow[np]; p->nallowed = 1;
		np++;
	}
	c.peers = peers; c.npeers = (size_t)np;
	r = netman_wg_link_add(m->iface);
	if (r < 0 && r != -EEXIST) { snprintf(err, en, "cannot create %s: %s (wireguard module?)", m->iface, strerror(-r)); return r; }
	if ((r = netman_wg_set(m->iface, &c)) < 0) { snprintf(err, en, "cannot configure %s: %s", m->iface, strerror(-r)); return r; }
	memset(c.priv, 0, sizeof c.priv);
	pre = m->prefix ? m->prefix : (is_v6(n[me].addr) ? 64 : 24);
	snprintf(a, sizeof a, "%s/%d", n[me].addr, pre);
	if ((r = netman_addr_add(m->iface, a)) < 0) { snprintf(err, en, "address %s: %s", a, strerror(-r)); return r; }
	if ((r = netman_link_set(m->iface, 1)) < 0) snprintf(err, en, "cannot bring %s up: %s", m->iface, strerror(-r));
	return r;
}

int netman_mesh_up(const netman_mesh *m, char *err, size_t en)
{
	err[0] = 0;
	return !strcmp(m->kind, "wg") ? up_wg(m, err, en) : up_batman(m, err, en);
}

int netman_mesh_down(const netman_mesh *m, char *err, size_t en)
{
	ent_req q;
	int r;
	err[0] = 0;
	ent_req_init(&q, RTM_DELLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	ent_attr(&q, IFLA_IFNAME, m->iface, (int)strlen(m->iface) + 1);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) snprintf(err, en, "%s: %s", m->iface, r == -ENODEV ? "not up" : strerror(-r));
	return r;
}

/* ---- status ---- */
int netman_mesh_status(FILE *out)
{
	char names[64][32], e[200];
	int n = mk_names(netman_mesh_dir, "mesh", names, 64);
	for (int i = 0; i < n; i++) {
		netman_mesh m;
		int up;
		if (load(names[i], &m, e, sizeof e) < 0) { fprintf(out, "%s down ? %s\n", names[i], e); continue; }
		up = ent_idx(m.iface) >= 0;
		if (!strcmp(m.kind, "wg")) {
			netman_wg_stat st;
			netman_mesh_node nd[128];
			int c = nodes_load(m.name, nd, 128, e, sizeof e);
			if (up && netman_wg_stats(m.iface, &st) == 0 && st.present) fprintf(out, "%s up wg %s  peers %d/%d  rx %lld tx %lld\n", m.name, m.iface, st.handshaked, st.npeers, st.rx, st.tx);
			else fprintf(out, "%s %s wg %s  peers 0/%d\n", m.name, up ? "up" : "down", m.iface, c > 0 ? c - 1 : 0);
		} else {
			int k = 0;
			char b[96], *sv = NULL, *t;
			mk_cpy(b, sizeof b, m.ifaces);
			for (t = strtok_r(b, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) k++;
			fprintf(out, "%s %s batman %s  ports %d\n", m.name, up ? "up" : "down", m.iface, k);
		}
	}
	return n;
}

/* ---- CLI ---- */
static int save_file(const char *name, const char *ext, const char *body, size_t len, mode_t mode, char *err, size_t en)
{
	char p[400];
	int r = mk_mkdirs(netman_mesh_dir);
	if (r < 0) { snprintf(err, en, "%s: %s", netman_mesh_dir, strerror(-r)); return r; }
	mk_path(p, sizeof p, netman_mesh_dir, name, ext);
	if ((r = ent_write_atomic(p, body, len, mode)) < 0) snprintf(err, en, "%.200s: %s", p, strerror(-r));
	return r;
}

static int node_cmd(FILE *out, FILE *err, int argc, char **argv)
{
	/* node NAME add NODE PUB EP ADDR | node NAME del NODE | node NAME list */
	netman_mesh_node n[128];
	char e[300], txt[16384], p[400];
	netman_mesh m;
	int cnt, l = 0;
	if (argc < 3 || !mk_name_ok(argv[1])) { fputs("usage: netman mesh node NAME add NODE PUBKEY ENDPOINT|- ADDRESS | del NODE | list\n", err); return 2; }
	if (load(argv[1], &m, e, sizeof e) < 0) { fprintf(err, "mesh: %s\n", e); return 1; }
	if (strcmp(m.kind, "wg")) { fputs("mesh: peer lists are for kind = wg\n", err); return 2; }
	mk_path(p, sizeof p, netman_mesh_dir, argv[1], "peers");
	if (ent_slurp(p, txt, sizeof txt) < 0) txt[0] = 0;
	if ((cnt = netman_mesh_nodes_parse(txt, n, 128, e, sizeof e)) < 0) { fprintf(err, "mesh: %s\n", e); return 1; }
	if (!strcmp(argv[2], "list")) { for (int i = 0; i < cnt; i++) fprintf(out, "%s %s %s %s%s\n", n[i].name, n[i].pub, n[i].ep, n[i].addr, !strcmp(n[i].name, m.self) ? "  (this node)" : ""); return 0; }
	if (!strcmp(argv[2], "add") && argc == 7) {
		char line[400];
		snprintf(line, sizeof line, "%s %s %s %s\n", argv[3], argv[4], argv[5], argv[6]);
		if (find_node(n, cnt, argv[3]) >= 0) { fprintf(err, "mesh: node %s exists (del it first)\n", argv[3]); return 1; }
		if (strlen(txt) + strlen(line) + 1 >= sizeof txt) { fputs("mesh: peer list too large\n", err); return 1; }
		strcat(txt, line);
		if (netman_mesh_nodes_parse(txt, n, 128, e, sizeof e) < 0) { fprintf(err, "mesh: %s\n", e); return 2; }
	} else if (!strcmp(argv[2], "del") && argc == 4) {
		int k = find_node(n, cnt, argv[3]);
		if (k < 0) { fprintf(err, "mesh: no node %s\n", argv[3]); return 1; }
		for (int i = 0; i < cnt; i++) if (i != k) l += snprintf(txt + l, sizeof txt - l, "%s %s %s %s\n", n[i].name, n[i].pub, n[i].ep, n[i].addr);
		txt[l] = 0;
	} else { fputs("usage: netman mesh node NAME add NODE PUBKEY ENDPOINT|- ADDRESS | del NODE | list\n", err); return 2; }
	if (save_file(argv[1], "peers", txt, strlen(txt), 0644, e, sizeof e) < 0) { fprintf(err, "mesh: %s\n", e); return 1; }
	fprintf(out, "%s: %s %s\n", argv[1], argv[2] , argv[3]);
	return 0;
}

int netman_mesh_main(FILE *out, FILE *err, int argc, char **argv)
{
	const char *c = argc > 0 ? argv[0] : "status";
	char e[300];
	netman_mesh m;
	int r;
	if (!strcmp(c, "status") || !strcmp(c, "list")) {
		if (netman_mesh_status(out) == 0) fputs("no mesh profiles (netman mesh save NAME kind=batman ifaces=eth1 | kind=wg self=NODE)\n", out);
		return 0;
	}
	if (!strcmp(c, "save") && argc >= 3) {
		char body[2048], p[400], txt[4096];
		mesh_init(&m);
		if (!mk_name_ok(argv[1])) { fprintf(err, "mesh: bad name '%s'\n", argv[1]); return 2; }
		mk_path(p, sizeof p, netman_mesh_dir, argv[1], "mesh");
		if (ent_slurp(p, txt, sizeof txt) > 0) netman_mesh_parse(txt, &m, e, sizeof e);
		for (int i = 2; i < argc; i++) {
			char *eq = strchr(argv[i], '='), k[24];
			if (!eq || eq == argv[i] || eq - argv[i] >= 24) { fprintf(err, "mesh: expected key=value, got '%s'\n", argv[i]); return 2; }
			snprintf(k, sizeof k, "%.*s", (int)(eq - argv[i]), argv[i]);
			if (set_kv(&m, k, eq + 1, e, sizeof e)) { fprintf(err, "mesh: %s\n", e); return 2; }
		}
		snprintf(m.name, sizeof m.name, "%s", argv[1]);
		if (netman_mesh_check(&m, e, sizeof e) < 0) { fprintf(err, "mesh: %s\n", e); return 2; }
		r = snprintf(body, sizeof body, "# netman mesh\n");
		r += netman_mesh_format(&m, body + r, sizeof body - r);
		if (save_file(argv[1], "mesh", body, (size_t)r, 0644, e, sizeof e) < 0) { fprintf(err, "mesh: %s\n", e); return 1; }
		fprintf(out, "saved %s (%s, %s)\n", m.name, m.kind, m.iface);
		return 0;
	}
	if (!strcmp(c, "node")) return node_cmd(out, err, argc, argv);
	if (!strcmp(c, "show") && argc == 2) {
		char body[2048];
		if (load(argv[1], &m, e, sizeof e) < 0) { fprintf(err, "mesh: %s\n", e); return 1; }
		netman_mesh_format(&m, body, sizeof body);
		fputs(body, out);
		return 0;
	}
	if (!strcmp(c, "init") && argc == 2) {
		unsigned char k[32], pub[32];
		char p[400], b[48], pb[48];
		if (load(argv[1], &m, e, sizeof e) < 0) { fprintf(err, "mesh: %s\n", e); return 1; }
		if (strcmp(m.kind, "wg")) { fputs("mesh: init is for kind = wg\n", err); return 2; }
		keypath(&m, p, sizeof p);
		if (mk_exists(p)) { fprintf(err, "mesh: %s exists; refusing to overwrite a private key\n", p); return 1; }
		if (netman_vpn_genkey(k) < 0) { fputs("mesh: cannot generate a key\n", err); return 1; }
		netman_vpn_b64_encode32(k, b); b[44] = 0;
		netman_vpn_pubkey(k, pub); netman_vpn_b64_encode32(pub, pb); pb[44] = 0;
		strcat(b, "\n");
		if (mk_mkdirs(netman_mesh_dir) < 0 || ent_write_atomic(p, b, strlen(b), 0600) < 0) { fprintf(err, "mesh: cannot write %s\n", p); return 1; }
		memset(k, 0, sizeof k); memset(b, 0, sizeof b);
		fprintf(out, "wrote %s (mode 0600)\npublic key: %s\n", p, pb);
		return 0;
	}
	if (!strcmp(c, "gen") && (argc == 2 || argc == 3)) {
		netman_mesh_node n[128];
		char *buf = malloc(65536), priv[64];
		int cnt, l;
		const char *node;
		unsigned char k[32];
		if (!buf) return 1;
		if (load(argv[1], &m, e, sizeof e) < 0 || strcmp(m.kind, "wg") ? (fprintf(err, "mesh: %s\n", e[0] ? e : "gen is for kind = wg"), 1) : 0) { free(buf); return 1; }
		if ((cnt = nodes_load(m.name, n, 128, e, sizeof e)) < 0) { fprintf(err, "mesh: %s\n", e); free(buf); return 1; }
		node = argc == 3 ? argv[2] : m.self;
		/* the private key is only ever filled in for THIS node, and only from its own key file */
		if (!strcmp(node, m.self) && key_load(&m, k, priv, sizeof priv, e, sizeof e) == 0) l = netman_mesh_gen(&m, n, cnt, node, priv, buf, 65536);
		else l = netman_mesh_gen(&m, n, cnt, node, NULL, buf, 65536);
		memset(priv, 0, sizeof priv); memset(k, 0, sizeof k);
		if (l < 0) { fprintf(err, "mesh: no node %s in the peer list\n", node); free(buf); return 1; }
		fwrite(buf, 1, (size_t)l, out);
		free(buf);
		return 0;
	}
	if ((!strcmp(c, "up") || !strcmp(c, "down")) && argc == 2) {
		int up = !strcmp(c, "up");
		if (load(argv[1], &m, e, sizeof e) < 0) { fprintf(err, "mesh: %s\n", e); return 1; }
		r = up ? netman_mesh_up(&m, e, sizeof e) : netman_mesh_down(&m, e, sizeof e);
		if (r == -EEXIST) { fprintf(out, "%s is already up\n", m.name); return 0; }
		if (r < 0) { fprintf(err, "mesh: %s\n", e[0] ? e : strerror(-r)); return 1; }
		fprintf(out, "%s %s\n", m.name, up ? "up" : "down");
		return 0;
	}
	if (!strcmp(c, "apply") && argc == 1) {
		char names[64][32];
		int n = mk_names(netman_mesh_dir, "mesh", names, 64), bad = 0;
		for (int i = 0; i < n; i++) {
			if (load(names[i], &m, e, sizeof e) < 0) { fprintf(err, "mesh %s: %s\n", names[i], e); bad++; continue; }
			if (!m.autostart) continue;
			r = netman_mesh_up(&m, e, sizeof e);
			if (r < 0 && r != -EEXIST) { fprintf(err, "mesh %s: %s\n", m.name, e); bad++; } else fprintf(out, "%s %s\n", m.name, r == -EEXIST ? "already up" : "up");
		}
		return bad ? 1 : 0;
	}
	if (!strcmp(c, "neighbors") && argc == 2) {
		char o[4096];
		if (load(argv[1], &m, e, sizeof e) < 0) { fprintf(err, "mesh: %s\n", e); return 1; }
		if (strcmp(m.kind, "batman")) { fputs("mesh: neighbors is for kind = batman\n", err); return 2; }
		if (!mk_have(netman_mesh_batctl)) { fprintf(err, "mesh: %s not installed (it is only needed to list neighbors and to tune)\n", netman_mesh_batctl); return 1; }
		{ char *a[] = { (char *)netman_mesh_batctl, "meshif", m.iface, "neighbors", NULL }; r = mk_run(a, o, sizeof o); }
		fputs(o, out);
		return r < 0 ? 1 : 0;
	}
	if (!strcmp(c, "del") && argc == 2) {
		char p[400];
		if (!mk_name_ok(argv[1])) { fprintf(err, "mesh: bad name\n"); return 2; }
		mk_path(p, sizeof p, netman_mesh_dir, argv[1], "mesh");
		if (unlink(p) < 0) { fprintf(err, "mesh: no profile %s\n", argv[1]); return 1; }
		fprintf(out, "deleted %s (its .peers and .key files are kept)\n", argv[1]);
		return 0;
	}
	fputs("usage: netman mesh [status] | save NAME k=v... | show NAME | up|down NAME | apply | del NAME\n"
	      "       netman mesh init NAME | node NAME add|del|list ... | gen NAME [NODE] | neighbors NAME\n", err);
	return 2;
}
