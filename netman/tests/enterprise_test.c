#define _GNU_SOURCE
/* enterprise_test.c - self-test for src/mods/enterprise.c, tether.c and licence.c.
 *
 *   ./enterprise_test            runs everything inside its OWN user+net namespace (the host network is never touched;
 *                                it refuses to run if the namespace cannot be made)
 *
 * Build (from netman/): cc -O2 -o tests/enterprise_test tests/enterprise_test.c src/mods/enterprise.c src/mods/tether.c src/mods/licence.c
 * Things the kernel/driver/namespace does not allow are reported as SKIP, not as failures.
 * `ip` and `nsenter` are used by the TEST only, as an independent cross-check and to set up the peer netns.
 */
#define NETMAN_NET_IMPLEMENTATION
#include "../src/mods/net.h"
#include "../src/mods/enterprise.h"
#include "../src/mods/licence.h"

#include <sched.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>

static int fails, checks, skips;
#define CHECK(c, ...)                                                                   \
	do {                                                                            \
		checks++;                                                               \
		if (!(c)) {                                                             \
			fails++;                                                        \
			printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #c);           \
			printf("" __VA_ARGS__);                                         \
			printf("\n");                                                   \
		} else printf("  ok   %s\n", #c);                                      \
	} while (0)
#define SKIP(...)                       \
	do {                            \
		skips++;                \
		printf("  SKIP ");      \
		printf(__VA_ARGS__);    \
		printf("\n");           \
	} while (0)

static int enter_ns(void)
{
	uid_t u = getuid();
	gid_t g = getgid();
	char m[64];
	int fd;
	if (unshare(CLONE_NEWUSER | CLONE_NEWNET) < 0) {
		perror("unshare");
		return -1;
	}
	if ((fd = open("/proc/self/setgroups", O_WRONLY)) >= 0) write(fd, "deny", 4), close(fd);
	snprintf(m, sizeof m, "0 %u 1", u);
	if ((fd = open("/proc/self/uid_map", O_WRONLY)) < 0 || write(fd, m, strlen(m)) < 0) return -1;
	close(fd);
	snprintf(m, sizeof m, "0 %u 1", g);
	if ((fd = open("/proc/self/gid_map", O_WRONLY)) < 0 || write(fd, m, strlen(m)) < 0) return -1;
	close(fd);
	return 0;
}

static pid_t holder;
static int start_holder(void)
{
	char a[64], x[64], y[64];
	ssize_t n;
	holder = fork();
	if (holder == 0) {
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (unshare(CLONE_NEWNET) < 0) _exit(1);
		for (;;) pause();
	}
	snprintf(a, sizeof a, "/proc/%d/ns/net", (int)holder);
	for (int i = 0; i < 100; i++) {
		usleep(20000);
		n = readlink(a, x, sizeof x - 1);
		if (n > 0) x[n] = 0;
		n = readlink("/proc/self/ns/net", y, sizeof y - 1);
		if (n > 0) y[n] = 0;
		if (strcmp(x, y)) return 0;
	}
	return -1;
}
static int sh(const char *fmt, ...)
{
	char c[800];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c, sizeof c, fmt, ap);
	va_end(ap);
	return system(c);
}
static int hsh(const char *fmt, ...) /* run in the holder netns */
{
	char c[700], d[900];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c, sizeof c, fmt, ap);
	va_end(ap);
	snprintf(d, sizeof d, "nsenter --net=/proc/%d/ns/net sh -c '%s'", (int)holder, c);
	return system(d);
}
static int has(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }
static int cap_cmd(const char *cmd, char *out, size_t n)
{
	FILE *p = popen(cmd, "r");
	size_t k;
	if (!p) return -1;
	k = fread(out, 1, n - 1, p);
	out[k] = 0;
	return pclose(p);
}
static netman_ent mk(const char *text)
{
	netman_ent e;
	int r = netman_ent_parse(text, &e);
	if (r < 0) {
		printf("  (spec parse failed: %s)\n", netman_ent_error());
		netman_ent_init(&e);
	}
	return e;
}

/* ------------------------------------------------------------------ spec */
static void t_spec(void)
{
	netman_ent e, f;
	char b[4096], b2[4096];
	printf("spec parse/format\n");
	e = mk("kind=bond\nname=bond0\nmode=802.3ad\nmiimon=100\nlacp_rate=fast\nxmit_hash_policy=layer3+4\nmember=eth0\nmember=eth1\naddress=10.0.0.2/24\n");
	CHECK(!strcmp(e.kind, "bond") && e.nmember == 2 && e.miimon == 100 && e.stp == -1, "kind=%s", e.kind);
	CHECK(netman_ent_format(&e, b, sizeof b) > 0);
	CHECK(netman_ent_parse(b, &f) == 0 && netman_ent_format(&f, b2, sizeof b2) > 0 && !strcmp(b, b2), "round trip differs:\n%s---\n%s", b, b2);
	e = mk("kind=bridge\niface=br0\nstp=1\nforward_delay=4\nvlan_filtering=1\nvlan=10,pvid,untagged\nvlan=20\nmember=a\n");
	CHECK(!strcmp(e.name, "br0") && e.nvlan == 2 && e.vlan[0].pvid && e.vlan[0].untagged && e.vlan[1].vid == 20);
	e = mk("kind=ecmp\nname=wan\nuplink=eth0,192.0.2.1,1.1.1.1,3\nuplink=eth1,198.51.100.1\n");
	CHECK(e.nup == 2 && e.up[0].weight == 3 && !strcmp(e.up[0].probe, "1.1.1.1") && e.up[1].weight == 1);
	CHECK(netman_ent_parse("kind=bond\nname=b\nmode=nonsense\n", &f) == -EINVAL, "%s", netman_ent_error());
	CHECK(netman_ent_parse("kind=bond\nname=b\nwhat=1\n", &f) == -EINVAL);
	CHECK(netman_ent_parse("kind=vlan\nname=v\nvid=5000\n", &f) == -EINVAL);
	CHECK(netman_ent_parse("kind=dot1x\nname=e\nidentity=a\"b\n", &f) == -EINVAL);
	CHECK(netman_ent_parse("kind=wat\nname=x\n", &f) == -EINVAL);
	CHECK(netman_ent_parse("kind=dummy\nname=../etc\n", &f) == -EINVAL);
}

/* ------------------------------------------------------------------ links */
static void t_dummy_bond(void)
{
	netman_ent_info i;
	char m[8][16];
	netman_ent e;
	char out[4096];
	FILE *f;
	printf("dummy, bonding, team\n");
	e = mk("kind=dummy\nname=dm0\nmtu=1400\naddress=10.9.0.1/24\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	CHECK(netman_ent_getinfo("dm0", &i) == 0 && !strcmp(i.kind, "dummy") && i.mtu == 1400 && i.up);
	CHECK(netman_ent_del(&e) == 0 && netman_ent_getinfo("dm0", &i) == -ENODEV);

	sh("ip link add s1 type dummy; ip link add s2 type dummy; ip link add s3 type dummy");
	e = mk("kind=bond\nname=b0\nmode=802.3ad\nmiimon=200\nlacp_rate=fast\nxmit_hash_policy=layer3+4\nmember=s1\nmember=s2\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	CHECK(netman_ent_getinfo("b0", &i) == 0 && !strcmp(i.kind, "bond") && i.mode == 4 && i.miimon == 200 && i.lacp == 1 && i.hash == 1,
	      "mode %d miimon %d lacp %d hash %d", i.mode, i.miimon, i.lacp, i.hash);
	CHECK(netman_ent_members("b0", m, 8) == 2 && !strcmp(m[0], "s1") && !strcmp(m[1], "s2"));
	{
		char o[2048];
		cap_cmd("ip -d link show b0 2>&1", o, sizeof o);
		CHECK(has(o, "802.3ad") && has(o, "lacp_rate fast") && has(o, "layer3+4"), "ip says: %s", o);
	}
	f = fmemopen(out, sizeof out, "w");
	netman_ent_show(&e, f);
	fclose(f);
	CHECK(has(out, "802.3ad") && has(out, "s1") && has(out, "s2"), "%s", out);
	CHECK(netman_ent_del(&e) == 0 && netman_ent_getinfo("b0", &i) == -ENODEV);

	e = mk("kind=bond\nname=b1\nmode=active-backup\nprimary=s3\nmember=s3\nmember=s1\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	CHECK(netman_ent_getinfo("b1", &i) == 0 && i.mode == 1 && i.primary == (int)if_nametoindex("s3") && i.miimon == 100);
	netman_ent_del(&e);
	for (const char *mm = "balance-xor"; mm; mm = !strcmp(mm, "balance-xor") ? "broadcast" : NULL) {
		char t[200];
		snprintf(t, sizeof t, "kind=bond\nname=b2\nmode=%s\nmember=s1\n", mm);
		e = mk(t);
		CHECK(netman_ent_add(&e) == 0, "%s: %s", mm, netman_ent_error());
		CHECK(netman_ent_getinfo("b2", &i) == 0 && i.mode == (!strcmp(mm, "broadcast") ? 3 : 2), "%s got mode %d", mm, i.mode);
		netman_ent_del(&e);
	}
	/* team == bonding with runner names mapped */
	e = mk("kind=team\nname=t0\nrunner=lacp\nmember=s1\nmember=s2\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	CHECK(netman_ent_getinfo("t0", &i) == 0 && !strcmp(i.kind, "bond") && i.mode == 4);
	netman_ent_del(&e);
	e = mk("kind=team\nname=t1\nrunner=activebackup\nmember=s1\n");
	CHECK(netman_ent_add(&e) == 0 && netman_ent_getinfo("t1", &i) == 0 && i.mode == 1);
	netman_ent_del(&e);
	sh("ip link del s1; ip link del s2; ip link del s3");
}

static void t_bridge(void)
{
	netman_ent_info i;
	netman_ent_vlan v[8];
	netman_ent e;
	char o[1024], out[2048];
	FILE *f;
	int n;
	printf("bridge, STP, vlan filtering\n");
	sh("ip link add p1 type dummy; ip link add p2 type dummy");
	e = mk("kind=bridge\nname=br0\nstp=1\nforward_delay=4\nvlan_filtering=1\nmember=p1\nmember=p2\nvlan=10,pvid,untagged\nvlan=20\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	CHECK(netman_ent_getinfo("br0", &i) == 0 && !strcmp(i.kind, "bridge") && i.stp == 1 && i.fwd_delay == 400 && i.vlan_filtering == 1,
	      "stp %d fd %d vf %d", i.stp, i.fwd_delay, i.vlan_filtering);
	n = netman_ent_bridge_vlans("p1", v, 8);
	{
		int has10 = 0, has20 = 0, pv = 0, un = 0;
		for (int k = 0; k < n; k++) {
			if (v[k].vid == 10) has10 = 1, pv = v[k].pvid, un = v[k].untagged;
			if (v[k].vid == 20) has20 = 1;
		}
		CHECK(has10 && has20 && pv && un, "n=%d", n);
	}
	cap_cmd("bridge vlan show dev p2 2>&1", o, sizeof o);
	CHECK(has(o, "10") && has(o, "PVID") && has(o, "20"), "bridge says: %s", o);
	f = fmemopen(out, sizeof out, "w");
	netman_ent_show(&e, f);
	fclose(f);
	CHECK(has(out, "stp on") && has(out, "port p1") && has(out, "vlan 10 pvid untagged"), "%s", out);
	netman_ent_del(&e);
	e = mk("kind=bridge\nname=br1\nstp=0\nmember=p1\n");
	CHECK(netman_ent_add(&e) == 0 && netman_ent_getinfo("br1", &i) == 0 && i.stp == 0 && i.vlan_filtering == 0);
	netman_ent_del(&e);
	sh("ip link del p1; ip link del p2");
}

static void t_vlans(void)
{
	netman_ent_info i;
	netman_ent e;
	char o[512];
	printf("vlan, QinQ, macvlan, ipvlan, veth\n");
	sh("ip link add pd type dummy");
	e = mk("kind=vlan\nname=pd.10\nparent=pd\nvid=10\n");
	{ int r_ = netman_ent_add(&e); CHECK(r_ == 0, "r=%d %s", r_, netman_ent_error()); }
	CHECK(netman_ent_getinfo("pd.10", &i) == 0 && !strcmp(i.kind, "vlan") && i.vid == 10 && i.proto == 0x8100 && i.link == (int)if_nametoindex("pd"));
	netman_ent_del(&e);
	e = mk("kind=vlan\nname=svc\nparent=pd\nvid=100\nproto=802.1ad\ninner_vid=20\ninner=svc.20\n");
	{ int r_ = netman_ent_add(&e); CHECK(r_ == 0, "r=%d %s", r_, netman_ent_error()); }
	CHECK(netman_ent_getinfo("svc", &i) == 0 && i.vid == 100 && i.proto == 0x88a8, "vid %d proto 0x%x", i.vid, i.proto);
	CHECK(netman_ent_getinfo("svc.20", &i) == 0 && i.vid == 20 && i.proto == 0x8100 && i.link == (int)if_nametoindex("svc"));
	cap_cmd("ip -d link show svc 2>&1", o, sizeof o);
	CHECK(has(o, "802.1ad"), "%s", o);
	netman_ent_del(&e);
	CHECK(netman_ent_getinfo("svc.20", &i) == -ENODEV, "inner should vanish with the outer");

	e = mk("kind=macvlan\nname=mv0\nparent=pd\nmode=vepa\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	CHECK(netman_ent_getinfo("mv0", &i) == 0 && !strcmp(i.kind, "macvlan") && i.mode == 2, "mode %d", i.mode);
	netman_ent_del(&e);
	e = mk("kind=ipvlan\nname=iv0\nparent=pd\nmode=l3\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	CHECK(netman_ent_getinfo("iv0", &i) == 0 && !strcmp(i.kind, "ipvlan") && i.mode == 1, "mode %d", i.mode);
	netman_ent_del(&e);
	e = mk("kind=macvlan\nname=mv1\nparent=nope\n");
	CHECK(netman_ent_add(&e) == -ENODEV, "missing parent should be ENODEV");

	e = mk("kind=veth\nname=ve0\npeer=ve1\naddress=10.30.0.1/24\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	CHECK(netman_ent_getinfo("ve0", &i) == 0 && !strcmp(i.kind, "veth") && i.link == (int)if_nametoindex("ve1"));
	CHECK(netman_ent_getinfo("ve1", &i) == 0);
	CHECK(netman_ent_add(&e) < 0, "second add of same veth must fail");
	CHECK(netman_ent_del(&e) == 0 && netman_ent_getinfo("ve1", &i) == -ENODEV, "peer should vanish too");
	sh("ip link del pd");
}

/* ------------------------------------------------------------------ routing */
static void t_routes(void)
{
	netman_ent e;
	char b[4096], o[2048];
	int n;
	char tabs[] = "/tmp/ent_rt_tablesXXXXXX";
	int fd = mkstemp(tabs);
	printf("routes, rules, tables\n");
	write(fd, "# t\n255 local\n100 isp1\n200 isp2\n", 31);
	close(fd);
	netman_ent_rt_tables = tabs;
	CHECK(netman_ent_table_id("isp2") == 200 && netman_ent_table_id("main") == 254 && netman_ent_table_id("77") == 77 && netman_ent_table_id("zzz") == -1);
	sh("ip link add r0 type dummy; ip addr add 10.50.0.1/24 dev r0; ip addr add fd00::1/64 dev r0 nodad; ip link set r0 up");
	e = mk("kind=route\nname=r0\ndst=10.60.0.0/24\ngateway=10.50.0.2\ntable=100\nmetric=20\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	n = netman_ent_route_list(100, 0, b, sizeof b);
	CHECK(n > 0 && has(b, "10.60.0.0/24 via 10.50.0.2 dev r0 metric 20 table 100"), "%s", b);
	cap_cmd("ip route show table 100", o, sizeof o);
	CHECK(has(o, "10.60.0.0/24 via 10.50.0.2 dev r0") && has(o, "metric 20"), "ip says: %s", o);
	CHECK(netman_ent_del(&e) == 0);
	CHECK(netman_ent_route_list(100, 0, b, sizeof b) == 0, "%s", b);
	e = mk("kind=route\nname=r0\ndst=10.61.0.0/24\ngateway=10.50.0.2\ntable=1000\n");
	CHECK(netman_ent_add(&e) == 0, "table >255: %s", netman_ent_error());
	CHECK(netman_ent_route_list(1000, 0, b, sizeof b) > 0 && has(b, "10.61.0.0/24"), "%s", b);
	netman_ent_del(&e);
	e = mk("kind=route\nname=r0\ndst=192.0.2.0/24\nrtype=blackhole\ntable=100\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	CHECK(netman_ent_route_list(100, 0, b, sizeof b) > 0 && has(b, "type blackhole"), "%s", b);
	netman_ent_del(&e);
	e = mk("kind=route\nname=r0\ndst=default\ngateway=10.50.0.9\ntable=100\n");
	CHECK(netman_ent_add(&e) == 0 && netman_ent_route_list(100, AF_INET, b, sizeof b) > 0 && has(b, "default via 10.50.0.9"), "%s", b);
	netman_ent_del(&e);
	e = mk("kind=route\nname=r0\ndst=2001:db8:5::/48\ngateway=fd00::2\ntable=100\n");
	if (netman_ent_add(&e) == 0) {
		CHECK(netman_ent_route_list(100, AF_INET6, b, sizeof b) > 0 && has(b, "2001:db8:5::/48"), "%s", b);
		netman_ent_del(&e);
	} else SKIP("ipv6 route: %s", netman_ent_error());

	e = mk("kind=rule\nname=isp1\nfrom=10.50.0.0/24\ntable=100\nprio=1000\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	n = netman_ent_rule_list(AF_INET, b, sizeof b);
	CHECK(n > 0 && has(b, "1000: from 10.50.0.0/24 lookup 100"), "%s", b);
	cap_cmd("ip rule show", o, sizeof o);
	CHECK(has(o, "1000:") && has(o, "from 10.50.0.0/24") && has(o, "lookup 100"), "ip says: %s", o);
	CHECK(netman_ent_add(&e) == 0, "re-adding an identical rule is idempotent");
	CHECK(netman_ent_del(&e) == 0 && !has((netman_ent_rule_list(AF_INET, b, sizeof b), b), "lookup 100"));
	e = mk("kind=rule\nname=mk\nfwmark=0x10\ntable=2000\nprio=1100\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	netman_ent_rule_list(AF_INET, b, sizeof b);
	CHECK(has(b, "fwmark 0x10") && has(b, "lookup 2000"), "%s", b);
	netman_ent_del(&e);
	e = mk("kind=rule\nname=bad\nfrom=10.0.0.0/8\n");
	CHECK(netman_ent_add(&e) == -EINVAL, "rule without table");
	sh("ip link del r0");
	unlink(tabs);
	netman_ent_rt_tables = "/etc/iproute2/rt_tables";
}

/* ECMP ----------------------------------------------------------------- */
static int g_alive[4];
static int fake_probe(const char *ifn, const char *t, int ms)
{
	(void)t, (void)ms;
	return g_alive[ifn[1] - '0'] ? 0 : -ETIMEDOUT;
}
static void t_ecmp(void)
{
	netman_ent e;
	char b[2048], o[1024];
	int (*saved)(const char *, const char *, int) = netman_ent_probe_fn;
	printf("ECMP multi-uplink\n");
	if (start_holder() < 0) { SKIP("no holder netns"); return; }
	for (int k = 0; k < 2; k++) {
		char c[300];
		snprintf(c, sizeof c, "ip link add u%d type veth peer name h%d; ip link set h%d netns %d; ip addr add 10.%d.0.1/24 dev u%d; ip link set u%d up", k, k, k, (int)holder, 70 + k, k, k);
		sh("%s", c);
		hsh("ip addr add 10.%d.0.2/24 dev h%d; ip link set h%d up; ip link set lo up", 70 + k, k, k);
	}
	/* scripted probe: hysteresis and route switching */
	netman_ent_probe_fn = fake_probe;
	g_alive[0] = g_alive[1] = 1;
	e = mk("kind=ecmp\nname=wan\nfail_after=2\nok_after=2\nuplink=u0,10.70.0.2,10.70.0.2,1\nuplink=u1,10.71.0.2,10.71.0.2,3\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	netman_ent_route_list(254, AF_INET, b, sizeof b);
	CHECK(has(b, "default") && has(b, "nexthop via 10.70.0.2 dev u0 weight 1") && has(b, "nexthop via 10.71.0.2 dev u1 weight 3") && has(b, "metric 50"), "%s", b);
	cap_cmd("ip route show default", o, sizeof o);
	CHECK(has(o, "nexthop via 10.70.0.2 dev u0 weight 1") && has(o, "nexthop via 10.71.0.2 dev u1 weight 3"), "ip says: %s", o);
	CHECK(netman_ent_ecmp_tick(&e, 0) == 0, "healthy tick changes nothing");
	g_alive[0] = 0;
	CHECK(netman_ent_ecmp_tick(&e, 0) == 0 && e.up[0].up == 1, "one failure is below fail_after");
	CHECK(netman_ent_ecmp_tick(&e, 0) == 1 && e.up[0].up == 0, "second failure flips u0 down: %s", netman_ent_error());
	netman_ent_route_list(254, AF_INET, b, sizeof b);
	CHECK(has(b, "default via 10.71.0.2 dev u1") && !has(b, "nexthop") && !has(b, "default via 10.70"), "failover to u1 only:\n%s", b);
	g_alive[0] = 1;
	CHECK(netman_ent_ecmp_tick(&e, 0) == 0 && e.up[0].up == 0, "one success is below ok_after");
	CHECK(netman_ent_ecmp_tick(&e, 0) == 1 && e.up[0].up == 1);
	netman_ent_route_list(254, AF_INET, b, sizeof b);
	CHECK(has(b, "nexthop via 10.70.0.2 dev u0") && has(b, "nexthop via 10.71.0.2 dev u1"), "recovered:\n%s", b);
	g_alive[0] = g_alive[1] = 0;
	netman_ent_ecmp_tick(&e, 0);
	netman_ent_ecmp_tick(&e, 0);
	netman_ent_route_list(254, AF_INET, b, sizeof b);
	CHECK(!has(b, "default"), "all uplinks dead: no default route\n%s", b);
	g_alive[0] = g_alive[1] = 1;
	netman_ent_ecmp_tick(&e, 0);
	netman_ent_ecmp_tick(&e, 0);
	netman_ent_route_list(254, AF_INET, b, sizeof b);
	CHECK(has(b, "nexthop"), "default restored\n%s", b);
	CHECK(netman_ent_del(&e) == 0);
	netman_ent_route_list(254, AF_INET, b, sizeof b);
	CHECK(!has(b, "default"), "%s", b);

	/* real ICMP probe against the peer netns */
	netman_ent_probe_fn = saved;
	CHECK(saved("u0", "10.70.0.2", 1000) == 0, "real ICMP echo to the holder netns");
	CHECK(saved("u0", "10.70.0.99", 300) != 0, "no reply from a non-existent host");
	hsh("ip addr del 10.70.0.2/24 dev h0");
	CHECK(saved("u0", "10.70.0.2", 300) != 0, "no reply once the peer address is gone");
	hsh("ip addr add 10.70.0.2/24 dev h0");
	e = mk("kind=ecmp\nname=wan\nprobe_ms=300\nfail_after=1\nok_after=1\nuplink=u0,10.70.0.2\nuplink=u1,10.71.0.2\n");
	netman_ent_add(&e);
	hsh("ip addr del 10.71.0.2/24 dev h1");
	CHECK(netman_ent_ecmp_tick(&e, 0) == 1 && e.up[1].up == 0 && e.up[0].up == 1, "real probe detects u1 down");
	netman_ent_route_list(254, AF_INET, b, sizeof b);
	CHECK(has(b, "default via 10.70.0.2 dev u0") && !has(b, "nexthop") && !has(b, "default via 10.71"), "%s", b);
	netman_ent_del(&e);
	sh("ip link del u0; ip link del u1");
	kill(holder, SIGKILL);
	waitpid(holder, NULL, 0);
	holder = 0;
}

/* ------------------------------------------------------------------ link, ipv6, wol */
static void t_link_ipv6(void)
{
	netman_ent e;
	netman_ent_info i;
	char out[1024], cidr[80], b[1024], rd[16];
	FILE *f;
	int fd;
	netman_ent x;
	printf("link settings, IPv6, PD\n");
	sh("ip link add l0 type dummy; ip link set l0 up");
	e = mk("kind=link\nname=l0\nmtu=1380\ntso=0\ngso=0\ngro=0\nsg=1\n");
	{
		int r = netman_ent_add(&e);
		CHECK(netman_ent_getinfo("l0", &i) == 0 && i.mtu == 1380, "mtu %d", i.mtu);
		if (r != 0) SKIP("ethtool offloads on dummy: %s (mtu applied)", netman_ent_error());
		else {
			char o[2048];
			cap_cmd("ethtool -k l0 2>&1 | grep -E 'tcp-segmentation-offload|generic-receive-offload'", o, sizeof o);
			CHECK(1, "offload ioctls accepted");
			f = fmemopen(out, sizeof out, "w");
			netman_ent_show(&e, f);
			fclose(f);
			CHECK(has(out, "mtu 1380") && (has(out, "gro off") || has(out, "tso off")), "%s", out);
		}
	}
	CHECK(netman_ent_del(&e) == 0 && netman_ent_getinfo("l0", &i) == 0 && i.mtu == 1500);
	e = mk("kind=link\nname=l0\nspeed=1000\nautoneg=0\nduplex=full\n");
	{
		int r = netman_ent_add(&e);
		if (r < 0) SKIP("speed/duplex on dummy: %s", netman_ent_error());
		else CHECK(1, "speed/duplex accepted");
	}
	e = mk("kind=link\nname=nope0\nmtu=1400\n");
	CHECK(netman_ent_add(&e) < 0);

	e = mk("kind=ipv6\nname=l0\naccept_ra=2\nautoconf=0\ntempaddr=2\nforwarding=1\naddress=2001:db8:1::1/64\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	{
		const char *keys[] = {"accept_ra", "autoconf", "use_tempaddr", "forwarding"};
		const char *want[] = {"2", "0", "2", "1"};
		for (int k = 0; k < 4; k++) {
			char p[200];
			snprintf(p, sizeof p, "/proc/sys/net/ipv6/conf/l0/%s", keys[k]);
			fd = open(p, O_RDONLY);
			memset(rd, 0, sizeof rd);
			if (fd >= 0) read(fd, rd, sizeof rd - 1), close(fd);
			CHECK(rd[0] == want[k][0], "%s = %s want %s", keys[k], rd, want[k]);
		}
	}
	f = fmemopen(out, sizeof out, "w");
	netman_ent_show(&e, f);
	fclose(f);
	CHECK(has(out, "accept_ra 2") && has(out, "2001:db8:1::1/64"), "%s", out);
	netman_ent_del(&e);

	CHECK(netman_ent_pd_subnet("2001:db8:abcd::/48", 1, cidr, sizeof cidr) == 0 && !strcmp(cidr, "2001:db8:abcd:1::1/64"), "%s", cidr);
	CHECK(netman_ent_pd_subnet("2001:db8:0:ab00::/56", 3, cidr, sizeof cidr) == 0 && !strcmp(cidr, "2001:db8:0:ab03::1/64"), "%s", cidr);
	CHECK(netman_ent_pd_subnet("2001:db8:0:ab00::/56", 256, cidr, sizeof cidr) == -ERANGE);
	CHECK(netman_ent_pd_subnet("2001:db8::/72", 0, cidr, sizeof cidr) < 0);
	CHECK(netman_ent_pd_subnet("10.0.0.0/8", 0, cidr, sizeof cidr) < 0);
	netman_ent_init(&x);
	CHECK(netman_ent_pd_from_env("2001:db8:77::/48,3600,7200 fd00::/64,1,2", &x) == 0 && !strcmp(x.pd_prefix, "2001:db8:77::/48"));
	sh("ip link add dn0 type dummy; ip link add dn1 type dummy");
	e = mk("kind=ipv6\nname=l0\npd_prefix=2001:db8:77::/48\nmember=dn0\nmember=dn1\n");
	CHECK(netman_ent_add(&e) == 0, "%s", netman_ent_error());
	cap_cmd("ip -6 addr show dev dn1", b, sizeof b);
	CHECK(has(b, "2001:db8:77:2::1/64"), "%s", b);
	cap_cmd("ip -6 addr show dev dn0", b, sizeof b);
	CHECK(has(b, "2001:db8:77:1::1/64"), "%s", b);
	CHECK(netman_ent_pd_helper_conf(&e, b, sizeof b) > 0 && has(b, "interface=dn0") && has(b, "ra-stateless") && has(b, "interface=dn1"), "%s", b);
	netman_ent_del(&e);
	cap_cmd("ip -6 addr show dev dn1", b, sizeof b);
	CHECK(!has(b, "2001:db8:77:2::1"), "PD address removed");
	sh("ip link del l0; ip link del dn0; ip link del dn1");
}

static void t_wol(void)
{
	unsigned char mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}, pkt[102];
	netman_ent e;
	int pfd[2], k;
	pid_t c;
	printf("Wake-on-LAN\n");
	CHECK(netman_ent_wol_packet(mac, pkt) == 102);
	{
		int ok = 1;
		for (k = 0; k < 6; k++) ok &= pkt[k] == 0xff;
		for (k = 0; k < 16; k++) ok &= !memcmp(pkt + 6 + 6 * k, mac, 6);
		CHECK(ok, "6x FF + 16x MAC");
	}
	CHECK(netman_ent_wol_send("lo", "zz") < 0);
	e = mk("kind=wol\nname=lo\nwol=g\n");
	{
		int r = netman_ent_add(&e);
		if (r < 0) SKIP("ethtool WoL setting: %s", netman_ent_error());
		else CHECK(1, "WoL set");
	}
	e = mk("kind=wol\nname=lo\nwol=xyz\n");
	CHECK(netman_ent_add(&e) == -EINVAL);
	if (start_holder() < 0) { SKIP("no holder netns"); return; }
	sh("ip link add wl0 type veth peer name wl1; ip link set wl1 netns %d; ip addr add 10.77.0.1/24 dev wl0; ip link set wl0 up", (int)holder);
	hsh("ip addr add 10.77.0.2/24 dev wl1; ip link set wl1 up");
	pipe(pfd);
	c = fork();
	if (c == 0) {
		char a[64];
		int fd, s = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(9)};
		struct pollfd p;
		unsigned char buf[200];
		ssize_t n;
		snprintf(a, sizeof a, "/proc/%d/ns/net", (int)holder);
		if ((fd = open(a, O_RDONLY)) < 0 || setns(fd, CLONE_NEWNET) < 0) _exit(1);
		s = socket(AF_INET, SOCK_DGRAM, 0);
		if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0) _exit(2);
		write(pfd[1], "R", 1);
		p.fd = s, p.events = POLLIN;
		if (poll(&p, 1, 4000) <= 0) _exit(3);
		n = recv(s, buf, sizeof buf, 0);
		_exit(n == 102 && !memcmp(buf, pkt, 102) ? 0 : 4);
	}
	{
		char r;
		int st = -1;
		read(pfd[0], &r, 1);
		usleep(100000);
		CHECK(netman_ent_wol_send("wl0", "02:11:22:33:44:55") == 0, "%s", strerror(errno));
		waitpid(c, &st, 0);
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "listener in the peer netns saw the exact magic packet (exit %d)", WEXITSTATUS(st));
	}
	sh("ip link del wl0");
	kill(holder, SIGKILL);
	waitpid(holder, NULL, 0);
	holder = 0;
}

/* ------------------------------------------------------------------ tether, dot1x */
static void t_tether(void)
{
	netman_ent_tether_dev d[8];
	netman_ent e;
	char b[2048], root[] = "/tmp/ent_sysXXXXXX", p[300], q[300], run[] = "/tmp/ent_runXXXXXX";
	int n;
	const char *oldsys = netman_ent_sys, *oldrun = netman_run_dir;
	printf("tether (USB/BT detect, nft, dnsmasq, share-out)\n");
	mkdtemp(root);
	/* fake sysfs: usb0 (rndis_host), usb1 (cdc_ncm), eth0 (r8169, not tethering), bnep0 (bluetooth), wlan0 without a driver link */
	sh("mkdir -p %s/class/net/usb0/device %s/class/net/usb1/device %s/class/net/eth0/device %s/class/net/bnep0 %s/class/net/wlan0 %s/bus/usb/drivers/rndis_host %s/bus/usb/drivers/cdc_ncm %s/bus/pci/drivers/r8169", root, root, root, root, root, root, root, root);
	sh("ln -s %s/bus/usb/drivers/rndis_host %s/class/net/usb0/device/driver; ln -s %s/bus/usb/drivers/cdc_ncm %s/class/net/usb1/device/driver; ln -s %s/bus/pci/drivers/r8169 %s/class/net/eth0/device/driver", root, root, root, root, root, root);
	netman_ent_sys = root;
	n = netman_ent_tether_detect(d, 8);
	CHECK(n == 3, "n=%d", n);
	if (n == 3) {
		CHECK(!strcmp(d[0].ifn, "bnep0") && !strcmp(d[0].kind, "bt"));
		CHECK(!strcmp(d[1].ifn, "usb0") && !strcmp(d[1].driver, "rndis_host") && !strcmp(d[1].kind, "usb"));
		CHECK(!strcmp(d[2].ifn, "usb1") && !strcmp(d[2].driver, "cdc_ncm"));
	}
	netman_ent_sys = oldsys;

	e = mk("kind=tether\nname=dn\nupstream=up0\naddress=192.168.44.1/24\n");
	CHECK(netman_ent_tether_nft(&e, b, sizeof b) > 0);
	CHECK(has(b, "table inet netman_tether {\n") && has(b, "delete table inet netman_tether") && has(b, "iifname \"dn\" oifname \"up0\" masquerade") &&
		      has(b, "iifname \"up0\" oifname \"dn\" ct state established,related accept") && has(b, "hook postrouting priority 100") && !has(b, "table inet netman {"),
	      "%s", b);
	CHECK(netman_ent_tether_dnsmasq(&e, b, sizeof b) > 0 && has(b, "interface=dn\n") && has(b, "listen-address=192.168.44.1\n") &&
		      has(b, "dhcp-range=192.168.44.10,192.168.44.100,255.255.255.0,12h\n") && has(b, "dhcp-option=3,192.168.44.1\n"), "%s", b);
	e = mk("kind=tether\nname=dn\naddress=10.9.0.1/30\n");
	CHECK(netman_ent_tether_dnsmasq(&e, b, sizeof b) < 0, "a /30 needs an explicit pool");
	e = mk("kind=tether\nname=dn\naddress=10.9.0.1/16\ndhcp_start=10.9.1.1\ndhcp_end=10.9.1.99\ndns=9.9.9.9\n");
	CHECK(netman_ent_tether_dnsmasq(&e, b, sizeof b) > 0 && has(b, "dhcp-range=10.9.1.1,10.9.1.99,255.255.0.0,12h") && has(b, "server=9.9.9.9"), "%s", b);
	CHECK(netman_ent_tether_nft(&e, b, sizeof b) > 0 && has(b, "iifname \"dn\" oifname != \"dn\" masquerade"), "%s", b);

	/* apply in the namespace: addresses, ip_forward, real nft table, dnsmasq */
	mkdtemp(run);
	netman_run_dir = run;
	sh("ip link add dn type dummy; ip link add up0 type dummy; ip link set up0 up");
	e = mk("kind=tether\nname=dn\nupstream=up0\naddress=192.168.44.1/24\n");
	{
		int r = netman_ent_add(&e);
		if (r == -ENOENT) SKIP("tether apply: %s", netman_ent_error());
		else if (r < 0 && has(netman_ent_error(), "dnsmasq")) {
			SKIP("dnsmasq cannot drop privileges inside a single-uid user namespace: %s", netman_ent_error());
			r = 0;
		} else CHECK(r == 0, "%s", netman_ent_error());
		cap_cmd("nft list table inet netman_tether 2>&1", b, sizeof b);
		CHECK(has(b, "masquerade") && has(b, "iifname \"dn\" oifname \"up0\""), "nft says: %s", b);
		cap_cmd("ip -4 addr show dn", b, sizeof b);
		CHECK(has(b, "192.168.44.1/24"), "%s", b);
		{
			char f[16] = {0};
			int fd = open("/proc/sys/net/ipv4/ip_forward", O_RDONLY);
			if (fd >= 0) read(fd, f, 15), close(fd);
			CHECK(f[0] == '1', "ip_forward=%s", f);
		}
		snprintf(p, sizeof p, "%s/tether-dn.conf", run);
		CHECK(access(p, R_OK) == 0 || r != 0, "dnsmasq config written");
		{
			char c[400], o[300];
			snprintf(c, sizeof c, "dnsmasq --test -C %s 2>&1", p);
			cap_cmd(c, o, sizeof o);
			CHECK(has(o, "syntax check OK"), "dnsmasq --test: %s", o);
		}
		netman_ent_del(&e);
		cap_cmd("nft list table inet netman_tether 2>&1", b, sizeof b);
		CHECK(!has(b, "masquerade"), "table removed again: %s", b);
		(void)q;
	}
	e = mk("kind=tether\nname=bnep0\nmode=panu\nbtmac=AA:BB:CC:DD:EE:FF\n");
	n = netman_ent_add(&e);
	CHECK(n < 0, "BT PAN must fail cleanly here (no bluez/D-Bus in the test): %d %s", n, netman_ent_error());
	SKIP("USB RNDIS/CDC tether and Bluetooth PAN need a real phone/bluez: detection, rulesets and config text are tested; the live link is UNVERIFIED");
	netman_ent_sys = oldsys;
	netman_run_dir = oldrun;
	sh("ip link del dn; ip link del up0; rm -rf %s %s", root, run);
}

static void t_dot1x(void)
{
	netman_ent e;
	char b[1024], run[] = "/tmp/ent_dx1XXXXXX", p[300];
	struct stat st;
	const char *oldrun = netman_run_dir;
	printf("802.1X wired\n");
	e = mk("kind=dot1x\nname=eth9\neap=PEAP\nidentity=alice\npassword=s3cret\nca_cert=/etc/ssl/ca.pem\n");
	CHECK(netman_ent_dot1x_conf(&e, b, sizeof b) > 0);
	CHECK(has(b, "key_mgmt=IEEE8021X") && has(b, "eap=PEAP") && has(b, "identity=\"alice\"") && has(b, "password=\"s3cret\"") && has(b, "phase2=\"auth=MSCHAPV2\"") &&
		      has(b, "ca_cert=\"/etc/ssl/ca.pem\"") && has(b, "eapol_flags=0") && has(b, "ap_scan=0"), "%s", b);
	e = mk("kind=dot1x\nname=eth9\neap=TLS\nidentity=host/a\nclient_cert=/c.pem\nprivate_key=/k.pem\n");
	CHECK(netman_ent_dot1x_conf(&e, b, sizeof b) > 0 && has(b, "eap=TLS") && has(b, "client_cert=\"/c.pem\"") && has(b, "private_key=\"/k.pem\"") && !has(b, "phase2"), "%s", b);
	e = mk("kind=dot1x\nname=eth9\nidentity=a\n");
	CHECK(netman_ent_dot1x_conf(&e, b, sizeof b) < 0, "PEAP without password");
	/* writing the config + starting the supplicant: the config file is the only part verifiable without a switch */
	mkdtemp(run);
	netman_run_dir = run;
	e = mk("kind=dot1x\nname=dx0\nidentity=alice\npassword=s3cret\n");
	sh("ip link add dx0 type dummy");
	{
		int r = netman_ent_add(&e);
		snprintf(p, sizeof p, "%s/dot1x-dx0.conf", run);
		CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0600, "config holds a password: mode 0600");
		if (r < 0) SKIP("wpa_supplicant wired start: %s", netman_ent_error());
		else CHECK(1, "supplicant started");
		netman_ent_del(&e);
		CHECK(access(p, F_OK) != 0, "config removed on del");
	}
	SKIP("802.1X against a real authenticator is UNVERIFIED (needs a switch/RADIUS)");
	netman_run_dir = oldrun;
	sh("ip link del dx0; rm -rf %s", run);
}

/* ------------------------------------------------------------------ licences + keys */
static time_t g_now;
static time_t fake_now(void) { return g_now; }
static void put(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");
	fputs(s, f);
	fclose(f);
}
static void t_licence(void)
{
	char dir[] = "/tmp/ent_licXXXXXX", p[400], hook[400], b[4096], pub[45], priv[45], pub2[45], olds[100], news[100];
	netman_lic l;
	struct tm t = {.tm_year = 126, .tm_mon = 10, .tm_mday = 8, .tm_hour = 12};
	FILE *f;
	struct stat st, st2;
	char out[2048];
	printf("licences and key rotation\n");
	mkdtemp(dir);
	netman_lic_dir = dir;
	g_now = timegm(&t); /* 2026-11-08 12:00 UTC */
	netman_lic_now = fake_now;
	snprintf(p, sizeof p, "%s/ok.lic", dir), put(p, "name=ok\nholder=ACME\nkind=support\nexpires=2027-12-31\nkey=AAA\n");
	snprintf(p, sizeof p, "%s/soon.lic", dir), put(p, "name=soon\nholder=ACME\nkind=seat\nexpires=2026-11-20\n");
	snprintf(p, sizeof p, "%s/old.lic", dir), put(p, "name=old\nexpires=2026-11-01\n");
	snprintf(p, sizeof p, "%s/today.lic", dir), put(p, "name=today\nexpires=2026-11-08\n");
	snprintf(p, sizeof p, "%s/forever.lic", dir), put(p, "name=forever\nexpires=never\n");
	snprintf(p, sizeof p, "%s/junk.lic", dir), put(p, "name=junk\nexpires=yesterday\n");
	CHECK(netman_lic_load("ok", &l) == 0 && l.state == 0 && l.days > 400 && !strcmp(l.holder, "ACME"));
	CHECK(netman_lic_load("soon", &l) == 0 && l.state == 1 && l.days == 12, "state %d days %d", l.state, l.days);
	CHECK(netman_lic_load("old", &l) == 0 && l.state == 2 && l.days < 0, "days %d", l.days);
	CHECK(netman_lic_load("today", &l) == 0 && l.state == 1, "valid through its whole expiry day: state %d", l.state);
	CHECK(netman_lic_load("forever", &l) == 0 && l.state == 0 && l.days == 99999);
	CHECK(netman_lic_load("junk", &l) < 0 && netman_lic_load("../x", &l) < 0);
	f = fmemopen(out, sizeof out, "w");
	CHECK(netman_lic_status(f) == 3, "expiring: soon, today; expired: old");
	fclose(f);
	CHECK(has(out, "EXPIRED") && has(out, "EXPIRING") && has(out, "old") && has(out, "forever") && !has(out, "junk"), "%s", out);
	g_now += 86400 * 2; /* today's licence is now past its day */
	CHECK(netman_lic_load("today", &l) == 0 && l.state == 2);
	g_now -= 86400 * 2;

	/* renew through a hook; no network involved */
	netman_lic_renew_hook = "";
	CHECK(netman_lic_renew("soon") == -ENOENT, "no hook configured");
	snprintf(hook, sizeof hook, "%s/hook", dir);
	put(hook, "#!/bin/sh\nsed 's/^expires=.*/expires=2028-06-30/' \"$2\" > \"$3\"\n");
	chmod(hook, 0700);
	netman_lic_renew_hook = hook;
	CHECK(netman_lic_renew("soon") == 0, "%s", netman_ent_error());
	CHECK(netman_lic_load("soon", &l) == 0 && l.state == 0 && l.days > 500 && !strcmp(l.holder, "ACME"), "days %d", l.days);
	snprintf(p, sizeof p, "%s/soon.lic.prev", dir);
	CHECK(access(p, R_OK) == 0, "previous licence kept");
	snprintf(p, sizeof p, "%s/soon.lic.new", dir);
	CHECK(access(p, F_OK) != 0, "no temp file left");
	put(hook, "#!/bin/sh\ncp \"$2\" \"$3\"\n"); /* hook that returns the same expiry */
	CHECK(netman_lic_renew("ok") < 0 && has(netman_ent_error(), "later"), "%s", netman_ent_error());
	CHECK(netman_lic_load("ok", &l) == 0 && l.days > 400, "untouched after refused renewal");
	put(hook, "#!/bin/sh\nexit 3\n");
	CHECK(netman_lic_renew("old") < 0);
	put(hook, "#!/bin/sh\necho 'name=other\nexpires=2040-01-01' > \"$3\"\n");
	CHECK(netman_lic_renew("old") < 0 && has(netman_ent_error(), "not"), "%s", netman_ent_error());
	netman_lic_renew_hook = "/nonexistent/hook";
	CHECK(netman_lic_renew("old") == -ENOENT);
	netman_lic_renew_hook = "";

	/* keys: RFC 7748 section 6.1 */
	{
		static const unsigned char sk[32] = {0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45, 0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a, 0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a};
		static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		char enc[45];
		int i, o = 0;
		for (i = 0; i < 30; i += 3) {
			unsigned v = sk[i] << 16 | sk[i + 1] << 8 | sk[i + 2];
			enc[o++] = b64[v >> 18], enc[o++] = b64[(v >> 12) & 63], enc[o++] = b64[(v >> 6) & 63], enc[o++] = b64[v & 63];
		}
		{
			unsigned v = sk[30] << 16 | sk[31] << 8;
			enc[o++] = b64[v >> 18], enc[o++] = b64[(v >> 12) & 63], enc[o++] = b64[(v >> 6) & 63], enc[o++] = '=';
		}
		enc[o] = 0;
		CHECK(netman_key_pub(enc, pub) == 0);
		/* expected public key 8520f009...9b4e6a, in base64 */
		{
			static const unsigned char pk[32] = {0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54, 0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a, 0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4, 0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a};
			char want[45];
			o = 0;
			for (i = 0; i < 30; i += 3) {
				unsigned v = pk[i] << 16 | pk[i + 1] << 8 | pk[i + 2];
				want[o++] = b64[v >> 18], want[o++] = b64[(v >> 12) & 63], want[o++] = b64[(v >> 6) & 63], want[o++] = b64[v & 63];
			}
			{
				unsigned v = pk[30] << 16 | pk[31] << 8;
				want[o++] = b64[v >> 18], want[o++] = b64[(v >> 12) & 63], want[o++] = b64[(v >> 6) & 63], want[o++] = '=';
			}
			want[o] = 0;
			CHECK(!strcmp(pub, want), "got %s want %s", pub, want);
		}
	}
	CHECK(netman_key_generate(priv) == 0 && strlen(priv) == 44 && netman_key_pub(priv, pub) == 0 && strlen(pub) == 44);
	CHECK(netman_key_pub("short", pub) < 0);
	/* cross-check with wg when installed */
	{
		char cmd[200], o[100];
		snprintf(cmd, sizeof cmd, "echo '%s' | wg pubkey 2>/dev/null", priv);
		if (cap_cmd(cmd, o, sizeof o) == 0 && strlen(o) > 40) {
			o[strcspn(o, "\n")] = 0;
			CHECK(!strcmp(o, pub), "wg says %s, we say %s", o, pub);
		} else SKIP("wg not installed: pubkey cross-check against the RFC vector only");
	}
	netman_lic_apply_hook = "";
	CHECK(netman_key_rotate("wg0", pub) == 0, "%s", netman_ent_error());
	snprintf(p, sizeof p, "%s/wg0.key", dir);
	CHECK(stat(p, &st) == 0 && (st.st_mode & 0777) == 0600);
	CHECK(netman_key_show("wg0", pub2) == 0 && !strcmp(pub, pub2));
	{
		char kf[100] = {0};
		FILE *k = fopen(p, "r");
		fgets(kf, sizeof kf, k);
		fclose(k);
		kf[strcspn(kf, "\n")] = 0;
		strcpy(olds, kf);
	}
	CHECK(netman_key_rotate("wg0", pub2) == 0 && strcmp(pub, pub2), "second rotation yields a different key");
	snprintf(p, sizeof p, "%s/wg0.key.prev", dir);
	{
		char kf[100] = {0};
		FILE *k = fopen(p, "r");
		if (k) fgets(kf, sizeof kf, k), fclose(k);
		kf[strcspn(kf, "\n")] = 0;
		CHECK(!strcmp(kf, olds), "previous key preserved as .prev");
	}
	snprintf(b, sizeof b, "%s/wg0.key.new", dir);
	CHECK(access(b, F_OK) != 0, "no .new left");
	snprintf(b, sizeof b, "%s/wg0.key", dir);
	CHECK(stat(b, &st) == 0 && stat(p, &st2) == 0 && st.st_ino != st2.st_ino);
	{
		char kf[100] = {0};
		FILE *k = fopen(b, "r");
		fgets(kf, sizeof kf, k), fclose(k);
		kf[strcspn(kf, "\n")] = 0;
		strcpy(news, kf);
	}
	/* apply hook: failure rolls back, success reports the new pubkey */
	snprintf(hook, sizeof hook, "%s/apply", dir);
	put(hook, "#!/bin/sh\nexit 1\n");
	chmod(hook, 0700);
	netman_lic_apply_hook = hook;
	CHECK(netman_key_rotate("wg0", pub) < 0);
	{
		char kf[100] = {0};
		FILE *k = fopen(b, "r");
		fgets(kf, sizeof kf, k), fclose(k);
		kf[strcspn(kf, "\n")] = 0;
		CHECK(!strcmp(kf, news), "failed apply hook restores the previous key");
	}
	snprintf(b, sizeof b, "%s/applied", dir);
	snprintf(p, sizeof p, "#!/bin/sh\necho \"$1 $2\" > %s\n", b);
	put(hook, p);
	CHECK(netman_key_rotate("wg0", pub) == 0);
	{
		char got[200] = {0};
		FILE *k = fopen(b, "r");
		if (k) fgets(got, sizeof got, k), fclose(k);
		snprintf(p, sizeof p, "wg0 %s\n", pub);
		CHECK(!strcmp(got, p), "hook got NAME PUBKEY: %s", got);
	}
	CHECK(netman_key_show("nokey", pub) < 0 && netman_key_rotate("../x", pub) < 0);
	netman_lic_apply_hook = "";
	netman_lic_now = NULL;
	sh("rm -rf %s", dir);
}

int main(void)
{
	if (enter_ns() < 0) {
		fprintf(stderr, "enterprise_test: cannot create a user+net namespace; refusing to touch the host network\n");
		return 2;
	}
	sh("ip link set lo up");
	netman_dhcp_timeout_ms = 500;
	t_spec();
	t_dummy_bond();
	t_bridge();
	t_vlans();
	t_routes();
	t_ecmp();
	t_link_ipv6();
	t_wol();
	t_tether();
	t_dot1x();
	t_licence();
	printf("\n%d checks, %d failed, %d skipped\n", checks, fails, skips);
	return fails ? 1 : 0;
}
