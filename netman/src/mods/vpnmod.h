/* vpnmod.h - the interface every non-WireGuard VPN type implements, plus the small helpers they share. Real .c: vpnmod.c.
 *
 * vpn.h keeps the profile directory, the kill switch, hooks, list/status/info/import and dispatches by profile type to a
 * netman_vpnmod (openvpn.c, ipsec.c, l2tp.c, openconnect.c, sstp.c). A module is one .c file with one const struct; the
 * registry in vpnmod.c is the only list. Profiles: NAME.ovpn / NAME.conf (OpenVPN, as before) or NAME.vpn (key = value,
 * `Type = ipsec|l2tp|openconnect|sstp`, see the readme). PPTP is deliberately NOT a type: it is cryptographically broken.
 *
 * Secrets never go on a command line: they travel in root-only files (0600, mode checked) or on the child's stdin.
 * Every function takes a netman_vm_ctx (directories and the streams messages go to) so modules need nothing from vpn.h.
 */
#ifndef NETMAN_VPNMOD_H
#define NETMAN_VPNMOD_H
#include <stdio.h>
#include <stddef.h>

typedef struct { const char *conf_dir, *run_dir; FILE *out, *err; int mute; } netman_vm_ctx;

/* ---- key = value profiles (NAME.vpn) ------------------------------------------------------------------------------- */
typedef struct { char *k, *v; } netman_vm_kv;                 /* k: lower case, spaces/_/- removed ("Remote ID" -> "remoteid") */
typedef struct { netman_vm_kv *kv; size_t n; } netman_vm_prof;
int  netman_vm_parse(const char *text, netman_vm_prof *p, char *err, size_t en);   /* 0 ok, -1 + "line N: ..." */
const char *netman_vm_get(const netman_vm_prof *p, const char *key);               /* first value or NULL; KEY already normalised */
int  netman_vm_next(const netman_vm_prof *p, const char *key, size_t *iter, const char **val);   /* every value of a repeated key */
void netman_vm_free(netman_vm_prof *p);
int  netman_vm_native(const char *text, char *type, size_t n);   /* 1 when TEXT is a NAME.vpn profile (has a Type key); type filled lower case */
int  netman_vm_tok_ok(const char *s);                          /* host / id / path-ish token: no spaces, quotes, braces, control chars; 1 ok */
int  netman_vm_cidr(const char *s, char *ip, size_t ipn, int *prefix, int *fam);   /* "10.0.0.0/8" or bare address; 0 ok */
int  netman_vm_split(const char *list, char **items, int max, char *buf, size_t bn);  /* split a "a, b c" list in buf; count or -1 */
void netman_vm_hex(const char *s, char *out, size_t n);        /* "0x" + hex of S (strongSwan secret syntax) */
int  netman_vm_msg(const netman_vm_ctx *cx, const char *fmt, ...);   /* message to cx->out */
int  netman_vm_fail(const netman_vm_ctx *cx, const char *fmt, ...);  /* "netman vpn: ..." to cx->err (unless mute); returns 1 */

/* ---- processes and files ------------------------------------------------------------------------------------------- */
int  netman_vm_which(const char *bin, char *path, size_t n);   /* 1 if executable BIN is on $PATH (path filled) */
int  netman_vm_run(char *const argv[], const char *input, char *out, size_t on, int timeout_ms, int *status);   /* no shell; out = stdout+stderr; 0 ran, -1 could not start / timed out */
int  netman_vm_spawn(char *const argv[], const char *input, const char *logfile);   /* run to exit with stdin=INPUT, output appended to LOGFILE; exit status or -1 */
int  netman_vm_pid_alive(const char *pidfile, const char *needle, int *pid);   /* 1 when the pid is alive AND needle is one of its argv words */
int  netman_vm_mkdirs(const char *path, unsigned mode);
int  netman_vm_write(const char *path, const void *data, size_t n, unsigned mode);   /* atomic, 0 ok */
char *netman_vm_slurp(const char *path, size_t *len, size_t cap);
int  netman_vm_secret_ok(const netman_vm_ctx *cx, const char *path);   /* root/self-owned, mode 0600 */
int  netman_vm_log_has(const netman_vm_ctx *cx, const char *name, const char *needle);
int  netman_vm_path(char *buf, size_t n, const netman_vm_ctx *cx, const char *name, const char *ext);   /* RUN_DIR/NAME.EXT */
int  netman_vm_routes(const char *ifname, const char *list, int add);    /* ip route add|del for a "a, b" list via `ip`; 0 ok */

int  netman_vm_ifname(const char *prefix, const char *name, char *out, size_t n);   /* "l2-" + NAME cut to 15 chars */
int  netman_vm_ks_host(const char *host, char lines[][160], int max, const char *l4);   /* kill-switch accept rules for every address of HOST; l4 "" = any protocol, else "udp dport 4500" ... */
int  netman_vm_defroute_host(const char *ip, char *undo, size_t n);   /* keep the route to the VPN server outside the tunnel: `ip route replace IP/32 via GW dev DEV`; undo = args to delete it */

/* ---- the module interface ------------------------------------------------------------------------------------------ */
typedef struct netman_vpnmod {
	const char *type;       /* "openvpn" */
	const char *ext;        /* ".ovpn" or ".vpn": what import writes */
	const char *binary;     /* the client program it drives, NULL = none */
	const char *toggle;     /* config key (vpn.openvpn ...) */
	int  (*detect)(const char *text);                                                       /* 1 when TEXT is a profile of this type */
	int  (*validate)(const char *text, char *err, size_t en);                               /* 0 ok */
	int  (*gen)(const char *name, const char *text, char *out, size_t n, char *err, size_t en);   /* the client's own config (golden-tested); 0 ok; NULL = n/a */
	int  (*up)(const netman_vm_ctx *cx, const char *name, const char *path, const char *text, int wait);
	int  (*down)(const netman_vm_ctx *cx, const char *name, const char *text);
	const char *(*state)(const netman_vm_ctx *cx, const char *name);                        /* down | connecting | connected | auth failed */
	int  (*ks)(const char *text, char lines[][160], int max, char *tun, size_t tunn, const char *name);   /* kill-switch accept rules for the transport; count, -1 bad; tun = tunnel iface pattern ("" = none) */
	int  (*info)(const netman_vm_ctx *cx, const char *name, const char *text);              /* "key   value" lines on cx->out */
} netman_vpnmod;

extern const netman_vpnmod netman_openvpn_mod, netman_ipsec_mod, netman_l2tp_mod, netman_openconnect_mod, netman_sstp_mod;
const netman_vpnmod *const *netman_vpnmods(void);                   /* NULL terminated; index + 2 = vpn.h type number */
const netman_vpnmod *netman_vpnmod_detect(const char *text);        /* first module whose detect() accepts TEXT */
const netman_vpnmod *netman_vpnmod_by_type(const char *type);
#endif
