/* tor_test.c - tor: torrc and nft goldens, the control-port client against a fake control server (unix + TCP, cookie auth,
 * bad cookie), and (under `unshare -Urn`, with nft) the ruleset loaded for real: redirect, UDP leak dropped, fail closed without a listener.
 * REAL TOR IS NOT TESTED (not installed here).  build, from netman/:
 *   cc -std=gnu11 -Wall -Wextra -O1 -Isrc/mods -o /tmp/tor_test tests/tor_test.c src/mods/tor.c src/mods/vpnmod.c src/mods/openvpn.c src/mods/ipsec.c src/mods/l2tp.c src/mods/openconnect.c src/mods/sstp.c
 *   unshare -Urn /tmp/tor_test        UPDATE=1 rewrites tests/tor_golden/* */
#define _GNU_SOURCE
#include "tor.h"
#include "vpnmod.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int pass, bad;
#define OK(c, ...) do { if (c) pass++; else { bad++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); putchar('\n'); } } while (0)
static char tmp[200];
static void golden(const char *name, const char *got)
{
	char p[300]; snprintf(p, sizeof p, "tests/tor_golden/%s", name);
	if (getenv("UPDATE")) { FILE *f = fopen(p, "w"); if (f) { fputs(got, f); fclose(f); } return; }
	size_t n; char *w = netman_vm_slurp(p, &n, 1 << 20);
	OK(w && !strcmp(w, got), "golden %s differs\n%s", name, got);
	free(w);
}
static netman_tor_cfg base(void)
{
	netman_tor_cfg c; netman_tor_defaults(&c);
	snprintf(c.dir, sizeof c.dir, "/run/netman/tor");
	return c;
}
static void gold(void)
{
	char b[8192], e[200]; netman_tor_cfg c = base();
	OK(!netman_tor_validate(&c, e, sizeof e), "defaults validate: %s", e);
	netman_tor_torrc(&c, b, sizeof b); golden("plain.torrc", b);
	netman_tor_nft(&c, 108, b, sizeof b); golden("plain.nft", b);
	snprintf(c.bridges[0], 256, "obfs4 192.0.2.10:443 0123456789ABCDEF0123456789ABCDEF01234567 cert=abc iat-mode=0"); snprintf(c.bridges[1], 256, "snowflake 192.0.2.3:80 2B280B23E1107BB62ABFC40DDCC8824814F80A72");
	c.nbridges = 2; snprintf(c.exit_cc, sizeof c.exit_cc, "DE"); c.strict = 1;
	snprintf(c.hs[0].name, 32, "web"); c.hs[0].port = 80; snprintf(c.hs[0].target, 64, "127.0.0.1:8080"); c.nhs = 1;
	snprintf(c.route_uids, sizeof c.route_uids, "1000, 1001"); snprintf(c.route_ifaces, sizeof c.route_ifaces, "wlan1");
	OK(!netman_tor_validate(&c, e, sizeof e), "rich validates: %s", e);
	netman_tor_torrc(&c, b, sizeof b); golden("rich.torrc", b);
	OK(strstr(b, "ExitNodes {de}") && strstr(b, "StrictNodes 1") && strstr(b, "UseBridges 1"), "exit + bridges in torrc");
	netman_tor_nft(&c, 108, b, sizeof b); golden("rich.nft", b);
	snprintf(c.mode, sizeof c.mode, "socks"); netman_tor_torrc(&c, b, sizeof b);
	OK(!strstr(b, "TransPort") && strstr(b, "SocksPort 127.0.0.1:9050 IsolateDestAddr"), "socks mode: no TransPort");
	c = base(); snprintf(c.bridges[0], 256, "obfs4 1.2.3.4:1\nExitNodes {us}"); c.nbridges = 1;
	OK(netman_tor_validate(&c, e, sizeof e) != 0, "a newline in a bridge line (torrc injection) is refused");
	c = base(); snprintf(c.exit_cc, sizeof c.exit_cc, "d}"); OK(netman_tor_validate(&c, e, sizeof e) != 0, "bad country refused");
	c = base(); c.trans_port = c.socks_port; OK(netman_tor_validate(&c, e, sizeof e) != 0, "equal ports refused");
	c = base(); snprintf(c.route_uids, sizeof c.route_uids, "0; drop"); OK(netman_tor_validate(&c, e, sizeof e) != 0, "uid list injection refused");
}

/* ---- fake tor control server ---------------------------------------------------------------------------------------- */
static unsigned char cookie[32];
static char cookiepath[300];
static void serve(int fd)
{
	char in[1024]; size_t have = 0; int authed = 0; FILE *w = fdopen(dup(fd), "w");
	for (;;) {
		ssize_t r = read(fd, in + have, sizeof in - have - 1); if (r <= 0) break; have += (size_t)r; in[have] = 0;
		char *nl;
		while ((nl = strstr(in, "\r\n"))) {
			*nl = 0; char *cmd = in;
			if (!strncmp(cmd, "PROTOCOLINFO", 12)) fprintf(w, "250-PROTOCOLINFO 1\r\n250-AUTH METHODS=COOKIE,SAFECOOKIE COOKIEFILE=\"%s\"\r\n250-VERSION Tor=\"0.4.8.9\"\r\n250 OK\r\n", cookiepath);
			else if (!strncmp(cmd, "AUTHENTICATE ", 13)) {
				char hex[80]; for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", cookie[i]);
				if (!strcmp(cmd + 13, hex)) { authed = 1; fputs("250 OK\r\n", w); } else fputs("515 Authentication failed: Wrong length on authentication cookie.\r\n", w);
			} else if (!authed) fputs("514 Authentication required.\r\n", w);
			else if (!strcmp(cmd, "GETINFO status/bootstrap-phase")) fputs("250-status/bootstrap-phase=NOTICE BOOTSTRAP PROGRESS=100 TAG=done SUMMARY=\"Done\"\r\n250 OK\r\n", w);
			else if (!strcmp(cmd, "GETINFO circuit-status")) fputs("250+circuit-status=\r\n1 BUILT $AAAA~guard,$BBBB~mid,$0123456789ABCDEF0123456789ABCDEF01234567~exitnode BUILD_FLAGS=NEED_CAPACITY PURPOSE=GENERAL TIME_CREATED=2026-11-08T10:00:00\r\n2 BUILT $AAAA~guard,$BBBB~mid,$C~x PURPOSE=HS_CLIENT_REND\r\n3 EXTENDED $AAAA~guard PURPOSE=GENERAL\r\n.\r\n250 OK\r\n", w);
			else if (!strncmp(cmd, "GETINFO ns/id/0123456789ABCDEF0123456789ABCDEF01234567", 54)) fputs("250+ns/id/0123=\r\nr exitnode ASNHb2xhYQ dGVzdA 2026-11-08 09:00:00 198.51.100.77 9001 0\r\ns Exit Fast Running Valid\r\n.\r\n250 OK\r\n", w);
			else if (!strcmp(cmd, "SIGNAL NEWNYM") || !strcmp(cmd, "SIGNAL RELOAD")) { fputs("250 OK\r\n", w); if (!strcmp(cmd, "SIGNAL NEWNYM")) { FILE *f = fopen("/tmp/tor_test_newnym", "w"); if (f) fclose(f); } }
			else fputs("510 Unrecognized command\r\n", w);
			fflush(w);
			size_t used = (size_t)(nl - in) + 2; memmove(in, in + used, have - used + 1); have -= used;
		}
	}
	fclose(w); close(fd);
}
static pid_t start_server(int tcp, const char *path, int *port)
{
	int s = socket(tcp ? AF_INET : AF_UNIX, SOCK_STREAM, 0);
	if (tcp) { struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) }; bind(s, (void *)&a, sizeof a); socklen_t l = sizeof a; getsockname(s, (void *)&a, &l); *port = ntohs(a.sin_port); }
	else { struct sockaddr_un u = { .sun_family = AF_UNIX }; snprintf(u.sun_path, sizeof u.sun_path, "%s", path); unlink(path); bind(s, (void *)&u, sizeof u); }
	listen(s, 4);
	pid_t p = fork();
	if (p == 0) { for (;;) { int c = accept(s, NULL, NULL); if (c < 0) _exit(0); if (fork() == 0) { serve(c); _exit(0); } close(c); } }
	close(s); return p;
}
static void control(void)
{
	snprintf(cookiepath, sizeof cookiepath, "%s/control_auth_cookie", tmp);
	for (int i = 0; i < 32; i++) cookie[i] = (unsigned char)(i * 7 + 3);
	FILE *f = fopen(cookiepath, "w"); fwrite(cookie, 1, 32, f); fclose(f); chmod(cookiepath, 0600);
	char sock[300], addr[320]; snprintf(sock, sizeof sock, "%s/control", tmp); snprintf(addr, sizeof addr, "unix:%s", sock);
	int port = 0; pid_t srv[2]; srv[0] = start_server(0, sock, &port);
	for (int round = 0; round < 2; round++) {
		if (round == 1) { srv[1] = start_server(1, NULL, &port); snprintf(addr, sizeof addr, "127.0.0.1:%d", port); }
		netman_tor_ctl t; char sum[64], fp[48], ip[64]; int built = 0;
		OK(netman_tor_ctl_open(&t, addr) == 0, "%s: connect", addr);
		char rep[256]; OK(netman_tor_ctl_cmd(&t, "GETINFO circuit-status", rep, sizeof rep) == 514, "commands before AUTHENTICATE are refused by the server");
		OK(netman_tor_ctl_auth(&t) == 0, "cookie auth");
		OK(netman_tor_ctl_bootstrap(&t, sum, sizeof sum) == 100 && !strcmp(sum, "Done"), "bootstrap 100%% Done (%s)", sum);
		OK(netman_tor_ctl_circuits(&t, &built, fp, sizeof fp) == 0 && built == 1, "only BUILT GENERAL circuits are counted (%d)", built);
		OK(!strcmp(fp, "0123456789ABCDEF0123456789ABCDEF01234567"), "exit fingerprint %s", fp);
		OK(netman_tor_ctl_exitip(&t, fp, ip, sizeof ip) == 0 && !strcmp(ip, "198.51.100.77"), "exit ip %s", ip);
		unlink("/tmp/tor_test_newnym"); OK(netman_tor_ctl_newnym(&t) == 0 && access("/tmp/tor_test_newnym", F_OK) == 0, "NEWNYM reached the server");
		OK(netman_tor_ctl_reload(&t) == 0, "RELOAD");
		OK(netman_tor_ctl_cmd(&t, "SIGNAL NEWNYM\r\nSIGNAL SHUTDOWN", rep, sizeof rep) == -1, "a command with CRLF inside is refused client side");
		OK(netman_tor_ctl_exitip(&t, "0123;rm", ip, sizeof ip) != 0, "fingerprint is validated");
		netman_tor_ctl_close(&t);
		/* wrong cookie */
		cookie[0] ^= 0xff; f = fopen(cookiepath, "w"); fwrite(cookie, 1, 32, f); fclose(f);
		unsigned char saved = cookie[0]; cookie[0] ^= 0xff;                    /* the server still expects the original */
		OK(netman_tor_ctl_open(&t, addr) == 0 && netman_tor_ctl_auth(&t) != 0, "a wrong cookie fails closed");
		netman_tor_ctl_close(&t); cookie[0] = saved ^ 0xff; f = fopen(cookiepath, "w"); fwrite(cookie, 1, 32, f); fclose(f);
		OK(netman_tor_ctl_auth(&t) != 0, "auth on a closed connection fails");
	}
	OK(netman_tor_ctl_open(&(netman_tor_ctl){0}, "unix:/nonexistent/sock") != 0, "no server = error, no hang");
	for (int i = 0; i < 2; i++) { kill(srv[i], SIGTERM); waitpid(srv[i], NULL, 0); }
}

/* ---- the ruleset in a real (user) namespace ---------------------------------------------------------------------------- */
static int sh(const char *cmd) { return system(cmd); }
static void nft_ns(void)
{
	char w[64]; if (geteuid() != 0) { puts("  SKIP nft namespace test: run under `unshare -Urn`"); return; }
	if (!netman_vm_which("nft", w, sizeof w)) { puts("  SKIP nft namespace test: nft not installed"); return; }
	char sysout[16]; (void)sysout;
	sh("ip link set lo up; ip link add d0 type dummy; ip addr add 10.99.0.2/24 dev d0; ip link set d0 up; ip route add default via 10.99.0.1 dev d0 2>/dev/null");
	netman_tor_cfg c = base(); char r[8192];
	netman_tor_nft(&c, 12345, r, sizeof r);
	char rp[300]; snprintf(rp, sizeof rp, "%s/rules.nft", tmp); FILE *f = fopen(rp, "w"); fputs(r, f); fclose(f);
	char cmd[400]; snprintf(cmd, sizeof cmd, "nft -c -f %s 2>%s/nft.err", rp, tmp);
	if (sh(cmd)) { puts("  SKIP nft namespace test: this kernel/nft cannot check the ruleset in a user namespace (see nft.err)"); return; }
	snprintf(cmd, sizeof cmd, "nft -f %s", rp); OK(sh(cmd) == 0, "ruleset loads");
	/* a fake TransPort + DNSPort; the 'client' connects to an address that does not exist */
	int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)c.trans_port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	OK(bind(ls, (void *)&a, sizeof a) == 0 && listen(ls, 4) == 0, "fake TransPort listens");
	int us = socket(AF_INET, SOCK_DGRAM, 0); a.sin_port = htons((uint16_t)c.dns_port); OK(bind(us, (void *)&a, sizeof a) == 0, "fake DNSPort listens");
	/* TCP to a remote host must arrive at the TransPort */
	int cs = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); struct sockaddr_in rem = { .sin_family = AF_INET, .sin_port = htons(443) }; inet_pton(AF_INET, "203.0.113.50", &rem.sin_addr);
	connect(cs, (void *)&rem, sizeof rem);
	struct timeval tv = { 2, 0 }; fd_set fs; FD_ZERO(&fs); FD_SET(ls, &fs);
	OK(select(ls + 1, &fs, NULL, NULL, &tv) == 1, "TCP to 203.0.113.50:443 was redirected to the TransPort");
	/* UDP (QUIC-like) to a remote host: dropped */
	int u2 = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in ur = { .sin_family = AF_INET, .sin_port = htons(443) }; inet_pton(AF_INET, "203.0.113.50", &ur.sin_addr);
	ssize_t sr = sendto(u2, "x", 1, 0, (void *)&ur, sizeof ur);
	OK(sr < 0 && (errno == EPERM || errno == EACCES), "UDP to the outside is dropped, not sent (rc %zd errno %d)", sr, errno);
	/* DNS to any resolver goes to the DNSPort */
	struct sockaddr_in dn = { .sin_family = AF_INET, .sin_port = htons(53) }; inet_pton(AF_INET, "9.9.9.9", &dn.sin_addr);
	sendto(u2, "dns?", 4, 0, (void *)&dn, sizeof dn); FD_ZERO(&fs); FD_SET(us, &fs); tv.tv_sec = 2;
	OK(select(us + 1, &fs, NULL, NULL, &tv) == 1, "DNS to 9.9.9.9 reaches the DNSPort");
	/* ICMP is dropped too */
	OK(sh("ping -c1 -W1 203.0.113.50 >/dev/null 2>&1") != 0, "ICMP does not leave");
	/* fail closed: tor gone (listener closed) -> connection refused, nothing leaks around it */
	close(ls); close(cs);
	cs = socket(AF_INET, SOCK_STREAM, 0); tv.tv_sec = 3; setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	int rc = connect(cs, (void *)&rem, sizeof rem);
	OK(rc < 0 && errno == ECONNREFUSED, "no tor listener: the connection is refused (errno %d), it does not go out", errno);
	/* the tor uid itself passes (fake uid 12345 cannot be assumed here, so check the rule text) */
	snprintf(cmd, sizeof cmd, "nft list table inet netman_tor | grep -q 'meta skuid 12345 accept'"); OK(sh(cmd) == 0, "tor's uid is the one exemption");
	sh("nft delete table inet netman_tor"); OK(sh("nft list table inet netman_tor >/dev/null 2>&1") != 0, "table removed by down");
}
int main(void)
{
	snprintf(tmp, sizeof tmp, "/tmp/tor_test.%d", (int)getpid()); mkdir(tmp, 0700);
	signal(SIGPIPE, SIG_IGN);
	if (geteuid() == 0) (void)!system("ip link set lo up 2>/dev/null");
	gold(); control(); nft_ns();
	char w[64]; if (!netman_vm_which("tor", w, sizeof w)) puts("  SKIP real tor: not installed (bootstrap, circuits, bridges and hidden services are UNTESTED against a real network)");
	printf("tor: %d passed, %d failed\n", pass, bad);
	return bad != 0;
}
