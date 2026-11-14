/* net.h - interfaces, addresses, routes, DHCPv4, DNS, profiles. stb-style single header.
 *
 *   #define NETMAN_NET_IMPLEMENTATION   in exactly one .c file, then #include "net.h"
 *
 * No D-Bus, no Python, no libraries: rtnetlink + a small DHCPv4 client.
 *   interfaces   netman_list netman_find netman_print
 *   link/addr    netman_link_set netman_addr_add/del/flush netman_route_add/del   (v4 and v6)
 *   dhcp         netman_dhcp_acquire/renew/reboot/release, netman_lease_apply/unapply
 *   dns          netman_dns_set/clear: /etc/resolv.conf (atomic) or systemd-resolved
 *   profiles     /etc/netman/NAME.conf: netman_profile_load/save/list  (apply/autoconnect/cache: roaming.h)
 *
 * All functions return 0 / count on success and a negative errno on failure
 * (NETMAN_E_NAK for a DHCPNAK); netman_error() holds a text for the last parse error.
 * Needs CAP_NET_ADMIN (+ CAP_NET_RAW, port 68) for anything that changes state;
 * listing works unprivileged. Not thread-safe (static error buffer).
 *
 * Not done: ARP conflict probe of a DHCP lease, DHCPv6/SLAAC management, link-local
 * fallback, bonding/bridge/vlan, hostname option.
 */
#ifndef NETMAN_NET_H
#define NETMAN_NET_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define NETMAN_MAXIF 32
#define NETMAN_MAXADDR 8
#define NETMAN_MAXROUTE 8
#define NETMAN_E_NAK (-1000)

typedef struct {
	char name[16];
	int idx, up, carrier, mtu;
	unsigned char mac[6];
	int naddr;
	char addr[NETMAN_MAXADDR][64]; /* "10.0.0.5/24", "fe80::1/64" */
	char gw4[40], gw6[40];	     /* default gateways, "" if none */
	int nroute;
	char route[NETMAN_MAXROUTE][96]; /* non-kernel, non-default: "10.1.0.0/16 via 10.0.0.1" */
} netman_if;

typedef struct {
	char ifn[16];
	unsigned char mac[6];
	uint32_t ip, mask, router, server, dns[4]; /* network byte order */
	int ndns;
	uint32_t lease_s, t1, t2; /* seconds; lease_s 0xffffffff = infinite */
	time_t got;		  /* CLOCK_MONOTONIC seconds when the lease (re)started */
	char domain[64];
} netman_lease;

typedef struct {
	char name[64], iface[16];
	int dhcp;	  /* method=dhcp (1) or static (0) */
	char addr[64];	  /* static: "ip/prefix" */
	char gateway[46]; /* static */
	char dns[3][46];
	int ndns;
	char route[NETMAN_MAXROUTE][96]; /* "dst/len via gw" */
	int nroute;
	int autoconnect;
	int priority; /* higher wins when several profiles want the same interface */
	int randmac;  /* stable random (locally administered) MAC per profile */
} netman_profile;

/* tunables (defined in the implementation) */
extern const char *netman_run_dir;     /* "/run/netman"  dns state */
extern const char *netman_resolv_path; /* "/etc/resolv.conf" */
extern const char *netman_conf_dir;    /* "/etc/netman" */
extern int netman_dns_mode;	     /* 0 auto, 1 resolv.conf only, 2 systemd-resolved only */
extern const char *netman_state_dir;   /* "/var/lib/netman" lease cache + mac secret */
extern int netman_dhcp_rto_ms;	     /* first retransmit, doubles (default 1000) */
extern int netman_dhcp_timeout_ms;     /* full-DISCOVER budget (default 6000) */
extern int netman_reboot_timeout_ms;   /* cached-lease INIT-REBOOT budget (default 1200) */

const char *netman_error(void);

int netman_list(netman_if *out, int max);
int netman_find(const char *ifn, netman_if *out); /* 0 or -ENODEV */
void netman_print(FILE *f);

int netman_link_set(const char *ifn, int up);
int netman_addr_add(const char *ifn, const char *cidr); /* already present = success */
int netman_addr_del(const char *ifn, const char *cidr); /* absent = success */
int netman_addr_flush(const char *ifn);		      /* all v4 + global v6, keeps fe80:: */
/* dst "default" or "ip/len"; gw may be NULL/"" for an on-link route */
int netman_route_add(const char *ifn, const char *dst, const char *gw, int metric);
int netman_route_del(const char *ifn, const char *dst, const char *gw, int metric);

int netman_dhcp_acquire(const char *ifn, int timeout_ms, netman_lease *l);
int netman_dhcp_renew(netman_lease *l, int broadcast, int timeout_ms); /* 0 same ip, 1 ip changed */
/* INIT-REBOOT: ask for a cached address back (1 broadcast REQUEST, no DISCOVER). NETMAN_E_NAK if refused */
int netman_dhcp_reboot(const char *ifn, const netman_lease *cached, int timeout_ms, netman_lease *l);
int netman_dhcp_release(const netman_lease *l);
int netman_lease_apply(const netman_lease *l);
int netman_lease_unapply(const netman_lease *l);
void netman_lease_print(FILE *f, const netman_lease *l);

int netman_dns_set(const char *ifn, const char **ns, int n, const char *search);
int netman_dns_clear(const char *ifn);

int netman_profile_load(const char *name, netman_profile *p);
int netman_profile_save(const netman_profile *p);
int netman_profile_list(char names[][64], int max);
int netman_metric(const char *ifn);		   /* route metric: wired 100, wifi 600 */

#endif /* NETMAN_NET_H */

#ifdef NETMAN_NET_IMPLEMENTATION
#ifndef NETMAN_NET_IMPLEMENTATION_DONE
#define NETMAN_NET_IMPLEMENTATION_DONE

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/if_addr.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#ifndef IFF_LOWER_UP
#define IFF_LOWER_UP 0x10000
#endif

const char *netman_run_dir = "/run/netman";
const char *netman_resolv_path = "/etc/resolv.conf";
const char *netman_conf_dir = "/etc/netman";
int netman_dns_mode = 0;
const char *netman_state_dir = "/var/lib/netman";
int netman_dhcp_rto_ms = 1000;
int netman_dhcp_timeout_ms = 6000;
int netman_reboot_timeout_ms = 1200;

static char netman__err[256];
const char *netman_error(void) { return netman__err; }
static int netman__fail(int e, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(netman__err, sizeof netman__err, fmt, ap);
	va_end(ap);
	return e < 0 ? e : -e;
}
static void netman__cpy(char *d, size_t n, const char *s) /* truncating copy, always NUL-terminated */
{
	size_t k = s ? strlen(s) : 0;
	if (!n) return;
	if (k >= n) k = n - 1;
	if (k) memcpy(d, s, k);
	d[k] = 0;
}
static long netman__ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static time_t netman__now(void) { return netman__ms() / 1000; }

/* ---------- ip parsing ---------- */
typedef struct {
	int fam, plen; /* plen -1 = none given */
	unsigned char b[16];
} netman__ip;

static int netman__parse(const char *s, netman__ip *o, int need_plen)
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
static int netman__iplen(const netman__ip *i) { return i->fam == AF_INET ? 4 : 16; }

/* ---------- netlink ---------- */
typedef struct {
	struct nlmsghdr h;
	union {
		struct ifinfomsg i;
		struct ifaddrmsg a;
		struct rtmsg r;
	} m;
	char buf[512];
} netman__req;

static void netman__req_init(netman__req *q, int type, int flags, size_t msglen)
{
	memset(q, 0, sizeof *q);
	q->h.nlmsg_type = type;
	q->h.nlmsg_flags = NLM_F_REQUEST | flags;
	q->h.nlmsg_len = NLMSG_LENGTH(msglen);
}
static void netman__attr(netman__req *q, int type, const void *d, int len)
{
	struct rtattr *a = (struct rtattr *)((char *)q + NLMSG_ALIGN(q->h.nlmsg_len));
	if (NLMSG_ALIGN(q->h.nlmsg_len) + RTA_SPACE(len) > sizeof *q) return; /* cannot happen: attrs are tiny */
	a->rta_type = type;
	a->rta_len = RTA_LENGTH(len);
	if (len) memcpy(RTA_DATA(a), d, len);
	q->h.nlmsg_len = NLMSG_ALIGN(q->h.nlmsg_len) + RTA_ALIGN(a->rta_len);
}

typedef void (*netman__cb)(const struct nlmsghdr *, void *);
static int netman__nl(netman__req *q, netman__cb cb, void *u)
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

/* ---------- listing ---------- */
typedef struct {
	netman_if *t;
	int max, n;
} netman__lctx;

static netman_if *netman__byidx(netman__lctx *c, int idx)
{
	int i;
	for (i = 0; i < c->n; i++)
		if (c->t[i].idx == idx) return &c->t[i];
	return NULL;
}
static void netman__fmt(char *d, size_t n, int fam, const void *a, int plen)
{
	char t[INET6_ADDRSTRLEN];
	if (!inet_ntop(fam, a, t, sizeof t)) t[0] = 0;
	if (plen >= 0) snprintf(d, n, "%s/%d", t, plen);
	else snprintf(d, n, "%s", t);
}
static void netman__link_cb(const struct nlmsghdr *m, void *u)
{
	netman__lctx *c = u;
	const struct ifinfomsg *i = NLMSG_DATA(m);
	int len = IFLA_PAYLOAD(m);
	const struct rtattr *a;
	netman_if *f;
	if (m->nlmsg_type != RTM_NEWLINK || c->n >= c->max) return;
	f = &c->t[c->n++];
	memset(f, 0, sizeof *f);
	f->idx = i->ifi_index;
	f->up = !!(i->ifi_flags & IFF_UP);
	f->carrier = !!(i->ifi_flags & IFF_LOWER_UP);
	for (a = IFLA_RTA(i); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
		if (a->rta_type == IFLA_IFNAME) netman__cpy(f->name, sizeof f->name, RTA_DATA(a));
		else if (a->rta_type == IFLA_MTU && RTA_PAYLOAD(a) >= 4) memcpy(&f->mtu, RTA_DATA(a), 4);
		else if (a->rta_type == IFLA_ADDRESS && RTA_PAYLOAD(a) == 6) memcpy(f->mac, RTA_DATA(a), 6);
	}
	/* kernel fallback tunnel devices (appear in every new netns once ipip/sit/gre/ip6_tunnel/vti are loaded): never a real link, hidden while down */
	if (!f->up) {
		static const char *const fb[] = { "tunl0", "sit0", "gre0", "gretap0", "erspan0", "ip6tnl0", "ip6gre0", "ip6gretap0", "ip6erspan0", "ip_vti0", "ip6_vti0", NULL };
		for (int k = 0; fb[k]; k++) if (!strcmp(f->name, fb[k])) { c->n--; break; }
	}
}
static void netman__addr_cb(const struct nlmsghdr *m, void *u)
{
	netman__lctx *c = u;
	const struct ifaddrmsg *i = NLMSG_DATA(m);
	int len = IFA_PAYLOAD(m);
	const struct rtattr *a;
	const void *addr = NULL, *local = NULL;
	netman_if *f;
	if (m->nlmsg_type != RTM_NEWADDR) return;
	if (!(f = netman__byidx(c, i->ifa_index)) || f->naddr >= NETMAN_MAXADDR) return;
	for (a = IFA_RTA(i); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
		if (a->rta_type == IFA_ADDRESS) addr = RTA_DATA(a);
		else if (a->rta_type == IFA_LOCAL) local = RTA_DATA(a);
	}
	if (!(local ? local : addr)) return;
	netman__fmt(f->addr[f->naddr++], 64, i->ifa_family, local ? local : addr, i->ifa_prefixlen);
}
static void netman__route_cb(const struct nlmsghdr *m, void *u)
{
	netman__lctx *c = u;
	const struct rtmsg *r = NLMSG_DATA(m);
	int len = RTM_PAYLOAD(m), oif = 0;
	const struct rtattr *a;
	const void *dst = NULL, *gw = NULL;
	netman_if *f;
	char d[64], g[48];
	if (m->nlmsg_type != RTM_NEWROUTE || r->rtm_table != RT_TABLE_MAIN || r->rtm_type != RTN_UNICAST) return;
	for (a = RTM_RTA(r); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
		if (a->rta_type == RTA_DST) dst = RTA_DATA(a);
		else if (a->rta_type == RTA_GATEWAY) gw = RTA_DATA(a);
		else if (a->rta_type == RTA_OIF && RTA_PAYLOAD(a) >= 4) memcpy(&oif, RTA_DATA(a), 4);
	}
	if (!(f = netman__byidx(c, oif))) return;
	if (r->rtm_dst_len == 0) {
		if (gw) netman__fmt(r->rtm_family == AF_INET ? f->gw4 : f->gw6, 40, r->rtm_family, gw, -1);
		return;
	}
	if (r->rtm_protocol == RTPROT_KERNEL || f->nroute >= NETMAN_MAXROUTE) return;
	netman__fmt(d, sizeof d, r->rtm_family, dst ? dst : "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", r->rtm_dst_len);
	if (gw) {
		netman__fmt(g, sizeof g, r->rtm_family, gw, -1);
		snprintf(f->route[f->nroute++], 96, "%.45s via %.44s", d, g);
	} else snprintf(f->route[f->nroute++], 96, "%s", d);
}

int netman_list(netman_if *out, int max)
{
	netman__lctx c = {out, max, 0};
	netman__req q;
	int r;
	netman__req_init(&q, RTM_GETLINK, NLM_F_DUMP, sizeof(struct ifinfomsg));
	if ((r = netman__nl(&q, netman__link_cb, &c)) < 0) return r;
	netman__req_init(&q, RTM_GETADDR, NLM_F_DUMP, sizeof(struct ifaddrmsg));
	if ((r = netman__nl(&q, netman__addr_cb, &c)) < 0) return r;
	netman__req_init(&q, RTM_GETROUTE, NLM_F_DUMP, sizeof(struct rtmsg));
	if ((r = netman__nl(&q, netman__route_cb, &c)) < 0) return r;
	return c.n;
}
int netman_find(const char *ifn, netman_if *out)
{
	netman_if t[NETMAN_MAXIF];
	int n = netman_list(t, NETMAN_MAXIF), i;
	if (n < 0) return n;
	for (i = 0; i < n; i++)
		if (!strcmp(t[i].name, ifn)) {
			*out = t[i];
			return 0;
		}
	return -ENODEV;
}
void netman_print(FILE *f)
{
	netman_if t[NETMAN_MAXIF];
	int n = netman_list(t, NETMAN_MAXIF), i, j;
	for (i = 0; i < n; i++) {
		netman_if *x = &t[i];
		fprintf(f, "%-12s %-4s %-10s %02x:%02x:%02x:%02x:%02x:%02x mtu %d\n", x->name, x->up ? "UP" : "DOWN",
			x->carrier ? "carrier" : "no-carrier", x->mac[0], x->mac[1], x->mac[2], x->mac[3], x->mac[4],
			x->mac[5], x->mtu);
		for (j = 0; j < x->naddr; j++) fprintf(f, "  addr  %s\n", x->addr[j]);
		if (*x->gw4) fprintf(f, "  gw    %s\n", x->gw4);
		if (*x->gw6) fprintf(f, "  gw    %s\n", x->gw6);
		for (j = 0; j < x->nroute; j++) fprintf(f, "  route %s\n", x->route[j]);
	}
}

/* ---------- link / address / route ---------- */
static int netman__idx(const char *ifn)
{
	unsigned i = ifn && *ifn ? if_nametoindex(ifn) : 0;
	return i ? (int)i : -ENODEV;
}
int netman_link_set(const char *ifn, int up)
{
	netman__req q;
	int i = netman__idx(ifn);
	if (i < 0) return i;
	netman__req_init(&q, RTM_NEWLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	q.m.i.ifi_index = i;
	q.m.i.ifi_change = IFF_UP;
	q.m.i.ifi_flags = up ? IFF_UP : 0;
	return netman__nl(&q, NULL, NULL);
}
static int netman__addr(int type, int flags, const char *ifn, const char *cidr)
{
	netman__req q;
	netman__ip a;
	int i = netman__idx(ifn), r;
	if (i < 0) return i;
	if (netman__parse(cidr, &a, 1) < 0) return netman__fail(-EINVAL, "bad address '%s' (want ip/prefix)", cidr);
	netman__req_init(&q, type, NLM_F_ACK | flags, sizeof(struct ifaddrmsg));
	q.m.a.ifa_family = a.fam;
	q.m.a.ifa_prefixlen = a.plen;
	q.m.a.ifa_index = i;
	netman__attr(&q, IFA_LOCAL, a.b, netman__iplen(&a));
	netman__attr(&q, IFA_ADDRESS, a.b, netman__iplen(&a));
	if (a.fam == AF_INET && type == RTM_NEWADDR) {
		uint32_t ip, m = a.plen ? htonl(~0u << (32 - a.plen)) : 0, b;
		memcpy(&ip, a.b, 4);
		b = ip | ~m;
		netman__attr(&q, IFA_BROADCAST, &b, 4);
	}
	r = netman__nl(&q, NULL, NULL);
	if (type == RTM_NEWADDR && r == -EEXIST) r = 0;
	if (type == RTM_DELADDR && r == -EADDRNOTAVAIL) r = 0;
	return r;
}
int netman_addr_add(const char *ifn, const char *cidr) { return netman__addr(RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, ifn, cidr); }
int netman_addr_del(const char *ifn, const char *cidr) { return netman__addr(RTM_DELADDR, 0, ifn, cidr); }
int netman_addr_flush(const char *ifn)
{
	netman_if f;
	int r = netman_find(ifn, &f), i, rc = 0;
	if (r < 0) return r;
	for (i = 0; i < f.naddr; i++) {
		if (!strncmp(f.addr[i], "fe80:", 5)) continue;
		if ((r = netman_addr_del(ifn, f.addr[i])) < 0) rc = r;
	}
	return rc;
}
static int netman__route(int type, const char *ifn, const char *dst, const char *gw, int metric, int onlink)
{
	netman__req q;
	netman__ip d, g;
	int i = netman__idx(ifn), has_gw = gw && *gw, deflt = !dst || !strcmp(dst, "default");
	if (i < 0) return i;
	memset(&d, 0, sizeof d);
	if (has_gw && netman__parse(gw, &g, 0) < 0) return netman__fail(-EINVAL, "bad gateway '%s'", gw);
	if (!deflt && netman__parse(dst, &d, 1) < 0) return netman__fail(-EINVAL, "bad route '%s' (want ip/len or default)", dst);
	if (deflt) {
		if (!has_gw) return netman__fail(-EINVAL, "default route needs a gateway");
		d.fam = g.fam;
		d.plen = 0;
	}
	if (has_gw && g.fam != d.fam) return netman__fail(-EINVAL, "address family mismatch in route");
	netman__req_init(&q, type, NLM_F_ACK | (type == RTM_NEWROUTE ? NLM_F_CREATE | NLM_F_REPLACE : 0), sizeof(struct rtmsg));
	q.m.r.rtm_family = d.fam;
	q.m.r.rtm_dst_len = d.plen;
	q.m.r.rtm_table = RT_TABLE_MAIN;
	q.m.r.rtm_protocol = RTPROT_BOOT;
	q.m.r.rtm_scope = has_gw ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
	q.m.r.rtm_type = RTN_UNICAST;
	if (onlink) q.m.r.rtm_flags |= RTNH_F_ONLINK;
	if (d.plen) netman__attr(&q, RTA_DST, d.b, netman__iplen(&d));
	if (has_gw) netman__attr(&q, RTA_GATEWAY, g.b, netman__iplen(&g));
	netman__attr(&q, RTA_OIF, &i, 4);
	if (metric > 0) netman__attr(&q, RTA_PRIORITY, &metric, 4);
	return netman__nl(&q, NULL, NULL);
}
int netman_route_add(const char *ifn, const char *dst, const char *gw, int metric)
{
	int r = netman__route(RTM_NEWROUTE, ifn, dst, gw, metric, 0);
	/* gateway outside the interface subnet (common with /32 DHCP leases): retry on-link */
	if ((r == -ENETUNREACH || r == -EINVAL) && gw && *gw) r = netman__route(RTM_NEWROUTE, ifn, dst, gw, metric, 1);
	return r;
}
int netman_route_del(const char *ifn, const char *dst, const char *gw, int metric)
{
	int r = netman__route(RTM_DELROUTE, ifn, dst, gw, metric, 0);
	return r == -ESRCH ? 0 : r;
}

/* ---------- small file helpers ---------- */
static int netman__mkdirs(const char *p)
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
/* temp file in the same directory, fsync, rename: readers never see a half-written file */
static int netman__write_atomic(const char *path, const char *data, size_t len, mode_t mode)
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
fail:
	{
		int e = errno;
		close(fd);
		unlink(t);
		return -e;
	}
}
static int netman__slurp(const char *path, char *buf, size_t n)
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
static int netman__safe_name(const char *s, size_t max)
{
	size_t i, n = s ? strlen(s) : 0;
	if (!n || n >= max || s[0] == '.') return 0;
	for (i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-' || s[i] == '.'))
			return 0;
	return 1;
}

/* ---------- dns ---------- */
#define NETMAN_HDR "# generated by netman - do not edit\n"

static int netman__use_resolved(void)
{
	if (netman_dns_mode == 1) return 0;
	if (netman_dns_mode == 2) return 1;
	if (access("/run/systemd/resolve/io.systemd.Resolve", F_OK) != 0) return 0;
	{ /* resolvectl must be there too, else we cannot talk to it */
		const char *p = getenv("PATH");
		char t[600];
		while (p && *p) {
			size_t n = strcspn(p, ":");
			snprintf(t, sizeof t, "%.*s/resolvectl", (int)n, p);
			if (access(t, X_OK) == 0) return 1;
			p += n + (p[n] ? 1 : 0);
		}
	}
	return 0;
}
static int netman__run(char *const argv[])
{
	int st;
	pid_t p = fork();
	if (p < 0) return -errno;
	if (!p) {
		execvp(argv[0], argv);
		_exit(127);
	}
	while (waitpid(p, &st, 0) < 0)
		if (errno != EINTR) return -errno;
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -EIO;
}
static int netman__dns_rebuild(void)
{
	struct dirent **ls = NULL;
	char out[8192], path[600], orig[600], cur[8192];
	int n, i, o = 0, any = 0, r = 0;
	snprintf(orig, sizeof orig, "%s/resolv.conf.orig", netman_run_dir);
	n = scandir(netman_run_dir, &ls, NULL, alphasort);
	o += snprintf(out, sizeof out, NETMAN_HDR);
	for (i = 0; i < n; i++) {
		char t[2048], *l, *save;
		if (strncmp(ls[i]->d_name, "dns.", 4)) continue;
		snprintf(path, sizeof path, "%s/%s", netman_run_dir, ls[i]->d_name);
		if (netman__slurp(path, t, sizeof t) < 0) continue;
		for (l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
			if (!strncmp(l, "nameserver", 10)) { /* skip exact duplicate servers from other interfaces */
				char needle[300];
				snprintf(needle, sizeof needle, "\n%s\n", l);
				if (strstr(out, needle)) continue;
			}
			if (o + (int)strlen(l) + 2 < (int)sizeof out) o += snprintf(out + o, sizeof out - o, "%s\n", l);
			any = 1;
		}
	}
	for (i = 0; i < n; i++) free(ls[i]);
	free(ls);
	if (netman__slurp(netman_resolv_path, cur, sizeof cur) < 0) cur[0] = 0;
	if (!any) { /* nothing left: give the original resolv.conf back */
		char b[8192];
		int k = netman__slurp(orig, b, sizeof b);
		if (k >= 0) {
			r = netman__write_atomic(netman_resolv_path, b, k, 0644);
			if (r == 0) unlink(orig);
		} else if (!strncmp(cur, NETMAN_HDR, strlen(NETMAN_HDR))) {
			r = netman__write_atomic(netman_resolv_path, NETMAN_HDR, strlen(NETMAN_HDR), 0644);
		}
		return r;
	}
	if (strncmp(cur, NETMAN_HDR, strlen(NETMAN_HDR)) && access(orig, F_OK) != 0 && cur[0])
		netman__write_atomic(orig, cur, strlen(cur), 0644); /* keep the user's file once */
	return netman__write_atomic(netman_resolv_path, out, o, 0644);
}
int netman_dns_set(const char *ifn, const char **ns, int n, const char *search)
{
	char body[1024], path[600];
	int i, o = 0, r;
	if (!netman__safe_name(ifn, 16)) return netman__fail(-EINVAL, "bad interface name");
	for (i = 0; i < n; i++) {
		netman__ip a;
		if (netman__parse(ns[i], &a, 0) < 0 || a.plen >= 0) return netman__fail(-EINVAL, "bad dns server '%s'", ns[i]);
	}
	if (search && *search) {
		const char *s;
		for (s = search; *s; s++)
			if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '-'))
				return netman__fail(-EINVAL, "bad search domain '%s'", search);
	}
	if (netman__use_resolved()) {
		char *argv[32];
		int k = 0;
		argv[k++] = "resolvectl";
		argv[k++] = "dns";
		argv[k++] = (char *)ifn;
		for (i = 0; i < n && k < 28; i++) argv[k++] = (char *)ns[i];
		argv[k] = NULL;
		if ((r = netman__run(argv)) < 0) return r;
		if (search && *search) {
			char *d[] = {"resolvectl", "domain", (char *)ifn, (char *)search, NULL};
			return netman__run(d);
		}
		return 0;
	}
	if ((r = netman__mkdirs(netman_run_dir)) < 0) return r;
	for (i = 0; i < n; i++) o += snprintf(body + o, sizeof body - o, "nameserver %s\n", ns[i]);
	if (search && *search) o += snprintf(body + o, sizeof body - o, "search %s\n", search);
	snprintf(path, sizeof path, "%s/dns.%s", netman_run_dir, ifn);
	if ((r = netman__write_atomic(path, body, o, 0644)) < 0) return r;
	return netman__dns_rebuild();
}
int netman_dns_clear(const char *ifn)
{
	char path[600];
	if (!netman__safe_name(ifn, 16)) return -EINVAL;
	if (netman__use_resolved()) {
		char *d[] = {"resolvectl", "revert", (char *)ifn, NULL};
		return netman__run(d);
	}
	snprintf(path, sizeof path, "%s/dns.%s", netman_run_dir, ifn);
	if (unlink(path) < 0 && errno == ENOENT) return 0;
	return netman__dns_rebuild();
}

/* ---------- DHCPv4 ---------- */
enum { DH_DISCOVER = 1, DH_OFFER, DH_REQUEST, DH_DECLINE, DH_ACK, DH_NAK, DH_RELEASE };

struct netman__dh {
	uint8_t op, htype, hlen, hops;
	uint32_t xid;
	uint16_t secs, flags;
	uint32_t ciaddr, yiaddr, siaddr, giaddr;
	uint8_t chaddr[16], sname[64], file[128];
	uint8_t magic[4];
	uint8_t opt[340];
} __attribute__((packed));

static uint32_t netman__xid(void)
{
	uint32_t x = 0;
	if (getrandom(&x, 4, 0) != 4) x = (uint32_t)netman__ms() * 2654435761u;
	return x;
}
static int netman__build(struct netman__dh *d, const netman_lease *l, int type, uint32_t xid, uint32_t ciaddr, uint32_t reqip,
		       uint32_t srv, int bcast)
{
	uint8_t *o = d->opt;
	static const uint8_t prl[] = {1, 3, 6, 15, 51, 54, 58, 59};
	memset(d, 0, sizeof *d);
	d->op = 1;
	d->htype = 1;
	d->hlen = 6;
	d->xid = xid;
	d->flags = bcast ? htons(0x8000) : 0;
	d->ciaddr = ciaddr;
	memcpy(d->chaddr, l->mac, 6);
	memcpy(d->magic, "\x63\x82\x53\x63", 4);
	*o++ = 53, *o++ = 1, *o++ = type;
	*o++ = 61, *o++ = 7, *o++ = 1, memcpy(o, l->mac, 6), o += 6;
	if (type != DH_RELEASE) {
		*o++ = 55, *o++ = sizeof prl, memcpy(o, prl, sizeof prl), o += sizeof prl;
		*o++ = 57, *o++ = 2, *o++ = 0x05, *o++ = 0xdc;
	}
	if (reqip) *o++ = 50, *o++ = 4, memcpy(o, &reqip, 4), o += 4;
	if (srv) *o++ = 54, *o++ = 4, memcpy(o, &srv, 4), o += 4;
	*o++ = 255;
	{
		size_t n = (size_t)(o - (uint8_t *)d);
		return n < 300 ? 300 : (int)n; /* BOOTP minimum; zero padded by memset */
	}
}
/* parse a reply; fills r. returns message type or 0 if it is not for us / malformed */
static int netman__parse_reply(const uint8_t *p, size_t len, uint32_t xid, const unsigned char *mac, netman_lease *r)
{
	const struct netman__dh *d = (const void *)p;
	size_t i = offsetof(struct netman__dh, opt);
	int type = 0;
	if (len < i + 1 || d->op != 2 || d->xid != xid || memcmp(d->chaddr, mac, 6) || memcmp(d->magic, "\x63\x82\x53\x63", 4)) return 0;
	r->ip = d->yiaddr;
	while (i < len) {
		uint8_t c = p[i++], n;
		if (c == 0) continue;
		if (c == 255) break;
		if (i >= len) return 0;
		n = p[i++];
		if (i + n > len) return 0;
		switch (c) {
		case 53:
			if (n == 1) type = p[i];
			break;
		case 1:
			if (n == 4) memcpy(&r->mask, p + i, 4);
			break;
		case 3:
			if (n >= 4) memcpy(&r->router, p + i, 4);
			break;
		case 6:
			for (r->ndns = 0; r->ndns < 4 && (r->ndns + 1) * 4 <= n; r->ndns++) memcpy(&r->dns[r->ndns], p + i + r->ndns * 4, 4);
			break;
		case 15:
			if (n < sizeof r->domain) {
				memcpy(r->domain, p + i, n);
				r->domain[n] = 0;
				while (n && r->domain[n - 1] == 0) n--; /* some servers NUL-terminate */
				r->domain[n] = 0;
			}
			break;
		case 51:
			if (n == 4) memcpy(&r->lease_s, p + i, 4), r->lease_s = ntohl(r->lease_s);
			break;
		case 54:
			if (n == 4) memcpy(&r->server, p + i, 4);
			break;
		case 58:
			if (n == 4) memcpy(&r->t1, p + i, 4), r->t1 = ntohl(r->t1);
			break;
		case 59:
			if (n == 4) memcpy(&r->t2, p + i, 4), r->t2 = ntohl(r->t2);
			break;
		}
		i += n;
	}
	return type;
}
static int netman__dsock(const char *ifn, uint32_t bindip)
{
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(68), .sin_addr.s_addr = bindip};
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), one = 1;
	if (fd < 0) return -errno;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifn, strlen(ifn) + 1) < 0 || bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	return fd;
}
static int netman__send(int fd, const void *b, int n, uint32_t dst)
{
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = dst};
	return sendto(fd, b, n, 0, (struct sockaddr *)&a, sizeof a) == n ? 0 : -errno;
}
/* DISCOVER/REQUEST before we own an address must leave with source 0.0.0.0. A UDP socket cannot do that: the
 * kernel picks some other interface's address as source. So build IP+UDP ourselves on a packet socket. */
static uint16_t netman__csum(const void *p, int n)
{
	const uint8_t *b = p;
	uint32_t s = 0;
	for (; n > 1; n -= 2, b += 2) s += (b[0] << 8) | b[1];
	if (n) s += b[0] << 8;
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return htons((uint16_t)~s);
}
static int netman__send_raw(const char *ifn, const void *dh, int n)
{
	uint8_t pkt[1500];
	struct iphdr *ip = (void *)pkt;
	struct udphdr *u = (void *)(pkt + sizeof *ip);
	struct sockaddr_ll ll = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_IP), .sll_halen = 6};
	int fd, tot = (int)(sizeof *ip + sizeof *u) + n, r = 0;
	if (tot > (int)sizeof pkt || !(ll.sll_ifindex = if_nametoindex(ifn))) return -EINVAL;
	memset(ll.sll_addr, 0xff, 6);
	memset(pkt, 0, sizeof *ip + sizeof *u);
	ip->version = 4, ip->ihl = 5, ip->tot_len = htons(tot), ip->ttl = 64, ip->protocol = IPPROTO_UDP;
	ip->daddr = htonl(INADDR_BROADCAST);
	ip->check = netman__csum(ip, sizeof *ip);
	u->source = htons(68), u->dest = htons(67), u->len = htons(sizeof *u + n); /* UDP checksum 0 = none, legal in IPv4 */
	memcpy(pkt + sizeof *ip + sizeof *u, dh, n);
	if ((fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) return -errno;
	if (sendto(fd, pkt, tot, 0, (void *)&ll, sizeof ll) != tot) r = -errno;
	close(fd);
	return r;
}
/* wait up to ms for a reply of the wanted type (or NAK); returns the type, 0 on timeout */
static int netman__rxsock(const char *ifn)
{ /* packet socket: sees the OFFER/ACK before rp_filter judges the (still unroutable) server address */
	struct sockaddr_ll ll = {.sll_family = AF_PACKET, .sll_protocol = htons(ETH_P_IP)};
	int fd;
	if (!(ll.sll_ifindex = if_nametoindex(ifn))) return -ENODEV;
	if ((fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, htons(ETH_P_IP))) < 0) return -errno;
	if (bind(fd, (void *)&ll, sizeof ll) < 0) {
		int e = errno;
		close(fd);
		return -e;
	}
	return fd;
}
static int netman__wait_(int fd, int raw, uint32_t xid, const unsigned char *mac, int want, int ms, netman_lease *r)
{
	long end = netman__ms() + ms;
	for (;;) {
		struct pollfd p = {fd, POLLIN, 0};
		uint8_t buf[1500];
		long left = end - netman__ms();
		ssize_t n;
		int t;
		if (left <= 0) return 0;
		if (poll(&p, 1, (int)left) <= 0) {
			if (errno == EINTR) continue;
			return 0;
		}
		uint8_t *pl = buf;
		struct sockaddr_ll from;
		socklen_t fl = sizeof from;
		if ((n = recvfrom(fd, buf, sizeof buf, 0, (void *)&from, &fl)) < 0) continue;
		if (raw) { /* strip IP + UDP, accept only UDP to port 68 */
			struct iphdr *ip = (void *)buf;
			size_t hl;
			if (from.sll_pkttype == PACKET_OUTGOING || n < (ssize_t)sizeof *ip || ip->version != 4) continue;
			hl = ip->ihl * 4u;
			if (ip->protocol != IPPROTO_UDP || hl < sizeof *ip || (size_t)n < hl + sizeof(struct udphdr) ||
			    ((struct udphdr *)(buf + hl))->dest != htons(68) || (ntohs(ip->frag_off) & 0x3fff)) continue;
			pl = buf + hl + sizeof(struct udphdr);
			n -= hl + sizeof(struct udphdr);
		}
		memset(r->dns, 0, sizeof r->dns);
		r->ndns = 0, r->mask = r->router = r->server = r->t1 = r->t2 = r->lease_s = 0, r->domain[0] = 0;
		t = netman__parse_reply(pl, n, xid, mac, r);
		if (t == want || t == DH_NAK) return t;
	}
}
static int netman__wait(int fd, uint32_t xid, const unsigned char *mac, int want, int ms, netman_lease *r)
{
	return netman__wait_(fd, 0, xid, mac, want, ms, r);
}
static void netman__finalize(netman_lease *l)
{
	if (!l->mask) { /* classful fallback, RFC 2131 leaves the option mandatory in practice */
		uint8_t f = ((uint8_t *)&l->ip)[0];
		l->mask = htonl(f < 128 ? 0xff000000u : f < 192 ? 0xffff0000u : 0xffffff00u);
	}
	if (!l->lease_s) l->lease_s = 3600;
	if (l->lease_s != 0xffffffffu) {
		if (!l->t1 || l->t1 >= l->lease_s) l->t1 = l->lease_s / 2;
		if (!l->t2 || l->t2 >= l->lease_s || l->t2 <= l->t1) l->t2 = (uint32_t)((uint64_t)l->lease_s * 7 / 8);
		if (l->t1 > l->t2) l->t1 = l->t2; /* server T1 above the derived T2 would skip the unicast renew (proved in lean4/Dhcp.lean) */
	}
	l->got = netman__now();
}

int netman_dhcp_acquire(const char *ifn, int timeout_ms, netman_lease *L)
{
	netman_if f;
	netman_lease o, a;
	struct netman__dh d;
	long end = netman__ms() + timeout_ms;
	int fd, rx, r, rto = netman_dhcp_rto_ms, n;
	if ((r = netman_find(ifn, &f)) < 0) return r;
	if (!f.up && (r = netman_link_set(ifn, 1)) < 0) return r;
	memset(&o, 0, sizeof o);
	netman__cpy(o.ifn, sizeof o.ifn, ifn);
	memcpy(o.mac, f.mac, 6);
	if ((fd = netman__dsock(ifn, 0)) < 0) return fd;
	if ((rx = netman__rxsock(ifn)) < 0) {
		close(fd);
		return rx;
	}
	for (;;) {
		uint32_t xid = netman__xid();
		long left = end - netman__ms();
		int w, t;
		if (left <= 0) {
			close(fd), close(rx);
			return -ETIMEDOUT;
		}
		w = rto < left ? rto : (int)left;
		n = netman__build(&d, &o, DH_DISCOVER, xid, 0, 0, 0, 1);
		if ((r = netman__send_raw(ifn, &d, n)) < 0) {
			close(fd), close(rx);
			return r;
		}
		memset(&a, 0, sizeof a);
		if ((t = netman__wait_(rx, 1, xid, o.mac, DH_OFFER, w, &a)) != DH_OFFER) {
			if (t == 0 && rto < 16000) rto *= 2;
			continue;
		}
		if (!a.ip || !a.server) continue; /* useless offer */
		n = netman__build(&d, &o, DH_REQUEST, xid, 0, a.ip, a.server, 1);
		if ((r = netman__send_raw(ifn, &d, n)) < 0) {
			close(fd), close(rx);
			return r;
		}
		left = end - netman__ms();
		w = netman_dhcp_rto_ms < left ? netman_dhcp_rto_ms : (int)left;
		if (w <= 0) continue;
		{
			netman_lease b;
			memset(&b, 0, sizeof b);
			t = netman__wait_(rx, 1, xid, o.mac, DH_ACK, w, &b);
			if (t == DH_ACK && b.ip) {
				b.ifn[0] = 0;
				netman__cpy(b.ifn, sizeof b.ifn, ifn);
				memcpy(b.mac, o.mac, 6);
				if (!b.server) b.server = a.server;
				netman__finalize(&b);
				*L = b;
				close(fd), close(rx);
				return 0;
			}
			/* NAK or silence: start over */
		}
	}
}

int netman_dhcp_renew(netman_lease *l, int broadcast, int timeout_ms)
{
	struct netman__dh d;
	netman_lease a;
	uint32_t xid = netman__xid();
	int fd = netman__dsock(l->ifn, l->ip), n, t, r;
	if (fd < 0) return fd;
	n = netman__build(&d, l, DH_REQUEST, xid, l->ip, 0, 0, 0);
	if ((r = netman__send(fd, &d, n, broadcast ? htonl(INADDR_BROADCAST) : l->server)) < 0) {
		close(fd);
		return r;
	}
	memset(&a, 0, sizeof a);
	t = netman__wait(fd, xid, l->mac, DH_ACK, timeout_ms, &a);
	close(fd);
	if (t == DH_NAK) return NETMAN_E_NAK;
	if (t != DH_ACK) return -ETIMEDOUT;
	{
		int changed = a.ip != l->ip;
		if (!a.server) a.server = l->server;
		netman__cpy(a.ifn, sizeof a.ifn, l->ifn);
		memcpy(a.mac, l->mac, 6);
		netman__finalize(&a);
		*l = a;
		return changed;
	}
}
int netman_dhcp_release(const netman_lease *l)
{
	struct netman__dh d;
	int fd = netman__dsock(l->ifn, l->ip), n, r;
	if (fd < 0) return fd;
	n = netman__build(&d, l, DH_RELEASE, netman__xid(), l->ip, 0, l->server, 0);
	r = netman__send(fd, &d, n, l->server);
	close(fd);
	return r;
}

static int netman__plen(uint32_t mask_be)
{
	uint32_t m = ntohl(mask_be), inv = ~m;
	int n = 0;
	if (inv & (inv + 1)) return -1; /* holes in the mask */
	while (m) n += m & 1, m >>= 1;
	return n;
}
void netman_lease_print(FILE *f, const netman_lease *l)
{
	char a[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];
	int i;
	inet_ntop(AF_INET, &l->ip, a, sizeof a);
	fprintf(f, "ip %s/%d\n", a, netman__plen(l->mask));
	inet_ntop(AF_INET, &l->router, a, sizeof a);
	inet_ntop(AF_INET, &l->server, b, sizeof b);
	fprintf(f, "router %s\nserver %s\nlease %u t1 %u t2 %u\n", l->router ? a : "-", b, l->lease_s, l->t1, l->t2);
	for (i = 0; i < l->ndns; i++) {
		inet_ntop(AF_INET, &l->dns[i], a, sizeof a);
		fprintf(f, "dns %s\n", a);
	}
	if (l->domain[0]) fprintf(f, "domain %s\n", l->domain);
}
int netman_lease_apply(const netman_lease *l)
{
	char cidr[48], rt[INET_ADDRSTRLEN], dn[4][INET_ADDRSTRLEN];
	const char *ns[4];
	int p = netman__plen(l->mask), i, r;
	if (p < 0 || !l->ip) return -EINVAL;
	inet_ntop(AF_INET, &l->ip, cidr, sizeof cidr);
	snprintf(cidr + strlen(cidr), sizeof cidr - strlen(cidr), "/%d", p);
	if ((r = netman_addr_add(l->ifn, cidr)) < 0) return r;
	if (l->router) {
		inet_ntop(AF_INET, &l->router, rt, sizeof rt);
		if ((r = netman_route_add(l->ifn, "default", rt, netman_metric(l->ifn))) < 0) return r;
	}
	if (l->ndns) {
		for (i = 0; i < l->ndns; i++) inet_ntop(AF_INET, &l->dns[i], dn[i], sizeof dn[i]), ns[i] = dn[i];
		if ((r = netman_dns_set(l->ifn, ns, l->ndns, l->domain)) < 0) return r;
	}
	return 0;
}
int netman_lease_unapply(const netman_lease *l)
{
	char cidr[48], rt[INET_ADDRSTRLEN];
	int p = netman__plen(l->mask);
	if (l->router) {
		inet_ntop(AF_INET, &l->router, rt, sizeof rt);
		netman_route_del(l->ifn, "default", rt, netman_metric(l->ifn));
	}
	netman_dns_clear(l->ifn);
	if (p < 0) return 0;
	inet_ntop(AF_INET, &l->ip, cidr, sizeof cidr);
	snprintf(cidr + strlen(cidr), sizeof cidr - strlen(cidr), "/%d", p);
	return netman_addr_del(l->ifn, cidr);
}
int netman_metric(const char *ifn)
{
	return (!strncmp(ifn, "wl", 2) || !strncmp(ifn, "wlan", 4)) ? 600 : 100; /* wired beats wifi */
}
int netman_dhcp_reboot(const char *ifn, const netman_lease *cached, int timeout_ms, netman_lease *L)
{
	netman_if f;
	netman_lease o;
	struct netman__dh d;
	long end = netman__ms() + timeout_ms;
	int fd, rx, r, n, each = timeout_ms / 3 > 100 ? timeout_ms / 3 : 100;
	uint32_t xid = netman__xid();
	if ((r = netman_find(ifn, &f)) < 0) return r;
	if (!f.up && (r = netman_link_set(ifn, 1)) < 0) return r;
	memset(&o, 0, sizeof o);
	netman__cpy(o.ifn, sizeof o.ifn, ifn);
	memcpy(o.mac, f.mac, 6);
	if ((fd = netman__dsock(ifn, 0)) < 0) return fd;
	if ((rx = netman__rxsock(ifn)) < 0) {
		close(fd);
		return rx;
	}
	while (netman__ms() < end) {
		netman_lease a;
		long left = end - netman__ms();
		int t;
		n = netman__build(&d, &o, DH_REQUEST, xid, 0, cached->ip, 0, 1);
		if ((r = netman__send_raw(ifn, &d, n)) < 0) break;
		memset(&a, 0, sizeof a);
		t = netman__wait_(rx, 1, xid, o.mac, DH_ACK, each < left ? each : (int)left, &a);
		if (t == DH_NAK) {
			close(fd), close(rx);
			return NETMAN_E_NAK;
		}
		if (t == DH_ACK && a.ip) {
			netman__cpy(a.ifn, sizeof a.ifn, ifn);
			memcpy(a.mac, o.mac, 6);
			netman__finalize(&a);
			*L = a;
			close(fd), close(rx);
			return 0;
		}
		r = -ETIMEDOUT;
	}
	close(fd), close(rx);
	return r < 0 ? r : -ETIMEDOUT;
}
/* ---------- profiles ---------- */
static char *netman__trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t') s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}
static int netman__route_ok(const char *s)
{
	char t[96], *v;
	netman__ip a, g;
	snprintf(t, sizeof t, "%s", s);
	if ((v = strstr(t, " via "))) {
		*v = 0;
		v += 5;
		if (netman__parse(v, &g, 0) < 0 || g.plen >= 0) return 0;
	}
	return netman__parse(t, &a, 1) == 0;
}
static int netman__profile_read(const char *path, const char *name, netman_profile *p)
{
	char buf[4096], *l, *save;
	int r;
	memset(p, 0, sizeof *p);
	if ((r = netman__slurp(path, buf, sizeof buf)) < 0) return netman__fail(r, "cannot read %s: %s", path, strerror(-r));
	netman__cpy(p->name, sizeof p->name, name);
	for (l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		char *k = netman__trim(l), *v;
		if (!*k || *k == '#') continue;
		if (!(v = strchr(k, '='))) return netman__fail(-EINVAL, "%s: line without '=': %s", path, k);
		*v++ = 0;
		k = netman__trim(k), v = netman__trim(v);
		if (!strcmp(k, "iface")) {
			if (!netman__safe_name(v, sizeof p->iface)) return netman__fail(-EINVAL, "%s: bad iface '%s'", path, v);
			netman__cpy(p->iface, sizeof p->iface, v);
		} else if (!strcmp(k, "method")) {
			if (!strcmp(v, "dhcp")) p->dhcp = 1;
			else if (!strcmp(v, "static")) p->dhcp = 0;
			else return netman__fail(-EINVAL, "%s: method must be dhcp or static", path);
		} else if (!strcmp(k, "address")) {
			netman__ip a;
			if (netman__parse(v, &a, 1) < 0) return netman__fail(-EINVAL, "%s: bad address '%s'", path, v);
			netman__cpy(p->addr, sizeof p->addr, v);
		} else if (!strcmp(k, "gateway")) {
			netman__ip a;
			if (netman__parse(v, &a, 0) < 0 || a.plen >= 0) return netman__fail(-EINVAL, "%s: bad gateway '%s'", path, v);
			netman__cpy(p->gateway, sizeof p->gateway, v);
		} else if (!strcmp(k, "dns")) {
			char *s2, *t;
			for (t = strtok_r(v, ", ", &s2); t; t = strtok_r(NULL, ", ", &s2)) {
				netman__ip a;
				if (p->ndns >= 3) return netman__fail(-EINVAL, "%s: at most 3 dns servers", path);
				if (netman__parse(t, &a, 0) < 0 || a.plen >= 0) return netman__fail(-EINVAL, "%s: bad dns '%s'", path, t);
				netman__cpy(p->dns[p->ndns++], 46, t);
			}
		} else if (!strcmp(k, "route")) {
			if (p->nroute >= NETMAN_MAXROUTE || !netman__route_ok(v)) return netman__fail(-EINVAL, "%s: bad route '%s'", path, v);
			netman__cpy(p->route[p->nroute++], 96, v);
		} else if (!strcmp(k, "priority")) {
			char *e;
			long pv = strtol(v, &e, 10);
			if (!*v || *e || pv < -1000 || pv > 1000) return netman__fail(-EINVAL, "%s: bad priority", path);
			p->priority = (int)pv;
		} else if (!strcmp(k, "randmac")) {
			p->randmac = !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "true");
		} else if (!strcmp(k, "autoconnect")) {
			p->autoconnect = !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "true");
		} else return netman__fail(-EINVAL, "%s: unknown key '%s'", path, k);
	}
	if (!p->iface[0]) return netman__fail(-EINVAL, "%s: iface= is required", path);
	if (!p->dhcp && !p->addr[0]) return netman__fail(-EINVAL, "%s: static profile needs address=", path);
	return 0;
}
int netman_profile_load(const char *name, netman_profile *p)
{
	char path[600];
	if (!netman__safe_name(name, sizeof p->name)) return netman__fail(-EINVAL, "bad profile name");
	snprintf(path, sizeof path, "%s/%s.conf", netman_conf_dir, name);
	return netman__profile_read(path, name, p);
}
int netman_profile_save(const netman_profile *p)
{
	char path[600], b[2048];
	int o = 0, i, r;
	if (!netman__safe_name(p->name, sizeof p->name)) return netman__fail(-EINVAL, "bad profile name");
	if (!netman__safe_name(p->iface, sizeof p->iface)) return netman__fail(-EINVAL, "bad iface");
	if (!p->dhcp && !p->addr[0]) return netman__fail(-EINVAL, "static profile needs an address");
	o += snprintf(b + o, sizeof b - o, "iface=%s\nmethod=%s\n", p->iface, p->dhcp ? "dhcp" : "static");
	if (p->addr[0]) o += snprintf(b + o, sizeof b - o, "address=%s\n", p->addr);
	if (p->gateway[0]) o += snprintf(b + o, sizeof b - o, "gateway=%s\n", p->gateway);
	if (p->ndns) {
		o += snprintf(b + o, sizeof b - o, "dns=");
		for (i = 0; i < p->ndns; i++) o += snprintf(b + o, sizeof b - o, "%s%s", i ? "," : "", p->dns[i]);
		o += snprintf(b + o, sizeof b - o, "\n");
	}
	for (i = 0; i < p->nroute; i++) o += snprintf(b + o, sizeof b - o, "route=%s\n", p->route[i]);
	o += snprintf(b + o, sizeof b - o, "autoconnect=%d\npriority=%d\nrandmac=%d\n", p->autoconnect ? 1 : 0, p->priority, p->randmac ? 1 : 0);
	if (o >= (int)sizeof b) return -E2BIG;
	if ((r = netman__mkdirs(netman_conf_dir)) < 0) return r;
	snprintf(path, sizeof path, "%s/%s.conf", netman_conf_dir, p->name);
	/* validate exactly what we wrote: save never leaves a file that load would refuse */
	{
		netman_profile chk;
		char tmp[600];
		int fd;
		snprintf(tmp, sizeof tmp, "%s/.save.XXXXXX", netman_conf_dir);
		if ((fd = mkstemp(tmp)) < 0) return -errno;
		if (write(fd, b, o) != o || fchmod(fd, 0644) < 0 || fsync(fd) < 0) {
			r = -errno;
			close(fd);
			unlink(tmp);
			return r;
		}
		close(fd);
		if ((r = netman__profile_read(tmp, p->name, &chk)) < 0 || rename(tmp, path) < 0) {
			if (r >= 0) r = -errno;
			unlink(tmp);
			return r;
		}
	}
	return 0;
}
static int netman__cmp(const void *a, const void *b) { return strcmp(a, b); }
int netman_profile_list(char names[][64], int max)
{
	DIR *d = opendir(netman_conf_dir);
	struct dirent *e;
	int n = 0;
	if (!d) return errno == ENOENT ? 0 : -errno;
	while ((e = readdir(d)) && n < max) {
		size_t l = strlen(e->d_name);
		if (l < 6 || strcmp(e->d_name + l - 5, ".conf") || l - 5 >= 64) continue;
		memcpy(names[n], e->d_name, l - 5);
		names[n][l - 5] = 0;
		if (netman__safe_name(names[n], 64)) n++;
	}
	closedir(d);
	qsort(names, n, 64, netman__cmp);
	return n;
}
#endif /* NETMAN_NET_IMPLEMENTATION_DONE */
#endif /* NETMAN_NET_IMPLEMENTATION */
