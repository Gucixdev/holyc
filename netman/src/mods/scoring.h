/* scoring.h - network quality score 0..100 (signal, gateway RTT, gateway packet loss, captive/no-internet).
 * stb-style single header.
 *
 *   #define NETMAN_SCORING_IMPLEMENTATION   in exactly one .c file, then #include "scoring.h"
 *
 *   netman_score(&in)                     pure function: inputs -> 0..100 (unknown inputs are skipped, not punished)
 *   netman_score_class(score)             NETMAN_Q_GOOD / OK / WEAK / DEAD
 *   netman_score_should_leave(cur, cand)  hysteresis decision used by roaming
 *   netman_gwstat_*                       rolling loss (last 32 probes) + smoothed RTT
 *   netman_signal_dbm(ifn)                /proc/net/wireless (0 = not wifi / unknown)
 *   netman_gw_ping(ifn, gw, timeout)      one ICMP echo through an unprivileged ICMP datagram socket (needs
 *                                         net.ipv4.ping_group_range to include the gid); RTT in us or -errno
 *   netman_score_iface(ifn, probe)        measure + score one interface (roaming's netman_roam_score hook)
 *
 * Weights: signal 30, rtt 30, loss 40, renormalised over the known ones. A captive portal caps the score at 20,
 * no connectivity at 10, 100% loss at 0.
 */
#ifndef NETMAN_SCORING_H
#define NETMAN_SCORING_H

#include <stdint.h>

#define NETMAN_UNKNOWN (-1000) /* "not measured" for every field of netman_score_in */
#define NETMAN_Q_GOOD 70
#define NETMAN_Q_OK 40
#define NETMAN_Q_WEAK 20
#define NETMAN_HYST 15 /* a candidate must beat the current network by this much to justify a switch */

typedef struct {
	int wired;     /* 1: signal is irrelevant (counted as perfect) */
	int signal_dbm; /* e.g. -67; NETMAN_UNKNOWN */
	int rtt_ms;     /* gateway round trip; NETMAN_UNKNOWN, or < 0 for "no answer" */
	int loss_pct;   /* 0..100 to the gateway; NETMAN_UNKNOWN */
	int probe;      /* netman_probe(): 0 internet, 1 captive portal, < 0 none; NETMAN_UNKNOWN */
} netman_score_in;

typedef struct {
	uint32_t ring; /* bit set = lost, newest in bit 0 */
	int n;	       /* probes recorded, saturates at 32 */
	int rtt_ms;    /* smoothed (7/8 old + 1/8 new) over answered probes, NETMAN_UNKNOWN before the first */
} netman_gwstat;

void netman_score_in_init(netman_score_in *in); /* everything NETMAN_UNKNOWN, wired 0 */
int netman_score(const netman_score_in *in);
int netman_score_class(int score);
int netman_score_should_leave(int cur, int cand);

void netman_gwstat_init(netman_gwstat *s);
void netman_gwstat_add(netman_gwstat *s, int rtt_ms); /* rtt_ms < 0 = lost */
int netman_gwstat_loss(const netman_gwstat *s);	      /* percent, NETMAN_UNKNOWN when empty */

int netman_signal_dbm(const char *ifn);
long netman_gw_ping(const char *ifn, const char *gw, int timeout_ms);
int netman_score_iface(const char *ifn, int probe_result);

#endif /* NETMAN_SCORING_H */

#ifdef NETMAN_SCORING_IMPLEMENTATION
#ifndef NETMAN_SCORING_IMPLEMENTATION_DONE
#define NETMAN_SCORING_IMPLEMENTATION_DONE

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

void netman_score_in_init(netman_score_in *in)
{
	in->wired = 0;
	in->signal_dbm = in->rtt_ms = in->loss_pct = in->probe = NETMAN_UNKNOWN;
}

static int netman__lin(int v, int worst, int best) /* v mapped to 0..100, worst -> 0, best -> 100 */
{
	if (best > worst) return v >= best ? 100 : v <= worst ? 0 : (v - worst) * 100 / (best - worst);
	return v <= best ? 100 : v >= worst ? 0 : (worst - v) * 100 / (worst - best);
}

int netman_score(const netman_score_in *in)
{
	int sum = 0, w = 0, s, cap = 100;
	if (in->wired) sum += 30 * 100, w += 30;
	else if (in->signal_dbm != NETMAN_UNKNOWN) sum += 30 * netman__lin(in->signal_dbm, -90, -50), w += 30;
	if (in->rtt_ms != NETMAN_UNKNOWN) {
		sum += 30 * (in->rtt_ms < 0 ? 0 : netman__lin(in->rtt_ms, 500, 20));
		w += 30;
	}
	if (in->loss_pct != NETMAN_UNKNOWN) {
		int l = in->loss_pct < 0 ? 0 : in->loss_pct > 100 ? 100 : in->loss_pct;
		sum += 40 * netman__lin(l, 25, 0); /* 25% loss is already unusable */
		w += 40;
		if (l >= 100) cap = 0;
	}
	if (in->probe != NETMAN_UNKNOWN && in->probe != 0) cap = cap < (in->probe == 1 ? 20 : 10) ? cap : (in->probe == 1 ? 20 : 10);
	if (!w) s = in->probe == 0 ? 60 : in->probe == NETMAN_UNKNOWN ? 50 : 0; /* nothing measured: neutral */
	else s = sum / w;
	if (s > cap) s = cap;
	return s;
}

int netman_score_class(int score)
{
	return score >= NETMAN_Q_GOOD ? NETMAN_Q_GOOD : score >= NETMAN_Q_OK ? NETMAN_Q_OK : score >= NETMAN_Q_WEAK ? NETMAN_Q_WEAK : 0;
}

int netman_score_should_leave(int cur, int cand)
{
	if (cur < NETMAN_Q_WEAK) return cand > cur + 5;	       /* dead network: any real improvement */
	if (cur >= NETMAN_Q_GOOD) return 0;		       /* good enough, never flap */
	return cand >= cur + NETMAN_HYST;
}

void netman_gwstat_init(netman_gwstat *s)
{
	s->ring = 0, s->n = 0, s->rtt_ms = NETMAN_UNKNOWN;
}

void netman_gwstat_add(netman_gwstat *s, int rtt_ms)
{
	s->ring <<= 1;
	if (rtt_ms < 0) s->ring |= 1;
	else s->rtt_ms = s->rtt_ms == NETMAN_UNKNOWN ? rtt_ms : (s->rtt_ms * 7 + rtt_ms + 4) / 8;
	if (s->n < 32) s->n++;
}

int netman_gwstat_loss(const netman_gwstat *s)
{
	uint32_t m = s->n >= 32 ? 0xffffffffu : ((1u << s->n) - 1);
	int lost = __builtin_popcount(s->ring & m);
	return s->n ? lost * 100 / s->n : NETMAN_UNKNOWN;
}

int netman_signal_dbm(const char *ifn)
{
	char line[256], name[32];
	int dbm = 0;
	FILE *f = fopen("/proc/net/wireless", "r");
	if (!f) return 0;
	while (fgets(line, sizeof line, f)) {
		float link, lvl;
		char *c = strchr(line, ':');
		if (!c) continue;
		*c = 0;
		if (sscanf(line, " %31s", name) != 1 || strcmp(name, ifn)) continue;
		if (sscanf(c + 1, " %*x %f %f", &link, &lvl) == 2) dbm = (int)(lvl > 0 ? lvl - 256 : lvl); /* 8-bit wrapped or plain dBm */
		break;
	}
	fclose(f);
	return dbm;
}

long netman_gw_ping(const char *ifn, const char *gw, int timeout_ms)
{
	struct sockaddr_in a = {.sin_family = AF_INET};
	unsigned char pkt[16] = {8, 0, 0, 0, 0, 0, 0, 1}, rx[128];
	struct timespec t0, t1;
	int fd, r;
	if (inet_pton(AF_INET, gw, &a.sin_addr) != 1) return -EINVAL;
	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMP);
	if (fd < 0) return -errno;
	if (ifn && *ifn && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifn, strlen(ifn) + 1) < 0) {
		r = -errno, close(fd);
		return r;
	}
	pkt[6] = (unsigned char)(getpid() >> 8), pkt[7] = (unsigned char)getpid(); /* seq; the kernel owns id + checksum */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (sendto(fd, pkt, sizeof pkt, 0, (struct sockaddr *)&a, sizeof a) < 0) {
		r = -errno, close(fd);
		return r;
	}
	for (;;) {
		struct pollfd p = {fd, POLLIN, 0};
		long el;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		el = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
		if (el >= timeout_ms || poll(&p, 1, timeout_ms - (int)el) <= 0) {
			close(fd);
			return -ETIMEDOUT;
		}
		if (recv(fd, rx, sizeof rx, 0) >= 8 && rx[0] == 0) { /* echo reply */
			clock_gettime(CLOCK_MONOTONIC, &t1);
			close(fd);
			return (t1.tv_sec - t0.tv_sec) * 1000000L + (t1.tv_nsec - t0.tv_nsec) / 1000L;
		}
	}
}

/* Gateway taken from the main routing table through /proc/net/route (no netlink dependency). */
static int netman__gw4(const char *ifn, char *out, size_t n)
{
	char line[256], nm[32];
	unsigned dst, gw, fl;
	FILE *f = fopen("/proc/net/route", "r");
	int ok = 0;
	if (!f) return -errno;
	if (!fgets(line, sizeof line, f)) line[0] = 0;
	while (fgets(line, sizeof line, f))
		if (sscanf(line, "%31s %x %x %x", nm, &dst, &gw, &fl) == 4 && !strcmp(nm, ifn) && dst == 0 && (fl & 2) && gw) {
			struct in_addr a = {.s_addr = gw};
			inet_ntop(AF_INET, &a, out, (socklen_t)n);
			ok = 1;
			break;
		}
	fclose(f);
	return ok ? 0 : -ENETUNREACH;
}

int netman_score_iface(const char *ifn, int probe_result)
{
	netman_score_in in;
	netman_gwstat st;
	char gw[INET_ADDRSTRLEN], path[96];
	int i, dbm;
	netman_score_in_init(&in);
	in.probe = probe_result;
	snprintf(path, sizeof path, "/sys/class/net/%s/wireless", ifn);
	in.wired = access(path, F_OK) != 0;
	if (!in.wired && (dbm = netman_signal_dbm(ifn)) != 0) in.signal_dbm = dbm;
	if (netman__gw4(ifn, gw, sizeof gw) == 0) {
		netman_gwstat_init(&st);
		for (i = 0; i < 4; i++) {
			long us = netman_gw_ping(ifn, gw, 400);
			if (us == -EACCES || us == -EPERM || us == -EAFNOSUPPORT) break; /* cannot ping at all: leave unknown */
			netman_gwstat_add(&st, us < 0 ? -1 : (int)((us + 500) / 1000));
		}
		if (st.n) {
			in.loss_pct = netman_gwstat_loss(&st);
			in.rtt_ms = st.rtt_ms == NETMAN_UNKNOWN ? -1 : st.rtt_ms;
		}
	}
	return netman_score(&in);
}

#endif /* NETMAN_SCORING_IMPLEMENTATION_DONE */
#endif /* NETMAN_SCORING_IMPLEMENTATION */
