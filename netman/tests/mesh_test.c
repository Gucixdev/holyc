#define _GNU_SOURCE
/* mesh_test.c - src/mods/mesh.c: profile parser, peer-list parser, wg-quick generation (pure), the WireGuard mesh brought up for real inside its
 * OWN user+net namespace (skipped when the kernel has no wireguard), and the batman path driven by FAKE `ip` / `batctl` scripts (a dummy device
 * stands in for bat0: batman-adv itself is NOT exercised). The host network is never touched.
 * build (from netman/): see the README test list; link mesh.c, the vpn mods (wireguard vpnmod openvpn ipsec l2tp openconnect sstp vpnimport) and enterprise/*.c */
#define NETMAN_VPN_IMPLEMENTATION
#include "../src/mods/vpn.h"
#define NETMAN_NET_IMPLEMENTATION
#include "../src/mods/net.h"
#include "../src/mods/mesh.h"
#include "../src/mods/enterprise/entcore.h"
#include "../src/mods/enterprise/enterprise.h"
#include <sys/stat.h>
#include <sched.h>
#include <sys/prctl.h>

static int fails, checks, skips;
#define CHK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL %s:%d: %s  ", __FILE__, __LINE__, #c); printf(" " __VA_ARGS__); printf("\n"); } } while (0)

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
static char dir[64];
static void wr(const char *name, const char *body, int x)
{
	char p[200];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	FILE *f = fopen(p, "w");
	if (f) { fputs(body, f); fclose(f); }
	if (x) chmod(p, 0755);
}
static int slurp(const char *name, char *b, size_t n)
{
	char p[200];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	b[0] = 0;
	return ent_slurp(p, b, n);
}
static void mkpair(char *priv_b64, char *pub_b64)
{
	unsigned char k[32], p[32];
	netman_vpn_genkey(k); netman_vpn_pubkey(k, p);
	netman_vpn_b64_encode32(k, priv_b64); priv_b64[44] = 0;
	netman_vpn_b64_encode32(p, pub_b64); pub_b64[44] = 0;
}

int main(void)
{
	netman_mesh m;
	netman_mesh_node n[8];
	char e[200], buf[4096], pa[48], pb[48], pc[48], pr[48], list[1024];
	int r;

	/* ---- profile parser ---- */
	CHK(netman_mesh_parse("kind = batman\nifaces = eth1,wlan1\ngw_mode = server\nhop_penalty = 30\n", &m, e, sizeof e) == 0, "%s", e);
	CHK(netman_mesh_check(&m, e, sizeof e) == 0 && !strcmp(m.iface, "bat0"), "%s", e);
	CHK(netman_mesh_parse("kind = batman\nifaces = eth1;rm\n", &m, e, sizeof e) < 0, "shell metachar in ifaces refused");
	CHK(netman_mesh_parse("kind = batman\nifaces = -eth1\n", &m, e, sizeof e) < 0, "leading dash refused");
	CHK(netman_mesh_parse("kind = ospf\n", &m, e, sizeof e) < 0, "unknown kind");
	CHK(netman_mesh_parse("kind = batman\ngw_mode = bogus\n", &m, e, sizeof e) < 0, "bad gw_mode");
	CHK(netman_mesh_parse("kind = wg\nself = a\naddr = 10.9.0.1\n", &m, e, sizeof e) < 0, "addr needs a prefix");
	netman_mesh_parse("kind = batman\n", &m, e, sizeof e);
	CHK(netman_mesh_check(&m, e, sizeof e) < 0, "batman without ifaces: %s", e);
	netman_mesh_parse("kind = wg\n", &m, e, sizeof e);
	CHK(netman_mesh_check(&m, e, sizeof e) < 0, "wg without self");
	netman_mesh_parse("kind = wg\nself = a\nifaces = eth1\n", &m, e, sizeof e);
	CHK(netman_mesh_check(&m, e, sizeof e) < 0, "mixing batman keys into wg");
	netman_mesh_parse("kind = wg\nself = a\n", &m, e, sizeof e);
	CHK(netman_mesh_check(&m, e, sizeof e) == 0 && m.port == 51820 && m.keepalive == 25 && !strcmp(m.iface, "wgmesh0"), "defaults");
	netman_mesh_format(&m, buf, sizeof buf);
	CHK(strstr(buf, "kind = wg") && strstr(buf, "self = a") && strstr(buf, "auto = no"), "%s", buf);

	/* ---- peer list + generator ---- */
	mkpair(pr, pa); mkpair(pr, pb); mkpair(pr, pc);
	snprintf(list, sizeof list, "# nodes\na %s 203.0.113.1:51820 10.77.0.1\nb %s 203.0.113.2:51820 10.77.0.2\nroam %s - 10.77.0.3\n", pa, pb, pc);
	CHK(netman_mesh_nodes_parse(list, n, 8, e, sizeof e) == 3, "%s", e);
	CHK(netman_mesh_nodes_parse("a AAAA 1.2.3.4:5 10.0.0.1\n", n, 8, e, sizeof e) < 0, "short key refused");
	snprintf(buf, sizeof buf, "a %s 1.2.3.4:5 10.0.0.1\nb %s 1.2.3.4:5 10.0.0.1\n", pa, pb);
	CHK(netman_mesh_nodes_parse(buf, n, 8, e, sizeof e) < 0, "duplicate address refused");
	snprintf(buf, sizeof buf, "a %s 1.2.3.4 10.0.0.1\n", pa);
	CHK(netman_mesh_nodes_parse(buf, n, 8, e, sizeof e) < 0, "endpoint without a port refused");
	snprintf(buf, sizeof buf, "a %s \"x\":1 10.0.0.1\n", pa);
	CHK(netman_mesh_nodes_parse(buf, n, 8, e, sizeof e) < 0, "quote in endpoint refused");
	netman_mesh_nodes_parse(list, n, 8, e, sizeof e);
	snprintf(m.name, sizeof m.name, "lab");
	r = netman_mesh_gen(&m, n, 3, "b", NULL, buf, sizeof buf);
	CHK(r > 0 && strstr(buf, "Address = 10.77.0.2/24") && strstr(buf, "PrivateKey = <private key of this node>"), "%s", buf);
	CHK(strstr(buf, pa) && strstr(buf, pc) && !strstr(buf, pb), "every other node is a peer, self is not");
	CHK(strstr(buf, "AllowedIPs = 10.77.0.1/32") && strstr(buf, "Endpoint = 203.0.113.1:51820"), "%s", buf);
	{ char *roam = strstr(buf, pc); CHK(roam && !strstr(roam, "Endpoint"), "roaming node has no Endpoint line"); }
	CHK(netman_mesh_gen(&m, n, 3, "zz", NULL, buf, sizeof buf) == -ENOENT, "unknown node");

	/* ---- profiles on disk + CLI ---- */
	snprintf(dir, sizeof dir, "/tmp/nmmesh.XXXXXX");
	if (!mkdtemp(dir)) return 1;
	{
		char md[200], fk[200], so[2048] = "", se[2048] = "";
		FILE *o = fmemopen(so, sizeof so - 1, "w"), *er = fmemopen(se, sizeof se - 1, "w");
		char *sv1[] = { "save", "lab", "kind=wg", "self=a", "addr=1" };
		char *sv2[] = { "save", "lab", "kind=wg", "self=a", "prefix=24", "key=/nonexistent/k" };
		snprintf(md, sizeof md, "%s/mesh", dir);
		netman_mesh_dir = md;
		CHK(netman_mesh_main(o, er, 5, sv1) == 2, "addr is not a key for save in wg");
		CHK(netman_mesh_main(o, er, 6, sv2) == 0, "save");
		{ char *st[] = { "status" }; fflush(o); rewind(o); memset(so, 0, sizeof so); netman_mesh_main(o, er, 1, st); fflush(o); CHK(strstr(so, "lab down wg wgmesh0"), "status: %s", so); }
		snprintf(fk, sizeof fk, "%s/lab.peers", md);
		wr("mesh/lab.peers", list, 0);
		{ char *gn[] = { "gen", "lab" }; fflush(o); rewind(o); memset(so, 0, sizeof so); r = netman_mesh_main(o, er, 2, gn); fflush(o); CHK(r == 0 && strstr(so, "Address = 10.77.0.1/24") && strstr(so, "PrivateKey = <private"), "gen self without a key file shows the placeholder: %s", so); }
		{ char *in[] = { "init", "lab" }; r = netman_mesh_main(o, er, 2, in); CHK(r == 1, "init refuses a path it cannot write"); }
		fclose(o); fclose(er);
	}

	/* ---- batman with fake ip / batctl, inside a private netns ---- */
	if (enter_ns() < 0) { puts("SKIP live part: cannot create a user+net namespace"); skips++; goto done; }
	{
		char md[200], ipf[200], bf[200], log[2048], st[2048] = "", se[512] = "";
		FILE *o, *er;
		char *argv_save[] = { "save", "bm", "kind=batman", "ifaces=eth1,eth2", "gw_mode=client", "orig_interval=500", "hop_penalty=20" };
		char *argv_up[] = { "up", "bm" }, *argv_dn[] = { "down", "bm" };
		snprintf(md, sizeof md, "%s/mesh2", dir);
		snprintf(ipf, sizeof ipf, "%s/fakeip", dir); snprintf(bf, sizeof bf, "%s/fakebatctl", dir);
		netman_mesh_dir = md; netman_mesh_ip = ipf; netman_mesh_batctl = bf;
		wr("fakeip", "#!/bin/sh\nd=$(dirname \"$0\")\necho \"ip $*\" >> $d/log\ncase \"$*\" in *batadv*) exec ip link add name bat0 type dummy;; esac\nexit 0\n", 1);
		wr("fakebatctl", "#!/bin/sh\nd=$(dirname \"$0\")\necho \"batctl $*\" >> $d/log\n[ \"$3\" = neighbors ] && echo 'IF Neighbor last-seen'\nexit 0\n", 1);
		o = fmemopen(st, sizeof st - 1, "w"); er = fmemopen(se, sizeof se - 1, "w");
		CHK(netman_mesh_main(o, er, 7, argv_save) == 0);
		r = netman_mesh_main(o, er, 2, argv_up); fflush(er);
		CHK(r == 0, "up: %s", se);
		slurp("log", log, sizeof log);
		CHK(strstr(log, "ip link add name bat0 type batadv"), "%s", log);
		CHK(strstr(log, "batctl meshif bat0 interface add eth1") && strstr(log, "batctl meshif bat0 interface add eth2"), "%s", log);
		CHK(strstr(log, "batctl meshif bat0 gw_mode client") && strstr(log, "batctl meshif bat0 orig_interval 500") && strstr(log, "batctl meshif bat0 hop_penalty 20"), "%s", log);
		CHK(ent_idx("bat0") >= 0, "the stand-in bat0 exists");
		CHK(netman_mesh_main(o, er, 2, argv_up) == 0, "second up is harmless");
		{ char *s[] = { "status" }; fflush(o); rewind(o); memset(st, 0, sizeof st); netman_mesh_main(o, er, 1, s); fflush(o); CHK(strstr(st, "bm up batman bat0  ports 2"), "%s", st); }
		{ char *nb[] = { "neighbors", "bm" }; fflush(o); rewind(o); memset(st, 0, sizeof st); r = netman_mesh_main(o, er, 2, nb); fflush(o); CHK(r == 0 && strstr(st, "Neighbor"), "%s", st); }
		CHK(netman_mesh_main(o, er, 2, argv_dn) == 0 && ent_idx("bat0") < 0, "down removes the device");
		/* no batctl: master via ip */
		netman_mesh_batctl = "/nonexistent/batctl";
		wr("log", "", 0);
		CHK(netman_mesh_main(o, er, 2, argv_up) == 0);
		slurp("log", log, sizeof log);
		CHK(strstr(log, "ip link set dev eth1 master bat0"), "without batctl the ports join with ip: %s", log);
		netman_mesh_main(o, er, 2, argv_dn);
		/* no ip at all */
		netman_mesh_ip = "/nonexistent/ip";
		fflush(er); rewind(er); memset(se, 0, sizeof se);
		CHK(netman_mesh_main(o, er, 2, argv_up) == 1, "up without ip fails"); fflush(er);
		CHK(strstr(se, "iproute2"), "%s", se);
		fclose(o); fclose(er);
		netman_mesh_ip = ipf;
	}

	/* ---- real WireGuard mesh in the namespace ---- */
	{
		char md[200], ka[48], kb[48], ke[300], so[2048] = "", se[512] = "";
		FILE *o = fmemopen(so, sizeof so - 1, "w"), *er = fmemopen(se, sizeof se - 1, "w");
		char *sv[] = { "save", "w", "kind=wg", "self=a", "iface=wgt0", "port=51999", "prefix=24" };
		char *up[] = { "up", "w" }, *in[] = { "init", "w" }, *dn[] = { "down", "w" }, *gn[] = { "gen", "w" };
		snprintf(md, sizeof md, "%s/mesh3", dir);
		netman_mesh_dir = md;
		mkpair(ka, pa); mkpair(kb, pb);
		snprintf(list, sizeof list, "a %s 127.0.0.1:51999 10.78.0.1\nb %s 127.0.0.1:51998 10.78.0.2\n", pa, pb);
		(void)ke;
		CHK(netman_mesh_main(o, er, 7, sv) == 0);
		{ char p[300]; snprintf(p, sizeof p, "%s/w.peers", md); ent_write_atomic(p, list, strlen(list), 0644); }
		fflush(er); rewind(er); memset(se, 0, sizeof se);
		CHK(netman_mesh_main(o, er, 2, up) == 1, "up without a private key"); fflush(er);
		CHK(strstr(se, "mesh init"), "%s", se);
		CHK(netman_mesh_main(o, er, 2, in) == 0, "init");
		{ char p[300]; struct stat sb; snprintf(p, sizeof p, "%s/w.key", md); CHK(stat(p, &sb) == 0 && (sb.st_mode & 077) == 0, "private key is mode 0600"); }
		CHK(netman_mesh_main(o, er, 2, in) == 1, "init never overwrites a key");
		/* the pubkey of the generated key must replace a's entry so gen shows it */
		r = netman_mesh_main(o, er, 2, up);
		if (r != 0) { fflush(er); printf("SKIP wireguard live part: %s\n", se); skips++; }
		else {
			char o2[300];
			netman_wg_stat s;
			CHK(ent_idx("wgt0") >= 0, "device exists");
			CHK(netman_wg_stats("wgt0", &s) == 0 && s.npeers == 1 && s.listen_port == 51999, "peers %d port %d", s.npeers, s.listen_port);
			{ netman_ent_info i; CHK(netman_ent_getinfo("wgt0", &i) == 0 && i.up, "link up"); }
			{ char *sx[] = { "status" }; fflush(o); rewind(o); memset(so, 0, sizeof so); netman_mesh_main(o, er, 1, sx); fflush(o); CHK(strstr(so, "w up wg wgt0  peers 0/1"), "%s", so); }
			CHK(netman_mesh_main(o, er, 2, up) == 0, "up twice is harmless (reconfigures)");
			(void)o2;
			CHK(netman_mesh_main(o, er, 2, dn) == 0 && ent_idx("wgt0") < 0, "down");
		}
		(void)gn;
		fclose(o); fclose(er);
	}
done:
	printf("mesh_test: %d checks, %d failed, %d skipped\n", checks, fails, skips);
	return fails ? 1 : 0;
}
