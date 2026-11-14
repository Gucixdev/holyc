/* vpn.h - stb-style single-header VPN layer for netman (NetworkManager drop-in, no python, no libs).
 *
 *   #define _GNU_SOURCE             before the first system include
 *   #define NETMAN_VPN_IMPLEMENTATION  in exactly ONE .c file
 *   #include "vpn.h"
 *
 * WireGuard is done natively: rtnetlink creates the link / addresses / routes / policy rules and the WireGuard generic-netlink
 * API configures it (no wg(8), no wg-quick). Input is a normal wg-quick .conf (Address DNS MTU ListenPort FwMark Table
 * PreUp/PostUp/PreDown/PostDown, [Peer] PublicKey PresharedKey Endpoint AllowedIPs PersistentKeepalive). A default route in
 * AllowedIPs uses wg-quick's scheme: fwmark on the device, "not fwmark N -> table N" rule plus "main suppress_prefixlength 0".
 * OpenVPN is run as `openvpn --daemon` with pidfile + log; credentials only ever come from a root-only file, never argv.
 *
 * Profiles: <netman_vpn_conf_dir>/NAME.conf (WireGuard when it has [Interface], else OpenVPN), NAME.ovpn, or NAME.vpn (key = value with
 * `Type = ipsec|l2tp|openconnect|sstp`). Every non-WireGuard type is a module (vpnmod.h: openvpn.c ipsec.c l2tp.c openconnect.c sstp.c); this file
 * only dispatches to them and keeps the kill switch, hooks, list/status/info/import for all. PPTP is deliberately not supported (broken crypto).
 * Runtime state: <netman_vpn_run_dir> (pidfiles, logs, resolv.conf backup). Not thread-safe. Messages go to netman_vpn_out / netman_vpn_err.
 * Private keys are never printed or logged and are wiped after use. Config files must be owned by root (or the caller) and
 * not group/world writable because PostUp & co. run as shell commands, exactly as in wg-quick.
 *
 * Extras NetworkManager cannot do: kill switch (nftables, atomic, survives tunnel flaps), DNS-leak guard, per-command tunnel
 * (netman_vpn_run: the command lives in its own netns whose only route is the tunnel), reconnect watchdog, MTU probe, fastest relay,
 * providers (Mullvad, NordVPN; table is pluggable).
 */
#ifndef NETMAN_VPN_H
#define NETMAN_VPN_H

#include <stdio.h>
#include <stddef.h>
#include "wireguard.h"
#include "vpnmod.h"
#include "vpnimport.h"

#ifndef NETMAN_VPN_API
#define NETMAN_VPN_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

#define NETMAN_VPN_WG   1
#define NETMAN_VPN_OVPN 2
#define NETMAN_VPN_IPSEC 3        /* 2.. = index in netman_vpnmods() + 2: openvpn ipsec l2tp openconnect sstp */
#define NETMAN_VPN_L2TP 4
#define NETMAN_VPN_OPENCONNECT 5
#define NETMAN_VPN_SSTP 6

typedef netman_wg_ip netman_vpn_ip;       /* family AF_INET / AF_INET6 */
typedef struct { char **v; size_t n; } netman_vpn_strs;
typedef struct {
    unsigned char pub[32], psk[32]; int has_psk;
    char *endpoint;                     /* "host:port" or "[v6]:port", NULL = none */
    int keepalive;                      /* seconds, 0 = off */
    netman_vpn_ip *allowed; size_t nallowed;
} netman_vpn_peer;
typedef struct {
    unsigned char priv[32]; int has_priv;
    int listen_port, mtu;
    unsigned fwmark; int has_fwmark, fwmark_off;
    int table_off; long table;          /* table_off: install no routes; table 0 = auto */
    netman_vpn_ip *addr; size_t naddr;
    netman_vpn_strs dns, search, pre_up, post_up, pre_down, post_down;
    netman_vpn_peer *peers; size_t npeers;
} netman_vpn_wg;

typedef netman_wg_stat netman_vpn_wgstat;     /* live numbers come from wireguard.c */

extern FILE *netman_vpn_out, *netman_vpn_err;                    /* NULL = stdout / stderr */
extern const char *netman_vpn_conf_dir;                    /* default /etc/netman/vpn */
extern const char *netman_vpn_run_dir;                     /* default /run/netman/vpn */
extern const char *netman_vpn_resolv_path;                 /* default /etc/resolv.conf */

/* keys */
NETMAN_VPN_API int  netman_vpn_b64_decode32(const char *s, unsigned char out[32]);   /* 0 ok, -1 not exactly a 32-byte key */
NETMAN_VPN_API void netman_vpn_b64_encode32(const unsigned char in[32], char out[45]);
NETMAN_VPN_API void netman_vpn_pubkey(const unsigned char priv[32], unsigned char pub[32]);
NETMAN_VPN_API int  netman_vpn_genkey(unsigned char priv[32]);
/* config */
NETMAN_VPN_API int  netman_vpn_wg_parse(const char *text, size_t len, netman_vpn_wg *c, char *err, size_t errsz);   /* 0 ok, -1 + message (never the value) */
NETMAN_VPN_API void netman_vpn_wg_free(netman_vpn_wg *c);
NETMAN_VPN_API int  netman_vpn_valid_name(const char *name, size_t maxlen);
NETMAN_VPN_API const char *netman_vpn_type_name(const char *name);                    /* "wireguard", "openvpn", "ipsec", "l2tp", "openconnect", "sstp" or NULL */
extern int (*netman_vpn_enabled_cb)(const char *type);     /* NULL = everything allowed; the CLI sets it from the vpn.* / wireguard.enabled config keys */
NETMAN_VPN_API int  netman_vpn_type(const char *name);                                /* NETMAN_VPN_WG, NETMAN_VPN_OVPN, 0 = no such profile */
/* control: all return 0 ok, nonzero failure (message in netman_vpn_err) */
NETMAN_VPN_API int  netman_vpn_up(const char *name, int wait_sec);                    /* wait_sec: OpenVPN wait for "connected" */
NETMAN_VPN_API int  netman_vpn_down(const char *name);
NETMAN_VPN_API int  netman_vpn_status(const char *name);                              /* prints; 0 up, 1 down */
NETMAN_VPN_API int  netman_vpn_list(void);                                            /* "NAME TYPE up|down|connecting|failed|locked|untrusted [ep=.. hs=S rx=B tx=B]" per profile, then "killswitch on|off|unknown" */
NETMAN_VPN_API int  netman_vpn_info(const char *name);                                /* "key   value" lines: type, state, endpoint, handshake, transfer, allowed ips, split/full tunnel, dns, ... */
NETMAN_VPN_API int  netman_vpn_import(const char *file, const char *name_or_null);    /* copy a WireGuard / OpenVPN config into the profile dir (mode 0600); refuses to overwrite */
NETMAN_VPN_API int  netman_vpn_ks_state(void);                                        /* nftables kill switch table: 1 on, 0 off, -1 cannot tell (not root / no nft) */
NETMAN_VPN_API void netman_vpn_env(void);                                             /* NETMAN_VPN_CONF_DIR / NETMAN_VPN_RUN_DIR -> netman_vpn_conf_dir / _run_dir */
NETMAN_VPN_API int  netman_vpn_logs(const char *name, int lines);                     /* OpenVPN log tail */
NETMAN_VPN_API int  netman_vpn_wg_stats(const char *ifname, netman_vpn_wgstat *st);
/* extras */
NETMAN_VPN_API int  netman_vpn_killswitch(const char *name, int on);                  /* nft table inet netman_vpn_ks; on needs the profile's endpoints */
NETMAN_VPN_API int  netman_vpn_run(const char *name, char *const argv[]);             /* run one command through the tunnel in its own netns; returns its exit status */
NETMAN_VPN_API int  netman_vpn_watch_once(const char *name, int stale_sec);           /* 0 healthy, 1 reconnected, -1 failed */
NETMAN_VPN_API int  netman_vpn_watch(const char *name, int interval_sec, int stale_sec);  /* loop forever */
NETMAN_VPN_API int  netman_vpn_rtt_ms(const char *ip, int timeout_ms);                /* ICMP echo via unprivileged ping socket; ms or -1 */
NETMAN_VPN_API int  netman_vpn_mtu_probe(const char *ip);                             /* largest MTU that passes unfragmented, or -1 */
/* providers */
typedef struct {
    char id[64], country[8], city[48], host[64], endpoint_ip[48], pubkey[48];
    int port, multihop_port, load;
} netman_vpn_relay;
typedef struct {
    const char *name;
    int  (*parse)(const char *json, size_t len, netman_vpn_relay *out, int max);            /* pure: relay list -> relays (testable offline) */
    const char *list_url;
    int  (*connect)(const char *profile, const netman_vpn_relay *r, const netman_vpn_relay *via); /* writes <profile>.conf, `via` = multihop entry or NULL */
} netman_vpn_provider;
extern const netman_vpn_provider netman_vpn_providers[];
NETMAN_VPN_API int  netman_vpn_provider_list(const char *provider, const char *country_or_null);  /* fetch (curl) + print */
NETMAN_VPN_API int  netman_vpn_provider_pick(const char *provider, const char *country, const char *city, int fastest, netman_vpn_relay *out);
NETMAN_VPN_API int  netman_vpn_provider_connect(const char *provider, const char *profile, const char *country, const char *city, int fastest, const char *via_country);
NETMAN_VPN_API int  netman_vpn_mullvad_parse(const char *json, size_t len, netman_vpn_relay *out, int max);
NETMAN_VPN_API int  netman_vpn_nord_parse(const char *json, size_t len, netman_vpn_relay *out, int max);
/* argv-style front end: up|down|status|list|logs|ks|run|watch|probe|genkey|pubkey|providers|connect */
NETMAN_VPN_API int  netman_vpn_cmd(int argc, char **argv);

#ifdef __cplusplus
}
#endif
#endif /* NETMAN_VPN_H */

#ifdef NETMAN_VPN_IMPLEMENTATION
#ifndef NETMAN_VPN_IMPLEMENTATION_ONCE
#define NETMAN_VPN_IMPLEMENTATION_ONCE
#ifndef _GNU_SOURCE
#error "define _GNU_SOURCE before the first system include when using NETMAN_VPN_IMPLEMENTATION"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <time.h>
#include <dirent.h>
#include <poll.h>
#include <limits.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/random.h>
#include <sys/mount.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/genetlink.h>
#include <linux/if_link.h>
#include <linux/fib_rules.h>
#include <linux/icmp.h>

FILE *netman_vpn_out, *netman_vpn_err;
const char *netman_vpn_conf_dir = "/etc/netman/vpn";
const char *netman_vpn_run_dir = "/run/netman/vpn";
const char *netman_vpn_resolv_path = "/etc/resolv.conf";

#define NV_OUT (netman_vpn_out ? netman_vpn_out : stdout)
#define NV_ERR (netman_vpn_err ? netman_vpn_err : stderr)

static void netman_vpn__say(const char *f, ...) { va_list a; va_start(a, f); vfprintf(NV_OUT, f, a); va_end(a); fputc('\n', NV_OUT); }
static int netman_vpn__mute;
static int netman_vpn__fail(const char *f, ...) { if (netman_vpn__mute) return 1; va_list a; va_start(a, f); fputs("netman vpn: ", NV_ERR); vfprintf(NV_ERR, f, a); va_end(a); fputc('\n', NV_ERR); return 1; }

static void netman_vpn__wipe(void *p, size_t n) { explicit_bzero(p, n); }

/* ---------------------------------------------------------------- base64 + curve25519 (TweetNaCl scalarmult) */
static const char netman_vpn__b64c[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

NETMAN_VPN_API void netman_vpn_b64_encode32(const unsigned char in[32], char out[45]) {
    int o = 0;
    for (int i = 0; i < 32; i += 3) {
        unsigned v = in[i] << 16 | (i + 1 < 32 ? in[i + 1] << 8 : 0) | (i + 2 < 32 ? in[i + 2] : 0);
        out[o++] = netman_vpn__b64c[v >> 18 & 63]; out[o++] = netman_vpn__b64c[v >> 12 & 63];
        out[o++] = i + 1 < 32 ? netman_vpn__b64c[v >> 6 & 63] : '='; out[o++] = i + 2 < 32 ? netman_vpn__b64c[v & 63] : '=';
    }
    out[o] = 0;
}

NETMAN_VPN_API int netman_vpn_b64_decode32(const char *s, unsigned char out[32]) {
    if (strlen(s) != 44 || s[43] != '=') return -1;
    unsigned char buf[33]; int n = 0; unsigned acc = 0; int bits = 0;
    for (int i = 0; i < 43; i++) {
        const char *p = memchr(netman_vpn__b64c, s[i], 64);
        if (!p || !s[i]) return -1;
        acc = acc << 6 | (unsigned)(p - netman_vpn__b64c); bits += 6;
        if (bits >= 8) { bits -= 8; buf[n++] = (unsigned char)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    if (n != 32 || acc != 0) return -1;      /* canonical: trailing bits must be zero */
    memcpy(out, buf, 32);
    return 0;
}

typedef long long netman_vpn__gf[16];
static const netman_vpn__gf netman_vpn__121665 = {0xDB41, 1};
static void netman_vpn__car(netman_vpn__gf o) {
    for (int i = 0; i < 16; i++) { long long c; o[i] += 1LL << 16; c = o[i] >> 16; o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15); o[i] -= c * 65536LL; }
}
static void netman_vpn__sel(netman_vpn__gf p, netman_vpn__gf q, int b) { long long c = ~(long long)(b - 1); for (int i = 0; i < 16; i++) { long long t = c & (p[i] ^ q[i]); p[i] ^= t; q[i] ^= t; } }
static void netman_vpn__pack(unsigned char *o, const netman_vpn__gf n) {
    netman_vpn__gf m, t; int b;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    netman_vpn__car(t); netman_vpn__car(t); netman_vpn__car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) { m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1); m[i - 1] &= 0xffff; }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1); m[14] &= 0xffff;
        netman_vpn__sel(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) { o[2 * i] = t[i] & 0xff; o[2 * i + 1] = (unsigned char)(t[i] >> 8); }
}
static void netman_vpn__unpack(netman_vpn__gf o, const unsigned char *n) { for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((long long)n[2 * i + 1] << 8); o[15] &= 0x7fff; }
static void netman_vpn__A(netman_vpn__gf o, const netman_vpn__gf a, const netman_vpn__gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void netman_vpn__Z(netman_vpn__gf o, const netman_vpn__gf a, const netman_vpn__gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }
static void netman_vpn__M(netman_vpn__gf o, const netman_vpn__gf a, const netman_vpn__gf b) {
    long long t[31] = {0};
    for (int i = 0; i < 16; i++) for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    netman_vpn__car(o); netman_vpn__car(o);
}
static void netman_vpn__inv(netman_vpn__gf o, const netman_vpn__gf i) {
    netman_vpn__gf c; for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) { netman_vpn__M(c, c, c); if (a != 2 && a != 4) netman_vpn__M(c, c, i); }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}
static void netman_vpn__scalarmult(unsigned char *q, const unsigned char *n, const unsigned char *p) {
    unsigned char z[32]; long long x[80], r; netman_vpn__gf a, b, c, d, e, f;
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64; z[0] &= 248;
    netman_vpn__unpack(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        netman_vpn__sel(a, b, (int)r); netman_vpn__sel(c, d, (int)r);
        netman_vpn__A(e, a, c); netman_vpn__Z(a, a, c); netman_vpn__A(c, b, d); netman_vpn__Z(b, b, d);
        netman_vpn__M(d, e, e); netman_vpn__M(f, a, a); netman_vpn__M(a, c, a); netman_vpn__M(c, b, e);
        netman_vpn__A(e, a, c); netman_vpn__Z(a, a, c); netman_vpn__M(b, a, a); netman_vpn__Z(c, d, f);
        netman_vpn__M(a, c, netman_vpn__121665); netman_vpn__A(a, a, d); netman_vpn__M(c, c, a); netman_vpn__M(a, d, f);
        netman_vpn__M(d, b, x); netman_vpn__M(b, e, e);
        netman_vpn__sel(a, b, (int)r); netman_vpn__sel(c, d, (int)r);
    }
    for (int i = 0; i < 16; i++) { x[i + 16] = a[i]; x[i + 32] = c[i]; x[i + 48] = b[i]; x[i + 64] = d[i]; }
    netman_vpn__inv(x + 32, x + 32);
    netman_vpn__M(x + 16, x + 16, x + 32);
    netman_vpn__pack(q, x + 16);
    netman_vpn__wipe(z, sizeof z); netman_vpn__wipe(x, sizeof x);
}

NETMAN_VPN_API void netman_vpn_pubkey(const unsigned char priv[32], unsigned char pub[32]) {
    unsigned char base[32] = {9};
    netman_vpn__scalarmult(pub, priv, base);
}

NETMAN_VPN_API int netman_vpn_genkey(unsigned char priv[32]) {
    size_t got = 0;
    while (got < 32) { ssize_t r = getrandom(priv + got, 32 - got, 0); if (r < 0) { if (errno == EINTR) continue; return -1; } got += (size_t)r; }
    priv[0] &= 248; priv[31] = (priv[31] & 127) | 64;
    return 0;
}

/* ---------------------------------------------------------------- small helpers */
static int netman_vpn__push(void **arr, size_t *n, size_t esz, const void *el) {
    void *p = realloc(*arr, (*n + 1) * esz);
    if (!p) return -1;
    *arr = p; memcpy((char *)p + *n * esz, el, esz); (*n)++;
    return 0;
}
static int netman_vpn__spush(netman_vpn_strs *s, const char *v) {
    char *d = strdup(v);
    if (!d) return -1;
    if (netman_vpn__push((void **)&s->v, &s->n, sizeof(char *), &d)) { free(d); return -1; }
    return 0;
}
static void netman_vpn__sfree(netman_vpn_strs *s) { for (size_t i = 0; i < s->n; i++) free(s->v[i]); free(s->v); s->v = NULL; s->n = 0; }
static char *netman_vpn__trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}
static char *netman_vpn__tok(char **sp) {          /* next comma separated, trimmed item; NULL at end */
    if (!*sp) return NULL;
    char *s = *sp, *c = strchr(s, ',');
    if (c) { *c = 0; *sp = c + 1; } else *sp = NULL;
    return netman_vpn__trim(s);
}
static int netman_vpn__ieq(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

NETMAN_VPN_API int netman_vpn_valid_name(const char *n, size_t maxlen) {
    size_t l = n ? strlen(n) : 0;
    if (l == 0 || l > maxlen) return 0;
    if (n[0] == '.' ) return 0;
    for (size_t i = 0; i < l; i++) if (!(isalnum((unsigned char)n[i]) || strchr("_=+.-", n[i]))) return 0;
    return 1;
}

static int netman_vpn__ip_parse(const char *s, netman_vpn_ip *o) {
    char buf[64]; const char *sl = strchr(s, '/');
    size_t l = sl ? (size_t)(sl - s) : strlen(s);
    if (l == 0 || l >= sizeof buf) return -1;
    memcpy(buf, s, l); buf[l] = 0;
    memset(o, 0, sizeof *o);
    if (strchr(buf, ':')) { if (inet_pton(AF_INET6, buf, o->addr) != 1) return -1; o->family = AF_INET6; o->prefix = 128; }
    else { if (inet_pton(AF_INET, buf, o->addr) != 1) return -1; o->family = AF_INET; o->prefix = 32; }
    if (sl) {
        char *e; long p = strtol(sl + 1, &e, 10);
        if (e == sl + 1 || *e || p < 0 || p > o->prefix) return -1;
        o->prefix = (int)p;
    }
    return 0;
}
static void netman_vpn__ip_mask(netman_vpn_ip *ip) {
    int alen = ip->family == AF_INET ? 4 : 16;
    for (int i = 0; i < alen; i++) {
        int bits = ip->prefix - i * 8;
        if (bits >= 8) continue;
        ip->addr[i] &= bits <= 0 ? 0 : (unsigned char)(0xff << (8 - bits));
    }
}
static const char *netman_vpn__ip_str(const netman_vpn_ip *ip, char *buf, size_t n) { return inet_ntop(ip->family, ip->addr, buf, (socklen_t)n); }

static int netman_vpn__ep_resolve(const char *ep, struct sockaddr_storage *ss, socklen_t *sl) {
    netman_wg_ep e; if (netman_wg_resolve(ep, &e)) return -1;
    *ss = e.ss; *sl = e.sl; return 0;
}

/* ---------------------------------------------------------------- wg-quick config parser */
NETMAN_VPN_API void netman_vpn_wg_free(netman_vpn_wg *c) {
    if (!c) return;
    for (size_t i = 0; i < c->npeers; i++) { free(c->peers[i].endpoint); free(c->peers[i].allowed); netman_vpn__wipe(c->peers[i].psk, 32); }
    free(c->peers); free(c->addr);
    netman_vpn__sfree(&c->dns); netman_vpn__sfree(&c->search); netman_vpn__sfree(&c->pre_up); netman_vpn__sfree(&c->post_up); netman_vpn__sfree(&c->pre_down); netman_vpn__sfree(&c->post_down);
    netman_vpn__wipe(c->priv, 32);
    memset(c, 0, sizeof *c);
}

static int netman_vpn__perr(char *err, size_t n, int line, const char *msg) { if (err && n) snprintf(err, n, "line %d: %s", line, msg); return -1; }
static int netman_vpn__uint(const char *s, unsigned long max, unsigned long *out) {
    char *e; if (!*s || !isdigit((unsigned char)*s)) return -1;
    errno = 0; unsigned long v = strtoul(s, &e, 0);
    if (*e || errno || v > max) return -1;
    *out = v; return 0;
}
static int netman_vpn__domain_ok(const char *s) {
    size_t l = strlen(s);
    if (l == 0 || l > 253) return 0;
    for (; *s; s++) if (!(isalnum((unsigned char)*s) || *s == '.' || *s == '-' || *s == '_')) return 0;
    return 1;
}

NETMAN_VPN_API int netman_vpn_wg_parse(const char *text, size_t len, netman_vpn_wg *c, char *err, size_t errsz) {
    memset(c, 0, sizeof *c);
    if (len > (256u << 10)) return netman_vpn__perr(err, errsz, 0, "config too large");
    if (memchr(text, 0, len)) return netman_vpn__perr(err, errsz, 0, "NUL byte in config");
    int sec = 0, line = 0, seen_if = 0, peer_line = 0; netman_vpn_peer *p = NULL;
    size_t pos = 0;
    while (pos < len) {
        size_t e = pos; while (e < len && text[e] != '\n') e++;
        char buf[4100]; size_t ll = e - pos;
        line++;
        if (ll >= sizeof buf - 1) { netman_vpn_wg_free(c); return netman_vpn__perr(err, errsz, line, "line too long"); }
        memcpy(buf, text + pos, ll); buf[ll] = 0; pos = e + 1;
        char *hash = strchr(buf, '#'); if (hash) *hash = 0;
        char *s = netman_vpn__trim(buf);
        if (!*s) continue;
        const char *msg = NULL;
        if (*s == '[') {
            size_t sl = strlen(s);
            if (s[sl - 1] != ']') { msg = "bad section"; goto bad; }
            s[sl - 1] = 0; s++;
            if (netman_vpn__ieq(s, "Interface")) { if (seen_if) { msg = "second [Interface]"; goto bad; } seen_if = 1; sec = 1; p = NULL; }
            else if (netman_vpn__ieq(s, "Peer")) {
                netman_vpn_peer np; memset(&np, 0, sizeof np);
                if (netman_vpn__push((void **)&c->peers, &c->npeers, sizeof np, &np)) { msg = "out of memory"; goto bad; }
                p = &c->peers[c->npeers - 1]; sec = 2; peer_line = line;
                if (c->npeers > 1) { /* previous peer must have a key */
                    int zero = 1; for (int i = 0; i < 32; i++) if (c->peers[c->npeers - 2].pub[i]) zero = 0;
                    if (zero) { line = peer_line - 1; msg = "peer without PublicKey"; goto bad; }
                }
            } else { msg = "unknown section"; goto bad; }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq || !sec) { msg = sec ? "expected key = value" : "key outside a section"; goto bad; }
        *eq = 0;
        char *k = netman_vpn__trim(s), *v = netman_vpn__trim(eq + 1);
        if (sec == 1) {
            unsigned long u;
            if (netman_vpn__ieq(k, "PrivateKey")) { if (netman_vpn_b64_decode32(v, c->priv)) { msg = "PrivateKey is not a valid key"; goto bad; } c->has_priv = 1; }
            else if (netman_vpn__ieq(k, "Address")) {
                char *it, *sp = v;
                while ((it = netman_vpn__tok(&sp))) { netman_vpn_ip ip; if (!*it) continue; if (netman_vpn__ip_parse(it, &ip)) { msg = "bad Address"; goto bad; } if (netman_vpn__push((void **)&c->addr, &c->naddr, sizeof ip, &ip)) { msg = "out of memory"; goto bad; } }
            }
            else if (netman_vpn__ieq(k, "DNS")) {
                char *it, *sp = v;
                while ((it = netman_vpn__tok(&sp))) {
                    netman_vpn_ip ip; if (!*it) continue;
                    if (!strchr(it, '/') && netman_vpn__ip_parse(it, &ip) == 0) { if (netman_vpn__spush(&c->dns, it)) { msg = "out of memory"; goto bad; } }
                    else if (netman_vpn__domain_ok(it)) { if (netman_vpn__spush(&c->search, it)) { msg = "out of memory"; goto bad; } }
                    else { msg = "bad DNS entry"; goto bad; }
                }
            }
            else if (netman_vpn__ieq(k, "MTU")) { if (netman_vpn__uint(v, 65535, &u) || u < 576) { msg = "bad MTU"; goto bad; } c->mtu = (int)u; }
            else if (netman_vpn__ieq(k, "ListenPort")) { if (netman_vpn__uint(v, 65535, &u)) { msg = "bad ListenPort"; goto bad; } c->listen_port = (int)u; }
            else if (netman_vpn__ieq(k, "FwMark")) { if (netman_vpn__ieq(v, "off")) c->fwmark_off = 1; else { if (netman_vpn__uint(v, 0xffffffffUL, &u) || u == 0) { msg = "bad FwMark"; goto bad; } c->fwmark = (unsigned)u; c->has_fwmark = 1; } }
            else if (netman_vpn__ieq(k, "Table")) {
                if (netman_vpn__ieq(v, "off")) c->table_off = 1;
                else if (netman_vpn__ieq(v, "auto")) c->table = 0;
                else { if (netman_vpn__uint(v, 0x7fffffffUL, &u) || u == 0 || u == 253 || u == 254 || u == 255) { msg = "bad Table"; goto bad; } c->table = (long)u; }
            }
            else if (netman_vpn__ieq(k, "PreUp")) { if (netman_vpn__spush(&c->pre_up, v)) { msg = "out of memory"; goto bad; } }
            else if (netman_vpn__ieq(k, "PostUp")) { if (netman_vpn__spush(&c->post_up, v)) { msg = "out of memory"; goto bad; } }
            else if (netman_vpn__ieq(k, "PreDown")) { if (netman_vpn__spush(&c->pre_down, v)) { msg = "out of memory"; goto bad; } }
            else if (netman_vpn__ieq(k, "PostDown")) { if (netman_vpn__spush(&c->post_down, v)) { msg = "out of memory"; goto bad; } }
            else if (netman_vpn__ieq(k, "SaveConfig")) { }
            else { msg = "unknown [Interface] key"; goto bad; }
        } else {
            unsigned long u;
            if (netman_vpn__ieq(k, "PublicKey")) { if (netman_vpn_b64_decode32(v, p->pub)) { msg = "PublicKey is not a valid key"; goto bad; } }
            else if (netman_vpn__ieq(k, "PresharedKey")) { if (netman_vpn_b64_decode32(v, p->psk)) { msg = "PresharedKey is not a valid key"; goto bad; } p->has_psk = 1; }
            else if (netman_vpn__ieq(k, "Endpoint")) {
                char h[256]; int port;
                if (netman_wg_ep_split(v, h, sizeof h, &port)) { msg = "bad Endpoint"; goto bad; }
                free(p->endpoint); p->endpoint = strdup(v); if (!p->endpoint) { msg = "out of memory"; goto bad; }
            }
            else if (netman_vpn__ieq(k, "AllowedIPs")) {
                char *it, *sp = v;
                while ((it = netman_vpn__tok(&sp))) { netman_vpn_ip ip; if (!*it) continue; if (netman_vpn__ip_parse(it, &ip)) { msg = "bad AllowedIPs"; goto bad; } if (netman_vpn__push((void **)&p->allowed, &p->nallowed, sizeof ip, &ip)) { msg = "out of memory"; goto bad; } }
            }
            else if (netman_vpn__ieq(k, "PersistentKeepalive")) { if (netman_vpn__ieq(v, "off")) p->keepalive = 0; else { if (netman_vpn__uint(v, 65535, &u)) { msg = "bad PersistentKeepalive"; goto bad; } p->keepalive = (int)u; } }
            else { msg = "unknown [Peer] key"; goto bad; }
        }
        continue;
    bad:
        netman_vpn_wg_free(c);
        return netman_vpn__perr(err, errsz, line, msg);
    }
    if (!seen_if) { netman_vpn_wg_free(c); return netman_vpn__perr(err, errsz, 0, "no [Interface] section"); }
    if (c->npeers) {
        int zero = 1; for (int i = 0; i < 32; i++) if (c->peers[c->npeers - 1].pub[i]) zero = 0;
        if (zero) { netman_vpn_wg_free(c); return netman_vpn__perr(err, errsz, 0, "peer without PublicKey"); }
    }
    return 0;
}

/* ---------------------------------------------------------------- file helpers */
static char *netman_vpn__slurp(const char *path, size_t *len, size_t cap, int trust) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size > cap) { close(fd); errno = EFBIG; return NULL; }
    if (trust && ((st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 022))) { close(fd); errno = EACCES; return NULL; }
    size_t want = st.st_size ? (size_t)st.st_size : (cap < 65536 ? cap : 65536);   /* /proc files report size 0 */
    char *b = malloc(want + 1);
    if (!b) { close(fd); return NULL; }
    size_t got = 0;
    while (got < want) { ssize_t r = read(fd, b + got, want - got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    b[got] = 0; if (len) *len = got;
    return b;
}
static int netman_vpn__spit(const char *path, const void *data, size_t n, mode_t mode) {   /* atomic: tmp in same dir + rename */
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.netman vpn.tmp", path) >= (int)sizeof tmp) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
    if (fd < 0) return -1;
    const char *p = data; size_t off = 0;
    while (off < n) { ssize_t w = write(fd, p + off, n - off); if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; } off += (size_t)w; }
    if (fchmod(fd, mode) || fsync(fd) || close(fd)) { unlink(tmp); return -1; }
    if (rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}
static int netman_vpn__mkdirs(const char *path, mode_t mode) {
    char b[PATH_MAX]; snprintf(b, sizeof b, "%s", path);
    for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(b, mode) && errno != EEXIST) return -1; *p = '/'; }
    return (mkdir(b, mode) && errno != EEXIST) ? -1 : 0;
}
static int netman_vpn__rundir(void) { return netman_vpn__mkdirs(netman_vpn_run_dir, 0700); }
static const char *netman_vpn__rp(char *buf, size_t n, const char *name, const char *ext) { snprintf(buf, n, "%s/%s.%s", netman_vpn_run_dir, name, ext); return buf; }

/* ---------------------------------------------------------------- netlink */
typedef struct { unsigned char b[1 << 16]; size_t len; int ovf; } netman_vpn__msg;
static unsigned netman_vpn__seq;

static void *netman_vpn__put(netman_vpn__msg *m, size_t n) {
    size_t a = NLMSG_ALIGN(n);
    if (m->len + a > sizeof m->b) { m->ovf = 1; return NULL; }
    void *p = m->b + m->len; memset(p, 0, a); m->len += a; return p;
}
static void netman_vpn__hdr(netman_vpn__msg *m, int type, int flags, const void *body, size_t bl) {
    m->len = 0; m->ovf = 0;
    struct nlmsghdr *h = netman_vpn__put(m, sizeof *h);
    h->nlmsg_type = (unsigned short)type; h->nlmsg_flags = (unsigned short)(NLM_F_REQUEST | flags);
    void *b = netman_vpn__put(m, bl);
    if (b && body) memcpy(b, body, bl);
}
static void netman_vpn__attr(netman_vpn__msg *m, int type, const void *d, size_t n) {
    struct nlattr *a = netman_vpn__put(m, NLA_HDRLEN + n);
    if (!a) return;
    a->nla_type = (unsigned short)type; a->nla_len = (unsigned short)(NLA_HDRLEN + n);
    if (n) memcpy((char *)a + NLA_HDRLEN, d, n);
}
static void netman_vpn__attr_u32(netman_vpn__msg *m, int t, unsigned v) { netman_vpn__attr(m, t, &v, 4); }
static void netman_vpn__attr_str(netman_vpn__msg *m, int t, const char *s) { netman_vpn__attr(m, t, s, strlen(s) + 1); }

typedef int (*netman_vpn__cb)(const struct nlmsghdr *, void *);
static int netman_vpn__nl_open(int proto) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, proto);
    if (fd < 0) return -1;
    struct sockaddr_nl sa = {.nl_family = AF_NETLINK};
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return -1; }
    return fd;
}
/* returns 0 or -errno */
static int netman_vpn__talk(int fd, netman_vpn__msg *m, netman_vpn__cb cb, void *ud) {
    if (m->ovf) return -E2BIG;
    struct nlmsghdr *h = (struct nlmsghdr *)m->b;
    h->nlmsg_len = (unsigned)m->len; h->nlmsg_seq = ++netman_vpn__seq;
    int flags = h->nlmsg_flags;
    if (send(fd, m->b, m->len, 0) < 0) return -errno;
    unsigned char *rb = malloc(1 << 17);
    if (!rb) return -ENOMEM;
    int rc = 0, done = 0;
    while (!done) {
        ssize_t n = recv(fd, rb, 1 << 17, 0);
        if (n < 0) { if (errno == EINTR) continue; rc = -errno; break; }
        for (struct nlmsghdr *r = (struct nlmsghdr *)rb; NLMSG_OK(r, (unsigned)n); r = NLMSG_NEXT(r, n)) {
            if (r->nlmsg_seq != h->nlmsg_seq) continue;
            if (r->nlmsg_type == NLMSG_ERROR) { rc = ((struct nlmsgerr *)NLMSG_DATA(r))->error; done = 1; break; }
            if (r->nlmsg_type == NLMSG_DONE) { done = 1; break; }
            if (cb) cb(r, ud);
            if (!(flags & NLM_F_ROOT) && !(flags & NLM_F_ACK)) done = 1;
        }
    }
    free(rb);
    return rc;
}
#define NV_DUMP (NLM_F_ROOT | NLM_F_MATCH)

static int netman_vpn__rt_link_del(int fd, int idx) {
    netman_vpn__msg *m = malloc(sizeof *m); if (!m) return -ENOMEM;
    struct ifinfomsg ifi = {.ifi_family = AF_UNSPEC, .ifi_index = idx};
    netman_vpn__hdr(m, RTM_DELLINK, NLM_F_ACK, &ifi, sizeof ifi);
    int r = netman_vpn__talk(fd, m, NULL, NULL); free(m); return r;
}
static int netman_vpn__rt_link_set(int fd, int idx, int up, int mtu) {
    netman_vpn__msg *m = malloc(sizeof *m); if (!m) return -ENOMEM;
    struct ifinfomsg ifi = {.ifi_family = AF_UNSPEC, .ifi_index = idx};
    if (up) { ifi.ifi_flags = IFF_UP; ifi.ifi_change = IFF_UP; }
    netman_vpn__hdr(m, RTM_NEWLINK, NLM_F_ACK, &ifi, sizeof ifi);
    if (mtu > 0) netman_vpn__attr_u32(m, IFLA_MTU, (unsigned)mtu);
    int r = netman_vpn__talk(fd, m, NULL, NULL); free(m); return r;
}
static int netman_vpn__rt_addr(int fd, int idx, const netman_vpn_ip *a) {
    netman_vpn__msg *m = malloc(sizeof *m); if (!m) return -ENOMEM;
    struct ifaddrmsg ifa = {.ifa_family = (unsigned char)a->family, .ifa_prefixlen = (unsigned char)a->prefix, .ifa_scope = RT_SCOPE_UNIVERSE, .ifa_index = (unsigned)idx};
    int al = a->family == AF_INET ? 4 : 16;
    netman_vpn__hdr(m, RTM_NEWADDR, NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE, &ifa, sizeof ifa);
    netman_vpn__attr(m, IFA_LOCAL, a->addr, (size_t)al);
    netman_vpn__attr(m, IFA_ADDRESS, a->addr, (size_t)al);
    int r = netman_vpn__talk(fd, m, NULL, NULL); free(m); return r;
}
static int netman_vpn__rt_route(int fd, int idx, const netman_vpn_ip *d0, unsigned table) {
    netman_vpn__msg *m = malloc(sizeof *m); if (!m) return -ENOMEM;
    netman_vpn_ip d = *d0; netman_vpn__ip_mask(&d);
    struct rtmsg rm = {.rtm_family = (unsigned char)d.family, .rtm_dst_len = (unsigned char)d.prefix, .rtm_table = table < 256 ? (unsigned char)table : RT_TABLE_COMPAT,
                       .rtm_protocol = RTPROT_BOOT, .rtm_scope = RT_SCOPE_LINK, .rtm_type = RTN_UNICAST};
    netman_vpn__hdr(m, RTM_NEWROUTE, NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE, &rm, sizeof rm);
    if (d.prefix > 0) netman_vpn__attr(m, RTA_DST, d.addr, d.family == AF_INET ? 4 : 16);
    netman_vpn__attr_u32(m, RTA_OIF, (unsigned)idx);
    netman_vpn__attr_u32(m, RTA_TABLE, table);
    int r = netman_vpn__talk(fd, m, NULL, NULL); free(m); return r;
}
/* kind 0: "not fwmark MARK lookup TABLE"; kind 1: "lookup main suppress_prefixlength 0" */
static int netman_vpn__rt_rule(int fd, int family, int kind, unsigned mark, unsigned table, int del) {
    netman_vpn__msg *m = malloc(sizeof *m); if (!m) return -ENOMEM;
    struct fib_rule_hdr fr = {.family = (unsigned char)family, .action = FR_ACT_TO_TBL};
    unsigned t = kind ? 254 : table;
    fr.table = t < 256 ? (unsigned char)t : RT_TABLE_COMPAT;
    if (kind == 0) fr.flags = FIB_RULE_INVERT;
    netman_vpn__hdr(m, del ? RTM_DELRULE : RTM_NEWRULE, NLM_F_ACK | (del ? 0 : (NLM_F_CREATE | NLM_F_EXCL)), &fr, sizeof fr);
    if (kind == 0) netman_vpn__attr_u32(m, FRA_FWMARK, mark);
    netman_vpn__attr_u32(m, FRA_TABLE, t);
    if (kind == 1) netman_vpn__attr_u32(m, FRA_SUPPRESS_PREFIXLEN, 0);
    int r = netman_vpn__talk(fd, m, NULL, NULL); free(m); return r;
}

/* the kernel side of WireGuard lives in wireguard.c; this adapts a parsed profile to it */
typedef netman_wg_ep netman_vpn__ep;
static int netman_vpn__wg_set(const char *ifname, const netman_vpn_wg *c, const netman_vpn__ep *eps, int fwmark_set, unsigned fwmark) {
    netman_wg_peer *pv = calloc(c->npeers ? c->npeers : 1, sizeof *pv);
    if (!pv) return -ENOMEM;
    for (size_t i = 0; i < c->npeers; i++) {
        const netman_vpn_peer *p = &c->peers[i];
        memcpy(pv[i].pub, p->pub, 32); memcpy(pv[i].psk, p->psk, 32); pv[i].has_psk = p->has_psk;
        if (p->endpoint) { pv[i].has_ep = 1; pv[i].ep = eps[i]; }
        pv[i].keepalive = p->keepalive; pv[i].allowed = p->allowed; pv[i].nallowed = p->nallowed;
    }
    netman_wg_conf wc; memset(&wc, 0, sizeof wc);
    memcpy(wc.priv, c->priv, 32); wc.has_priv = c->has_priv; wc.listen_port = c->listen_port;
    wc.has_fwmark = fwmark_set; wc.fwmark = fwmark; wc.peers = pv; wc.npeers = c->npeers;
    int r = netman_wg_set(ifname, &wc);
    netman_vpn__wipe(&wc, sizeof wc); netman_vpn__wipe(pv, c->npeers * sizeof *pv); free(pv);
    return r;
}
NETMAN_VPN_API int netman_vpn_wg_stats(const char *ifname, netman_vpn_wgstat *st) { return netman_wg_stats(ifname, st); }

/* ---------------------------------------------------------------- resolv.conf with backup / restore */
static int netman_vpn__dns_set(const char *name, const netman_vpn_wg *c) {
    if (!c->dns.n && !c->search.n) return 0;
    char p1[PATH_MAX], marker[128];
    struct stat st;
    snprintf(marker, sizeof marker, "# netman_vpn:%s\n", name);
    if (lstat(netman_vpn_resolv_path, &st) == 0 && S_ISLNK(st.st_mode)) {
        char t[PATH_MAX]; ssize_t n = readlink(netman_vpn_resolv_path, t, sizeof t - 1);
        if (n < 0) return -1;
        if (netman_vpn__spit(netman_vpn__rp(p1, sizeof p1, name, "resolv.link"), t, (size_t)n, 0600)) return -1;
    } else if (errno == ENOENT || stat(netman_vpn_resolv_path, &st) != 0) {
        if (netman_vpn__spit(netman_vpn__rp(p1, sizeof p1, name, "resolv.none"), "", 0, 0600)) return -1;
    } else {
        size_t l; char *old = netman_vpn__slurp(netman_vpn_resolv_path, &l, 1 << 20, 0);
        if (!old) return -1;
        int r = netman_vpn__spit(netman_vpn__rp(p1, sizeof p1, name, "resolv.bak"), old, l, 0600);
        free(old); if (r) return -1;
    }
    size_t cap = 256; for (size_t i = 0; i < c->dns.n; i++) cap += strlen(c->dns.v[i]) + 16;
    for (size_t i = 0; i < c->search.n; i++) cap += strlen(c->search.v[i]) + 2;
    char *buf = malloc(cap); if (!buf) return -1;
    size_t o = (size_t)snprintf(buf, cap, "%s", marker);
    if (c->search.n) { o += (size_t)snprintf(buf + o, cap - o, "search"); for (size_t i = 0; i < c->search.n; i++) o += (size_t)snprintf(buf + o, cap - o, " %s", c->search.v[i]); o += (size_t)snprintf(buf + o, cap - o, "\n"); }
    for (size_t i = 0; i < c->dns.n; i++) o += (size_t)snprintf(buf + o, cap - o, "nameserver %s\n", c->dns.v[i]);
    int r = netman_vpn__spit(netman_vpn_resolv_path, buf, o, 0644);
    free(buf);
    return r;
}
static int netman_vpn__dns_restore(const char *name) {
    char pl[PATH_MAX], pb[PATH_MAX], pn[PATH_MAX], marker[128];
    netman_vpn__rp(pl, sizeof pl, name, "resolv.link"); netman_vpn__rp(pb, sizeof pb, name, "resolv.bak"); netman_vpn__rp(pn, sizeof pn, name, "resolv.none");
    int have_l = access(pl, F_OK) == 0, have_b = access(pb, F_OK) == 0, have_n = access(pn, F_OK) == 0;
    if (!have_l && !have_b && !have_n) return 0;
    snprintf(marker, sizeof marker, "# netman_vpn:%s\n", name);
    size_t l; char *cur = netman_vpn__slurp(netman_vpn_resolv_path, &l, 1 << 20, 0);
    int ours = cur && strncmp(cur, marker, strlen(marker)) == 0;
    free(cur);
    int r = 0;
    if (!ours) { netman_vpn__fail("%s was changed by someone else, not restoring it", netman_vpn_resolv_path); r = 1; }
    else if (have_l) { size_t tl; char *t = netman_vpn__slurp(pl, &tl, PATH_MAX, 0); if (!t) r = 1; else { unlink(netman_vpn_resolv_path); if (symlink(t, netman_vpn_resolv_path)) r = 1; free(t); } }
    else if (have_b) { size_t bl; char *b = netman_vpn__slurp(pb, &bl, 1 << 20, 0); if (!b || netman_vpn__spit(netman_vpn_resolv_path, b, bl, 0644)) r = 1; free(b); }
    else if (unlink(netman_vpn_resolv_path)) r = 1;
    unlink(pl); unlink(pb); unlink(pn);
    return r;
}

/* ---------------------------------------------------------------- hooks */
static int netman_vpn__hooks(const netman_vpn_strs *h, const char *ifname) {
    for (size_t i = 0; i < h->n; i++) {
        char cmd[4096]; size_t o = 0; const char *s = h->v[i];
        for (; *s && o + 64 < sizeof cmd; s++) {
            if (s[0] == '%' && s[1] == 'i') { o += (size_t)snprintf(cmd + o, sizeof cmd - o, "%s", ifname); s++; }
            else cmd[o++] = *s;
        }
        cmd[o] = 0;
        pid_t pid = fork();
        if (pid < 0) return -1;
        if (pid == 0) { execl("/bin/sh", "sh", "-c", cmd, (char *)NULL); _exit(127); }
        int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return netman_vpn__fail("hook failed: %s", h->v[i]);
    }
    return 0;
}

/* ---------------------------------------------------------------- profiles */
static int netman_vpn__detect(const char *t) {
    for (const char *p = t; *p; ) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && isspace((unsigned char)*p)) { p++; l--; }
        if (l >= 11 && strncasecmp(p, "[interface]", 11) == 0) return NETMAN_VPN_WG;
        if (!e) break;
        p = e + 1;
    }
    const netman_vpnmod *const *m = netman_vpnmods();
    for (int i = 0; m[i]; i++) if (m[i]->detect(t)) return i + 2;
    return 0;
}
static const netman_vpnmod *netman_vpn__mod(int ty) { const netman_vpnmod *const *m = netman_vpnmods(); for (int i = 0; m[i]; i++) if (i + 2 == ty) return m[i]; return NULL; }
int (*netman_vpn_enabled_cb)(const char *type);
static netman_vm_ctx netman_vpn__ctx(void) { netman_vm_ctx c; c.conf_dir = netman_vpn_conf_dir; c.run_dir = netman_vpn_run_dir; c.out = NV_OUT; c.err = NV_ERR; c.mute = netman_vpn__mute; return c; }
static const char *netman_vpn__tyname(int ty) { const netman_vpnmod *m = netman_vpn__mod(ty); return ty == NETMAN_VPN_WG ? "wireguard" : m ? m->type : "?"; }
static const char *netman_vpn__mstate(const char *name, int ty) { netman_vm_ctx c = netman_vpn__ctx(); const netman_vpnmod *m = netman_vpn__mod(ty); return m ? m->state(&c, name) : "down"; }
/* PreUp / PostUp / PreDown / PostDown of a NAME.vpn profile (same %i = profile name rule as WireGuard); other profile kinds have none */
static int netman_vpn__mod_hooks(const char *text, const char *key, const char *name) {
    netman_vm_prof p; char e[64]; netman_vpn_strs h = {0}; int rc = 0;
    if (!netman_vm_native(text, NULL, 0) || netman_vm_parse(text, &p, e, sizeof e)) return 0;
    size_t it = 0; const char *v;
    while (netman_vm_next(&p, key, &it, &v)) netman_vpn__spush(&h, v);
    if (h.n) rc = netman_vpn__hooks(&h, name);
    netman_vpn__sfree(&h); netman_vm_free(&p); return rc;
}
/* loads NAME.conf / NAME.ovpn from the profile dir; trust-checked. *type 0 when missing. */
static char *netman_vpn__load(const char *name, size_t *len, int *type, char *path, size_t pn) {
    static const char *ext[] = {"conf", "ovpn", "vpn"};
    *type = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(path, pn, "%s/%s.%s", netman_vpn_conf_dir, name, ext[i]);
        char *t = netman_vpn__slurp(path, len, 256u << 10, 1);
        if (t) { *type = netman_vpn__detect(t); if (!*type) { free(t); netman_vpn__fail("%s: cannot tell the VPN type (WireGuard [Interface], OpenVPN remote/client, or Type = ... in a .vpn file)", path); return NULL; } return t; }
        if (errno == EACCES) { netman_vpn__fail("%s must be owned by root (or you) and not group/world writable", path); return NULL; }
        if (errno != ENOENT) { netman_vpn__fail("%s: %s", path, strerror(errno)); return NULL; }
    }
    netman_vpn__fail("no profile %s in %s", name, netman_vpn_conf_dir);
    return NULL;
}
NETMAN_VPN_API const char *netman_vpn_type_name(const char *name) {
    if (!netman_vpn_valid_name(name, 32)) return NULL;
    int t = netman_vpn_type(name); return t ? netman_vpn__tyname(t) : NULL;
}
NETMAN_VPN_API int netman_vpn_type(const char *name) {
    char path[PATH_MAX]; size_t l; int ty;
    if (!netman_vpn_valid_name(name, 32)) return 0;
    char *t = netman_vpn__load(name, &l, &ty, path, sizeof path);
    free(t); return ty;
}

/* ---------------------------------------------------------------- WireGuard up / down */
typedef struct { unsigned fwmark, table; int rule4, rule6, dns; } netman_vpn__state;
static void netman_vpn__state_save(const char *name, const netman_vpn__state *s) {
    char p[PATH_MAX], b[160]; netman_vpn__rp(p, sizeof p, name, "state");
    int n = snprintf(b, sizeof b, "fwmark=%u\ntable=%u\nrule4=%d\nrule6=%d\ndns=%d\n", s->fwmark, s->table, s->rule4, s->rule6, s->dns);
    netman_vpn__spit(p, b, (size_t)n, 0600);
}
static int netman_vpn__state_load(const char *name, netman_vpn__state *s) {
    char p[PATH_MAX]; memset(s, 0, sizeof *s); netman_vpn__rp(p, sizeof p, name, "state");
    char *t = netman_vpn__slurp(p, NULL, 4096, 0); if (!t) return -1;
    sscanf(t, "fwmark=%u\ntable=%u\nrule4=%d\nrule6=%d\ndns=%d", &s->fwmark, &s->table, &s->rule4, &s->rule6, &s->dns);
    free(t); return 0;
}
/* asks the kernel (rtnetlink) for the link kind, so it is right in any network namespace (sysfs may show another one) */
static int netman_vpn__is_wg_link(const char *name) { return netman_wg_is_link(name); }
static unsigned netman_vpn__hashmark(const char *n) { unsigned h = 2166136261u; for (; *n; n++) h = (h ^ (unsigned char)*n) * 16777619u; return 51820 + h % 4096; }

/* undo everything we did for NAME; tolerant of partial state */
static int netman_vpn__wg_teardown(const char *name) {
    int rc = 0; netman_vpn__state s;
    int have = netman_vpn__state_load(name, &s) == 0;
    int fd = netman_vpn__nl_open(NETLINK_ROUTE);
    if (fd < 0) return netman_vpn__fail("netlink: %s", strerror(errno));
    if (have && (s.rule4 || s.rule6)) {
        for (int f = 0; f < 2; f++) {
            if (!(f ? s.rule6 : s.rule4)) continue;
            int fam = f ? AF_INET6 : AF_INET;
            int r1 = netman_vpn__rt_rule(fd, fam, 0, s.fwmark, s.table, 1), r2 = netman_vpn__rt_rule(fd, fam, 1, 0, 0, 1);
            if (r1 && r1 != -ENOENT) rc = netman_vpn__fail("delete rule: %s", strerror(-r1));
            if (r2 && r2 != -ENOENT) rc = netman_vpn__fail("delete rule: %s", strerror(-r2));
        }
    }
    if (netman_vpn__dns_restore(name)) rc = 1;
    int idx = (int)if_nametoindex(name);
    if (idx) { int r = netman_vpn__rt_link_del(fd, idx); if (r) rc = netman_vpn__fail("delete link %s: %s", name, strerror(-r)); }
    close(fd);
    char p[PATH_MAX]; unlink(netman_vpn__rp(p, sizeof p, name, "state"));
    return rc;
}

static int netman_vpn__has_default(const netman_vpn_wg *c, int fam) {
    for (size_t i = 0; i < c->npeers; i++) for (size_t j = 0; j < c->peers[i].nallowed; j++) if (c->peers[i].allowed[j].family == fam && c->peers[i].allowed[j].prefix == 0) return 1;
    return 0;
}

static int netman_vpn__wg_up(const char *name, const char *text, size_t len) {
    netman_vpn_wg c; char err[128];
    if (!netman_vpn_valid_name(name, 15)) return netman_vpn__fail("interface name %s is invalid (1-15 chars of A-Za-z0-9_=+.-)", name);
    if (netman_vpn_wg_parse(text, len, &c, err, sizeof err)) return netman_vpn__fail("%s: %s", name, err);
    int rc = 1, fd = -1, linked = 0; netman_vpn__ep *eps = NULL; netman_vpn__state st; memset(&st, 0, sizeof st);
    if (!c.has_priv) { netman_vpn__fail("%s: no PrivateKey", name); goto out; }
    if (if_nametoindex(name)) { netman_vpn__fail("%s is already up (or the name is taken)", name); goto out; }
    netman_vpn__wg_teardown(name);          /* leftovers of a tunnel that died without `down` (rules, DNS backup, state) */
    int d4 = netman_vpn__has_default(&c, AF_INET), d6 = netman_vpn__has_default(&c, AF_INET6);
    unsigned fwmark = c.has_fwmark ? c.fwmark : netman_vpn__hashmark(name);
    int fw_set = !c.fwmark_off;
    unsigned table = c.table ? (unsigned)c.table : fwmark;
    if ((d4 || d6) && !c.table_off && !fw_set) { netman_vpn__fail("%s: default route with FwMark = off cannot be routed safely", name); goto out; }
    if (table == 0 || table == 253 || table == 254 || table == 255) { netman_vpn__fail("%s: routing table %u is reserved", name, table); goto out; }
    eps = calloc(c.npeers ? c.npeers : 1, sizeof *eps);
    if (!eps) { netman_vpn__fail("out of memory"); goto out; }
    for (size_t i = 0; i < c.npeers; i++) if (c.peers[i].endpoint && netman_vpn__ep_resolve(c.peers[i].endpoint, &eps[i].ss, &eps[i].sl)) { netman_vpn__fail("%s: cannot resolve a peer endpoint", name); goto out; }
    if (netman_vpn__rundir()) { netman_vpn__fail("%s: %s%s", netman_vpn_run_dir, strerror(errno), errno == EACCES || errno == EPERM || errno == EROFS ? " (vpn up/down needs root)" : ""); goto out; }
    if (netman_vpn__hooks(&c.pre_up, name)) goto out;
    fd = netman_vpn__nl_open(NETLINK_ROUTE);
    if (fd < 0) { netman_vpn__fail("netlink: %s", strerror(errno)); goto out; }
    int r = netman_wg_link_add(name);
    if (r) { netman_vpn__fail("create wireguard link: %s%s", strerror(-r), r == -EOPNOTSUPP ? " (kernel without wireguard?)" : r == -EPERM ? " (need root / CAP_NET_ADMIN)" : ""); goto out; }
    linked = 1; st.fwmark = fwmark; st.table = table; netman_vpn__state_save(name, &st);
    r = netman_vpn__wg_set(name, &c, eps, fw_set, fwmark);
    if (r) { netman_vpn__fail("configure wireguard: %s", strerror(-r)); goto out; }
    int idx = (int)if_nametoindex(name);
    for (size_t i = 0; i < c.naddr; i++) if ((r = netman_vpn__rt_addr(fd, idx, &c.addr[i]))) { netman_vpn__fail("add address: %s", strerror(-r)); goto out; }
    if ((r = netman_vpn__rt_link_set(fd, idx, 1, c.mtu ? c.mtu : 1420))) { netman_vpn__fail("bring link up: %s", strerror(-r)); goto out; }
    if (!c.table_off) {
        for (size_t i = 0; i < c.npeers; i++) for (size_t j = 0; j < c.peers[i].nallowed; j++) {
            const netman_vpn_ip *a = &c.peers[i].allowed[j];
            unsigned t = a->prefix == 0 ? table : 254;
            if ((r = netman_vpn__rt_route(fd, idx, a, t))) { netman_vpn__fail("add route: %s", strerror(-r)); goto out; }
        }
        for (int f = 0; f < 2; f++) {
            if (!(f ? d6 : d4)) continue;
            int famx = f ? AF_INET6 : AF_INET;
            if ((r = netman_vpn__rt_rule(fd, famx, 0, fwmark, table, 0)) || (r = netman_vpn__rt_rule(fd, famx, 1, 0, 0, 0))) { netman_vpn__fail("add policy rule: %s", strerror(-r)); goto out; }
            if (f) st.rule6 = 1; else st.rule4 = 1;
            netman_vpn__state_save(name, &st);
        }
    }
    if (c.dns.n || c.search.n) { if (netman_vpn__dns_set(name, &c)) { netman_vpn__fail("set DNS: %s", strerror(errno)); goto out; } st.dns = 1; netman_vpn__state_save(name, &st); }
    if (netman_vpn__hooks(&c.post_up, name)) goto out;
    netman_vpn__say("%s: up (%zu peer%s)", name, c.npeers, c.npeers == 1 ? "" : "s");
    rc = 0;
out:
    if (rc && linked) netman_vpn__wg_teardown(name);
    if (fd >= 0) close(fd);
    free(eps); netman_vpn_wg_free(&c);
    return rc;
}

static int netman_vpn__wg_down(const char *name, const char *text, size_t len) {
    if (!netman_vpn_valid_name(name, 15)) return netman_vpn__fail("bad name");
    if (!if_nametoindex(name)) return netman_vpn__fail("%s is not up", name);
    if (!netman_vpn__is_wg_link(name)) return netman_vpn__fail("%s exists but is not a wireguard link; refusing to delete it", name);
    netman_vpn_wg c; int have = text && netman_vpn_wg_parse(text, len, &c, NULL, 0) == 0;
    if (have && netman_vpn__hooks(&c.pre_down, name)) { netman_vpn_wg_free(&c); return 1; }
    int rc = netman_vpn__wg_teardown(name);
    if (have) { if (netman_vpn__hooks(&c.post_down, name)) rc = 1; netman_vpn_wg_free(&c); }
    if (!rc) netman_vpn__say("%s: down", name);
    return rc;
}

static int netman_vpn__secret_file_ok(const char *path) {
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return netman_vpn__fail("%s: missing or not a regular file", path), 0;
    if ((st.st_uid != 0 && st.st_uid != geteuid()) || (st.st_mode & 077)) return netman_vpn__fail("%s must be owned by root (or you) with mode 0600", path), 0;
    return 1;
}
/* ---------------------------------------------------------------- logs (the modules write RUN/NAME.log) */
NETMAN_VPN_API int netman_vpn_logs(const char *name, int lines) {
    if (!netman_vpn_valid_name(name, 32)) return netman_vpn__fail("bad name");
    char lf[PATH_MAX]; netman_vpn__rp(lf, sizeof lf, name, "log");
    size_t l; char *t = netman_vpn__slurp(lf, &l, 8u << 20, 0);
    if (!t) return netman_vpn__fail("no log for %s", name);
    if (lines <= 0) lines = 40;
    char *p = t + l; int seen = 0;
    while (p > t && seen <= lines) { p--; if (*p == '\n' && p != t + l - 1) seen++; }
    if (*p == '\n') p++;
    fputs(p, NV_OUT); free(t); return 0;
}

/* ---------------------------------------------------------------- public control */
NETMAN_VPN_API int netman_vpn_up(const char *name, int wait_sec) {
    if (!netman_vpn_valid_name(name, 32)) return netman_vpn__fail("bad profile name %s", name ? name : "(null)");
    char path[PATH_MAX]; size_t l; int ty;
    char *t = netman_vpn__load(name, &l, &ty, path, sizeof path);
    if (!t) return 1;
    int rc;
    if (netman_vpn_enabled_cb && !netman_vpn_enabled_cb(netman_vpn__tyname(ty))) { netman_vpn__wipe(t, l); free(t); return netman_vpn__fail("%s profiles are disabled in config (netman config toggle vpn.%s)", netman_vpn__tyname(ty), netman_vpn__tyname(ty)); }
    if (ty == NETMAN_VPN_WG) rc = netman_vpn__wg_up(name, t, l);
    else {
        netman_vm_ctx cx = netman_vpn__ctx();
        if (netman_vpn__mod_hooks(t, "preup", name)) rc = 1;
        else if ((rc = netman_vpn__mod(ty)->up(&cx, name, path, t, wait_sec)) == 0 && netman_vpn__mod_hooks(t, "postup", name)) rc = 1;
    }
    netman_vpn__wipe(t, l); free(t);
    return rc;
}
NETMAN_VPN_API int netman_vpn_down(const char *name) {
    if (!netman_vpn_valid_name(name, 32)) return netman_vpn__fail("bad profile name");
    char path[PATH_MAX]; size_t l = 0; int ty;
    char *t = netman_vpn__load(name, &l, &ty, path, sizeof path);
    int rc;
    if (!t) { if (if_nametoindex(name) && netman_vpn__is_wg_link(name)) return netman_vpn__wg_down(name, NULL, 0); return 1; }
    if (ty == NETMAN_VPN_WG) rc = netman_vpn__wg_down(name, t, l);
    else {
        netman_vm_ctx cx = netman_vpn__ctx();
        netman_vpn__mod_hooks(t, "predown", name);
        rc = netman_vpn__mod(ty)->down(&cx, name, t);
        if (netman_vpn__mod_hooks(t, "postdown", name)) rc = 1;
    }
    netman_vpn__wipe(t, l); free(t);
    return rc;
}
static void netman_vpn__hbytes(long long v, char *b, size_t n) {
    const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"}; double d = (double)v; int i = 0;
    while (d >= 1024 && i < 4) { d /= 1024; i++; }
    if (i == 0) snprintf(b, n, "%lld B", v); else snprintf(b, n, "%.2f %s", d, u[i]);
}
NETMAN_VPN_API int netman_vpn_status(const char *name) {
    if (!netman_vpn_valid_name(name, 32)) return netman_vpn__fail("bad name");
    int ty = netman_vpn_type(name);
    if (!ty && !(if_nametoindex(name) && netman_vpn__is_wg_link(name))) return 1;
    if (ty != NETMAN_VPN_WG && ty) { const char *s = netman_vpn__mstate(name, ty); netman_vpn__say("%s: %s %s", name, netman_vpn__tyname(ty), s); return strcmp(s, "down") ? 0 : 1; }
    netman_vpn_wgstat st;
    if (netman_vpn_wg_stats(name, &st) || !st.present) { netman_vpn__say("%s: wireguard down", name); return 1; }
    char rx[32], tx[32], hs[48] = "never";
    netman_vpn__hbytes(st.rx, rx, sizeof rx); netman_vpn__hbytes(st.tx, tx, sizeof tx);
    if (st.last_handshake > 0) { long long age = (long long)time(NULL) - st.last_handshake; snprintf(hs, sizeof hs, "%llds ago", age < 0 ? 0 : age); }
    netman_vpn__say("%s: wireguard up, %d peer%s, handshake %s, rx %s, tx %s", name, st.npeers, st.npeers == 1 ? "" : "s", hs, rx, tx);
    return 0;
}
NETMAN_VPN_API void netman_vpn_env(void) {
    const char *e;
    if ((e = getenv("NETMAN_VPN_CONF_DIR")) && *e) netman_vpn_conf_dir = e;
    if ((e = getenv("NETMAN_VPN_RUN_DIR")) && *e) netman_vpn_run_dir = e;
}
static void netman_vpn__hs_text(long long hs, char *b, size_t n) {
    if (hs <= 0) { snprintf(b, n, "never"); return; }
    long long age = (long long)time(NULL) - hs; if (age < 0) age = 0;
    if (age < 120) snprintf(b, n, "%llds ago", age); else if (age < 7200) snprintf(b, n, "%lldm ago", age / 60); else snprintf(b, n, "%lldh ago", age / 3600);
}
/* first peer's endpoint of a WireGuard config text, or "" */
static void netman_vpn__first_ep(const char *text, size_t len, char *ep, size_t n) {
    netman_vpn_wg c; ep[0] = 0;
    if (netman_vpn_wg_parse(text, len, &c, NULL, 0)) return;
    for (size_t i = 0; i < c.npeers; i++) if (c.peers[i].endpoint) { snprintf(ep, n, "%s", c.peers[i].endpoint); break; }
    netman_vpn_wg_free(&c);
}
NETMAN_VPN_API int netman_vpn_ks_state(void) {
    int p[2]; if (pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) { int dn = open("/dev/null", O_WRONLY); if (dn >= 0) dup2(dn, 1); dup2(p[1], 2); execlp("nft", "nft", "list", "table", "inet", "netman_vpn_ks", (char *)NULL); _exit(127); }
    close(p[1]);
    char err[512]; size_t o = 0; ssize_t r;
    while (o < sizeof err - 1 && (r = read(p[0], err + o, sizeof err - 1 - o)) != 0) { if (r < 0) { if (errno == EINTR) continue; break; } o += (size_t)r; }
    err[o] = 0; close(p[0]);
    int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(st)) return -1;
    if (WEXITSTATUS(st) == 0) return 1;
    if (WEXITSTATUS(st) == 127 || strstr(err, "ermitted") || strstr(err, "ermission")) return -1;
    return 0;
}
NETMAN_VPN_API int netman_vpn_list(void) {
    DIR *d = opendir(netman_vpn_conf_dir);
    if (!d) { if (errno == ENOENT) goto ks; return netman_vpn__fail("%s: %s", netman_vpn_conf_dir, strerror(errno)); }
    struct dirent *e;
    while ((e = readdir(d))) {
        char *dot = strrchr(e->d_name, '.');
        if (!dot || (strcmp(dot, ".conf") && strcmp(dot, ".ovpn") && strcmp(dot, ".vpn"))) continue;
        char nm[256]; snprintf(nm, sizeof nm, "%.*s", (int)(dot - e->d_name), e->d_name);
        if (!netman_vpn_valid_name(nm, 32)) continue;
        char path[PATH_MAX], extra[300] = ""; size_t l = 0; int ty = 0; const char *state = "down";
        netman_vpn__mute = 1; char *t = netman_vpn__load(nm, &l, &ty, path, sizeof path); netman_vpn__mute = 0;
        if (!t) {                                           /* cannot read it: say so instead of hiding the profile */
            char fp[PATH_MAX]; snprintf(fp, sizeof fp, "%s/%s", netman_vpn_conf_dir, e->d_name);
            if (access(fp, R_OK)) netman_vpn__say("%-16s %-10s %s", nm, dot[1] == 'o' ? "openvpn" : "?", "locked");
            else if (errno == EACCES) netman_vpn__say("%-16s %-10s %s", nm, dot[1] == 'o' ? "openvpn" : "?", "untrusted");
            continue;
        }
        if (ty == NETMAN_VPN_WG) {
            char ep[128]; netman_vpn__first_ep(t, l, ep, sizeof ep);
            state = if_nametoindex(nm) && netman_vpn__is_wg_link(nm) ? "up" : "down";
            size_t o = 0;
            if (ep[0]) o += (size_t)snprintf(extra + o, sizeof extra - o, " ep=%s", ep);
            if (!strcmp(state, "up")) {
                netman_vpn_wgstat st;
                if (netman_vpn_wg_stats(nm, &st) == 0 && st.present)
                    snprintf(extra + o, sizeof extra - o, " hs=%lld rx=%lld tx=%lld", st.last_handshake > 0 ? (long long)time(NULL) - st.last_handshake : -1LL, st.rx, st.tx);
            }
        } else {
            const char *s = netman_vpn__mstate(nm, ty);
            state = !strcmp(s, "connected") ? "up" : !strcmp(s, "auth failed") ? "failed" : s;
        }
        netman_vpn__wipe(t, l); free(t);
        netman_vpn__say("%-16s %-10s %s%s", nm, netman_vpn__tyname(ty), state, extra);
    }
    closedir(d);
ks:;
    int k = netman_vpn_ks_state();
    netman_vpn__say("killswitch %s", k > 0 ? "on" : k == 0 ? "off" : "unknown");
    return 0;
}
NETMAN_VPN_API int netman_vpn_info(const char *name) {
    if (!netman_vpn_valid_name(name, 32)) return netman_vpn__fail("bad profile name");
    char path[PATH_MAX]; size_t l = 0; int ty;
    char *t = netman_vpn__load(name, &l, &ty, path, sizeof path);
    if (!t) return 1;
    netman_vpn__say("profile      %s", path);
    if (ty != NETMAN_VPN_WG) {
        netman_vm_ctx cx = netman_vpn__ctx(); int k;
        netman_vpn__mod(ty)->info(&cx, name, t);
        k = netman_vpn_ks_state();
        netman_vpn__say("kill switch  %s", k > 0 ? "on" : k == 0 ? "off" : "unknown (needs root and nft)");
        netman_vpn__wipe(t, l); free(t); return 0;
    }
    netman_vpn_wg c; char err[128];
    int up = if_nametoindex(name) && netman_vpn__is_wg_link(name);
    netman_vpn__say("type         wireguard");
    netman_vpn__say("state        %s", up ? "up" : "down");
    if (netman_vpn_wg_parse(t, l, &c, err, sizeof err)) { netman_vpn__say("config       %s", err); netman_vpn__wipe(t, l); free(t); return 0; }
    netman_vpn__wipe(t, l); free(t);
    char b[160];
    for (size_t i = 0; i < c.naddr; i++) netman_vpn__say("%-12s %s/%d", i ? "" : "address", netman_vpn__ip_str(&c.addr[i], b, sizeof b), c.addr[i].prefix);
    if (c.mtu) netman_vpn__say("mtu          %d", c.mtu);
    if (c.dns.n) { char d[300] = ""; for (size_t i = 0; i < c.dns.n; i++) snprintf(d + strlen(d), sizeof d - strlen(d), "%s%s", i ? " " : "", c.dns.v[i]); netman_vpn__say("dns          %s", d); }
    int full = 0;
    for (size_t i = 0; i < c.npeers; i++) {
        const netman_vpn_peer *p = &c.peers[i];
        netman_vpn__say("peer %zu       endpoint %s", i + 1, p->endpoint ? p->endpoint : "-");
        if (p->keepalive) netman_vpn__say("             keepalive %ds", p->keepalive);
        char al[400] = "";
        for (size_t j = 0; j < p->nallowed; j++) {
            if (p->allowed[j].prefix == 0) full = 1;
            size_t o = strlen(al); if (o + 60 > sizeof al) { snprintf(al + o, sizeof al - o, " ..."); break; }
            snprintf(al + o, sizeof al - o, "%s%s/%d", j ? " " : "", netman_vpn__ip_str(&p->allowed[j], b, sizeof b), p->allowed[j].prefix);
        }
        netman_vpn__say("             allowed ips %s", al[0] ? al : "-");
    }
    netman_vpn__say("routing      %s", c.table_off ? "no routes installed (Table = off)" : full ? "full tunnel (default route through the VPN)" : "split tunnel (only the allowed ips go through the VPN)");
    if (up) {
        netman_vpn_wgstat st;
        if (netman_vpn_wg_stats(name, &st) == 0 && st.present) {
            char hs[48], rx[32], tx[32]; netman_vpn__hs_text(st.last_handshake, hs, sizeof hs);
            netman_vpn__hbytes(st.rx, rx, sizeof rx); netman_vpn__hbytes(st.tx, tx, sizeof tx);
            netman_vpn__say("handshake    %s  (%d of %d peers)", hs, st.handshaked, st.npeers);
            netman_vpn__say("transfer     rx %s  tx %s", rx, tx);
            netman_vpn__say("listen port  %d", st.listen_port);
            netman_wg_peerinfo *pi; size_t np;
            if (netman_wg_peers(name, &pi, &np) == 0) {
                for (size_t i = 0; i < np; i++) {
                    char key[45], php[48], prx[32], ptx[32];
                    netman_vpn_b64_encode32(pi[i].pub, key); netman_vpn__hs_text(pi[i].last_handshake, php, sizeof php);
                    netman_vpn__hbytes(pi[i].rx, prx, sizeof prx); netman_vpn__hbytes(pi[i].tx, ptx, sizeof ptx);
                    netman_vpn__say("live peer %zu  %.8s...  handshake %s  rx %s  tx %s", i + 1, key, php, prx, ptx);
                }
                netman_wg_peers_free(pi, np);
            }
        } else netman_vpn__say("handshake    - (reading the live tunnel needs root)");
    }
    int k = netman_vpn_ks_state();
    netman_vpn__say("kill switch  %s", k > 0 ? "on" : k == 0 ? "off" : "unknown (needs root and nft)");
    netman_vpn_wg_free(&c);
    return 0;
}
NETMAN_VPN_API int netman_vpn_import(const char *file, const char *name_in) {
    if (!file || !*file) return netman_vpn__fail("import: no file given (netman vpn import FILE [NAME])");
    size_t l = 0; char *t = netman_vpn__slurp(file, &l, 256u << 10, 0);
    if (!t) return netman_vpn__fail("%s: %s", file, errno == EFBIG ? "not a regular file or too big (max 256 KiB)" : strerror(errno));
    netman_vpnimp o; char err[400];
    if (netman_vpnimp_convert(t, l, &o, err, sizeof err)) { netman_vpn__wipe(t, l); free(t); return netman_vpn__fail("%s: %s", file, err); }
    netman_vpn__wipe(t, l); free(t);
    char name[64];
    if (name_in && *name_in) snprintf(name, sizeof name, "%s", name_in);
    else {
        const char *b = strrchr(file, '/'); b = b ? b + 1 : file;
        snprintf(name, sizeof name, "%s", b);
        char *dot = strrchr(name, '.'); if (dot && dot != name) *dot = 0;
        for (char *c = name; *c; c++) if (!(isalnum((unsigned char)*c) || strchr("_=+.-", *c))) *c = '-';
        while (name[0] == '.' || name[0] == '-') memmove(name, name + 1, strlen(name));
    }
    if (!netman_vpn_valid_name(name, 32)) { netman_vpnimp_free(&o); return netman_vpn__fail("\"%s\" is not a usable profile name (1-32 chars of A-Za-z0-9_=+.-; WireGuard interface names are cut to 15)", name); }
    int wg = !strcmp(o.type, "wireguard");
    if (wg) {
        netman_vpn_wg c; char e2[128];
        if (netman_vpn_wg_parse(o.text, o.len, &c, e2, sizeof e2)) { netman_vpnimp_free(&o); return netman_vpn__fail("%s: %s", file, e2); }
        netman_vpn_wg_free(&c);
        if (strlen(name) > 15) netman_vpn__fail("note: %s is longer than 15 chars; a WireGuard interface name cannot be, so `up` will refuse it (use: vpn import FILE SHORTNAME)", name);
    }
    static const char *const exts[] = { "conf", "ovpn", "vpn" };
    char path[PATH_MAX], other[PATH_MAX]; struct stat st;
    for (int i = 0; i < 3; i++) {
        snprintf(other, sizeof other, "%s/%s.%s", netman_vpn_conf_dir, name, exts[i]);
        if (lstat(other, &st) == 0) { netman_vpnimp_free(&o); return netman_vpn__fail("a profile called %s already exists in %s (pass another NAME)", name, netman_vpn_conf_dir); }
    }
    snprintf(path, sizeof path, "%s/%s%s", netman_vpn_conf_dir, name, o.ext);
    if (netman_vpn__mkdirs(netman_vpn_conf_dir, 0755)) { int e = errno; netman_vpnimp_free(&o); return netman_vpn__fail("%s: %s%s", netman_vpn_conf_dir, strerror(e), e == EACCES || e == EPERM ? " (import needs root)" : ""); }
    if (netman_vpn__spit(path, o.text, o.len, 0600)) { int e = errno; netman_vpnimp_free(&o); return netman_vpn__fail("%s: %s%s", path, strerror(e), e == EACCES || e == EPERM ? " (import needs root)" : ""); }
    int refs = !strcmp(o.type, "openvpn") && (strstr(o.text, "\nca ") || strstr(o.text, "\ncert ") || strstr(o.text, "\nkey ") || strstr(o.text, "\nauth-user-pass "));
    char ty[16], nt[400]; snprintf(ty, sizeof ty, "%s", o.type); snprintf(nt, sizeof nt, "%s", o.note);
    netman_vpnimp_free(&o);
    netman_vpn__say("imported %s as %s (%s, mode 0600)", file, path, ty);
    if (nt[0]) netman_vpn__say("note: %s", nt);
    if (refs) netman_vpn__say("note: this OpenVPN profile references other files (ca/cert/key/auth); copy them next to it in %s", netman_vpn_conf_dir);
    return 0;
}

/* ---------------------------------------------------------------- kill switch (nftables, one atomic ruleset) */
static int netman_vpn__nft(const char *rules, int quiet) {
    int p[2]; if (pipe2(p, O_CLOEXEC)) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { dup2(p[0], 0); if (quiet) { int dn = open("/dev/null", O_WRONLY); if (dn >= 0) dup2(dn, 2); } execlp("nft", "nft", "-f", "-", (char *)NULL); _exit(127); }
    close(p[0]);
    size_t n = strlen(rules), off = 0;
    while (off < n) { ssize_t w = write(p[1], rules + off, n - off); if (w < 0) { if (errno == EINTR) continue; break; } off += (size_t)w; }
    close(p[1]);
    int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}
/* endpoints of a profile as "ip port proto" strings; returns count */
static int netman_vpn__endpoints(const char *text, size_t len, int ty, char eps[][160], int max, char *tun, size_t tunn, const char *name) {
    int n = 0;
    if (ty == NETMAN_VPN_WG) {
        netman_vpn_wg c; if (netman_vpn_wg_parse(text, len, &c, NULL, 0)) return -1;
        snprintf(tun, tunn, "%s", name);
        for (size_t i = 0; i < c.npeers && n < max; i++) {
            struct sockaddr_storage ss; socklen_t sl; char ip[64]; int port = 0;
            if (!c.peers[i].endpoint || netman_vpn__ep_resolve(c.peers[i].endpoint, &ss, &sl)) continue;
            if (ss.ss_family == AF_INET) { inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, ip, sizeof ip); port = ntohs(((struct sockaddr_in *)&ss)->sin_port); snprintf(eps[n++], 160, "ip daddr %s udp dport %d accept", ip, port); }
            else { inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, ip, sizeof ip); port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port); snprintf(eps[n++], 160, "ip6 daddr %s udp dport %d accept", ip, port); }
        }
        netman_vpn_wg_free(&c);
    } else {
        const netman_vpnmod *m = netman_vpn__mod(ty);
        tun[0] = 0;
        if (!m) return -1;
        n = m->ks(text, eps, max, tun, tunn, name);
    }
    return n;
}
NETMAN_VPN_API int netman_vpn_killswitch(const char *name, int on) {
    if (!netman_vpn_valid_name(name, 32)) return netman_vpn__fail("bad profile name");
    if (!on) {
        if (netman_vpn__nft("delete table inet netman_vpn_ks\n", 1)) return netman_vpn__fail("kill switch was not active (or nft failed)");
        netman_vpn__say("kill switch off"); return 0;
    }
    char path[PATH_MAX]; size_t l; int ty;
    char *t = netman_vpn__load(name, &l, &ty, path, sizeof path); if (!t) return 1;
    static char eps[32][160]; char tun[32];
    int n = netman_vpn__endpoints(t, l, ty, eps, 32, tun, sizeof tun, name);
    netman_vpn__wipe(t, l); free(t);
    if (n <= 0) return netman_vpn__fail("%s: no resolvable endpoint, refusing to arm a kill switch that would cut everything", name);
    size_t cap = 4096 + (size_t)n * 128; char *r = malloc(cap); if (!r) return 1;
    size_t o = (size_t)snprintf(r, cap, "table inet netman_vpn_ks\ndelete table inet netman_vpn_ks\ntable inet netman_vpn_ks {\n chain out {\n  type filter hook output priority -10; policy drop;\n"
        "  oifname \"lo\" accept\n");
    if (tun[0]) o += (size_t)snprintf(r + o, cap - o, "  oifname \"%s\" accept\n", tun);
    for (int i = 0; i < n; i++) o += (size_t)snprintf(r + o, cap - o, "  %s\n", eps[i]);
    o += (size_t)snprintf(r + o, cap - o, "  udp sport 68 udp dport 67 accept\n  udp sport 546 udp dport 547 accept\n"
        "  meta l4proto { tcp, udp } th dport { 53, 853 } drop\n");              /* DNS leak guard: resolvers only through the tunnel */
    if (getenv("NETMAN_VPN_KS_LAN") && strcmp(getenv("NETMAN_VPN_KS_LAN"), "1") == 0)
        o += (size_t)snprintf(r + o, cap - o, "  ip daddr { 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16 } accept\n  ip6 daddr { fc00::/7, fe80::/10 } accept\n");
    snprintf(r + o, cap - o, " }\n}\n");
    int rc = netman_vpn__nft(r, 0); free(r);
    if (rc) return netman_vpn__fail("nft refused the kill switch ruleset (need root / CAP_NET_ADMIN, nft installed)");
    netman_vpn__say("kill switch on: only %s, loopback, DHCP and %d endpoint%s can leave this machine", tun[0] ? tun : "IPsec-protected traffic", n, n == 1 ? "" : "s");
    return 0;
}

/* ---------------------------------------------------------------- run one command through the tunnel (own netns) */
static int netman_vpn__rd(int fd) { char c; ssize_t r; while ((r = read(fd, &c, 1)) < 0 && errno == EINTR) {} return r == 1 ? 0 : -1; }
static int netman_vpn__wr(int fd) { char c = 1; ssize_t r; while ((r = write(fd, &c, 1)) < 0 && errno == EINTR) {} return r == 1 ? 0 : -1; }

NETMAN_VPN_API int netman_vpn_run(const char *name, char *const argv[]) {
    if (!argv || !argv[0]) return netman_vpn__fail("run: no command");
    if (!netman_vpn_valid_name(name, 15)) return netman_vpn__fail("bad profile name");
    char path[PATH_MAX]; size_t l; int ty;
    char *t = netman_vpn__load(name, &l, &ty, path, sizeof path); if (!t) return 1;
    if (ty != NETMAN_VPN_WG) { free(t); return netman_vpn__fail("run: only WireGuard profiles"); }
    netman_vpn_wg c; char err[128];
    if (netman_vpn_wg_parse(t, l, &c, err, sizeof err)) { netman_vpn__wipe(t, l); free(t); return netman_vpn__fail("%s: %s", name, err); }
    netman_vpn__wipe(t, l); free(t);
    netman_vpn__ep *eps = calloc(c.npeers ? c.npeers : 1, sizeof *eps);
    int p1[2], p2[2], rc = 1, fd = -1;
    if (!eps || pipe(p1) || pipe(p2)) { netman_vpn_wg_free(&c); free(eps); return netman_vpn__fail("run: setup failed"); }
    for (size_t i = 0; i < c.npeers; i++) if (c.peers[i].endpoint && netman_vpn__ep_resolve(c.peers[i].endpoint, &eps[i].ss, &eps[i].sl)) { netman_vpn__fail("%s: cannot resolve a peer endpoint", name); goto fin; }
    pid_t kid = fork();
    if (kid < 0) { netman_vpn__fail("fork: %s", strerror(errno)); goto fin; }
    if (kid == 0) {
        close(p1[0]); close(p2[1]);
        if (unshare(CLONE_NEWNET | CLONE_NEWNS)) { fprintf(NV_ERR, "netman vpn: run: unshare: %s (need root or user namespaces)\n", strerror(errno)); _exit(126); }
        mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
        if (c.dns.n) {                                       /* this command resolves through the tunnel's DNS only */
            char rf[PATH_MAX], buf[2048]; size_t o = 0;
            snprintf(rf, sizeof rf, "%s/%s.%d.resolv", netman_vpn_run_dir, name, (int)getpid());
            for (size_t i = 0; i < c.dns.n && o + 64 < sizeof buf; i++) o += (size_t)snprintf(buf + o, sizeof buf - o, "nameserver %s\n", c.dns.v[i]);
            if (netman_vpn__rundir() == 0 && netman_vpn__spit(rf, buf, o, 0644) == 0) { if (mount(rf, netman_vpn_resolv_path, NULL, MS_BIND, NULL)) fprintf(NV_ERR, "netman vpn: run: cannot bind DNS file: %s (command keeps the host resolver)\n", strerror(errno)); unlink(rf); }
        }
        netman_vpn__wr(p1[1]);                                     /* netns exists */
        if (netman_vpn__rd(p2[0])) _exit(125);                     /* the tunnel has been moved in */
        int rt = netman_vpn__nl_open(NETLINK_ROUTE); if (rt < 0) _exit(125);
        int lo = (int)if_nametoindex("lo"), idx = (int)if_nametoindex(name);
        if (!idx) { fprintf(NV_ERR, "netman vpn: run: tunnel did not arrive\n"); _exit(125); }
        if (lo) netman_vpn__rt_link_set(rt, lo, 1, 0);
        for (size_t i = 0; i < c.naddr; i++) netman_vpn__rt_addr(rt, idx, &c.addr[i]);
        if (netman_vpn__rt_link_set(rt, idx, 1, c.mtu ? c.mtu : 1420)) _exit(125);
        for (size_t i = 0; i < c.npeers; i++) for (size_t j = 0; j < c.peers[i].nallowed; j++) netman_vpn__rt_route(rt, idx, &c.peers[i].allowed[j], 254);
        close(rt);
        execvp(argv[0], argv);
        fprintf(NV_ERR, "netman vpn: run: %s: %s\n", argv[0], strerror(errno)); _exit(127);
    }
    close(p1[1]); close(p2[0]); p1[1] = p2[0] = -1;
    char tmpn[IFNAMSIZ]; snprintf(tmpn, sizeof tmpn, "nmbr%d", (int)(getpid() % 100000));
    if (netman_vpn__rd(p1[0])) { netman_vpn__fail("run: child failed before the namespace existed"); goto reap; }
    fd = netman_vpn__nl_open(NETLINK_ROUTE);
    int r = fd < 0 ? -errno : netman_wg_link_add(tmpn);
    if (r) { netman_vpn__fail("create wireguard link: %s", strerror(-r)); goto reap; }
    if ((r = netman_vpn__wg_set(tmpn, &c, eps, 0, 0))) { netman_vpn__fail("configure wireguard: %s", strerror(-r)); netman_vpn__rt_link_del(fd, (int)if_nametoindex(tmpn)); goto reap; }
    {   /* move into the child's netns, renaming to NAME; the UDP socket stays in the host namespace (that is the point) */
        netman_vpn__msg *m = malloc(sizeof *m); if (!m) goto reap;
        struct ifinfomsg ifi = {.ifi_family = AF_UNSPEC, .ifi_index = (int)if_nametoindex(tmpn)};
        netman_vpn__hdr(m, RTM_NEWLINK, NLM_F_ACK, &ifi, sizeof ifi);
        netman_vpn__attr_u32(m, IFLA_NET_NS_PID, (unsigned)kid);
        netman_vpn__attr_str(m, IFLA_IFNAME, name);
        r = netman_vpn__talk(fd, m, NULL, NULL); free(m);
        if (r) { netman_vpn__fail("move tunnel into namespace: %s", strerror(-r)); netman_vpn__rt_link_del(fd, (int)if_nametoindex(tmpn)); goto reap; }
    }
    netman_vpn__wr(p2[1] >= 0 ? p2[1] : -1);
    rc = 0;
reap:;
    if (rc) { kill(kid, SIGKILL); }
    {
        int st; while (waitpid(kid, &st, 0) < 0 && errno == EINTR) {}
        if (!rc) rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
fin:
    if (fd >= 0) close(fd);
    netman_vpn_wg_free(&c); free(eps);
    return rc;
}

/* ---------------------------------------------------------------- watchdog */
NETMAN_VPN_API int netman_vpn_watch_once(const char *name, int stale_sec) {
    if (!netman_vpn_valid_name(name, 32)) return -1;
    int ty = netman_vpn_type(name); if (!ty) return -1;
    int healthy = 0;
    if (ty != NETMAN_VPN_WG) healthy = strcmp(netman_vpn__mstate(name, ty), "connected") == 0;
    else if (if_nametoindex(name) && netman_vpn__is_wg_link(name)) {
        netman_vpn_wgstat st; char sp[PATH_MAX]; struct stat ss;
        if (netman_vpn_wg_stats(name, &st) == 0 && st.present) {
            long long now = (long long)time(NULL);
            if (st.last_handshake > 0) healthy = now - st.last_handshake <= stale_sec;
            else { netman_vpn__rp(sp, sizeof sp, name, "state"); healthy = stat(sp, &ss) == 0 && now - (long long)ss.st_mtime <= stale_sec; }   /* grace for a fresh tunnel */
        }
    }
    if (healthy) return 0;
    netman_vpn__say("%s: unhealthy, reconnecting", name);
    if (ty == NETMAN_VPN_WG) { if (if_nametoindex(name) && netman_vpn__is_wg_link(name)) netman_vpn_down(name); }
    else if (strcmp(netman_vpn__mstate(name, ty), "down")) { netman_vm_ctx cx = netman_vpn__ctx(); netman_vpn__mod(ty)->down(&cx, name, NULL); }
    return netman_vpn_up(name, 30) == 0 ? 1 : -1;
}
NETMAN_VPN_API int netman_vpn_watch(const char *name, int interval_sec, int stale_sec) {
    if (interval_sec <= 0) interval_sec = 15;
    if (stale_sec <= 0) stale_sec = 180;
    for (;;) { int r = netman_vpn_watch_once(name, stale_sec); if (r < 0) netman_vpn__fail("%s: reconnect failed, retrying", name); fflush(NULL); sleep((unsigned)interval_sec); }
}

/* ---------------------------------------------------------------- ICMP rtt + path MTU (unprivileged ping socket, IPv4) */
static int netman_vpn__ping(const char *ip, int payload, int df, int timeout_ms, int *rtt) {   /* 1 reply, 0 none, -1 too big for the path, -2 no ping socket */
    struct in_addr a; if (inet_pton(AF_INET, ip, &a) != 1) return -2;
    int raw = 0, s;
retry_raw:
    s = raw ? socket(AF_INET, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMP) : socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_ICMP);   /* unprivileged ping socket first, raw as root/CAP_NET_RAW fallback */
    if (s < 0 && !raw) { raw = 1; goto retry_raw; }
    if (s < 0) return -2;
    int pmtu = df ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
    setsockopt(s, IPPROTO_IP, IP_MTU_DISCOVER, &pmtu, sizeof pmtu);
    unsigned char pkt[2048]; memset(pkt, 0xa5, sizeof pkt);
    struct icmphdr *h = (struct icmphdr *)pkt; h->type = ICMP_ECHO; h->code = 0; h->un.echo.sequence = htons(1); h->un.echo.id = raw ? htons((unsigned short)getpid()) : 0;
    if (payload > 1900) payload = 1900;
    if (raw) {
        unsigned long sum = 0; size_t tot = sizeof *h + (size_t)payload; h->checksum = 0;
        for (size_t i = 0; i + 1 < tot; i += 2) sum += (unsigned long)(pkt[i] << 8 | pkt[i + 1]);
        if (tot & 1) sum += (unsigned long)(pkt[tot - 1] << 8);
        while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
        h->checksum = htons((unsigned short)~sum);
    }
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_addr = a};
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    if (sendto(s, pkt, sizeof *h + (size_t)payload, 0, (struct sockaddr *)&to, sizeof to) < 0) { int e = errno; close(s); if (!raw && (e == EACCES || e == EPERM)) { raw = 1; goto retry_raw; } return e == EMSGSIZE ? -1 : 0; }
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long left = timeout_ms - ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
        if (left <= 0) { close(s); return 0; }
        struct pollfd pf = {s, POLLIN, 0};
        if (poll(&pf, 1, (int)left) <= 0) { close(s); return 0; }
        unsigned char rb[2048]; ssize_t n = recv(s, rb, sizeof rb, MSG_DONTWAIT);
        ssize_t off = raw && n > 20 ? (rb[0] & 15) * 4 : 0;
        if (n - off >= (ssize_t)sizeof *h && ((struct icmphdr *)(rb + off))->type == ICMP_ECHOREPLY && (!raw || ((struct icmphdr *)(rb + off))->un.echo.id == h->un.echo.id)) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (rtt) *rtt = (int)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000);
            close(s); return 1;
        }
    }
}
NETMAN_VPN_API int netman_vpn_rtt_ms(const char *ip, int timeout_ms) { int rtt = -1; return netman_vpn__ping(ip, 16, 0, timeout_ms > 0 ? timeout_ms : 1000, &rtt) == 1 ? rtt : -1; }
NETMAN_VPN_API int netman_vpn_mtu_probe(const char *ip) {
    int ok = 0, rtt;
    for (int i = 0; i < 3 && !ok; i++) ok = netman_vpn__ping(ip, 8, 0, 1000, &rtt) == 1;
    if (!ok) return -1;
    int lo = 8, hi = 1472;                       /* payload bytes; MTU = payload + 8 (icmp) + 20 (ip) */
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2, good = 0;
        for (int i = 0; i < 2 && !good; i++) good = netman_vpn__ping(ip, mid, 1, 800, &rtt) == 1;
        if (good) lo = mid; else hi = mid - 1;
    }
    return lo + 28;
}

/* ---------------------------------------------------------------- tiny JSON reader (token array, no copies) */
typedef struct { int type, start, end, size; } netman_vpn__jt;      /* type: 1 object, 2 array, 3 string, 4 other; size = direct children (object: keys) */
typedef struct { const char *s; size_t n; netman_vpn__jt *t; int nt, cap; } netman_vpn__jp;
static int netman_vpn__jval(netman_vpn__jp *p, size_t *i, int depth);
static void netman_vpn__jws(netman_vpn__jp *p, size_t *i) { while (*i < p->n && isspace((unsigned char)p->s[*i])) (*i)++; }
static int netman_vpn__jstr_end(netman_vpn__jp *p, size_t *i) {      /* *i at opening quote; leaves *i after closing quote */
    (*i)++;
    while (*i < p->n) { if (p->s[*i] == '\\') *i += 2; else if (p->s[*i] == '"') { (*i)++; return 0; } else (*i)++; }
    return -1;
}
static int netman_vpn__jval(netman_vpn__jp *p, size_t *i, int depth) {
    if (depth > 24 || p->nt >= p->cap) return -1;
    netman_vpn__jws(p, i);
    if (*i >= p->n) return -1;
    int me = p->nt++; netman_vpn__jt *t = &p->t[me]; t->start = (int)*i; t->size = 0;
    char c = p->s[*i];
    if (c == '{' || c == '[') {
        t->type = c == '{' ? 1 : 2; (*i)++;
        netman_vpn__jws(p, i);
        if (*i < p->n && p->s[*i] == (c == '{' ? '}' : ']')) { (*i)++; p->t[me].end = (int)*i; return 0; }
        for (;;) {
            netman_vpn__jws(p, i);
            if (c == '{') {
                if (*i >= p->n || p->s[*i] != '"') return -1;
                if (p->nt >= p->cap) return -1;
                netman_vpn__jt *k = &p->t[p->nt++]; k->type = 3; k->start = (int)*i + 1; k->size = 0;
                if (netman_vpn__jstr_end(p, i)) return -1;
                k->end = (int)*i - 1;
                netman_vpn__jws(p, i); if (*i >= p->n || p->s[*i] != ':') return -1; (*i)++;
            }
            if (netman_vpn__jval(p, i, depth + 1)) return -1;
            p->t[me].size++;
            netman_vpn__jws(p, i); if (*i >= p->n) return -1;
            if (p->s[*i] == ',') { (*i)++; continue; }
            if (p->s[*i] == (c == '{' ? '}' : ']')) { (*i)++; p->t[me].end = (int)*i; return 0; }
            return -1;
        }
    }
    if (c == '"') { t->type = 3; t->start++; if (netman_vpn__jstr_end(p, i)) return -1; p->t[me].end = (int)*i - 1; return 0; }
    t->type = 4;
    while (*i < p->n && !strchr(",]} \t\r\n", p->s[*i])) (*i)++;
    p->t[me].end = (int)*i;
    return p->t[me].end > p->t[me].start ? 0 : -1;
}
static int netman_vpn__jparse(netman_vpn__jp *p, const char *s, size_t n) {
    p->s = s; p->n = n; p->nt = 0; p->cap = (int)(n < 12u << 20 ? n + 16 : 0);
    if (!p->cap) return -1;
    p->t = malloc((size_t)p->cap * sizeof *p->t); if (!p->t) return -1;
    size_t i = 0;
    if (netman_vpn__jval(p, &i, 0)) { free(p->t); p->t = NULL; return -1; }
    return 0;
}
static int netman_vpn__jnext(const netman_vpn__jp *p, int i) {          /* index after the subtree at i */
    const netman_vpn__jt *t = &p->t[i]; int j = i + 1;
    if (t->type == 1) for (int k = 0; k < t->size; k++) { j++; j = netman_vpn__jnext(p, j); }
    else if (t->type == 2) for (int k = 0; k < t->size; k++) j = netman_vpn__jnext(p, j);
    return j;
}
static int netman_vpn__jget(const netman_vpn__jp *p, int obj, const char *key) {
    if (obj < 0 || p->t[obj].type != 1) return -1;
    int j = obj + 1; size_t kl = strlen(key);
    for (int k = 0; k < p->t[obj].size; k++) {
        const netman_vpn__jt *kt = &p->t[j];
        if ((size_t)(kt->end - kt->start) == kl && memcmp(p->s + kt->start, key, kl) == 0) return j + 1;
        j = netman_vpn__jnext(p, j + 1);
    }
    return -1;
}
static int netman_vpn__jelem(const netman_vpn__jp *p, int arr, int idx) {
    if (arr < 0 || p->t[arr].type != 2 || idx >= p->t[arr].size) return -1;
    int j = arr + 1; for (int k = 0; k < idx; k++) j = netman_vpn__jnext(p, j);
    return j;
}
static int netman_vpn__jcopy(const netman_vpn__jp *p, int i, char *out, size_t n) {      /* strings only; handles \" \\ \/ */
    if (i < 0 || p->t[i].type != 3) return -1;
    size_t o = 0;
    for (int k = p->t[i].start; k < p->t[i].end; k++) {
        char ch = p->s[k];
        if (ch == '\\' && k + 1 < p->t[i].end) { k++; ch = p->s[k]; if (ch == 'u' || ch == 'n' || ch == 't' || ch == 'r' || ch == 'b' || ch == 'f') ch = '?'; }
        if (o + 1 >= n) return -1;
        out[o++] = ch;
    }
    out[o] = 0; return 0;
}
static int netman_vpn__jint(const netman_vpn__jp *p, int i, int dflt) {
    if (i < 0 || p->t[i].type != 4) return dflt;
    char b[24]; int l = p->t[i].end - p->t[i].start; if (l <= 0 || l >= (int)sizeof b) return dflt;
    memcpy(b, p->s + p->t[i].start, (size_t)l); b[l] = 0;
    char *e; long v = strtol(b, &e, 10); return *e ? dflt : (int)v;
}
/* field hygiene: relay data comes from the network and ends up in a config file */
static int netman_vpn__safe_word(const char *s, size_t max) {
    size_t l = strlen(s); if (!l || l >= max) return 0;
    for (; *s; s++) if (!(isalnum((unsigned char)*s) || strchr(".-_", *s))) return 0;
    return 1;
}
static int netman_vpn__relay_ok(netman_vpn_relay *r) {
    unsigned char k[32]; struct in_addr a;
    if (netman_vpn_b64_decode32(r->pubkey, k) || inet_pton(AF_INET, r->endpoint_ip, &a) != 1) return 0;
    if (!netman_vpn__safe_word(r->host, sizeof r->host) || !netman_vpn__safe_word(r->id, sizeof r->id)) return 0;
    if (!r->country[0] || !netman_vpn__safe_word(r->country, sizeof r->country)) return 0;
    if (r->city[0] && !netman_vpn__safe_word(r->city, sizeof r->city)) snprintf(r->city, sizeof r->city, "%s", "");
    for (char *c = r->country; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char *c = r->city; *c; c++) *c = (char)tolower((unsigned char)*c);
    if (!r->port) r->port = 51820;
    return 1;
}

NETMAN_VPN_API int netman_vpn_mullvad_parse(const char *json, size_t len, netman_vpn_relay *out, int max) {
    netman_vpn__jp p; int n = 0;
    if (netman_vpn__jparse(&p, json, len)) return -1;
    int countries = netman_vpn__jget(&p, 0, "countries");
    if (countries < 0 || p.t[countries].type != 2) { free(p.t); return -1; }
    for (int ci = 0; ci < p.t[countries].size; ci++) {
        int co = netman_vpn__jelem(&p, countries, ci), cities = netman_vpn__jget(&p, co, "cities");
        if (cities < 0 || p.t[cities].type != 2) continue;
        char ccode[8] = ""; netman_vpn__jcopy(&p, netman_vpn__jget(&p, co, "code"), ccode, sizeof ccode);
        for (int yi = 0; yi < p.t[cities].size; yi++) {
            int city = netman_vpn__jelem(&p, cities, yi), relays = netman_vpn__jget(&p, city, "relays");
            if (relays < 0 || p.t[relays].type != 2) continue;
            char ycode[48] = ""; netman_vpn__jcopy(&p, netman_vpn__jget(&p, city, "code"), ycode, sizeof ycode);
            for (int ri = 0; ri < p.t[relays].size && n < max; ri++) {
                int r = netman_vpn__jelem(&p, relays, ri); netman_vpn_relay x; memset(&x, 0, sizeof x);
                if (netman_vpn__jcopy(&p, netman_vpn__jget(&p, r, "hostname"), x.host, sizeof x.host) || netman_vpn__jcopy(&p, netman_vpn__jget(&p, r, "ipv4_addr_in"), x.endpoint_ip, sizeof x.endpoint_ip) ||
                    netman_vpn__jcopy(&p, netman_vpn__jget(&p, r, "public_key"), x.pubkey, sizeof x.pubkey)) continue;
                snprintf(x.id, sizeof x.id, "%s", x.host); snprintf(x.country, sizeof x.country, "%s", ccode); snprintf(x.city, sizeof x.city, "%s", ycode);
                x.port = 51820; x.multihop_port = netman_vpn__jint(&p, netman_vpn__jget(&p, r, "multihop_port"), 0); x.load = -1;
                if (netman_vpn__relay_ok(&x)) out[n++] = x;
            }
        }
    }
    free(p.t); return n;
}

NETMAN_VPN_API int netman_vpn_nord_parse(const char *json, size_t len, netman_vpn_relay *out, int max) {
    netman_vpn__jp p; int n = 0;
    if (netman_vpn__jparse(&p, json, len)) return -1;
    if (p.t[0].type != 2) { free(p.t); return -1; }
    for (int si = 0; si < p.t[0].size && n < max; si++) {
        int s = netman_vpn__jelem(&p, 0, si); netman_vpn_relay x; memset(&x, 0, sizeof x);
        char status[16] = "";
        netman_vpn__jcopy(&p, netman_vpn__jget(&p, s, "status"), status, sizeof status);
        if (strcmp(status, "online") != 0) continue;
        if (netman_vpn__jcopy(&p, netman_vpn__jget(&p, s, "hostname"), x.host, sizeof x.host) || netman_vpn__jcopy(&p, netman_vpn__jget(&p, s, "station"), x.endpoint_ip, sizeof x.endpoint_ip)) continue;
        snprintf(x.id, sizeof x.id, "%s", x.host);
        x.load = netman_vpn__jint(&p, netman_vpn__jget(&p, s, "load"), -1); x.port = 51820;
        int loc = netman_vpn__jelem(&p, netman_vpn__jget(&p, s, "locations"), 0), country = netman_vpn__jget(&p, netman_vpn__jget(&p, loc, "country"), "code") ;
        netman_vpn__jcopy(&p, country, x.country, sizeof x.country);
        int cityname = netman_vpn__jget(&p, netman_vpn__jget(&p, netman_vpn__jget(&p, loc, "country"), "city"), "dns_name");
        netman_vpn__jcopy(&p, cityname, x.city, sizeof x.city);
        int techs = netman_vpn__jget(&p, s, "technologies"), found = 0;
        for (int ti = 0; techs >= 0 && ti < p.t[techs].size && !found; ti++) {
            int tc = netman_vpn__jelem(&p, techs, ti); char id[32] = "";
            netman_vpn__jcopy(&p, netman_vpn__jget(&p, tc, "identifier"), id, sizeof id);
            if (strcmp(id, "wireguard_udp") != 0) continue;
            int md = netman_vpn__jget(&p, tc, "metadata");
            for (int mi = 0; md >= 0 && mi < p.t[md].size; mi++) {
                int m = netman_vpn__jelem(&p, md, mi); char nm[24] = "";
                netman_vpn__jcopy(&p, netman_vpn__jget(&p, m, "name"), nm, sizeof nm);
                if (strcmp(nm, "public_key") == 0 && netman_vpn__jcopy(&p, netman_vpn__jget(&p, m, "value"), x.pubkey, sizeof x.pubkey) == 0) found = 1;
            }
        }
        if (found && netman_vpn__relay_ok(&x)) out[n++] = x;
    }
    free(p.t); return n;
}

/* ---------------------------------------------------------------- http through curl (no libs; secrets only on stdin) */
static char *netman_vpn__curl(const char *cfg_stdin, size_t *outlen) {
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC)) return NULL;
    pid_t pid = fork();
    if (pid < 0) return NULL;
    if (pid == 0) {
        dup2(in[0], 0); dup2(out[1], 1);
        execlp("curl", "curl", "-sS", "--fail-with-body", "--max-time", "30", "--proto", "=https", "--tlsv1.2", "--max-filesize", "10000000", "-K", "-", (char *)NULL);
        _exit(127);
    }
    close(in[0]); close(out[1]);
    size_t n = strlen(cfg_stdin), off = 0;
    while (off < n) { ssize_t w = write(in[1], cfg_stdin + off, n - off); if (w < 0) { if (errno == EINTR) continue; break; } off += (size_t)w; }
    close(in[1]);
    size_t cap = 1 << 16, len = 0; char *buf = malloc(cap);
    for (;;) {
        if (len + 4096 > cap) { if (cap > (12u << 20)) break; cap *= 2; char *nb = realloc(buf, cap); if (!nb) break; buf = nb; }
        ssize_t r = read(out[0], buf + len, cap - len - 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        len += (size_t)r;
    }
    close(out[0]);
    int st; while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (!buf) return NULL;
    buf[len] = 0;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { free(buf); return NULL; }
    if (outlen) *outlen = len;
    return buf;
}
static void netman_vpn__cfg_quote(char *dst, size_t n, const char *s) {       /* curl -K value quoting */
    size_t o = 0; dst[o++] = '"';
    for (; *s && o + 3 < n; s++) { if (*s == '"' || *s == '\\') dst[o++] = '\\'; if (*s == '\n' || *s == '\r') continue; dst[o++] = *s; }
    dst[o++] = '"'; dst[o] = 0;
}
static char *netman_vpn__secret(const char *envname) {          /* value from the file named by $envname; must be 0600 and ours; trimmed */
    const char *f = getenv(envname);
    if (!f || !*f) { netman_vpn__fail("set %s to a file holding the secret (mode 0600)", envname); return NULL; }
    if (!netman_vpn__secret_file_ok(f)) return NULL;
    char *t = netman_vpn__slurp(f, NULL, 4096, 0); if (!t) { netman_vpn__fail("%s: %s", f, strerror(errno)); return NULL; }
    char *v = netman_vpn__trim(t); char *r = strdup(v); netman_vpn__wipe(t, strlen(t)); free(t);
    for (char *c = r; r && *c; c++) if (!(isprint((unsigned char)*c) && *c != ' ')) { netman_vpn__fail("%s: secret has odd characters", f); free(r); return NULL; }
    return r;
}

/* ---------------------------------------------------------------- providers */
static int netman_vpn__write_profile(const char *profile, const char *priv_b64, const char *addr, const char *dns, const netman_vpn_relay *r, const char *ep_ip, int ep_port) {
    if (!netman_vpn_valid_name(profile, 15)) return netman_vpn__fail("bad profile name");
    char buf[1024], path[PATH_MAX];
    int n = snprintf(buf, sizeof buf, "# generated by netman vpn for %s (%s)\n[Interface]\nPrivateKey = %s\nAddress = %s\nDNS = %s\n\n[Peer]\nPublicKey = %s\nAllowedIPs = 0.0.0.0/0, ::/0\nEndpoint = %s:%d\nPersistentKeepalive = 25\n",
                     r->host, r->country, priv_b64, addr, dns, r->pubkey, ep_ip, ep_port);
    if (netman_vpn__mkdirs(netman_vpn_conf_dir, 0755)) return netman_vpn__fail("%s: %s", netman_vpn_conf_dir, strerror(errno));
    snprintf(path, sizeof path, "%s/%s.conf", netman_vpn_conf_dir, profile);
    int rc = netman_vpn__spit(path, buf, (size_t)n, 0600);
    netman_vpn__wipe(buf, sizeof buf);
    return rc ? netman_vpn__fail("%s: %s", path, strerror(errno)) : 0;
}
static int netman_vpn__mullvad_connect(const char *profile, const netman_vpn_relay *r, const netman_vpn_relay *via) {
    char path[PATH_MAX], priv[45] = "", addr[160] = "";
    snprintf(path, sizeof path, "%s/%s.conf", netman_vpn_conf_dir, profile);
    size_t l; char *old = netman_vpn__slurp(path, &l, 1 << 16, 1);
    if (old) {   /* reuse the registered device: keep PrivateKey + Address, swap only the relay */
        netman_vpn_wg c;
        if (netman_vpn_wg_parse(old, l, &c, NULL, 0) == 0 && c.has_priv && c.naddr) {
            netman_vpn_b64_encode32(c.priv, priv);
            char a4[64]; netman_vpn__ip_str(&c.addr[0], a4, sizeof a4); snprintf(addr, sizeof addr, "%s/%d", a4, c.addr[0].prefix);
            for (size_t i = 1; i < c.naddr && strlen(addr) < 60; i++) { netman_vpn__ip_str(&c.addr[i], a4, sizeof a4); size_t o = strlen(addr); snprintf(addr + o, sizeof addr - o, ", %s/%d", a4, c.addr[i].prefix); }
            netman_vpn_wg_free(&c);
        }
        netman_vpn__wipe(old, l); free(old);
    }
    if (!priv[0]) {    /* register a new device (needs an account number; NOT tested against the live API) */
        char *acct = netman_vpn__secret("NETMAN_VPN_MULLVAD_ACCOUNT_FILE"); if (!acct) return 1;
        unsigned char k[32], pub[32]; char pubb[45];
        if (netman_vpn_genkey(k)) { free(acct); return netman_vpn__fail("no randomness"); }
        netman_vpn_pubkey(k, pub); netman_vpn_b64_encode32(pub, pubb); netman_vpn_b64_encode32(k, priv);
        char q[256], cfg[1024];
        snprintf(cfg, sizeof cfg, "url = \"https://api.mullvad.net/auth/v1/token\"\nheader = \"Content-Type: application/json\"\n");
        char body[200]; snprintf(body, sizeof body, "{\"account_number\":\"%s\"}", acct);
        netman_vpn__cfg_quote(q, sizeof q, body); size_t o = strlen(cfg); snprintf(cfg + o, sizeof cfg - o, "data = %s\n", q);
        char *resp = netman_vpn__curl(cfg, NULL); netman_vpn__wipe(cfg, sizeof cfg); netman_vpn__wipe(body, sizeof body); free(acct);
        netman_vpn__jp p; char tok[256] = "";
        if (!resp || netman_vpn__jparse(&p, resp, strlen(resp))) { free(resp); netman_vpn__wipe(priv, sizeof priv); return netman_vpn__fail("mullvad: token request failed (check the account number)"); }
        netman_vpn__jcopy(&p, netman_vpn__jget(&p, 0, "access_token"), tok, sizeof tok); free(p.t); free(resp);
        if (!tok[0]) return netman_vpn__fail("mullvad: no access_token in the reply");
        char hdr[400]; snprintf(hdr, sizeof hdr, "Authorization: Bearer %s", tok); netman_vpn__cfg_quote(q, sizeof q, hdr);
        snprintf(cfg, sizeof cfg, "url = \"https://api.mullvad.net/accounts/v1/devices\"\nheader = \"Content-Type: application/json\"\nheader = %s\n", q);
        snprintf(body, sizeof body, "{\"pubkey\":\"%s\",\"hijack_dns\":false}", pubb); netman_vpn__cfg_quote(q, sizeof q, body);
        o = strlen(cfg); snprintf(cfg + o, sizeof cfg - o, "data = %s\n", q);
        resp = netman_vpn__curl(cfg, NULL); netman_vpn__wipe(cfg, sizeof cfg); netman_vpn__wipe(tok, sizeof tok); netman_vpn__wipe(hdr, sizeof hdr);
        char v4[64] = "", v6[96] = "";
        if (!resp || netman_vpn__jparse(&p, resp, strlen(resp))) { free(resp); netman_vpn__wipe(priv, sizeof priv); return netman_vpn__fail("mullvad: device registration failed (too many devices on the account?)"); }
        netman_vpn__jcopy(&p, netman_vpn__jget(&p, 0, "ipv4_address"), v4, sizeof v4); netman_vpn__jcopy(&p, netman_vpn__jget(&p, 0, "ipv6_address"), v6, sizeof v6); free(p.t); free(resp);
        netman_vpn_ip t; if (netman_vpn__ip_parse(v4, &t) || t.family != AF_INET) { netman_vpn__wipe(priv, sizeof priv); return netman_vpn__fail("mullvad: unexpected address in the reply"); }
        snprintf(addr, sizeof addr, "%s", v4);
        if (v6[0] && netman_vpn__ip_parse(v6, &t) == 0 && t.family == AF_INET6) { size_t o2 = strlen(addr); snprintf(addr + o2, sizeof addr - o2, ", %s", v6); }
    }
    const char *dns = getenv("NETMAN_VPN_MULLVAD_DNS");           /* 10.64.0.1 plain; 100.64.0.1 adblock; 100.64.0.7 ad+tracker+malware... (see mullvad docs) */
    char dnsb[64] = "10.64.0.1";
    if (dns) { netman_vpn_ip t; if (netman_vpn__ip_parse(dns, &t) == 0) snprintf(dnsb, sizeof dnsb, "%s", dns); }
    int rc;
    if (via) {                                                /* multihop: entry relay's address, exit relay's multihop port + key */
        if (!r->multihop_port) { netman_vpn__wipe(priv, sizeof priv); return netman_vpn__fail("relay %s has no multihop port", r->host); }
        rc = netman_vpn__write_profile(profile, priv, addr, dnsb, r, via->endpoint_ip, r->multihop_port);
    } else rc = netman_vpn__write_profile(profile, priv, addr, dnsb, r, r->endpoint_ip, r->port);
    netman_vpn__wipe(priv, sizeof priv);
    return rc;
}
static int netman_vpn__nord_connect(const char *profile, const netman_vpn_relay *r, const netman_vpn_relay *via) {
    if (via) return netman_vpn__fail("nordvpn: multihop is not supported through this path");
    char *tok = netman_vpn__secret("NETMAN_VPN_NORD_TOKEN_FILE"); if (!tok) return 1;
    char cfg[512], q[300], user[300];
    snprintf(user, sizeof user, "token:%s", tok); netman_vpn__cfg_quote(q, sizeof q, user);
    snprintf(cfg, sizeof cfg, "url = \"https://api.nordvpn.com/v1/users/services/credentials\"\nuser = %s\n", q);
    netman_vpn__wipe(user, sizeof user); netman_vpn__wipe(tok, strlen(tok)); free(tok);
    char *resp = netman_vpn__curl(cfg, NULL); netman_vpn__wipe(cfg, sizeof cfg);
    netman_vpn__jp p; char priv[64] = "";
    if (!resp || netman_vpn__jparse(&p, resp, strlen(resp))) { free(resp); return netman_vpn__fail("nordvpn: credentials request failed (bad or expired token?)"); }
    netman_vpn__jcopy(&p, netman_vpn__jget(&p, 0, "nordlynx_private_key"), priv, sizeof priv); free(p.t); netman_vpn__wipe(resp, strlen(resp)); free(resp);
    unsigned char k[32];
    if (netman_vpn_b64_decode32(priv, k)) { netman_vpn__wipe(priv, sizeof priv); return netman_vpn__fail("nordvpn: no valid nordlynx_private_key in the reply"); }
    netman_vpn__wipe(k, 32);
    int rc = netman_vpn__write_profile(profile, priv, "10.5.0.2/32", "103.86.96.100, 103.86.99.100", r, r->endpoint_ip, r->port);
    netman_vpn__wipe(priv, sizeof priv);
    return rc;
}
const netman_vpn_provider netman_vpn_providers[] = {
    {"mullvad", netman_vpn_mullvad_parse, "https://api.mullvad.net/public/relays/wireguard/v1/", netman_vpn__mullvad_connect},
    {"nordvpn", netman_vpn_nord_parse, "https://api.nordvpn.com/v1/servers/recommendations?filters%5Bservers_technologies%5D%5Bidentifier%5D=wireguard_udp&limit=200", netman_vpn__nord_connect},
    {NULL, NULL, NULL, NULL}};

static const netman_vpn_provider *netman_vpn__prov(const char *n) { for (const netman_vpn_provider *p = netman_vpn_providers; p->name; p++) if (n && strcmp(p->name, n) == 0) return p; return NULL; }
#define NV_MAXRELAYS 4096
static int netman_vpn__fetch_relays(const netman_vpn_provider *pv, netman_vpn_relay **out) {
    char cfg[1024], q[900]; netman_vpn__cfg_quote(q, sizeof q, pv->list_url); snprintf(cfg, sizeof cfg, "url = %s\n", q);
    size_t l; char *j = netman_vpn__curl(cfg, &l);
    if (!j) { netman_vpn__fail("%s: could not fetch the relay list (offline? curl missing?)", pv->name); return -1; }
    *out = malloc(sizeof(netman_vpn_relay) * NV_MAXRELAYS);
    int n = *out ? pv->parse(j, l, *out, NV_MAXRELAYS) : -1;
    free(j);
    if (n <= 0) { netman_vpn__fail("%s: relay list is empty or unparsable", pv->name); free(*out); *out = NULL; return -1; }
    return n;
}
NETMAN_VPN_API int netman_vpn_provider_list(const char *provider, const char *country) {
    const netman_vpn_provider *pv = netman_vpn__prov(provider); if (!pv) return netman_vpn__fail("unknown provider %s", provider ? provider : "");
    netman_vpn_relay *r; int n = netman_vpn__fetch_relays(pv, &r); if (n < 0) return 1;
    for (int i = 0; i < n; i++) if (!country || strcasecmp(country, r[i].country) == 0) netman_vpn__say("%-24s %-3s %-12s %-16s load %d", r[i].host, r[i].country, r[i].city, r[i].endpoint_ip, r[i].load);
    free(r); return 0;
}
/* pure selection over a parsed list (testable): returns index or -1 */
static int netman_vpn__select(const netman_vpn_relay *r, int n, const char *country, const char *city, int fastest) {
    int best = -1, bestscore = 1 << 30, tested = 0;
    for (int i = 0; i < n; i++) {
        if (country && *country && strcasecmp(country, r[i].country)) continue;
        if (city && *city && strcasecmp(city, r[i].city)) continue;
        int score = r[i].load >= 0 ? r[i].load : 50;
        if (fastest && tested < 12) { int rtt = netman_vpn_rtt_ms(r[i].endpoint_ip, 800); tested++; score = rtt >= 0 ? rtt : 5000; }
        else if (fastest) score = 1 << 20;
        if (score < bestscore || best < 0) { best = i; bestscore = score; }
    }
    return best;
}
NETMAN_VPN_API int netman_vpn_provider_pick(const char *provider, const char *country, const char *city, int fastest, netman_vpn_relay *out) {
    const netman_vpn_provider *pv = netman_vpn__prov(provider); if (!pv) return netman_vpn__fail("unknown provider %s", provider ? provider : "");
    netman_vpn_relay *r; int n = netman_vpn__fetch_relays(pv, &r); if (n < 0) return 1;
    int i = netman_vpn__select(r, n, country, city, fastest);
    if (i < 0) { free(r); return netman_vpn__fail("no relay matches that location"); }
    *out = r[i]; free(r); return 0;
}
NETMAN_VPN_API int netman_vpn_provider_connect(const char *provider, const char *profile, const char *country, const char *city, int fastest, const char *via_country) {
    const netman_vpn_provider *pv = netman_vpn__prov(provider); if (!pv) return netman_vpn__fail("unknown provider %s", provider ? provider : "");
    netman_vpn_relay *r; int n = netman_vpn__fetch_relays(pv, &r); if (n < 0) return 1;
    int i = netman_vpn__select(r, n, country, city, fastest), v = -1;
    if (via_country && *via_country) v = netman_vpn__select(r, n, via_country, NULL, fastest);
    if (i < 0 || (via_country && *via_country && v < 0)) { free(r); return netman_vpn__fail("no relay matches that location"); }
    netman_vpn_relay x = r[i], e; if (v >= 0) e = r[v];
    free(r);
    int rc = pv->connect(profile, &x, v >= 0 ? &e : NULL);
    if (!rc) netman_vpn__say("%s: profile written for %s (%s%s%s)", profile, x.host, x.country, v >= 0 ? " via " : "", v >= 0 ? e.host : "");
    return rc;
}

/* ---------------------------------------------------------------- command front end */
static void netman_vpn__usage(void) {
    fputs("usage: netman vpn list | up NAME [secs] | down NAME | status [NAME] | info NAME | logs NAME [N]\n"
          "       netman vpn import FILE [NAME]      copy a WireGuard / OpenVPN config into the profile dir (mode 0600)\n"
          "       netman vpn ks NAME on|off          kill switch (nftables, atomic, survives tunnel flaps; NETMAN_VPN_KS_LAN=1 allows LAN)\n"
          "       netman vpn run NAME CMD [ARGS]     run one command through the tunnel in its own network namespace\n"
          "       netman vpn watch NAME [interval [stale]]   reconnect when the tunnel dies or the handshake gets stale\n"
          "       netman vpn rtt IP | probe IP       ICMP rtt / path-MTU search (IPv4)\n"
          "       netman vpn providers | relays PROVIDER [CC] | connect PROVIDER NAME [CC [CITY]] [--fastest] [--via CC]\n"
          "       netman vpn genkey | pubkey         (pubkey reads a private key on stdin)\n", NV_ERR);
}
NETMAN_VPN_API int netman_vpn_cmd(int argc, char **argv) {
    netman_vpn_env();
    if (argc < 2) { netman_vpn__usage(); return 2; }
    const char *c = argv[1];
    if (!strcmp(c, "list")) return netman_vpn_list();
    if (!strcmp(c, "import") && argc >= 3) return netman_vpn_import(argv[2], argc > 3 ? argv[3] : NULL);
    if (!strcmp(c, "info") && argc >= 3) return netman_vpn_info(argv[2]);
    if (!strcmp(c, "up") && argc >= 3) return netman_vpn_up(argv[2], argc > 3 ? atoi(argv[3]) : 30);
    if (!strcmp(c, "down") && argc >= 3) return netman_vpn_down(argv[2]);
    if (!strcmp(c, "status")) {
        if (argc >= 3) return netman_vpn_status(argv[2]);
        DIR *d = opendir(netman_vpn_conf_dir); if (!d) return netman_vpn__fail("%s: %s", netman_vpn_conf_dir, strerror(errno));
        struct dirent *en; while ((en = readdir(d))) { char *dot = strrchr(en->d_name, '.'); if (!dot || (strcmp(dot, ".conf") && strcmp(dot, ".ovpn") && strcmp(dot, ".vpn"))) continue; char nm[256]; snprintf(nm, sizeof nm, "%.*s", (int)(dot - en->d_name), en->d_name); if (!netman_vpn_valid_name(nm, 32)) continue; netman_vpn__mute = 1; int has = netman_vpn_type(nm); netman_vpn__mute = 0; if (has) netman_vpn_status(nm); }
        closedir(d); return 0;
    }
    if (!strcmp(c, "logs") && argc >= 3) return netman_vpn_logs(argv[2], argc > 3 ? atoi(argv[3]) : 40);
    if ((!strcmp(c, "ks") || !strcmp(c, "killswitch")) && argc >= 4 && (!strcmp(argv[3], "on") || !strcmp(argv[3], "off"))) return netman_vpn_killswitch(argv[2], !strcmp(argv[3], "on"));
    if (!strcmp(c, "run") && argc >= 4) return netman_vpn_run(argv[2], argv + 3);
    if (!strcmp(c, "watch") && argc >= 3) return netman_vpn_watch(argv[2], argc > 3 ? atoi(argv[3]) : 15, argc > 4 ? atoi(argv[4]) : 180);
    if (!strcmp(c, "rtt") && argc >= 3) { int r = netman_vpn_rtt_ms(argv[2], 1000); if (r < 0) return netman_vpn__fail("no reply from %s", argv[2]); netman_vpn__say("%d ms", r); return 0; }
    if (!strcmp(c, "probe") && argc >= 3) { int m = netman_vpn_mtu_probe(argv[2]); if (m < 0) return netman_vpn__fail("%s does not answer pings (or no unprivileged ping socket: net.ipv4.ping_group_range)", argv[2]); netman_vpn__say("path mtu %d (set MTU = %d in the profile for a tunnel over this path: subtract 60 for IPv4 / 80 for IPv6 transport)", m, m - 60); return 0; }
    if (!strcmp(c, "providers")) { for (const netman_vpn_provider *p = netman_vpn_providers; p->name; p++) netman_vpn__say("%s", p->name); return 0; }
    if (!strcmp(c, "relays") && argc >= 3) return netman_vpn_provider_list(argv[2], argc > 3 ? argv[3] : NULL);
    if (!strcmp(c, "connect") && argc >= 4) {
        const char *cc = NULL, *city = NULL, *via = NULL; int fast = 0, pos = 0;
        for (int i = 4; i < argc; i++) { if (!strcmp(argv[i], "--fastest")) fast = 1; else if (!strcmp(argv[i], "--via") && i + 1 < argc) via = argv[++i]; else if (pos == 0) { cc = argv[i]; pos++; } else if (pos == 1) { city = argv[i]; pos++; } }
        int rc = netman_vpn_provider_connect(argv[2], argv[3], cc, city, fast, via);
        return rc ? rc : netman_vpn_up(argv[3], 30);
    }
    if (!strcmp(c, "genkey")) { unsigned char k[32]; char b[45]; if (netman_vpn_genkey(k)) return 1; netman_vpn_b64_encode32(k, b); puts(b); netman_vpn__wipe(k, 32); netman_vpn__wipe(b, sizeof b); return 0; }
    if (!strcmp(c, "pubkey")) { char in[128]; unsigned char k[32], p[32]; char b[45]; if (!fgets(in, sizeof in, stdin)) return 1; in[strcspn(in, "\r\n")] = 0; if (netman_vpn_b64_decode32(in, k)) return netman_vpn__fail("not a valid key"); netman_vpn_pubkey(k, p); netman_vpn_b64_encode32(p, b); puts(b); netman_vpn__wipe(k, 32); netman_vpn__wipe(in, sizeof in); return 0; }
    netman_vpn__usage(); return 2;
}

#endif /* NETMAN_VPN_IMPLEMENTATION_ONCE */
#endif /* NETMAN_VPN_IMPLEMENTATION */
