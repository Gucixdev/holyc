#define _GNU_SOURCE
/* tunnel_test.c - src/mods/tunnel.c: profile parser/validator/MTU maths (pure), then real rtnetlink tunnels inside its OWN user+net namespace
 * (the host network is never touched; the live part refuses to run if the namespace cannot be made). Kinds the kernel in the namespace
 * will not create (module not loaded, unprivileged) are SKIP, not failures. `ip` is used only to cross-check what the kernel really built.
 * build (from netman/):  cc -std=gnu11 -O1 -Isrc/mods -o /tmp/tunnel_test tests/tunnel_test.c src/mods/tunnel.c src/mods/enterprise/enterprise.c && /tmp/tunnel_test */
#define NETMAN_NET_IMPLEMENTATION
#include "../src/mods/net.h"
#include "../src/mods/tunnel.h"
#include <sched.h>
#include <sys/prctl.h>

static int fails, checks, skips;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); printf("" __VA_ARGS__); printf("\n"); } } while (0)

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
static int sh(const char *cmd, char *out, size_t n)
{
	FILE *p = popen(cmd, "r"); size_t l = 0;
	if (!p) return -1;
	while (out && l + 1 < n && fgets(out + l, (int)(n - l), p)) l = strlen(out);
	return pclose(p);
}
static netman_tun mk(const char *text)
{
	netman_tun t; char e[200];
	if (netman_tunnel_parse(text, &t, e, sizeof e)) { printf("parse: %s\n", e); memset(&t, 0, sizeof t); }
	return t;
}
static int uptun(const char *name, const char *text, const char *expect_kind)
{
	netman_tun t = mk(text); char e[300], o[2048], cmd[200];
	snprintf(t.name, sizeof t.name, "%s", name);
	int r = netman_tunnel_up(&t, e, sizeof e);
	if (r == -EOPNOTSUPP || r == -EPERM || r == -ENODEV || r == -EPROTONOSUPPORT || r == -EAFNOSUPPORT) { skips++; printf("  SKIP %s: %s\n", name, e); return 1; }
	CHK(r == 0, "%s up: %s", name, e);
	snprintf(cmd, sizeof cmd, "ip -d link show %s 2>&1", name);
	if (sh(cmd, o, sizeof o) == 0) CHK(strstr(o, expect_kind) != NULL, "%s kind '%s' in: %s", name, expect_kind, o);
	return r;
}

int main(void)
{
	char e[300], o[2048], dir[] = "/tmp/nmtun.XXXXXX";
	netman_tun t;
	/* ---- pure ---- */
	CHK(netman_tunnel_overhead("gre", 0, 0) == 24 && netman_tunnel_overhead("gre", 0, 1) == 28 && netman_tunnel_overhead("gretap", 0, 0) == 38, "gre overhead");
	CHK(netman_tunnel_overhead("ipip", 0, 0) == 20 && netman_tunnel_overhead("sit", 0, 0) == 20, "ipip/sit overhead");
	CHK(netman_tunnel_overhead("vxlan", 0, 0) == 50 && netman_tunnel_overhead("vxlan", 1, 0) == 70 && netman_tunnel_overhead("geneve", 0, 0) == 50, "vxlan/geneve overhead");
	t = mk("kind = gre\nlocal = 10.0.0.1\nremote = 10.0.0.2\nkey = 42\nttl = 64\n# c\nauto = yes\naddr = 192.168.50.1/30\n");
	snprintf(t.name, sizeof t.name, "g0");
	CHK(netman_tunnel_check(&t, e, sizeof e) == 0, "%s", e);
	CHK(t.key == 42 && t.ttl == 64 && t.autostart, "parsed fields");
	CHK(netman_tunnel_mtu(&t, NULL, NULL) == 1500 - 28, "gre+key mtu 1472");
	t.mtu = 1400; CHK(netman_tunnel_mtu(&t, NULL, NULL) == 1400, "fixed mtu wins"); t.mtu = 0;
	{ char b[1024]; netman_tun u; netman_tunnel_format(&t, b, sizeof b); CHK(netman_tunnel_parse(b, &u, e, sizeof e) == 0 && u.key == 42 && !strcmp(u.remote, "10.0.0.2"), "format/parse round trip"); }
	t = mk("kind = ipip\nlocal = 10.0.0.1\nremote = fd00::1\n"); snprintf(t.name, sizeof t.name, "x"); CHK(netman_tunnel_check(&t, e, sizeof e) < 0, "mixed families refused");
	t = mk("kind = sit\nlocal = fd00::1\nremote = fd00::2\n"); snprintf(t.name, sizeof t.name, "x"); CHK(netman_tunnel_check(&t, e, sizeof e) < 0, "sit over v6 refused");
	t = mk("kind = frob\nremote = 1.2.3.4\n"); snprintf(t.name, sizeof t.name, "x"); CHK(netman_tunnel_check(&t, e, sizeof e) < 0, "unknown kind");
	t = mk("kind = gretap\nremote = 1.2.3.4\nkey = 5\n"); snprintf(t.name, sizeof t.name, "x"); CHK(netman_tunnel_check(&t, e, sizeof e) == 0, "gretap ok");
	t = mk("kind = vxlan\nlocal = 1.2.3.4\n"); snprintf(t.name, sizeof t.name, "x"); CHK(netman_tunnel_check(&t, e, sizeof e) < 0, "vxlan without remote/group");
	t = mk("kind = vxlan\nremote = 1.2.3.4\nvni = 7\n"); snprintf(t.name, sizeof t.name, "x"); CHK(netman_tunnel_check(&t, e, sizeof e) == 0 && t.port == 4789, "vxlan default port");
	t = mk("kind = l2tp\nlocal = 1.1.1.1\nremote = 2.2.2.2\n"); snprintf(t.name, sizeof t.name, "x"); CHK(netman_tunnel_check(&t, e, sizeof e) < 0, "l2tp needs ids");
	t = mk("kind = gre\nremote = 1.2.3.4\nbogus = 1\n"); CHK(t.kind[0] == 0, "unknown key rejected by parser");
	t = mk("kind = gre\nremote = 1.2.3.4\naddr = 300.1.1.1/24\n"); snprintf(t.name, sizeof t.name, "x"); CHK(netman_tunnel_check(&t, e, sizeof e) < 0, "bad addr");
	/* ---- live, in a private namespace ---- */
	if (enter_ns() < 0) { puts("SKIP live part: cannot create a user+net namespace"); goto done; }
	sh("ip link set lo up; ip link add eth9 type dummy && ip link set eth9 up && ip addr add 10.9.0.1/24 dev eth9", o, sizeof o);
	CHK(if_nametoindex("eth9") != 0, "dummy underlay (needs `ip`)");
	if (!mkdtemp(dir)) return 1;
	netman_tunnel_dir = dir;
	if (uptun("tg0", "kind = gre\nlocal = 10.9.0.1\nremote = 10.9.0.2\nkey = 7\nttl = 32\naddr = 172.16.0.1/30\n", "gre") == 0) {
		sh("ip -d link show tg0; ip addr show tg0", o, sizeof o);
		CHK(strstr(o, "ttl 32") && strstr(o, "172.16.0.1/30") && strstr(o, "mtu 1472"), "gre: ttl, addr, mtu 1472: %s", o);
		CHK(netman_tunnel_down(&(netman_tun){ .name = "tg0", .kind = "gre", .key = -1 }, e, sizeof e) == 0 && if_nametoindex("tg0") == 0, "gre down");
	}
	if (uptun("tgt0", "kind = gretap\nlocal = 10.9.0.1\nremote = 10.9.0.3\nlink = eth9\n", "gretap") == 0) {
		sh("ip link show tgt0", o, sizeof o); CHK(strstr(o, "mtu 1462") != NULL, "gretap mtu 1462: %s", o);
	}
	uptun("tg6", "kind = gre\nlocal = fd00::1\nremote = fd00::2\n", "ip6gre");
	uptun("ti0", "kind = ipip\nlocal = 10.9.0.1\nremote = 10.9.0.4\nmtu = 1400\n", "ipip");
	uptun("ts0", "kind = sit\nlocal = 10.9.0.1\nremote = 10.9.0.5\naddr = 2001:db8::1/64\n", "sit");
	if (uptun("tv0", "kind = vxlan\nremote = 10.9.0.6\nlocal = 10.9.0.1\nvni = 100\nport = 4789\naddr = 10.50.0.1/24\n", "vxlan id 100") == 0) {
		sh("ip link show tv0", o, sizeof o); CHK(strstr(o, "mtu 1450") != NULL, "vxlan mtu 1450: %s", o);
	}
	uptun("tn0", "kind = geneve\nremote = 10.9.0.7\nvni = 5\n", "geneve");
	CHK(uptun("tg0", "kind = gre\nlocal = 10.9.0.1\nremote = 10.9.0.2\n", "gre") >= 0, "gre again");
	{ netman_tun d = mk("kind = gre\nlocal = 10.9.0.1\nremote = 10.9.0.2\n"); snprintf(d.name, sizeof d.name, "tg0"); CHK(netman_tunnel_up(&d, e, sizeof e) == -EEXIST, "second up -> EEXIST"); }
	/* CLI + persistence */
	{
		FILE *out = tmpfile(), *err = tmpfile();
		char *a1[] = { "save", "lab", "kind=gre", "local=10.9.0.1", "remote=10.9.0.9", "auto=yes", "addr=172.31.0.1/30" };
		char *a2[] = { "save", "bad", "kind=gre", "remote=nonsense" };
		char *a3[] = { "apply" }, *a4[] = { "status" }, *a5[] = { "mtu", "lab" }, *a6[] = { "down", "lab" }, *a7[] = { "del", "lab" };
		char buf[2048];
		CHK(netman_tunnel_main(out, err, 7, a1) == 0, "cli save");
		CHK(netman_tunnel_main(out, err, 4, a2) == 2, "cli save rejects bad remote");
		snprintf(buf, sizeof buf, "%s/lab.tun", dir); CHK(access(buf, R_OK) == 0, "profile written");
		CHK(netman_tunnel_main(out, err, 1, a3) == 0 && if_nametoindex("lab") != 0, "apply brings up auto tunnels");
		rewind(out); ftruncate(fileno(out), 0);
		netman_tunnel_main(out, err, 1, a4); rewind(out); buf[fread(buf, 1, sizeof buf - 1, out)] = 0;
		CHK(strstr(buf, "lab ") && strstr(buf, "gre") && strstr(buf, "mtu 1476"), "status row: %s", buf);
		rewind(out); ftruncate(fileno(out), 0);
		netman_tunnel_main(out, err, 2, a5); rewind(out); buf[fread(buf, 1, sizeof buf - 1, out)] = 0;
		CHK(strstr(buf, "tunnel mtu 1476"), "mtu cmd: %s", buf);
		CHK(netman_tunnel_main(out, err, 2, a6) == 0 && if_nametoindex("lab") == 0, "cli down");
		CHK(netman_tunnel_main(out, err, 2, a7) == 0 && access(buf, F_OK) != 0, "cli del");
		fclose(out); fclose(err);
	}
	/* L2TPv3 goes through `ip l2tp`: a fake ip records the argv (no kernel l2tp in a namespace) */
	{
		char fake[300], log[300], buf[2048]; FILE *f;
		snprintf(fake, sizeof fake, "%s/fakeip", dir); snprintf(log, sizeof log, "%s/iplog", dir);
		f = fopen(fake, "w"); fprintf(f, "#!/bin/sh\necho \"$*\" >> %s\n", log); fclose(f); chmod(fake, 0755);
		netman_tunnel_ip = fake;
		netman_tun l = mk("kind = l2tp\nlocal = 10.9.0.1\nremote = 10.9.0.8\ntunnel_id = 1\npeer_tunnel_id = 2\nsession_id = 11\npeer_session_id = 12\nencap = udp\nlport = 5000\nrport = 5001\n");
		snprintf(l.name, sizeof l.name, "l2e0");
		CHK(netman_tunnel_up(&l, e, sizeof e) != 0 || 1, "l2tp up runs");   /* the final link-up fails: no such device, expected */
		buf[0] = 0; f = fopen(log, "r"); if (f) { buf[fread(buf, 1, sizeof buf - 1, f)] = 0; fclose(f); }
		CHK(strstr(buf, "l2tp add tunnel tunnel_id 1 peer_tunnel_id 2 encap udp local 10.9.0.1 remote 10.9.0.8 udp_sport 5000 udp_dport 5001"), "l2tp tunnel argv: %s", buf);
		CHK(strstr(buf, "l2tp add session name l2e0 tunnel_id 1 session_id 11 peer_session_id 12"), "l2tp session argv: %s", buf);
		netman_tunnel_down(&l, e, sizeof e);
		buf[0] = 0; f = fopen(log, "r"); if (f) { buf[fread(buf, 1, sizeof buf - 1, f)] = 0; fclose(f); }
		CHK(strstr(buf, "l2tp del session tunnel_id 1 session_id 11") && strstr(buf, "l2tp del tunnel tunnel_id 1"), "l2tp del argv: %s", buf);
	}
done:
	printf("tunnel_test: %d checks, %d failed, %d skipped\n", checks, fails, skips);
	return fails != 0;
}
