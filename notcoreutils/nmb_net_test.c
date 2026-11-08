#define _GNU_SOURCE
/* nmb_net_test.c - CLI + self-test for nmb_net.h.
 *
 *   ./nmb_net_test list|print                 read-only, safe on the host
 *   ./nmb_net_test selftest [dnsmasq]         everything else, inside its OWN user+net namespace
 *   ./nmb_net_test server IF IP/PLEN POOL LEASE LOG   throw-away DHCP server (SIGUSR1 mute, SIGUSR2 NAK next)
 *
 * selftest unshares CLONE_NEWUSER|CLONE_NEWNET first, so the host network is never touched
 * (it refuses to run if that fails). Build: cc -O2 -o nmb_net_test nmb_net_test.c
 */
#define NMB_NET_IMPLEMENTATION
#include "nmb_net.h"

#include <sched.h>
#include <sys/prctl.h>
#include <sys/wait.h>

static int fails, checks;
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

/* ---------- throw-away DHCP server ---------- */
static volatile sig_atomic_t g_mute, g_nak;
static void on_usr1(int s) { (void)s; g_mute = !g_mute; }
static void on_usr2(int s) { (void)s; g_nak = 1; }
static void slog(const char *path, const char *what, int unicast)
{
	FILE *f = fopen(path, "a");
	if (f) fprintf(f, "%s%s\n", what, unicast ? " unicast" : ""), fclose(f);
}
static int serve(const char *ifn, const char *cidr, const char *pool, unsigned lease, const char *log)
{
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(67)};
	unsigned char macs[8][6], used = 0;
	uint32_t srv, pool0;
	char ip[32];
	int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1, plen;
	snprintf(ip, sizeof ip, "%s", cidr);
	plen = atoi(strchr(ip, '/') + 1);
	*strchr(ip, '/') = 0;
	inet_pton(AF_INET, ip, &srv);
	inet_pton(AF_INET, pool, &pool0);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifn, strlen(ifn) + 1) < 0 || bind(fd, (void *)&a, sizeof a) < 0) {
		perror("server bind");
		return 1;
	}
	signal(SIGUSR1, on_usr1);
	signal(SIGUSR2, on_usr2);
	for (;;) {
		uint8_t b[1500], o[300], *p;
		struct sockaddr_in from;
		socklen_t fl = sizeof from;
		ssize_t n = recvfrom(fd, b, sizeof b, 0, (void *)&from, &fl);
		int type = 0, i, slot = -1, nak = 0;
		uint32_t req = 0, ciaddr, yi;
		if (n < 241) continue;
		memcpy(&ciaddr, b + 12, 4);
		for (p = b + 240; p < b + n && *p != 255;) {
			if (*p == 0) {
				p++;
				continue;
			}
			if (p[0] == 53) type = p[2];
			if (p[0] == 50) memcpy(&req, p + 2, 4);
			p += 2 + p[1];
		}
		if (g_mute) {
			slog(log, "muted-drop", 0);
			continue;
		}
		for (i = 0; i < used; i++)
			if (!memcmp(macs[i], b + 28, 6)) slot = i;
		if (slot < 0 && used < 8) memcpy(macs[slot = used++], b + 28, 6);
		yi = htonl(ntohl(pool0) + slot);
		if (type == 7) {
			slog(log, "RELEASE", 1);
			continue;
		}
		if (type != 1 && type != 3) continue;
		if (type == 3 && ((req && req != yi) || (ciaddr && ciaddr != yi))) nak = 1;
		if (type == 3 && g_nak) nak = 1, g_nak = 0;
		slog(log, type == 1 ? "DISCOVER" : nak ? "REQUEST-nak" : "REQUEST", ciaddr != 0);
		memset(o, 0, sizeof o);
		o[0] = 2, o[1] = 1, o[2] = 6;
		memcpy(o + 4, b + 4, 4);	  /* xid */
		memcpy(o + 10, b + 10, 2);	  /* flags */
		if (!nak) memcpy(o + 16, &yi, 4); /* yiaddr */
		memcpy(o + 28, b + 28, 16);	  /* chaddr */
		memcpy(o + 236, "\x63\x82\x53\x63", 4);
		p = o + 240;
		*p++ = 53, *p++ = 1, *p++ = nak ? 6 : type == 1 ? 2 : 5;
		*p++ = 54, *p++ = 4, memcpy(p, &srv, 4), p += 4;
		if (!nak) {
			uint32_t m = htonl(~0u << (32 - plen)), l = htonl(lease), d2;
			inet_pton(AF_INET, "9.9.9.9", &d2);
			*p++ = 51, *p++ = 4, memcpy(p, &l, 4), p += 4;
			*p++ = 1, *p++ = 4, memcpy(p, &m, 4), p += 4;
			*p++ = 3, *p++ = 4, memcpy(p, &srv, 4), p += 4;
			*p++ = 6, *p++ = 8, memcpy(p, &srv, 4), memcpy(p + 4, &d2, 4), p += 8;
			*p++ = 15, *p++ = 7, memcpy(p, "lan.lan", 7), p += 7;
		}
		*p++ = 255;
		from.sin_family = AF_INET, from.sin_port = htons(68);
		if (ciaddr) from.sin_addr.s_addr = ciaddr;
		else from.sin_addr.s_addr = htonl(INADDR_BROADCAST);
		sendto(fd, o, p - o < 300 ? 300 : p - o, 0, (void *)&from, sizeof from);
	}
}

/* ---------- helpers ---------- */
static int sh(const char *fmt, ...)
{
	char c[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c, sizeof c, fmt, ap);
	va_end(ap);
	return system(c);
}
static int has_addr(const char *ifn, const char *pre)
{
	nmbn_if f;
	int i;
	if (nmbn_find(ifn, &f) < 0) return 0;
	for (i = 0; i < f.naddr; i++)
		if (!strncmp(f.addr[i], pre, strlen(pre))) return 1;
	return 0;
}
static int log_count(const char *log, const char *what)
{
	char c[300], line[128];
	int n = 0;
	FILE *f = fopen(log, "r");
	(void)c;
	if (!f) return 0;
	while (fgets(line, sizeof line, f))
		if (strstr(line, what)) n++;
	fclose(f);
	return n;
}
static int slurp_has(const char *path, const char *needle)
{
	char b[4096];
	int fd = open(path, O_RDONLY), n;
	if (fd < 0) return 0;
	n = read(fd, b, sizeof b - 1);
	close(fd);
	if (n < 0) return 0;
	b[n] = 0;
	return strstr(b, needle) != NULL;
}
static volatile int g_stop;
static void on_alarm(int s) { (void)s; g_stop = 1; }

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

/* The servers live in a second netns ("holder"): with client and server in ONE netns the kernel picks the
 * server's address as source and loops the broadcast back through lo, which is not what a real LAN does. */
static pid_t holder;
static int start_holder(void)
{
	char a[64], b[64], x[64], y[64];
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
	(void)b;
	return -1;
}
static void in_holder(void)
{
	char a[64];
	int fd;
	snprintf(a, sizeof a, "/proc/%d/ns/net", (int)holder);
	if ((fd = open(a, O_RDONLY)) < 0 || setns(fd, CLONE_NEWNET) < 0) _exit(1);
	close(fd);
}
static int srvsh(const char *fmt, ...)
{
	char c[512], d[700];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(c, sizeof c, fmt, ap);
	va_end(ap);
	snprintf(d, sizeof d, "nsenter --net=/proc/%d/ns/net sh -c '%s'", (int)holder, c);
	return system(d);
}
static long since_ms(const struct timespec *a)
{
	struct timespec b;
	clock_gettime(CLOCK_MONOTONIC, &b);
	return (b.tv_sec - a->tv_sec) * 1000 + (b.tv_nsec - a->tv_nsec) / 1000000;
}
static long wait_addr(const char *ifn, const char *pre, int want, int max_ms)
{ /* ms until has_addr(ifn,pre)==want, -1 on timeout */
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (since_ms(&t0) < max_ms) {
		if (has_addr(ifn, pre) == want) return since_ms(&t0);
		usleep(1000);
	}
	return -1;
}
static void http_serve(const char *ip)
{ /* tiny probe target: 204 normally, 302 "portal" after SIGUSR1 */
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(80)};
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	inet_pton(AF_INET, ip, &a.sin_addr);
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	if (bind(fd, (void *)&a, sizeof a) < 0 || listen(fd, 4) < 0) _exit(1);
	signal(SIGUSR1, on_usr1);
	for (;;) {
		char buf[512];
		int c = accept(fd, NULL, NULL);
		const char *r204 = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n",
			   *r302 = "HTTP/1.1 302 Found\r\nLocation: http://login.example/\r\nConnection: close\r\n\r\n";
		if (c < 0) continue;
		if (read(c, buf, sizeof buf) > 0) write(c, g_mute ? r302 : r204, strlen(g_mute ? r302 : r204));
		close(c);
	}
}
static pid_t start_server(const char *ifn, const char *cidr, const char *pool, unsigned lease, const char *log)
{
	pid_t p = fork();
	if (p == 0) prctl(PR_SET_PDEATHSIG, SIGKILL), in_holder(), _exit(serve(ifn, cidr, pool, lease, log));
	usleep(300000);
	return p;
}

static int selftest(int dnsmasq)
{
	char dir[] = "/tmp/nmbn.XXXXXX", rd[300], cd[300], rc[300], log[300];
	nmbn_if f;
	nmbn_lease L;
	nmbn_dhcpc c;
	pid_t srv;
	int i;

	if (enter_ns() < 0) {
		printf("cannot create user+net namespace; refusing to touch the host\n");
		return 2;
	}
	if (!mkdtemp(dir)) return 2;
	snprintf(rd, sizeof rd, "%s/run", dir), snprintf(cd, sizeof cd, "%s/conf", dir);
	snprintf(rc, sizeof rc, "%s/resolv.conf", dir), snprintf(log, sizeof log, "%s/server.log", dir);
	nmbn_run_dir = rd, nmbn_conf_dir = cd, nmbn_resolv_path = rc;
	nmbn_dns_mode = 1; /* never talk to the host's systemd-resolved */
	nmbn_dhcp_rto_ms = 500;
	printf("namespace ok, state in %s\n", dir);
	if (getuid() != 0) return 2;
	sh("ip link set lo up; ip link add d0 type dummy; ip link add v0 type veth peer name v1; ip link add wl0 type veth peer name w1");
	if (start_holder() < 0 || sh("ip link set v1 netns %d; ip link set w1 netns %d", (int)holder, (int)holder)) {
		printf("cannot create the server namespace\n");
		return 2;
	}
	srvsh("ip link set lo up");

	if (dnsmasq) {
		pid_t d;
		printf("[dnsmasq interop]\n");
		srvsh("ip link set v1 up; ip addr add 10.78.0.1/24 dev v1");
		d = fork();
		if (d == 0) {
			prctl(PR_SET_PDEATHSIG, SIGKILL);
			in_holder();
			execlp("dnsmasq", "dnsmasq", "--keep-in-foreground", "--user=root", "--interface=v1", "--bind-interfaces",
			       "--port=0", "--no-resolv", "--no-hosts", "--dhcp-range=10.78.0.50,10.78.0.60,5m",
			       "--dhcp-option=6,1.1.1.1,8.8.8.8", "--dhcp-option=3,10.78.0.1", "--dhcp-option=15,dm.lan",
			       "--dhcp-leasefile=/dev/null", "--pid-file=", NULL);
			_exit(127);
		}
		sleep(1);
		memset(&L, 0, sizeof L);
		i = nmbn_dhcp_acquire("v0", 8000, &L);
		CHECK(i == 0, "acquire=%d", i);
		if (i == 0) {
			char want[32];
			uint32_t ip = ntohl(L.ip);
			nmbn_lease_print(stdout, &L);
			CHECK(ip >= 0x0a4e0032 && ip <= 0x0a4e003c, "ip in pool");
			CHECK(L.ndns == 2 && L.router, "dns+router parsed");
			CHECK(L.lease_s == 300 && L.t1 == 150 && L.t2 == 262, "lease 300 -> t1 150 t2 262, got %u/%u", L.t1, L.t2);
			CHECK(strcmp(L.domain, "dm.lan") == 0, "domain '%s'", L.domain);
			CHECK(nmbn_lease_apply(&L) == 0, "apply");
			snprintf(want, sizeof want, "10.78.0.%u/24", ip & 0xff);
			CHECK(has_addr("v0", want), "address %s present", want);
			CHECK(slurp_has(rc, "nameserver 1.1.1.1"), "resolv.conf written");
			CHECK(nmbn_dhcp_renew(&L, 0, 3000) == 0, "unicast renew against dnsmasq");
			nmbn_dhcp_release(&L);
			nmbn_lease_unapply(&L);
		}
		kill(d, SIGTERM);
		waitpid(d, NULL, 0);
		goto done;
	}

	/* --- listing --- */
	printf("[list]\n");
	CHECK(nmbn_find("d0", &f) == 0 && !f.up, "d0 found, down");
	CHECK(nmbn_find("nope0", &f) == -ENODEV, "missing iface -> ENODEV");
	{
		nmbn_if t[NMBN_MAXIF];
		CHECK(nmbn_list(t, NMBN_MAXIF) == 4, "lo d0 v0 wl0 (servers are in the other namespace)");
	}
	/* --- link / addr / route --- */
	printf("[link addr route]\n");
	CHECK(nmbn_link_set("d0", 1) == 0 && (nmbn_find("d0", &f), f.up), "link up");
	CHECK(nmbn_link_set("d0", 0) == 0 && (nmbn_find("d0", &f), !f.up), "link down");
	nmbn_link_set("d0", 1);
	CHECK(nmbn_addr_add("d0", "10.1.0.5/24") == 0 && has_addr("d0", "10.1.0.5/24"), "v4 add");
	CHECK(nmbn_addr_add("d0", "10.1.0.5/24") == 0, "v4 add twice is idempotent");
	CHECK(nmbn_addr_add("d0", "fd00:1::5/64") == 0 && has_addr("d0", "fd00:1::5/64"), "v6 add");
	CHECK(nmbn_addr_add("d0", "10.1.0.999/24") < 0 && nmbn_addr_add("d0", "10.1.0.5") < 0, "bad cidr rejected");
	CHECK(nmbn_route_add("d0", "default", "10.1.0.1", 0) == 0, "default route");
	nmbn_find("d0", &f);
	CHECK(strcmp(f.gw4, "10.1.0.1") == 0, "gw4 '%s'", f.gw4);
	CHECK(nmbn_route_add("d0", "10.9.0.0/16", "10.1.0.2", 100) == 0, "static route with metric");
	CHECK(nmbn_route_add("d0", "10.1.0.250/32", NULL, 0) == 0, "on-link route");
	CHECK(nmbn_route_add("d0", "default", "fd00:1::1", 0) == 0, "v6 default");
	nmbn_find("d0", &f);
	CHECK(strcmp(f.gw6, "fd00:1::1") == 0, "gw6 '%s'", f.gw6);
	{
		int has = 0;
		for (i = 0; i < f.nroute; i++) has |= !strcmp(f.route[i], "10.9.0.0/16 via 10.1.0.2");
		CHECK(has, "route listed");
	}
	CHECK(nmbn_route_add("d0", "10.9.0.0/16", "fd00::1", 0) < 0, "family mismatch rejected");
	CHECK(nmbn_route_del("d0", "10.9.0.0/16", "10.1.0.2", 100) == 0, "route del");
	CHECK(nmbn_route_del("d0", "10.9.0.0/16", "10.1.0.2", 100) == 0, "route del twice ok");
	CHECK(nmbn_route_del("d0", "default", "10.1.0.1", 0) == 0 && (nmbn_find("d0", &f), !f.gw4[0]), "default del");
	CHECK(nmbn_addr_flush("d0") == 0 && !has_addr("d0", "10.") && !has_addr("d0", "fd00"), "flush");
	CHECK(nmbn_addr_del("d0", "10.1.0.5/24") == 0, "del absent ok");
	CHECK(nmbn_addr_add("zz9", "10.1.0.5/24") == -ENODEV, "unknown iface");

	/* --- dns file --- */
	printf("[dns]\n");
	{
		const char *a[] = {"1.1.1.1", "9.9.9.9"}, *b[] = {"1.1.1.1", "2606:4700::1111"}, *bad[] = {"1.1.1"};
		FILE *o = fopen(rc, "w");
		fputs("nameserver 192.0.2.53\n", o);
		fclose(o);
		CHECK(nmbn_dns_set("d0", bad, 1, NULL) < 0, "bad dns rejected");
		CHECK(nmbn_dns_set("d0", a, 2, "lan") == 0, "set d0");
		CHECK(slurp_has(rc, "nameserver 1.1.1.1") && slurp_has(rc, "search lan") && !slurp_has(rc, "192.0.2.53"), "file content");
		CHECK(nmbn_dns_set("v0", b, 2, NULL) == 0, "set v0");
		{
			char t[2000];
			int fd = open(rc, O_RDONLY), n = read(fd, t, sizeof t - 1), k = 0;
			char *p;
			close(fd);
			t[n] = 0;
			for (p = t; (p = strstr(p, "nameserver 1.1.1.1")); p++) k++;
			CHECK(k == 1 && slurp_has(rc, "2606:4700::1111"), "merged, duplicate server once");
		}
		CHECK(nmbn_dns_clear("d0") == 0 && slurp_has(rc, "2606:4700::1111") && !slurp_has(rc, "9.9.9.9"), "clear d0 keeps v0");
		CHECK(nmbn_dns_clear("v0") == 0 && slurp_has(rc, "192.0.2.53") && !slurp_has(rc, "generated"), "last clear restores original");
	}

	/* --- profiles --- */
	printf("[profiles]\n");
	{
		nmbn_profile p, q;
		char names[8][64];
		memset(&p, 0, sizeof p);
		snprintf(p.name, 64, "office");
		snprintf(p.iface, 16, "d0");
		snprintf(p.addr, 64, "10.2.0.5/24");
		snprintf(p.gateway, 46, "10.2.0.1");
		snprintf(p.dns[0], 46, "10.2.0.1");
		snprintf(p.dns[1], 46, "9.9.9.9");
		p.ndns = 2;
		snprintf(p.route[0], 96, "10.8.0.0/16 via 10.2.0.9");
		p.nroute = 1;
		p.autoconnect = 0;
		CHECK(nmbn_profile_save(&p) == 0, "save: %s", nmbn_error());
		CHECK(nmbn_profile_load("office", &q) == 0 && !q.dhcp && !strcmp(q.addr, p.addr) && q.ndns == 2 && q.nroute == 1 &&
			      !strcmp(q.route[0], p.route[0]) && !q.autoconnect,
		      "roundtrip");
		CHECK(nmbn_profile_load("../etc/passwd", &q) < 0, "path traversal refused");
		strcpy(p.name, "bad"), strcpy(p.addr, "10.2.0.300/24");
		CHECK(nmbn_profile_save(&p) < 0, "invalid address refused at save (%s)", nmbn_error());
		CHECK(nmbn_profile_load("bad", &q) < 0, "...and nothing was written");
		snprintf(p.name, 64, "wan"), p.dhcp = 1, p.addr[0] = 0, p.gateway[0] = 0, p.ndns = 0, p.nroute = 0, snprintf(p.iface, 16, "v0");
		p.autoconnect = 1;
		CHECK(nmbn_profile_save(&p) == 0, "dhcp profile save");
		CHECK(nmbn_profile_list(names, 8) == 2 && !strcmp(names[0], "office") && !strcmp(names[1], "wan"), "list sorted");
		{
			FILE *o;
			char path[400];
			snprintf(path, sizeof path, "%s/junk.conf", cd);
			o = fopen(path, "w");
			fputs("iface=d0\nmethod=carrier-pigeon\n", o);
			fclose(o);
			CHECK(nmbn_profile_load("junk", &q) < 0, "unknown method refused: %s", nmbn_error());
			unlink(path);
		}
		CHECK(nmbn_profile_load("office", &q) == 0 && nmbn_profile_apply(&q, NULL) == 0, "apply static: %s", nmbn_error());
		nmbn_find("d0", &f);
		CHECK(has_addr("d0", "10.2.0.5/24") && !strcmp(f.gw4, "10.2.0.1"), "static applied");
		CHECK(slurp_has(rc, "nameserver 10.2.0.1"), "static dns");
		CHECK(nmbn_profile_down(&q, NULL) == 0 && !has_addr("d0", "10.2.0.5") && !slurp_has(rc, "10.2.0.1"), "static down");
	}

	/* --- dhcp against our server --- */
	printf("[dhcp]\n");
	srvsh("ip link set v1 up; ip addr add 10.77.0.1/24 dev v1");
	srv = start_server("v1", "10.77.0.1/24", "10.77.0.100", 12, log);
	memset(&L, 0, sizeof L);
	i = nmbn_dhcp_acquire("v0", 8000, &L);
	CHECK(i == 0, "acquire=%d", i);
	if (i == 0) {
		nmbn_lease_print(stdout, &L);
		CHECK(L.ip == htonl(0x0a4d0064) && L.lease_s == 12 && L.t1 == 6 && L.t2 == 10, "lease fields");
		CHECK(L.ndns == 2 && strcmp(L.domain, "lan.lan") == 0, "dns+domain");
		CHECK(nmbn_lease_apply(&L) == 0, "apply");
		nmbn_find("v0", &f);
		CHECK(has_addr("v0", "10.77.0.100/24") && !strcmp(f.gw4, "10.77.0.1"), "addr + default route");
		CHECK(slurp_has(rc, "nameserver 10.77.0.1") && slurp_has(rc, "search lan.lan"), "dns applied");
		CHECK(sh("ping -c1 -W1 10.77.0.1 >/dev/null 2>&1") == 0, "ping the server over the new address");
		/* second acquire, same MAC -> same address (server keeps its binding) */
		{
			nmbn_lease l2;
			CHECK(nmbn_dhcp_acquire("v0", 5000, &l2) == 0 && l2.ip == L.ip, "re-acquire gets same ip");
		}
	}
	memset(&c, 0, sizeof c);
	c.l = L, c.st = 1;
	c.l.got = (time_t)(c.l.got - 0);
	/* T1: pretend 6 s passed -> unicast renew */
	c.l.got -= 6;
	{
		int before = log_count(log, "REQUEST unicast"), r = nmbn_dhcpc_step(&c, "v0");
		CHECK(r == 0 && log_count(log, "REQUEST unicast") == before + 1, "T1 renew sent unicast (r=%d)", r);
		CHECK(nmbn__now() - c.l.got <= 1, "lease timers restarted");
	}
	/* T2: server mute -> renew fails, then unmute -> rebind via broadcast works */
	c.l.got -= 10;
	kill(srv, SIGUSR1);
	usleep(100000);
	{
		int r = nmbn_dhcpc_step(&c, "v0");
		CHECK(r < 0 && has_addr("v0", "10.77.0.100/24"), "rebind attempt fails while server is mute (r=%d), address kept", r);
		kill(srv, SIGUSR1);
		usleep(100000);
		c.next_try = 0;
		c.l.got = nmbn__now() - 10; /* absolute: the 1 s mute timeout must not push us past the 12 s lease */
		r = nmbn_dhcpc_step(&c, "v0");
		CHECK(r == 0 && nmbn__now() - c.l.got <= 1, "rebind (broadcast) succeeded after unmute (r=%d)", r);
		CHECK(log_count(log, "REQUEST") >= 3, "server saw the broadcast REQUEST");
	}
	/* NAK: server revokes -> deconfigure, next step acquires again */
	c.l.got = nmbn__now() - 7;
	c.next_try = 0;
	kill(srv, SIGUSR2);
	usleep(100000);
	{
		int r = nmbn_dhcpc_step(&c, "v0");
		CHECK(r == 1 && c.st == 0 && !has_addr("v0", "10.77.0.100"), "NAK removes address (r=%d)", r);
		c.next_try = 0;
		r = nmbn_dhcpc_step(&c, "v0");
		CHECK(r == 0 && c.st == 1 && has_addr("v0", "10.77.0.100/24"), "reacquired after NAK (r=%d)", r);
	}
	/* expiry */
	c.l.got -= 100;
	CHECK(nmbn_dhcpc_step(&c, "v0") == 1 && c.st == 0 && !has_addr("v0", "10.77.0.100") && !slurp_has(rc, "10.77.0.1"),
	      "expired lease dropped, dns cleared");
	/* release */
	c.next_try = 0;
	CHECK(nmbn_dhcpc_step(&c, "v0") == 0 && c.st == 1, "bound again");
	nmbn_dhcpc_stop(&c, 1);
	usleep(200000);
	CHECK(log_count(log, "RELEASE") == 1 && !has_addr("v0", "10.77.0.100"), "release sent, address removed");

	/* --- autoconnect + roaming: eth (v0, net A 10.77/24) and wifi (wl0, net B 10.88/24, plus a broken higher-priority profile) --- */
	printf("[autoconnect / roaming]\n");
	{
		nmbn_profile p;
		pid_t ac, srvb, web;
		char logb[300], cache[400], out[2048];
		struct timespec t0;
		long t_cold, t_wifi, t_cached, t_nak, t_unplug;
		int d_before, n;
		FILE *pp;

		snprintf(logb, sizeof logb, "%s/serverB.log", dir);
		snprintf(cache, sizeof cache, "%s/lease.wan", dir);
		nmbn_state_dir = dir;
		nmbn_profile_load("office", &p), p.autoconnect = 0, nmbn_profile_save(&p);
		nmbn_profile_load("wan", &p), p.priority = 10, p.autoconnect = 1, nmbn_profile_save(&p);
		memset(&p, 0, sizeof p);
		snprintf(p.name, 64, "wifi"), snprintf(p.iface, 16, "wl0"), p.dhcp = 1, p.autoconnect = 1, p.priority = 5;
		CHECK(nmbn_profile_save(&p) == 0, "wifi profile");
		memset(&p, 0, sizeof p); /* 'ghost': best priority on wl0 but its apply always fails (v6 gateway for a v4 route) */
		snprintf(p.name, 64, "ghost"), snprintf(p.iface, 16, "wl0"), snprintf(p.addr, 64, "10.88.0.77/24");
		snprintf(p.route[0], 96, "10.5.0.0/16 via fd00::1"), p.nroute = 1, p.autoconnect = 1, p.priority = 20;
		CHECK(nmbn_profile_save(&p) == 0, "ghost profile: %s", nmbn_error());

		/* net B */
		srvsh("ip link set w1 up; ip addr add 10.88.0.1/24 dev w1");
		srvb = start_server("w1", "10.88.0.1/24", "10.88.0.100", 12, logb);
		web = fork();
		if (web == 0) prctl(PR_SET_PDEATHSIG, SIGKILL), in_holder(), http_serve("10.88.0.1");
		srvsh("ip link set v1 down; ip link set w1 down"); /* nothing plugged in yet */
		usleep(300000);

		ac = fork();
		if (ac == 0) {
			g_stop = 0;
			signal(SIGTERM, on_alarm);
			nmbn_autoconnect(&g_stop, 30000);
			_exit(0);
		}
		usleep(300000);

		clock_gettime(CLOCK_MONOTONIC, &t0);
		srvsh("ip link set v1 up");
		t_cold = wait_addr("v0", "10.77.0.100/24", 1, 8000);
		(void)t0;
		CHECK(t_cold >= 0, "ethernet: carrier -> address");
		printf("  time: ethernet carrier -> address, no cache (full DISCOVER/OFFER/REQUEST/ACK): %ld ms\n", t_cold);
		CHECK(access(cache, F_OK) == 0, "lease cached for next time");

		srvsh("ip link set w1 up");
		t_wifi = wait_addr("wl0", "10.88.0.1", 1, 12000);
		CHECK(t_wifi >= 0, "wifi up despite the failing higher-priority profile");
		printf("  time: wifi carrier -> address, with a broken top-priority profile tried first: %ld ms\n", t_wifi);
		CHECK(!has_addr("wl0", "10.88.0.77"), "ghost's half-applied address was cleaned");

		pp = popen("ip -4 route show default", "r");
		n = pp ? fread(out, 1, sizeof out - 1, pp) : 0;
		if (pp) pclose(pp);
		out[n > 0 ? n : 0] = 0;
		CHECK(strstr(out, "dev v0 metric 100") && strstr(out, "dev wl0 metric 600"), "two default routes, ethernet preferred by metric:\n%s", out);
		CHECK(slurp_has(rc, "10.77.0.1") && slurp_has(rc, "10.88.0.1"), "dns from both networks present");

		/* unplug ethernet */
		clock_gettime(CLOCK_MONOTONIC, &t0);
		srvsh("ip link set v1 down");
		t_unplug = wait_addr("v0", "10.77.", 0, 5000);
		CHECK(t_unplug >= 0, "unplug -> address removed");
		usleep(100000);
		pp = popen("ip -4 route show default", "r");
		n = pp ? fread(out, 1, sizeof out - 1, pp) : 0;
		if (pp) pclose(pp);
		out[n > 0 ? n : 0] = 0;
		CHECK(!strstr(out, "dev v0"), "no stale default route via the unplugged interface");
		CHECK(!slurp_has(rc, "10.77.0.1") && slurp_has(rc, "10.88.0.1"), "stale dns of the old network gone, new one kept");
		printf("  time: ethernet unplug -> old address/routes/dns removed: %ld ms\n", t_unplug);

		/* replug: cached lease (INIT-REBOOT) */
		d_before = log_count(log, "DISCOVER");
		srvsh("ip link set v1 up");
		t_cached = wait_addr("v0", "10.77.0.100/24", 1, 8000);
		CHECK(t_cached >= 0 && log_count(log, "DISCOVER") == d_before, "replug reused the cached lease, no DISCOVER (%d -> %d)", d_before, log_count(log, "DISCOVER"));
		printf("  time: ethernet replug with cached lease (INIT-REBOOT): %ld ms\n", t_cached);

		/* replug: server NAKs the cached lease -> fall back to a full DHCP */
		srvsh("ip link set v1 down");
		wait_addr("v0", "10.77.", 0, 5000);
		kill(srv, SIGUSR2);
		usleep(100000);
		d_before = log_count(log, "DISCOVER");
		srvsh("ip link set v1 up");
		t_nak = wait_addr("v0", "10.77.0.100/24", 1, 8000);
		CHECK(t_nak >= 0 && log_count(log, "DISCOVER") == d_before + 1, "NAK -> full DISCOVER fallback");
		printf("  time: ethernet replug, cached lease NAKed, full DHCP fallback: %ld ms\n", t_nak);

		/* priority switch: wifi alone is up when ethernet drops; nothing else should be touched */
		CHECK(has_addr("wl0", "10.88.0.1"), "wifi stayed connected through all ethernet changes");

		/* connectivity probe (direct, via wl0) */
		nmbn_probe_ip = "10.88.0.1";
		CHECK(nmbn_probe("wl0", 2000) == 0, "probe: 204 -> internet");
		kill(web, SIGUSR1);
		usleep(100000);
		CHECK(nmbn_probe("wl0", 2000) == 1, "probe: 302 -> captive portal");
		kill(web, SIGKILL);
		waitpid(web, NULL, 0);
		usleep(100000);
		CHECK(nmbn_probe("wl0", 1500) < 0, "probe: nothing listening -> no connectivity");
		nmbn_probe_ip = "";

		kill(ac, SIGTERM);
		waitpid(ac, NULL, 0);
		CHECK(!has_addr("v0", "10.77.") && !has_addr("wl0", "10.88."), "SIGTERM -> clean shutdown, nothing left behind");
		kill(srvb, SIGKILL);
		waitpid(srvb, NULL, 0);
	}
	/* --- random MAC + lease cache file --- */
	printf("[mac / cache]\n");
	{
		unsigned char a[6], b[6], c2[6];
		nmbn_if f;
		nmbn_lease l;
		nmbn_mac_for("home", a), nmbn_mac_for("home", b), nmbn_mac_for("cafe", c2);
		CHECK(!memcmp(a, b, 6) && memcmp(a, c2, 6) && (a[0] & 2) && !(a[0] & 1), "stable per key, differs per key, locally administered unicast");
		CHECK(nmbn_mac_set("d0", a) == 0 && nmbn_find("d0", &f) == 0 && !memcmp(f.mac, a, 6) && f.up, "mac set, link back up");
		CHECK(nmbn_lease_load("wan", "v0", &l) == 0 && l.ndns == 2 && !strcmp(l.domain, "lan.lan"), "cache file loads");
		nmbn_lease_forget("wan");
		CHECK(nmbn_lease_load("wan", "v0", &l) < 0, "forget");
	}
	kill(srv, SIGKILL);
	waitpid(srv, NULL, 0);
done:
	kill(holder, SIGKILL);
	sh("rm -rf %s", dir); /* our own mkdtemp dir in /tmp */
	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc >= 2 && (!strcmp(argv[1], "list") || !strcmp(argv[1], "print"))) return nmbn_print(stdout), 0;
	if (argc >= 2 && !strcmp(argv[1], "selftest")) return selftest(argc > 2 && !strcmp(argv[2], "dnsmasq"));
	if (argc == 7 && !strcmp(argv[1], "server")) return serve(argv[2], argv[3], argv[4], atoi(argv[5]), argv[6]);
	fprintf(stderr, "usage: %s list | selftest [dnsmasq] | server IF IP/PLEN POOL LEASE LOG\n", argv[0]);
	return 2;
}
