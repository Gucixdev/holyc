/* roaming.h - autoconnect policy, lease cache, connectivity probe, random MAC. stb-style single header.
 *
 *   #define NETMAN_ROAMING_IMPLEMENTATION   in exactly one .c file (after NETMAN_NET_IMPLEMENTATION), then #include "roaming.h"
 *
 * Builds on net.h (same translation unit for the implementation, it shares net.h's static helpers).
 *   lease cache  netman_lease_save/load/forget, netman_dhcp_connect (INIT-REBOOT first, full DHCP on NAK/timeout)
 *   dhcp client  netman_dhcpc_step/stop (renew T1 / rebind T2 / expiry)
 *   probe        netman_probe (204 = internet, other = captive portal)
 *   mac          netman_mac_set, netman_mac_for (stable random per profile)
 *   profiles     netman_profile_apply/down
 *   autoconnect  netman_autoconnect: event driven, per-profile backoff, ranking, switch; asks scoring.h to leave dead nets
 */
#ifndef NETMAN_ROAMING_H
#define NETMAN_ROAMING_H

#include "net.h"

typedef struct {
	netman_lease l;
	int st; /* 0 idle, 1 bound */
	time_t next_try;
	char key[64]; /* lease-cache key (profile name, or SSID/BSSID from the wifi layer); "" = no cache */
} netman_dhcpc;

extern const char *netman_probe_ip;   /* connectivity probe target, "" = off */
extern const char *netman_probe_host; /* Host: header (default "connectivitycheck.gstatic.com") */
extern const char *netman_probe_path; /* default "/generate_204" */
extern int netman_probe_port;	      /* default 80 */

/* cache first, full DHCP only on NAK/timeout; *cached = 1 if the cache answered; saves the result */
int netman_dhcp_connect(const char *ifn, const char *key, int timeout_ms, netman_lease *l, int *cached);
int netman_lease_save(const char *key, const netman_lease *l);
int netman_lease_load(const char *key, const char *ifn, netman_lease *l); /* -ESTALE when expired */
void netman_lease_forget(const char *key);
int netman_dhcpc_step(netman_dhcpc *c, const char *ifn); /* call about once a second */
int netman_dhcpc_stop(netman_dhcpc *c, int release);

int netman_profile_apply(const netman_profile *p, netman_lease *l); /* l only filled for dhcp */
int netman_profile_down(const netman_profile *p, const netman_lease *l);
/* Event-driven (netlink link/addr/route + timers, no polling). poll_ms = longest idle sleep (0 = 30000).
 * Per interface: best profile by priority/last success, 1 s..60 s per-profile backoff on failure,
 * immediate fallthrough to the next candidate, clean switch (addresses, default routes, dns). */
int netman_autoconnect(volatile int *stop, int poll_ms);
/* Optional quality hook (scoring.h's netman_score_iface fits): called after a profile came up with the
 * probe result (or NETMAN_UNKNOWN-like -1000 when probing is off). A score below 20 makes roaming drop
 * this network for another candidate on the same interface, exactly like a failed probe does. */
extern int (*netman_roam_score)(const char *ifn, int probe_result);

int netman_iface_reset(const char *ifn); /* drop default routes, addresses and dns of ifn */
int netman_mac_set(const char *ifn, const unsigned char mac[6]);
void netman_mac_for(const char *key, unsigned char mac[6]); /* stable per key and machine, locally administered */
/* HTTP GET via ifn: 0 = 204 (internet), 1 = other answer (captive portal), <0 = no connectivity */
int netman_probe(const char *ifn, int timeout_ms);

#endif /* NETMAN_ROAMING_H */

#ifdef NETMAN_ROAMING_IMPLEMENTATION
#ifndef NETMAN_ROAMING_IMPLEMENTATION_DONE
#define NETMAN_ROAMING_IMPLEMENTATION_DONE

int (*netman_roam_score)(const char *ifn, int probe_result) = NULL;
const char *netman_probe_ip = "";
const char *netman_probe_host = "connectivitycheck.gstatic.com";
const char *netman_probe_path = "/generate_204";
int netman_probe_port = 80;

int netman_dhcpc_step(netman_dhcpc *c, const char *ifn)
{
	time_t now = netman__now();
	netman_lease *l = &c->l;
	long age;
	int r;
	if (c->st == 0) {
		if (now < c->next_try) return -EAGAIN;
		if ((r = netman_dhcp_connect(ifn, c->key, netman_dhcp_timeout_ms, l, NULL)) < 0) {
			c->next_try = now + 5;
			return r;
		}
		if ((r = netman_lease_apply(l)) < 0) return r;
		c->st = 1, c->next_try = 0;
		return 0;
	}
	if (l->lease_s == 0xffffffffu) return 0;
	age = now - l->got;
	if (age >= (long)l->lease_s) { /* expired: drop everything, start over next call */
		netman_lease_unapply(l);
		c->st = 0, c->next_try = 0;
		return 1;
	}
	if (age < (long)l->t1 || now < c->next_try) return 0;
	{
		netman_lease old = *l;
		int bc = age >= (long)l->t2; /* T2: rebind, ask anyone */
		r = netman_dhcp_renew(l, bc, 1000);
		if (r == NETMAN_E_NAK) { /* server revoked the lease */
			netman_lease_unapply(&old);
			c->st = 0, c->next_try = 0;
			return 1;
		}
		if (r < 0) {
			long rest = (long)(bc ? l->lease_s : l->t2) - age;
			c->next_try = now + (rest / 2 > 1 ? rest / 2 : 1); /* RFC 2131: half the remaining time, min 1 s */
			return r;
		}
		if (r == 1) netman_lease_unapply(&old);
		if (c->key[0]) netman_lease_save(c->key, l);
		c->next_try = 0;
		return netman_lease_apply(l);
	}
}
int netman_dhcpc_stop(netman_dhcpc *c, int release)
{
	if (c->st != 1) return 0;
	if (release) netman_dhcp_release(&c->l);
	netman_lease_unapply(&c->l);
	c->st = 0;
	return 0;
}

/* ---------- roaming helpers ---------- */
int netman_iface_reset(const char *ifn)
{
	netman_if f;
	int r = netman_find(ifn, &f);
	if (r < 0) return r;
	if (f.gw4[0]) netman_route_del(ifn, "default", f.gw4, netman_metric(ifn));
	if (f.gw6[0]) netman_route_del(ifn, "default", f.gw6, netman_metric(ifn));
	netman_dns_clear(ifn);
	return netman_addr_flush(ifn);
}
int netman_mac_set(const char *ifn, const unsigned char mac[6])
{
	netman_if f;
	netman__req q;
	int r = netman_find(ifn, &f), was_up;
	if (r < 0) return r;
	was_up = f.up;
	if (was_up && (r = netman_link_set(ifn, 0)) < 0) return r; /* the kernel refuses MAC changes on a running link */
	netman__req_init(&q, RTM_NEWLINK, NLM_F_ACK, sizeof(struct ifinfomsg));
	q.m.i.ifi_index = f.idx;
	netman__attr(&q, IFLA_ADDRESS, mac, 6);
	r = netman__nl(&q, NULL, NULL);
	if (was_up) netman_link_set(ifn, 1);
	return r;
}
void netman_mac_for(const char *key, unsigned char mac[6])
{
	char path[600];
	unsigned char sec[16];
	uint64_t h = 1469598103934665603ull;
	size_t i;
	snprintf(path, sizeof path, "%s/mac.secret", netman_state_dir);
	{
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		ssize_t n = fd >= 0 ? read(fd, sec, sizeof sec) : -1;
		if (fd >= 0) close(fd);
		if (n != (ssize_t)sizeof sec) { /* first use: create the per-machine secret */
			if (getrandom(sec, sizeof sec, 0) != (ssize_t)sizeof sec) memset(sec, 0x5a, sizeof sec);
			if (netman__mkdirs(netman_state_dir) == 0) netman__write_atomic(path, (char *)sec, sizeof sec, 0600);
		}
	}
	for (i = 0; i < sizeof sec; i++) h = (h ^ sec[i]) * 1099511628211ull;
	for (i = 0; key[i]; i++) h = (h ^ (unsigned char)key[i]) * 1099511628211ull;
	for (i = 0; i < 6; i++) mac[i] = (unsigned char)(h >> (i * 8));
	mac[0] = (mac[0] & 0xfe) | 0x02; /* unicast, locally administered */
}

/* ---------- lease cache ---------- */
static void netman__lpath(char *d, size_t n, const char *key) { snprintf(d, n, "%s/lease.%s", netman_state_dir, key); }
int netman_lease_save(const char *key, const netman_lease *l)
{
	char p[600], b[1024], ip[16][INET_ADDRSTRLEN];
	int o, i;
	if (!netman__safe_name(key, 64)) return -EINVAL;
	netman__mkdirs(netman_state_dir);
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
	netman__lpath(p, sizeof p, key);
	return netman__write_atomic(p, b, o, 0600);
}
void netman_lease_forget(const char *key)
{
	char p[600];
	if (!netman__safe_name(key, 64)) return;
	netman__lpath(p, sizeof p, key);
	unlink(p);
}
int netman_lease_load(const char *key, const char *ifn, netman_lease *l)
{
	char p[600], b[1024], *ln, *sv;
	long saved = 0;
	if (!netman__safe_name(key, 64)) return -EINVAL;
	netman__lpath(p, sizeof p, key);
	if (netman__slurp(p, b, sizeof b) < 0) return -ENOENT;
	memset(l, 0, sizeof *l);
	netman__cpy(l->ifn, sizeof l->ifn, ifn);
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
		else if (!strcmp(ln, "domain")) netman__cpy(l->domain, sizeof l->domain, v);
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

int netman_dhcp_connect(const char *ifn, const char *key, int timeout_ms, netman_lease *l, int *cached)
{
	netman_lease c;
	int r;
	if (cached) *cached = 0;
	if (key && *key && netman_lease_load(key, ifn, &c) == 0) {
		r = netman_dhcp_reboot(ifn, &c, netman_reboot_timeout_ms, l);
		if (r == 0) {
			if (cached) *cached = 1;
			netman_lease_save(key, l);
			return 0;
		}
		netman_lease_forget(key); /* NAK or silence: this network is not (or no longer) the cached one */
	}
	if ((r = netman_dhcp_acquire(ifn, timeout_ms, l)) == 0 && key && *key) netman_lease_save(key, l);
	return r;
}

/* ---------- connectivity probe ---------- */
int netman_probe(const char *ifn, int timeout_ms)
{
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(netman_probe_port)};
	char req[512], resp[256];
	int fd, n, r = 0, code;
	long end = netman__ms() + timeout_ms;
	struct pollfd p;
	if (!netman_probe_ip || !*netman_probe_ip || inet_pton(AF_INET, netman_probe_ip, &a.sin_addr) != 1) return -EINVAL;
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
	n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: netman\r\n\r\n", netman_probe_path,
		     netman_probe_host);
	if (send(fd, req, n, MSG_NOSIGNAL) != n) {
		r = -errno;
		goto out;
	}
	n = 0;
	while (n < 12) {
		long left = end - netman__ms();
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

int netman_profile_apply(const netman_profile *p, netman_lease *l)
{
	int i, r;
	const char *ns[3];
	if ((r = netman_link_set(p->iface, 1)) < 0) return r;
	if (p->dhcp) {
		netman_lease t;
		if (p->randmac) {
			unsigned char m[6];
			netman_mac_for(p->name, m);
			if ((r = netman_mac_set(p->iface, m)) < 0) return r;
		}
		if ((r = netman_dhcp_connect(p->iface, p->name, netman_dhcp_timeout_ms, &t, NULL)) < 0) return r;
		if ((r = netman_lease_apply(&t)) < 0) return r;
		if (l) *l = t;
		return 0;
	}
	if ((r = netman_addr_add(p->iface, p->addr)) < 0) return r;
	if (p->gateway[0] && (r = netman_route_add(p->iface, "default", p->gateway, netman_metric(p->iface))) < 0) return r;
	for (i = 0; i < p->nroute; i++) {
		char t[96], *v = strstr(strcpy(t, p->route[i]), " via ");
		if (v) *v = 0, v += 5;
		if ((r = netman_route_add(p->iface, t, v, 0)) < 0) return r;
	}
	for (i = 0; i < p->ndns; i++) ns[i] = p->dns[i];
	if (p->ndns && (r = netman_dns_set(p->iface, ns, p->ndns, NULL)) < 0) return r;
	return 0;
}
int netman_profile_down(const netman_profile *p, const netman_lease *l)
{
	int i;
	if (p->dhcp) {
		if (l) {
			netman_dhcp_release(l);
			return netman_lease_unapply(l);
		}
		return 0;
	}
	for (i = 0; i < p->nroute; i++) {
		char t[96], *v = strstr(strcpy(t, p->route[i]), " via ");
		if (v) *v = 0, v += 5;
		netman_route_del(p->iface, t, v, 0);
	}
	if (p->gateway[0]) netman_route_del(p->iface, "default", p->gateway, netman_metric(p->iface));
	netman_dns_clear(p->iface);
	return netman_addr_del(p->iface, p->addr);
}

/* ---------- autoconnect (event driven) ---------- */
#define NETMAN_SLOTS 16
typedef struct {
	char name[64];
	netman_profile p;
	netman_dhcpc d;
	int valid, active, fails;
	time_t backoff, last_ok;
} netman__slot;

static int netman__bound(const netman__slot *s) { return s->p.dhcp ? s->d.st == 1 : s->active; }
static void netman__down(netman__slot *s) /* clean teardown: the next network starts from nothing */
{
	if (netman__bound(s) || s->active || s->d.st) netman_iface_reset(s->p.iface);
	s->active = 0, s->d.st = 0, s->d.next_try = 0;
}
static void netman__penalize(netman__slot *s)
{
	int e = s->fails < 3 ? s->fails : 3;
	s->fails++;
	/* 1, 2, 4 s while it might just be slow, then 5 min: a network that failed 3 times in a row is dropped,
	 * because every retry of a higher-priority profile costs a reconnect of the one that works */
	s->backoff = netman__now() + (e < 3 ? (1 << e) : 300);
}
static time_t netman__due(const netman__slot *s)
{
	time_t now = netman__now(), best = 0, t[4];
	int i;
	if (s->backoff > now) best = s->backoff;
	if (s->p.dhcp && s->d.st == 1 && s->d.l.lease_s != 0xffffffffu) {
		const netman_lease *l = &s->d.l;
		t[0] = l->got + l->t1, t[1] = l->got + l->t2, t[2] = l->got + l->lease_s, t[3] = s->d.next_try;
		for (i = 0; i < 4; i++)
			if (t[i] > now && (!best || t[i] < best)) best = t[i];
	}
	return best;
}
static netman__slot *netman__best(netman__slot *S, int ns, const char *ifn, time_t now)
{
	netman__slot *b = NULL;
	int i;
	for (i = 0; i < ns; i++) {
		netman__slot *s = &S[i];
		if (!s->valid || strcmp(s->p.iface, ifn) || s->backoff > now) continue;
		if (!b || s->p.priority > b->p.priority || (s->p.priority == b->p.priority && s->last_ok > b->last_ok)) b = s;
	}
	return b;
}
/* one reconcile pass; returns the next time something needs doing (0 = nothing scheduled) */
static time_t netman__pass(netman__slot *S, int *nsp)
{
	char names[NETMAN_SLOTS][64];
	netman_if ifs[NETMAN_MAXIF];
	int n = netman_profile_list(names, NETMAN_SLOTS), nif = netman_list(ifs, NETMAN_MAXIF), i, j, ns = *nsp;
	time_t now = netman__now(), next = 0;
	for (i = 0; i < ns; i++) S[i].valid = 0;
	for (i = 0; i < n; i++) {
		netman_profile p;
		netman__slot *s = NULL;
		for (j = 0; j < ns; j++)
			if (!strcmp(S[j].name, names[i])) s = &S[j];
		if (netman_profile_load(names[i], &p) < 0 || !p.autoconnect) continue;
		if (!s) {
			if (ns >= NETMAN_SLOTS) continue;
			s = &S[ns++];
			memset(s, 0, sizeof *s);
			netman__cpy(s->name, sizeof s->name, names[i]);
			netman__cpy(s->d.key, sizeof s->d.key, names[i]);
		}
		s->p = p, s->valid = 1;
	}
	*nsp = ns;
	for (i = 0; i < ns; i++) /* profile removed or autoconnect switched off */
		if (!S[i].valid && (S[i].active || S[i].d.st)) netman__down(&S[i]);
	for (i = 0; i < ns; i++) {
		const char *ifn = S[i].p.iface;
		const netman_if *f = NULL;
		int tries;
		if (!S[i].valid) continue;
		for (j = 0; j < i; j++)
			if (S[j].valid && !strcmp(S[j].p.iface, ifn)) break;
		if (j < i) continue; /* interface already handled */
		for (j = 0; j < nif; j++)
			if (!strcmp(ifs[j].name, ifn)) f = &ifs[j];
		if (f && !f->up) netman_link_set(ifn, 1); /* its LOWER_UP event will bring us back here */
		if (!f || !f->carrier) { /* cable out / interface gone: forget, redo on return */
			for (j = 0; j < ns; j++)
				if (S[j].valid && !strcmp(S[j].p.iface, ifn)) netman__down(&S[j]);
			continue;
		}
		for (tries = 0; tries <= ns; tries++) {
			netman__slot *best = netman__best(S, ns, ifn, now), *cur = NULL;
			int r;
			for (j = 0; j < ns; j++)
				if (S[j].valid && !strcmp(S[j].p.iface, ifn) && netman__bound(&S[j])) cur = &S[j];
			if (cur && cur->backoff > now) { /* current one was judged bad (probe): leave it */
				netman__down(cur);
				cur = NULL;
			}
			if (!best) break;
			if (cur && cur != best) {
				if (best->p.priority <= cur->p.priority) best = cur;
				else netman__down(cur), cur = NULL;
			}
			if (cur == best) { /* already connected: DHCP maintenance only */
				if (best->p.dhcp) netman_dhcpc_step(&best->d, ifn);
				if (best->p.dhcp && best->d.st == 0) continue; /* lease lost, loop acquires again */
				break;
			}
			best->d.next_try = 0;
			if (best->p.dhcp) {
				if (best->p.randmac) {
					unsigned char m[6];
					netman_mac_for(best->name, m);
					netman_mac_set(ifn, m);
				}
				r = netman_dhcpc_step(&best->d, ifn);
			} else {
				r = netman_profile_apply(&best->p, NULL);
				if (r == 0) best->active = 1;
			}
			if (r < 0 || !netman__bound(best)) { /* fall through to the next candidate right away */
				netman_iface_reset(ifn); /* a failed apply may have left half of itself behind */
				best->active = 0, best->d.st = 0;
				netman__penalize(best);
				now = netman__now();
				continue;
			}
			best->fails = 0, best->last_ok = now;
			if ((netman_probe_ip && *netman_probe_ip) || netman_roam_score) { /* portal / dead uplink: prefer another candidate if there is one */
				int pr = (netman_probe_ip && *netman_probe_ip) ? netman_probe(ifn, 2000) : -1000, other = 0;
				for (j = 0; j < ns; j++)
					if (&S[j] != best && S[j].valid && !strcmp(S[j].p.iface, ifn) && S[j].backoff <= now) other = 1;
				if (netman_roam_score && netman_roam_score(ifn, pr) < 20) pr = -1; /* dead or weak: same as no connectivity */
				if (pr != 0 && pr != -1000 && other) {
					netman__down(best);
					netman__penalize(best);
					now = netman__now();
					continue;
				}
			}
			break;
		}
	}
	for (i = 0; i < ns; i++)
		if (S[i].valid) {
			time_t d = netman__due(&S[i]);
			if (d && (!next || d < next)) next = d;
		}
	return next;
}

int netman_autoconnect(volatile int *stop, int poll_ms)
{
	static netman__slot S[NETMAN_SLOTS];
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
		time_t due = netman__pass(S, &ns), now = netman__now();
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
		if (S[i].p.dhcp && S[i].d.st) netman_dhcp_release(&S[i].d.l);
		netman__down(&S[i]);
	}
	return 0;
}

#endif /* NETMAN_ROAMING_IMPLEMENTATION_DONE */
#endif /* NETMAN_ROAMING_IMPLEMENTATION */
