/* priv.h - when netman needs root, and how it gets it (sudo re-exec with the password asked on the terminal).
 *
 *   int nm_priv_needed(argc, argv, tui, wpa, wpa_ok, euid)   pure decision, unit-tested (tests/priv_test.c)
 *   int nm_priv_escalate(argc, argv)                         exec `sudo -p PROMPT -- SELF --priv-done ARGS...`; returns only on failure
 *
 * What needs root here (see the readme for the full table):
 *   - Wi-Fi through wpa_supplicant: its control socket dir (/run/wpa_supplicant) is root-only unless wpa_supplicant runs with
 *     `ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev` and you are in that group; `wpa_ok` says whether we can already open it.
 *   - Wi-Fi through iwd: D-Bus policy lets groups `wheel` and `network` talk to it, no root.
 *   - Bluetooth (bluez): the default D-Bus policy allows any user, no root; bluez (root) calls our pairing agent, which is allowed.
 *   - `fw` (nft ruleset), and later the IP / DHCP / DNS / VPN parts (netlink, nft): root.
 * The password is never put on a command line: sudo prompts for it itself, before the TUI draws anything.
 * Environment: NM_SUDO=never (never ask) | always (ask even when not needed) | auto (default); NM_SUDO_CMD=path of the sudo binary.
 */
#ifndef NM_PRIV_H
#define NM_PRIV_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

static inline int nm_priv_needed(int argc, char **argv, int tui, int wpa, int wpa_ok, unsigned euid)
{
	const char *mode = getenv("NM_SUDO"), *c = argc > 1 ? argv[1] : "";
	if (euid == 0) return 0;
	if (mode && !strcmp(mode, "never")) return 0;
	if (mode && !strcmp(mode, "always")) return !(!*c || !strcmp(c, "help") || !strcmp(c, "-h") || !strcmp(c, "--help")) || tui;
	if (tui || !strcmp(c, "wifi")) return wpa && !wpa_ok;
	if (!strcmp(c, "fw")) return 1;
	return 0;   /* status, help, bt ...: no root */
}

/* returns -1 on failure (message already printed) */
static inline int nm_priv_escalate(int argc, char **argv)
{
	const char *sudo = getenv("NM_SUDO_CMD");
	char self[PATH_MAX], *av[argc + 8];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	int k = 0;
	if (n <= 0) { fputs("netman: cannot find myself to re-run under sudo\n", stderr); return -1; }
	self[n] = 0;
	if (!sudo || !*sudo) sudo = "sudo";
	av[k++] = (char *)sudo;
	av[k++] = "-p";
	av[k++] = "[netman] password for %p: ";
	av[k++] = "--";
	av[k++] = self;
	av[k++] = "--priv-done";   /* loop guard: the re-run must not ask again */
	for (int i = 1; i < argc; i++) av[k++] = argv[i];
	av[k] = NULL;
	fputs("netman: needs root for this (wpa_supplicant socket / netlink); asking sudo\n", stderr);
	execvp(sudo, av);
	fprintf(stderr, "netman: cannot run %s (%s); run netman as root, or see the readme for the group route\n", sudo, strerror(errno));
	return -1;
}
#endif
