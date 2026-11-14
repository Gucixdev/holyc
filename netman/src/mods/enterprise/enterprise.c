/* enterprise.c - see enterprise.h. Links, bonding/team, bridge, vlan, macvlan/ipvlan, veth, dummy, routes, rules,
 * ECMP, link settings, Wake-on-LAN, IPv6, wired 802.1X, spec profiles, and the private helpers of entcore.h. */
#define _GNU_SOURCE
#include "enterprise.h"
#include "entcore.h"
#include "../net.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <linux/fib_rules.h>
#include <linux/icmp.h>
#include <linux/if_bridge.h>
#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/ip.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <linux/veth.h>

const char *netman_ent_sys = "/sys";
const char *netman_ent_proc = "/proc";
const char *netman_ent_rt_tables = "/etc/iproute2/rt_tables";
static int ent__icmp_probe(const char *ifn, const char *target, int timeout_ms);
int (*netman_ent_probe_fn)(const char *, const char *, int) = ent__icmp_probe;

/* ======================================================================== shared helpers (entcore.h) */
static char ent__err[256];
const char *netman_ent_error(void) { return ent__err; }
int ent_fail(int e, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(ent__err, sizeof ent__err, fmt, ap);
	va_end(ap);
	return e < 0 ? e : -e;
}
void ent_cpy(char *d, size_t n, const char *s)
{
	size_t k = s ? strlen(s) : 0;
	if (!n) return;
	if (k >= n) k = n - 1;
	if (k) memcpy(d, s, k);
	d[k] = 0;
}
int ent_safe_name(const char *s, size_t max)
{
	size_t i, n = s ? strlen(s) : 0;
	if (!n || n >= max || s[0] == '.') return 0;
	for (i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-' || s[i] == '.')) return 0;
	return 1;
}
char *ent_trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t') s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}
long ent_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
int ent_mkdirs(const char *p)
{
	char t[512], *s;
	if (strlen(p) >= sizeof t) return -ENAMETOOLONG;
	strcpy(t, p);
	for (s = t + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			if (mkdir(t, 0755) < 0 && errno != EEXIST) return -errno;
			*s = '/';
		}
	return (mkdir(t, 0755) < 0 && errno != EEXIST) ? -errno : 0;
}
int ent_write_atomic(const char *path, const char *data, size_t len, mode_t mode)
{
	char t[600];
	int fd;
	size_t off = 0;
	if (snprintf(t, sizeof t, "%s.XXXXXX", path) >= (int)sizeof t) return -ENAMETOOLONG;
	if ((fd = mkstemp(t)) < 0) return -errno;
	while (off < len) {
		ssize_t w = write(fd, data + off, len - off);
		if (w < 0) {
			if (errno == EINTR) continue;
			goto fail;
		}
		off += w;
	}
	if (fchmod(fd, mode) < 0 || fsync(fd) < 0) goto fail;
	close(fd);
	if (rename(t, path) < 0) {
		unlink(t);
		return -errno;
	}
	return 0;
fail : {
	int e = errno;
	close(fd);
	unlink(t);
	return -e;
}
}
int ent_slurp(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t r;
	if (fd < 0) return -errno;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r < 0) return -errno;
	buf[r] = 0;
	return (int)r;
}
void ent_put(struct ent_out *o, const char *fmt, ...)
{
	va_list ap;
	int k;
	va_start(ap, fmt);
	k = vsnprintf(o->p + o->n, o->n < o->cap ? o->cap - o->n : 0, fmt, ap);
	va_end(ap);
	if (k > 0) o->n += k;
}
int ent_exec(char *const argv[], const char *input, char *err, size_t errn)
{
	int in[2] = {-1, -1}, ou[2] = {-1, -1}, st;
	pid_t p;
	if (err && errn) err[0] = 0;
	if (pipe(in) < 0 || pipe(ou) < 0) return -errno;
	if ((p = fork()) < 0) return -errno;
	if (!p) {
		dup2(in[0], 0);
		dup2(ou[1], 2);
		dup2(ou[1], 1);
		close(in[0]), close(in[1]), close(ou[0]), close(ou[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(in[0]);
	close(ou[1]);
	if (input) {
		size_t n = strlen(input), off = 0;
		signal(SIGPIPE, SIG_IGN);
		while (off < n) {
			ssize_t w = write(in[1], input + off, n - off);
			if (w < 0 && errno == EINTR) continue;
			if (w <= 0) break;
			off += w;
		}
	}
	close(in[1]);
	{
		size_t got = 0;
		char tmp[256];
		ssize_t k;
		while ((k = read(ou[0], tmp, sizeof tmp)) > 0)
			if (err && got + 1 < errn) {
				size_t c = (size_t)k < errn - got - 1 ? (size_t)k : errn - got - 1;
				memcpy(err + got, tmp, c);
				got += c;
				err[got] = 0;
			}
	}
	close(ou[0]);
	while (waitpid(p, &st, 0) < 0)
		if (errno != EINTR) return -errno;
	if (WIFEXITED(st) && WEXITSTATUS(st) == 127) return -ENOENT;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -EIO;
}
int ent_parse_ip(const char *s, ent_ip *o, int need_plen)
{
	char t[64], *sl;
	if (!s || !*s || strlen(s) >= sizeof t) return -EINVAL;
	strcpy(t, s);
	o->plen = -1;
	if ((sl = strchr(t, '/'))) {
		char *e;
		long v;
		*sl++ = 0;
		v = strtol(sl, &e, 10);
		if (!*sl || *e || v < 0 || v > 128) return -EINVAL;
		o->plen = (int)v;
	}
	if (need_plen && o->plen < 0) return -EINVAL;
	memset(o->b, 0, 16);
	if (inet_pton(AF_INET, t, o->b) == 1) o->fam = AF_INET;
	else if (inet_pton(AF_INET6, t, o->b) == 1) o->fam = AF_INET6;
	else return -EINVAL;
	if (o->plen > (o->fam == AF_INET ? 32 : 128)) return -EINVAL;
	return 0;
}
int ent_iplen(const ent_ip *i) { return i->fam == AF_INET ? 4 : 16; }
void ent_req_init(ent_req *q, int type, int flags, size_t msglen)
{
	memset(q, 0, sizeof *q);
	q->h.nlmsg_type = type;
	q->h.nlmsg_flags = NLM_F_REQUEST | flags;
	q->h.nlmsg_len = NLMSG_LENGTH(msglen);
}
void ent_attr(ent_req *q, int type, const void *d, int len)
{
	struct rtattr *a = (struct rtattr *)((char *)q + NLMSG_ALIGN(q->h.nlmsg_len));
	if (NLMSG_ALIGN(q->h.nlmsg_len) + RTA_SPACE(len) > sizeof *q) return; /* specs are small: cannot happen */
	a->rta_type = type;
	a->rta_len = RTA_LENGTH(len);
	if (len) memcpy(RTA_DATA(a), d, len);
	q->h.nlmsg_len = NLMSG_ALIGN(q->h.nlmsg_len) + RTA_ALIGN(a->rta_len);
}
int ent_nl(ent_req *q, ent_cb cb, void *u)
{
	struct sockaddr_nl sa = {.nl_family = AF_NETLINK};
	struct timeval tv = {5, 0};
	static char buf[65536];
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE), r = 0;
	if (fd < 0) return -errno;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	q->h.nlmsg_seq = 1;
	if (sendto(fd, q, q->h.nlmsg_len, 0, (struct sockaddr *)&sa, sizeof sa) < 0) {
		r = -errno;
		close(fd);
		return r;
	}
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof buf, 0);
		struct nlmsghdr *m = (struct nlmsghdr *)buf;
		if (n < 0) {
			if (errno == EINTR) continue;
			r = -errno;
			break;
		}
		for (; NLMSG_OK(m, (size_t)n); m = NLMSG_NEXT(m, n)) {
			if (m->nlmsg_type == NLMSG_DONE) goto out;
			if (m->nlmsg_type == NLMSG_ERROR) {
				r = ((struct nlmsgerr *)NLMSG_DATA(m))->error;
				goto out;
			}
			if (cb) cb(m, u);
		}
	}
out:
	close(fd);
	return r;
}
int ent_idx(const char *ifn)
{
	unsigned i = ifn && *ifn ? if_nametoindex(ifn) : 0;
	return i ? (int)i : -ENODEV;
}

#define ENT_FAIL(...) ent_fail(-EINVAL, __VA_ARGS__)
static int ent__nm(const char *s) { return ent_safe_name(s, 16); }
static int ent__is(const netman_ent *e, const char *k) { return !strcmp(e->kind, k); }
static pid_t ent__readpid(const char *path)
{
	char b[32];
	return ent_slurp(path, b, sizeof b) > 0 ? (pid_t)atoi(b) : 0;
}
static void ent__stop(const char *pidfile)
{
	pid_t p = ent__readpid(pidfile);
	if (p > 1) kill(p, SIGTERM);
	unlink(pidfile);
}

/* ======================================================================== spec: keys, parse, format */
#define KS(k, f) {k, 's', offsetof(netman_ent, f), sizeof(((netman_ent *)0)->f)}
#define KI(k, f) {k, 'i', offsetof(netman_ent, f), 0}
#define KT(k, f) {k, 't', offsetof(netman_ent, f), 0}
static const struct {
	const char *k;
	char t;
	size_t off, sz;
} ent__keys[] = {
	KS("kind", kind), KS("name", name), KS("iface", name), KS("parent", parent), KS("peer", peer), KS("inner", inner),
	KS("mode", mode), KS("primary", primary), KS("lacp_rate", lacp_rate), KS("xmit_hash_policy", hash), KS("runner", runner),
	KI("miimon", miimon), KT("stp", stp), KI("forward_delay", fwd_delay), KT("vlan_filtering", vlan_filtering), KI("vid", vid),
	KI("inner_vid", inner_vid), KI("mtu", mtu), KI("netns_pid", netns_pid), KS("proto", proto),
	KS("address", addr), KS("gateway", gateway), KS("dns", dns),
	KI("table", table), KI("metric", metric), KS("dst", dst), KS("src", src), KS("rtype", rtype),
	KS("from", from), KS("to", to), KS("iif", iif), KS("oif", oif), KI("fwmark", fwmark), KI("prio", prio),
	KI("probe_ms", probe_ms), KI("fail_after", fail_after), KI("ok_after", ok_after),
	KS("upstream", upstream), KS("btmac", btmac), KS("dhcp_start", dhcp_start), KS("dhcp_end", dhcp_end),
	KS("eap", eap), KS("identity", identity), KS("anon_identity", anon_identity), KS("password", password),
	KS("ca_cert", ca_cert), KS("client_cert", client_cert), KS("private_key", private_key), KS("phase2", phase2),
	KT("tso", tso), KT("gso", gso), KT("gro", gro), KT("sg", sg), KT("rxcsum", rxcsum), KT("txcsum", txcsum),
	KI("speed", speed), KT("autoneg", autoneg), KS("duplex", duplex), KS("wol", wol),
	KT("accept_ra", accept_ra), KT("autoconf", autoconf), KT("tempaddr", tempaddr), KT("forwarding", forwarding),
	KS("pd_prefix", pd_prefix),
};
#define ENT_NKEYS (sizeof ent__keys / sizeof ent__keys[0])

void netman_ent_init(netman_ent *e)
{
	size_t i;
	memset(e, 0, sizeof *e);
	for (i = 0; i < ENT_NKEYS; i++)
		if (ent__keys[i].t == 't') *(int *)((char *)e + ent__keys[i].off) = -1;
}
static int ent__int(const char *v, int *o)
{
	char *end;
	long n;
	if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "on")) return *o = 1, 0;
	if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "off")) return *o = 0, 0;
	n = strtol(v, &end, 0);
	if (!*v || *end || n < -2147483647L || n > 2147483647L) return -EINVAL;
	*o = (int)n;
	return 0;
}
int netman_ent_parse(const char *text, netman_ent *e)
{
	char *buf = strdup(text), *l, *save;
	int rc = 0;
	if (!buf) return -ENOMEM;
	netman_ent_init(e);
	for (l = strtok_r(buf, "\n", &save); l && !rc; l = strtok_r(NULL, "\n", &save)) {
		char *k = ent_trim(l), *v;
		size_t i;
		if (!*k || *k == '#') continue;
		if (!(v = strchr(k, '='))) { rc = ENT_FAIL("line without '=': %s", k); break; }
		*v++ = 0;
		k = ent_trim(k), v = ent_trim(v);
		if (!strcmp(k, "member")) {
			if (e->nmember >= NETMAN_ENT_MEMBERS || !ent__nm(v)) rc = ENT_FAIL("bad member '%s'", v);
			else ent_cpy(e->member[e->nmember++], 16, v);
			continue;
		}
		if (!strcmp(k, "vlan")) { /* vlan = 10[,pvid][,untagged] */
			char *s2, *t = strtok_r(v, ", ", &s2);
			netman_ent_vlan *vl = &e->vlan[e->nvlan < NETMAN_ENT_VLANS ? e->nvlan : 0];
			if (e->nvlan >= NETMAN_ENT_VLANS || !t || (vl->vid = atoi(t)) < 1 || vl->vid > 4094) { rc = ENT_FAIL("bad vlan '%s'", v); continue; }
			for (t = strtok_r(NULL, ", ", &s2); t; t = strtok_r(NULL, ", ", &s2))
				if (!strcmp(t, "pvid")) vl->pvid = 1;
				else if (!strcmp(t, "untagged")) vl->untagged = 1;
				else rc = ENT_FAIL("bad vlan flag '%s'", t);
			e->nvlan++;
			continue;
		}
		if (!strcmp(k, "uplink")) { /* uplink = IF,GATEWAY[,PROBE[,WEIGHT]] */
			char *f[4] = {0}, *s2, *t;
			int n = 0;
			netman_ent_uplink *u = &e->up[e->nup < NETMAN_ENT_UPLINKS ? e->nup : 0];
			for (t = strtok_r(v, ",", &s2); t && n < 4; t = strtok_r(NULL, ",", &s2)) f[n++] = ent_trim(t);
			if (e->nup >= NETMAN_ENT_UPLINKS || n < 2 || !ent__nm(f[0])) { rc = ENT_FAIL("bad uplink '%s'", v); continue; }
			ent_cpy(u->ifn, 16, f[0]);
			ent_cpy(u->gw, 46, f[1]);
			if (n > 2) ent_cpy(u->probe, 46, f[2]);
			u->weight = n > 3 ? atoi(f[3]) : 1;
			u->up = 1;
			e->nup++;
			continue;
		}
		for (i = 0; i < ENT_NKEYS; i++)
			if (!strcmp(k, ent__keys[i].k)) break;
		if (i == ENT_NKEYS) { rc = ENT_FAIL("unknown key '%s'", k); break; }
		if (ent__keys[i].t == 's') {
			if (strlen(v) >= ent__keys[i].sz) rc = ENT_FAIL("%s too long", k);
			else strcpy((char *)e + ent__keys[i].off, v);
		} else if (ent__int(v, (int *)((char *)e + ent__keys[i].off)) < 0) rc = ENT_FAIL("bad number for %s: '%s'", k, v);
	}
	free(buf);
	return rc ? rc : netman_ent_check(e);
}
int netman_ent_format(const netman_ent *e, char *out, size_t cap)
{
	size_t i, o = 0;
	int j;
#define P(...)                                                                          \
	do {                                                                            \
		int n_ = snprintf(out + o, o < cap ? cap - o : 0, __VA_ARGS__);         \
		if (n_ < 0) return -EIO;                                                \
		o += n_;                                                                \
	} while (0)
	P("kind=%s\n", e->kind);
	for (i = 0; i < ENT_NKEYS; i++) {
		const char *p = (const char *)e + ent__keys[i].off;
		if (!strcmp(ent__keys[i].k, "kind") || !strcmp(ent__keys[i].k, "iface")) continue;
		if (ent__keys[i].t == 's') {
			if (*p) P("%s=%s\n", ent__keys[i].k, p);
		} else if (ent__keys[i].t == 't') {
			if (*(const int *)p != -1) P("%s=%d\n", ent__keys[i].k, *(const int *)p);
		} else if (*(const int *)p) P("%s=%d\n", ent__keys[i].k, *(const int *)p);
	}
	for (j = 0; j < e->nmember; j++) P("member=%s\n", e->member[j]);
	for (j = 0; j < e->nvlan; j++) P("vlan=%d%s%s\n", e->vlan[j].vid, e->vlan[j].pvid ? ",pvid" : "", e->vlan[j].untagged ? ",untagged" : "");
	for (j = 0; j < e->nup; j++) P("uplink=%s,%s,%s,%d\n", e->up[j].ifn, e->up[j].gw, e->up[j].probe, e->up[j].weight ? e->up[j].weight : 1);
#undef P
	return o >= cap ? -E2BIG : (int)o;
}

static const char *const ent__kinds[] = {"bond", "team", "bridge", "vlan", "macvlan", "ipvlan", "veth", "dummy", "tether", "dot1x", "route", "rule", "ecmp", "link", "ipv6", "wol", NULL};
static const char *const ent__bmodes[] = {"balance-rr", "active-backup", "balance-xor", "broadcast", "802.3ad", "balance-tlb", "balance-alb"};
static const char *const ent__bhash[] = {"layer2", "layer3+4", "layer2+3", "encap2+3", "encap3+4", "vlan+srcmac"};

static int ent__bond_mode(const netman_ent *e)
{
	static const struct { const char *r, *m; } team[] = {{"activebackup", "active-backup"}, {"lacp", "802.3ad"}, {"loadbalance", "balance-xor"}, {"broadcast", "broadcast"}, {"roundrobin", "balance-rr"}};
	const char *m = e->mode;
	size_t i;
	if (ent__is(e, "team") && e->runner[0])
		for (i = 0; i < 5; i++)
			if (!strcmp(e->runner, team[i].r)) m = team[i].m;
	if (!*m) return 1; /* active-backup */
	if (!strcmp(m, "lacp")) m = "802.3ad";
	for (i = 0; i < 7; i++)
		if (!strcmp(m, ent__bmodes[i]) || (m[0] >= '0' && m[0] <= '6' && !m[1] && m[0] - '0' == (int)i)) return (int)i;
	return -1;
}
int netman_ent_check(const netman_ent *e)
{
	int i;
	for (i = 0; ent__kinds[i]; i++)
		if (ent__is(e, ent__kinds[i])) break;
	if (!ent__kinds[i]) return ENT_FAIL("unknown kind '%s'", e->kind);
	if (!ent__nm(e->name)) return ENT_FAIL("bad or missing name");
	if ((ent__is(e, "bond") || ent__is(e, "team")) && ent__bond_mode(e) < 0) return ENT_FAIL("bad bond mode '%s'", e->mode);
	if (e->hash[0]) {
		for (i = 0; i < 6 && strcmp(e->hash, ent__bhash[i]); i++) ;
		if (i == 6) return ENT_FAIL("bad xmit_hash_policy '%s'", e->hash);
	}
	if (e->lacp_rate[0] && strcmp(e->lacp_rate, "slow") && strcmp(e->lacp_rate, "fast")) return ENT_FAIL("lacp_rate must be slow or fast");
	for (i = 0; i < e->nmember; i++)
		if (!ent__nm(e->member[i])) return ENT_FAIL("bad member name");
	if (e->vid && (e->vid < 1 || e->vid > 4094)) return ENT_FAIL("vlan id must be 1..4094");
	if (e->proto[0] && strcmp(e->proto, "802.1q") && strcmp(e->proto, "802.1ad")) return ENT_FAIL("proto must be 802.1q or 802.1ad");
	for (i = 0; i < e->nup; i++)
		if (!ent__nm(e->up[i].ifn) || e->up[i].weight < 0 || e->up[i].weight > 255) return ENT_FAIL("bad uplink %d", i);
	if (e->wol[0] && strspn(e->wol, "pumbagsd") != strlen(e->wol)) return ENT_FAIL("wol letters are p u m b a g s d");
	if (ent__is(e, "dot1x")) { /* the strings land in a quoted wpa_supplicant config: no quotes, backslashes, newlines */
		const char *s[] = {e->identity, e->anon_identity, e->password, e->ca_cert, e->client_cert, e->private_key, e->phase2, e->eap};
		for (i = 0; i < 8; i++)
			if (strpbrk(s[i], "\"\\\r\n")) return ENT_FAIL("dot1x fields may not contain quotes, backslashes or newlines");
	}
	return 0;
}

static int ent__path(char *p, size_t n, const char *name, const char *ext)
{
	if (!ent_safe_name(name, 64)) return ent_fail(-EINVAL, "bad profile name");
	snprintf(p, n, "%s/%s.%s", netman_conf_dir, name, ext);
	return 0;
}
int netman_ent_load(const char *name, netman_ent *e)
{
	char path[600], buf[8192];
	int r = ent__path(path, sizeof path, name, "ent");
	if (r < 0) return r;
	if ((r = ent_slurp(path, buf, sizeof buf)) < 0) return ent_fail(r, "cannot read %s: %s", path, strerror(-r));
	if (!(r = netman_ent_parse(buf, e)) && !e->name[0]) ent_cpy(e->name, 16, name);
	return r;
}
int netman_ent_save(const netman_ent *e)
{
	char path[600], b[8192];
	int n = netman_ent_format(e, b, sizeof b), r;
	if (n < 0) return n;
	if ((r = netman_ent_check(e)) < 0) return r;
	if ((r = ent_mkdirs(netman_conf_dir)) < 0) return r;
	if ((r = ent__path(path, sizeof path, e->name, "ent")) < 0) return r;
	return ent_write_atomic(path, b, n, ent__is(e, "dot1x") ? 0600 : 0644);
}
int netman_ent_list(char names[][64], int max)
{
	struct dirent **ls = NULL;
	int n = scandir(netman_conf_dir, &ls, NULL, alphasort), i, c = 0;
	if (n < 0) return errno == ENOENT ? 0 : -errno;
	for (i = 0; i < n; i++) {
		size_t l = strlen(ls[i]->d_name);
		if (c < max && l > 4 && !strcmp(ls[i]->d_name + l - 4, ".ent") && l - 4 < 64) {
			memcpy(names[c], ls[i]->d_name, l - 4);
			names[c++][l - 4] = 0;
		}
		free(ls[i]);
	}
	free(ls);
	return c;
}
static int ent__rank(const char *k)
{
	static const char *const r[] = {"dummy veth", "bond team", "bridge", "vlan macvlan ipvlan", "link ipv6 wol", "route rule ecmp", "tether dot1x"};
	int i;
	for (i = 0; i < 7; i++) {
		const char *p = strstr(r[i], k);
		if (p && (p == r[i] || p[-1] == ' ') && (!p[strlen(k)] || p[strlen(k)] == ' ')) return i;
	}
	return 9;
}
int netman_ent_apply_all(void)
{
	char names[64][64];
	static netman_ent es[64];
	int n = netman_ent_list(names, 64), i, rank, bad = 0;
	if (n < 0) return n;
	for (i = 0; i < n; i++)
		if (netman_ent_load(names[i], &es[i]) < 0) {
			fprintf(stderr, "netman: %s: %s\n", names[i], netman_ent_error());
			es[i].kind[0] = 0;
			bad++;
		}
	for (rank = 0; rank < 8; rank++)
		for (i = 0; i < n; i++)
			if (es[i].kind[0] && ent__rank(es[i].kind) == rank) {
				int r = netman_ent_add(&es[i]);
				if (r < 0 && r != -EEXIST) {
					fprintf(stderr, "netman: %s: %s\n", names[i], r == -EINVAL ? netman_ent_error() : strerror(-r));
					bad++;
				}
			}
	return bad;
}

/* ======================================================================== netlink helpers */
static void ent__a8(ent_req *q, int t, uint8_t v) { ent_attr(q, t, &v, 1); }
static void ent__a16(ent_req *q, int t, uint16_t v) { ent_attr(q, t, &v, 2); }
static void ent__a32(ent_req *q, int t, uint32_t v) { ent_attr(q, t, &v, 4); }
static void ent__astr(ent_req *q, int t, const char *s) { ent_attr(q, t, s, (int)strlen(s) + 1); }
static int ent__nest(ent_req *q, int type)
{
	int off = NLMSG_ALIGN(q->h.nlmsg_len);
	ent_attr(q, type, NULL, 0);
	return off;
}
static void ent__end(ent_req *q, int off) { ((struct rtattr *)((char *)q + off))->rta_len = q->h.nlmsg_len - off; }

static int ent__begin(ent_req *q, const char *name, const char *kind, int parent, int mtu)
{
	int li;
	ent_req_init(q, RTM_NEWLINK, NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL, sizeof(struct ifinfomsg));
	ent__astr(q, IFLA_IFNAME, name);
	if (parent > 0) ent__a32(q, IFLA_LINK, parent);
	if (mtu > 0) ent__a32(q, IFLA_MTU, mtu);
	li = ent__nest(q, IFLA_LINKINFO);
	ent__astr(q, IFLA_INFO_KIND, kind);
	return li;
}
static int ent__dellink(const char *name)
{
	ent_req q;
	ent_req_init(&q, RTM_DELLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	ent__astr(&q, IFLA_IFNAME, name);
	return ent_nl(&q, NULL, NULL);
}
static int ent__setmtu(const char *ifn, int mtu)
{
	ent_req q;
	int i = ent_idx(ifn);
	if (i < 0) return i;
	ent_req_init(&q, RTM_NEWLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	q.m.i.ifi_index = i;
	ent__a32(&q, IFLA_MTU, mtu);
	return ent_nl(&q, NULL, NULL);
}
static int ent__enslave(const char *slave, int master)
{
	ent_req q;
	int i = ent_idx(slave), r;
	if (i < 0) return ent_fail(i, "no such interface '%s'", slave);
	netman_link_set(slave, 0); /* bonding refuses a slave that is up */
	ent_req_init(&q, RTM_NEWLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	q.m.i.ifi_index = i;
	ent__a32(&q, IFLA_MASTER, master);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) return ent_fail(r, "cannot enslave %s: %s", slave, strerror(-r));
	return netman_link_set(slave, 1);
}
static void ent__rta(struct rtattr **tb, int max, struct rtattr *r, int len)
{
	memset(tb, 0, sizeof(*tb) * (max + 1));
	for (; RTA_OK(r, len); r = RTA_NEXT(r, len))
		if (r->rta_type <= max) tb[r->rta_type] = r;
}
static uint32_t ent__u(struct rtattr *a)
{
	if (!a) return (uint32_t)-1;
	switch (RTA_PAYLOAD(a)) {
	case 1: return *(uint8_t *)RTA_DATA(a);
	case 2: return *(uint16_t *)RTA_DATA(a);
	default: return *(uint32_t *)RTA_DATA(a);
	}
}

/* ---------- link queries ---------- */
static void ent__info_cb(const struct nlmsghdr *m, void *u)
{
	netman_ent_info *o = u;
	struct ifinfomsg *i = NLMSG_DATA(m);
	struct rtattr *tb[64], *li[16], *d[64];
	if (m->nlmsg_type != RTM_NEWLINK) return;
	ent__rta(tb, 63, IFLA_RTA(i), IFLA_PAYLOAD(m));
	o->idx = i->ifi_index;
	o->up = !!(i->ifi_flags & IFF_UP);
	o->master = tb[IFLA_MASTER] ? (int)ent__u(tb[IFLA_MASTER]) : 0;
	o->link = tb[IFLA_LINK] ? (int)ent__u(tb[IFLA_LINK]) : 0;
	o->mtu = tb[IFLA_MTU] ? (int)ent__u(tb[IFLA_MTU]) : 0;
	if (!tb[IFLA_LINKINFO]) return;
	ent__rta(li, 15, RTA_DATA(tb[IFLA_LINKINFO]), RTA_PAYLOAD(tb[IFLA_LINKINFO]));
	if (li[IFLA_INFO_KIND]) ent_cpy(o->kind, sizeof o->kind, RTA_DATA(li[IFLA_INFO_KIND]));
	if (!li[IFLA_INFO_DATA]) return;
	ent__rta(d, 63, RTA_DATA(li[IFLA_INFO_DATA]), RTA_PAYLOAD(li[IFLA_INFO_DATA]));
	if (!strcmp(o->kind, "bond")) {
		o->mode = (int)ent__u(d[IFLA_BOND_MODE]);
		o->miimon = (int)ent__u(d[IFLA_BOND_MIIMON]);
		o->lacp = (int)ent__u(d[IFLA_BOND_AD_LACP_RATE]);
		o->hash = (int)ent__u(d[IFLA_BOND_XMIT_HASH_POLICY]);
		o->primary = (int)ent__u(d[IFLA_BOND_PRIMARY]);
	} else if (!strcmp(o->kind, "bridge")) {
		o->stp = (int)ent__u(d[IFLA_BR_STP_STATE]);
		o->fwd_delay = (int)ent__u(d[IFLA_BR_FORWARD_DELAY]);
		o->vlan_filtering = (int)ent__u(d[IFLA_BR_VLAN_FILTERING]);
	} else if (!strcmp(o->kind, "vlan")) {
		o->vid = (int)ent__u(d[IFLA_VLAN_ID]);
		o->proto = d[IFLA_VLAN_PROTOCOL] ? ntohs(*(uint16_t *)RTA_DATA(d[IFLA_VLAN_PROTOCOL])) : 0x8100;
	} else if (!strcmp(o->kind, "macvlan") || !strcmp(o->kind, "ipvlan")) o->mode = (int)ent__u(d[1]); /* IFLA_*VLAN_MODE == 1 */
}
int netman_ent_getinfo(const char *ifn, netman_ent_info *o)
{
	ent_req q;
	int r;
	memset(o, -1, sizeof *o);
	o->kind[0] = 0;
	o->idx = 0;
	ent_req_init(&q, RTM_GETLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	ent__astr(&q, IFLA_IFNAME, ifn);
	r = ent_nl(&q, ent__info_cb, o);
	if (r < 0) return r;
	return o->idx ? 0 : -ENODEV;
}
struct ent_lk { char name[16]; int idx, master; };
struct ent_lks { struct ent_lk *v; int n, max; };
static void ent__lk_cb(const struct nlmsghdr *m, void *u)
{
	struct ent_lks *l = u;
	struct ifinfomsg *i = NLMSG_DATA(m);
	struct rtattr *tb[64];
	if (m->nlmsg_type != RTM_NEWLINK || l->n >= l->max) return;
	ent__rta(tb, 63, IFLA_RTA(i), IFLA_PAYLOAD(m));
	if (!tb[IFLA_IFNAME]) return;
	ent_cpy(l->v[l->n].name, 16, RTA_DATA(tb[IFLA_IFNAME]));
	l->v[l->n].idx = i->ifi_index;
	l->v[l->n].master = tb[IFLA_MASTER] ? (int)ent__u(tb[IFLA_MASTER]) : 0;
	l->n++;
}
int netman_ent_members(const char *master, char out[][16], int max)
{
	static struct ent_lk v[512];
	struct ent_lks l = {v, 0, 512};
	ent_req q;
	int mi = ent_idx(master), r, i, c = 0;
	if (mi < 0) return mi;
	ent_req_init(&q, RTM_GETLINK, NLM_F_DUMP, sizeof(struct ifinfomsg));
	if ((r = ent_nl(&q, ent__lk_cb, &l)) < 0) return r;
	for (i = 0; i < l.n && c < max; i++)
		if (v[i].master == mi) ent_cpy(out[c++], 16, v[i].name);
	return c;
}

/* ======================================================================== bonding / team */
int netman_ent_bond_add(const netman_ent *e)
{
	ent_req q;
	int li, di, r, i, mode, pi = 0;
	if ((r = netman_ent_check(e)) < 0) return r;
	mode = ent__bond_mode(e);
	if (e->primary[0] && (pi = ent_idx(e->primary)) < 0) return ent_fail(pi, "primary '%s' does not exist", e->primary);
	li = ent__begin(&q, e->name, "bond", 0, e->mtu);
	di = ent__nest(&q, IFLA_INFO_DATA);
	ent__a8(&q, IFLA_BOND_MODE, mode);
	ent__a32(&q, IFLA_BOND_MIIMON, e->miimon > 0 ? e->miimon : 100);
	if (pi > 0) ent__a32(&q, IFLA_BOND_PRIMARY, pi);
	if (e->lacp_rate[0]) ent__a8(&q, IFLA_BOND_AD_LACP_RATE, !strcmp(e->lacp_rate, "fast"));
	if (e->hash[0])
		for (i = 0; i < 6; i++)
			if (!strcmp(e->hash, ent__bhash[i])) ent__a8(&q, IFLA_BOND_XMIT_HASH_POLICY, i);
	ent__end(&q, di);
	ent__end(&q, li);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) return ent_fail(r, "bond %s: %s", e->name, strerror(-r));
	{
		int bi = ent_idx(e->name);
		for (i = 0; i < e->nmember; i++)
			if ((r = ent__enslave(e->member[i], bi)) < 0) return r;
	}
	if (e->addr[0] && (r = netman_addr_add(e->name, e->addr)) < 0) return r;
	return netman_link_set(e->name, 1);
}
int netman_ent_bond_del(const netman_ent *e) { return ent__dellink(e->name); }
/* add / remove ONE slave of a live bond (bonding.c re-exports this) */
int netman_ent_bond_slave(const char *bond, const char *slave, int add)
{
	ent_req q;
	int bi = ent_idx(bond), si = ent_idx(slave), r;
	if (bi < 0) return ent_fail(bi, "no such bond '%s'", bond);
	if (si < 0) return ent_fail(si, "no such interface '%s'", slave);
	if (add) return ent__enslave(slave, bi);
	ent_req_init(&q, RTM_NEWLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	q.m.i.ifi_index = si;
	ent__a32(&q, IFLA_MASTER, 0);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) return ent_fail(r, "cannot release %s: %s", slave, strerror(-r));
	return 0;
}
int netman_ent_bond_show(const netman_ent *e, FILE *f)
{
	netman_ent_info i;
	char m[NETMAN_ENT_MEMBERS * 2][16];
	int n, k, r = netman_ent_getinfo(e->name, &i);
	if (r < 0) return r;
	fprintf(f, "%s %s: mode %s miimon %d", e->name, ent__is(e, "team") ? "team (bonding)" : "bond", i.mode >= 0 && i.mode < 7 ? ent__bmodes[i.mode] : "?", i.miimon);
	if (i.mode == 4) fprintf(f, " lacp_rate %s", i.lacp ? "fast" : "slow");
	if (i.hash >= 0 && i.hash < 6) fprintf(f, " hash %s", ent__bhash[i.hash]);
	if (i.primary > 0) {
		char b[IF_NAMESIZE];
		fprintf(f, " primary %s", if_indextoname(i.primary, b) ? b : "?");
	}
	fprintf(f, "\n  slaves:");
	n = netman_ent_members(e->name, m, NETMAN_ENT_MEMBERS * 2);
	for (k = 0; k < n; k++) fprintf(f, " %s", m[k]);
	fprintf(f, "%s\n", n ? "" : " (none)");
	return 0;
}
int netman_ent_team_add(const netman_ent *e) { return netman_ent_bond_add(e); }
int netman_ent_team_del(const netman_ent *e) { return netman_ent_bond_del(e); }
int netman_ent_team_show(const netman_ent *e, FILE *f) { return netman_ent_bond_show(e, f); }

/* ======================================================================== bridge */
static int ent__brvlan(int op, const char *port, int vid, int pvid, int untagged)
{
	ent_req q;
	struct bridge_vlan_info vi = {0};
	int i = ent_idx(port), af;
	if (i < 0) return i;
	vi.vid = vid;
	vi.flags = (pvid ? BRIDGE_VLAN_INFO_PVID : 0) | (untagged ? BRIDGE_VLAN_INFO_UNTAGGED : 0);
	ent_req_init(&q, op, NLM_F_ACK, sizeof(struct ifinfomsg));
	q.m.i.ifi_family = AF_BRIDGE;
	q.m.i.ifi_index = i;
	af = ent__nest(&q, IFLA_AF_SPEC);
	ent_attr(&q, IFLA_BRIDGE_VLAN_INFO, &vi, sizeof vi);
	ent__end(&q, af);
	return ent_nl(&q, NULL, NULL);
}
int netman_ent_bridge_add(const netman_ent *e)
{
	ent_req q;
	int li, di, r, i, k, bi;
	if ((r = netman_ent_check(e)) < 0) return r;
	li = ent__begin(&q, e->name, "bridge", 0, e->mtu);
	di = ent__nest(&q, IFLA_INFO_DATA);
	if (e->stp >= 0) ent__a32(&q, IFLA_BR_STP_STATE, e->stp);
	if (e->fwd_delay > 0) ent__a32(&q, IFLA_BR_FORWARD_DELAY, e->fwd_delay * 100); /* USER_HZ ticks */
	if (e->vlan_filtering >= 0) ent__a8(&q, IFLA_BR_VLAN_FILTERING, e->vlan_filtering);
	ent__end(&q, di);
	ent__end(&q, li);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) return ent_fail(r, "bridge %s: %s", e->name, strerror(-r));
	bi = ent_idx(e->name);
	for (i = 0; i < e->nmember; i++) {
		if ((r = ent__enslave(e->member[i], bi)) < 0) return r;
		for (k = 0; k < e->nvlan; k++)
			if ((r = ent__brvlan(RTM_SETLINK, e->member[i], e->vlan[k].vid, e->vlan[k].pvid, e->vlan[k].untagged)) < 0)
				return ent_fail(r, "vlan %d on %s: %s", e->vlan[k].vid, e->member[i], strerror(-r));
	}
	if (e->addr[0] && (r = netman_addr_add(e->name, e->addr)) < 0) return r;
	return netman_link_set(e->name, 1);
}
int netman_ent_bridge_del(const netman_ent *e) { return ent__dellink(e->name); }
struct ent_brv { int idx, n, max; netman_ent_vlan *v; };
static void ent__brv_cb(const struct nlmsghdr *m, void *u)
{
	struct ent_brv *c = u;
	struct ifinfomsg *i = NLMSG_DATA(m);
	struct rtattr *tb[64], *a;
	int len;
	if (m->nlmsg_type != RTM_NEWLINK || i->ifi_index != c->idx) return;
	ent__rta(tb, 63, IFLA_RTA(i), IFLA_PAYLOAD(m));
	if (!tb[IFLA_AF_SPEC]) return;
	a = RTA_DATA(tb[IFLA_AF_SPEC]);
	len = RTA_PAYLOAD(tb[IFLA_AF_SPEC]);
	for (; RTA_OK(a, len); a = RTA_NEXT(a, len))
		if (a->rta_type == IFLA_BRIDGE_VLAN_INFO && c->n < c->max && RTA_PAYLOAD(a) >= sizeof(struct bridge_vlan_info)) {
			struct bridge_vlan_info *v = RTA_DATA(a);
			c->v[c->n].vid = v->vid;
			c->v[c->n].pvid = !!(v->flags & BRIDGE_VLAN_INFO_PVID);
			c->v[c->n].untagged = !!(v->flags & BRIDGE_VLAN_INFO_UNTAGGED);
			c->n++;
		}
}
int netman_ent_bridge_vlans(const char *port, netman_ent_vlan *out, int max)
{
	struct ent_brv c = {ent_idx(port), 0, max, out};
	ent_req q;
	int r;
	if (c.idx < 0) return c.idx;
	ent_req_init(&q, RTM_GETLINK, NLM_F_DUMP, sizeof(struct ifinfomsg));
	q.m.i.ifi_family = AF_BRIDGE;
	ent__a32(&q, IFLA_EXT_MASK, RTEXT_FILTER_BRVLAN);
	if ((r = ent_nl(&q, ent__brv_cb, &c)) < 0) return r;
	return c.n;
}
int netman_ent_bridge_show(const netman_ent *e, FILE *f)
{
	netman_ent_info i;
	char m[NETMAN_ENT_MEMBERS * 2][16];
	netman_ent_vlan v[NETMAN_ENT_VLANS];
	int n, k, j, nv, r = netman_ent_getinfo(e->name, &i);
	if (r < 0) return r;
	fprintf(f, "%s bridge: stp %s forward_delay %ds vlan_filtering %s\n", e->name, i.stp > 0 ? "on" : "off", i.fwd_delay / 100, i.vlan_filtering > 0 ? "on" : "off");
	n = netman_ent_members(e->name, m, NETMAN_ENT_MEMBERS * 2);
	for (k = 0; k < n; k++) {
		fprintf(f, "  port %s", m[k]);
		nv = netman_ent_bridge_vlans(m[k], v, NETMAN_ENT_VLANS);
		for (j = 0; j < nv; j++) fprintf(f, " vlan %d%s%s", v[j].vid, v[j].pvid ? " pvid" : "", v[j].untagged ? " untagged" : "");
		fputc('\n', f);
	}
	return 0;
}

/* ======================================================================== vlan / macvlan / ipvlan / veth / dummy */
int netman_ent_vlan_add(const netman_ent *e)
{
	ent_req q;
	int li, di, r, p, qinq;
	if ((r = netman_ent_check(e)) < 0) return r;
	if (e->vid < 1) return ENT_FAIL("vlan needs vid=");
	if ((p = ent_idx(e->parent)) < 0) return ent_fail(p, "vlan %s: parent '%s' does not exist", e->name, e->parent);
	qinq = !strcmp(e->proto, "802.1ad");
	netman_link_set(e->parent, 1); /* a vlan cannot come up over a down lower device */
	li = ent__begin(&q, e->name, "vlan", p, e->mtu);
	di = ent__nest(&q, IFLA_INFO_DATA);
	ent__a16(&q, IFLA_VLAN_ID, e->vid);
	if (qinq) ent__a16(&q, IFLA_VLAN_PROTOCOL, htons(ETH_P_8021AD));
	ent__end(&q, di);
	ent__end(&q, li);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) return ent_fail(r, "vlan %s: %s", e->name, strerror(-r));
	if (e->inner_vid > 0) { /* QinQ: a plain 802.1Q vlan riding inside the outer service tag */
		char in[16];
		int pi = ent_idx(e->name);
		netman_link_set(e->name, 1);
		ent_cpy(in, sizeof in, e->inner);
		if (!in[0] && snprintf(in, sizeof in, "%s.%d", e->name, e->inner_vid) >= (int)sizeof in) return ENT_FAIL("inner vlan name too long, set inner=");
		li = ent__begin(&q, in, "vlan", pi, 0);
		di = ent__nest(&q, IFLA_INFO_DATA);
		ent__a16(&q, IFLA_VLAN_ID, e->inner_vid);
		ent__end(&q, di);
		ent__end(&q, li);
		if ((r = ent_nl(&q, NULL, NULL)) < 0) return ent_fail(r, "inner vlan %s: %s", in, strerror(-r));
		netman_link_set(in, 1);
	}
	if (e->addr[0] && (r = netman_addr_add(e->name, e->addr)) < 0) return r;
	return netman_link_set(e->name, 1);
}
int netman_ent_vlan_del(const netman_ent *e) { return ent__dellink(e->name); } /* deleting the outer removes the inner */
int netman_ent_vlan_show(const netman_ent *e, FILE *f)
{
	netman_ent_info i;
	char b[IF_NAMESIZE];
	int r = netman_ent_getinfo(e->name, &i);
	if (r < 0) return r;
	fprintf(f, "%s vlan %d on %s proto %s\n", e->name, i.vid, i.link > 0 && if_indextoname(i.link, b) ? b : "?", i.proto == 0x88a8 ? "802.1ad" : "802.1Q");
	return 0;
}

struct ent_mode { const char *n; int v; };
static const struct ent_mode ent__mvmodes[] = {{"private", MACVLAN_MODE_PRIVATE}, {"vepa", MACVLAN_MODE_VEPA}, {"bridge", MACVLAN_MODE_BRIDGE}, {"passthru", MACVLAN_MODE_PASSTHRU}, {"source", MACVLAN_MODE_SOURCE}};
static const struct ent_mode ent__ipmodes[] = {{"l2", IPVLAN_MODE_L2}, {"l3", IPVLAN_MODE_L3}, {"l3s", IPVLAN_MODE_L3S}};
static int ent__mode(const char *m, const char *dflt, const struct ent_mode *t, int n, int *out)
{
	int i;
	if (!*m) m = dflt;
	for (i = 0; i < n; i++)
		if (!strcmp(m, t[i].n)) return *out = t[i].v, 0;
	return ENT_FAIL("bad mode '%s'", m);
}
static int ent__vlandev(const netman_ent *e, const char *kind)
{
	ent_req q;
	int li, di, r, p, mode = 0;
	if ((r = netman_ent_check(e)) < 0) return r;
	if ((p = ent_idx(e->parent)) < 0) return ent_fail(p, "%s %s: parent '%s' does not exist", kind, e->name, e->parent);
	if (!strcmp(kind, "macvlan") ? ent__mode(e->mode, "bridge", ent__mvmodes, 5, &mode) : ent__mode(e->mode, "l2", ent__ipmodes, 3, &mode)) return -EINVAL;
	netman_link_set(e->parent, 1);
	li = ent__begin(&q, e->name, kind, p, e->mtu);
	di = ent__nest(&q, IFLA_INFO_DATA);
	if (!strcmp(kind, "macvlan")) ent__a32(&q, IFLA_MACVLAN_MODE, mode);
	else ent__a16(&q, IFLA_IPVLAN_MODE, mode);
	ent__end(&q, di);
	ent__end(&q, li);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) return ent_fail(r, "%s %s: %s", kind, e->name, strerror(-r));
	if (e->addr[0] && (r = netman_addr_add(e->name, e->addr)) < 0) return r;
	return netman_link_set(e->name, 1);
}
int netman_ent_macvlan_add(const netman_ent *e) { return ent__vlandev(e, "macvlan"); }
int netman_ent_ipvlan_add(const netman_ent *e) { return ent__vlandev(e, "ipvlan"); }
int netman_ent_macvlan_del(const netman_ent *e) { return ent__dellink(e->name); }
int netman_ent_ipvlan_del(const netman_ent *e) { return ent__dellink(e->name); }
static int ent__vshow(const netman_ent *e, FILE *f, const struct ent_mode *t, int n)
{
	netman_ent_info i;
	char b[IF_NAMESIZE];
	int r = netman_ent_getinfo(e->name, &i), k;
	if (r < 0) return r;
	fprintf(f, "%s %s on %s mode ", e->name, i.kind, i.link > 0 && if_indextoname(i.link, b) ? b : "?");
	for (k = 0; k < n; k++)
		if (t[k].v == i.mode) fputs(t[k].n, f);
	fputc('\n', f);
	return 0;
}
int netman_ent_macvlan_show(const netman_ent *e, FILE *f) { return ent__vshow(e, f, ent__mvmodes, 5); }
int netman_ent_ipvlan_show(const netman_ent *e, FILE *f) { return ent__vshow(e, f, ent__ipmodes, 3); }

int netman_ent_veth_add(const netman_ent *e)
{
	ent_req q;
	int li, di, pe, r;
	if ((r = netman_ent_check(e)) < 0) return r;
	if (!ent__nm(e->peer)) return ENT_FAIL("veth needs peer=");
	li = ent__begin(&q, e->name, "veth", 0, e->mtu);
	di = ent__nest(&q, IFLA_INFO_DATA);
	pe = ent__nest(&q, VETH_INFO_PEER);
	{ /* the peer attribute carries a bare ifinfomsg followed by the peer's own attributes */
		int start = NLMSG_ALIGN(q.h.nlmsg_len);
		memset((char *)&q + start, 0, sizeof(struct ifinfomsg));
		q.h.nlmsg_len = start + sizeof(struct ifinfomsg);
	}
	ent__astr(&q, IFLA_IFNAME, e->peer);
	if (e->netns_pid > 0) ent__a32(&q, IFLA_NET_NS_PID, e->netns_pid);
	ent__end(&q, pe);
	ent__end(&q, di);
	ent__end(&q, li);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) return ent_fail(r, "veth %s: %s", e->name, strerror(-r));
	if (e->addr[0] && (r = netman_addr_add(e->name, e->addr)) < 0) return r;
	return netman_link_set(e->name, 1);
}
int netman_ent_veth_del(const netman_ent *e) { return ent__dellink(e->name); } /* the kernel removes the peer too */
int netman_ent_veth_show(const netman_ent *e, FILE *f)
{
	netman_ent_info i;
	char b[IF_NAMESIZE];
	int r = netman_ent_getinfo(e->name, &i);
	if (r < 0) return r;
	fprintf(f, "%s veth peer %s mtu %d %s\n", e->name, i.link > 0 && if_indextoname(i.link, b) ? b : "(other netns)", i.mtu, i.up ? "up" : "down");
	return 0;
}

int netman_ent_dummy_add(const netman_ent *e)
{
	ent_req q;
	int li, r;
	if ((r = netman_ent_check(e)) < 0) return r;
	li = ent__begin(&q, e->name, "dummy", 0, e->mtu);
	ent__end(&q, li);
	if ((r = ent_nl(&q, NULL, NULL)) < 0) return ent_fail(r, "dummy %s: %s", e->name, strerror(-r));
	if (e->addr[0] && (r = netman_addr_add(e->name, e->addr)) < 0) return r;
	return netman_link_set(e->name, 1);
}
int netman_ent_dummy_del(const netman_ent *e) { return ent__dellink(e->name); }
int netman_ent_dummy_show(const netman_ent *e, FILE *f)
{
	netman_ent_info i;
	int r = netman_ent_getinfo(e->name, &i);
	if (r < 0) return r;
	fprintf(f, "%s dummy mtu %d %s\n", e->name, i.mtu, i.up ? "up" : "down");
	return 0;
}

/* ======================================================================== routes, rules, tables */
int netman_ent_table_id(const char *s)
{
	char line[128], nm[64];
	FILE *f;
	int t = -1, n;
	if (!s || !*s) return -1;
	if (s[0] >= '0' && s[0] <= '9') return atoi(s);
	if (!strcmp(s, "main")) return (int)RT_TABLE_MAIN;
	if (!strcmp(s, "local")) return RT_TABLE_LOCAL;
	if (!strcmp(s, "default")) return RT_TABLE_DEFAULT;
	if ((f = fopen(netman_ent_rt_tables, "r"))) {
		while (fgets(line, sizeof line, f))
			if (line[0] != '#' && sscanf(line, "%d %63s", &n, nm) == 2 && !strcmp(nm, s)) t = n;
		fclose(f);
	}
	return t;
}
static void ent__rtmsg_table(ent_req *q, int table)
{
	q->m.r.rtm_table = table > 255 ? (int)RT_TABLE_UNSPEC : table;
	ent__a32(q, RTA_TABLE, table);
}
static int ent__route(int op, const netman_ent *e, int onlink)
{
	ent_req q;
	ent_ip d, g;
	int oif = 0, has_gw = e->gateway[0], deflt = !e->dst[0] || !strcmp(e->dst, "default"), table = e->table > 0 ? e->table : (int)RT_TABLE_MAIN;
	int type = RTN_UNICAST, del = op == RTM_DELROUTE;
	memset(&d, 0, sizeof d);
	if (e->rtype[0]) {
		if (!strcmp(e->rtype, "blackhole")) type = RTN_BLACKHOLE;
		else if (!strcmp(e->rtype, "unreachable")) type = RTN_UNREACHABLE;
		else if (!strcmp(e->rtype, "prohibit")) type = RTN_PROHIBIT;
		else if (strcmp(e->rtype, "unicast")) return ENT_FAIL("bad rtype '%s'", e->rtype);
	}
	if (has_gw && ent_parse_ip(e->gateway, &g, 0) < 0) return ENT_FAIL("bad gateway '%s'", e->gateway);
	if (!deflt && ent_parse_ip(e->dst, &d, 1) < 0) return ENT_FAIL("bad dst '%s'", e->dst);
	if (deflt) {
		d.fam = has_gw ? g.fam : AF_INET;
		d.plen = 0;
	}
	if (has_gw && g.fam != d.fam) return ENT_FAIL("address family mismatch in route");
	if (e->name[0] && type == RTN_UNICAST && (oif = ent_idx(e->name)) < 0) return ent_fail(oif, "no such interface '%s'", e->name);
	ent_req_init(&q, op, NLM_F_ACK | (op == RTM_NEWROUTE ? NLM_F_CREATE | NLM_F_REPLACE : 0), sizeof(struct rtmsg));
	q.m.r.rtm_family = d.fam;
	q.m.r.rtm_dst_len = d.plen;
	q.m.r.rtm_protocol = del ? 0 : RTPROT_STATIC;
	q.m.r.rtm_scope = del ? RT_SCOPE_NOWHERE : (type != RTN_UNICAST || has_gw) ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
	q.m.r.rtm_type = del ? 0 : type;
	if (onlink) q.m.r.rtm_flags |= RTNH_F_ONLINK;
	ent__rtmsg_table(&q, table);
	if (d.plen) ent_attr(&q, RTA_DST, d.b, ent_iplen(&d));
	if (e->src[0]) {
		ent_ip s;
		if (ent_parse_ip(e->src, &s, 0) < 0 || s.fam != d.fam) return ENT_FAIL("bad src '%s'", e->src);
		ent_attr(&q, RTA_PREFSRC, s.b, ent_iplen(&s));
	}
	if (has_gw) ent_attr(&q, RTA_GATEWAY, g.b, ent_iplen(&g));
	if (oif > 0) ent__a32(&q, RTA_OIF, oif);
	if (e->metric > 0) ent__a32(&q, RTA_PRIORITY, e->metric);
	return ent_nl(&q, NULL, NULL);
}
int netman_ent_route_add(const netman_ent *e)
{
	int r = ent__route(RTM_NEWROUTE, e, 0);
	if ((r == -ENETUNREACH || r == -EINVAL) && e->gateway[0] && e->name[0]) r = ent__route(RTM_NEWROUTE, e, 1); /* /32 leases etc. */
	return r < 0 && r != -EINVAL ? ent_fail(r, "route: %s", strerror(-r)) : r;
}
int netman_ent_route_del(const netman_ent *e)
{
	int r = ent__route(RTM_DELROUTE, e, 0);
	return r == -ESRCH ? 0 : r;
}

struct ent_rctx { struct ent_out o; int table, fam; };
static void ent__route_cb(const struct nlmsghdr *m, void *u)
{
	struct ent_rctx *c = u;
	struct rtmsg *r = NLMSG_DATA(m);
	struct rtattr *tb[RTA_MAX + 1];
	char b[64], n[IF_NAMESIZE];
	int tab;
	if (m->nlmsg_type != RTM_NEWROUTE) return;
	ent__rta(tb, RTA_MAX, RTM_RTA(r), RTM_PAYLOAD(m));
	tab = tb[RTA_TABLE] ? (int)ent__u(tb[RTA_TABLE]) : r->rtm_table;
	if ((c->table && tab != c->table) || (c->fam && r->rtm_family != c->fam)) return;
	if (tb[RTA_DST]) {
		inet_ntop(r->rtm_family, RTA_DATA(tb[RTA_DST]), b, sizeof b);
		ent_put(&c->o, "%s/%d", b, r->rtm_dst_len);
	} else ent_put(&c->o, "default");
	if (r->rtm_type != RTN_UNICAST) ent_put(&c->o, " type %s", r->rtm_type == RTN_BLACKHOLE ? "blackhole" : r->rtm_type == RTN_UNREACHABLE ? "unreachable" : r->rtm_type == RTN_PROHIBIT ? "prohibit" : "other");
	if (tb[RTA_MULTIPATH]) {
		struct rtnexthop *nh = RTA_DATA(tb[RTA_MULTIPATH]);
		int len = RTA_PAYLOAD(tb[RTA_MULTIPATH]);
		if (tb[RTA_PRIORITY]) ent_put(&c->o, " metric %u", ent__u(tb[RTA_PRIORITY]));
		while (len >= (int)sizeof *nh && nh->rtnh_len >= sizeof *nh && (int)nh->rtnh_len <= len) {
			struct rtattr *g[RTA_MAX + 1];
			ent__rta(g, RTA_MAX, RTNH_DATA(nh), nh->rtnh_len - sizeof *nh);
			ent_put(&c->o, " nexthop");
			if (g[RTA_GATEWAY]) {
				inet_ntop(r->rtm_family, RTA_DATA(g[RTA_GATEWAY]), b, sizeof b);
				ent_put(&c->o, " via %s", b);
			}
			ent_put(&c->o, " dev %s weight %d", if_indextoname(nh->rtnh_ifindex, n) ? n : "?", nh->rtnh_hops + 1);
			len -= RTNH_ALIGN(nh->rtnh_len);
			nh = RTNH_NEXT(nh);
		}
	} else {
		if (tb[RTA_GATEWAY]) {
			inet_ntop(r->rtm_family, RTA_DATA(tb[RTA_GATEWAY]), b, sizeof b);
			ent_put(&c->o, " via %s", b);
		}
		if (tb[RTA_OIF]) ent_put(&c->o, " dev %s", if_indextoname(ent__u(tb[RTA_OIF]), n) ? n : "?");
		if (tb[RTA_PRIORITY]) ent_put(&c->o, " metric %u", ent__u(tb[RTA_PRIORITY]));
	}
	ent_put(&c->o, " table %d\n", tab);
}
int netman_ent_route_list(int table, int family, char *out, size_t cap)
{
	struct ent_rctx c = {{out, 0, cap}, table, family};
	ent_req q;
	int r;
	if (cap) out[0] = 0;
	ent_req_init(&q, RTM_GETROUTE, NLM_F_DUMP, sizeof(struct rtmsg));
	q.m.r.rtm_family = AF_UNSPEC;
	if ((r = ent_nl(&q, ent__route_cb, &c)) < 0) return r;
	return (int)c.o.n;
}
int netman_ent_route_show(const netman_ent *e, FILE *f)
{
	char b[8192];
	int n = netman_ent_route_list(e->table > 0 ? e->table : (int)RT_TABLE_MAIN, 0, b, sizeof b);
	if (n < 0) return n;
	fputs(b, f);
	return 0;
}

static int ent__rule(int op, const netman_ent *e)
{
	ent_req q;
	struct fib_rule_hdr *h;
	ent_ip a;
	int fam = AF_INET;
	if (e->from[0] && ent_parse_ip(e->from, &a, 0) == 0) fam = a.fam;
	else if (e->to[0] && ent_parse_ip(e->to, &a, 0) == 0) fam = a.fam;
	if (e->table <= 0) return ENT_FAIL("rule needs table=");
	ent_req_init(&q, op, NLM_F_ACK | (op == RTM_NEWRULE ? NLM_F_CREATE | NLM_F_EXCL : 0), sizeof(struct fib_rule_hdr));
	h = (struct fib_rule_hdr *)NLMSG_DATA(&q.h);
	h->family = fam;
	h->action = FR_ACT_TO_TBL;
	if (e->from[0]) {
		if (ent_parse_ip(e->from, &a, 0) < 0 || a.fam != fam) return ENT_FAIL("bad from '%s'", e->from);
		h->src_len = a.plen < 0 ? (fam == AF_INET ? 32 : 128) : a.plen;
		ent_attr(&q, FRA_SRC, a.b, ent_iplen(&a));
	}
	if (e->to[0]) {
		if (ent_parse_ip(e->to, &a, 0) < 0 || a.fam != fam) return ENT_FAIL("bad to '%s'", e->to);
		h->dst_len = a.plen < 0 ? (fam == AF_INET ? 32 : 128) : a.plen;
		ent_attr(&q, FRA_DST, a.b, ent_iplen(&a));
	}
	if (e->iif[0]) ent__astr(&q, FRA_IIFNAME, e->iif);
	if (e->oif[0]) ent__astr(&q, FRA_OIFNAME, e->oif);
	if (e->fwmark) {
		ent__a32(&q, FRA_FWMARK, e->fwmark);
		ent__a32(&q, FRA_FWMASK, 0xffffffffu);
	}
	if (e->prio > 0) ent__a32(&q, FRA_PRIORITY, e->prio);
	h->table = e->table > 255 ? 0 : e->table;
	ent__a32(&q, FRA_TABLE, e->table);
	return ent_nl(&q, NULL, NULL);
}
int netman_ent_rule_add(const netman_ent *e)
{
	int r = ent__rule(RTM_NEWRULE, e);
	return r == -EEXIST ? 0 : r;
}
int netman_ent_rule_del(const netman_ent *e)
{
	int r = ent__rule(RTM_DELRULE, e);
	return r == -ENOENT ? 0 : r;
}
static void ent__rule_cb(const struct nlmsghdr *m, void *u)
{
	struct ent_rctx *c = u;
	struct fib_rule_hdr *h = NLMSG_DATA(m);
	struct rtattr *tb[FRA_MAX + 1];
	char b[64];
	if (m->nlmsg_type != RTM_NEWRULE || (c->fam && h->family != c->fam)) return;
	ent__rta(tb, FRA_MAX, (struct rtattr *)((char *)h + NLMSG_ALIGN(sizeof *h)), m->nlmsg_len - NLMSG_LENGTH(sizeof *h));
	ent_put(&c->o, "%u:", tb[FRA_PRIORITY] ? ent__u(tb[FRA_PRIORITY]) : 0);
	if (tb[FRA_SRC]) { inet_ntop(h->family, RTA_DATA(tb[FRA_SRC]), b, sizeof b); ent_put(&c->o, " from %s/%d", b, h->src_len); }
	if (tb[FRA_DST]) { inet_ntop(h->family, RTA_DATA(tb[FRA_DST]), b, sizeof b); ent_put(&c->o, " to %s/%d", b, h->dst_len); }
	if (tb[FRA_FWMARK]) ent_put(&c->o, " fwmark 0x%x", ent__u(tb[FRA_FWMARK]));
	if (tb[FRA_IIFNAME]) ent_put(&c->o, " iif %s", (char *)RTA_DATA(tb[FRA_IIFNAME]));
	if (tb[FRA_OIFNAME]) ent_put(&c->o, " oif %s", (char *)RTA_DATA(tb[FRA_OIFNAME]));
	ent_put(&c->o, " lookup %u\n", tb[FRA_TABLE] ? ent__u(tb[FRA_TABLE]) : h->table);
}
int netman_ent_rule_list(int family, char *out, size_t cap)
{
	struct ent_rctx c = {{out, 0, cap}, 0, family};
	ent_req q;
	int r;
	if (cap) out[0] = 0;
	ent_req_init(&q, RTM_GETRULE, NLM_F_DUMP, sizeof(struct fib_rule_hdr));
	((struct fib_rule_hdr *)NLMSG_DATA(&q.h))->family = family;
	if ((r = ent_nl(&q, ent__rule_cb, &c)) < 0) return r;
	return (int)c.o.n;
}
int netman_ent_rule_show(const netman_ent *e, FILE *f)
{
	char b[8192];
	int n = netman_ent_rule_list(0, b, sizeof b);
	(void)e;
	if (n < 0) return n;
	fputs(b, f);
	return 0;
}

/* ======================================================================== ECMP multi-uplink with failover */
static uint16_t ent__cksum(const void *p, int n)
{
	const uint8_t *b = p;
	uint32_t s = 0;
	for (; n > 1; n -= 2, b += 2) s += (b[0] << 8) | b[1];
	if (n) s += b[0] << 8;
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return htons(~s);
}
static int ent__icmp_probe(const char *ifn, const char *target, int timeout_ms)
{
	struct sockaddr_in to = {.sin_family = AF_INET};
	struct { struct icmphdr h; char pad[16]; } pkt;
	static unsigned short seq;
	long end;
	int fd, ok = -ETIMEDOUT;
	uint16_t id = (uint16_t)getpid();
	if (inet_pton(AF_INET, target, &to.sin_addr) != 1) return -EINVAL;
	if ((fd = socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMP)) < 0) return -errno;
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifn, strlen(ifn) + 1) < 0) { ok = -errno; close(fd); return ok; }
	memset(&pkt, 0, sizeof pkt);
	pkt.h.type = ICMP_ECHO;
	pkt.h.un.echo.id = htons(id);
	pkt.h.un.echo.sequence = htons(++seq);
	pkt.h.checksum = ent__cksum(&pkt, sizeof pkt);
	if (sendto(fd, &pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) < 0) { ok = -errno; close(fd); return ok; }
	end = ent_ms() + timeout_ms;
	for (;;) {
		struct pollfd p = {fd, POLLIN, 0};
		unsigned char buf[256];
		long left = end - ent_ms();
		ssize_t n;
		if (left <= 0 || poll(&p, 1, (int)left) <= 0) break;
		n = recv(fd, buf, sizeof buf, 0);
		if (n >= 28) {
			struct iphdr *ip = (struct iphdr *)buf;
			struct icmphdr *ic = (struct icmphdr *)(buf + ip->ihl * 4);
			if (ip->saddr == to.sin_addr.s_addr && ic->type == ICMP_ECHOREPLY && ic->un.echo.id == htons(id) && ic->un.echo.sequence == htons(seq)) { ok = 0; break; }
		}
	}
	close(fd);
	return ok;
}

static int ent__ecmp_install(const netman_ent *e)
{
	ent_req q;
	ent_ip g[NETMAN_ENT_UPLINKS];
	int ix[NETMAN_ENT_UPLINKS], n = 0, i, table = e->table > 0 ? e->table : (int)RT_TABLE_MAIN, metric = e->metric > 0 ? e->metric : 50, fam = 0, r, mp;
	for (i = 0; i < e->nup; i++) {
		if (!e->up[i].up) continue;
		if (ent_parse_ip(e->up[i].gw, &g[n], 0) < 0) return ENT_FAIL("bad uplink gateway '%s'", e->up[i].gw);
		if (fam && g[n].fam != fam) return ENT_FAIL("uplinks must share one address family");
		if (ent_idx(e->up[i].ifn) < 0) continue;
		fam = g[n].fam;
		ix[n++] = i;
	}
	ent_req_init(&q, n ? RTM_NEWROUTE : RTM_DELROUTE, NLM_F_ACK | (n ? NLM_F_CREATE | NLM_F_REPLACE : 0), sizeof(struct rtmsg));
	q.m.r.rtm_family = fam ? fam : AF_INET;
	q.m.r.rtm_protocol = n ? RTPROT_STATIC : 0;
	q.m.r.rtm_scope = n ? RT_SCOPE_UNIVERSE : RT_SCOPE_NOWHERE;
	q.m.r.rtm_type = n ? RTN_UNICAST : 0;
	ent__rtmsg_table(&q, table);
	ent__a32(&q, RTA_PRIORITY, metric);
	if (n == 1) {
		ent_attr(&q, RTA_GATEWAY, g[0].b, ent_iplen(&g[0]));
		ent__a32(&q, RTA_OIF, ent_idx(e->up[ix[0]].ifn));
	} else if (n > 1) {
		mp = ent__nest(&q, RTA_MULTIPATH);
		for (i = 0; i < n; i++) {
			int start = NLMSG_ALIGN(q.h.nlmsg_len);
			struct rtnexthop *nh = (struct rtnexthop *)((char *)&q + start);
			int w = e->up[ix[i]].weight > 0 ? e->up[ix[i]].weight : 1;
			if ((size_t)start + 64 > sizeof q) return ent_fail(-E2BIG, "too many uplinks");
			memset(nh, 0, sizeof *nh);
			nh->rtnh_hops = w - 1;
			nh->rtnh_ifindex = ent_idx(e->up[ix[i]].ifn);
			q.h.nlmsg_len = start + sizeof *nh;
			ent_attr(&q, RTA_GATEWAY, g[i].b, ent_iplen(&g[i]));
			nh->rtnh_len = q.h.nlmsg_len - start;
		}
		ent__end(&q, mp);
	}
	r = ent_nl(&q, NULL, NULL);
	if (!n && (r == -ESRCH || r == -ENOENT)) r = 0;
	if (r < 0) return ent_fail(r, "default route: %s", strerror(-r));
	return n;
}
static void ent__ecmp_defaults(netman_ent *e)
{
	int i;
	if (e->probe_ms <= 0) e->probe_ms = 1000;
	if (e->fail_after <= 0) e->fail_after = 3;
	if (e->ok_after <= 0) e->ok_after = 2;
	for (i = 0; i < e->nup; i++)
		if (e->up[i].weight <= 0) e->up[i].weight = 1;
}
int netman_ent_ecmp_tick(netman_ent *e, int force)
{
	int i, changed = 0, r;
	ent__ecmp_defaults(e);
	for (i = 0; i < e->nup; i++) {
		netman_ent_uplink *u = &e->up[i];
		netman_if f;
		int alive = netman_find(u->ifn, &f) == 0 && f.up && f.carrier;
		if (alive) alive = netman_ent_probe_fn(u->ifn, u->probe[0] ? u->probe : u->gw, e->probe_ms) == 0;
		if (alive) {
			u->fails = 0;
			if (++u->oks >= e->ok_after && !u->up) u->up = 1, changed++;
		} else {
			u->oks = 0;
			if (++u->fails >= e->fail_after && u->up) u->up = 0, changed++;
		}
		if (u->fails > 1000) u->fails = 1000;
		if (u->oks > 1000) u->oks = 1000;
	}
	if (changed || force) {
		if ((r = ent__ecmp_install(e)) < 0) return r;
	}
	return changed;
}
int netman_ent_ecmp_run(netman_ent *e, int interval_ms, volatile sig_atomic_t *stop)
{
	int r = netman_ent_ecmp_tick(e, 1);
	if (r < 0) return r;
	while (!*stop) {
		struct timespec ts = {interval_ms / 1000, (interval_ms % 1000) * 1000000L};
		nanosleep(&ts, NULL);
		if (netman_ent_ecmp_tick(e, 0) < 0) fprintf(stderr, "netman: ecmp: %s\n", netman_ent_error());
	}
	return 0;
}
int netman_ent_ecmp_add(const netman_ent *e)
{
	netman_ent tmp = *e;
	int i, r;
	if ((r = netman_ent_check(e)) < 0) return r;
	if (!tmp.nup) return ENT_FAIL("ecmp needs at least one uplink=");
	ent__ecmp_defaults(&tmp);
	for (i = 0; i < tmp.nup; i++) tmp.up[i].up = 1; /* optimistic first install; the first ticks correct it */
	return ent__ecmp_install(&tmp) < 0 ? -EINVAL : 0;
}
int netman_ent_ecmp_del(const netman_ent *e)
{
	netman_ent tmp = *e;
	int i;
	for (i = 0; i < tmp.nup; i++) tmp.up[i].up = 0;
	return ent__ecmp_install(&tmp) < 0 ? -EINVAL : 0;
}
int netman_ent_ecmp_show(const netman_ent *e, FILE *f)
{
	char b[4096];
	int i;
	for (i = 0; i < e->nup; i++)
		fprintf(f, "uplink %s via %s probe %s weight %d: %s\n", e->up[i].ifn, e->up[i].gw, e->up[i].probe[0] ? e->up[i].probe : e->up[i].gw, e->up[i].weight, e->up[i].up ? "up" : "DOWN");
	if (netman_ent_route_list(e->table > 0 ? e->table : (int)RT_TABLE_MAIN, 0, b, sizeof b) > 0) fputs(b, f);
	return 0;
}

/* ======================================================================== link settings (MTU, offload, ethtool), Wake-on-LAN */
static int ent__ethtool(const char *ifn, void *data)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), r = 0;
	if (fd < 0) return -errno;
	memset(&ifr, 0, sizeof ifr);
	ent_cpy(ifr.ifr_name, sizeof ifr.ifr_name, ifn);
	ifr.ifr_data = data;
	if (ioctl(fd, SIOCETHTOOL, &ifr) < 0) r = -errno;
	close(fd);
	return r;
}
static int ent__etv(const char *ifn, unsigned cmd, int *val, int set)
{
	struct ethtool_value v = {cmd, set ? (unsigned)*val : 0};
	int r = ent__ethtool(ifn, &v);
	if (!set && r == 0) *val = v.data;
	return r;
}
int netman_ent_link_add(const netman_ent *e)
{
	static const struct { const char *n; unsigned set; size_t off; } of[] = {
		{"tso", ETHTOOL_STSO, offsetof(netman_ent, tso)}, {"gso", ETHTOOL_SGSO, offsetof(netman_ent, gso)},
		{"gro", ETHTOOL_SGRO, offsetof(netman_ent, gro)}, {"sg", ETHTOOL_SSG, offsetof(netman_ent, sg)},
		{"rxcsum", ETHTOOL_SRXCSUM, offsetof(netman_ent, rxcsum)}, {"txcsum", ETHTOOL_STXCSUM, offsetof(netman_ent, txcsum)}};
	size_t i;
	int r, bad = 0;
	if ((r = netman_ent_check(e)) < 0) return r;
	if (ent_idx(e->name) < 0) return ent_fail(-ENODEV, "no such interface '%s'", e->name);
	if (e->mtu > 0 && (r = ent__setmtu(e->name, e->mtu)) < 0) return ent_fail(r, "mtu %d on %s: %s", e->mtu, e->name, strerror(-r));
	for (i = 0; i < sizeof of / sizeof *of; i++) {
		int v = *(const int *)((const char *)e + of[i].off);
		if (v >= 0 && (r = ent__etv(e->name, of[i].set, &v, 1)) < 0) bad = ent_fail(r, "%s on %s: %s", of[i].n, e->name, strerror(-r));
	}
	if (e->speed > 0 || e->duplex[0] || e->autoneg >= 0) {
		struct ethtool_cmd c = {.cmd = ETHTOOL_GSET};
		if ((r = ent__ethtool(e->name, &c)) < 0) return ent_fail(r, "ethtool settings on %s: %s", e->name, strerror(-r));
		c.cmd = ETHTOOL_SSET;
		if (e->autoneg >= 0) c.autoneg = e->autoneg;
		if (e->speed > 0) ethtool_cmd_speed_set(&c, e->speed);
		if (e->duplex[0]) c.duplex = !strcmp(e->duplex, "full") ? DUPLEX_FULL : DUPLEX_HALF;
		if ((r = ent__ethtool(e->name, &c)) < 0) return ent_fail(r, "ethtool settings on %s: %s", e->name, strerror(-r));
	}
	return bad;
}
int netman_ent_link_del(const netman_ent *e) { return e->mtu > 0 ? ent__setmtu(e->name, 1500) : 0; } /* back to the Ethernet default */
int netman_ent_link_show(const netman_ent *e, FILE *f)
{
	static const struct { const char *n; unsigned get; } of[] = {{"tso", ETHTOOL_GTSO}, {"gso", ETHTOOL_GGSO}, {"gro", ETHTOOL_GGRO}, {"sg", ETHTOOL_GSG}, {"rxcsum", ETHTOOL_GRXCSUM}, {"txcsum", ETHTOOL_GTXCSUM}};
	netman_ent_info i;
	size_t k;
	int r = netman_ent_getinfo(e->name, &i), v;
	if (r < 0) return r;
	fprintf(f, "%s mtu %d", e->name, i.mtu);
	for (k = 0; k < sizeof of / sizeof *of; k++)
		if (ent__etv(e->name, of[k].get, &v, 0) == 0) fprintf(f, " %s %s", of[k].n, v ? "on" : "off");
	fputc('\n', f);
	return 0;
}

static unsigned ent__wol_flags(const char *s)
{
	unsigned w = 0;
	for (; *s; s++)
		switch (*s) {
		case 'p': w |= WAKE_PHY; break;
		case 'u': w |= WAKE_UCAST; break;
		case 'm': w |= WAKE_MCAST; break;
		case 'b': w |= WAKE_BCAST; break;
		case 'a': w |= WAKE_ARP; break;
		case 'g': w |= WAKE_MAGIC; break;
		case 's': w |= WAKE_MAGICSECURE; break;
		}
	return w;
}
static void ent__wol_str(unsigned w, char *o)
{
	static const struct { unsigned f; char c; } t[] = {{WAKE_PHY, 'p'}, {WAKE_UCAST, 'u'}, {WAKE_MCAST, 'm'}, {WAKE_BCAST, 'b'}, {WAKE_ARP, 'a'}, {WAKE_MAGIC, 'g'}, {WAKE_MAGICSECURE, 's'}};
	size_t i;
	int n = 0;
	for (i = 0; i < sizeof t / sizeof *t; i++)
		if (w & t[i].f) o[n++] = t[i].c;
	if (!n) o[n++] = 'd';
	o[n] = 0;
}
int netman_ent_wol_add(const netman_ent *e)
{
	struct ethtool_wolinfo w = {.cmd = ETHTOOL_GWOL};
	int r;
	if ((r = netman_ent_check(e)) < 0) return r;
	if (!e->wol[0]) return ENT_FAIL("wol needs wol=<letters>");
	if ((r = ent__ethtool(e->name, &w)) < 0) return ent_fail(r, "wake-on-lan on %s: %s", e->name, r == -EOPNOTSUPP ? "driver does not support it" : strerror(-r));
	w.cmd = ETHTOOL_SWOL;
	w.wolopts = ent__wol_flags(e->wol);
	if (w.wolopts & ~w.supported) return ent_fail(-EOPNOTSUPP, "%s supports only wake modes 0x%x", e->name, w.supported);
	if ((r = ent__ethtool(e->name, &w)) < 0) return ent_fail(r, "wake-on-lan on %s: %s", e->name, strerror(-r));
	return 0;
}
int netman_ent_wol_del(const netman_ent *e)
{
	struct ethtool_wolinfo w = {.cmd = ETHTOOL_SWOL, .wolopts = 0};
	return ent__ethtool(e->name, &w);
}
int netman_ent_wol_show(const netman_ent *e, FILE *f)
{
	struct ethtool_wolinfo w = {.cmd = ETHTOOL_GWOL};
	char a[16], b[16];
	int r = ent__ethtool(e->name, &w);
	if (r < 0) return r;
	ent__wol_str(w.supported, a);
	ent__wol_str(w.wolopts, b);
	fprintf(f, "%s wake-on-lan supports %s, set %s\n", e->name, a, b);
	return 0;
}
int netman_ent_wol_packet(const unsigned char mac[6], unsigned char out[102])
{
	int i;
	memset(out, 0xff, 6);
	for (i = 0; i < 16; i++) memcpy(out + 6 + 6 * i, mac, 6);
	return 102;
}
int netman_ent_wol_send(const char *ifn, const char *macs)
{
	unsigned m[6];
	unsigned char mac[6], pkt[102];
	struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(9)};
	int fd, one = 1, i, r = 0;
	if (sscanf(macs, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) return ENT_FAIL("bad MAC '%s'", macs);
	for (i = 0; i < 6; i++) mac[i] = (unsigned char)m[i];
	netman_ent_wol_packet(mac, pkt);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	if ((fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) return -errno;
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	if (ifn && *ifn && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifn, strlen(ifn) + 1) < 0) r = -errno;
	if (!r && sendto(fd, pkt, sizeof pkt, 0, (struct sockaddr *)&to, sizeof to) < 0) r = -errno;
	close(fd);
	return r;
}

/* ======================================================================== IPv6: SLAAC / privacy / forwarding / DHCPv6-PD hooks */
static int ent__sysw(const char *ifn, const char *key, int v)
{
	char p[300], d[16];
	int fd, n = snprintf(d, sizeof d, "%d\n", v);
	snprintf(p, sizeof p, "%s/sys/net/ipv6/conf/%s/%s", netman_ent_proc, ifn, key);
	if ((fd = open(p, O_WRONLY | O_CLOEXEC)) < 0) return -errno;
	if (write(fd, d, n) != n) {
		n = -errno;
		close(fd);
		return n;
	}
	close(fd);
	return 0;
}
static int ent__sysr(const char *ifn, const char *key)
{
	char p[300], b[32];
	snprintf(p, sizeof p, "%s/sys/net/ipv6/conf/%s/%s", netman_ent_proc, ifn, key);
	return ent_slurp(p, b, sizeof b) < 0 ? -1 : atoi(b);
}
int netman_ent_pd_subnet(const char *prefix, int idx, char *cidr, size_t n)
{
	ent_ip p;
	uint64_t net = 0;
	int i, bits;
	char b[64];
	if (ent_parse_ip(prefix, &p, 1) < 0 || p.fam != AF_INET6) return ENT_FAIL("bad IPv6 prefix '%s'", prefix);
	if (p.plen > 64) return ENT_FAIL("delegated prefix longer than /64");
	bits = 64 - p.plen;
	if (idx < 0 || (bits < 31 && idx >= (1 << bits))) return ent_fail(-ERANGE, "prefix %s has no subnet number %d", prefix, idx);
	for (i = 0; i < 8; i++) net = (net << 8) | p.b[i];
	net = (net & (p.plen ? ~0ULL << bits : 0)) | (uint64_t)idx;
	for (i = 7; i >= 0; i--, net >>= 8) p.b[i] = net & 0xff;
	memset(p.b + 8, 0, 8);
	p.b[15] = 1;
	if (!inet_ntop(AF_INET6, p.b, b, sizeof b)) return -EINVAL;
	snprintf(cidr, n, "%s/64", b);
	return 0;
}
int netman_ent_pd_from_env(const char *prefixes, netman_ent *e)
{
	char t[128], *c;
	ent_ip p;
	if (!prefixes || !*prefixes) return ENT_FAIL("no prefixes");
	ent_cpy(t, sizeof t, prefixes);
	if ((c = strpbrk(t, ", "))) *c = 0; /* first entry; odhcp6c adds ",preferred,valid" */
	if (ent_parse_ip(t, &p, 1) < 0 || p.fam != AF_INET6) return ENT_FAIL("bad delegated prefix '%s'", t);
	ent_cpy(e->pd_prefix, sizeof e->pd_prefix, t);
	return 0;
}
int netman_ent_pd_helper_conf(const netman_ent *e, char *out, size_t cap)
{
	struct ent_out o = {out, 0, cap};
	int i;
	if (cap) out[0] = 0;
	ent_put(&o, ENT_HDR "# dnsmasq: router advertisements + stateless DHCPv6 for the delegated-prefix downstream ifaces\nenable-ra\n");
	for (i = 0; i < e->nmember; i++) ent_put(&o, "interface=%s\ndhcp-range=::,constructor:%s,ra-stateless,ra-names\n", e->member[i], e->member[i]);
	return o.n >= cap ? -E2BIG : (int)o.n;
}
int netman_ent_ipv6_add(const netman_ent *e)
{
	int r, i, bad = 0;
	char cidr[80];
	if ((r = netman_ent_check(e)) < 0) return r;
	if (ent_idx(e->name) < 0) return ent_fail(-ENODEV, "no such interface '%s'", e->name);
	if (e->accept_ra >= 0 && (r = ent__sysw(e->name, "accept_ra", e->accept_ra)) < 0) bad = ent_fail(r, "accept_ra: %s", strerror(-r));
	if (e->autoconf >= 0 && (r = ent__sysw(e->name, "autoconf", e->autoconf)) < 0) bad = ent_fail(r, "autoconf: %s", strerror(-r));
	if (e->tempaddr >= 0 && (r = ent__sysw(e->name, "use_tempaddr", e->tempaddr)) < 0) bad = ent_fail(r, "use_tempaddr: %s", strerror(-r));
	if (e->forwarding >= 0 && (r = ent__sysw(e->name, "forwarding", e->forwarding)) < 0) bad = ent_fail(r, "forwarding: %s", strerror(-r));
	if (e->addr[0] && (r = netman_addr_add(e->name, e->addr)) < 0) return r;
	if (e->pd_prefix[0])
		for (i = 0; i < e->nmember; i++) { /* DHCPv6-PD hook: one /64 per downstream iface, ::1 on it */
			if ((r = netman_ent_pd_subnet(e->pd_prefix, i + 1, cidr, sizeof cidr)) < 0) return r;
			if ((r = netman_addr_add(e->member[i], cidr)) < 0) return r;
		}
	return bad;
}
int netman_ent_ipv6_del(const netman_ent *e)
{
	int i;
	char cidr[80];
	ent__sysw(e->name, "accept_ra", 1);
	ent__sysw(e->name, "autoconf", 1);
	ent__sysw(e->name, "use_tempaddr", 0);
	ent__sysw(e->name, "forwarding", 0);
	if (e->addr[0]) netman_addr_del(e->name, e->addr);
	if (e->pd_prefix[0])
		for (i = 0; i < e->nmember; i++)
			if (netman_ent_pd_subnet(e->pd_prefix, i + 1, cidr, sizeof cidr) == 0) netman_addr_del(e->member[i], cidr);
	return 0;
}
int netman_ent_ipv6_show(const netman_ent *e, FILE *f)
{
	netman_if i;
	int k, r = netman_find(e->name, &i);
	if (r < 0) return r;
	fprintf(f, "%s ipv6: accept_ra %d autoconf %d use_tempaddr %d forwarding %d\n", e->name, ent__sysr(e->name, "accept_ra"), ent__sysr(e->name, "autoconf"), ent__sysr(e->name, "use_tempaddr"), ent__sysr(e->name, "forwarding"));
	for (k = 0; k < i.naddr; k++)
		if (strchr(i.addr[k], ':')) fprintf(f, "  %s\n", i.addr[k]);
	return 0;
}

/* ======================================================================== 802.1X wired */
int netman_ent_dot1x_conf(const netman_ent *e, char *out, size_t cap)
{
	struct ent_out o = {out, 0, cap};
	const char *eap = e->eap[0] ? e->eap : "PEAP";
	int tls = !strcasecmp(eap, "TLS");
	if (cap) out[0] = 0;
	if (!e->identity[0]) return ENT_FAIL("dot1x needs identity=");
	if (tls ? (!e->client_cert[0] || !e->private_key[0]) : !e->password[0]) return ENT_FAIL(tls ? "EAP-TLS needs client_cert= and private_key=" : "dot1x needs password=");
	ent_put(&o, ENT_HDR "ctrl_interface=/run/wpa_supplicant\nap_scan=0\neapol_version=2\nnetwork={\n\tkey_mgmt=IEEE8021X\n\teap=%s\n\teapol_flags=0\n\tidentity=\"%s\"\n", eap, e->identity);
	if (e->anon_identity[0]) ent_put(&o, "\tanonymous_identity=\"%s\"\n", e->anon_identity);
	if (!tls) ent_put(&o, "\tpassword=\"%s\"\n", e->password);
	if (e->ca_cert[0]) ent_put(&o, "\tca_cert=\"%s\"\n", e->ca_cert);
	if (tls) ent_put(&o, "\tclient_cert=\"%s\"\n\tprivate_key=\"%s\"\n", e->client_cert, e->private_key);
	if (e->password[0] && tls) ent_put(&o, "\tprivate_key_passwd=\"%s\"\n", e->password);
	ent_put(&o, !tls && e->phase2[0] ? "\tphase2=\"%s\"\n" : !tls ? "\tphase2=\"%s\"\n" : "%s", !tls && e->phase2[0] ? e->phase2 : !tls ? "auth=MSCHAPV2" : "");
	ent_put(&o, "}\n");
	return o.n >= cap ? -E2BIG : (int)o.n;
}
int netman_ent_dot1x_add(const netman_ent *e)
{
	char conf[1024], path[300], pid[300], err[256];
	char *argv[] = {"wpa_supplicant", "-B", "-D", "wired", "-i", (char *)e->name, "-c", path, "-P", pid, NULL};
	int r;
	if ((r = netman_ent_check(e)) < 0) return r;
	if ((r = netman_ent_dot1x_conf(e, conf, sizeof conf)) < 0) return r;
	if ((r = ent_mkdirs(netman_run_dir)) < 0) return r;
	snprintf(path, sizeof path, "%s/dot1x-%s.conf", netman_run_dir, e->name);
	snprintf(pid, sizeof pid, "%s/dot1x-%s.pid", netman_run_dir, e->name);
	if ((r = ent_write_atomic(path, conf, strlen(conf), 0600)) < 0) return r; /* holds the password: root only */
	netman_link_set(e->name, 1);
	if ((r = ent_exec(argv, NULL, err, sizeof err)) < 0) return ent_fail(r, "wpa_supplicant (optional helper) failed on %s: %s", e->name, r == -ENOENT ? "not installed" : err);
	return 0;
}
int netman_ent_dot1x_del(const netman_ent *e)
{
	char p[300];
	snprintf(p, sizeof p, "%s/dot1x-%s.pid", netman_run_dir, e->name);
	ent__stop(p);
	snprintf(p, sizeof p, "%s/dot1x-%s.conf", netman_run_dir, e->name);
	unlink(p);
	return 0;
}
int netman_ent_dot1x_show(const netman_ent *e, FILE *f)
{
	char p[300];
	pid_t pid;
	snprintf(p, sizeof p, "%s/dot1x-%s.pid", netman_run_dir, e->name);
	pid = ent__readpid(p);
	fprintf(f, "%s 802.1X wired: supplicant %s\n", e->name, pid > 1 && kill(pid, 0) == 0 ? "running" : "not running");
	return 0;
}

/* ======================================================================== dispatch */
#define ENT_SWITCH(fn, ...)                                                          \
	do {                                                                         \
		const char *k = e->kind;                                             \
		if (!strcmp(k, "bond")) return netman_ent_bond_##fn(__VA_ARGS__);    \
		if (!strcmp(k, "team")) return netman_ent_team_##fn(__VA_ARGS__);    \
		if (!strcmp(k, "bridge")) return netman_ent_bridge_##fn(__VA_ARGS__);\
		if (!strcmp(k, "vlan")) return netman_ent_vlan_##fn(__VA_ARGS__);    \
		if (!strcmp(k, "macvlan")) return netman_ent_macvlan_##fn(__VA_ARGS__);\
		if (!strcmp(k, "ipvlan")) return netman_ent_ipvlan_##fn(__VA_ARGS__);\
		if (!strcmp(k, "veth")) return netman_ent_veth_##fn(__VA_ARGS__);    \
		if (!strcmp(k, "dummy")) return netman_ent_dummy_##fn(__VA_ARGS__);  \
		if (!strcmp(k, "tether")) return netman_ent_tether_##fn(__VA_ARGS__);\
		if (!strcmp(k, "dot1x")) return netman_ent_dot1x_##fn(__VA_ARGS__);  \
		if (!strcmp(k, "route")) return netman_ent_route_##fn(__VA_ARGS__);  \
		if (!strcmp(k, "rule")) return netman_ent_rule_##fn(__VA_ARGS__);    \
		if (!strcmp(k, "ecmp")) return netman_ent_ecmp_##fn(__VA_ARGS__);    \
		if (!strcmp(k, "link")) return netman_ent_link_##fn(__VA_ARGS__);    \
		if (!strcmp(k, "ipv6")) return netman_ent_ipv6_##fn(__VA_ARGS__);    \
		if (!strcmp(k, "wol")) return netman_ent_wol_##fn(__VA_ARGS__);      \
		return ENT_FAIL("unknown kind '%s'", k);                             \
	} while (0)
int netman_ent_add(const netman_ent *e) { ENT_SWITCH(add, e); }
int netman_ent_del(const netman_ent *e) { ENT_SWITCH(del, e); }
int netman_ent_show(const netman_ent *e, FILE *f) { ENT_SWITCH(show, e, f); }
