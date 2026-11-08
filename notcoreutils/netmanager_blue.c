/* netmanager_blue - small network + bluetooth manager; thin CLI over netmanager_blue.h (the stb-style library).
 *
 *   netmanager_blue                 status of ethernet, wifi and bluetooth
 *   netmanager_blue wifi list|connect SSID [PASSPHRASE]|disconnect|forget SSID
 *   netmanager_blue bt list|scan [SEC]|connect|disconnect|pair|trust|remove NAME_OR_MAC|power on|off
 *   netmanager_blue fw              firewall ruleset (nft)
 */
#define _GNU_SOURCE
#define NMB_IMPLEMENTATION
#include "netmanager_blue.h"

int main(int argc, char **argv)
{
	const char *c = argc > 1 ? argv[1] : "", *sub = argc > 2 ? argv[2] : "list";
	if (!*c || !strcmp(c, "status")) { nmb_status(); return 0; }
	if (!strcmp(c, "fw")) return nmb_fw();
	if (!strcmp(c, "wifi")) {
		int wpa = nmb_wifi_backend_is_wpa();
		if (!strcmp(sub, "list")) return nmb_wifi_list();
		if (!strcmp(sub, "disconnect")) return nmb_wifi_disconnect();
		if (!strcmp(sub, "connect") && argc > 3) return nmb_wifi_connect(argv[3], argc > 4 ? argv[4] : NULL);
		if (!strcmp(sub, "forget") && argc > 3 && !wpa) return nmb_wifi_forget(argv[3]);
		fputs(wpa ? "usage: wifi list | connect SSID [PASSPHRASE] | disconnect\n" : "usage: wifi list | connect SSID [PASSPHRASE] | disconnect | forget SSID\n", stderr);
		return 2;
	}
	if (!strcmp(c, "bt")) {
		if (!strcmp(sub, "list")) return nmb_bt_list();
		if (!strcmp(sub, "scan")) return nmb_bt_scan(argc > 3 ? atoi(argv[3]) : 5);
		if (!strcmp(sub, "power") && argc > 3) return nmb_bt_power(!strcmp(argv[3], "on"));
		if (argc > 3) {
			int rc = nmb_bt_action(sub, argv[3]);
			if (rc != 2) return rc;
		}
		fputs("usage: bt list | scan [SEC] | connect|disconnect|pair|trust|remove NAME_OR_MAC | power on|off\n", stderr);
		return 2;
	}
	fputs("usage: netmanager_blue [status] | wifi ... | bt ... | fw\n", stderr);
	return 2;
}
