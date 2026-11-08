/* tunnel.c - see tunnel.h. */
#include "modkit.h"
#include "net.h"
#include "tunnel.h"
#include "enterprise/enterprise.h"
#include <arpa/inet.h>
#include <linux/if_link.h>
#include <linux/if_tunnel.h>
#include <net/if.h>

const char *netman_tunnel_dir = "/etc/netman/tunnel";
const char *netman_tunnel_ip = "ip";

static const char *const kinds[] = { "gre", "gretap", "ipip", "sit", "vxlan", "geneve", "l2tp", NULL };
static int is_v6(const char *a) { return a && strchr(a, ':') != NULL; }
static int addr_ok(const char *a) { unsigned char b[16]; return a[0] && (inet_pton(AF_INET, a, b) == 1 || inet_pton(AF_INET6, a, b) == 1); }

int netman_tunnel_overhead(const char *k, int v6, int key)
{
	int o = v6 ? 40 : 20;
	if (!strcmp(k, "gre")) return o + 4 + (key ? 4 : 0);
	if (!strcmp(k, "gretap")) return o + 4 + (key ? 4 : 0) + 14;
	if (!strcmp(k, "ipip") || !strcmp(k, "sit")) return o;
	if (!strcmp(k, "vxlan") || !strcmp(k, "geneve")) return o + 8 + 8 + 14;
	if (!strcmp(k, "l2tp")) return o + 4 + 14;   /* ip encap; udp adds 8 (tunnel_overhead cannot see encap: see netman_tunnel_mtu) */
	return o;
}

static void tun_init(netman_tun *t) { memset(t, 0, sizeof *t); t->key = -1; t->ttl = -1; }

static int kv_cb(const char *k, const char *v, void *u)
{
	netman_tun *t = u;
	int x;
	if (!strcmp(k, "kind")) mk_cpy(t->kind, sizeof t->kind, v);
	else if (!strcmp(k, "local")) mk_cpy(t->local, sizeof t->local, v);
	else if (!strcmp(k, "remote")) mk_cpy(t->remote, sizeof t->remote, v);
	else if (!strcmp(k, "link")) mk_cpy(t->link, sizeof t->link, v);
	else if (!strcmp(k, "group")) mk_cpy(t->group, sizeof t->group, v);
	else if (!strcmp(k, "addr")) mk_cpy(t->addr, sizeof t->addr, v);
	else if (!strcmp(k, "encap")) mk_cpy(t->encap, sizeof t->encap, v);
	else if (!strcmp(k, "key")) { char *e; t->key = strtol(v, &e, 0); if (*e || t->key < 0 || t->key > 0xffffffffL) return -EINVAL; }
	else if (!strcmp(k, "ttl")) { if (mk_int(v, 0, 255, &x)) return -EINVAL; t->ttl = x; }
	else if (!strcmp(k, "mtu")) { if (!strcmp(v, "auto")) t->mtu = 0; else if (mk_int(v, 68, 65535, &t->mtu)) return -EINVAL; }
	else if (!strcmp(k, "auto")) { if (!mk_bool_valid(v)) return -EINVAL; t->autostart = mk_bool(v); }
	else if (!strcmp(k, "vni")) { if (mk_int(v, 0, 0xffffff, &t->vni)) return -EINVAL; }
	else if (!strcmp(k, "port")) { if (mk_int(v, 1, 65535, &t->port)) return -EINVAL; }
	else if (!strcmp(k, "lport")) { if (mk_int(v, 1, 65535, &t->lport)) return -EINVAL; }
	else if (!strcmp(k, "rport")) { if (mk_int(v, 1, 65535, &t->rport)) return -EINVAL; }
	else if (!strcmp(k, "tunnel_id")) t->tid = atol(v);
	else if (!strcmp(k, "peer_tunnel_id")) t->ptid = atol(v);
	else if (!strcmp(k, "session_id")) t->sid = atol(v);
	else if (!strcmp(k, "peer_session_id")) t->psid = atol(v);
	else return -ENOENT;
	return 0;
}

static int set_kv(netman_tun *t, const char *k, const char *v, char *err, size_t en)
{
	int r = kv_cb(k, v, t);
	if (r == -ENOENT) snprintf(err, en, "unknown key '%s'", k);
	else if (r) snprintf(err, en, "bad value for %s: '%s'", k, v);
	return r ? -1 : 0;
}

int netman_tunnel_parse(const char *text, netman_tun *t, char *err, size_t en)
{
	char *c = strdup(text), *sv = NULL, *ln;
	tun_init(t);
	if (!c) return -1;
	for (ln = strtok_r(c, "\n", &sv); ln; ln = strtok_r(NULL, "\n", &sv)) {
		char *p = ent_trim(ln), *eq;
		if (!*p || *p == '#') continue;
		if (!(eq = strchr(p, '='))) { snprintf(err, en, "not key = value: '%s'", p); free(c); return -1; }
		*eq = 0;
		if (set_kv(t, ent_trim(p), ent_trim(eq + 1), err, en)) { free(c); return -1; }
	}
	free(c);
	return 0;
}

int netman_tunnel_format(const netman_tun *t, char *b, size_t n)
{
	int l = snprintf(b, n, "kind = %s\n", t->kind);
#define S(k, f) if (t->f[0]) l += snprintf(b + l, l < (int)n ? n - l : 0, k " = %s\n", t->f)
#define N(k, f, z) if (t->f != z) l += snprintf(b + l, l < (int)n ? n - l : 0, k " = %ld\n", (long)t->f)
	S("local", local); S("remote", remote); S("link", link); S("group", group); S("addr", addr); S("encap", encap);
	N("key", key, -1); N("ttl", ttl, -1); N("mtu", mtu, 0); N("vni", vni, 0); N("port", port, 0); N("lport", lport, 0); N("rport", rport, 0);
	N("tunnel_id", tid, 0); N("peer_tunnel_id", ptid, 0); N("session_id", sid, 0); N("peer_session_id", psid, 0);
	l += snprintf(b + l, l < (int)n ? n - l : 0, "auto = %s\n", t->autostart ? "yes" : "no");
	return l;
}

int netman_tunnel_check(netman_tun *t, char *err, size_t en)
{
	int k, v6;
#define BAD(...) do { snprintf(err, en, __VA_ARGS__); return -1; } while (0)
	for (k = 0; kinds[k]; k++) if (!strcmp(t->kind, kinds[k])) break;
	if (!kinds[k]) BAD("kind must be one of: gre gretap ipip sit vxlan geneve l2tp");
	if (!mk_name_ok(t->name) || strlen(t->name) > 15) BAD("bad tunnel name '%s'", t->name);
	if (t->local[0] && !addr_ok(t->local)) BAD("local '%s' is not an IP address", t->local);
	if (t->remote[0] && !addr_ok(t->remote)) BAD("remote '%s' is not an IP address", t->remote);
	if (t->group[0] && (!addr_ok(t->group) || strcmp(t->kind, "vxlan"))) BAD("group is a multicast address and only vxlan has one");
	if (t->link[0] && (!mk_name_ok(t->link) || strlen(t->link) > 15)) BAD("bad link '%s'", t->link);
	if (t->local[0] && t->remote[0] && is_v6(t->local) != is_v6(t->remote)) BAD("local and remote must be the same IP family");
	v6 = is_v6(t->local) || is_v6(t->remote);
	if ((!strcmp(t->kind, "ipip") || !strcmp(t->kind, "sit")) && v6) BAD("%s is IPv4-only here (IPv6 underlay: use gre/gretap/vxlan/geneve)", t->kind);
	if (strcmp(t->kind, "vxlan") && !t->remote[0] && strcmp(t->kind, "gre") && strcmp(t->kind, "ipip") && strcmp(t->kind, "gretap")) BAD("%s needs remote", t->kind);
	if (!strcmp(t->kind, "vxlan") && !t->remote[0] && !t->group[0]) BAD("vxlan needs remote (unicast) or group (multicast)");
	if (t->key >= 0 && strcmp(t->kind, "gre") && strcmp(t->kind, "gretap")) BAD("key is for gre/gretap only (use vni for vxlan/geneve)");
	if (!strcmp(t->kind, "l2tp")) {
		if (!t->local[0] || !t->remote[0]) BAD("l2tp needs local and remote");
		if (t->tid <= 0 || t->ptid <= 0 || t->sid <= 0 || t->psid <= 0) BAD("l2tp needs tunnel_id, peer_tunnel_id, session_id, peer_session_id (> 0)");
		if (!t->encap[0]) strcpy(t->encap, "ip");
		if (strcmp(t->encap, "ip") && strcmp(t->encap, "udp")) BAD("encap is ip or udp");
		if (!strcmp(t->encap, "udp") && (!t->lport || !t->rport)) BAD("l2tp over udp needs lport and rport");
	}
	if (t->addr[0]) {
		char a[64], *sl;
		snprintf(a, sizeof a, "%s", t->addr);
		if (!(sl = strchr(a, '/')) || (*sl++ = 0, !addr_ok(a)) || atoi(sl) <= 0) BAD("addr '%s' is not ADDRESS/PREFIX", t->addr);
	}
	if (!t->port && !strcmp(t->kind, "vxlan")) t->port = 4789;
	if (!t->port && !strcmp(t->kind, "geneve")) t->port = 6081;
	return 0;
}

int netman_tunnel_mtu(const netman_tun *t, int *underlay, int *overhead)
{
	char p[300], b[16];
	int u = 1500, o;
	if (t->link[0]) {
		snprintf(p, sizeof p, "%s/class/net/%s/mtu", netman_ent_sys, t->link);
		if (ent_slurp(p, b, sizeof b) > 0 && atoi(b) > 0) u = atoi(b);
	}
	o = netman_tunnel_overhead(t->kind, is_v6(t->local) || is_v6(t->remote), t->key >= 0);
	if (!strcmp(t->kind, "l2tp") && !strcmp(t->encap, "udp")) o += 8;
	if (underlay) *underlay = u;
	if (overhead) *overhead = o;
	return t->mtu ? t->mtu : u - o;
}

/* ---- rtnetlink ---- */
static int nest(ent_req *q, int type) { int off = NLMSG_ALIGN(q->h.nlmsg_len); ent_attr(q, type, NULL, 0); return off; }
static void nend(ent_req *q, int off) { ((struct rtattr *)((char *)q + off))->rta_len = q->h.nlmsg_len - off; }
static void a8(ent_req *q, int t, uint8_t v) { ent_attr(q, t, &v, 1); }
static void a16(ent_req *q, int t, uint16_t v) { ent_attr(q, t, &v, 2); }
static void a32(ent_req *q, int t, uint32_t v) { ent_attr(q, t, &v, 4); }
static void aip(ent_req *q, int t, const char *s) { unsigned char b[16]; if (inet_pton(is_v6(s) ? AF_INET6 : AF_INET, s, b) == 1) ent_attr(q, t, b, is_v6(s) ? 16 : 4); }

static int rt_create(const netman_tun *t, int mtu, char *err, size_t en)
{
	ent_req q;
	int li, di, link = 0, v6 = is_v6(t->local) || is_v6(t->remote), r;
	const char *kind = t->kind;
	char kb[16];
	if (t->link[0] && (link = ent_idx(t->link)) < 0) { snprintf(err, en, "link %s does not exist", t->link); return link; }
	if (v6 && (!strcmp(kind, "gre") || !strcmp(kind, "gretap"))) { snprintf(kb, sizeof kb, "ip6%s", kind); kind = kb; }
	ent_req_init(&q, RTM_NEWLINK, NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL, sizeof(struct ifinfomsg));
	ent_attr(&q, IFLA_IFNAME, t->name, (int)strlen(t->name) + 1);
	if (mtu > 0) a32(&q, IFLA_MTU, (uint32_t)mtu);
	li = nest(&q, IFLA_LINKINFO);
	ent_attr(&q, IFLA_INFO_KIND, kind, (int)strlen(kind) + 1);
	di = nest(&q, IFLA_INFO_DATA);
	if (!strncmp(kind, "gre", 3) || !strncmp(kind, "ip6gre", 6)) {
		if (link) a32(&q, IFLA_GRE_LINK, (uint32_t)link);
		if (t->key >= 0) {
			a16(&q, IFLA_GRE_IFLAGS, htons(GRE_KEY)); a16(&q, IFLA_GRE_OFLAGS, htons(GRE_KEY));
			a32(&q, IFLA_GRE_IKEY, htonl((uint32_t)t->key)); a32(&q, IFLA_GRE_OKEY, htonl((uint32_t)t->key));
		}
		if (t->local[0]) aip(&q, IFLA_GRE_LOCAL, t->local);
		if (t->remote[0]) aip(&q, IFLA_GRE_REMOTE, t->remote);
		if (t->ttl >= 0) a8(&q, IFLA_GRE_TTL, (uint8_t)t->ttl);
	} else if (!strcmp(kind, "ipip") || !strcmp(kind, "sit")) {
		if (link) a32(&q, IFLA_IPTUN_LINK, (uint32_t)link);
		if (t->local[0]) aip(&q, IFLA_IPTUN_LOCAL, t->local);
		if (t->remote[0]) aip(&q, IFLA_IPTUN_REMOTE, t->remote);
		if (t->ttl >= 0) a8(&q, IFLA_IPTUN_TTL, (uint8_t)t->ttl);
	} else if (!strcmp(kind, "vxlan")) {
		a32(&q, IFLA_VXLAN_ID, (uint32_t)t->vni);
		if (link) a32(&q, IFLA_VXLAN_LINK, (uint32_t)link);
		if (t->remote[0]) aip(&q, is_v6(t->remote) ? IFLA_VXLAN_GROUP6 : IFLA_VXLAN_GROUP, t->remote);
		if (t->group[0]) aip(&q, is_v6(t->group) ? IFLA_VXLAN_GROUP6 : IFLA_VXLAN_GROUP, t->group);
		if (t->local[0]) aip(&q, is_v6(t->local) ? IFLA_VXLAN_LOCAL6 : IFLA_VXLAN_LOCAL, t->local);
		if (t->ttl >= 0) a8(&q, IFLA_VXLAN_TTL, (uint8_t)t->ttl);
		a16(&q, IFLA_VXLAN_PORT, htons((uint16_t)t->port));
	} else if (!strcmp(kind, "geneve")) {
		a32(&q, IFLA_GENEVE_ID, (uint32_t)t->vni);
		aip(&q, is_v6(t->remote) ? IFLA_GENEVE_REMOTE6 : IFLA_GENEVE_REMOTE, t->remote);
		if (t->ttl >= 0) a8(&q, IFLA_GENEVE_TTL, (uint8_t)t->ttl);
		a16(&q, IFLA_GENEVE_PORT, htons((uint16_t)t->port));
	}
	nend(&q, di);
	nend(&q, li);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) snprintf(err, en, "%s %s: %s%s", t->kind, t->name, strerror(-r), r == -EOPNOTSUPP || r == -ENODEV || r == -EPERM ? " (is the kernel module loaded / are you root?)" : "");
	return r;
}

static int l2tp_run(const netman_tun *t, int add, char *err, size_t en)
{
	char tid[16], ptid[16], sid[16], psid[16], lp[8], rp[8];
	char *a[40]; int n = 0, r;
	snprintf(tid, sizeof tid, "%ld", t->tid); snprintf(ptid, sizeof ptid, "%ld", t->ptid); snprintf(sid, sizeof sid, "%ld", t->sid); snprintf(psid, sizeof psid, "%ld", t->psid);
	snprintf(lp, sizeof lp, "%d", t->lport); snprintf(rp, sizeof rp, "%d", t->rport);
	if (add) {
		char *tn[] = { (char *)netman_tunnel_ip, "l2tp", "add", "tunnel", "tunnel_id", tid, "peer_tunnel_id", ptid, "encap", (char *)t->encap, "local", (char *)t->local, "remote", (char *)t->remote, NULL, NULL, NULL, NULL, NULL };
		int k = 14;
		if (!strcmp(t->encap, "udp")) { tn[k++] = "udp_sport"; tn[k++] = lp; tn[k++] = "udp_dport"; tn[k++] = rp; }
		if ((r = mk_run(tn, err, en)) < 0) { if (r == -ENOENT) snprintf(err, en, "L2TPv3 needs the `ip` binary (tunnel.ip_bin) and the l2tp_eth / l2tp_ip modules"); return r; }
		a[n++] = (char *)netman_tunnel_ip; a[n++] = "l2tp"; a[n++] = "add"; a[n++] = "session"; a[n++] = "name"; a[n++] = (char *)t->name;
		a[n++] = "tunnel_id"; a[n++] = tid; a[n++] = "session_id"; a[n++] = sid; a[n++] = "peer_session_id"; a[n++] = psid; a[n] = NULL;
		if ((r = mk_run(a, err, en)) < 0) { char *d[] = { (char *)netman_tunnel_ip, "l2tp", "del", "tunnel", "tunnel_id", tid, NULL }; mk_run(d, NULL, 0); }
		return r;
	}
	{
		char *ds[] = { (char *)netman_tunnel_ip, "l2tp", "del", "session", "tunnel_id", tid, "session_id", sid, NULL };
		char *dt[] = { (char *)netman_tunnel_ip, "l2tp", "del", "tunnel", "tunnel_id", tid, NULL };
		mk_run(ds, NULL, 0);
		return mk_run(dt, err, en);
	}
}

int netman_tunnel_up(const netman_tun *tin, char *err, size_t en)
{
	netman_tun t = *tin;
	int r, mtu, u, o;
	err[0] = 0;
	if (netman_tunnel_check(&t, err, en) < 0) return -EINVAL;
	mtu = netman_tunnel_mtu(&t, &u, &o);
	if (mtu < 68) { snprintf(err, en, "computed MTU %d is too small (underlay %d, overhead %d)", mtu, u, o); return -EINVAL; }
	r = !strcmp(t.kind, "l2tp") ? l2tp_run(&t, 1, err, en) : rt_create(&t, mtu, err, en);
	if (r < 0) return r;
	if (!strcmp(t.kind, "l2tp") && ent_idx(t.name) > 0) {   /* l2tpeth devices take their MTU after creation */
		ent_req q;
		ent_req_init(&q, RTM_NEWLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
		q.m.i.ifi_index = ent_idx(t.name);
		a32(&q, IFLA_MTU, (uint32_t)mtu);
		ent_nl(&q, NULL, NULL);
	}
	if (t.addr[0] && (r = netman_addr_add(t.name, t.addr)) < 0) { snprintf(err, en, "address %s: %s", t.addr, strerror(-r)); return r; }
	if ((r = netman_link_set(t.name, 1)) < 0) snprintf(err, en, "cannot bring %s up: %s", t.name, strerror(-r));
	return r;
}

int netman_tunnel_down(const netman_tun *tin, char *err, size_t en)
{
	netman_tun t = *tin;
	ent_req q;
	int r;
	err[0] = 0;
	if (!strcmp(t.kind, "l2tp")) { if (netman_tunnel_check(&t, err, en) < 0) return -EINVAL; return l2tp_run(&t, 0, err, en); }
	ent_req_init(&q, RTM_DELLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	ent_attr(&q, IFLA_IFNAME, t.name, (int)strlen(t.name) + 1);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) snprintf(err, en, "%s: %s", t.name, r == -ENODEV ? "not up" : strerror(-r));
	return r;
}

/* ---- profiles ---- */
static int load(const char *name, netman_tun *t, char *err, size_t en)
{
	char p[400], txt[4096];
	if (!mk_name_ok(name)) { snprintf(err, en, "bad tunnel name '%s'", name); return -1; }
	mk_path(p, sizeof p, netman_tunnel_dir, name, "tun");
	if (ent_slurp(p, txt, sizeof txt) < 0) { snprintf(err, en, "no tunnel profile '%s' in %s (netman tunnel save)", name, netman_tunnel_dir); return -1; }
	if (netman_tunnel_parse(txt, t, err, en) < 0) return -1;
	snprintf(t->name, sizeof t->name, "%s", name);
	return netman_tunnel_check(t, err, en);
}

static long statn(const char *ifn, const char *f)
{
	char p[300], b[32];
	snprintf(p, sizeof p, "%s/class/net/%s/%s", netman_ent_sys, ifn, f);
	return ent_slurp(p, b, sizeof b) > 0 ? atol(b) : -1;
}
/* link state from rtnetlink (the right network namespace; sysfs may belong to another one) */
static int live_state(const char *ifn, char *st, size_t n, int *mtu)
{
	netman_ent_info i;
	if (netman_ent_getinfo(ifn, &i) < 0) { snprintf(st, n, "down"); return 0; }   /* profile only */
	snprintf(st, n, "%s", i.up ? "up" : "down");
	*mtu = i.mtu;
	return 1;
}

int netman_tunnel_status(FILE *out)
{
	char names[64][32], err[200];
	int n = mk_names(netman_tunnel_dir, "tun", names, 64);
	for (int i = 0; i < n; i++) {
		netman_tun t;
		char st[16];
		if (load(names[i], &t, err, sizeof err) < 0) { fprintf(out, "%s invalid  %s\n", names[i], err); continue; }
		int mtu = 0, present = live_state(t.name, st, sizeof st, &mtu);
		fprintf(out, "%s %s %s %s>%s", t.name, present ? st : "down", t.kind, t.local[0] ? t.local : "any", t.remote[0] ? t.remote : t.group[0] ? t.group : "any");
		if (present) { long rx = statn(t.name, "statistics/rx_bytes"), tx = statn(t.name, "statistics/tx_bytes"); fprintf(out, "  mtu %d", mtu); if (rx >= 0) fprintf(out, "  rx %ld tx %ld bytes", rx, tx); }
		else fprintf(out, "  (not up%s)", t.autostart ? ", auto" : "");
		fputc('\n', out);
	}
	return n;
}

static int profile_save(const netman_tun *t, char *err, size_t en)
{
	char p[400], body[2048];
	int l = snprintf(body, sizeof body, "# netman tunnel\n"), r;
	l += netman_tunnel_format(t, body + l, sizeof body - l);
	if ((r = mk_mkdirs(netman_tunnel_dir)) < 0) { snprintf(err, en, "%s: %s", netman_tunnel_dir, strerror(-r)); return r; }
	mk_path(p, sizeof p, netman_tunnel_dir, t->name, "tun");
	if ((r = ent_write_atomic(p, body, (size_t)l, 0644)) < 0) snprintf(err, en, "%.200s: %s", p, strerror(-r));
	return r;
}

int netman_tunnel_main(FILE *out, FILE *err, int argc, char **argv)
{
	const char *c = argc > 0 ? argv[0] : "status";
	char e[300];
	netman_tun t;
	int r;
	if (!strcmp(c, "status") || !strcmp(c, "list")) {
		if (netman_tunnel_status(out) == 0) fputs("no tunnel profiles (netman tunnel save NAME kind=gre local=... remote=...)\n", out);
		return 0;
	}
	if (!strcmp(c, "save") && argc >= 3) {
		tun_init(&t);
		snprintf(t.name, sizeof t.name, "%s", argv[1]);
		if (!mk_name_ok(argv[1])) { fprintf(err, "tunnel: bad name '%s'\n", argv[1]); return 2; }
		{ char p[400], txt[4096]; mk_path(p, sizeof p, netman_tunnel_dir, argv[1], "tun"); if (ent_slurp(p, txt, sizeof txt) > 0) netman_tunnel_parse(txt, &t, e, sizeof e); }
		for (int i = 2; i < argc; i++) {
			char *eq = strchr(argv[i], '='), k[24];
			if (!eq || eq == argv[i] || eq - argv[i] >= 24) { fprintf(err, "tunnel: expected key=value, got '%s'\n", argv[i]); return 2; }
			snprintf(k, sizeof k, "%.*s", (int)(eq - argv[i]), argv[i]);
			if (set_kv(&t, k, eq + 1, e, sizeof e)) { fprintf(err, "tunnel: %s\n", e); return 2; }
		}
		if (netman_tunnel_check(&t, e, sizeof e) < 0) { fprintf(err, "tunnel: %s\n", e); return 2; }
		if (profile_save(&t, e, sizeof e) < 0) { fprintf(err, "tunnel: %s\n", e); return 1; }
		{
			int u, o, m = netman_tunnel_mtu(&t, &u, &o);
			fprintf(out, "saved %s (%s); mtu %s%d (underlay %d - overhead %d)\n", t.name, t.kind, t.mtu ? "" : "auto ", m, u, o);
		}
		return 0;
	}
	if (!strcmp(c, "show") && argc == 2) {
		char body[2048];
		if (load(argv[1], &t, e, sizeof e) < 0) { fprintf(err, "tunnel: %s\n", e); return 1; }
		netman_tunnel_format(&t, body, sizeof body);
		fputs(body, out);
		return 0;
	}
	if (!strcmp(c, "mtu") && argc == 2) {
		int u, o, m;
		if (load(argv[1], &t, e, sizeof e) < 0) { fprintf(err, "tunnel: %s\n", e); return 1; }
		m = netman_tunnel_mtu(&t, &u, &o);
		fprintf(out, "%s: underlay %d, %s overhead %d, tunnel mtu %d%s\n", t.name, u, t.kind, o, m, t.mtu ? " (fixed in the profile)" : "");
		return 0;
	}
	if ((!strcmp(c, "up") || !strcmp(c, "down")) && argc == 2) {
		int up = !strcmp(c, "up");
		if (load(argv[1], &t, e, sizeof e) < 0) { fprintf(err, "tunnel: %s\n", e); return 1; }
		r = up ? netman_tunnel_up(&t, e, sizeof e) : netman_tunnel_down(&t, e, sizeof e);
		if (r == -EEXIST) { fprintf(out, "%s is already up\n", t.name); return 0; }
		if (r < 0) { fprintf(err, "tunnel: %s\n", e[0] ? e : strerror(-r)); return 1; }
		fprintf(out, "%s %s\n", t.name, up ? "up" : "down");
		return 0;
	}
	if (!strcmp(c, "apply") && argc == 1) {
		char names[64][32];
		int n = mk_names(netman_tunnel_dir, "tun", names, 64), bad = 0, done = 0;
		for (int i = 0; i < n; i++) {
			if (load(names[i], &t, e, sizeof e) < 0) { fprintf(err, "tunnel %s: %s\n", names[i], e); bad++; continue; }
			if (!t.autostart) continue;
			r = netman_tunnel_up(&t, e, sizeof e);
			if (r < 0 && r != -EEXIST) { fprintf(err, "tunnel %s: %s\n", t.name, e); bad++; } else { done++; fprintf(out, "%s %s\n", t.name, r == -EEXIST ? "already up" : "up"); }
		}
		if (!done && !bad) fputs("no tunnel profile has auto = yes\n", out);
		return bad ? 1 : 0;
	}
	if (!strcmp(c, "del") && argc == 2) {
		char p[400];
		if (!mk_name_ok(argv[1])) return 2;
		if (load(argv[1], &t, e, sizeof e) == 0) netman_tunnel_down(&t, e, sizeof e);
		mk_path(p, sizeof p, netman_tunnel_dir, argv[1], "tun");
		if (unlink(p) < 0) { fprintf(err, "tunnel: no profile '%s'\n", argv[1]); return 1; }
		fprintf(out, "tunnel %s removed\n", argv[1]);
		return 0;
	}
	fputs("usage: netman tunnel status|list|save NAME k=v...|show NAME|mtu NAME|up NAME|down NAME|apply|del NAME\n", err);
	return 2;
}
