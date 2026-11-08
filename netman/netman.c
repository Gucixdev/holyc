/* netman - small network + bluetooth manager; thin CLI over netmanager_blue.h (the stb-style library).
 *
 *   netman                     status of ethernet, wifi and bluetooth
 *   netman -tui                the terminal UI (also --tui, -t)
 *   netman help                this list
 *   netman wifi list|connect SSID [PASSPHRASE]|disconnect|forget SSID
 *   netman wifi wps pbc|pin [PIN]|cancel
 *   netman wifi hotspot start SSID [PASSPHRASE]|stop
 *   netman wifi known [autoconnect SSID on|off]
 *   netman bt list|scan [SEC]|connect|disconnect|pair|trust|remove NAME_OR_MAC|power on|off
 *   netman bt agent [SEC]            pairing agent (PIN / passkey / confirmation prompts on this terminal)
 *   netman bt pair NAME --pin PIN    pair non-interactively (PIN answers every prompt, confirmations accepted)
 *   netman fw                        firewall ruleset (nft)
 *
 * Root: when the command needs it (see priv.h) netman re-runs itself through sudo and the password is asked on the terminal first.
 * NM_SUDO=never|always|auto, NM_SUDO_CMD=path of sudo.
 */
#define _GNU_SOURCE
#define NMB_IMPLEMENTATION
#include "netmanager_blue.h"
#define NM_TUI_IMPLEMENTATION
#include "tui.h"
#include "priv.h"

static const char usage[] =
	"usage: netman [status] | -tui | help\n"
	"       netman wifi list | connect SSID [PASSPHRASE] | disconnect | forget SSID | wps pbc|pin [PIN]|cancel\n"
	"                   | hotspot start SSID [PASSPHRASE]|stop | known [autoconnect SSID on|off]\n"
	"       netman bt list | scan [SEC] | connect|disconnect|pair|trust|remove NAME_OR_MAC | power on|off | agent [SEC]   (pair accepts --pin PIN)\n"
	"       netman fw\n"
	"root: re-runs itself through sudo when needed (NM_SUDO=never|always|auto); -tui shows a full help popup on ?\n";

int main(int argc, char **argv)
{
	const char *c, *sub;
	int tui = 0, done = 0;
	if (argc > 1 && !strcmp(argv[1], "--priv-done")) { done = 1; memmove(argv + 1, argv + 2, (argc - 1) * sizeof *argv); argc--; }
	for (int i = 1; i < argc; i++)   /* -tui anywhere (the easy one to remember), --tui and -t as aliases */
		if (!strcmp(argv[i], "-tui") || !strcmp(argv[i], "--tui") || !strcmp(argv[i], "-t")) tui = 1;
	if (argc > 1 && (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) { fputs(usage, stdout); return 0; }
	if (!done) {
		int wifi = tui || (argc > 1 && !strcmp(argv[1], "wifi"));
		const char *wd = getenv("NMB_WPA_DIR");
		int wpa = wifi ? nmb_wifi_backend_is_wpa() : 0, ok = access(wd && *wd ? wd : "/run/wpa_supplicant", R_OK | X_OK) == 0;
		if (nm_priv_needed(argc, argv, tui, wpa, ok, (unsigned)geteuid()) && nm_priv_escalate(argc, argv) < 0) return 1;
	}
	if (tui) return nm_tui();
	for (int i = 1; i + 1 < argc; i++)   /* --pin PIN anywhere: take it out of argv */
		if (!strcmp(argv[i], "--pin")) {
			nmb_bt_set_pin(argv[i + 1]);
			memmove(argv + i, argv + i + 2, (argc - i - 1) * sizeof *argv);   /* includes the NULL terminator */
			argc -= 2;
			break;
		}
	c = argc > 1 ? argv[1] : "";
	sub = argc > 2 ? argv[2] : "list";
	if (!*c || !strcmp(c, "status")) { nmb_status(); return 0; }
	if (!strcmp(c, "fw")) return nmb_fw();
	if (!strcmp(c, "wifi")) {
		int wpa = nmb_wifi_backend_is_wpa();
		if (!strcmp(sub, "list")) return nmb_wifi_list();
		if (!strcmp(sub, "disconnect")) return nmb_wifi_disconnect();
		if (!strcmp(sub, "connect") && argc > 3) return nmb_wifi_connect(argv[3], argc > 4 ? argv[4] : NULL);
		if (!strcmp(sub, "forget") && argc > 3 && !wpa) return nmb_wifi_forget(argv[3]);
		if (!strcmp(sub, "wps") && argc > 3) {
			if (!strcmp(argv[3], "pbc")) return nmb_wifi_wps_pbc();
			if (!strcmp(argv[3], "pin")) return nmb_wifi_wps_pin(argc > 4 ? argv[4] : NULL);
			if (!strcmp(argv[3], "cancel")) return nmb_wifi_wps_cancel();
		}
		if (!strcmp(sub, "hotspot") && argc > 3) {
			if (!strcmp(argv[3], "start") && argc > 4) return nmb_wifi_hotspot_start(argv[4], argc > 5 ? argv[5] : NULL);
			if (!strcmp(argv[3], "stop")) return nmb_wifi_hotspot_stop();
		}
		if (!strcmp(sub, "known")) {
			if (argc == 3) return nmb_wifi_known();
			if (argc == 6 && !strcmp(argv[3], "autoconnect") && (!strcmp(argv[5], "on") || !strcmp(argv[5], "off"))) return nmb_wifi_autoconnect(argv[4], !strcmp(argv[5], "on"));
		}
		fputs("usage: netman wifi list | connect SSID [PASSPHRASE] | disconnect | forget SSID | wps pbc|pin [PIN]|cancel | hotspot start SSID [PASSPHRASE]|stop | known [autoconnect SSID on|off]\n", stderr);
		return 2;
	}
	if (!strcmp(c, "bt")) {
		if (!strcmp(sub, "list")) return nmb_bt_list();
		if (!strcmp(sub, "scan")) return nmb_bt_scan(argc > 3 ? atoi(argv[3]) : 5);
		if (!strcmp(sub, "agent")) return nmb_bt_agent(argc > 3 ? atoi(argv[3]) : 0);
		if (!strcmp(sub, "power") && argc > 3) return nmb_bt_power(!strcmp(argv[3], "on"));
		if (argc > 3) {
			int rc = nmb_bt_action(sub, argv[3]);
			if (rc != 2) return rc;
		}
		fputs("usage: netman bt list | scan [SEC] | connect|disconnect|pair|trust|remove NAME_OR_MAC | power on|off | agent [SEC]  (pair accepts --pin PIN)\n", stderr);
		return 2;
	}
	fputs(usage, stderr);
	return 2;
}
