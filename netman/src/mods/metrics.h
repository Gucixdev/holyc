/* metrics.h - per-interface counters and rates for the TUI. stb-style single header, self-contained.
 *
 *   #define NETMAN_METRICS_IMPLEMENTATION   in exactly one .c file, then #include "metrics.h"
 *
 *   netman_metrics_poll(&m, now_ms)        refresh every interface and derive rates from the previous poll
 *                                          (netlink RTM_GETLINK/IFLA_STATS64 first, /sys/class/net fallback)
 *   netman_metrics_sysfs(name, &st)        one interface from /sys/class/net/NAME/{statistics,operstate,speed,carrier}
 *   netman_metrics_netlink(out, max)       all interfaces in one netlink dump (no rates)
 *   netman_metrics_rate(prev, cur, dt_ms)  counter delta per second; a counter that went backwards (reset, wrap,
 *                                          interface re-created) yields 0, never a huge bogus rate
 *   netman_metrics_fmt_rate / _fmt_bytes   "1.2 MB/s", "340 KiB" for the UI
 *   netman_metrics_find(&m, name)           (netman_metrics must be zero-initialised before the first poll)
 * Unprivileged; not thread-safe.
 */
#ifndef NETMAN_METRICS_H
#define NETMAN_METRICS_H

#include <stddef.h>
#include <stdint.h>

#define NETMAN_METRICS_MAXIF 32

typedef struct {
	char name[16];
	int idx;
	char oper[16]; /* "up", "down", "unknown", ... */
	int carrier;   /* 1/0, -1 unknown */
	int speed_mbps; /* -1 unknown (wifi, down links) */
	uint64_t rx_bytes, tx_bytes, rx_pkts, tx_pkts, rx_err, tx_err, rx_drop, tx_drop;
	long t_ms;     /* monotonic time of the sample */
	double rx_bps, tx_bps, rx_pps, tx_pps; /* bytes/packets per second since the previous poll; 0 on the first */
} netman_ifstat;

typedef struct {
	netman_ifstat s[NETMAN_METRICS_MAXIF];
	int n;
} netman_metrics;

double netman_metrics_rate(uint64_t prev, uint64_t cur, long dt_ms);
int netman_metrics_sysfs(const char *name, netman_ifstat *st);
int netman_metrics_netlink(netman_ifstat *out, int max);
int netman_metrics_poll(netman_metrics *m, long now_ms);
const netman_ifstat *netman_metrics_find(const netman_metrics *m, const char *name);
void netman_metrics_fmt_rate(char *buf, size_t n, double bytes_per_s);
void netman_metrics_fmt_bytes(char *buf, size_t n, uint64_t bytes);

#endif /* NETMAN_METRICS_H */

#ifdef NETMAN_METRICS_IMPLEMENTATION
#ifndef NETMAN_METRICS_IMPLEMENTATION_DONE
#define NETMAN_METRICS_IMPLEMENTATION_DONE

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

double netman_metrics_rate(uint64_t prev, uint64_t cur, long dt_ms)
{
	if (dt_ms <= 0 || cur < prev) return 0;
	return (double)(cur - prev) * 1000.0 / (double)dt_ms;
}

static int netman__mrd(const char *dir, const char *name, const char *file, char *buf, size_t n)
{
	char p[300];
	FILE *f;
	snprintf(p, sizeof p, "/sys/class/net/%.15s/%s%s%s", name, dir, *dir ? "/" : "", file);
	if (!(f = fopen(p, "r"))) return -errno;
	if (!fgets(buf, (int)n, f)) buf[0] = 0; /* speed/carrier read fails with EINVAL on a down link */
	fclose(f);
	buf[strcspn(buf, "\n")] = 0;
	return buf[0] ? 0 : -EIO;
}

static uint64_t netman__mu64(const char *name, const char *file)
{
	char b[32];
	return netman__mrd("statistics", name, file, b, sizeof b) == 0 ? strtoull(b, NULL, 10) : 0;
}

static void netman__mextra(netman_ifstat *st)
{
	char b[32];
	st->carrier = netman__mrd("", st->name, "carrier", b, sizeof b) == 0 ? atoi(b) : -1;
	st->speed_mbps = -1;
	if (netman__mrd("", st->name, "speed", b, sizeof b) == 0 && atoi(b) > 0) st->speed_mbps = atoi(b);
	if (netman__mrd("", st->name, "operstate", st->oper, sizeof st->oper) < 0) snprintf(st->oper, sizeof st->oper, "unknown");
}

int netman_metrics_sysfs(const char *name, netman_ifstat *st)
{
	char b[32];
	memset(st, 0, sizeof *st);
	if (!name || !*name || strchr(name, '/') || !strcmp(name, "..") || !strcmp(name, ".")) return -EINVAL;
	if (netman__mrd("", name, "ifindex", b, sizeof b) < 0) return -ENODEV;
	st->idx = atoi(b);
	snprintf(st->name, sizeof st->name, "%s", name);
	st->rx_bytes = netman__mu64(name, "rx_bytes"), st->tx_bytes = netman__mu64(name, "tx_bytes");
	st->rx_pkts = netman__mu64(name, "rx_packets"), st->tx_pkts = netman__mu64(name, "tx_packets");
	st->rx_err = netman__mu64(name, "rx_errors"), st->tx_err = netman__mu64(name, "tx_errors");
	st->rx_drop = netman__mu64(name, "rx_dropped"), st->tx_drop = netman__mu64(name, "tx_dropped");
	netman__mextra(st);
	return 0;
}

int netman_metrics_netlink(netman_ifstat *out, int max)
{
	struct {
		struct nlmsghdr h;
		struct ifinfomsg i;
	} req = {{sizeof req, RTM_GETLINK, NLM_F_REQUEST | NLM_F_DUMP, 1, 0}, {AF_UNSPEC, 0, 0, 0, 0, 0}};
	struct sockaddr_nl sa = {.nl_family = AF_NETLINK};
	char buf[32768];
	int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE), n = 0, done = 0;
	if (fd < 0) return -errno;
	if (sendto(fd, &req, sizeof req, 0, (struct sockaddr *)&sa, sizeof sa) < 0) {
		int e = -errno;
		close(fd);
		return e;
	}
	while (!done) {
		ssize_t len = recv(fd, buf, sizeof buf, 0);
		struct nlmsghdr *h = (struct nlmsghdr *)buf;
		if (len < 0) {
			int e = -errno;
			close(fd);
			return e;
		}
		for (; NLMSG_OK(h, (size_t)len); h = NLMSG_NEXT(h, len)) {
			struct ifinfomsg *im;
			struct rtattr *a;
			int al;
			netman_ifstat *st;
			if (h->nlmsg_type == NLMSG_DONE) { done = 1; break; }
			if (h->nlmsg_type == NLMSG_ERROR) { close(fd); return -EIO; }
			if (h->nlmsg_type != RTM_NEWLINK || n >= max) continue;
			im = NLMSG_DATA(h);
			st = &out[n];
			memset(st, 0, sizeof *st);
			st->idx = im->ifi_index;
			st->carrier = (im->ifi_flags & IFF_LOWER_UP) ? 1 : 0;
			st->speed_mbps = -1;
			snprintf(st->oper, sizeof st->oper, "unknown");
			for (a = IFLA_RTA(im), al = IFLA_PAYLOAD(h); RTA_OK(a, al); a = RTA_NEXT(a, al)) {
				if (a->rta_type == IFLA_IFNAME) snprintf(st->name, sizeof st->name, "%s", (char *)RTA_DATA(a));
				else if (a->rta_type == IFLA_OPERSTATE && RTA_PAYLOAD(a) >= 1) {
					static const char *const o[] = {"unknown", "notpresent", "down", "lowerlayerdown", "testing", "dormant", "up"};
					unsigned v = *(unsigned char *)RTA_DATA(a);
					snprintf(st->oper, sizeof st->oper, "%s", v < 7 ? o[v] : "unknown");
				} else if (a->rta_type == IFLA_STATS64 && RTA_PAYLOAD(a) >= sizeof(struct rtnl_link_stats64)) {
					struct rtnl_link_stats64 s;
					memcpy(&s, RTA_DATA(a), sizeof s); /* RTA_DATA is only 4-aligned */
					st->rx_bytes = s.rx_bytes, st->tx_bytes = s.tx_bytes;
					st->rx_pkts = s.rx_packets, st->tx_pkts = s.tx_packets;
					st->rx_err = s.rx_errors, st->tx_err = s.tx_errors;
					st->rx_drop = s.rx_dropped, st->tx_drop = s.tx_dropped;
				}
			}
			if (st->name[0]) n++;
		}
	}
	close(fd);
	return n;
}

int netman_metrics_poll(netman_metrics *m, long now_ms)
{
	netman_ifstat cur[NETMAN_METRICS_MAXIF];
	int n = netman_metrics_netlink(cur, NETMAN_METRICS_MAXIF), i, j;
	if (n < 0) { /* no netlink: walk sysfs */
		DIR *d = opendir("/sys/class/net");
		struct dirent *e;
		n = 0;
		if (!d) return -errno;
		while ((e = readdir(d)) && n < NETMAN_METRICS_MAXIF)
			if (e->d_name[0] != '.' && netman_metrics_sysfs(e->d_name, &cur[n]) == 0) n++;
		closedir(d);
	}
	for (i = 0; i < n; i++) {
		netman_ifstat *c = &cur[i];
		c->t_ms = now_ms;
		if (c->speed_mbps == -1 && c->carrier != 0) { /* speed/carrier only live in sysfs */
			char b[32];
			if (netman__mrd("", c->name, "speed", b, sizeof b) == 0 && atoi(b) > 0) c->speed_mbps = atoi(b);
		}
		for (j = 0; j < m->n; j++)
			if (!strcmp(m->s[j].name, c->name) && m->s[j].idx == c->idx) {
				long dt = now_ms - m->s[j].t_ms;
				c->rx_bps = netman_metrics_rate(m->s[j].rx_bytes, c->rx_bytes, dt);
				c->tx_bps = netman_metrics_rate(m->s[j].tx_bytes, c->tx_bytes, dt);
				c->rx_pps = netman_metrics_rate(m->s[j].rx_pkts, c->rx_pkts, dt);
				c->tx_pps = netman_metrics_rate(m->s[j].tx_pkts, c->tx_pkts, dt);
				break;
			}
	}
	memcpy(m->s, cur, (size_t)n * sizeof cur[0]);
	m->n = n;
	return n;
}

const netman_ifstat *netman_metrics_find(const netman_metrics *m, const char *name)
{
	int i;
	for (i = 0; i < m->n; i++)
		if (!strcmp(m->s[i].name, name)) return &m->s[i];
	return NULL;
}

void netman_metrics_fmt_bytes(char *buf, size_t n, uint64_t b)
{
	static const char *const u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
	double v = (double)b;
	int k = 0;
	while (v >= 1024 && k < 4) v /= 1024, k++;
	if (k == 0) snprintf(buf, n, "%llu B", (unsigned long long)b);
	else snprintf(buf, n, "%.1f %s", v, u[k]);
}

void netman_metrics_fmt_rate(char *buf, size_t n, double bps)
{
	static const char *const u[] = {"B/s", "KB/s", "MB/s", "GB/s"};
	int k = 0;
	if (bps < 0) bps = 0;
	while (bps >= 1000 && k < 3) bps /= 1000, k++;
	if (k == 0) snprintf(buf, n, "%.0f %s", bps, u[k]);
	else snprintf(buf, n, "%.1f %s", bps, u[k]);
}

#endif /* NETMAN_METRICS_IMPLEMENTATION_DONE */
#endif /* NETMAN_METRICS_IMPLEMENTATION */
