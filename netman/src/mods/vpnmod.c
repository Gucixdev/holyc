/* vpnmod.c - registry and shared helpers of the VPN type modules (see vpnmod.h). Plain C, no libraries. */
#define _GNU_SOURCE
#include "vpnmod.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

static const netman_vpnmod *const mods[] = { &netman_openvpn_mod, &netman_ipsec_mod, &netman_l2tp_mod, &netman_openconnect_mod, &netman_sstp_mod, NULL };
const netman_vpnmod *const *netman_vpnmods(void) { return mods; }
const netman_vpnmod *netman_vpnmod_detect(const char *text) { for (int i = 0; mods[i]; i++) if (mods[i]->detect(text)) return mods[i]; return NULL; }
const netman_vpnmod *netman_vpnmod_by_type(const char *t) { for (int i = 0; t && mods[i]; i++) if (!strcmp(mods[i]->type, t)) return mods[i]; return NULL; }

/* ---------------------------------------------------------------- messages */
int netman_vm_msg(const netman_vm_ctx *cx, const char *f, ...) { va_list a; va_start(a, f); vfprintf(cx && cx->out ? cx->out : stdout, f, a); va_end(a); fputc('\n', cx && cx->out ? cx->out : stdout); return 0; }
int netman_vm_fail(const netman_vm_ctx *cx, const char *f, ...)
{
	if (cx && cx->mute) return 1;
	FILE *e = cx && cx->err ? cx->err : stderr; va_list a; va_start(a, f);
	fputs("netman vpn: ", e); vfprintf(e, f, a); fputc('\n', e); va_end(a); return 1;
}

/* ---------------------------------------------------------------- key = value profiles */
static void norm_key(const char *s, char *o, size_t n)
{
	size_t j = 0;
	for (; *s && j + 1 < n; s++) if (*s != ' ' && *s != '\t' && *s != '_' && *s != '-') o[j++] = (char)tolower((unsigned char)*s);
	o[j] = 0;
}
int netman_vm_parse(const char *text, netman_vm_prof *p, char *err, size_t en)
{
	int line = 0; p->kv = NULL; p->n = 0;
	for (const char *s = text; *s; line++) {
		const char *e = strchr(s, '\n'); size_t l = e ? (size_t)(e - s) : strlen(s);
		char buf[1100];
		if (l > 1023) { snprintf(err, en, "line %d: too long", line + 1); goto bad; }
		memcpy(buf, s, l); buf[l] = 0;
		for (size_t i = 0; i < l; i++) if ((unsigned char)buf[i] < 32 && buf[i] != '\t' && buf[i] != '\r') { snprintf(err, en, "line %d: control character", line + 1); goto bad; }
		char *t = buf; while (isspace((unsigned char)*t)) t++;
		size_t tl = strlen(t); while (tl && isspace((unsigned char)t[tl - 1])) t[--tl] = 0;
		s = e ? e + 1 : s + l;
		if (!*t || *t == '#' || *t == ';' || *t == '[') continue;
		char *eq = strchr(t, '=');
		if (!eq) { snprintf(err, en, "line %d: expected key = value", line + 1); goto bad; }
		*eq = 0; char *v = eq + 1; while (isspace((unsigned char)*v)) v++;
		size_t kl = strlen(t); while (kl && isspace((unsigned char)t[kl - 1])) t[--kl] = 0;
		char k[64]; norm_key(t, k, sizeof k);
		if (!*k) { snprintf(err, en, "line %d: empty key", line + 1); goto bad; }
		netman_vm_kv *nk = realloc(p->kv, (p->n + 1) * sizeof *nk);
		if (!nk) { snprintf(err, en, "out of memory"); goto bad; }
		p->kv = nk; p->kv[p->n].k = strdup(k); p->kv[p->n].v = strdup(v);
		if (!p->kv[p->n].k || !p->kv[p->n].v) { snprintf(err, en, "out of memory"); goto bad; }
		p->n++;
	}
	return 0;
bad:
	netman_vm_free(p); return -1;
}
const char *netman_vm_get(const netman_vm_prof *p, const char *key) { for (size_t i = 0; i < p->n; i++) if (!strcmp(p->kv[i].k, key)) return p->kv[i].v; return NULL; }
int netman_vm_next(const netman_vm_prof *p, const char *key, size_t *it, const char **val)
{
	for (; *it < p->n; (*it)++) if (!strcmp(p->kv[*it].k, key)) { *val = p->kv[(*it)++].v; return 1; }
	return 0;
}
void netman_vm_free(netman_vm_prof *p)
{
	for (size_t i = 0; i < p->n; i++) { if (p->kv[i].v) explicit_bzero(p->kv[i].v, strlen(p->kv[i].v)); free(p->kv[i].k); free(p->kv[i].v); }
	free(p->kv); p->kv = NULL; p->n = 0;
}
int netman_vm_native(const char *text, char *type, size_t n)
{
	netman_vm_prof p; char e[64]; const char *t; int r = 0;
	if (type && n) type[0] = 0;
	if (strstr(text, "ype") == NULL || netman_vm_parse(text, &p, e, sizeof e)) return 0;
	if ((t = netman_vm_get(&p, "type"))) { r = 1; if (type) { size_t i = 0; for (; t[i] && i + 1 < n; i++) type[i] = (char)tolower((unsigned char)t[i]); type[i] = 0; } }
	netman_vm_free(&p); return r;
}
int netman_vm_tok_ok(const char *s)
{
	if (!s || !*s || strlen(s) > 255) return 0;
	for (; *s; s++) if (!(isalnum((unsigned char)*s) || strchr("._-:@/+,=%~[]", *s))) return 0;
	return 1;
}
int netman_vm_cidr(const char *s, char *ip, size_t ipn, int *prefix, int *fam)
{
	char b[80]; unsigned char raw[16];
	if (!s || strlen(s) >= sizeof b) return -1;
	strcpy(b, s);
	char *sl = strchr(b, '/'); int pf = -1;
	if (sl) { *sl = 0; char *end; long v = strtol(sl + 1, &end, 10); if (*end || end == sl + 1 || v < 0 || v > 128) return -1; pf = (int)v; }
	int f = inet_pton(AF_INET, b, raw) == 1 ? AF_INET : inet_pton(AF_INET6, b, raw) == 1 ? AF_INET6 : 0;
	if (!f) return -1;
	if (pf < 0) pf = f == AF_INET ? 32 : 128;
	if (pf > (f == AF_INET ? 32 : 128)) return -1;
	if (ip) snprintf(ip, ipn, "%s", b);
	if (prefix) *prefix = pf;
	if (fam) *fam = f;
	return 0;
}
int netman_vm_split(const char *list, char **items, int max, char *buf, size_t bn)
{
	int n = 0;
	if (!list || strlen(list) >= bn) return -1;
	strcpy(buf, list);
	for (char *p = buf; *p; ) {
		while (*p == ',' || isspace((unsigned char)*p)) p++;
		if (!*p) break;
		if (n >= max) return -1;
		items[n++] = p;
		while (*p && *p != ',' && !isspace((unsigned char)*p)) p++;
		if (*p) *p++ = 0;
	}
	return n;
}
void netman_vm_hex(const char *s, char *out, size_t n)
{
	size_t o = 0; if (n < 3) { if (n) out[0] = 0; return; }
	out[o++] = '0'; out[o++] = 'x';
	for (; *s && o + 3 < n; s++) o += (size_t)snprintf(out + o, n - o, "%02x", (unsigned char)*s);
	out[o] = 0;
}

/* ---------------------------------------------------------------- files and processes */
int netman_vm_which(const char *bin, char *path, size_t n)
{
	const char *pe = getenv("PATH"); if (!pe) pe = "/usr/local/bin:/usr/bin:/bin";
	if (strchr(bin, '/')) { if (access(bin, X_OK) == 0) { snprintf(path, n, "%s", bin); return 1; } return 0; }
	char *c = strdup(pe), *sp = NULL; if (!c) return 0;
	for (char *d = strtok_r(c, ":", &sp); d; d = strtok_r(NULL, ":", &sp)) {
		char t[PATH_MAX]; struct stat st;
		snprintf(t, sizeof t, "%s/%s", *d ? d : ".", bin);
		if (stat(t, &st) == 0 && S_ISREG(st.st_mode) && access(t, X_OK) == 0) { snprintf(path, n, "%s", t); free(c); return 1; }
	}
	free(c); return 0;
}
int netman_vm_mkdirs(const char *path, unsigned mode)
{
	char b[PATH_MAX]; snprintf(b, sizeof b, "%s", path);
	for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(b, mode) && errno != EEXIST) return -1; *p = '/'; }
	return mkdir(b, mode) && errno != EEXIST ? -1 : 0;
}
int netman_vm_write(const char *path, const void *data, size_t n, unsigned mode)
{
	char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode); if (fd < 0) return -1;
	const char *p = data; size_t off = 0;
	while (off < n) { ssize_t w = write(fd, p + off, n - off); if (w < 0) { if (errno == EINTR) continue; int e = errno; close(fd); unlink(tmp); errno = e; return -1; } off += (size_t)w; }
	if (fchmod(fd, mode) || close(fd) || rename(tmp, path)) { int e = errno; unlink(tmp); errno = e; return -1; }
	return 0;
}
char *netman_vm_slurp(const char *path, size_t *len, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); if (fd < 0) return NULL;
	struct stat st; if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > cap) { close(fd); errno = EFBIG; return NULL; }
	char *b = malloc((size_t)st.st_size + 1); if (!b) { close(fd); return NULL; }
	size_t o = 0; ssize_t r;
	while (o < (size_t)st.st_size && (r = read(fd, b + o, (size_t)st.st_size - o)) != 0) { if (r < 0) { if (errno == EINTR) continue; free(b); close(fd); return NULL; } o += (size_t)r; }
	close(fd); b[o] = 0; if (len) *len = o; return b;
}
int netman_vm_secret_ok(const netman_vm_ctx *cx, const char *path)
{
	struct stat st;
	if (stat(path, &st) || !S_ISREG(st.st_mode)) return netman_vm_fail(cx, "%s: missing or not a regular file", path), 0;
	if ((st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 077)) return netman_vm_fail(cx, "%s must be owned by root (or you) with mode 0600", path), 0;
	return 1;
}
int netman_vm_path(char *buf, size_t n, const netman_vm_ctx *cx, const char *name, const char *ext) { return snprintf(buf, n, "%s/%s.%s", cx->run_dir, name, ext); }
int netman_vm_log_has(const netman_vm_ctx *cx, const char *name, const char *needle)
{
	char lf[PATH_MAX]; netman_vm_path(lf, sizeof lf, cx, name, "log");
	char *t = netman_vm_slurp(lf, NULL, 8u << 20); if (!t) return 0;
	int r = strstr(t, needle) != NULL; free(t); return r;
}
int netman_vm_pid_alive(const char *pidfile, const char *needle, int *pid)
{
	char *t = netman_vm_slurp(pidfile, NULL, 64); if (!t) return 0;
	long p = strtol(t, NULL, 10); free(t);
	if (p <= 1 || kill((pid_t)p, 0)) return 0;
	char cp[64]; snprintf(cp, sizeof cp, "/proc/%ld/cmdline", p);
	size_t l; char *cl = netman_vm_slurp(cp, &l, 1 << 16); if (!cl) return 0;
	int ok = 0;                                         /* never trust a stale pidfile: the cmdline must carry our marker */
	for (size_t i = 0; i < l; i += strlen(cl + i) + 1) if (strcmp(cl + i, needle) == 0) ok = 1;
	free(cl);
	if (ok && pid) *pid = (int)p;
	return ok;
}
static void feed(int fd, const char *s) { size_t n = s ? strlen(s) : 0, o = 0; while (o < n) { ssize_t w = write(fd, s + o, n - o); if (w < 0) { if (errno == EINTR) continue; break; } o += (size_t)w; } }
int netman_vm_run(char *const argv[], const char *input, char *out, size_t on, int timeout_ms, int *status)
{
	int ip[2], op[2]; if (out && on) out[0] = 0;
	if (pipe2(ip, O_CLOEXEC) || pipe2(op, O_CLOEXEC)) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		dup2(ip[0], 0); dup2(op[1], 1); dup2(op[1], 2);
		execvp(argv[0], argv); _exit(127);
	}
	close(ip[0]); close(op[1]);
	signal(SIGPIPE, SIG_IGN);
	feed(ip[1], input); close(ip[1]);
	size_t o = 0; long long deadline = timeout_ms > 0 ? (long long)time(NULL) * 1000 + timeout_ms : 0; int timed = 0;
	for (;;) {
		struct pollfd pf = { op[0], POLLIN, 0 };
		int w = poll(&pf, 1, 200);
		if (w > 0) {
			char tmp[4096]; ssize_t r = read(op[0], tmp, sizeof tmp);
			if (r <= 0) { if (r < 0 && errno == EINTR) continue; break; }
			if (out && o + 1 < on) { size_t c = (size_t)r < on - 1 - o ? (size_t)r : on - 1 - o; memcpy(out + o, tmp, c); o += c; out[o] = 0; }
		} else if (w < 0 && errno != EINTR) break;
		if (deadline && (long long)time(NULL) * 1000 > deadline) { kill(pid, SIGKILL); timed = 1; break; }
	}
	close(op[0]);
	int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	if (timed) return -1;
	if (WIFEXITED(st) && WEXITSTATUS(st) == 127 && !o) { if (status) *status = 127; return -1; }
	if (status) *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
	return 0;
}
int netman_vm_spawn(char *const argv[], const char *input, const char *logfile)
{
	int ip[2]; if (pipe2(ip, O_CLOEXEC)) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		int lg = open(logfile ? logfile : "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0600);
		dup2(ip[0], 0); if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); }
		execvp(argv[0], argv); _exit(127);
	}
	close(ip[0]); signal(SIGPIPE, SIG_IGN);
	feed(ip[1], input); close(ip[1]);
	int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
/* split tunnel / extra routes through an interface: `ip route add|del NET dev IF` (only this exec, argv built from checked tokens) */
int netman_vm_routes(const char *ifname, const char *list, int add)
{
	char buf[512], *it[32]; int n = netman_vm_split(list, it, 32, buf, sizeof buf), rc = 0;
	for (int i = 0; i < n; i++) {
		char ip[64], net[80]; int pf, fam;
		if (netman_vm_cidr(it[i], ip, sizeof ip, &pf, &fam)) { rc = -1; continue; }
		snprintf(net, sizeof net, "%s/%d", ip, pf);
		char *av[] = { "ip", fam == AF_INET6 ? "-6" : "-4", "route", add ? "replace" : "del", net, "dev", (char *)ifname, NULL };
		char o[256]; int st = 0;
		if (netman_vm_run(av, NULL, o, sizeof o, 5000, &st) || (st && add)) rc = -1;
	}
	return rc;
}
int netman_vm_ifname(const char *prefix, const char *name, char *out, size_t n)
{
	snprintf(out, n, "%s%s", prefix, name);
	if (strlen(out) > 15) out[15] = 0;
	return 0;
}
int netman_vm_ks_host(const char *host, char lines[][160], int max, const char *l4)
{
	struct addrinfo hints, *res, *r; int n = 0;
	memset(&hints, 0, sizeof hints); hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, NULL, &hints, &res)) return 0;
	for (r = res; r && n < max; r = r->ai_next) {
		char ip[64];
		if (r->ai_family == AF_INET) { inet_ntop(AF_INET, &((struct sockaddr_in *)r->ai_addr)->sin_addr, ip, sizeof ip); snprintf(lines[n++], 160, "ip daddr %s %s accept", ip, l4 ? l4 : ""); }
		else if (r->ai_family == AF_INET6) { inet_ntop(AF_INET6, &((struct sockaddr_in6 *)r->ai_addr)->sin6_addr, ip, sizeof ip); snprintf(lines[n++], 160, "ip6 daddr %s %s accept", ip, l4 ? l4 : ""); }
	}
	freeaddrinfo(res);
	for (int i = 0; i < n; i++) { char *d = strstr(lines[i], "  "); while (d) { memmove(d, d + 1, strlen(d)); d = strstr(lines[i], "  "); } }
	return n;
}
int netman_vm_defroute_host(const char *ip, char *undo, size_t un)
{
	char o[512], net[96]; int st = 0, v6 = strchr(ip, ':') != NULL;
	char *g[] = { "ip", v6 ? "-6" : "-4", "route", "get", (char *)ip, NULL };
	undo[0] = 0;
	if (netman_vm_run(g, NULL, o, sizeof o, 5000, &st) || st) return -1;
	char via[64] = "", dev[32] = "";
	char *p = strstr(o, " via "); if (p) sscanf(p + 5, "%63s", via);
	p = strstr(o, " dev "); if (p) sscanf(p + 5, "%31s", dev);
	if (!dev[0]) return -1;
	snprintf(net, sizeof net, "%s/%d", ip, v6 ? 128 : 32);
	char *a1[] = { "ip", v6 ? "-6" : "-4", "route", "replace", net, via[0] ? "via" : "dev", via[0] ? via : dev, via[0] ? "dev" : NULL, via[0] ? dev : NULL, NULL };
	if (netman_vm_run(a1, NULL, o, sizeof o, 5000, &st) || st) return -1;
	snprintf(undo, un, "%s %s", v6 ? "-6" : "-4", net);
	return 0;
}
