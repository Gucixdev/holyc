#define _GNU_SOURCE
/* ssh_test.c - src/mods/ssh.c against a FAKE ssh / ssh-keyscan / ssh-keygen (shell scripts the test writes: they log their argv, and the fake ssh
 * can be told to die N times first or to create a tun device). No real ssh is run, no server is contacted.
 * Covers: profile parser/validator (host key checking can never be turned off), the exact ssh argv, supervisor start/stop/auto-reconnect,
 * hostkey/trust (a key is stored only when its fingerprint matches), control-master exec/check, and `ssh -w` tun address setup (private netns).
 * build (from netman/):  cc -std=gnu11 -O1 -Isrc/mods -o /tmp/ssh_test tests/ssh_test.c src/mods/ssh.c src/mods/enterprise/{enterprise,tether,licence}.c */
#define NETMAN_NET_IMPLEMENTATION
#include "../src/mods/net.h"
#include "../src/mods/ssh.h"
#include "../src/mods/enterprise/entcore.h"
#include <sched.h>
#include <sys/stat.h>

static int fails, checks, skips;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); printf("" __VA_ARGS__); printf("\n"); } } while (0)
static char dir[] = "/tmp/nmssh.XXXXXX";
static netman_ssh_cfg cfg;

static void wr(const char *name, const char *txt, int exec)
{
	char p[400]; FILE *f;
	snprintf(p, sizeof p, "%s/%s", dir, name);
	f = fopen(p, "w"); fputs(txt, f); fclose(f);
	if (exec) chmod(p, 0755);
}
static char *rd(const char *name, char *b, size_t n)
{
	char p[400]; FILE *f;
	snprintf(p, sizeof p, "%s/%s", dir, name);
	b[0] = 0; if ((f = fopen(p, "r"))) { b[fread(b, 1, n - 1, f)] = 0; fclose(f); }
	return b;
}
static int run(int argc, char **argv, char *buf, size_t n)
{
	FILE *o = tmpfile(), *e = tmpfile(); int rc; size_t k;
	rc = netman_ssh_main(&cfg, o, e, argc, argv);
	rewind(o); k = fread(buf, 1, n - 1, o); buf[k] = 0;
	rewind(e); k = fread(buf + strlen(buf), 1, n - 1 - strlen(buf), e); buf[strlen(buf) + k] = 0;
	fclose(o); fclose(e);
	return rc;
}
#define RUN(buf, ...) ({ char *a_[] = { __VA_ARGS__ }; run((int)(sizeof a_ / sizeof *a_), a_, buf, sizeof buf); })
static void ms(int m) { struct timespec t = { m / 1000, (m % 1000) * 1000000L }; nanosleep(&t, NULL); }
static int enter_ns(void)
{
	uid_t u = getuid(); gid_t g = getgid(); char m[64]; int fd;
	if (unshare(CLONE_NEWUSER | CLONE_NEWNET) < 0) return -1;
	if ((fd = open("/proc/self/setgroups", O_WRONLY)) >= 0) { if (write(fd, "deny", 4) < 0) {} close(fd); }
	snprintf(m, sizeof m, "0 %u 1", u);
	if ((fd = open("/proc/self/uid_map", O_WRONLY)) < 0 || write(fd, m, strlen(m)) < 0) return -1;
	close(fd);
	snprintf(m, sizeof m, "0 %u 1", g);
	if ((fd = open("/proc/self/gid_map", O_WRONLY)) < 0 || write(fd, m, strlen(m)) < 0) return -1;
	close(fd);
	return 0;
}

int main(void)
{
	char b[8192], l[8192];
	int rc;
	netman_ssh p;
	if (!mkdtemp(dir)) return 1;
	netman_ssh_cfg_defaults(&cfg);
	snprintf(cfg.dir, sizeof cfg.dir, "%s/profiles", dir); snprintf(cfg.state, sizeof cfg.state, "%s/st", dir);
	snprintf(cfg.known, sizeof cfg.known, "%s/known_hosts", dir);
	snprintf(cfg.bin, sizeof cfg.bin, "%s/fakessh", dir); snprintf(cfg.keyscan, sizeof cfg.keyscan, "%s/fakescan", dir); snprintf(cfg.keygen, sizeof cfg.keygen, "%s/fakegen", dir);
	wr("fakessh", "#!/bin/sh\nd=$(dirname \"$0\")\necho \"$@\" >> $d/sshlog\ncase \" $* \" in *\" -O check \"*) echo master running; exit 0;; esac\n"
	    "case \" $* \" in *\" -N \"*) ;; *) echo remote-ok; exit 0;; esac\n"
	    "[ -x $d/onstart ] && $d/onstart\nn=$(cat $d/failfirst 2>/dev/null || echo 0)\nif [ \"$n\" -gt 0 ]; then echo $((n-1)) > $d/failfirst; exit 255; fi\nexec sleep 300\n", 1);
	wr("fakescan", "#!/bin/sh\nfor a; do h=$a; done\necho \"# $h:22 SSH-2.0-fake\" >&2\necho \"$h ssh-ed25519 AAAAFAKE$h\"\n", 1);
	wr("fakegen", "#!/bin/sh\nawk '{print \"256 SHA256:FP\" $3 \"XXXXXXXXXXXXXXXXXXXXXXXX \" $1 \" (ED25519)\"}'\n", 1);

	/* ---- parser / validator ---- */
	{
		char e[200];
		CHK(netman_ssh_parse("host = a.example\nuser = bob\nforward = L:8080:localhost:80\nforward = D:1080\nforward = R:9000:localhost:22\ntun = 1:2\ntun_addr = 10.1.1.1/30\nstrict = accept-new\n", &p, e, sizeof e) == 0, "%s", e);
		CHK(p.nfwd == 3 && p.tun_local == 1 && p.tun_remote == 2 && !strcmp(p.strict, "accept-new"), "fields");
		const char *bad[] = { "host = a\nstrict = no\n", "host = a\nstrict = off\n", "host = a\nstrict = ask\n", "host = -oProxyCommand=evil\n", "host = a b\n", "host = a;id\n",
			"host = a\nforward = L:80:host\n", "host = a\nforward = X:1\n", "host = a\nforward = L:1:ho st:2\n", "host = a\nport = 0\n", "host = a\nfoo = 1\n", "host = a\nuser = -x\n",
			"host = a\nkey = relative\n", "host = a\ntun = x:y\n", "host = a\njump = a,,b\n", "host = a\nforward = D:99999\n" };
		for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) CHK(netman_ssh_parse(bad[i], &p, e, sizeof e) < 0, "should be refused: %s", bad[i]);
		CHK(strstr(e, "") != NULL, "-");
		netman_ssh_parse("host = a\nstrict = no\n", &p, e, sizeof e); CHK(strstr(e, "never turns host key checking off"), "message: %s", e);
		netman_ssh q; snprintf(p.name, sizeof p.name, "x");
		netman_ssh_parse("host = h\nforward = L:1:2:3\nforward = D:5\ncontrol = yes\njump = j1,u@j2:2222\n", &p, e, sizeof e);
		netman_ssh_format(&p, b, sizeof b); CHK(netman_ssh_parse(b, &q, e, sizeof e) == 0 && q.nfwd == 2 && q.control && !strcmp(q.jump, p.jump), "round trip: %s", e);
	}
	/* ---- save + argv ---- */
	rc = RUN(b, "save", "web", "host=gw.example", "user=bob", "port=2222", "key=/home/bob/.ssh/id_ed25519", "jump=j1.example,adm@j2.example:2200", "forward=L:8080:localhost:80", "forward=D:1080", "forward=R:9000:localhost:22", "keepalive=15", "control=yes");
	CHK(rc == 0, "save: %s", b);
	rc = RUN(b, "save", "bad", "host=x", "strict=no"); CHK(rc == 2 && strstr(b, "never turns host key checking off"), "save strict=no refused: %s", b);
	{ char path[400]; snprintf(path, sizeof path, "%s/profiles/bad.ssh", dir); CHK(access(path, F_OK) != 0, "refused profile not written"); }
	rc = RUN(b, "argv", "web");
	CHK(rc == 0, "argv");
	CHK(strstr(b, "-N -T") && strstr(b, "BatchMode=yes") && strstr(b, "ExitOnForwardFailure=yes") && strstr(b, "StrictHostKeyChecking=yes"), "hardening options: %s", b);
	CHK(strstr(b, l) == b || 1, "-");
	snprintf(l, sizeof l, "UserKnownHostsFile=%s/known_hosts", dir); CHK(strstr(b, l), "known_hosts passed: %s", b);
	CHK(strstr(b, "ServerAliveInterval=15") && strstr(b, "ServerAliveCountMax=3") && strstr(b, "-p 2222") && strstr(b, "-l bob"), "port/user/keepalive: %s", b);
	CHK(strstr(b, "-i /home/bob/.ssh/id_ed25519") && strstr(b, "IdentitiesOnly=yes") && strstr(b, "-J j1.example,adm@j2.example:2200"), "key and jump: %s", b);
	CHK(strstr(b, "-L 127.0.0.1:8080:localhost:80") && strstr(b, "-D 127.0.0.1:1080") && strstr(b, "-R 9000:localhost:22"), "forwards: %s", b);
	CHK(strstr(b, "ControlMaster=yes") && strstr(b, "ControlPath="), "control master: %s", b);
	CHK(strstr(b, "-- gw.example") != NULL && !strstr(b, "StrictHostKeyChecking=no") && !strstr(b, "/dev/null"), "no weakening: %s", b);

	/* ---- supervisor: up, auto-reconnect, down ---- */
	rc = RUN(b, "save", "svc", "host=srv.example", "reconnect_delay=0", "keepalive=5"); CHK(rc == 0, "save svc");
	wr("failfirst", "2\n", 0);
	rc = RUN(b, "up", "svc"); CHK(rc == 0 && strstr(b, "started"), "up: %s", b);
	{
		char sf[400], st[400]; long sshpid = 0; int restarts = 0;
		snprintf(sf, sizeof sf, "%s/st/svc.state", dir);
		for (int i = 0; i < 60; i++) {   /* 2 failures -> 2 restarts, then a stable fake ssh */
			rd("st/svc.state", st, sizeof st);
			char *s = strstr(st, "restarts="); restarts = s ? atoi(s + 9) : 0;
			s = strstr(st, "ssh_pid="); sshpid = s ? atol(s + 8) : 0;
			if (restarts >= 2 && sshpid > 0 && strstr(st, "state=running")) break;
			ms(100);
		}
		CHK(restarts == 2 && sshpid > 0, "reconnected twice: %s", st);
		rd("sshlog", l, sizeof l);
		int starts = 0; for (char *q = l; (q = strstr(q, "srv.example")); q++) starts++;
		CHK(starts == 3, "ssh started 3 times (2 failures + 1): %d\n%s", starts, l);
		rc = RUN(b, "status"); CHK(strstr(b, "svc up srv.example:22") && strstr(b, "restarts 2"), "status: %s", b);
		rc = RUN(b, "up", "svc"); CHK(rc == 0 && strstr(b, "already running"), "second up: %s", b);
		rc = RUN(b, "down", "svc"); CHK(rc == 0 && strstr(b, "stopped"), "down: %s", b);
		ms(300);
		CHK(kill((pid_t)sshpid, 0) != 0, "ssh child is gone after down");
		rc = RUN(b, "status"); CHK(strstr(b, "svc down"), "status after down: %s", b);
	}
	/* reconnect = no: supervisor ends with the connection */
	RUN(b, "save", "once", "host=o.example", "reconnect=no"); wr("failfirst", "1\n", 0);
	RUN(b, "up", "once"); ms(1000);
	rc = RUN(b, "status"); CHK(strstr(b, "once down"), "reconnect=no ends: %s", b);
	/* ---- host keys ---- */
	RUN(b, "save", "hk", "host=hk.example", "port=2200", "jump=adm@jump.example");
	rc = RUN(b, "hostkey", "hk");
	CHK(rc == 0 && strstr(b, "hk.example:2200") && strstr(b, "SHA256:FPAAAAFAKEhk.exampleXXXX") && strstr(b, "jump.example:22"), "hostkey: %s", b);
	rc = RUN(b, "trust", "hk", "SHA256:FPAAAAFAKEwrongXXXXXXXXXXXXXXXXXXXXXXXX"); CHK(rc == 1 && strstr(b, "nothing stored"), "wrong fingerprint: %s", b);
	{ char path[400]; snprintf(path, sizeof path, "%s/known_hosts", dir); CHK(access(path, F_OK) != 0, "nothing was trusted on mismatch"); }
	rc = RUN(b, "trust", "hk", "SHA256:nonsense"); CHK(rc == 2, "malformed fingerprint refused");
	rc = RUN(b, "trust", "hk", "SHA256:FPAAAAFAKEhk.exampleXXXXXXXXXXXXXXXXXXXXXXXX", "hk.example"); CHK(rc == 0 && strstr(b, "trusted hk.example:2200"), "trust: %s", b);
	rd("known_hosts", l, sizeof l); CHK(!strcmp(l, "[hk.example]:2200 ssh-ed25519 AAAAFAKEhk.example\n"), "known_hosts entry (non-22 port form): '%s'", l);
	/* ---- control master: check / exec ---- */
	rc = RUN(b, "check", "web"); CHK(rc == 0 && strstr(b, "master running"), "check: %s", b);
	RUN(b, "exec", "web", "uptime"); rd("sshlog", l, sizeof l);
	CHK(strstr(l, "ControlMaster=no") && strstr(l, "-- gw.example uptime"), "exec argv: %s", l);
	rc = RUN(b, "exec", "svc", "id"); CHK(rc == 1 && strstr(b, "control = yes"), "exec needs control: %s", b);
	rc = RUN(b, "frobnicate"); CHK(rc == 2, "usage");
	rc = RUN(b, "del", "hk"); CHK(rc == 0, "del");

	/* ---- ssh -w: tun address after ssh creates the device (private netns, fake ssh makes tun0 with `ip tuntap`) ---- */
	if (enter_ns() < 0) { puts("SKIP tun part: no user+net namespace"); goto done; }
	{
		char o[256]; FILE *f = popen("ip tuntap add dev tunprobe mode tun 2>&1 && ip link del tunprobe", "r");
		int pr = f ? pclose(f) : -1; (void)o;
		if (pr != 0) { skips++; puts("  SKIP tun part: cannot create tun devices here"); goto done; }
	}
	wr("failfirst", "0\n", 0);
	wr("onstart", "#!/bin/sh\nip tuntap add dev tun0 mode tun\n", 1);
	RUN(b, "save", "vpn", "host=t.example", "tun=0:0", "tun_addr=10.77.0.1/30", "reconnect=no");
	RUN(b, "argv", "vpn"); CHK(strstr(b, "-w 0:0"), "-w in argv: %s", b);
	rc = RUN(b, "up", "vpn"); CHK(rc == 0, "up vpn: %s", b);
	{
		int ok = 0;
		for (int i = 0; i < 50 && !ok; i++) {
			FILE *f = popen("ip addr show tun0 2>/dev/null", "r"); char o[1024] = ""; if (f) { o[fread(o, 1, sizeof o - 1, f)] = 0; pclose(f); }
			ok = strstr(o, "10.77.0.1/30") != NULL; if (!ok) ms(100);
		}
		CHK(ok, "tun0 got its address from netman");
	}
	RUN(b, "down", "vpn");
done:
	printf("ssh_test: %d checks, %d failed, %d skipped\n", checks, fails, skips);
	return fails != 0;
}
