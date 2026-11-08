#define _GNU_SOURCE
/* net_score_test.c - unit tests for scoring.h and metrics.h.
 *   ./net_score_test            pure tests + read-only host reads (live part skipped)
 * The live part (traffic on lo, ICMP ping) only runs under `unshare -Urn ./net_score_test`, never on the host. Build: cc -std=gnu11 -O2 -o net_score_test net_score_test.c */
#define NETMAN_NET_IMPLEMENTATION
#define NETMAN_SCORING_IMPLEMENTATION
#define NETMAN_METRICS_IMPLEMENTATION
#include "../src/mods/net.h"
#include "../src/mods/scoring.h"
#include "../src/mods/metrics.h"


static int fails, checks, skips;
#define CHECK(c, ...)                                                           \
	do {                                                                    \
		checks++;                                                       \
		if (!(c)) {                                                     \
			fails++;                                                \
			printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #c);   \
			printf("" __VA_ARGS__);                                 \
			printf("\n");                                           \
		} else printf("  ok   %s\n", #c);                              \
	} while (0)

static netman_score_in mk(int wired, int sig, int rtt, int loss, int probe)
{
	netman_score_in i = {wired, sig, rtt, loss, probe};
	return i;
}

static void test_score(void)
{
	netman_score_in i;
	netman_gwstat g;
	int k;
	puts("[score]");
	i = mk(1, NETMAN_UNKNOWN, 5, 0, 0);
	CHECK(netman_score(&i) >= 95, "wired fast clean = %d", netman_score(&i));
	i = mk(0, -45, 10, 0, 0);
	CHECK(netman_score(&i) == 100);
	i = mk(0, -90, 500, 25, 0);
	CHECK(netman_score(&i) == 0);
	i = mk(0, -70, 80, 2, 0);
	k = netman_score(&i);
	CHECK(k > 40 && k < 90, "mid = %d", k);
	i = mk(0, -50, 20, 0, 1);
	CHECK(netman_score(&i) == 20, "captive portal cap");
	i = mk(1, NETMAN_UNKNOWN, 5, 0, -1);
	CHECK(netman_score(&i) == 10, "no internet cap");
	i = mk(1, NETMAN_UNKNOWN, 5, 100, 0);
	CHECK(netman_score(&i) == 0, "100%% loss = dead");
	i = mk(1, NETMAN_UNKNOWN, -1, 100, NETMAN_UNKNOWN);
	CHECK(netman_score(&i) == 0, "gateway silent");
	netman_score_in_init(&i);
	CHECK(netman_score(&i) == 50, "nothing known = neutral");
	i.probe = 0;
	CHECK(netman_score(&i) == 60, "only internet ok");
	i = mk(0, -60, NETMAN_UNKNOWN, NETMAN_UNKNOWN, NETMAN_UNKNOWN);
	CHECK(netman_score(&i) == 75, "signal only, unknown fields not punished");
	{ /* monotonic in signal */
		int prev = -1, ok = 1, d;
		for (d = -95; d <= -40; d++) {
			netman_score_in x = mk(0, d, 30, 0, 0);
			int s = netman_score(&x);
			if (s < prev) ok = 0;
			prev = s;
		}
		CHECK(ok, "score never drops as signal improves");
	}
	{ /* clamps */
		netman_score_in x = mk(0, -10, 0, -5, 0), y = mk(0, -200, 100000, 500, 0);
		CHECK(netman_score(&x) == 100 && netman_score(&y) == 0);
	}
	CHECK(netman_score_class(100) == NETMAN_Q_GOOD && netman_score_class(70) == NETMAN_Q_GOOD);
	CHECK(netman_score_class(69) == NETMAN_Q_OK && netman_score_class(40) == NETMAN_Q_OK);
	CHECK(netman_score_class(39) == NETMAN_Q_WEAK && netman_score_class(20) == NETMAN_Q_WEAK);
	CHECK(netman_score_class(19) == 0 && netman_score_class(0) == 0);
	CHECK(netman_score_should_leave(10, 30) == 1, "dead -> any real improvement");
	CHECK(netman_score_should_leave(10, 12) == 0, "dead -> marginal is not worth it");
	CHECK(netman_score_should_leave(80, 100) == 0, "good never flaps");
	CHECK(netman_score_should_leave(50, 60) == 0 && netman_score_should_leave(50, 65) == 1, "hysteresis");

	puts("[gwstat]");
	netman_gwstat_init(&g);
	CHECK(netman_gwstat_loss(&g) == NETMAN_UNKNOWN && g.rtt_ms == NETMAN_UNKNOWN);
	netman_gwstat_add(&g, 10);
	CHECK(netman_gwstat_loss(&g) == 0 && g.rtt_ms == 10);
	netman_gwstat_add(&g, -1);
	CHECK(netman_gwstat_loss(&g) == 50 && g.rtt_ms == 10, "loss does not move rtt");
	netman_gwstat_add(&g, 90);
	CHECK(g.rtt_ms == 20, "ewma %d", g.rtt_ms);
	for (k = 0; k < 40; k++) netman_gwstat_add(&g, 5);
	CHECK(g.n == 32 && netman_gwstat_loss(&g) == 0, "window forgets old losses");
	for (k = 0; k < 8; k++) netman_gwstat_add(&g, -1);
	CHECK(netman_gwstat_loss(&g) == 25, "8 of 32 lost");

	puts("[wifi/gw readers]");
	CHECK(netman_signal_dbm("no-such-if0") == 0);
	CHECK(netman_gw_ping("lo", "not-an-ip", 10) == -EINVAL);
}

static void test_metrics_pure(void)
{
	char b[32];
	netman_ifstat st;
	netman_metrics m;
	int n, i, haslo = 0;
	memset(&m, 0, sizeof m);
	puts("[metrics pure]");
	CHECK(netman_metrics_rate(1000, 3000, 1000) == 2000.0);
	CHECK(netman_metrics_rate(1000, 3000, 500) == 4000.0);
	CHECK(netman_metrics_rate(5000, 100, 1000) == 0.0, "counter reset -> 0");
	CHECK(netman_metrics_rate(0, 100, 0) == 0.0 && netman_metrics_rate(0, 100, -5) == 0.0);
	netman_metrics_fmt_rate(b, sizeof b, 0); CHECK(!strcmp(b, "0 B/s"), "%s", b);
	netman_metrics_fmt_rate(b, sizeof b, 1500); CHECK(!strcmp(b, "1.5 KB/s"), "%s", b);
	netman_metrics_fmt_rate(b, sizeof b, 2.5e6); CHECK(!strcmp(b, "2.5 MB/s"), "%s", b);
	netman_metrics_fmt_bytes(b, sizeof b, 512); CHECK(!strcmp(b, "512 B"), "%s", b);
	netman_metrics_fmt_bytes(b, sizeof b, 1536); CHECK(!strcmp(b, "1.5 KiB"), "%s", b);
	netman_metrics_fmt_bytes(b, sizeof b, 3ull << 30); CHECK(!strcmp(b, "3.0 GiB"), "%s", b);
	CHECK(netman_metrics_sysfs("../etc", &st) == -EINVAL && netman_metrics_sysfs("a/b", &st) == -EINVAL);
	CHECK(netman_metrics_sysfs("no-such-if0", &st) == -ENODEV);
	puts("[metrics host, read-only]");
	n = netman_metrics_netlink(m.s, NETMAN_METRICS_MAXIF);
	CHECK(n > 0, "netlink dump = %d", n);
	for (i = 0; i < n; i++) haslo |= !strcmp(m.s[i].name, "lo");
	CHECK(haslo, "lo listed");
	CHECK(netman_metrics_sysfs("lo", &st) == 0 && st.idx > 0 && !strcmp(st.name, "lo"));
	CHECK(netman_metrics_poll(&m, 1000) > 0 && netman_metrics_find(&m, "lo") && !netman_metrics_find(&m, "zzz"));
	CHECK(netman_metrics_find(&m, "lo")->rx_bps == 0, "first poll has no rates");
}

static void test_live(void)
{
	netman_metrics m;
	const netman_ifstat *lo;
	netman_ifstat st;
	int s, r, i;
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(9), .sin_addr.s_addr = htonl(0x7f000001)};
	char buf[512] = {0};
	long us;
	memset(&m, 0, sizeof m); /* netman_metrics must start zeroed */
	puts("[live in own netns: lo traffic]");
	CHECK(netman_link_set("lo", 1) == 0);
	CHECK(netman_addr_add("lo", "127.0.0.1/8") == 0);
	CHECK(netman_metrics_poll(&m, 0) > 0);
	lo = netman_metrics_find(&m, "lo");
	CHECK(lo && lo->rx_bytes == lo->tx_bytes, "lo symmetric before traffic");
	s = socket(AF_INET, SOCK_DGRAM, 0);
	for (i = 0; i < 100; i++) r = (int)sendto(s, buf, sizeof buf, 0, (struct sockaddr *)&a, sizeof a);
	close(s);
	CHECK(r == (int)sizeof buf);
	CHECK(netman_metrics_poll(&m, 1000) > 0);
	lo = netman_metrics_find(&m, "lo");
	CHECK(lo && lo->tx_pkts >= 100 && lo->tx_bytes >= 100 * 512, "tx counted: %llu", lo ? (unsigned long long)lo->tx_bytes : 0);
	CHECK(lo && lo->tx_bps >= 100 * 512 - 1 && lo->rx_bps > 0, "rate %.0f B/s", lo ? lo->tx_bps : 0);
	CHECK(lo && lo->tx_pps >= 99, "pps %.0f", lo ? lo->tx_pps : 0);
	CHECK(netman_metrics_sysfs("lo", &st) == 0 && lo && st.tx_bytes >= lo->tx_bytes, "sysfs agrees with netlink");
	CHECK(lo && (!strcmp(lo->oper, "unknown") || !strcmp(lo->oper, "up")) && lo->carrier == 1);
	us = netman_gw_ping("lo", "127.0.0.1", 500);
	if (us == -EACCES || us == -EPERM || us == -EAFNOSUPPORT) {
		skips++;
		printf("  skip ICMP ping (%s: ping_group_range)\n", strerror((int)-us));
	} else CHECK(us > 0 && us < 500000, "rtt %ld us", us);
	us = netman_gw_ping("lo", "127.0.0.2", 100);
	if (us != -EACCES && us != -EPERM && us != -EAFNOSUPPORT) CHECK(us > 0, "lo answers all of 127/8 (%ld)", us);
}

/* Live part only inside a user namespace (run the binary under `unshare -Urn`): never on the host. */
static int in_userns(void)
{
	char b[128] = "";
	unsigned long a, c, n;
	FILE *f = fopen("/proc/self/uid_map", "r");
	if (!f) return 0;
	if (!fgets(b, sizeof b, f)) b[0] = 0;
	fclose(f);
	if (sscanf(b, "%lu %lu %lu", &a, &c, &n) != 3) return 0;
	return !(c == 0 && n > 100000); /* identity map of the whole range = the initial namespace */
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	test_score();
	test_metrics_pure();
	if (in_userns() && getuid() == 0) test_live();
	else {
		skips++;
		puts("  skip live part: run under `unshare -Urn` to enable it");
	}
	printf("\n%d checks, %d failed, %d skipped\n", checks, fails, skips);
	return fails != 0;
}
