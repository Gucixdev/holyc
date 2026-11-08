/* netman - network + Bluetooth manager, a NetworkManager/bluez drop-in in plain C.
 *
 * Build (see ./run):  cc -std=gnu11 -O2 -Isrc -Isrc/mods src/netman.c src/config.c src/mods/enterprise.c src/mods/tether.c src/mods/licence.c src/mods/wireguard.c -o bin/netman
 * One translation unit: the stb-style modules in src/mods/, the command table (cli.h), help (help.c) and the TUI (tui.h) are included here.
 * `netman help` lists every command; the table in cli.h is the single source for dispatch, root decision, help and the man page.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#define NETMAN_WIFI_IMPLEMENTATION
#define NETMAN_BT_IMPLEMENTATION
#define NETMAN_NET_IMPLEMENTATION
#define NETMAN_ROAMING_IMPLEMENTATION
#define NETMAN_SCORING_IMPLEMENTATION
#define NETMAN_METRICS_IMPLEMENTATION
#define NETMAN_VPN_IMPLEMENTATION
#define NETMAN_FW_IMPLEMENTATION
#include "mods/vpn.h"   /* first: its system includes must precede net.h's IFF_LOWER_UP fallback define */
#include "mods/net.h"
#include "mods/scoring.h"
#include "mods/roaming.h"
#include "mods/metrics.h"
#include "mods/dbus.h"
#include "mods/wifi.h"
#include "mods/btth.h"
#include "mods/firewall.h"
#include "mods/enterprise.h"
#include "mods/licence.h"
#include "config.h"
#include "priv.h"
#include "cli.h"
#include "help.c"
#include "tui.h"

#ifndef NETMAN_NO_MAIN
int main(int argc, char **argv)
{
	const nc_cmd *c;
	int words = 0, done = 0, tui = 0, wpa = 0, ok = 1;
	char err[600];
	if (argc > 1 && !strcmp(argv[1], "--priv-done")) { done = 1; memmove(argv + 1, argv + 2, (argc - 1) * sizeof *argv); argc--; }
	for (int i = 1; i < argc; i++)   /* -tui anywhere (the easy one to remember), --tui and -t as aliases */
		if (!strcmp(argv[i], "-tui") || !strcmp(argv[i], "--tui") || !strcmp(argv[i], "-t")) { tui = 1; argv[1] = "-tui"; argc = 2; break; }
	if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) argv[1] = "help";
	if (argc == 1) { static char *st[] = { "netman", "status", NULL }; argv = st; argc = 2; }
	c = nc_match(argc - 1, argv + 1, &words);
	if (!c) {
		fprintf(stderr, "netman: unknown command '%s'\n", argv[1]);
		fputs("try: netman help\n", stderr);
		return 2;
	}
	/* configuration: a broken file stops everything except `config` and `help`, which are how you fix it */
	if (netman_config_load(&nc_cfg, err, sizeof err)) {
		if (strcmp(c->name, "help") && strncmp(c->name, "config", 6) && strcmp(c->name, "fw preset")) { fprintf(stderr, "netman: %s\n(netman config check, or fix the file)\n", err); return 1; }
		netman_config_defaults(&nc_cfg);
	}
	nc_apply(&nc_cfg);
	if (!tui && !nc_enabled(c->module)) {
		fprintf(stderr, "%s disabled in config (netman config toggle %s.enabled)\n", c->module, c->module);
		return 3;
	}
	if (!done && c->root != NM_ROOT_NEVER) {
		const char *wd = getenv("NETMAN_WPA_DIR");
		if (c->root == NM_ROOT_WPA && (tui || !strcmp(c->module, "wifi"))) {
			wpa = netman_wifi_backend_is_wpa();
			ok = access(wd && *wd ? wd : "/run/wpa_supplicant", R_OK | X_OK) == 0;
		}
		int root = c->root;
		if (tui && nc_enabled("vpn") && nc_vpn_has_profiles()) root = NM_ROOT_YES;   /* vpn up/down/kill switch are netlink + nft: the TUI needs root as soon as there is a profile to bring up */
		if (nm_priv_needed(root, netman_config_str(&nc_cfg, NC_PRIV_SUDO), wpa, ok, (unsigned)geteuid()) && nm_priv_escalate(argc, argv) < 0) return 1;
	}
	if (tui) return nc_tui();
	{
		int n = argc - 1 - words;
		char **a = argv + 1 + words;
		for (int i = 0; i + 1 < n; i++)   /* --pin PIN anywhere: take it out of the args */
			if (!strcmp(a[i], "--pin")) {
				netman_bt_set_pin(a[i + 1]);
				memmove(a + i, a + i + 2, (size_t)(n - i - 2) * sizeof *a);
				n -= 2;
				break;
			}
		return c->fn(c, n, a);
	}
}
#endif
