/* sstp.c - SSTP placeholder module (see sstp.h): detect/validate/info only, up always refuses. */
#define _GNU_SOURCE
#include "sstp.h"
#include <string.h>
#include <limits.h>

static int detect(const char *t) { char ty[24]; return netman_vm_native(t, ty, sizeof ty) && !strcmp(ty, "sstp"); }
static int validate(const char *t, char *err, size_t en) { netman_vm_prof p; if (netman_vm_parse(t, &p, err, en)) return -1; int ok = netman_vm_get(&p, "server") != NULL; netman_vm_free(&p); if (!ok) snprintf(err, en, "Server missing"); return ok ? 0 : -1; }
static int up(const netman_vm_ctx *cx, const char *name, const char *path, const char *text, int wait)
{
	(void)path; (void)text; (void)wait;
	return netman_vm_fail(cx, "%s: SSTP is not supported (the only client, sstp-client, is not a maintained component here and was never tested); use OpenConnect or IPsec/IKEv2", name);
}
static int down(const netman_vm_ctx *cx, const char *name, const char *text) { (void)text; return netman_vm_fail(cx, "%s: SSTP is not supported, nothing to take down", name); }
static const char *state(const netman_vm_ctx *cx, const char *name) { (void)cx; (void)name; return "down"; }
static int ks(const char *t, char l[][160], int m, char *tun, size_t tn, const char *name) { (void)t; (void)l; (void)m; (void)name; if (tn) tun[0] = 0; return -1; }
static int info(const netman_vm_ctx *cx, const char *name, const char *text)
{
	char p[PATH_MAX]; (void)name; (void)text;
	netman_vm_msg(cx, "type         sstp (unsupported)");
	netman_vm_msg(cx, "state        down");
	netman_vm_msg(cx, "client       %s", netman_vm_which("sstpc", p, sizeof p) ? "sstpc is installed, but netman does not drive it" : "no sstpc on PATH");
	return 0;
}
const netman_vpnmod netman_sstp_mod = { "sstp", ".vpn", "sstpc", "vpn.sstp", detect, validate, NULL, up, down, state, ks, info };
