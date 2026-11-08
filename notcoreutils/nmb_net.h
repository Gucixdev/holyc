/* nmb_net.h - wired/IP half of netmanager_blue. stb-style single header.
 *
 *   #define NMB_NET_IMPLEMENTATION   in exactly one .c file, then #include "nmb_net.h"
 *
 * No D-Bus, no Python, no libraries: rtnetlink + a small DHCPv4 client.
 *   interfaces   nmbn_list nmbn_find nmbn_print
 *   link/addr    nmbn_link_set nmbn_addr_add/del/flush nmbn_route_add/del   (v4 and v6)
 *   dhcp         nmbn_dhcp_acquire/renew/release, nmbn_lease_apply/unapply,
 *                nmbn_dhcpc_step (renew T1 / rebind T2 / expiry state machine)
 *   dns          nmbn_dns_set/clear: /etc/resolv.conf (atomic) or systemd-resolved
 *   profiles     /etc/netmanager_blue/NAME.conf: nmbn_profile_load/save/list/apply/down
 *   daemon       nmbn_autoconnect(&stop, poll_ms)
 *
 * All functions return 0 / count on success and a negative errno on failure
 * (NMBN_E_NAK for a DHCPNAK); nmbn_error() holds a text for the last parse error.
 * Needs CAP_NET_ADMIN (+ CAP_NET_RAW, port 68) for anything that changes state;
 * listing works unprivileged. Not thread-safe (static error buffer).
 *
 * Not done: ARP conflict probe of a DHCP lease, DHCPv6/SLAAC management, link-local
 * fallback, bonding/bridge/vlan, hostname option.
 */
#ifndef NMB_NET_H
#define NMB_NET_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define NMBN_MAXIF 32
#define NMBN_MAXADDR 8
#define NMBN_MAXROUTE 8
#define NMBN_E_NAK (-1000)

typedef struct {
	char name[16];
	int idx, up, carrier, mtu;
	unsigned char mac[6];
	int naddr;
	char addr[NMBN_MAXADDR][64]; /* "10.0.0.5/24", "fe80::1/64" */
	char gw4[40], gw6[40];	     /* default gateways, "" if none */
	int nroute;
	char route[NMBN_MAXROUTE][96]; /* non-kernel, non-default: "10.1.0.0/16 via 10.0.0.1" */
} nmbn_if;

typedef struct {
	char ifn[16];
	unsigned char mac[6];
	uint32_t ip, mask, router, server, dns[4]; /* network byte order */
	int ndns;
	uint32_t lease_s, t1, t2; /* seconds; lease_s 0xffffffff = infinite */
	time_t got;		  /* CLOCK_MONOTONIC seconds when the lease (re)started */
	char domain[64];
} nmbn_lease;

typedef struct {
	char name[64], iface[16];
	int dhcp;	  /* method=dhcp (1) or static (0) */
	char addr[64];	  /* static: "ip/prefix" */
	char gateway[46]; /* static */
	char dns[3][46];
	int ndns;
	char route[NMBN_MAXROUTE][96]; /* "dst/len via gw" */
	int nroute;
	int autoconnect;
	int priority; /* higher wins when several profiles want the same interface */
	int randmac;  /* stable random (locally administered) MAC per profile */
} nmbn_profile;

typedef struct {
	nmbn_lease l;
	int st; /* 0 idle, 1 bound */
	time_t next_try;
	char key[64]; /* lease-cache key (profile name, or SSID/BSSID from the wifi layer); "" = no cache */
} nmbn_dhcpc;

/* tunables (defined in the implementation) */
extern const char *nmbn_run_dir;     /* "/run/netmanager_blue"  dns state */
extern const char *nmbn_resolv_path; /* "/etc/resolv.conf" */
extern const char *nmbn_conf_dir;    /* "/etc/netmanager_blue" */
extern int nmbn_dns_mode;	     /* 0 auto, 1 resolv.conf only, 2 systemd-resolved only */
extern const char *nmbn_state_dir;   /* "/var/lib/netmanager_blue" lease cache + mac secret */
extern int nmbn_dhcp_rto_ms;	     /* first retransmit, doubles (default 1000) */
extern int nmbn_dhcp_timeout_ms;     /* full-DISCOVER budget (default 6000) */
extern int nmbn_reboot_timeout_ms;   /* cached-lease INIT-REBOOT budget (default 1200) */
extern const char *nmbn_probe_ip;    /* connectivity probe target, "" = off */
extern const char *nmbn_probe_host;  /* Host: header (default "connectivitycheck.gstatic.com") */
extern const char *nmbn_probe_path;  /* default "/generate_204" */
extern int nmbn_probe_port;	     /* default 80 */

const char *nmbn_error(void);

int nmbn_list(nmbn_if *out, int max);
int nmbn_find(const char *ifn, nmbn_if *out); /* 0 or -ENODEV */
void nmbn_print(FILE *f);

int nmbn_link_set(const char *ifn, int up);
int nmbn_addr_add(const char *ifn, const char *cidr); /* already present = success */
int nmbn_addr_del(const char *ifn, const char *cidr); /* absent = success */
int nmbn_addr_flush(const char *ifn);		      /* all v4 + global v6, keeps fe80:: */
/* dst "default" or "ip/len"; gw may be NULL/"" for an on-link route */
int nmbn_route_add(const char *ifn, const char *dst, const char *gw, int metric);
int nmbn_route_del(const char *ifn, const char *dst, const char *gw, int metric);

int nmbn_dhcp_acquire(const char *ifn, int timeout_ms, nmbn_lease *l);
int nmbn_dhcp_renew(nmbn_lease *l, int broadcast, int timeout_ms); /* 0 same ip, 1 ip changed */
/* INIT-REBOOT: ask for a cached address back (1 broadcast REQUEST, no DISCOVER). NMBN_E_NAK if refused */
int nmbn_dhcp_reboot(const char *ifn, const nmbn_lease *cached, int timeout_ms, nmbn_lease *l);
/* cache first, full DHCP only on NAK/timeout; *cached = 1 if the cache answered; saves the result */
int nmbn_dhcp_connect(const char *ifn, const char *key, int timeout_ms, nmbn_lease *l, int *cached);
int nmbn_lease_save(const char *key, const nmbn_lease *l);
int nmbn_lease_load(const char *key, const char *ifn, nmbn_lease *l); /* -ESTALE when expired */
void nmbn_lease_forget(const char *key);
int nmbn_dhcp_release(const nmbn_lease *l);
int nmbn_lease_apply(const nmbn_lease *l);
int nmbn_lease_unapply(const nmbn_lease *l);
int nmbn_dhcpc_step(nmbn_dhcpc *c, const char *ifn); /* call about once a second */
int nmbn_dhcpc_stop(nmbn_dhcpc *c, int release);
void nmbn_lease_print(FILE *f, const nmbn_lease *l);

int nmbn_dns_set(const char *ifn, const char **ns, int n, const char *search);
int nmbn_dns_clear(const char *ifn);

int nmbn_profile_load(const char *name, nmbn_profile *p);
int nmbn_profile_load(const char *name, nmbn_profile *p);
int nmbn_profile_save(const nmbn_profile *p);
int nmbn_profile_list(char names[][64], int max);
int nmbn_profile_apply(const nmbn_profile *p, nmbn_lease *l); /* l only filled for dhcp */
int nmbn_profile_down(const nmbn_profile *p, const nmbn_lease *l);
/* Event-driven (netlink link/addr/route + timers, no polling). poll_ms = longest idle sleep (0 = 30000).
 * Per interface: best profile by priority/last success, 1 s..60 s per-profile backoff on failure,
 * immediate fallthrough to the next candidate, clean switch (addresses, default routes, dns). */
int nmbn_autoconnect(volatile int *stop, int poll_ms);

int nmbn_metric(const char *ifn);		   /* route metric: wired 100, wifi 600 */
int nmbn_iface_reset(const char *ifn);		   /* drop default routes, addresses and dns of ifn */
int nmbn_mac_set(const char *ifn, const unsigned char mac[6]);
void nmbn_mac_for(const char *key, unsigned char mac[6]); /* stable per key and machine, locally administered */
/* HTTP GET via ifn: 0 = 204 (internet), 1 = other answer (captive portal), <0 = no connectivity */
int nmbn_probe(const char *ifn, int timeout_ms);

#endif /* NMB_NET_H */

#ifdef NMB_NET_IMPLEMENTATION
#ifndef NMB_NET_IMPLEMENTATION_DONE
#define NMB_NET_IMPLEMENTATION_DONE

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

const char *nmbn_run_dir = "/run/netmanager_blue";
const char *nmbn_resolv_path = "/etc/resolv.conf";
const char *nmbn_conf_dir = "/etc/netmanager_blue";
int nmbn_dns_mode = 0;
const char *nmbn_state_dir = "/var/lib/netmanager_blue";
int nmbn_dhcp_rto_ms = 1000;
int nmbn_dhcp_timeout_ms = 6000;
int nmbn_reboot_timeout_ms = 1200;
const char *nmbn_probe_ip = "";
const char *nmbn_probe_host = "connectivitycheck.gstatic.com";
const char *nmbn_probe_path = "/generate_204";
int nmbn_probe_port = 80;

static char nmbn__err[256];
const char *nmbn_error(void) { return nmbn__err; }
static int nmbn__fail(int e, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(nmbn__err, sizeof nmbn__err, fmt, ap);
	va_end(ap);
	return e < 0 ? e : -e;
}
static void nmbn__cpy(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s ? s : ""); }
static long nmbn__ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static time_t nmbn__now(void) { return nmbn__ms() / 1000; }

/* ---------- ip parsing ---------- */
typedef struct {
	int fam, plen; /* plen -1 = none given */
	unsigned char b[16];
} nmbn__ip;

static int nmbn__parse(const char *s, nmbn__ip *o, int need_plen)
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
static int nmbn__iplen(const nmbn__ip *i) { return i->fam == AF_INET ? 4 : 16; }

/* ---------- netlink ---------- */
typedef struct {
	struct nlmsghdr h;
	union {
		struct ifinfomsg i;
		struct ifaddrmsg a;
		struct rtmsg r;
	} m;
	char buf[512];
} nmbn__req;

static void nmbn__req_init(nmbn__req *q, int type, int flags, size_t msglen)
{
	memset(q, 0, sizeof *q);
	q->h.nlmsg_type = type;
	q->h.nlmsg_flags = NLM_F_REQUEST | flags;
	q->h.nlmsg_len = NLMSG_LENGTH(msglen);
}
static void nmbn__attr(nmbn__req *q, int type, const void *d, int len)
{
	struct rtattr *a = (struct rtattr *)((char *)q + NLMSG_ALIGN(q->h.nlmsg_len));
	if (NLMSG_ALIGN(q->h.nlmsg_len) + RTA_SPACE(len) > sizeof *q) return; /* cannot happen: attrs are tiny */
	a->rta_type = type;
	a->rta_len = RTA_LENGTH(len);
	if (len) memcpy(RTA_DATA(a), d, len);
	q->h.nlmsg_len = NLMSG_ALIGN(q->h.nlmsg_len) + RTA_ALIGN(a->rta_len);
}

typedef void (*nmbn__cb)(const struct nlmsghdr *, void *);
static int nmbn__nl(nmbn__req *q, nmbn__cb cb, void *u)
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
	nmbn_if *t;
	int max, n;
} nmbn__lctx;

static nmbn_if *nmbn__byidx(nmbn__lctx *c, int idx)
{
	int i;
	for (i = 0; i < c->n; i++)
		if (c->t[i].idx == idx) return &c->t[i];
	return NULL;
}
static void nmbn__fmt(char *d, size_t n, int fam, const void *a, int plen)
{
	char t[INET6_ADDRSTRLEN];
	if (!inet_ntop(fam, a, t, sizeof t)) t[0] = 0;
	if (plen >= 0) snprintf(d, n, "%s/%d", t, plen);
	else snprintf(d, n, "%s", t);
}
static void nmbn__link_cb(const struct nlmsghdr *m, void *u)
{
	nmbn__lctx *c = u;
	const struct ifinfomsg *i = NLMSG_DATA(m);
	int len = IFLA_PAYLOAD(m);
	const struct rtattr *a;
	nmbn_if *f;
	if (m->nlmsg_type != RTM_NEWLINK || c->n >= c->max) return;
	f = &c->t[c->n++];
	memset(f, 0, sizeof *f);
	f->idx = i->ifi_index;
	f->up = !!(i->ifi_flags & IFF_UP);
	f->carrier = !!(i->ifi_flags & IFF_LOWER_UP);
	for (a = IFLA_RTA(i); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
		if (a->rta_type == IFLA_IFNAME) nmbn__cpy(f->name, sizeof f->name, RTA_DATA(a));
		else if (a->rta_type == IFLA_MTU && RTA_PAYLOAD(a) >= 4) memcpy(&f->mtu, RTA_DATA(a), 4);
		else if (a->rta_type == IFLA_ADDRESS && RTA_PAYLOAD(a) == 6) memcpy(f->mac, RTA_DATA(a), 6);
	}
}
static void nmbn__addr_cb(const struct nlmsghdr *m, void *u)
{
	nmbn__lctx *c = u;
	const struct ifaddrmsg *i = NLMSG_DATA(m);
	int len = IFA_PAYLOAD(m);
	const struct rtattr *a;
	const void *addr = NULL, *local = NULL;
	nmbn_if *f;
	if (m->nlmsg_type != RTM_NEWADDR) return;
	if (!(f = nmbn__byidx(c, i->ifa_index)) || f->naddr >= NMBN_MAXADDR) return;
	for (a = IFA_RTA(i); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
		if (a->rta_type == IFA_ADDRESS) addr = RTA_DATA(a);
		else if (a->rta_type == IFA_LOCAL) local = RTA_DATA(a);
	}
	if (!(local ? local : addr)) return;
	nmbn__fmt(f->addr[f->naddr++], 64, i->ifa_family, local ? local : addr, i->ifa_prefixlen);
}
static void nmbn__route_cb(const struct nlmsghdr *m, void *u)
{
	nmbn__lctx *c = u;
	const struct rtmsg *r = NLMSG_DATA(m);
	int len = RTM_PAYLOAD(m), oif = 0;
	const struct rtattr *a;
	const void *dst = NULL, *gw = NULL;
	nmbn_if *f;
	char d[64], g[48];
	if (m->nlmsg_type != RTM_NEWROUTE || r->rtm_table != RT_TABLE_MAIN || r->rtm_type != RTN_UNICAST) return;
	for (a = RTM_RTA(r); RTA_OK(a, len); a = RTA_NEXT(a, len)) {
		if (a->rta_type == RTA_DST) dst = RTA_DATA(a);
		else if (a->rta_type == RTA_GATEWAY) gw = RTA_DATA(a);
		else if (a->rta_type == RTA_OIF && RTA_PAYLOAD(a) >= 4) memcpy(&oif, RTA_DATA(a), 4);
	}
	if (!(f = nmbn__byidx(c, oif))) return;
	if (r->rtm_dst_len == 0) {
		if (gw) nmbn__fmt(r->rtm_family == AF_INET ? f->gw4 : f->gw6, 40, r->rtm_family, gw, -1);
		return;
	}
	if (r->rtm_protocol == RTPROT_KERNEL || f->nroute >= NMBN_MAXROUTE) return;
	nmbn__fmt(d, sizeof d, r->rtm_family, dst ? dst : "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", r->rtm_dst_len);
	if (gw) {
		nmbn__fmt(g, sizeof g, r->rtm_family, gw, -1);
		snprintf(f->route[f->nroute++], 96, "%.47s via %.44s", d, g);
	} else snprintf(f->route[f->nroute++], 96, "%s", d);
}

int nmbn_list(nmbn_if *out, int max)
{
	nmbn__lctx c = {out, max, 0};
	nmbn__req q;
	int r;
	nmbn__req_init(&q, RTM_GETLINK, NLM_F_DUMP, sizeof(struct ifinfomsg));
	if ((r = nmbn__nl(&q, nmbn__link_cb, &c)) < 0) return r;
	nmbn__req_init(&q, RTM_GETADDR, NLM_F_DUMP, sizeof(struct ifaddrmsg));
	if ((r = nmbn__nl(&q, nmbn__addr_cb, &c)) < 0) return r;
	nmbn__req_init(&q, RTM_GETROUTE, NLM_F_DUMP, sizeof(struct rtmsg));
	if ((r = nmbn__nl(&q, nmbn__route_cb, &c)) < 0) return r;
	return c.n;
}
int nmbn_find(const char *ifn, nmbn_if *out)
{
	nmbn_if t[NMBN_MAXIF];
	int n = nmbn_list(t, NMBN_MAXIF), i;
	if (n < 0) return n;
	for (i = 0; i < n; i++)
		if (!strcmp(t[i].name, ifn)) {
			*out = t[i];
			return 0;
		}
	return -ENODEV;
}
void nmbn_print(FILE *f)
{
	nmbn_if t[NMBN_MAXIF];
	int n = nmbn_list(t, NMBN_MAXIF), i, j;
	for (i = 0; i < n; i++) {
		nmbn_if *x = &t[i];
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
static int nmbn__idx(const char *ifn)
{
	unsigned i = ifn && *ifn ? if_nametoindex(ifn) : 0;
	return i ? (int)i : -ENODEV;
}
int nmbn_link_set(const char *ifn, int up)
{
	nmbn__req q;
	int i = nmbn__idx(ifn);
	if (i < 0) return i;
	nmbn__req_init(&q, RTM_NEWLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	q.m.i.ifi_index = i;
	q.m.i.ifi_change = IFF_UP;
	q.m.i.ifi_flags = up ? IFF_UP : 0;
	return nmbn__nl(&q, NULL, NULL);
}
static int nmbn__addr(int type, int flags, const char *ifn, const char *cidr)
{
	nmbn__req q;
	nmbn__ip a;
	int i = nmbn__idx(ifn), r;
	if (i < 0) return i;
	if (nmbn__parse(cidr, &a, 1) < 0) return nmbn__fail(-EINVAL, "bad address '%s' (want ip/prefix)", cidr);
	nmbn__req_init(&q, type, NLM_F_ACK | flags, sizeof(struct ifaddrmsg));
	q.m.a.ifa_family = a.fam;
	q.m.a.ifa_prefixlen = a.plen;
	q.m.a.ifa_index = i;
	nmbn__attr(&q, IFA_LOCAL, a.b, nmbn__iplen(&a));
	nmbn__attr(&q, IFA_ADDRESS, a.b, nmbn__iplen(&a));
	if (a.fam == AF_INET && type == RTM_NEWADDR) {
		uint32_t ip, m = a.plen ? htonl(~0u << (32 - a.plen)) : 0, b;
		memcpy(&ip, a.b, 4);
		b = ip | ~m;
		nmbn__attr(&q, IFA_BROADCAST, &b, 4);
	}
	r = nmbn__nl(&q, NULL, NULL);
	if (type == RTM_NEWADDR && r == -EEXIST) r = 0;
	if (type == RTM_DELADDR && r == -EADDRNOTAVAIL) r = 0;
	return r;
}
int nmbn_addr_add(const char *ifn, const char *cidr) { return nmbn__addr(RTM_NEWADDR, NLM_F_CREATE | NLM_F_EXCL, ifn, cidr); }
int nmbn_addr_del(const char *ifn, const char *cidr) { return nmbn__addr(RTM_DELADDR, 0, ifn, cidr); }
int nmbn_addr_flush(const char *ifn)
{
	nmbn_if f;
	int r = nmbn_find(ifn, &f), i, rc = 0;
	if (r < 0) return r;
	for (i = 0; i < f.naddr; i++) {
		if (!strncmp(f.addr[i], "fe80:", 5)) continue;
		if ((r = nmbn_addr_del(ifn, f.addr[i])) < 0) rc = r;
	}
	return rc;
}
static int nmbn__route(int type, const char *ifn, const char *dst, const char *gw, int metric, int onlink)
{
	nmbn__req q;
	nmbn__ip d, g;
	int i = nmbn__idx(ifn), has_gw = gw && *gw, deflt = !dst || !strcmp(dst, "default");
	if (i < 0) return i;
	memset(&d, 0, sizeof d);
	if (has_gw && nmbn__parse(gw, &g, 0) < 0) return nmbn__fail(-EINVAL, "bad gateway '%s'", gw);
	if (!deflt && nmbn__parse(dst, &d, 1) < 0) return nmbn__fail(-EINVAL, "bad route '%s' (want ip/len or default)", dst);
	if (deflt) {
		if (!has_gw) return nmbn__fail(-EINVAL, "default route needs a gateway");
		d.fam = g.fam;
		d.plen = 0;
	}
	if (has_gw && g.fam != d.fam) return nmbn__fail(-EINVAL, "address family mismatch in route");
	nmbn__req_init(&q, type, NLM_F_ACK | (type == RTM_NEWROUTE ? NLM_F_CREATE | NLM_F_REPLACE : 0), sizeof(struct rtmsg));
	q.m.r.rtm_family = d.fam;
	q.m.r.rtm_dst_len = d.plen;
	q.m.r.rtm_table = RT_TABLE_MAIN;
	q.m.r.rtm_protocol = RTPROT_BOOT;
	q.m.r.rtm_scope = has_gw ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
	q.m.r.rtm_type = RTN_UNICAST;
	if (onlink) q.m.r.rtm_flags |= RTNH_F_ONLINK;
	if (d.plen) nmbn__attr(&q, RTA_DST, d.b, nmbn__iplen(&d));
	if (has_gw) nmbn__attr(&q, RTA_GATEWAY, g.b, nmbn__iplen(&g));
	nmbn__attr(&q, RTA_OIF, &i, 4);
	if (metric > 0) nmbn__attr(&q, RTA_PRIORITY, &metric, 4);
	return nmbn__nl(&q, NULL, NULL);
}
int nmbn_route_add(const char *ifn, const char *dst, const char *gw, int metric)
{
	int r = nmbn__route(RTM_NEWROUTE, ifn, dst, gw, metric, 0);
	/* gateway outside the interface subnet (common with /32 DHCP leases): retry on-link */
	if ((r == -ENETUNREACH || r == -EINVAL) && gw && *gw) r = nmbn__route(RTM_NEWROUTE, ifn, dst, gw, metric, 1);
	return r;
}
int nmbn_route_del(const char *ifn, const char *dst, const char *gw, int metric)
{
	int r = nmbn__route(RTM_DELROUTE, ifn, dst, gw, metric, 0);
	return r == -ESRCH ? 0 : r;
}

/* ---------- small file helpers ---------- */
static int nmbn__mkdirs(const char *p)
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
static int nmbn__write_atomic(const char *path, const char *data, size_t len, mode_t mode)
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
static int nmbn__slurp(const char *path, char *buf, size_t n)
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
static int nmbn__safe_name(const char *s, size_t max)
{
	size_t i, n = s ? strlen(s) : 0;
	if (!n || n >= max || s[0] == '.') return 0;
	for (i = 0; i < n; i++)
		if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-' || s[i] == '.'))
			return 0;
	return 1;
}

/* ---------- dns ---------- */
#define NMBN_HDR "# generated by netmanager_blue - do not edit\n"

static int nmbn__use_resolved(void)
{
	if (nmbn_dns_mode == 1) return 0;
	if (nmbn_dns_mode == 2) return 1;
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
static int nmbn__run(char *const argv[])
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
static int nmbn__dns_rebuild(void)
{
	struct dirent **ls = NULL;
	char out[8192], path[600], orig[600], cur[8192];
	int n, i, o = 0, any = 0, r = 0;
	snprintf(orig, sizeof orig, "%s/resolv.conf.orig", nmbn_run_dir);
	n = scandir(nmbn_run_dir, &ls, NULL, alphasort);
	o += snprintf(out, sizeof out, NMBN_HDR);
	for (i = 0; i < n; i++) {
		char t[2048], *l, *save;
		if (strncmp(ls[i]->d_name, "dns.", 4)) continue;
		snprintf(path, sizeof path, "%s/%s", nmbn_run_dir, ls[i]->d_name);
		if (nmbn__slurp(path, t, sizeof t) < 0) continue;
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
	if (nmbn__slurp(nmbn_resolv_path, cur, sizeof cur) < 0) cur[0] = 0;
	if (!any) { /* nothing left: give the original resolv.conf back */
		char b[8192];
		int k = nmbn__slurp(orig, b, sizeof b);
		if (k >= 0) {
			r = nmbn__write_atomic(nmbn_resolv_path, b, k, 0644);
			if (r == 0) unlink(orig);
		} else if (!strncmp(cur, NMBN_HDR, strlen(NMBN_HDR))) {
			r = nmbn__write_atomic(nmbn_resolv_path, NMBN_HDR, strlen(NMBN_HDR), 0644);
		}
		return r;
	}
	if (strncmp(cur, NMBN_HDR, strlen(NMBN_HDR)) && access(orig, F_OK) != 0 && cur[0])
		nmbn__write_atomic(orig, cur, strlen(cur), 0644); /* keep the user's file once */
	return nmbn__write_atomic(nmbn_resolv_path, out, o, 0644);
}
int nmbn_dns_set(const char *ifn, const char **ns, int n, const char *search)
{
	char body[1024], path[600];
	int i, o = 0, r;
	if (!nmbn__safe_name(ifn, 16)) return nmbn__fail(-EINVAL, "bad interface name");
	for (i = 0; i < n; i++) {
		nmbn__ip a;
		if (nmbn__parse(ns[i], &a, 0) < 0 || a.plen >= 0) return nmbn__fail(-EINVAL, "bad dns server '%s'", ns[i]);
	}
	if (search && *search) {
		const char *s;
		for (s = search; *s; s++)
			if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '.' || *s == '-'))
				return nmbn__fail(-EINVAL, "bad search domain '%s'", search);
	}
	if (nmbn__use_resolved()) {
		char *argv[32];
		int k = 0;
		argv[k++] = "resolvectl";
		argv[k++] = "dns";
		argv[k++] = (char *)ifn;
		for (i = 0; i < n && k < 28; i++) argv[k++] = (char *)ns[i];
		argv[k] = NULL;
		if ((r = nmbn__run(argv)) < 0) return r;
		if (search && *search) {
			char *d[] = {"resolvectl", "domain", (char *)ifn, (char *)search, NULL};
			return nmbn__run(d);
		}
		return 0;
	}
	if ((r = nmbn__mkdirs(nmbn_run_dir)) < 0) return r;
	for (i = 0; i < n; i++) o += snprintf(body + o, sizeof body - o, "nameserver %s\n", ns[i]);
	if (search && *search) o += snprintf(body + o, sizeof body - o, "search %s\n", search);
	snprintf(path, sizeof path, "%s/dns.%s", nmbn_run_dir, ifn);
	if ((r = nmbn__write_atomic(path, body, o, 0644)) < 0) return r;
	return nmbn__dns_rebuild();
}
int nmbn_dns_clear(const char *ifn)
{
	char path[600];
	if (!nmbn__safe_name(ifn, 16)) return -EINVAL;
	if (nmbn__use_resolved()) {
		char *d[] = {"resolvectl", "revert", (char *)ifn, NULL};
		return nmbn__run(d);
	}
	snprintf(path, sizeof path, "%s/dns.%s", nmbn_run_dir, ifn);
	if (unlink(path) < 0 && errno == ENOENT) return 0;
	return nmbn__dns_rebuild();
}

/* ---------- DHCPv4 ---------- */
enum { DH_DISCOVER = 1, DH_OFFER, DH_REQUEST, DH_DECLINE, DH_ACK, DH_NAK, DH_RELEASE };

struct nmbn__dh {
	uint8_t op, htype, hlen, hops;
	uint32_t xid;
	uint16_t secs, flags;
	uint32_t ciaddr, yiaddr, siaddr, giaddr;
	uint8_t chaddr[16], sname[64], file[128];
	uint8_t magic[4];
	uint8_t opt[340];
} __attribute__((packed));

static uint32_t nmbn__xid(void)
{
	uint32_t x = 0;
	if (getrandom(&x, 4, 0) != 4) x = (uint32_t)nmbn__ms() * 2654435761u;
	return x;
}
static int nmbn__build(struct nmbn__dh *d, const nmbn_lease *l, int type, uint32_t xid, uint32_t ciaddr, uint32_t reqip,
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
static int nmbn__parse_reply(const uint8_t *p, size_t len, uint32_t xid, const unsigned char *mac, nmbn_lease *r)
{
	const struct nmbn__dh *d = (const void *)p;
	size_t i = offsetof(struct nmbn__dh, opt);
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
static int nmbn__dsock(const char *ifn, uint32_t bindip)
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
static int nmbn__send(int fd, const void *b, int n, uint32_t dst)
{
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(67), .sin_addr.s_addr = dst};
	return sendto(fd, b, n, 0, (struct sockaddr *)&a, sizeof a) == n ? 0 : -errno;
}
/* DISCOVER/REQUEST before we own an address must leave with source 0.0.0.0. A UDP socket cannot do that: the
 * kernel picks some other interface's address as source. So build IP+UDP ourselves on a packet socket. */
static uint16_t nmbn__csum(const void *p, int n)
{
	const uint8_t *b = p;
	uint32_t s = 0;
	for (; n > 1; n -= 2, b += 2) s += (b[0] << 8) | b[1];
	if (n) s += b[0] << 8;
	while (s >> 16) s = (s & 0xffff) + (s >> 16);
	return htons((uint16_t)~s);
}
static int nmbn__send_raw(const char *ifn, const void *dh, int n)
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
	ip->check = nmbn__csum(ip, sizeof *ip);
	u->source = htons(68), u->dest = htons(67), u->len = htons(sizeof *u + n); /* UDP checksum 0 = none, legal in IPv4 */
	memcpy(pkt + sizeof *ip + sizeof *u, dh, n);
	if ((fd = socket(AF_PACKET, SOCK_DGRAM | SOCK_CLOEXEC, 0)) < 0) return -errno;
	if (sendto(fd, pkt, tot, 0, (void *)&ll, sizeof ll) != tot) r = -errno;
	close(fd);
	return r;
}
/* wait up to ms for a reply of the wanted type (or NAK); returns the type, 0 on timeout */
static int nmbn__rxsock(const char *ifn)
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
static int nmbn__wait_(int fd, int raw, uint32_t xid, const unsigned char *mac, int want, int ms, nmbn_lease *r)
{
	long end = nmbn__ms() + ms;
	for (;;) {
		struct pollfd p = {fd, POLLIN, 0};
		uint8_t buf[1500];
		long left = end - nmbn__ms();
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
		t = nmbn__parse_reply(pl, n, xid, mac, r);
		if (t == want || t == DH_NAK) return t;
	}
}
static int nmbn__wait(int fd, uint32_t xid, const unsigned char *mac, int want, int ms, nmbn_lease *r)
{
	return nmbn__wait_(fd, 0, xid, mac, want, ms, r);
}
static void nmbn__finalize(nmbn_lease *l)
{
	if (!l->mask) { /* classful fallback, RFC 2131 leaves the option mandatory in practice */
		uint8_t f = ((uint8_t *)&l->ip)[0];
		l->mask = htonl(f < 128 ? 0xff000000u : f < 192 ? 0xffff0000u : 0xffffff00u);
	}
	if (!l->lease_s) l->lease_s = 3600;
	if (l->lease_s != 0xffffffffu) {
		if (!l->t1 || l->t1 >= l->lease_s) l->t1 = l->lease_s / 2;
		if (!l->t2 || l->t2 >= l->lease_s || l->t2 <= l->t1) l->t2 = (uint32_t)((uint64_t)l->lease_s * 7 / 8);
	}
	l->got = nmbn__now();
}

int nmbn_dhcp_acquire(const char *ifn, int timeout_ms, nmbn_lease *L)
{
	nmbn_if f;
	nmbn_lease o, a;
	struct nmbn__dh d;
	long end = nmbn__ms() + timeout_ms;
	int fd, rx, r, rto = nmbn_dhcp_rto_ms, n;
	if ((r = nmbn_find(ifn, &f)) < 0) return r;
	if (!f.up && (r = nmbn_link_set(ifn, 1)) < 0) return r;
	memset(&o, 0, sizeof o);
	nmbn__cpy(o.ifn, sizeof o.ifn, ifn);
	memcpy(o.mac, f.mac, 6);
	if ((fd = nmbn__dsock(ifn, 0)) < 0) return fd;
	if ((rx = nmbn__rxsock(ifn)) < 0) {
		close(fd);
		return rx;
	}
	for (;;) {
		uint32_t xid = nmbn__xid();
		long left = end - nmbn__ms();
		int w, t;
		if (left <= 0) {
			close(fd), close(rx);
			return -ETIMEDOUT;
		}
		w = rto < left ? rto : (int)left;
		n = nmbn__build(&d, &o, DH_DISCOVER, xid, 0, 0, 0, 1);
		if ((r = nmbn__send_raw(ifn, &d, n)) < 0) {
			close(fd), close(rx);
			return r;
		}
		memset(&a, 0, sizeof a);
		if ((t = nmbn__wait_(rx, 1, xid, o.mac, DH_OFFER, w, &a)) != DH_OFFER) {
			if (t == 0 && rto < 16000) rto *= 2;
			continue;
		}
		if (!a.ip || !a.server) continue; /* useless offer */
		n = nmbn__build(&d, &o, DH_REQUEST, xid, 0, a.ip, a.server, 1);
		if ((r = nmbn__send_raw(ifn, &d, n)) < 0) {
			close(fd), close(rx);
			return r;
		}
		left = end - nmbn__ms();
		w = nmbn_dhcp_rto_ms < left ? nmbn_dhcp_rto_ms : (int)left;
		if (w <= 0) continue;
		{
			nmbn_lease b;
			memset(&b, 0, sizeof b);
			t = nmbn__wait_(rx, 1, xid, o.mac, DH_ACK, w, &b);
			if (t == DH_ACK && b.ip) {
				b.ifn[0] = 0;
				nmbn__cpy(b.ifn, sizeof b.ifn, ifn);
				memcpy(b.mac, o.mac, 6);
				if (!b.server) b.server = a.server;
				nmbn__finalize(&b);
				*L = b;
				close(fd), close(rx);
				return 0;
			}
			/* NAK or silence: start over */
		}
	}
}

int nmbn_dhcp_renew(nmbn_lease *l, int broadcast, int timeout_ms)
{
	struct nmbn__dh d;
	nmbn_lease a;
	uint32_t xid = nmbn__xid();
	int fd = nmbn__dsock(l->ifn, l->ip), n, t, r;
	if (fd < 0) return fd;
	n = nmbn__build(&d, l, DH_REQUEST, xid, l->ip, 0, 0, 0);
	if ((r = nmbn__send(fd, &d, n, broadcast ? htonl(INADDR_BROADCAST) : l->server)) < 0) {
		close(fd);
		return r;
	}
	memset(&a, 0, sizeof a);
	t = nmbn__wait(fd, xid, l->mac, DH_ACK, timeout_ms, &a);
	close(fd);
	if (t == DH_NAK) return NMBN_E_NAK;
	if (t != DH_ACK) return -ETIMEDOUT;
	{
		int changed = a.ip != l->ip;
		if (!a.server) a.server = l->server;
		nmbn__cpy(a.ifn, sizeof a.ifn, l->ifn);
		memcpy(a.mac, l->mac, 6);
		nmbn__finalize(&a);
		*l = a;
		return changed;
	}
}
int nmbn_dhcp_release(const nmbn_lease *l)
{
	struct nmbn__dh d;
	int fd = nmbn__dsock(l->ifn, l->ip), n, r;
	if (fd < 0) return fd;
	n = nmbn__build(&d, l, DH_RELEASE, nmbn__xid(), l->ip, 0, l->server, 0);
	r = nmbn__send(fd, &d, n, l->server);
	close(fd);
	return r;
}

static int nmbn__plen(uint32_t mask_be)
{
	uint32_t m = ntohl(mask_be), inv = ~m;
	int n = 0;
	if (inv & (inv + 1)) return -1; /* holes in the mask */
	while (m) n += m & 1, m >>= 1;
	return n;
}
void nmbn_lease_print(FILE *f, const nmbn_lease *l)
{
	char a[INET_ADDRSTRLEN], b[INET_ADDRSTRLEN];
	int i;
	inet_ntop(AF_INET, &l->ip, a, sizeof a);
	fprintf(f, "ip %s/%d\n", a, nmbn__plen(l->mask));
	inet_ntop(AF_INET, &l->router, a, sizeof a);
	inet_ntop(AF_INET, &l->server, b, sizeof b);
	fprintf(f, "router %s\nserver %s\nlease %u t1 %u t2 %u\n", l->router ? a : "-", b, l->lease_s, l->t1, l->t2);
	for (i = 0; i < l->ndns; i++) {
		inet_ntop(AF_INET, &l->dns[i], a, sizeof a);
		fprintf(f, "dns %s\n", a);
	}
	if (l->domain[0]) fprintf(f, "domain %s\n", l->domain);
}
int nmbn_lease_apply(const nmbn_lease *l)
{
	char cidr[48], rt[INET_ADDRSTRLEN], dn[4][INET_ADDRSTRLEN];
	const char *ns[4];
	int p = nmbn__plen(l->mask), i, r;
	if (p < 0 || !l->ip) return -EINVAL;
	inet_ntop(AF_INET, &l->ip, cidr, sizeof cidr);
	snprintf(cidr + strlen(cidr), sizeof cidr - strlen(cidr), "/%d", p);
	if ((r = nmbn_addr_add(l->ifn, cidr)) < 0) return r;
	if (l->router) {
		inet_ntop(AF_INET, &l->router, rt, sizeof rt);
		if ((r = nmbn_route_add(l->ifn, "default", rt, nmbn_metric(l->ifn))) < 0) return r;
	}
	if (l->ndns) {
		for (i = 0; i < l->ndns; i++) inet_ntop(AF_INET, &l->dns[i], dn[i], sizeof dn[i]), ns[i] = dn[i];
		if ((r = nmbn_dns_set(l->ifn, ns, l->ndns, l->domain)) < 0) return r;
	}
	return 0;
}
int nmbn_lease_unapply(const nmbn_lease *l)
{
	char cidr[48], rt[INET_ADDRSTRLEN];
	int p = nmbn__plen(l->mask);
	if (l->router) {
		inet_ntop(AF_INET, &l->router, rt, sizeof rt);
		nmbn_route_del(l->ifn, "default", rt, nmbn_metric(l->ifn));
	}
	nmbn_dns_clear(l->ifn);
	if (p < 0) return 0;
	inet_ntop(AF_INET, &l->ip, cidr, sizeof cidr);
	snprintf(cidr + strlen(cidr), sizeof cidr - strlen(cidr), "/%d", p);
	return nmbn_addr_del(l->ifn, cidr);
}

int nmbn_dhcpc_step(nmbn_dhcpc *c, const char *ifn)
{
	time_t now = nmbn__now();
	nmbn_lease *l = &c->l;
	long age;
	int r;
	if (c->st == 0) {
		if (now < c->next_try) return -EAGAIN;
		if ((r = nmbn_dhcp_connect(ifn, c->key, nmbn_dhcp_timeout_ms, l, NULL)) < 0) {
			c->next_try = now + 5;
			return r;
		}
		if ((r = nmbn_lease_apply(l)) < 0) return r;
		c->st = 1, c->next_try = 0;
		return 0;
	}
	if (l->lease_s == 0xffffffffu) return 0;
	age = now - l->got;
	if (age >= (long)l->lease_s) { /* expired: drop everything, start over next call */
		nmbn_lease_unapply(l);
		c->st = 0, c->next_try = 0;
		return 1;
	}
	if (age < (long)l->t1 || now < c->next_try) return 0;
	{
		nmbn_lease old = *l;
		int bc = age >= (long)l->t2; /* T2: rebind, ask anyone */
		r = nmbn_dhcp_renew(l, bc, 1000);
		if (r == NMBN_E_NAK) { /* server revoked the lease */
			nmbn_lease_unapply(&old);
			c->st = 0, c->next_try = 0;
			return 1;
		}
		if (r < 0) {
			long rest = (long)(bc ? l->lease_s : l->t2) - age;
			c->next_try = now + (rest / 2 > 1 ? rest / 2 : 1); /* RFC 2131: half the remaining time, min 1 s */
			return r;
		}
		if (r == 1) nmbn_lease_unapply(&old);
		if (c->key[0]) nmbn_lease_save(c->key, l);
		c->next_try = 0;
		return nmbn_lease_apply(l);
	}
}
int nmbn_dhcpc_stop(nmbn_dhcpc *c, int release)
{
	if (c->st != 1) return 0;
	if (release) nmbn_dhcp_release(&c->l);
	nmbn_lease_unapply(&c->l);
	c->st = 0;
	return 0;
}

/* ---------- roaming helpers ---------- */
int nmbn_metric(const char *ifn)
{
	return (!strncmp(ifn, "wl", 2) || !strncmp(ifn, "wlan", 4)) ? 600 : 100; /* wired beats wifi */
}
int nmbn_iface_reset(const char *ifn)
{
	nmbn_if f;
	int r = nmbn_find(ifn, &f);
	if (r < 0) return r;
	if (f.gw4[0]) nmbn_route_del(ifn, "default", f.gw4, nmbn_metric(ifn));
	if (f.gw6[0]) nmbn_route_del(ifn, "default", f.gw6, nmbn_metric(ifn));
	nmbn_dns_clear(ifn);
	return nmbn_addr_flush(ifn);
}
int nmbn_mac_set(const char *ifn, const unsigned char mac[6])
{
	nmbn_if f;
	nmbn__req q;
	int r = nmbn_find(ifn, &f), was_up;
	if (r < 0) return r;
	was_up = f.up;
	if (was_up && (r = nmbn_link_set(ifn, 0)) < 0) return r; /* the kernel refuses MAC changes on a running link */
	nmbn__req_init(&q, RTM_NEWLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	q.m.i.ifi_index = f.idx;
	nmbn__attr(&q, IFLA_ADDRESS, mac, 6);
	r = nmbn__nl(&q, NULL, NULL);
	if (was_up) nmbn_link_set(ifn, 1);
	return r;
}
void nmbn_mac_for(const char *key, unsigned char mac[6])
{
	char path[600];
	unsigned char sec[16];
	uint64_t h = 1469598103934665603ull;
	size_t i;
	snprintf(path, sizeof path, "%s/mac.secret", nmbn_state_dir);
	{
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		ssize_t n = fd >= 0 ? read(fd, sec, sizeof sec) : -1;
		if (fd >= 0) close(fd);
		if (n != (ssize_t)sizeof sec) { /* first use: create the per-machine secret */
			if (getrandom(sec, sizeof sec, 0) != (ssize_t)sizeof sec) memset(sec, 0x5a, sizeof sec);
			if (nmbn__mkdirs(nmbn_state_dir) == 0) nmbn__write_atomic(path, (char *)sec, sizeof sec, 0600);
		}
	}
	for (i = 0; i < sizeof sec; i++) h = (h ^ sec[i]) * 1099511628211ull;
	for (i = 0; key[i]; i++) h = (h ^ (unsigned char)key[i]) * 1099511628211ull;
	for (i = 0; i < 6; i++) mac[i] = (unsigned char)(h >> (i * 8));
	mac[0] = (mac[0] & 0xfe) | 0x02; /* unicast, locally administered */
}

/* ---------- lease cache ---------- */
static void nmbn__lpath(char *d, size_t n, const char *key) { snprintf(d, n, "%s/lease.%s", nmbn_state_dir, key); }
int nmbn_lease_save(const char *key, const nmbn_lease *l)
{
	char p[600], b[1024], ip[16][INET_ADDRSTRLEN];
	int o, i;
	if (!nmbn__safe_name(key, 64)) return -EINVAL;
	nmbn__mkdirs(nmbn_state_dir);
	inet_ntop(AF_INET, &l->ip, ip[0], sizeof ip[0]);
	inet_ntop(AF_INET, &l->mask, ip[1], sizeof ip[1]);
	inet_ntop(AF_INET, &l->router, ip[2], sizeof ip[2]);
	inet_ntop(AF_INET, &l->server, ip[3], sizeof ip[3]);
	o = snprintf(b, sizeof b, "ip=%s\nmask=%s\nrouter=%s\nserver=%s\nlease=%u\nt1=%u\nt2=%u\nsaved=%ld\ndomain=%s\ndns=", ip[0], ip[1],
		     ip[2], ip[3], l->lease_s, l->t1, l->t2, (long)time(NULL), l->domain);
	for (i = 0; i < l->ndns; i++) {
		inet_ntop(AF_INET, &l->dns[i], ip[4], sizeof ip[4]);
		o += snprintf(b + o, sizeof b - o, "%s%s", i ? "," : "", ip[4]);
	}
	o += snprintf(b + o, sizeof b - o, "\n");
	nmbn__lpath(p, sizeof p, key);
	return nmbn__write_atomic(p, b, o, 0600);
}
void nmbn_lease_forget(const char *key)
{
	char p[600];
	if (!nmbn__safe_name(key, 64)) return;
	nmbn__lpath(p, sizeof p, key);
	unlink(p);
}
int nmbn_lease_load(const char *key, const char *ifn, nmbn_lease *l)
{
	char p[600], b[1024], *ln, *sv;
	long saved = 0;
	if (!nmbn__safe_name(key, 64)) return -EINVAL;
	nmbn__lpath(p, sizeof p, key);
	if (nmbn__slurp(p, b, sizeof b) < 0) return -ENOENT;
	memset(l, 0, sizeof *l);
	nmbn__cpy(l->ifn, sizeof l->ifn, ifn);
	for (ln = strtok_r(b, "\n", &sv); ln; ln = strtok_r(NULL, "\n", &sv)) {
		char *v = strchr(ln, '=');
		if (!v) continue;
		*v++ = 0;
		if (!strcmp(ln, "ip")) inet_pton(AF_INET, v, &l->ip);
		else if (!strcmp(ln, "mask")) inet_pton(AF_INET, v, &l->mask);
		else if (!strcmp(ln, "router")) inet_pton(AF_INET, v, &l->router);
		else if (!strcmp(ln, "server")) inet_pton(AF_INET, v, &l->server);
		else if (!strcmp(ln, "lease")) l->lease_s = strtoul(v, NULL, 10);
		else if (!strcmp(ln, "t1")) l->t1 = strtoul(v, NULL, 10);
		else if (!strcmp(ln, "t2")) l->t2 = strtoul(v, NULL, 10);
		else if (!strcmp(ln, "saved")) saved = strtol(v, NULL, 10);
		else if (!strcmp(ln, "domain")) nmbn__cpy(l->domain, sizeof l->domain, v);
		else if (!strcmp(ln, "dns")) {
			char *t, *s2;
			for (t = strtok_r(v, ",", &s2); t && l->ndns < 4; t = strtok_r(NULL, ",", &s2))
				if (inet_pton(AF_INET, t, &l->dns[l->ndns]) == 1) l->ndns++;
		}
	}
	/* a lease we cannot prove is still valid is not worth a REQUEST; the server would NAK it anyway */
	if (!l->ip || !l->mask || (l->lease_s != 0xffffffffu && time(NULL) - saved >= (long)l->lease_s)) return -ESTALE;
	return 0;
}

int nmbn_dhcp_reboot(const char *ifn, const nmbn_lease *cached, int timeout_ms, nmbn_lease *L)
{
	nmbn_if f;
	nmbn_lease o;
	struct nmbn__dh d;
	long end = nmbn__ms() + timeout_ms;
	int fd, rx, r, n, each = timeout_ms / 3 > 100 ? timeout_ms / 3 : 100;
	uint32_t xid = nmbn__xid();
	if ((r = nmbn_find(ifn, &f)) < 0) return r;
	if (!f.up && (r = nmbn_link_set(ifn, 1)) < 0) return r;
	memset(&o, 0, sizeof o);
	nmbn__cpy(o.ifn, sizeof o.ifn, ifn);
	memcpy(o.mac, f.mac, 6);
	if ((fd = nmbn__dsock(ifn, 0)) < 0) return fd;
	if ((rx = nmbn__rxsock(ifn)) < 0) {
		close(fd);
		return rx;
	}
	while (nmbn__ms() < end) {
		nmbn_lease a;
		long left = end - nmbn__ms();
		int t;
		n = nmbn__build(&d, &o, DH_REQUEST, xid, 0, cached->ip, 0, 1);
		if ((r = nmbn__send_raw(ifn, &d, n)) < 0) break;
		memset(&a, 0, sizeof a);
		t = nmbn__wait_(rx, 1, xid, o.mac, DH_ACK, each < left ? each : (int)left, &a);
		if (t == DH_NAK) {
			close(fd), close(rx);
			return NMBN_E_NAK;
		}
		if (t == DH_ACK && a.ip) {
			nmbn__cpy(a.ifn, sizeof a.ifn, ifn);
			memcpy(a.mac, o.mac, 6);
			nmbn__finalize(&a);
			*L = a;
			close(fd), close(rx);
			return 0;
		}
		r = -ETIMEDOUT;
	}
	close(fd), close(rx);
	return r < 0 ? r : -ETIMEDOUT;
}
int nmbn_dhcp_connect(const char *ifn, const char *key, int timeout_ms, nmbn_lease *l, int *cached)
{
	nmbn_lease c;
	int r;
	if (cached) *cached = 0;
	if (key && *key && nmbn_lease_load(key, ifn, &c) == 0) {
		r = nmbn_dhcp_reboot(ifn, &c, nmbn_reboot_timeout_ms, l);
		if (r == 0) {
			if (cached) *cached = 1;
			nmbn_lease_save(key, l);
			return 0;
		}
		nmbn_lease_forget(key); /* NAK or silence: this network is not (or no longer) the cached one */
	}
	if ((r = nmbn_dhcp_acquire(ifn, timeout_ms, l)) == 0 && key && *key) nmbn_lease_save(key, l);
	return r;
}

/* ---------- connectivity probe ---------- */
int nmbn_probe(const char *ifn, int timeout_ms)
{
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(nmbn_probe_port)};
	char req[512], resp[256];
	int fd, n, r = 0, code;
	long end = nmbn__ms() + timeout_ms;
	struct pollfd p;
	if (!nmbn_probe_ip || !*nmbn_probe_ip || inet_pton(AF_INET, nmbn_probe_ip, &a.sin_addr) != 1) return -EINVAL;
	if ((fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0) return -errno;
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifn, strlen(ifn) + 1) < 0) {
		r = -errno;
		goto out;
	}
	if (connect(fd, (void *)&a, sizeof a) < 0 && errno != EINPROGRESS) {
		r = -errno;
		goto out;
	}
	p.fd = fd, p.events = POLLOUT;
	if (poll(&p, 1, timeout_ms) <= 0) {
		r = -ETIMEDOUT;
		goto out;
	}
	{
		int e = 0;
		socklen_t el = sizeof e;
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
		if (e) {
			r = -e;
			goto out;
		}
	}
	n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: netmanager_blue\r\n\r\n", nmbn_probe_path,
		     nmbn_probe_host);
	if (send(fd, req, n, MSG_NOSIGNAL) != n) {
		r = -errno;
		goto out;
	}
	n = 0;
	while (n < 12) {
		long left = end - nmbn__ms();
		ssize_t k;
		p.events = POLLIN;
		if (left <= 0 || poll(&p, 1, (int)left) <= 0) {
			r = -ETIMEDOUT;
			goto out;
		}
		if ((k = recv(fd, resp + n, sizeof resp - 1 - n, 0)) <= 0) break;
		n += k;
	}
	resp[n > 0 ? n : 0] = 0;
	if (n < 12 || strncmp(resp, "HTTP/1.", 7)) {
		r = -EPROTO;
		goto out;
	}
	code = atoi(resp + 9);
	r = code == 204 ? 0 : 1; /* a redirect/login page where a 204 should be = captive portal */
out:
	close(fd);
	return r;
}

/* ---------- profiles ---------- */
static char *nmbn__trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t') s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
	return s;
}
static int nmbn__route_ok(const char *s)
{
	char t[96], *v;
	nmbn__ip a, g;
	snprintf(t, sizeof t, "%s", s);
	if ((v = strstr(t, " via "))) {
		*v = 0;
		v += 5;
		if (nmbn__parse(v, &g, 0) < 0 || g.plen >= 0) return 0;
	}
	return nmbn__parse(t, &a, 1) == 0;
}
static int nmbn__profile_read(const char *path, const char *name, nmbn_profile *p)
{
	char buf[4096], *l, *save;
	int r;
	memset(p, 0, sizeof *p);
	if ((r = nmbn__slurp(path, buf, sizeof buf)) < 0) return nmbn__fail(r, "cannot read %s: %s", path, strerror(-r));
	nmbn__cpy(p->name, sizeof p->name, name);
	for (l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		char *k = nmbn__trim(l), *v;
		if (!*k || *k == '#') continue;
		if (!(v = strchr(k, '='))) return nmbn__fail(-EINVAL, "%s: line without '=': %s", path, k);
		*v++ = 0;
		k = nmbn__trim(k), v = nmbn__trim(v);
		if (!strcmp(k, "iface")) {
			if (!nmbn__safe_name(v, sizeof p->iface)) return nmbn__fail(-EINVAL, "%s: bad iface '%s'", path, v);
			nmbn__cpy(p->iface, sizeof p->iface, v);
		} else if (!strcmp(k, "method")) {
			if (!strcmp(v, "dhcp")) p->dhcp = 1;
			else if (!strcmp(v, "static")) p->dhcp = 0;
			else return nmbn__fail(-EINVAL, "%s: method must be dhcp or static", path);
		} else if (!strcmp(k, "address")) {
			nmbn__ip a;
			if (nmbn__parse(v, &a, 1) < 0) return nmbn__fail(-EINVAL, "%s: bad address '%s'", path, v);
			nmbn__cpy(p->addr, sizeof p->addr, v);
		} else if (!strcmp(k, "gateway")) {
			nmbn__ip a;
			if (nmbn__parse(v, &a, 0) < 0 || a.plen >= 0) return nmbn__fail(-EINVAL, "%s: bad gateway '%s'", path, v);
			nmbn__cpy(p->gateway, sizeof p->gateway, v);
		} else if (!strcmp(k, "dns")) {
			char *s2, *t;
			for (t = strtok_r(v, ", ", &s2); t; t = strtok_r(NULL, ", ", &s2)) {
				nmbn__ip a;
				if (p->ndns >= 3) return nmbn__fail(-EINVAL, "%s: at most 3 dns servers", path);
				if (nmbn__parse(t, &a, 0) < 0 || a.plen >= 0) return nmbn__fail(-EINVAL, "%s: bad dns '%s'", path, t);
				nmbn__cpy(p->dns[p->ndns++], 46, t);
			}
		} else if (!strcmp(k, "route")) {
			if (p->nroute >= NMBN_MAXROUTE || !nmbn__route_ok(v)) return nmbn__fail(-EINVAL, "%s: bad route '%s'", path, v);
			nmbn__cpy(p->route[p->nroute++], 96, v);
		} else if (!strcmp(k, "priority")) {
			char *e;
			long pv = strtol(v, &e, 10);
			if (!*v || *e || pv < -1000 || pv > 1000) return nmbn__fail(-EINVAL, "%s: bad priority", path);
			p->priority = (int)pv;
		} else if (!strcmp(k, "randmac")) {
			p->randmac = !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "true");
		} else if (!strcmp(k, "autoconnect")) {
			p->autoconnect = !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "true");
		} else return nmbn__fail(-EINVAL, "%s: unknown key '%s'", path, k);
	}
	if (!p->iface[0]) return nmbn__fail(-EINVAL, "%s: iface= is required", path);
	if (!p->dhcp && !p->addr[0]) return nmbn__fail(-EINVAL, "%s: static profile needs address=", path);
	return 0;
}
int nmbn_profile_load(const char *name, nmbn_profile *p)
{
	char path[600];
	if (!nmbn__safe_name(name, sizeof p->name)) return nmbn__fail(-EINVAL, "bad profile name");
	snprintf(path, sizeof path, "%s/%s.conf", nmbn_conf_dir, name);
	return nmbn__profile_read(path, name, p);
}
int nmbn_profile_save(const nmbn_profile *p)
{
	char path[600], b[2048];
	int o = 0, i, r;
	if (!nmbn__safe_name(p->name, sizeof p->name)) return nmbn__fail(-EINVAL, "bad profile name");
	if (!nmbn__safe_name(p->iface, sizeof p->iface)) return nmbn__fail(-EINVAL, "bad iface");
	if (!p->dhcp && !p->addr[0]) return nmbn__fail(-EINVAL, "static profile needs an address");
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
	if ((r = nmbn__mkdirs(nmbn_conf_dir)) < 0) return r;
	snprintf(path, sizeof path, "%s/%s.conf", nmbn_conf_dir, p->name);
	/* validate exactly what we wrote: save never leaves a file that load would refuse */
	{
		nmbn_profile chk;
		char tmp[600];
		int fd;
		snprintf(tmp, sizeof tmp, "%s/.save.XXXXXX", nmbn_conf_dir);
		if ((fd = mkstemp(tmp)) < 0) return -errno;
		if (write(fd, b, o) != o || fchmod(fd, 0644) < 0 || fsync(fd) < 0) {
			r = -errno;
			close(fd);
			unlink(tmp);
			return r;
		}
		close(fd);
		if ((r = nmbn__profile_read(tmp, p->name, &chk)) < 0 || rename(tmp, path) < 0) {
			if (r >= 0) r = -errno;
			unlink(tmp);
			return r;
		}
	}
	return 0;
}
static int nmbn__cmp(const void *a, const void *b) { return strcmp(a, b); }
int nmbn_profile_list(char names[][64], int max)
{
	DIR *d = opendir(nmbn_conf_dir);
	struct dirent *e;
	int n = 0;
	if (!d) return errno == ENOENT ? 0 : -errno;
	while ((e = readdir(d)) && n < max) {
		size_t l = strlen(e->d_name);
		if (l < 6 || strcmp(e->d_name + l - 5, ".conf") || l - 5 >= 64) continue;
		memcpy(names[n], e->d_name, l - 5);
		names[n][l - 5] = 0;
		if (nmbn__safe_name(names[n], 64)) n++;
	}
	closedir(d);
	qsort(names, n, 64, nmbn__cmp);
	return n;
}
int nmbn_profile_apply(const nmbn_profile *p, nmbn_lease *l)
{
	int i, r;
	const char *ns[3];
	if ((r = nmbn_link_set(p->iface, 1)) < 0) return r;
	if (p->dhcp) {
		nmbn_lease t;
		if (p->randmac) {
			unsigned char m[6];
			nmbn_mac_for(p->name, m);
			if ((r = nmbn_mac_set(p->iface, m)) < 0) return r;
		}
		if ((r = nmbn_dhcp_connect(p->iface, p->name, nmbn_dhcp_timeout_ms, &t, NULL)) < 0) return r;
		if ((r = nmbn_lease_apply(&t)) < 0) return r;
		if (l) *l = t;
		return 0;
	}
	if ((r = nmbn_addr_add(p->iface, p->addr)) < 0) return r;
	if (p->gateway[0] && (r = nmbn_route_add(p->iface, "default", p->gateway, nmbn_metric(p->iface))) < 0) return r;
	for (i = 0; i < p->nroute; i++) {
		char t[96], *v = strstr(strcpy(t, p->route[i]), " via ");
		if (v) *v = 0, v += 5;
		if ((r = nmbn_route_add(p->iface, t, v, 0)) < 0) return r;
	}
	for (i = 0; i < p->ndns; i++) ns[i] = p->dns[i];
	if (p->ndns && (r = nmbn_dns_set(p->iface, ns, p->ndns, NULL)) < 0) return r;
	return 0;
}
int nmbn_profile_down(const nmbn_profile *p, const nmbn_lease *l)
{
	int i;
	if (p->dhcp) {
		if (l) {
			nmbn_dhcp_release(l);
			return nmbn_lease_unapply(l);
		}
		return 0;
	}
	for (i = 0; i < p->nroute; i++) {
		char t[96], *v = strstr(strcpy(t, p->route[i]), " via ");
		if (v) *v = 0, v += 5;
		nmbn_route_del(p->iface, t, v, 0);
	}
	if (p->gateway[0]) nmbn_route_del(p->iface, "default", p->gateway, nmbn_metric(p->iface));
	nmbn_dns_clear(p->iface);
	return nmbn_addr_del(p->iface, p->addr);
}

/* ---------- autoconnect (event driven) ---------- */
#define NMBN_SLOTS 16
typedef struct {
	char name[64];
	nmbn_profile p;
	nmbn_dhcpc d;
	int valid, active, fails;
	time_t backoff, last_ok;
} nmbn__slot;

static int nmbn__bound(const nmbn__slot *s) { return s->p.dhcp ? s->d.st == 1 : s->active; }
static void nmbn__down(nmbn__slot *s) /* clean teardown: the next network starts from nothing */
{
	if (nmbn__bound(s) || s->active || s->d.st) nmbn_iface_reset(s->p.iface);
	s->active = 0, s->d.st = 0, s->d.next_try = 0;
}
static void nmbn__penalize(nmbn__slot *s)
{
	int e = s->fails < 3 ? s->fails : 3;
	s->fails++;
	/* 1, 2, 4 s while it might just be slow, then 5 min: a network that failed 3 times in a row is dropped,
	 * because every retry of a higher-priority profile costs a reconnect of the one that works */
	s->backoff = nmbn__now() + (e < 3 ? (1 << e) : 300);
}
static time_t nmbn__due(const nmbn__slot *s)
{
	time_t now = nmbn__now(), best = 0, t[4];
	int i;
	if (s->backoff > now) best = s->backoff;
	if (s->p.dhcp && s->d.st == 1 && s->d.l.lease_s != 0xffffffffu) {
		const nmbn_lease *l = &s->d.l;
		t[0] = l->got + l->t1, t[1] = l->got + l->t2, t[2] = l->got + l->lease_s, t[3] = s->d.next_try;
		for (i = 0; i < 4; i++)
			if (t[i] > now && (!best || t[i] < best)) best = t[i];
	}
	return best;
}
static nmbn__slot *nmbn__best(nmbn__slot *S, int ns, const char *ifn, time_t now)
{
	nmbn__slot *b = NULL;
	int i;
	for (i = 0; i < ns; i++) {
		nmbn__slot *s = &S[i];
		if (!s->valid || strcmp(s->p.iface, ifn) || s->backoff > now) continue;
		if (!b || s->p.priority > b->p.priority || (s->p.priority == b->p.priority && s->last_ok > b->last_ok)) b = s;
	}
	return b;
}
/* one reconcile pass; returns the next time something needs doing (0 = nothing scheduled) */
static time_t nmbn__pass(nmbn__slot *S, int *nsp)
{
	char names[NMBN_SLOTS][64];
	nmbn_if ifs[NMBN_MAXIF];
	int n = nmbn_profile_list(names, NMBN_SLOTS), nif = nmbn_list(ifs, NMBN_MAXIF), i, j, ns = *nsp;
	time_t now = nmbn__now(), next = 0;
	for (i = 0; i < ns; i++) S[i].valid = 0;
	for (i = 0; i < n; i++) {
		nmbn_profile p;
		nmbn__slot *s = NULL;
		for (j = 0; j < ns; j++)
			if (!strcmp(S[j].name, names[i])) s = &S[j];
		if (nmbn_profile_load(names[i], &p) < 0 || !p.autoconnect) continue;
		if (!s) {
			if (ns >= NMBN_SLOTS) continue;
			s = &S[ns++];
			memset(s, 0, sizeof *s);
			nmbn__cpy(s->name, sizeof s->name, names[i]);
			nmbn__cpy(s->d.key, sizeof s->d.key, names[i]);
		}
		s->p = p, s->valid = 1;
	}
	*nsp = ns;
	for (i = 0; i < ns; i++) /* profile removed or autoconnect switched off */
		if (!S[i].valid && (S[i].active || S[i].d.st)) nmbn__down(&S[i]);
	for (i = 0; i < ns; i++) {
		const char *ifn = S[i].p.iface;
		const nmbn_if *f = NULL;
		int tries;
		if (!S[i].valid) continue;
		for (j = 0; j < i; j++)
			if (S[j].valid && !strcmp(S[j].p.iface, ifn)) break;
		if (j < i) continue; /* interface already handled */
		for (j = 0; j < nif; j++)
			if (!strcmp(ifs[j].name, ifn)) f = &ifs[j];
		if (f && !f->up) nmbn_link_set(ifn, 1); /* its LOWER_UP event will bring us back here */
		if (!f || !f->carrier) { /* cable out / interface gone: forget, redo on return */
			for (j = 0; j < ns; j++)
				if (S[j].valid && !strcmp(S[j].p.iface, ifn)) nmbn__down(&S[j]);
			continue;
		}
		for (tries = 0; tries <= ns; tries++) {
			nmbn__slot *best = nmbn__best(S, ns, ifn, now), *cur = NULL;
			int r;
			for (j = 0; j < ns; j++)
				if (S[j].valid && !strcmp(S[j].p.iface, ifn) && nmbn__bound(&S[j])) cur = &S[j];
			if (cur && cur->backoff > now) { /* current one was judged bad (probe): leave it */
				nmbn__down(cur);
				cur = NULL;
			}
			if (!best) break;
			if (cur && cur != best) {
				if (best->p.priority <= cur->p.priority) best = cur;
				else nmbn__down(cur), cur = NULL;
			}
			if (cur == best) { /* already connected: DHCP maintenance only */
				if (best->p.dhcp) nmbn_dhcpc_step(&best->d, ifn);
				if (best->p.dhcp && best->d.st == 0) continue; /* lease lost, loop acquires again */
				break;
			}
			best->d.next_try = 0;
			if (best->p.dhcp) {
				if (best->p.randmac) {
					unsigned char m[6];
					nmbn_mac_for(best->name, m);
					nmbn_mac_set(ifn, m);
				}
				r = nmbn_dhcpc_step(&best->d, ifn);
			} else {
				r = nmbn_profile_apply(&best->p, NULL);
				if (r == 0) best->active = 1;
			}
			if (r < 0 || !nmbn__bound(best)) { /* fall through to the next candidate right away */
				nmbn_iface_reset(ifn); /* a failed apply may have left half of itself behind */
				best->active = 0, best->d.st = 0;
				nmbn__penalize(best);
				now = nmbn__now();
				continue;
			}
			best->fails = 0, best->last_ok = now;
			if (nmbn_probe_ip && *nmbn_probe_ip) { /* portal / dead uplink: prefer another candidate if there is one */
				int pr = nmbn_probe(ifn, 2000), other = 0;
				for (j = 0; j < ns; j++)
					if (&S[j] != best && S[j].valid && !strcmp(S[j].p.iface, ifn) && S[j].backoff <= now) other = 1;
				if (pr != 0 && other) {
					nmbn__down(best);
					nmbn__penalize(best);
					now = nmbn__now();
					continue;
				}
			}
			break;
		}
	}
	for (i = 0; i < ns; i++)
		if (S[i].valid) {
			time_t d = nmbn__due(&S[i]);
			if (d && (!next || d < next)) next = d;
		}
	return next;
}

int nmbn_autoconnect(volatile int *stop, int poll_ms)
{
	static nmbn__slot S[NMBN_SLOTS];
	struct sockaddr_nl sa = {.nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_IFADDR};
	sigset_t block, orig;
	int ns = 0, i, ev = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_ROUTE);
	if (ev < 0 || bind(ev, (void *)&sa, sizeof sa) < 0) return ev < 0 ? -errno : -errno;
	if (poll_ms <= 0) poll_ms = 30000;
	memset(S, 0, sizeof S);
	sigemptyset(&block);
	sigaddset(&block, SIGTERM), sigaddset(&block, SIGINT), sigaddset(&block, SIGALRM);
	sigprocmask(SIG_BLOCK, &block, &orig); /* signals only delivered inside ppoll: no lost wakeup */
	while (!*stop) {
		time_t due = nmbn__pass(S, &ns), now = nmbn__now();
		struct pollfd p = {ev, POLLIN, 0};
		struct timespec ts;
		long ms = due ? (long)(due - now) * 1000 : poll_ms;
		if (*stop) break;
		if (ms > poll_ms) ms = poll_ms;
		if (ms < 0) ms = 0;
		ts.tv_sec = ms / 1000, ts.tv_nsec = (ms % 1000) * 1000000L;
		if (ppoll(&p, 1, &ts, &orig) > 0) {
			char junk[8192];
			while (recv(ev, junk, sizeof junk, 0) > 0) {
			} /* coalesce a burst of events into one pass */
		}
	}
	sigprocmask(SIG_SETMASK, &orig, NULL);
	close(ev);
	for (i = 0; i < ns; i++) { /* leave the system clean on shutdown */
		if (S[i].p.dhcp && S[i].d.st) nmbn_dhcp_release(&S[i].d.l);
		nmbn__down(&S[i]);
	}
	return 0;
}

#endif /* NMB_NET_IMPLEMENTATION_DONE */
#endif /* NMB_NET_IMPLEMENTATION */
