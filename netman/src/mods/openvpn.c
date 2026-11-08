/* openvpn.c - OpenVPN module for netman (see openvpn.h). Moved from vpn.h; same behaviour. */
#define _GNU_SOURCE
#include "openvpn.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

static char *trim(char *s) { while (isspace((unsigned char)*s)) s++; char *e = s + strlen(s); while (e > s && isspace((unsigned char)e[-1])) *--e = 0; return s; }

int netman_openvpn_detect(const char *t)
{
	if (netman_vm_native(t, NULL, 0)) return 0;
	for (const char *p = t; *p; ) {
		const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
		while (l && isspace((unsigned char)*p)) { p++; l--; }
		if ((l >= 6 && strncmp(p, "remote", 6) == 0 && (l == 6 || isspace((unsigned char)p[6]))) || (l >= 6 && strncmp(p, "client", 6) == 0) ||
		    (l >= 3 && strncmp(p, "dev", 3) == 0 && l > 3 && isspace((unsigned char)p[3])) || (l >= 5 && strncmp(p, "proto", 5) == 0)) return 1;
		if (!e) break;
		p = e + 1;
	}
	return 0;
}
static int detect(const char *t) { return netman_openvpn_detect(t); }
static int validate(const char *t, char *err, size_t en)
{
	int remote = 0;
	for (const char *p = t; *p; ) {
		const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l >= 7 && !strncmp(p, "remote ", 7)) remote = 1;
		if (!e) break;
		p = e + 1;
	}
	if (!remote) { snprintf(err, en, "no remote line"); return -1; }
	return 0;
}

static int pidfile(const netman_vm_ctx *cx, const char *name, char *pf, size_t n) { return netman_vm_path(pf, n, cx, name, "pid"); }
static int running(const netman_vm_ctx *cx, const char *name, int *pid) { char pf[PATH_MAX]; pidfile(cx, name, pf, sizeof pf); return netman_vm_pid_alive(pf, pf, pid); }
static const char *state(const netman_vm_ctx *cx, const char *name)
{
	if (!running(cx, name, NULL)) return "down";
	if (netman_vm_log_has(cx, name, "AUTH_FAILED")) return "auth failed";
	return netman_vm_log_has(cx, name, "Initialization Sequence Completed") ? "connected" : "connecting";
}

int netman_openvpn_args(const char *name, const char *dir, const char *conf, const char *text, const netman_vm_ctx *cx, char *av[], int max, char *auth, size_t an, int *bare_out)
{
	int bare = 0; auth[0] = 0;
	for (const char *p = text; *p; ) {
		const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
		char line[1024]; snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), p);
		char *s = trim(line);
		if (strncmp(s, "auth-user-pass", 14) == 0 && (s[14] == 0 || isspace((unsigned char)s[14]))) {
			char *a = trim(s + 14);
			if (*a) { if (a[0] == '/') snprintf(auth, an, "%s", a); else snprintf(auth, an, "%s/%s", cx->conf_dir, a); }
			else bare = 1;
		}
		if (!e) break;
		p = e + 1;
	}
	if (bare) snprintf(auth, an, "%s/%s.auth", cx->conf_dir, name);
	static char ident[80], pf[PATH_MAX], lf[PATH_MAX];
	pidfile(cx, name, pf, sizeof pf); netman_vm_path(lf, sizeof lf, cx, name, "log");
	snprintf(ident, sizeof ident, "nmbovpn-%s", name);
	int n = 0;
	av[n++] = "openvpn"; av[n++] = "--cd"; av[n++] = (char *)dir; av[n++] = "--config"; av[n++] = (char *)conf;
	av[n++] = "--daemon"; av[n++] = ident; av[n++] = "--writepid"; av[n++] = pf; av[n++] = "--log"; av[n++] = lf; av[n++] = "--auth-nocache";
	if (bare) { av[n++] = "--auth-user-pass"; av[n++] = auth; }     /* a path, never the secret itself */
	av[n] = NULL; (void)max;
	if (bare_out) *bare_out = bare;
	return n;
}

static int up(const netman_vm_ctx *cx, const char *name, const char *conf, const char *text, int wait_sec)
{
	if (running(cx, name, NULL)) return netman_vm_fail(cx, "%s is already running", name);
	if (netman_vm_mkdirs(cx->run_dir, 0700)) return netman_vm_fail(cx, "%s: %s%s", cx->run_dir, strerror(errno), errno == EACCES || errno == EPERM || errno == EROFS ? " (vpn up/down needs root)" : "");
	char auth[PATH_MAX], abs[PATH_MAX], dir[PATH_MAX], *av[24]; int bare;
	netman_openvpn_args(name, "", "", text, cx, av, 24, auth, sizeof auth, &bare);
	if (auth[0] && !netman_vm_secret_ok(cx, auth)) return 1;
	if (!realpath(conf, abs) || !realpath(cx->conf_dir, dir)) return netman_vm_fail(cx, "cannot resolve %s", conf);
	netman_openvpn_args(name, dir, abs, text, cx, av, 24, auth, sizeof auth, &bare);
	int rc = netman_vm_spawn(av, NULL, NULL);
	if (rc != 0) return netman_vm_fail(cx, "openvpn failed to start (exit %d); see `netman vpn logs %s`", rc, name);
	for (int i = 0; i < wait_sec * 5; i++) {
		const char *s = state(cx, name); int pid;
		if (!strcmp(s, "connected")) { netman_vm_msg(cx, "%s: connected", name); return 0; }
		if (!strcmp(s, "down")) { if (i < 15) { usleep(200000); continue; }      /* --daemon writes the pidfile after forking */
			return netman_vm_fail(cx, "%s: openvpn exited; see `netman vpn logs %s`", name, name); }
		if (!strcmp(s, "auth failed")) { if (running(cx, name, &pid)) kill(pid, SIGTERM); return netman_vm_fail(cx, "%s: authentication failed", name); }
		usleep(200000);
	}
	netman_vm_msg(cx, "%s: %s", name, state(cx, name));
	return 0;
}
static int down(const netman_vm_ctx *cx, const char *name, const char *text)
{
	int pid; char pf[PATH_MAX]; (void)text; pidfile(cx, name, pf, sizeof pf);
	if (!running(cx, name, &pid)) { unlink(pf); return netman_vm_fail(cx, "%s is not running", name); }
	kill(pid, SIGTERM);
	for (int i = 0; i < 40 && kill(pid, 0) == 0; i++) usleep(200000);
	if (kill(pid, 0) == 0) { kill(pid, SIGKILL); usleep(200000); }
	unlink(pf);
	netman_vm_msg(cx, "%s: down", name);
	return 0;
}

static int ks(const char *text, char eps[][160], int max, char *tun, size_t tunn, const char *name)
{
	int n = 0; char proto[8] = "udp"; (void)name;
	snprintf(tun, tunn, "tun*");
	for (const char *p = text; *p; ) {
		const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
		char line[512], h[256], pt[16] = ""; snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), p);
		char *s = trim(line);
		if (strncmp(s, "proto ", 6) == 0) snprintf(proto, sizeof proto, "%s", strncmp(trim(s + 6), "tcp", 3) == 0 ? "tcp" : "udp");
		if (strncmp(s, "remote ", 7) == 0 && n < max && sscanf(s + 7, "%255s %15s", h, pt) >= 1) {
			struct addrinfo hints = {0}, *res; hints.ai_socktype = SOCK_DGRAM;
			if (getaddrinfo(h, isdigit((unsigned char)pt[0]) ? pt : "1194", &hints, &res) == 0) {
				for (struct addrinfo *r = res; r && n < max; r = r->ai_next) {
					char ip[64]; int port;
					if (r->ai_family == AF_INET) { inet_ntop(AF_INET, &((struct sockaddr_in *)r->ai_addr)->sin_addr, ip, sizeof ip); port = ntohs(((struct sockaddr_in *)r->ai_addr)->sin_port); snprintf(eps[n++], 160, "ip daddr %s %s dport %d accept", ip, proto, port); }
					else if (r->ai_family == AF_INET6) { inet_ntop(AF_INET6, &((struct sockaddr_in6 *)r->ai_addr)->sin6_addr, ip, sizeof ip); port = ntohs(((struct sockaddr_in6 *)r->ai_addr)->sin6_port); snprintf(eps[n++], 160, "ip6 daddr %s %s dport %d accept", ip, proto, port); }
				}
				freeaddrinfo(res);
			}
		}
		if (!e) break;
		p = e + 1;
	}
	return n;
}

static int info(const netman_vm_ctx *cx, const char *name, const char *t)
{
	int pid;
	netman_vm_msg(cx, "type         openvpn");
	netman_vm_msg(cx, "state        %s", state(cx, name));
	for (const char *p = t; *p; ) {
		const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
		char line[512]; snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), p);
		char *s = trim(line);
		if (!strncmp(s, "remote ", 7) || !strncmp(s, "proto ", 6) || !strncmp(s, "dev ", 4) || !strncmp(s, "redirect-gateway", 16) || !strncmp(s, "route ", 6))
			netman_vm_msg(cx, "%-12s %s", strncmp(s, "remote", 6) == 0 ? "remote" : strncmp(s, "proto", 5) == 0 ? "proto" : strncmp(s, "dev", 3) == 0 ? "device" : "route", strchr(s, ' ') ? trim(strchr(s, ' ')) : "");
		if (!e) break;
		p = e + 1;
	}
	if (running(cx, name, &pid)) netman_vm_msg(cx, "pid          %d", pid);
	return 0;
}

const netman_vpnmod netman_openvpn_mod = { "openvpn", ".ovpn", "openvpn", "vpn.openvpn", detect, validate, NULL, up, down, state, ks, info };
