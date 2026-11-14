/* vpnmod_test.c - the VPN modules (ipsec, l2tp, openconnect, sstp) and `vpn import`, with fake client binaries. Nothing real is touched.
 * build:  cc -std=gnu11 -Wall -Wextra -O1 -Isrc -Isrc/mods -o /tmp/vpnmod_test tests/vpnmod_test.c src/mods/vpnmod.c src/mods/openvpn.c src/mods/ipsec.c src/mods/l2tp.c src/mods/openconnect.c src/mods/sstp.c src/mods/vpnimport.c ; run /tmp/vpnmod_test
 * run from the netman/ directory. UPDATE=1 rewrites the golden files. Real swanctl/xl2tpd/openconnect: SKIP when absent (live part untested here). */
#define _GNU_SOURCE
#include "vpnmod.h"
#include "vpnimport.h"
#include "ipsec.h"
#include "l2tp.h"
#include "openconnect.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int pass, bad;
#define OK(c, ...) do { if (c) pass++; else { bad++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); putchar('\n'); } } while (0)
static char tmp[256];
static char *rd(const char *p) { size_t n; return netman_vm_slurp(p, &n, 1 << 20); }
static void golden(const char *name, const char *got)
{
	char p[300]; snprintf(p, sizeof p, "tests/vpn_golden/%s", name);
	if (getenv("UPDATE")) { FILE *f = fopen(p, "w"); if (f) { fputs(got, f); fclose(f); } return; }
	char *w = rd(p);
	OK(w && !strcmp(w, got), "golden %s differs\n--- got:\n%s", name, got);
	free(w);
}
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); chmod(p, 0600); }

static const char *EAP = "Type = ipsec\nServer = vpn.example.org\nAuth = eap\nUsername = bob\nPassword = pa$$ w0rd\nVirtualIP = yes\nRemoteId = vpn.example.org\n";
static const char *PSK = "Type = ipsec\nServer = 203.0.113.9\nAuth = psk\nLocalId = laptop\nPSK = sh\"are\nRoutes = 10.0.0.0/8, 192.168.5.0/24\n";
static const char *L2 = "Type = l2tp\nServer = l2.example.net\nUsername = al\nPassword = we\"ird\\pw\nPSK = ipsecpsk\nMTU = 1400\n";
static const char *OC = "Type = openconnect\nServer = vpn.corp.example\nProtocol = anyconnect\nUsername = carol\nPassword = Tr0ub4dor&3\nAuthGroup = staff\nRoutes = 10.1.0.0/16, 2001:db8::/32\n";

static void gold(void)
{
	char o[8192], e[200];
	OK(!netman_ipsec_gen("home", EAP, o, sizeof o, e, sizeof e), "ipsec eap: %s", e); golden("ipsec_eap.swanctl", o);
	OK(!strstr(o, "pa$$ w0rd"), "ipsec: the password is hex, never plain");
	OK(!netman_ipsec_gen("lab", PSK, o, sizeof o, e, sizeof e), "ipsec psk: %s", e); golden("ipsec_psk.swanctl", o);
	OK(strstr(o, "10.0.0.0/8"), "ipsec: routes become traffic selectors");
	netman_l2tp_rundir = "/run/netman/vpn";
	OK(!netman_l2tp_gen("office", L2, o, sizeof o, e, sizeof e), "l2tp xl2tpd: %s", e); golden("l2tp.xl2tpd", o);
	OK(!netman_l2tp_ppp("office", L2, o, sizeof o, e, sizeof e), "l2tp ppp: %s", e); golden("l2tp.ppp", o);
	OK(strstr(o, "we\\\"ird\\\\pw"), "ppp options: quote and backslash escaped");
	OK(!netman_l2tp_ipsec("office", L2, o, sizeof o, e, sizeof e) && strstr(o, "transport"), "l2tp psk gets a transport-mode SA");
	netman_vm_ctx cx = { "/tmp", "/run/netman/vpn", stdout, stderr, 1 };
	char *av[64]; int n = netman_oc_args("corp", OC, &cx, av, 64, e, sizeof e);
	OK(n > 0, "oc args: %s", e);
	char all[2048] = ""; for (int i = 0; i < n; i++) { strcat(all, av[i]); strcat(all, "\n"); }
	OK(!strstr(all, "Tr0ub4dor"), "openconnect: the password is NOT on argv");
	OK(strstr(all, "--passwd-on-stdin"), "openconnect: password via stdin");
	golden("openconnect.argv", all);
	OK(!netman_oc_script(OC, o, sizeof o, e, sizeof e) && strstr(o, "CISCO_SPLIT_INC_0_ADDR"), "split-tunnel wrapper exports CISCO_SPLIT_INC");
	golden("openconnect.script", o);
	/* validation refuses injection */
	OK(netman_ipsec_mod.validate("Type = ipsec\nServer = a b\nAuth = eap\nUsername = x\nPassword = y\n", e, sizeof e) != 0, "space in Server refused");
	OK(netman_ipsec_mod.validate("Type = ipsec\nServer = h\nAuth = eap\nUsername = x}\nPassword = y\n", e, sizeof e) != 0, "brace in Username refused");
	OK(netman_sstp_mod.up(&cx, "s", "/x", "Type = sstp\nServer = h\n", 0) != 0, "sstp is refused with a message");
}

static int read_file_has(const char *p, const char *needle) { char *t = rd(p); int r = t && strstr(t, needle); free(t); return r; }
static void fakes(void)
{
	char bin[300], p[400], path[2048], run[300]; snprintf(bin, sizeof bin, "%s/bin", tmp); snprintf(run, sizeof run, "%s/run", tmp);
	mkdir(bin, 0755); mkdir(run, 0755);
	snprintf(p, sizeof p, "%s/swanctl", bin);
	put(p, "#!/bin/sh\necho \"$@\" >> \"$FAKE_LOG/swanctl.argv\"\ncase \"$*\" in *list-sas*) echo 'nm-home: #1, ESTABLISHED, IKEv2';; esac\nexit 0\n"); chmod(p, 0755);
	snprintf(p, sizeof p, "%s/openconnect", bin);
	put(p, "#!/bin/sh\necho \"$@\" > \"$FAKE_LOG/oc.argv\"\ncat > \"$FAKE_LOG/oc.stdin\"\nwhile [ $# -gt 0 ]; do [ \"$1\" = --pid-file ] && echo $$ > \"$2\"; shift; done\nexit 0\n"); chmod(p, 0755);
	snprintf(path, sizeof path, "%s:%s", bin, getenv("PATH")); setenv("PATH", path, 1); setenv("FAKE_LOG", tmp, 1);
	netman_vm_ctx cx = { tmp, run, stdout, stderr, 1 };
	char prof[400]; snprintf(prof, sizeof prof, "%s/home.vpn", tmp); put(prof, EAP);
	int rc = netman_ipsec_mod.up(&cx, "home", prof, EAP, 5);
	OK(rc == 0, "ipsec up with a fake swanctl (rc %d)", rc);
	snprintf(p, sizeof p, "%s/swanctl.argv", tmp);
	OK(read_file_has(p, "--load-all") && read_file_has(p, "--initiate") && read_file_has(p, "nm-home"), "swanctl was driven: load-all then initiate nm-home");
	OK(!read_file_has(p, "w0rd"), "the password never reaches swanctl's argv");
	snprintf(prof, sizeof prof, "%s/corp.vpn", tmp); put(prof, OC);
	rc = netman_openconnect_mod.up(&cx, "corp", prof, OC, 3);
	snprintf(p, sizeof p, "%s/oc.stdin", tmp);
	OK(read_file_has(p, "Tr0ub4dor&3"), "openconnect got the password on stdin (rc %d)", rc);
	snprintf(p, sizeof p, "%s/oc.argv", tmp);
	OK(!read_file_has(p, "Tr0ub4dor"), "...and not on its argv");
}

static void xl2tpd_accepts(void)
{
	char w[64], o[8192], e[200], cf[600], pp[600], out[4096]; if (!netman_vm_which("xl2tpd", w, sizeof w)) return;
	char rd[300]; snprintf(rd, sizeof rd, "%s/xl", tmp); mkdir(rd, 0755); netman_l2tp_rundir = rd;
	netman_l2tp_gen("office", L2, o, sizeof o, e, sizeof e); snprintf(cf, sizeof cf, "%s/x.conf", rd); put(cf, o);
	netman_l2tp_ppp("office", L2, o, sizeof o, e, sizeof e); snprintf(pp, sizeof pp, "%s/office.ppp", rd); put(pp, o);
	snprintf(pp, sizeof pp, "%s/office.chap-unused", rd); put(pp, "");
	char pid[400], ctl[400]; snprintf(pid, sizeof pid, "%s/x.pid", rd); snprintf(ctl, sizeof ctl, "%s/office.control", rd);
	char *av[] = { "timeout", "2", "xl2tpd", "-D", "-c", cf, "-p", pid, "-C", ctl, NULL }; int st = 0;
	int rc = netman_vm_run(av, NULL, out, sizeof out, 6000, &st);
	OK(rc == 0 && !strstr(out, "parse_config") && !strstr(out, "Unknown") && !strstr(out, "rror"), "xl2tpd parses the generated config (%s)", out);
}
static void imports(void)
{
	static const struct { const char *f, *type, *ext; int ok; } t[] = {
		{ "tests/vpn_samples/nm-wireguard.nmconnection", "wireguard", ".conf", 1 }, { "tests/vpn_samples/nm-openvpn.nmconnection", "openvpn", ".ovpn", 1 },
		{ "tests/vpn_samples/nm-strongswan.nmconnection", "ipsec", ".vpn", 1 }, { "tests/vpn_samples/swanctl.conf", "ipsec", ".vpn", 1 },
		{ "tests/vpn_samples/nm-l2tp.nmconnection", "l2tp", ".vpn", 1 }, { "tests/vpn_samples/nm-openconnect.nmconnection", "openconnect", ".vpn", 1 },
		{ "tests/vpn_samples/ipsec.conf", "ipsec", ".vpn", 1 }, { "tests/vpn_samples/client.ovpn", "openvpn", ".ovpn", 1 }, { "tests/vpn_samples/wg-quick.conf", "wireguard", ".conf", 1 },
		{ "tests/vpn_samples/nm-pptp.nmconnection", "", "", 0 },
	};
	for (unsigned i = 0; i < sizeof t / sizeof *t; i++) {
		size_t l; char *x = netman_vm_slurp(t[i].f, &l, 1 << 16), er[300] = ""; netman_vpnimp o;
		if (!x) { OK(0, "missing sample %s", t[i].f); continue; }
		int rc = netman_vpnimp_convert(x, l, &o, er, sizeof er);
		OK((rc == 0) == t[i].ok, "%s: rc %d (%s)", t[i].f, rc, er);
		if (rc == 0 && t[i].ok) {
			OK(!strcmp(o.type, t[i].type) && !strcmp(o.ext, t[i].ext), "%s: got %s%s", t[i].f, o.type, o.ext);
			if (!strcmp(o.ext, ".vpn")) {          /* round trip: what we wrote parses and validates in its own module */
				const netman_vpnmod *m = netman_vpnmod_detect(o.text); char e2[200] = "", full[4096];
				snprintf(full, sizeof full, "%s\nPassword = x\nUsername = x\n", o.text);   /* secrets are deliberately not imported */
				OK(m && !m->validate(full, e2, sizeof e2), "%s: converted profile validates (%s)", t[i].f, e2);
			}
			char name[300]; snprintf(name, sizeof name, "import_%u%s", i, o.ext);
			if (strcmp(o.type, "wireguard")) { char *s = o.text; (void)s; }
			if (strcmp(o.type, "wireguard")) golden(name, o.text);
		} else if (!t[i].ok) OK(strstr(er, "PPTP"), "pptp refusal says why");
		netman_vpnimp_free(&o); free(x);
	}
}
int main(void)
{
	snprintf(tmp, sizeof tmp, "/tmp/vpnmod_test.%d", (int)getpid()); mkdir(tmp, 0700);
	gold(); fakes(); xl2tpd_accepts(); imports();
	char w[64]; if (!netman_vm_which("xl2tpd", w, sizeof w)) puts("  SKIP xl2tpd config acceptance: xl2tpd not installed");
	if (!netman_vm_which("swanctl", w, sizeof w)) puts("  SKIP live IPsec: swanctl/charon not installed (generated config + fake-driven only)");
	if (!netman_vm_which("openconnect", w, sizeof w)) puts("  SKIP live OpenConnect: binary not installed (argv/stdin checked with a fake)");
	printf("vpnmod: %d passed, %d failed\n", pass, bad);
	return bad != 0;
}
