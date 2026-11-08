/* ssh.c - see ssh.h. */
#include "modkit.h"
#include "net.h"
#include "ssh.h"
#include <poll.h>

void netman_ssh_cfg_defaults(netman_ssh_cfg *c)
{
	memset(c, 0, sizeof *c);
	mk_cpy(c->dir, sizeof c->dir, "/etc/netman/ssh");
	mk_cpy(c->state, sizeof c->state, "/run/netman/ssh");
	mk_cpy(c->known, sizeof c->known, "/etc/netman/ssh/known_hosts");
	mk_cpy(c->bin, sizeof c->bin, "ssh");
	mk_cpy(c->keyscan, sizeof c->keyscan, "ssh-keyscan");
	mk_cpy(c->keygen, sizeof c->keygen, "ssh-keygen");
}

void netman_ssh_init(netman_ssh *p)
{
	memset(p, 0, sizeof *p);
	p->port = 22; p->keepalive = 30; p->keepalive_count = 3; p->connect_timeout = 15; p->tun_local = p->tun_remote = -1;
	p->reconnect = 1; p->reconnect_delay = 5;
	mk_cpy(p->strict, sizeof p->strict, "yes");
}

static int digits(const char *s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }
static int port_ok(const char *s) { int x; return digits(s) && !mk_int(s, 1, 65535, &x); }

/* L:LPORT:HOST:PORT  R:RPORT:HOST:PORT  D:PORT ; the spec after the kind goes to ssh as one argument */
static int fwd_ok(const char *f, char *why, size_t wn)
{
	char t[96], *a[5], *sv = NULL, *x;
	int n = 0;
	snprintf(t, sizeof t, "%s", f);
	if ((f[0] != 'L' && f[0] != 'R' && f[0] != 'D') || f[1] != ':') { snprintf(why, wn, "forward '%s' must start with L: R: or D:", f); return 0; }
	for (x = strtok_r(t + 2, ":", &sv); x && n < 5; x = strtok_r(NULL, ":", &sv)) a[n++] = x;
	if (f[0] == 'D') { if (n != 1 || !port_ok(a[0])) { snprintf(why, wn, "forward '%s': want D:PORT", f); return 0; } return 1; }
	if (n != 3 || !port_ok(a[0]) || !mk_token_ok(a[1], 80) || !port_ok(a[2])) { snprintf(why, wn, "forward '%s': want %c:PORT:HOST:PORT", f, f[0]); return 0; }
	return 1;
}

static int set(netman_ssh *p, const char *k, const char *v, char *err, size_t en)
{
	int x;
#define BADV(...) do { snprintf(err, en, __VA_ARGS__); return -1; } while (0)
	if (!strcmp(k, "host")) { if (!mk_token_ok(v, sizeof p->host) || strchr(v, '@') || strchr(v, ',')) BADV("bad host '%s'", v); mk_cpy(p->host, sizeof p->host, v); }
	else if (!strcmp(k, "user")) { if (!mk_token_ok(v, sizeof p->user) || strchr(v, '@')) BADV("bad user '%s'", v); mk_cpy(p->user, sizeof p->user, v); }
	else if (!strcmp(k, "key")) { if (v[0] != '/' || !mk_token_ok(v, sizeof p->key)) BADV("key must be an absolute path without spaces"); mk_cpy(p->key, sizeof p->key, v); }
	else if (!strcmp(k, "known_hosts")) { if (v[0] != '/' || !mk_token_ok(v, sizeof p->known_hosts)) BADV("known_hosts must be an absolute path without spaces"); mk_cpy(p->known_hosts, sizeof p->known_hosts, v); }
	else if (!strcmp(k, "jump")) { if (!mk_token_ok(v, sizeof p->jump) || strstr(v, ",,")) BADV("bad jump list '%s'", v); mk_cpy(p->jump, sizeof p->jump, v); }
	else if (!strcmp(k, "port")) { if (mk_int(v, 1, 65535, &p->port)) BADV("bad port '%s'", v); }
	else if (!strcmp(k, "keepalive")) { if (mk_int(v, 0, 3600, &p->keepalive)) BADV("keepalive is 0..3600 seconds"); }
	else if (!strcmp(k, "keepalive_count")) { if (mk_int(v, 1, 100, &p->keepalive_count)) BADV("keepalive_count is 1..100"); }
	else if (!strcmp(k, "connect_timeout")) { if (mk_int(v, 1, 600, &p->connect_timeout)) BADV("connect_timeout is 1..600 seconds"); }
	else if (!strcmp(k, "reconnect_delay")) { if (mk_int(v, 0, 3600, &p->reconnect_delay)) BADV("reconnect_delay is 0..3600 seconds"); }
	else if (!strcmp(k, "control") || !strcmp(k, "reconnect") || !strcmp(k, "auto")) {
		if (!mk_bool_valid(v)) BADV("%s is yes or no", k);
		x = mk_bool(v);
		if (!strcmp(k, "control")) p->control = x; else if (!strcmp(k, "reconnect")) p->reconnect = x; else p->autostart = x;
	}
	else if (!strcmp(k, "strict")) {
		if (strcmp(v, "yes") && strcmp(v, "accept-new")) BADV("strict must be 'yes' or 'accept-new': netman never turns host key checking off ('%s' refused)", v);
		mk_cpy(p->strict, sizeof p->strict, v);
	}
	else if (!strcmp(k, "tun")) {
		const char *c = strchr(v, ':');
		char l[8];
		if (!c || c == v || c - v > 3) BADV("tun is LOCAL:REMOTE device numbers, e.g. 0:0");
		snprintf(l, sizeof l, "%.*s", (int)(c - v), v);
		if (!digits(l) || !digits(c + 1) || atoi(l) > 255 || atoi(c + 1) > 255) BADV("tun is LOCAL:REMOTE device numbers, e.g. 0:0");
		p->tun_local = atoi(l); p->tun_remote = atoi(c + 1);
	}
	else if (!strcmp(k, "tun_addr")) { if (!strchr(v, '/') || !mk_token_ok(v, sizeof p->tun_addr)) BADV("tun_addr is ADDRESS/PREFIX"); mk_cpy(p->tun_addr, sizeof p->tun_addr, v); }
	else if (!strcmp(k, "forward")) {
		char why[160];
		if (!fwd_ok(v, why, sizeof why)) BADV("%s", why);
		if (p->nfwd >= NETMAN_SSH_FWD) BADV("at most %d forwards per profile", NETMAN_SSH_FWD);
		mk_cpy(p->fwd[p->nfwd++], sizeof p->fwd[0], v);
	}
	else if (!strcmp(k, "hostkey")) { /* informational: what the operator checked */ }
	else BADV("unknown key '%s'", k);
	return 0;
}

int netman_ssh_parse(const char *text, netman_ssh *p, char *err, size_t en)
{
	char *c = strdup(text), *sv = NULL, *ln;
	netman_ssh_init(p);
	if (!c) return -1;
	for (ln = strtok_r(c, "\n", &sv); ln; ln = strtok_r(NULL, "\n", &sv)) {
		char *q = ent_trim(ln), *eq;
		if (!*q || *q == '#') continue;
		if (!(eq = strchr(q, '='))) { snprintf(err, en, "not key = value: '%s'", q); free(c); return -1; }
		*eq = 0;
		if (set(p, ent_trim(q), ent_trim(eq + 1), err, en)) { free(c); return -1; }
	}
	free(c);
	return 0;
}

int netman_ssh_format(const netman_ssh *p, char *b, size_t n)
{
	int l = snprintf(b, n, "host = %s\nport = %d\n", p->host, p->port);
#define A(...) l += snprintf(b + l, l < (int)n ? n - l : 0, __VA_ARGS__)
	if (p->user[0]) A("user = %s\n", p->user);
	if (p->key[0]) A("key = %s\n", p->key);
	if (p->jump[0]) A("jump = %s\n", p->jump);
	if (p->known_hosts[0]) A("known_hosts = %s\n", p->known_hosts);
	for (int i = 0; i < p->nfwd; i++) A("forward = %s\n", p->fwd[i]);
	A("keepalive = %d\nkeepalive_count = %d\nconnect_timeout = %d\ncontrol = %s\n", p->keepalive, p->keepalive_count, p->connect_timeout, p->control ? "yes" : "no");
	if (p->tun_local >= 0) A("tun = %d:%d\n", p->tun_local, p->tun_remote);
	if (p->tun_addr[0]) A("tun_addr = %s\n", p->tun_addr);
	A("strict = %s\nreconnect = %s\nreconnect_delay = %d\nauto = %s\n", p->strict, p->reconnect ? "yes" : "no", p->reconnect_delay, p->autostart ? "yes" : "no");
	return l;
}

int netman_ssh_check(const netman_ssh *p, char *err, size_t en)
{
	if (!p->host[0]) { snprintf(err, en, "host is required"); return -1; }
	if (!mk_name_ok(p->name)) { snprintf(err, en, "bad profile name '%s'", p->name); return -1; }
	if (!p->nfwd && p->tun_local < 0) { /* a plain shell-less session is allowed: it keeps a control master or a jump alive */ }
	if (p->tun_addr[0] && p->tun_local < 0) { snprintf(err, en, "tun_addr needs tun = LOCAL:REMOTE"); return -1; }
	return 0;
}

/* ---- argv ---- */
static const char *known_for(const netman_ssh_cfg *c, const netman_ssh *p) { return p->known_hosts[0] ? p->known_hosts : c->known; }
static void ctl_path(const netman_ssh_cfg *c, const netman_ssh *p, char *o, size_t n) { mk_path(o, n, c->state, p->name, "ctl"); }

int netman_ssh_argv(const netman_ssh_cfg *c, const netman_ssh *p, char **argv, int max, char *buf, size_t bn)
{
	int k = 0;
	size_t off = 0;
	char *tmp;
#define ADD(...) do { if (k >= max - 1) return -1; tmp = buf + off; int w_ = snprintf(tmp, bn - off, __VA_ARGS__); if (w_ < 0 || off + (size_t)w_ + 1 >= bn) return -1; off += (size_t)w_ + 1; argv[k++] = tmp; } while (0)
	ADD("%s", c->bin);
	ADD("-N"); ADD("-T");
	ADD("-o"); ADD("BatchMode=yes");
	ADD("-o"); ADD("ExitOnForwardFailure=yes");
	ADD("-o"); ADD("StrictHostKeyChecking=%s", p->strict);
	ADD("-o"); ADD("UserKnownHostsFile=%s", known_for(c, p));
	ADD("-o"); ADD("VerifyHostKeyDNS=no");
	ADD("-o"); ADD("ServerAliveInterval=%d", p->keepalive);
	ADD("-o"); ADD("ServerAliveCountMax=%d", p->keepalive_count);
	ADD("-o"); ADD("ConnectTimeout=%d", p->connect_timeout);
	if (p->key[0]) { ADD("-o"); ADD("IdentitiesOnly=yes"); ADD("-i"); ADD("%s", p->key); }
	if (p->port != 22) { ADD("-p"); ADD("%d", p->port); }
	if (p->user[0]) { ADD("-l"); ADD("%s", p->user); }
	if (p->jump[0]) { ADD("-J"); ADD("%s", p->jump); }
	if (p->control) {
		char s[300];
		ctl_path(c, p, s, sizeof s);
		if (strlen(s) > 100) return -1;   /* unix socket path limit */
		ADD("-o"); ADD("ControlMaster=yes"); ADD("-o"); ADD("ControlPath=%s", s);
	} else { ADD("-o"); ADD("ControlMaster=no"); ADD("-o"); ADD("ControlPath=none"); }
	for (int i = 0; i < p->nfwd; i++) {
		const char *f = p->fwd[i];
		ADD("-%c", f[0]);
		if (f[0] == 'D') ADD("127.0.0.1:%s", f + 2); else if (f[0] == 'L') ADD("127.0.0.1:%s", f + 2); else ADD("%s", f + 2);
	}
	if (p->tun_local >= 0) { ADD("-w"); ADD("%d:%d", p->tun_local, p->tun_remote); }
	ADD("--"); ADD("%s", p->host);
	argv[k] = NULL;
	return k;
}

/* ---- state files ---- */
typedef struct { long pid, ssh_pid, since; int restarts, last_exit; char state[16]; } sstate;
static void sp(const netman_ssh_cfg *c, const char *name, const char *ext, char *o, size_t n) { mk_path(o, n, c->state, name, ext); }
static void state_write(const netman_ssh_cfg *c, const char *name, const sstate *s)
{
	char p[400], b[300];
	int l = snprintf(b, sizeof b, "pid=%ld\nssh_pid=%ld\nsince=%ld\nrestarts=%d\nlast_exit=%d\nstate=%s\n", s->pid, s->ssh_pid, s->since, s->restarts, s->last_exit, s->state);
	sp(c, name, "state", p, sizeof p);
	ent_write_atomic(p, b, (size_t)l, 0644);
}
static int state_read(const netman_ssh_cfg *c, const char *name, sstate *s)
{
	char p[400], b[400], *ln, *sv = NULL;
	memset(s, 0, sizeof *s);
	sp(c, name, "state", p, sizeof p);
	if (ent_slurp(p, b, sizeof b) < 0) return -1;
	for (ln = strtok_r(b, "\n", &sv); ln; ln = strtok_r(NULL, "\n", &sv)) {
		char *v = strchr(ln, '=');
		if (!v) continue;
		*v++ = 0;
		if (!strcmp(ln, "pid")) s->pid = atol(v); else if (!strcmp(ln, "ssh_pid")) s->ssh_pid = atol(v); else if (!strcmp(ln, "since")) s->since = atol(v);
		else if (!strcmp(ln, "restarts")) s->restarts = atoi(v); else if (!strcmp(ln, "last_exit")) s->last_exit = atoi(v); else if (!strcmp(ln, "state")) mk_cpy(s->state, sizeof s->state, v);
	}
	return 0;
}

/* ---- supervisor ---- */
static volatile sig_atomic_t g_stop;
static volatile pid_t g_child;
static void on_term(int s) { (void)s; g_stop = 1; if (g_child > 0) kill(g_child, SIGTERM); }
static void nap_ms(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; nanosleep(&t, NULL); }

/* once ssh -w has created tunN: address and link up. 1 when done */
static int tun_try(const netman_ssh *p)
{
	char ifn[16];
	snprintf(ifn, sizeof ifn, "tun%d", p->tun_local);
	if (!if_nametoindex(ifn)) return 0;
	if (p->tun_addr[0]) netman_addr_add(ifn, p->tun_addr);
	netman_link_set(ifn, 1);
	return 1;
}

/* runs ssh, restarts it as configured; returns when told to stop or (reconnect = no) when ssh ended. Never returns until then. */
static int supervise(const netman_ssh_cfg *c, const netman_ssh *p, char **argv)
{
	sstate s;
	struct sigaction sa;
	char logp[400], pidp[400], ctlp[400];
	int delay_ms = p->reconnect_delay ? p->reconnect_delay * 1000 : 100;
	memset(&s, 0, sizeof s);
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); sigaction(SIGHUP, &sa, NULL);
	sp(c, p->name, "log", logp, sizeof logp); sp(c, p->name, "pid", pidp, sizeof pidp); ctl_path(c, p, ctlp, sizeof ctlp);
	s.pid = getpid();
	mk_pid_write(pidp, s.pid);
	while (!g_stop) {
		pid_t pid;
		int st = 0, tun_done = 0;
		time_t t0 = time(NULL);
		unlink(ctlp);   /* a stale control socket makes ControlMaster=yes fail */
		if ((pid = fork()) < 0) break;
		if (!pid) {
			int fd = open(logp, O_WRONLY | O_CREAT | O_APPEND, 0600), nul = open("/dev/null", O_RDONLY);
			signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
			if (nul >= 0) dup2(nul, 0);
			if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
			execvp(argv[0], argv);
			dprintf(2, "netman: cannot run %s: %s\n", argv[0], strerror(errno));
			_exit(127);
		}
		g_child = pid;
		if (g_stop) kill(pid, SIGTERM);
		s.ssh_pid = pid; s.since = (long)t0; mk_cpy(s.state, sizeof s.state, "running");
		state_write(c, p->name, &s);
		for (;;) {
			pid_t w = waitpid(pid, &st, WNOHANG);
			if (w == pid) break;
			if (w < 0 && errno != EINTR) { st = 0; break; }
			if (p->tun_local >= 0 && !tun_done) tun_done = tun_try(p);
			nap_ms(100);
		}
		g_child = 0;
		s.last_exit = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
		s.ssh_pid = 0;
		if (g_stop || !p->reconnect) break;
		if (p->reconnect_delay && time(NULL) - t0 < 10) delay_ms = delay_ms * 2 > 60000 ? 60000 : delay_ms * 2; else delay_ms = p->reconnect_delay ? p->reconnect_delay * 1000 : 100;
		s.restarts++;
		mk_cpy(s.state, sizeof s.state, "reconnecting");
		state_write(c, p->name, &s);
		for (int waited = 0; waited < delay_ms && !g_stop; waited += 100) nap_ms(100);
	}
	mk_cpy(s.state, sizeof s.state, "stopped");
	state_write(c, p->name, &s);
	unlink(pidp); unlink(ctlp);
	return 0;
}

static int load(const netman_ssh_cfg *c, const char *name, netman_ssh *p, char *err, size_t en)
{
	char path[400], txt[4096];
	if (!mk_name_ok(name)) { snprintf(err, en, "bad profile name '%s'", name); return -1; }
	mk_path(path, sizeof path, c->dir, name, "ssh");
	if (ent_slurp(path, txt, sizeof txt) < 0) { snprintf(err, en, "no ssh profile '%s' in %s (netman ssh save %s host=...)", name, c->dir, name); return -1; }
	if (netman_ssh_parse(txt, p, err, en) < 0) return -1;
	snprintf(p->name, sizeof p->name, "%s", name);
	return netman_ssh_check(p, err, en);
}

static int running(const netman_ssh_cfg *c, const char *name, long *pid)
{
	char p[400];
	long x;
	sp(c, name, "pid", p, sizeof p);
	x = mk_pid_read(p);
	if (pid) *pid = x;
	return mk_pid_alive(x);
}

static int do_run(const netman_ssh_cfg *c, const netman_ssh *p, FILE *err)
{
	char *argv[96], buf[4096];
	if (netman_ssh_argv(c, p, argv, 96, buf, sizeof buf) < 0) { fprintf(err, "ssh %s: cannot build the command line (state path too long?)\n", p->name); return 1; }
	if (!mk_have(c->bin)) { fprintf(err, "ssh %s: '%s' not found (ssh.ssh_bin)\n", p->name, c->bin); return 1; }
	if (mk_mkdirs(c->state) < 0) { fprintf(err, "ssh: cannot create %s\n", c->state); return 1; }
	return supervise(c, p, argv);
}

static int do_up(const netman_ssh_cfg *c, const netman_ssh *p, FILE *out, FILE *err)
{
	pid_t f;
	long pid;
	if (running(c, p->name, &pid)) { fprintf(out, "%s already running (pid %ld)\n", p->name, pid); return 0; }
	if (!mk_have(c->bin)) { fprintf(err, "ssh %s: '%s' not found (ssh.ssh_bin)\n", p->name, c->bin); return 1; }
	if (!mk_exists(known_for(c, p)) && !strcmp(p->strict, "yes")) fprintf(err, "note: %s does not exist yet; with strict = yes the connection will fail until you `netman ssh trust %s FINGERPRINT`\n", known_for(c, p), p->name);
	if (mk_mkdirs(c->state) < 0) { fprintf(err, "ssh: cannot create %s\n", c->state); return 1; }
	fflush(NULL);
	if ((f = fork()) < 0) { fprintf(err, "ssh: fork: %s\n", strerror(errno)); return 1; }
	if (!f) {
		int nul = open("/dev/null", O_RDWR);
		setsid();
		if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
		if (chdir("/") < 0) {}
		_exit(do_run(c, p, stderr));
	}
	for (int i = 0; i < 50; i++) {   /* up to 5 s for the supervisor to start ssh */
		sstate s;
		if (state_read(c, p->name, &s) == 0 && s.pid == f && !strcmp(s.state, "running")) {
			if (p->control) {   /* wait for the master socket: that is "connected" */
				char cp[400];
				ctl_path(c, p, cp, sizeof cp);
				for (int j = 0; j < p->connect_timeout * 10 && !mk_exists(cp); j++) { nap_ms(100); if (!running(c, p->name, NULL)) break; }
				if (!mk_exists(cp)) { fprintf(out, "%s started (pid %ld) but is not connected yet; see %s/%s.log\n", p->name, (long)f, c->state, p->name); return 0; }
			}
			fprintf(out, "%s started (pid %ld)\n", p->name, (long)f);
			return 0;
		}
		nap_ms(100);
	}
	fprintf(err, "ssh %s: supervisor did not start\n", p->name);
	return 1;
}

static int do_down(const netman_ssh_cfg *c, const char *name, FILE *out, FILE *err)
{
	long pid;
	char p[400];
	if (!running(c, name, &pid)) { fprintf(out, "%s is not running\n", name); return 0; }
	kill((pid_t)pid, SIGTERM);
	for (int i = 0; i < 80 && mk_pid_alive(pid); i++) { int st; waitpid((pid_t)pid, &st, WNOHANG); nap_ms(100); }
	if (mk_pid_alive(pid)) { sstate s; kill((pid_t)pid, SIGKILL); if (state_read(c, name, &s) == 0 && s.ssh_pid > 1) kill((pid_t)s.ssh_pid, SIGKILL); fprintf(err, "ssh %s: did not stop in 8 s, killed\n", name); }
	sp(c, name, "pid", p, sizeof p); unlink(p);
	sp(c, name, "ctl", p, sizeof p); unlink(p);
	fprintf(out, "%s stopped\n", name);
	return 0;
}

int netman_ssh_status(const netman_ssh_cfg *c, FILE *out)
{
	char names[64][32], err[200];
	int n = mk_names(c->dir, "ssh", names, 64);
	for (int i = 0; i < n; i++) {
		netman_ssh p;
		sstate s;
		const char *st;
		if (load(c, names[i], &p, err, sizeof err) < 0) { fprintf(out, "%s invalid  %s\n", names[i], err); continue; }
		int alive = running(c, p.name, NULL), have = state_read(c, p.name, &s) == 0;
		st = !alive ? "down" : have && !strcmp(s.state, "running") ? "up" : "reconnecting";
		fprintf(out, "%s %s %s%s%s:%d", p.name, st, p.user, p.user[0] ? "@" : "", p.host, p.port);
		if (p.jump[0]) fprintf(out, "  via %s", p.jump);
		if (p.nfwd) fprintf(out, "  %d forward%s", p.nfwd, p.nfwd == 1 ? "" : "s");
		if (p.tun_local >= 0) fprintf(out, "  tun%d", p.tun_local);
		if (alive && have) {
			fprintf(out, "  restarts %d", s.restarts);
			if (!strcmp(st, "up")) { long up = (long)time(NULL) - s.since; fprintf(out, "  up %ldm", up / 60); }
			else fprintf(out, "  last exit %d", s.last_exit);
		}
		if (!alive && p.autostart) fputs("  (auto)", out);
		fputc('\n', out);
	}
	return n;
}

/* ---- host keys ---- */
static int scan(const netman_ssh_cfg *c, const char *host, int port, char *out, size_t n)
{
	char ps[8], *a[] = { (char *)c->keyscan, "-T", "8", "-p", ps, "--", (char *)host, NULL };
	int r;
	snprintf(ps, sizeof ps, "%d", port);
	/* keyscan prints '#' comment lines to stderr and the keys to stdout; ent_exec merges them: drop comments below */
	r = mk_run(a, out, n);
	if (r == -ENOENT) snprintf(out, n, "'%s' not found (ssh.keyscan_bin)", c->keyscan);
	return r;
}
/* fingerprint lines "256 SHA256:... host (ED25519)" for the key lines of KEYS, one per key line, in order; returns count */
static int fingerprints(const netman_ssh_cfg *c, const char *keys, char fp[][200], int max)
{
	char *a[] = { (char *)c->keygen, "-l", "-f", "-", NULL }, out[4096], *ln, *sv = NULL;
	int n = 0;
	if (ent_exec(a, keys, out, sizeof out) < 0) return 0;
	for (ln = strtok_r(out, "\n", &sv); ln && n < max; ln = strtok_r(NULL, "\n", &sv)) snprintf(fp[n++], 200, "%s", ln);
	return n;
}
static int keylines(const char *raw, char lines[][512], int max)
{
	char *t = strdup(raw), *ln, *sv = NULL;
	int n = 0;
	if (!t) return 0;
	for (ln = strtok_r(t, "\n", &sv); ln && n < max; ln = strtok_r(NULL, "\n", &sv))
		if (*ln && *ln != '#' && strlen(ln) < 512 && (strstr(ln, " ssh-") || strstr(ln, " ecdsa-") || strstr(ln, " sk-"))) snprintf(lines[n++], 512, "%s", ln);
	free(t);
	return n;
}

static void hosts_of(const netman_ssh *p, char hosts[][130], int ports[], int *n)
{
	char j[256], *t, *sv = NULL;
	*n = 0;
	snprintf(hosts[0], 130, "%s", p->host); ports[0] = p->port; *n = 1;
	snprintf(j, sizeof j, "%s", p->jump);
	for (t = strtok_r(j, ",", &sv); t && *n < 8; t = strtok_r(NULL, ",", &sv)) {
		char *at = strchr(t, '@'), *co;
		int port = 22;
		if (at) t = at + 1;
		if ((co = strrchr(t, ':'))) { *co = 0; port = atoi(co + 1); }
		snprintf(hosts[*n], 130, "%s", t); ports[*n] = port; (*n)++;
	}
}

static int cmd_hostkey(const netman_ssh_cfg *c, const netman_ssh *p, FILE *out, FILE *err)
{
	char hosts[8][130]; int ports[8], n, bad = 0;
	hosts_of(p, hosts, ports, &n);
	for (int i = 0; i < n; i++) {
		char raw[4096], lines[8][512], fp[8][200];
		int k, f;
		if (scan(c, hosts[i], ports[i], raw, sizeof raw) == -ENOENT) { fprintf(err, "%s: %s\n", hosts[i], raw); bad++; continue; }
		k = keylines(raw, lines, 8);
		if (!k) { fprintf(err, "%s: no host keys returned (unreachable?)\n", hosts[i]); bad++; continue; }
		{ char all[4096] = ""; for (int j = 0; j < k; j++) { strncat(all, lines[j], sizeof all - strlen(all) - 2); strcat(all, "\n"); } f = fingerprints(c, all, fp, 8); }
		fprintf(out, "%s:%d\n", hosts[i], ports[i]);
		for (int j = 0; j < f; j++) fprintf(out, "  %s\n", fp[j]);
	}
	if (!bad) fprintf(out, "compare with the fingerprint the server's owner gives you, then: netman ssh trust %s SHA256:...\n", p->name);
	return bad ? 1 : 0;
}

static int cmd_trust(const netman_ssh_cfg *c, const netman_ssh *p, const char *want, const char *only, FILE *out, FILE *err)
{
	char hosts[8][130], dirb[400]; int ports[8], n, stored = 0;
	hosts_of(p, hosts, ports, &n);
	if (strncmp(want, "SHA256:", 7) || strlen(want) < 20 || !mk_token_ok(want, 120)) { fputs("ssh: the fingerprint must look like SHA256:xxxxxxxx (from `netman ssh hostkey`)\n", err); return 2; }
	for (int i = 0; i < n; i++) {
		char raw[4096], lines[8][512], fp[8][200];
		int k, f;
		if (only && strcmp(only, hosts[i])) continue;
		if (scan(c, hosts[i], ports[i], raw, sizeof raw) == -ENOENT) { fprintf(err, "%s: %s\n", hosts[i], raw); continue; }
		k = keylines(raw, lines, 8);
		{ char all[4096] = ""; for (int j = 0; j < k; j++) { strncat(all, lines[j], sizeof all - strlen(all) - 2); strcat(all, "\n"); } f = fingerprints(c, all, fp, 8); }
		for (int j = 0; j < f && j < k; j++) {
			if (!strstr(fp[j], want)) continue;
			{
				const char *kh = known_for(c, p);
				FILE *fh;
				snprintf(dirb, sizeof dirb, "%s", kh);
				{ char *sl = strrchr(dirb, '/'); if (sl) { *sl = 0; mk_mkdirs(dirb); } }
				if (!(fh = fopen(kh, "a"))) { fprintf(err, "ssh: cannot write %s: %s\n", kh, strerror(errno)); return 1; }
				/* host[:port] form that ssh looks up: [host]:port for non-22 */
				{
					char *sp_ = strchr(lines[j], ' ');
					if (ports[i] != 22) fprintf(fh, "[%s]:%d%s\n", hosts[i], ports[i], sp_ ? sp_ : ""); else fprintf(fh, "%s%s\n", hosts[i], sp_ ? sp_ : "");
				}
				fclose(fh);
				fprintf(out, "trusted %s:%d (%s) in %s\n", hosts[i], ports[i], fp[j], kh);
				stored++;
			}
		}
	}
	if (!stored) { fprintf(err, "ssh: no server presented %s: nothing stored (wrong fingerprint, or a man in the middle)\n", want); return 1; }
	return 0;
}

static int cmd_exec(const netman_ssh_cfg *c, const netman_ssh *p, int check, int argc, char **argv, FILE *out, FILE *err)
{
	char *a[64], ctl[300], port[8], buf[4096];
	int k = 0;
	if (!p->control) { fprintf(err, "ssh %s: needs control = yes in the profile\n", p->name); return 1; }
	ctl_path(c, p, ctl, sizeof ctl);
	snprintf(port, sizeof port, "%d", p->port);
	a[k++] = (char *)c->bin; a[k++] = "-o"; a[k++] = "BatchMode=yes"; a[k++] = "-o"; a[k++] = "ControlMaster=no";
	{ snprintf(buf, sizeof buf, "ControlPath=%s", ctl); a[k++] = "-o"; a[k++] = buf; }
	a[k++] = "-p"; a[k++] = port;
	if (check) { a[k++] = "-O"; a[k++] = "check"; }
	a[k++] = "--"; a[k++] = (char *)p->host;
	for (int i = 0; i < argc && k < 62; i++) a[k++] = argv[i];
	a[k] = NULL;
	{
		char o[4096];
		int r = ent_exec(a, NULL, o, sizeof o);
		fputs(o, r < 0 && check ? err : out);
		if (r == -ENOENT) fprintf(err, "ssh: '%s' not found\n", c->bin);
		return r < 0 ? 1 : 0;
	}
}

int netman_ssh_main(const netman_ssh_cfg *c, FILE *out, FILE *err, int argc, char **argv)
{
	const char *cmd = argc > 0 ? argv[0] : "status";
	char e[300];
	netman_ssh p;
	if (!strcmp(cmd, "status") || !strcmp(cmd, "list")) {
		if (netman_ssh_status(c, out) == 0) fputs("no ssh profiles (netman ssh save NAME host=... user=... forward=L:8080:localhost:80)\n", out);
		return 0;
	}
	if (!strcmp(cmd, "save") && argc >= 3) {
		char path[400], txt[4096], body[4096];
		int l;
		if (!mk_name_ok(argv[1])) { fprintf(err, "ssh: bad profile name '%s'\n", argv[1]); return 2; }
		mk_path(path, sizeof path, c->dir, argv[1], "ssh");
		netman_ssh_init(&p);
		if (ent_slurp(path, txt, sizeof txt) > 0) netman_ssh_parse(txt, &p, e, sizeof e);   /* save edits an existing profile */
		snprintf(p.name, sizeof p.name, "%s", argv[1]);
		for (int i = 2; i < argc; i++) {
			char *eq = strchr(argv[i], '='), k[24];
			if (!eq || eq == argv[i] || eq - argv[i] >= 24) { fprintf(err, "ssh: expected key=value, got '%s'\n", argv[i]); return 2; }
			snprintf(k, sizeof k, "%.*s", (int)(eq - argv[i]), argv[i]);
			if (!strcmp(k, "forward") && !strcmp(eq + 1, "none")) { p.nfwd = 0; continue; }
			if (set(&p, k, eq + 1, e, sizeof e)) { fprintf(err, "ssh: %s\n", e); return 2; }
		}
		if (netman_ssh_check(&p, e, sizeof e) < 0) { fprintf(err, "ssh: %s\n", e); return 2; }
		l = snprintf(body, sizeof body, "# netman ssh profile\n");
		l += netman_ssh_format(&p, body + l, sizeof body - l);
		if (mk_mkdirs(c->dir) < 0 || ent_write_atomic(path, body, (size_t)l, 0600) < 0) { fprintf(err, "ssh: cannot write %s\n", path); return 1; }
		fprintf(out, "saved %s\n", p.name);
		return 0;
	}
	if (!strcmp(cmd, "apply") && argc == 1) {
		char names[64][32];
		int n = mk_names(c->dir, "ssh", names, 64), bad = 0, done = 0;
		for (int i = 0; i < n; i++) {
			if (load(c, names[i], &p, e, sizeof e) < 0) { fprintf(err, "ssh %s: %s\n", names[i], e); bad++; continue; }
			if (!p.autostart) continue;
			done++;
			bad += do_up(c, &p, out, err) != 0;
		}
		if (!done && !bad) fputs("no ssh profile has auto = yes\n", out);
		return bad ? 1 : 0;
	}
	if (argc == 2 && (!strcmp(cmd, "show") || !strcmp(cmd, "argv") || !strcmp(cmd, "up") || !strcmp(cmd, "down") || !strcmp(cmd, "run") || !strcmp(cmd, "hostkey") || !strcmp(cmd, "check") || !strcmp(cmd, "del"))) {
		if (!strcmp(cmd, "down")) { if (!mk_name_ok(argv[1])) return 2; return do_down(c, argv[1], out, err); }
		if (!strcmp(cmd, "del")) {
			char path[400];
			if (!mk_name_ok(argv[1])) return 2;
			do_down(c, argv[1], out, err);
			mk_path(path, sizeof path, c->dir, argv[1], "ssh");
			if (unlink(path) < 0) { fprintf(err, "ssh: no profile '%s'\n", argv[1]); return 1; }
			fprintf(out, "ssh profile %s removed (known_hosts untouched)\n", argv[1]);
			return 0;
		}
		if (load(c, argv[1], &p, e, sizeof e) < 0) { fprintf(err, "ssh: %s\n", e); return 1; }
		if (!strcmp(cmd, "show")) { char b[4096]; netman_ssh_format(&p, b, sizeof b); fputs(b, out); return 0; }
		if (!strcmp(cmd, "argv")) {
			char *a[96], buf[4096];
			if (netman_ssh_argv(c, &p, a, 96, buf, sizeof buf) < 0) { fputs("ssh: command line too long\n", err); return 1; }
			for (int i = 0; a[i]; i++) fprintf(out, "%s%s", i ? " " : "", a[i]);
			fputc('\n', out);
			return 0;
		}
		if (!strcmp(cmd, "up")) return do_up(c, &p, out, err);
		if (!strcmp(cmd, "run")) return do_run(c, &p, err);
		if (!strcmp(cmd, "check")) return cmd_exec(c, &p, 1, 0, NULL, out, err);
		return cmd_hostkey(c, &p, out, err);
	}
	if (!strcmp(cmd, "exec") && argc >= 3) {
		if (load(c, argv[1], &p, e, sizeof e) < 0) { fprintf(err, "ssh: %s\n", e); return 1; }
		return cmd_exec(c, &p, 0, argc - 2, argv + 2, out, err);
	}
	if (!strcmp(cmd, "trust") && (argc == 3 || argc == 4)) {
		if (load(c, argv[1], &p, e, sizeof e) < 0) { fprintf(err, "ssh: %s\n", e); return 1; }
		return cmd_trust(c, &p, argv[2], argc == 4 ? argv[3] : NULL, out, err);
	}
	fputs("usage: netman ssh status|list|save NAME k=v...|show NAME|argv NAME|up NAME|down NAME|run NAME|apply|del NAME|hostkey NAME|trust NAME SHA256:FP [HOST]|check NAME|exec NAME CMD...\n", err);
	return 2;
}
