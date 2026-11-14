#define _GNU_SOURCE
/* bonding_test.c - src/mods/bonding.c: the `bond` front end over enterprise.c. Status parsing runs against a FAKE sysfs tree
 * (netman_ent_sys); the real bonds are built with rtnetlink in the test's OWN user+net namespace (host network never touched).
 * `ip` is used only to cross-check. SKIP when the kernel will not make a bond there.
 * build (from netman/):  cc -std=gnu11 -O1 -Isrc/mods -o /tmp/bonding_test tests/bonding_test.c src/mods/bonding.c src/mods/enterprise/{enterprise,tether,licence}.c */
#define NETMAN_NET_IMPLEMENTATION
#include "../src/mods/net.h"
#include "../src/mods/bonding.h"
#include "../src/mods/enterprise/enterprise.h"
#include "../src/mods/enterprise/entcore.h"
#include <sched.h>
#include <sys/stat.h>

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
	out[0] = 0;
	while (l + 1 < n && fgets(out + l, (int)(n - l), p)) l = strlen(out);
	return pclose(p);
}
static void put(const char *root, const char *rel, const char *txt)
{
	char p[400], d[400]; FILE *f; char *s;
	snprintf(p, sizeof p, "%s/%s", root, rel); snprintf(d, sizeof d, "%s", p);
	s = strrchr(d, '/'); *s = 0; ent_mkdirs(d);
	f = fopen(p, "w"); fputs(txt, f); fclose(f);
}
static char *run(int argc, char **argv, int *rc, char *buf, size_t n)
{
	FILE *o = tmpfile(), *e = tmpfile();
	*rc = netman_bond_main(o, e, argc, argv);
	rewind(o); size_t k = fread(buf, 1, n - 1, o); buf[k] = 0;
	rewind(e); k = fread(buf + strlen(buf), 1, n - 1 - strlen(buf), e); buf[strlen(buf) + k] = 0;
	fclose(o); fclose(e);
	return buf;
}

int main(void)
{
	char dir[] = "/tmp/nmbond.XXXXXX", buf[4096], o[4096];
	int rc;
	if (!mkdtemp(dir)) return 1;
	/* ---- status from a fake sysfs ---- */
	{
		char root[300];
		snprintf(root, sizeof root, "%s/sys", dir);
		put(root, "class/net/bond0/bonding/mode", "active-backup 1\n");
		put(root, "class/net/bond0/bonding/slaves", "eth0 eth1\n");
		put(root, "class/net/bond0/bonding/active_slave", "eth0\n");
		put(root, "class/net/eth0/operstate", "up\n");
		put(root, "class/net/eth1/operstate", "down\n");
		put(root, "class/net/bond1/bonding/mode", "802.3ad 4\n");
		put(root, "class/net/bond1/bonding/slaves", "eth2 eth3\n");
		put(root, "class/net/eth2/operstate", "up\n");
		put(root, "class/net/eth3/operstate", "up\n");
		put(root, "class/net/bond2/bonding/mode", "balance-rr 0\n");
		put(root, "class/net/bond2/bonding/slaves", "\n");
		put(root, "class/net/lo/operstate", "unknown\n");
		netman_ent_sys = root;
		char *a[] = { "status" };
		run(1, a, &rc, buf, sizeof buf);
		CHK(rc == 0 && strstr(buf, "bond0 degraded active-backup  slaves 1/2 up  active eth0"), "bond0 degraded: %s", buf);
		CHK(strstr(buf, "bond1 up 802.3ad  slaves 2/2 up"), "bond1 up: %s", buf);
		CHK(strstr(buf, "bond2 down balance-rr  slaves 0/0 up"), "bond2 down: %s", buf);
		CHK(!strstr(buf, "lo "), "non-bond ignored");
		char *m[] = { "modes" };
		run(1, m, &rc, buf, sizeof buf);
		CHK(strstr(buf, "802.3ad") && strstr(buf, "active-backup"), "modes listed");
		netman_ent_sys = "/sys";
	}
	/* ---- argument checking (nothing reaches the kernel) ---- */
	{
		char *a1[] = { "add", "b0", "mode=nonsense", "eth1" }, *a2[] = { "add", "../x", "eth1" }, *a3[] = { "add", "b0", "bogus=1", "eth1" }, *a4[] = { "frob" };
		run(4, a1, &rc, buf, sizeof buf); CHK(rc == 2 && strstr(buf, "unknown mode"), "bad mode: %s", buf);
		run(3, a2, &rc, buf, sizeof buf); CHK(rc == 2, "bad name");
		run(4, a3, &rc, buf, sizeof buf); CHK(rc == 2 && strstr(buf, "unknown key"), "bad key");
		run(1, a4, &rc, buf, sizeof buf); CHK(rc == 2 && strstr(buf, "usage"), "usage");
	}
	/* ---- real bonds in a private namespace ---- */
	if (enter_ns() < 0) { puts("SKIP live: no user+net namespace"); goto done; }
	{
		char conf[300];
		snprintf(conf, sizeof conf, "%s/conf", dir); ent_mkdirs(conf); netman_conf_dir = conf;
		sh("ip link add eth1 type dummy; ip link add eth2 type dummy; ip link add eth3 type dummy", o, sizeof o);
		CHK(if_nametoindex("eth3") != 0, "dummies (needs ip)");
		char *a[] = { "add", "bnd0", "mode=active-backup", "miimon=100", "primary=eth1", "eth1", "eth2" };
		run(7, a, &rc, buf, sizeof buf);
		if (rc != 0 && (strstr(buf, "not supported") || strstr(buf, "Operation not") || strstr(buf, "Permission") || strstr(buf, "No such device"))) { skips++; printf("  SKIP bond create: %s", buf); goto done; }
		CHK(rc == 0, "bond add: %s", buf);
		sh("ip -d link show bnd0; ip link show eth1", o, sizeof o);
		CHK(strstr(o, "bond mode active-backup") && strstr(o, "miimon 100") && strstr(o, "master bnd0"), "kernel agrees: %s", o);
		snprintf(buf, sizeof buf, "%s/conf/bnd0.ent", dir); CHK(access(buf, R_OK) == 0, "profile saved");
		char *s1[] = { "slave", "add", "bnd0", "eth3" };
		run(4, s1, &rc, buf, sizeof buf); CHK(rc == 0, "slave add: %s", buf);
		sh("ip link show eth3", o, sizeof o); CHK(strstr(o, "master bnd0"), "eth3 enslaved");
		char *sh1[] = { "show", "bnd0" };
		run(2, sh1, &rc, buf, sizeof buf); CHK(rc == 0 && strstr(buf, "active-backup") && strstr(buf, "eth3") && strstr(buf, "profile: saved"), "show: %s", buf);
		char *s2[] = { "slave", "del", "bnd0", "eth3" };
		run(4, s2, &rc, buf, sizeof buf); CHK(rc == 0, "slave del: %s", buf);
		sh("ip link show eth3", o, sizeof o); CHK(!strstr(o, "master bnd0"), "eth3 released");
		{ netman_ent e; netman_ent_load("bnd0", &e); CHK(e.nmember == 2, "profile follows slave changes (%d)", e.nmember); }
		char *dl[] = { "del", "bnd0" };
		run(2, dl, &rc, buf, sizeof buf); CHK(rc == 0 && if_nametoindex("bnd0") == 0, "bond del: %s", buf);
		snprintf(buf, sizeof buf, "%s/conf/bnd0.ent", dir); CHK(access(buf, F_OK) != 0, "profile removed");
		char *ap[] = { "apply" };
		run(1, ap, &rc, buf, sizeof buf); CHK(rc == 0 && strstr(buf, "no saved bond profiles"), "apply with none: %s", buf);
		char *a2[] = { "add", "bnd1", "mode=802.3ad", "lacp_rate=fast", "eth1", "eth2" };
		run(6, a2, &rc, buf, sizeof buf); CHK(rc == 0, "lacp bond: %s", buf);
		sh("ip -d link show bnd1", o, sizeof o); CHK(strstr(o, "802.3ad") && strstr(o, "lacp_rate fast"), "lacp in kernel: %s", o);
		sh("ip link del bnd1", o, sizeof o);
		run(1, ap, &rc, buf, sizeof buf); CHK(rc == 0 && strstr(buf, "bnd1 created") && if_nametoindex("bnd1") != 0, "apply recreates from the profile: %s", buf);
	}
done:
	printf("bonding_test: %d checks, %d failed, %d skipped\n", checks, fails, skips);
	return fails != 0;
}
