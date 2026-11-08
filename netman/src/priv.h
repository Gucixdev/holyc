/* priv.h - when netman needs root, and how it gets it (sudo re-exec with the password asked on the terminal).
 *
 *   int nm_priv_needed(root_flag, mode, wpa, wpa_ok, euid)   pure decision, unit-tested (tests/priv_test.c); root_flag is the cli.h table column
 *   int nm_priv_escalate(argc, argv)                         exec `sudo -p PROMPT -- SELF --priv-done ARGS...`; returns only on failure
 *
 * What needs root here (see the readme for the full table):
 *   - Wi-Fi through wpa_supplicant: its control socket dir (/run/wpa_supplicant) is root-only unless wpa_supplicant runs with
 *     `ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev` and you are in that group; `wpa_ok` says whether we can already open it.
 *   - Wi-Fi through iwd: D-Bus policy lets groups `wheel` and `network` talk to it, no root.
 *   - Bluetooth (bluez): the default D-Bus policy allows any user, no root; bluez (root) calls our pairing agent, which is allowed.
 *   - `fw` (nft ruleset), and later the IP / DHCP / DNS / VPN parts (netlink, nft): root.
 * The password is never put on a command line: sudo prompts for it itself, before the TUI draws anything.
 * Config privileges.sudo = never | always | auto (default); NM_SUDO_CMD=path of the sudo binary.
 */
#ifndef NM_PRIV_H
#define NM_PRIV_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

/* what a command needs; comes from the flag column of the cli.h table */
enum { NM_ROOT_NO, NM_ROOT_YES, NM_ROOT_WPA, NM_ROOT_NEVER };   /* WPA = only when wpa_supplicant owns Wi-Fi and its socket is closed to us; NEVER = not even in "always" mode */

/* mode = config privileges.sudo (auto|never|always; env NETMAN_SUDO / NM_SUDO are folded into it by config.c) */
static inline int nm_priv_needed(int root, const char *mode, int wpa, int wpa_ok, unsigned euid)
{
	if (euid == 0 || root == NM_ROOT_NEVER) return 0;
	if (mode && !strcmp(mode, "never")) return 0;
	if (mode && !strcmp(mode, "always")) return 1;
	if (root == NM_ROOT_YES) return 1;
	if (root == NM_ROOT_WPA) return wpa && !wpa_ok;
	return 0;
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
