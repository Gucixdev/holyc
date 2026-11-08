/* firewall.h - firewall layer of netman (NetworkManager drop-in): one rule model, rendered for nftables or iptables.
 *
 * stb-style: `#define NETMAN_FW_IMPLEMENTATION` in exactly one .c file. Needs only libc + the `nft` binary
 * (or iptables-restore / ip6tables-restore as the legacy fallback). No Python, no D-Bus, no firewalld.
 *
 * What it owns, and nothing else:   nftables:  tables "inet netman", "arp netman_arp", "inet netman_vpn_ks" (kill switch)
 *                                   iptables:  chains NETMAN, NETMAN_OUT, NETMAN_FWD (+ NETMAN_POST/NETMAN_PRE in nat)
 * Any other table in a plan is refused, so a bug here can never flush the user's own rules.
 *
 *   model        nmfw_cfg = default policies + rules + per-interface zones + NAT/share + guards
 *   presets      lockdown, workstation, share (hotspot NAT), killswitch (output default-drop, only tunnel+endpoints)
 *                nmfw_preset(), nmfw_share(), nmfw_preset_killswitch()
 *   zones        trusted / home / public / untrusted per interface (the profile's `zone` key): trusted = accept all,
 *                home = mdns+ping, public = stealth+limits+name-leak guard, untrusted = public + NOTHING in (only dhcp/ND replies)
 *   guards       gateway ARP pin, DHCP server pin, IPv6 RA guard, reverse-path filter, new-connection rate limit,
 *                ICMP policy, name-leak guard on public networks (SMB/NetBIOS/mDNS/SSDP never leave a coffee-shop wifi)
 *   plan / diff  nmfw_plan() renders the exact ruleset text; nmfw_diff() compares it with the last applied plan
 *   apply        nmfw_apply(cfg, confirm_s): validate with `nft -c`, snapshot, one atomic transaction, and if
 *                confirm_s > 0 a detached watchdog restores the snapshot unless nmfw_confirm() arrives in time
 *   killswitch   nmfw_plan_killswitch() emits the same `inet netman_vpn_ks` table as vpn.h (so either can remove it)
 *
 * The renderer is pure (no I/O) and fuzzed; apply/rollback are tested only inside `unshare -Urn`. */
#ifndef NETMAN_FW_H
#define NETMAN_FW_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifndef NMFW_API
#define NMFW_API
#endif

#define NMFW_MAXRULES 128
#define NMFW_MAXZONES 16
#define NMFW_MAXPORTS 8
#define NMFW_MAXDNAT  8

enum { NMFW_IN, NMFW_OUT, NMFW_FWD };
enum { NMFW_ACCEPT, NMFW_DROP, NMFW_REJECT, NMFW_LOGDROP };
enum { NMFW_ANY, NMFW_TCP, NMFW_UDP, NMFW_ICMP, NMFW_ICMP6 };
enum { NMFW_CT_EST = 1, NMFW_CT_NEW = 2, NMFW_CT_INV = 4 };
enum { NMFW_TRUSTED, NMFW_HOME, NMFW_PUBLIC, NMFW_UNTRUSTED };
enum { NMFW_B_NONE, NMFW_B_NFT, NMFW_B_IPT };
enum { NMFW_ICMP_LIMIT, NMFW_ICMP_DROP, NMFW_ICMP_ALLOW };

typedef struct {
    int dir, act, proto, family;        /* family: 0 any, 4, 6 */
    char iface[17], oiface[17];         /* iface = in (IN/FWD) or out (OUT); oiface only for FWD; trailing '*' = wildcard */
    unsigned short ports[NMFW_MAXPORTS]; int nports;   /* destination ports (tcp/udp only) */
    char addr[64]; int addr_dst;        /* "10.0.0.0/8" or "fe80::/10"; matched as source unless addr_dst */
    unsigned ct;                        /* NMFW_CT_* mask, 0 = any */
    int rate;                           /* >0: matches only while under N packets/second */
    char comment[33];
} nmfw_rule;

typedef struct { char ip[64]; int port; int udp; } nmfw_endpoint;
typedef struct { char iface[17]; int level; } nmfw_zone;
typedef struct { char iface[17]; int proto; unsigned short port; char to[64]; unsigned short toport; } nmfw_dnat;

typedef struct {
    int pol_in, pol_out, pol_fwd;       /* NMFW_ACCEPT or NMFW_DROP */
    nmfw_rule rules[NMFW_MAXRULES]; int nrules;
    nmfw_zone zones[NMFW_MAXZONES]; int nzones;
    char wan[17], lan[17]; int masq;    /* share: lan -> wan forwarded and masqueraded */
    nmfw_dnat dnat[NMFW_MAXDNAT]; int ndnat;
    int icmp;                           /* NMFW_ICMP_* for echo-request */
    int limit_new;                      /* >0: more than N new connections/s from one packet stream are dropped */
    int rpfilter;                       /* reverse-path filter */
    int ra_guard; char ra_router[64];   /* drop router advertisements on non-trusted zones unless from ra_router */
    char dhcp_server[16];               /* pinned DHCPv4 server address; offers from others are dropped */
    char gw_ip[16], gw_mac[18];         /* pinned gateway: arp replies for gw_ip from another MAC are dropped */
} nmfw_cfg;

NMFW_API const char *nmfw_error(void);
NMFW_API void nmfw_cfg_init(nmfw_cfg *c);
NMFW_API int  nmfw_preset(nmfw_cfg *c, const char *name);                 /* lockdown | workstation */
NMFW_API int  nmfw_share(nmfw_cfg *c, const char *lan, const char *wan);  /* workstation + hotspot NAT */
NMFW_API int  nmfw_preset_killswitch(nmfw_cfg *c, const char *tun, const nmfw_endpoint *eps, int n); /* workstation + output drop except tunnel/endpoints */
NMFW_API int  nmfw_set_zone(nmfw_cfg *c, const char *iface, const char *level);
NMFW_API int  nmfw_parse_rule(const char *line, nmfw_rule *r);            /* "allow in tcp port 22 from 10.0.0.0/8 on wl0 rate 5" */
NMFW_API int  nmfw_add_rule(nmfw_cfg *c, const nmfw_rule *r);
NMFW_API int  nmfw_check(const nmfw_cfg *c);                              /* 0 ok, else nmfw_error() */
NMFW_API int  nmfw_plan(const nmfw_cfg *c, char *out, size_t cap);        /* nft text; length or -1 */
NMFW_API int  nmfw_plan_arp(const nmfw_cfg *c, char *out, size_t cap);    /* arp table text ("" if no gateway pin) */
NMFW_API int  nmfw_plan_ipt(const nmfw_cfg *c, int v6, char *out, size_t cap);
NMFW_API int  nmfw_plan_killswitch(const char *tun, const nmfw_endpoint *eps, int n, int allow_lan, char *out, size_t cap);

extern const char *nmfw_state_dir;                                       /* default /run/netman */
NMFW_API int  nmfw_backend(void);                                         /* NMFW_B_* */
extern int nmfw_force_backend;                                         /* 0 auto, else NMFW_B_* */
NMFW_API int  nmfw_apply(const nmfw_cfg *c, int confirm_s);
NMFW_API int  nmfw_confirm(void);
NMFW_API int  nmfw_rollback(void);
NMFW_API int  nmfw_remove(void);                                          /* delete everything netman owns */
NMFW_API int  nmfw_diff(const nmfw_cfg *c, char *out, size_t cap);
NMFW_API int  nmfw_killswitch(const char *tun, const nmfw_endpoint *eps, int n, int allow_lan, int on);
#ifdef __cplusplus
}
#endif
#endif /* NETMAN_FW_H */

#ifdef NETMAN_FW_IMPLEMENTATION
#ifndef NETMAN_FW_IMPL_ONCE
#define NETMAN_FW_IMPL_ONCE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const char *nmfw_state_dir = "/run/netman";
int nmfw_force_backend = 0;
static char nmfw__err[256];
const char *nmfw_error(void) { return nmfw__err; }
static int nmfw__fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(nmfw__err, sizeof nmfw__err, fmt, ap); va_end(ap);
    return -1;
}

/* ---------------------------------------------------------------- validation: nothing unvalidated reaches a ruleset */
static int nmfw__iface_ok(const char *s) {
    size_t n = strlen(s);
    if (n == 0 || n > 15) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '_' || c == '.' || c == '-') continue;
        if (c == '*' && i == n - 1 && n > 1) continue;
        return 0;
    }
    return 1;
}
/* "a.b.c.d[/n]" or ipv6[/n]; family 4/6 out */
static int nmfw__addr_ok(const char *s, int *fam) {
    char b[64]; size_t n = strlen(s);
    if (n == 0 || n >= sizeof b) return 0;
    memcpy(b, s, n + 1);
    char *sl = strchr(b, '/'); int pre = -1;
    if (sl) {
        *sl++ = 0;
        if (!*sl || strlen(sl) > 3) return 0;
        for (char *p = sl; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
        pre = atoi(sl);
    }
    unsigned char buf[16];
    if (inet_pton(AF_INET, b, buf) == 1) { if (pre > 32) return 0; *fam = 4; return 1; }
    if (strchr(b, ':') && inet_pton(AF_INET6, b, buf) == 1) { if (pre > 128) return 0; *fam = 6; return 1; }
    return 0;
}
static int nmfw__ip_ok(const char *s, int *fam) { return !strchr(s, '/') && nmfw__addr_ok(s, fam); }
static int nmfw__comment_ok(const char *s) {
    if (strlen(s) > 32) return 0;
    for (; *s; s++) if (!(isalnum((unsigned char)*s) || *s == ' ' || *s == '_' || *s == '.' || *s == ':' || *s == '-')) return 0;
    return 1;
}
static int nmfw__mac_ok(const char *s) {
    if (strlen(s) != 17) return 0;
    for (int i = 0; i < 17; i++) {
        if (i % 3 == 2) { if (s[i] != ':') return 0; }
        else if (!isxdigit((unsigned char)s[i])) return 0;
    }
    return 1;
}

static int nmfw__rule_check(const nmfw_rule *r) {
    int fam = 0;
    if (r->dir < NMFW_IN || r->dir > NMFW_FWD) return nmfw__fail("bad direction");
    if (r->act < NMFW_ACCEPT || r->act > NMFW_LOGDROP) return nmfw__fail("bad action");
    if (r->proto < NMFW_ANY || r->proto > NMFW_ICMP6) return nmfw__fail("bad protocol");
    if (r->family != 0 && r->family != 4 && r->family != 6) return nmfw__fail("bad family");
    if (r->iface[0] && !nmfw__iface_ok(r->iface)) return nmfw__fail("bad interface name");
    if (r->oiface[0] && !nmfw__iface_ok(r->oiface)) return nmfw__fail("bad interface name");
    if (r->oiface[0] && r->dir != NMFW_FWD) return nmfw__fail("'via' is only for fwd rules");
    if (r->nports < 0 || r->nports > NMFW_MAXPORTS) return nmfw__fail("too many ports");
    if (r->nports && r->proto != NMFW_TCP && r->proto != NMFW_UDP) return nmfw__fail("ports need tcp or udp");
    for (int i = 0; i < r->nports; i++) if (r->ports[i] == 0) return nmfw__fail("port 0");
    if (r->addr[0]) {
        if (!nmfw__addr_ok(r->addr, &fam)) return nmfw__fail("bad address");
        if (r->family && r->family != fam) return nmfw__fail("address family does not match v4/v6");
        if ((r->proto == NMFW_ICMP && fam == 6) || (r->proto == NMFW_ICMP6 && fam == 4)) return nmfw__fail("address family does not match protocol");
    }
    if (r->proto == NMFW_ICMP && r->family == 6) return nmfw__fail("icmp is IPv4 (use icmp6)");
    if (r->proto == NMFW_ICMP6 && r->family == 4) return nmfw__fail("icmp6 is IPv6");
    if (r->ct & ~(unsigned)(NMFW_CT_EST | NMFW_CT_NEW | NMFW_CT_INV)) return nmfw__fail("bad conntrack mask");
    if (r->rate < 0 || r->rate > 1000000) return nmfw__fail("bad rate");
    if (!nmfw__comment_ok(r->comment)) return nmfw__fail("comment may only contain letters, digits, space _ . : -");
    return 0;
}

/* ---------------------------------------------------------------- string builder */
typedef struct { char *p; size_t n, cap; int err; } nmfw__sb;
static void nmfw__put(nmfw__sb *b, const char *fmt, ...) {
    if (b->err) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->cap - b->n) { b->err = 1; if (b->cap) b->p[b->n] = 0; return; }
    b->n += (size_t)w;
}
static int nmfw__done(nmfw__sb *b) { if (b->err) return nmfw__fail("plan buffer too small"); return (int)b->n; }

/* ---------------------------------------------------------------- model */
void nmfw_cfg_init(nmfw_cfg *c) {
    memset(c, 0, sizeof *c);
    c->pol_in = NMFW_ACCEPT; c->pol_out = NMFW_ACCEPT; c->pol_fwd = NMFW_ACCEPT;
    c->icmp = NMFW_ICMP_ALLOW;
}
int nmfw_preset(nmfw_cfg *c, const char *name) {
    if (!name) return nmfw__fail("no preset name");
    nmfw_cfg_init(c);
    if (!strcmp(name, "lockdown")) {
        c->pol_in = NMFW_DROP; c->pol_fwd = NMFW_DROP; c->icmp = NMFW_ICMP_DROP;
        return 0;
    }
    if (!strcmp(name, "workstation")) {
        c->pol_in = NMFW_DROP; c->pol_fwd = NMFW_DROP; c->icmp = NMFW_ICMP_LIMIT;
        c->limit_new = 200; c->rpfilter = 1;
        return 0;
    }
    return nmfw__fail("unknown preset '%s' (lockdown, workstation; share needs nmfw_share, killswitch needs nmfw_preset_killswitch)", name);
}
int nmfw_set_zone(nmfw_cfg *c, const char *iface, const char *level) {
    int l;
    if (!iface || !nmfw__iface_ok(iface)) return nmfw__fail("bad interface name");
    if (!level) return nmfw__fail("no zone level");
    if (!strcmp(level, "trusted")) l = NMFW_TRUSTED; else if (!strcmp(level, "home")) l = NMFW_HOME;
    else if (!strcmp(level, "public")) l = NMFW_PUBLIC; else if (!strcmp(level, "untrusted")) l = NMFW_UNTRUSTED;
    else return nmfw__fail("unknown zone '%s' (trusted, home, public, untrusted)", level);
    for (int i = 0; i < c->nzones; i++) if (!strcmp(c->zones[i].iface, iface)) { c->zones[i].level = l; return 0; }
    if (c->nzones >= NMFW_MAXZONES) return nmfw__fail("too many zones");
    snprintf(c->zones[c->nzones].iface, 17, "%s", iface); c->zones[c->nzones++].level = l;
    return 0;
}
int nmfw_add_rule(nmfw_cfg *c, const nmfw_rule *r) {
    if (nmfw__rule_check(r)) return -1;
    if (c->nrules >= NMFW_MAXRULES) return nmfw__fail("too many rules");
    c->rules[c->nrules++] = *r;
    return 0;
}
int nmfw_share(nmfw_cfg *c, const char *lan, const char *wan) {
    if (!lan || !wan || !nmfw__iface_ok(lan) || !nmfw__iface_ok(wan)) return nmfw__fail("share needs a lan and a wan interface");
    if (!strcmp(lan, wan)) return nmfw__fail("lan and wan are the same interface");
    if (strchr(lan, '*') || strchr(wan, '*')) return nmfw__fail("share interfaces cannot be wildcards");
    if (nmfw_preset(c, "workstation")) return -1;
    snprintf(c->lan, 17, "%s", lan); snprintf(c->wan, 17, "%s", wan); c->masq = 1;
    nmfw_rule r; memset(&r, 0, sizeof r); r.dir = NMFW_IN; r.act = NMFW_ACCEPT; snprintf(r.iface, 17, "%s", lan);
    r.proto = NMFW_UDP; r.ports[0] = 67; r.nports = 1; snprintf(r.comment, 33, "share dhcp");
    if (nmfw_add_rule(c, &r)) return -1;
    r.ports[0] = 53; snprintf(r.comment, 33, "share dns udp");
    if (nmfw_add_rule(c, &r)) return -1;
    r.proto = NMFW_TCP; snprintf(r.comment, 33, "share dns tcp");
    if (nmfw_add_rule(c, &r)) return -1;
    return nmfw_set_zone(c, lan, "home");
}

int nmfw_preset_killswitch(nmfw_cfg *c, const char *tun, const nmfw_endpoint *eps, int n) {
    int fam;
    if (!tun || !nmfw__iface_ok(tun) || strchr(tun, '*')) return nmfw__fail("bad tunnel interface");
    if (!eps || n <= 0) return nmfw__fail("no endpoint: refusing a kill switch that would cut everything");
    if (n > NMFW_MAXRULES - 4) return nmfw__fail("too many endpoints");
    if (nmfw_preset(c, "workstation")) return -1;
    c->pol_out = NMFW_DROP;
    nmfw_rule r; memset(&r, 0, sizeof r); r.dir = NMFW_OUT; r.act = NMFW_ACCEPT; snprintf(r.iface, 17, "%s", tun); snprintf(r.comment, 33, "killswitch tunnel");
    if (nmfw_add_rule(c, &r)) return -1;
    /* DNS leak guard: resolvers only through the tunnel (the tunnel rule above matches first) */
    memset(&r, 0, sizeof r); r.dir = NMFW_OUT; r.act = NMFW_DROP; r.nports = 2; r.ports[0] = 53; r.ports[1] = 853; snprintf(r.comment, 33, "killswitch dns leak");
    r.proto = NMFW_UDP; if (nmfw_add_rule(c, &r)) return -1;
    r.proto = NMFW_TCP; if (nmfw_add_rule(c, &r)) return -1;
    for (int i = 0; i < n; i++) {
        if (!nmfw__ip_ok(eps[i].ip, &fam)) return nmfw__fail("bad endpoint address");
        if (eps[i].port < 1 || eps[i].port > 65535) return nmfw__fail("bad endpoint port");
        memset(&r, 0, sizeof r); r.dir = NMFW_OUT; r.act = NMFW_ACCEPT; r.proto = eps[i].udp ? NMFW_UDP : NMFW_TCP;
        r.nports = 1; r.ports[0] = (unsigned short)eps[i].port; snprintf(r.addr, sizeof r.addr, "%s", eps[i].ip); r.addr_dst = 1; snprintf(r.comment, 33, "killswitch endpoint");
        if (nmfw_add_rule(c, &r)) return -1;
    }
    return 0;
}

/* "allow|accept|drop|reject|logdrop in|out|fwd [tcp|udp|icmp|icmp6] [v4|v6] [port 22,80] [from A] [to A] [on IF] [via IF]
 *  [rate N] [new|est|invalid] [# comment]" */
static int nmfw__ports(const char *s, nmfw_rule *r) {
    char b[64]; if (strlen(s) >= sizeof b) return nmfw__fail("port list too long");
    snprintf(b, sizeof b, "%s", s);
    r->nports = 0;
    if (!*s || s[0] == ',' || s[strlen(s) - 1] == ',' || strstr(s, ",,")) return nmfw__fail("empty item in port list");
    for (char *sv, *t = strtok_r(b, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) {
        char *e; long v;
        if (r->nports >= NMFW_MAXPORTS) return nmfw__fail("too many ports");
        if (!isdigit((unsigned char)*t)) return nmfw__fail("bad port '%s'", t);
        v = strtol(t, &e, 10);
        if (*e || v < 1 || v > 65535) return nmfw__fail("bad port '%s'", t);
        r->ports[r->nports++] = (unsigned short)v;
    }
    return r->nports ? 0 : nmfw__fail("empty port list");
}
int nmfw_parse_rule(const char *line, nmfw_rule *r) {
    char b[256], *tok[40]; int nt = 0, haveproto = 0;
    memset(r, 0, sizeof *r);
    if (!line) return nmfw__fail("empty rule");
    size_t n = strlen(line);
    if (n >= sizeof b) return nmfw__fail("rule line too long");
    memcpy(b, line, n + 1);
    char *hash = strchr(b, '#');
    if (hash) {
        *hash++ = 0;
        while (*hash == ' ') hash++;
        size_t cl = strlen(hash); while (cl && (hash[cl - 1] == ' ' || hash[cl - 1] == '\n')) hash[--cl] = 0;
        if (!nmfw__comment_ok(hash)) return nmfw__fail("comment may only contain letters, digits, space _ . : -");
        snprintf(r->comment, sizeof r->comment, "%s", hash);
    }
    for (char *sv, *t = strtok_r(b, " \t\r\n", &sv); t; t = strtok_r(NULL, " \t\r\n", &sv)) {
        if (nt >= 40) return nmfw__fail("rule too long");
        tok[nt++] = t;
    }
    if (nt < 2) return nmfw__fail("rule needs an action and a direction");
    if (!strcmp(tok[0], "allow") || !strcmp(tok[0], "accept")) r->act = NMFW_ACCEPT;
    else if (!strcmp(tok[0], "drop")) r->act = NMFW_DROP;
    else if (!strcmp(tok[0], "reject")) r->act = NMFW_REJECT;
    else if (!strcmp(tok[0], "logdrop")) r->act = NMFW_LOGDROP;
    else return nmfw__fail("unknown action '%s'", tok[0]);
    if (!strcmp(tok[1], "in")) r->dir = NMFW_IN; else if (!strcmp(tok[1], "out")) r->dir = NMFW_OUT;
    else if (!strcmp(tok[1], "fwd")) r->dir = NMFW_FWD; else return nmfw__fail("unknown direction '%s'", tok[1]);
    for (int i = 2; i < nt; i++) {
        const char *t = tok[i];
        int needv = !strcmp(t, "port") || !strcmp(t, "from") || !strcmp(t, "to") || !strcmp(t, "on") || !strcmp(t, "via") || !strcmp(t, "rate");
        if (needv && i + 1 >= nt) return nmfw__fail("'%s' needs a value", t);
        if (!strcmp(t, "tcp") || !strcmp(t, "udp") || !strcmp(t, "icmp") || !strcmp(t, "icmp6")) {
            if (haveproto) return nmfw__fail("two protocols");
            haveproto = 1; r->proto = !strcmp(t, "tcp") ? NMFW_TCP : !strcmp(t, "udp") ? NMFW_UDP : !strcmp(t, "icmp") ? NMFW_ICMP : NMFW_ICMP6;
        } else if (!strcmp(t, "v4")) r->family = 4;
        else if (!strcmp(t, "v6")) r->family = 6;
        else if (!strcmp(t, "port")) { if (r->nports) return nmfw__fail("two port lists"); if (nmfw__ports(tok[++i], r)) return -1; }
        else if (!strcmp(t, "from") || !strcmp(t, "to")) {
            if (r->addr[0]) return nmfw__fail("two addresses");
            if (strlen(tok[i + 1]) >= sizeof r->addr) return nmfw__fail("bad address");
            snprintf(r->addr, sizeof r->addr, "%s", tok[++i]); r->addr_dst = (t[0] == 't');
        } else if (!strcmp(t, "on")) { if (r->iface[0]) return nmfw__fail("two interfaces"); if (strlen(tok[i + 1]) > 16) return nmfw__fail("bad interface name"); snprintf(r->iface, 17, "%s", tok[++i]); }
        else if (!strcmp(t, "via")) { if (r->oiface[0]) return nmfw__fail("two via interfaces"); if (strlen(tok[i + 1]) > 16) return nmfw__fail("bad interface name"); snprintf(r->oiface, 17, "%s", tok[++i]); }
        else if (!strcmp(t, "rate")) {
            char *e; long v; const char *a = tok[++i];
            if (!isdigit((unsigned char)*a)) return nmfw__fail("bad rate");
            v = strtol(a, &e, 10);
            if (*e || v < 1 || v > 1000000) return nmfw__fail("bad rate");
            r->rate = (int)v;
        } else if (!strcmp(t, "new")) r->ct |= NMFW_CT_NEW;
        else if (!strcmp(t, "est")) r->ct |= NMFW_CT_EST;
        else if (!strcmp(t, "invalid")) r->ct |= NMFW_CT_INV;
        else return nmfw__fail("unknown word '%.20s'", t);
    }
    return nmfw__rule_check(r);
}

int nmfw_check(const nmfw_cfg *c) {
    int fam;
    if (c->pol_in != NMFW_ACCEPT && c->pol_in != NMFW_DROP) return nmfw__fail("bad input policy");
    if (c->pol_out != NMFW_ACCEPT && c->pol_out != NMFW_DROP) return nmfw__fail("bad output policy");
    if (c->pol_fwd != NMFW_ACCEPT && c->pol_fwd != NMFW_DROP) return nmfw__fail("bad forward policy");
    if (c->nrules < 0 || c->nrules > NMFW_MAXRULES || c->nzones < 0 || c->nzones > NMFW_MAXZONES || c->ndnat < 0 || c->ndnat > NMFW_MAXDNAT) return nmfw__fail("bad counts");
    for (int i = 0; i < c->nrules; i++) if (nmfw__rule_check(&c->rules[i])) return -1;
    for (int i = 0; i < c->nzones; i++) {
        if (!nmfw__iface_ok(c->zones[i].iface)) return nmfw__fail("bad zone interface");
        if (c->zones[i].level < NMFW_TRUSTED || c->zones[i].level > NMFW_UNTRUSTED) return nmfw__fail("bad zone level");
    }
    if (c->masq && (!c->lan[0] || !c->wan[0] || !nmfw__iface_ok(c->lan) || !nmfw__iface_ok(c->wan))) return nmfw__fail("share needs lan and wan");
    if (c->icmp < NMFW_ICMP_LIMIT || c->icmp > NMFW_ICMP_ALLOW) return nmfw__fail("bad icmp policy");
    if (c->limit_new < 0 || c->limit_new > 1000000) return nmfw__fail("bad new-connection limit");
    if (c->ra_guard && !c->ra_router[0]) return nmfw__fail("ra_guard needs ra_router (the selected router's link-local address)");
    if (c->ra_router[0] && (!nmfw__ip_ok(c->ra_router, &fam) || fam != 6)) return nmfw__fail("bad ra_router");
    if (c->dhcp_server[0] && (!nmfw__ip_ok(c->dhcp_server, &fam) || fam != 4)) return nmfw__fail("bad dhcp_server");
    if ((c->gw_ip[0] != 0) != (c->gw_mac[0] != 0)) return nmfw__fail("gateway pin needs both gw_ip and gw_mac");
    if (c->gw_ip[0] && (!nmfw__ip_ok(c->gw_ip, &fam) || fam != 4)) return nmfw__fail("bad gw_ip");
    if (c->gw_mac[0] && !nmfw__mac_ok(c->gw_mac)) return nmfw__fail("bad gw_mac");
    for (int i = 0; i < c->ndnat; i++) {
        const nmfw_dnat *d = &c->dnat[i];
        if (!nmfw__iface_ok(d->iface) || (d->proto != NMFW_TCP && d->proto != NMFW_UDP) || !d->port || !d->toport || !nmfw__ip_ok(d->to, &fam) || fam != 4) return nmfw__fail("bad dnat entry %d", i);
    }
    return 0;
}

/* ---------------------------------------------------------------- nftables renderer */
static void nmfw__nft_ports(nmfw__sb *b, const char *proto, const unsigned short *p, int n) {
    if (n == 1) { nmfw__put(b, "%s dport %u ", proto, p[0]); return; }
    nmfw__put(b, "%s dport { ", proto);
    for (int i = 0; i < n; i++) nmfw__put(b, "%s%u", i ? ", " : "", p[i]);
    nmfw__put(b, " } ");
}
static void nmfw__nft_rule(nmfw__sb *b, const nmfw_rule *r) {
    int fam = r->family;
    if (r->addr[0]) nmfw__addr_ok(r->addr, &fam);
    if (r->family && !r->addr[0] && r->proto != NMFW_ICMP && r->proto != NMFW_ICMP6) nmfw__put(b, "meta nfproto %s ", r->family == 4 ? "ipv4" : "ipv6");
    if (r->iface[0]) nmfw__put(b, "%s \"%s\" ", r->dir == NMFW_OUT ? "oifname" : "iifname", r->iface);
    if (r->oiface[0]) nmfw__put(b, "oifname \"%s\" ", r->oiface);
    if (r->addr[0]) nmfw__put(b, "%s %s %s ", fam == 4 ? "ip" : "ip6", r->addr_dst ? "daddr" : "saddr", r->addr);
    if (r->proto == NMFW_TCP || r->proto == NMFW_UDP) {
        const char *p = r->proto == NMFW_TCP ? "tcp" : "udp";
        if (r->nports) nmfw__nft_ports(b, p, r->ports, r->nports); else nmfw__put(b, "meta l4proto %s ", p);
    } else if (r->proto == NMFW_ICMP) nmfw__put(b, "meta l4proto icmp ");
    else if (r->proto == NMFW_ICMP6) nmfw__put(b, "meta l4proto ipv6-icmp ");
    if (r->ct) {
        const char *s[3]; int k = 0;
        if (r->ct & NMFW_CT_EST) { s[k++] = "established"; s[k++] = "related"; }
        if (r->ct & NMFW_CT_NEW) s[k++] = "new";
        if (r->ct & NMFW_CT_INV) s[k++] = "invalid";
        if (k == 1) nmfw__put(b, "ct state %s ", s[0]);
        else { nmfw__put(b, "ct state { "); for (int i = 0; i < k; i++) nmfw__put(b, "%s%s", i ? ", " : "", s[i]); nmfw__put(b, " } "); }
    }
    if (r->rate) nmfw__put(b, "limit rate %d/second ", r->rate);
    switch (r->act) {
    case NMFW_ACCEPT: nmfw__put(b, "accept"); break;
    case NMFW_DROP: nmfw__put(b, "drop"); break;
    case NMFW_REJECT: nmfw__put(b, "reject"); break;
    default: nmfw__put(b, "log prefix \"netman: \" drop"); break;
    }
    if (r->comment[0]) nmfw__put(b, " comment \"%s\"", r->comment);   /* nft wants the comment after the verdict */
}
static const char *nmfw__pol(int p) { return p == NMFW_DROP ? "drop" : "accept"; }

int nmfw_plan(const nmfw_cfg *c, char *out, size_t cap) {
    nmfw__sb b = {out, 0, cap, 0};
    if (cap == 0) return nmfw__fail("plan buffer too small");
    out[0] = 0;
    if (nmfw_check(c)) return -1;
    nmfw__put(&b, "table inet netman\ndelete table inet netman\ntable inet netman {\n");
    if (c->rpfilter)
        nmfw__put(&b, " chain c_pre {\n  type filter hook prerouting priority -300; policy accept;\n"
                      "  udp sport 68 udp dport 67 accept\n  ip saddr 0.0.0.0 accept\n  fib saddr . iif oif missing drop\n }\n");
    /* ---- input */
    nmfw__put(&b, " chain c_in {\n  type filter hook input priority 0; policy %s;\n", nmfw__pol(c->pol_in));
    nmfw__put(&b, "  iifname \"lo\" accept\n  ct state { established, related } accept\n  ct state invalid drop\n");
    if (c->dhcp_server[0]) nmfw__put(&b, "  udp sport 67 udp dport 68 ip saddr != %s drop comment \"pinned dhcp server\"\n", c->dhcp_server);
    for (int i = 0; i < c->nzones; i++) {
        const nmfw_zone *z = &c->zones[i];
        if (z->level == NMFW_TRUSTED) { nmfw__put(&b, "  iifname \"%s\" accept\n", z->iface); continue; }
        if (z->level == NMFW_HOME) {
            nmfw__put(&b, "  iifname \"%s\" udp dport 5353 accept comment \"home mdns\"\n", z->iface);
            if (c->icmp != NMFW_ICMP_DROP) nmfw__put(&b, "  iifname \"%s\" icmp type echo-request accept\n", z->iface);
            continue;
        }
        if (z->level == NMFW_UNTRUSTED) {          /* nothing gets in, whatever the rules say: only dhcp/ND replies */
            if (c->ra_guard)
                nmfw__put(&b, "  iifname \"%s\" icmpv6 type nd-router-advert ip6 saddr != %s drop comment \"ra guard\"\n", z->iface, c->ra_router);
            nmfw__put(&b, "  iifname \"%s\" udp sport 67 udp dport 68 accept\n  iifname \"%s\" udp sport 547 udp dport 546 accept\n", z->iface, z->iface);
            nmfw__put(&b, "  iifname \"%s\" icmpv6 type { nd-router-advert, nd-neighbor-solicit, nd-neighbor-advert } accept\n  iifname \"%s\" drop comment \"untrusted zone\"\n", z->iface, z->iface);
            continue;
        }
        /* public / untrusted: stealth first, so the general icmp rule below never sees these */
        nmfw__put(&b, "  iifname \"%s\" icmp type echo-request drop\n  iifname \"%s\" icmpv6 type echo-request drop\n", z->iface, z->iface);
        if (c->ra_guard)
            nmfw__put(&b, "  iifname \"%s\" icmpv6 type nd-router-advert ip6 saddr != %s drop comment \"ra guard\"\n", z->iface, c->ra_router);
        nmfw__put(&b, "  iifname \"%s\" ct state new limit rate over 30/second burst 60 packets drop\n", z->iface);
    }
    if (c->limit_new) nmfw__put(&b, "  ct state new limit rate over %d/second burst %d packets drop\n", c->limit_new, c->limit_new * 2);
    for (int i = 0; i < c->nrules; i++) if (c->rules[i].dir == NMFW_IN) { nmfw__put(&b, "  "); nmfw__nft_rule(&b, &c->rules[i]); nmfw__put(&b, "\n"); }
    if (c->pol_in == NMFW_DROP) {
        nmfw__put(&b, "  udp sport 67 udp dport 68 accept comment \"dhcp client\"\n");
        nmfw__put(&b, "  udp sport 547 udp dport 546 accept comment \"dhcpv6 client\"\n");
        nmfw__put(&b, "  icmpv6 type { destination-unreachable, packet-too-big, time-exceeded, parameter-problem, nd-router-advert, nd-neighbor-solicit, nd-neighbor-advert } accept\n");
    }
    if (c->icmp == NMFW_ICMP_LIMIT) nmfw__put(&b, "  icmp type echo-request limit rate 5/second accept\n  icmpv6 type echo-request limit rate 5/second accept\n");
    else if (c->icmp == NMFW_ICMP_ALLOW && c->pol_in == NMFW_DROP) nmfw__put(&b, "  icmp type echo-request accept\n  icmpv6 type echo-request accept\n");
    nmfw__put(&b, " }\n");
    /* ---- forward */
    nmfw__put(&b, " chain c_fwd {\n  type filter hook forward priority 0; policy %s;\n  ct state { established, related } accept\n  ct state invalid drop\n", nmfw__pol(c->pol_fwd));
    if (c->masq) nmfw__put(&b, "  iifname \"%s\" oifname \"%s\" accept comment \"share\"\n", c->lan, c->wan);
    for (int i = 0; i < c->nrules; i++) if (c->rules[i].dir == NMFW_FWD) { nmfw__put(&b, "  "); nmfw__nft_rule(&b, &c->rules[i]); nmfw__put(&b, "\n"); }
    if (c->ndnat) nmfw__put(&b, "  ct status dnat accept\n");
    nmfw__put(&b, " }\n");
    /* ---- output */
    nmfw__put(&b, " chain c_out {\n  type filter hook output priority 0; policy %s;\n  oifname \"lo\" accept\n", nmfw__pol(c->pol_out));
    for (int i = 0; i < c->nzones; i++) {
        const nmfw_zone *z = &c->zones[i];
        if (z->level < NMFW_PUBLIC) continue;      /* name-leak guard: nothing that announces this machine leaves a public network */
        nmfw__put(&b, "  oifname \"%s\" tcp dport { 135, 139, 445 } drop comment \"no smb on public wifi\"\n", z->iface);
        nmfw__put(&b, "  oifname \"%s\" udp dport { 137, 138, 1900, 5353 } drop comment \"no netbios/ssdp/mdns on public wifi\"\n", z->iface);
    }
    for (int i = 0; i < c->nrules; i++) if (c->rules[i].dir == NMFW_OUT) { nmfw__put(&b, "  "); nmfw__nft_rule(&b, &c->rules[i]); nmfw__put(&b, "\n"); }
    if (c->pol_out == NMFW_DROP)
        nmfw__put(&b, "  ct state { established, related } accept\n  udp sport 68 udp dport 67 accept comment \"dhcp client\"\n  udp sport 546 udp dport 547 accept comment \"dhcpv6 client\"\n"
                      "  icmpv6 type { nd-router-solicit, nd-neighbor-solicit, nd-neighbor-advert } accept\n");
    nmfw__put(&b, " }\n");
    /* ---- nat */
    if (c->masq) nmfw__put(&b, " chain c_post {\n  type nat hook postrouting priority 100; policy accept;\n  oifname \"%s\" masquerade\n }\n", c->wan);
    if (c->ndnat) {
        nmfw__put(&b, " chain c_dnat {\n  type nat hook prerouting priority -100; policy accept;\n");
        for (int i = 0; i < c->ndnat; i++) {
            const nmfw_dnat *d = &c->dnat[i];
            nmfw__put(&b, "  iifname \"%s\" %s dport %u dnat ip to %s:%u\n", d->iface, d->proto == NMFW_TCP ? "tcp" : "udp", d->port, d->to, d->toport);
        }
        nmfw__put(&b, " }\n");
    }
    nmfw__put(&b, "}\n");
    return nmfw__done(&b);
}

int nmfw_plan_arp(const nmfw_cfg *c, char *out, size_t cap) {
    nmfw__sb b = {out, 0, cap, 0};
    if (cap == 0) return nmfw__fail("plan buffer too small");
    out[0] = 0;
    if (nmfw_check(c)) return -1;
    if (!c->gw_ip[0]) return 0;
    nmfw__put(&b, "table arp netman_arp\ndelete table arp netman_arp\ntable arp netman_arp {\n chain in {\n  type filter hook input priority 0; policy accept;\n"
                  "  arp saddr ip %s arp saddr ether != %s drop comment \"pinned gateway\"\n }\n}\n", c->gw_ip, c->gw_mac);
    return nmfw__done(&b);
}

/* same text as vpn.h's kill switch, so `netman_vpn_killswitch(off)` and nmfw_killswitch(off) remove each other's table */
int nmfw_plan_killswitch(const char *tun, const nmfw_endpoint *eps, int n, int allow_lan, char *out, size_t cap) {
    nmfw__sb b = {out, 0, cap, 0};
    int fam;
    if (cap == 0) return nmfw__fail("plan buffer too small");
    out[0] = 0;
    if (!tun || !nmfw__iface_ok(tun)) return nmfw__fail("bad tunnel interface");
    if (n <= 0 || !eps) return nmfw__fail("no endpoint: refusing to arm a kill switch that would cut everything");
    nmfw__put(&b, "table inet netman_vpn_ks\ndelete table inet netman_vpn_ks\ntable inet netman_vpn_ks {\n chain out {\n  type filter hook output priority -10; policy drop;\n"
                  "  oifname \"lo\" accept\n  oifname \"%s\" accept\n", tun);
    for (int i = 0; i < n; i++) {
        if (!nmfw__ip_ok(eps[i].ip, &fam)) return nmfw__fail("bad endpoint address");
        if (eps[i].port < 1 || eps[i].port > 65535) return nmfw__fail("bad endpoint port");
        nmfw__put(&b, "  %s daddr %s %s dport %d accept\n", fam == 4 ? "ip" : "ip6", eps[i].ip, eps[i].udp ? "udp" : "tcp", eps[i].port);
    }
    nmfw__put(&b, "  udp sport 68 udp dport 67 accept\n  udp sport 546 udp dport 547 accept\n  meta l4proto { tcp, udp } th dport { 53, 853 } drop\n");
    if (allow_lan) nmfw__put(&b, "  ip daddr { 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16 } accept\n  ip6 daddr { fc00::/7, fe80::/10 } accept\n");
    nmfw__put(&b, " }\n}\n");
    return nmfw__done(&b);
}

/* ---------------------------------------------------------------- iptables renderer (legacy fallback) */
static int nmfw__ipt_skip(const nmfw_rule *r, int v6) {
    int fam = r->family;
    if (r->addr[0]) nmfw__addr_ok(r->addr, &fam);
    if (r->proto == NMFW_ICMP) fam = 4;
    if (r->proto == NMFW_ICMP6) fam = 6;
    return fam && fam != (v6 ? 6 : 4);
}
static void nmfw__ipt_if(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s); for (char *p = d; *p; p++) if (*p == '*') *p = '+'; }
static void nmfw__ipt_rule(nmfw__sb *b, const char *chain, const nmfw_rule *r, int v6) {
    char tail[512] = ""; nmfw__sb t = {tail, 0, sizeof tail, 0};
    char itf[20];
    if (r->iface[0]) { nmfw__ipt_if(itf, sizeof itf, r->iface); nmfw__put(&t, "-%c %s ", r->dir == NMFW_OUT ? 'o' : 'i', itf); }
    if (r->oiface[0]) { nmfw__ipt_if(itf, sizeof itf, r->oiface); nmfw__put(&t, "-o %s ", itf); }
    if (r->addr[0]) nmfw__put(&t, "-%c %s ", r->addr_dst ? 'd' : 's', r->addr);
    if (r->proto == NMFW_TCP || r->proto == NMFW_UDP) {
        nmfw__put(&t, "-p %s ", r->proto == NMFW_TCP ? "tcp" : "udp");
        if (r->nports == 1) nmfw__put(&t, "--dport %u ", r->ports[0]);
        else if (r->nports > 1) { nmfw__put(&t, "-m multiport --dports "); for (int i = 0; i < r->nports; i++) nmfw__put(&t, "%s%u", i ? "," : "", r->ports[i]); nmfw__put(&t, " "); }
    } else if (r->proto == NMFW_ICMP) nmfw__put(&t, "-p icmp ");
    else if (r->proto == NMFW_ICMP6) nmfw__put(&t, "-p ipv6-icmp ");
    if (r->ct) {
        char st[48]; size_t k = 0; st[0] = 0;
        if (r->ct & NMFW_CT_EST) k += (size_t)snprintf(st + k, sizeof st - k, "%sESTABLISHED,RELATED", k ? "," : "");
        if (r->ct & NMFW_CT_NEW) k += (size_t)snprintf(st + k, sizeof st - k, "%sNEW", k ? "," : "");
        if (r->ct & NMFW_CT_INV) k += (size_t)snprintf(st + k, sizeof st - k, "%sINVALID", k ? "," : "");
        nmfw__put(&t, "-m conntrack --ctstate %s ", st);
    }
    if (r->rate) nmfw__put(&t, "-m limit --limit %d/second --limit-burst %d ", r->rate, r->rate);
    if (r->comment[0]) nmfw__put(&t, "-m comment --comment \"%s\" ", r->comment);
    (void)v6;
    if (r->act == NMFW_LOGDROP) nmfw__put(b, "-A %s %s-j LOG --log-prefix \"netman: \"\n-A %s %s-j DROP\n", chain, tail, chain, tail);
    else nmfw__put(b, "-A %s %s-j %s\n", chain, tail, r->act == NMFW_ACCEPT ? "ACCEPT" : r->act == NMFW_DROP ? "DROP" : "REJECT");
}
int nmfw_plan_ipt(const nmfw_cfg *c, int v6, char *out, size_t cap) {
    nmfw__sb b = {out, 0, cap, 0};
    if (cap == 0) return nmfw__fail("plan buffer too small");
    out[0] = 0;
    if (nmfw_check(c)) return -1;
    nmfw__put(&b, "*filter\n:NETMAN - [0:0]\n:NETMAN_OUT - [0:0]\n:NETMAN_FWD - [0:0]\n:NETMAN_LIM - [0:0]\n:NETMAN_LIMZ - [0:0]\n");
    nmfw__put(&b, "-A NETMAN -i lo -j ACCEPT\n-A NETMAN -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT\n-A NETMAN -m conntrack --ctstate INVALID -j DROP\n");
    if (!v6 && c->dhcp_server[0]) nmfw__put(&b, "-A NETMAN -p udp --sport 67 --dport 68 ! -s %s -j DROP\n", c->dhcp_server);
    for (int i = 0; i < c->nzones; i++) {
        const nmfw_zone *z = &c->zones[i]; char itf[20]; nmfw__ipt_if(itf, sizeof itf, z->iface);
        if (z->level == NMFW_TRUSTED) { nmfw__put(&b, "-A NETMAN -i %s -j ACCEPT\n", itf); continue; }
        if (z->level == NMFW_HOME) {
            nmfw__put(&b, "-A NETMAN -i %s -p udp --dport 5353 -j ACCEPT\n", itf);
            if (c->icmp != NMFW_ICMP_DROP) nmfw__put(&b, v6 ? "-A NETMAN -i %s -p ipv6-icmp --icmpv6-type echo-request -j ACCEPT\n" : "-A NETMAN -i %s -p icmp --icmp-type echo-request -j ACCEPT\n", itf);
            continue;
        }
        if (z->level == NMFW_UNTRUSTED) {
            if (v6 && c->ra_guard) nmfw__put(&b, "-A NETMAN -i %s -p ipv6-icmp --icmpv6-type router-advertisement ! -s %s -j DROP\n", itf, c->ra_router);
            nmfw__put(&b, v6 ? "-A NETMAN -i %s -p udp --sport 547 --dport 546 -j ACCEPT\n-A NETMAN -i %s -p ipv6-icmp --icmpv6-type router-advertisement -j ACCEPT\n-A NETMAN -i %s -p ipv6-icmp --icmpv6-type neighbour-solicitation -j ACCEPT\n-A NETMAN -i %s -p ipv6-icmp --icmpv6-type neighbour-advertisement -j ACCEPT\n"
                              : "-A NETMAN -i %s -p udp --sport 67 --dport 68 -j ACCEPT\n", itf, itf, itf, itf);
            nmfw__put(&b, "-A NETMAN -i %s -j DROP\n", itf);
            continue;
        }
        nmfw__put(&b, v6 ? "-A NETMAN -i %s -p ipv6-icmp --icmpv6-type echo-request -j DROP\n" : "-A NETMAN -i %s -p icmp --icmp-type echo-request -j DROP\n", itf);
        if (v6 && c->ra_guard) nmfw__put(&b, "-A NETMAN -i %s -p ipv6-icmp --icmpv6-type router-advertisement ! -s %s -j DROP\n", itf, c->ra_router);
        nmfw__put(&b, "-A NETMAN -i %s -m conntrack --ctstate NEW -j NETMAN_LIMZ\n", itf);
    }
    if (c->limit_new)
        nmfw__put(&b, "-A NETMAN -m conntrack --ctstate NEW -j NETMAN_LIM\n-A NETMAN_LIM -m limit --limit %d/second --limit-burst %d -j RETURN\n-A NETMAN_LIM -j DROP\n", c->limit_new, c->limit_new * 2);
    for (int i = 0; i < c->nrules; i++) if (c->rules[i].dir == NMFW_IN && !nmfw__ipt_skip(&c->rules[i], v6)) nmfw__ipt_rule(&b, "NETMAN", &c->rules[i], v6);
    if (c->pol_in == NMFW_DROP) {
        if (!v6) nmfw__put(&b, "-A NETMAN -p udp --sport 67 --dport 68 -j ACCEPT\n");
        else {
            nmfw__put(&b, "-A NETMAN -p udp --sport 547 --dport 546 -j ACCEPT\n");
            static const char *t6[] = {"destination-unreachable", "packet-too-big", "time-exceeded", "parameter-problem", "router-advertisement", "neighbour-solicitation", "neighbour-advertisement"};
            for (size_t i = 0; i < sizeof t6 / sizeof *t6; i++) nmfw__put(&b, "-A NETMAN -p ipv6-icmp --icmpv6-type %s -j ACCEPT\n", t6[i]);
        }
    }
    if (c->icmp == NMFW_ICMP_LIMIT) nmfw__put(&b, v6 ? "-A NETMAN -p ipv6-icmp --icmpv6-type echo-request -m limit --limit 5/second -j ACCEPT\n" : "-A NETMAN -p icmp --icmp-type echo-request -m limit --limit 5/second -j ACCEPT\n");
    else if (c->icmp == NMFW_ICMP_ALLOW && c->pol_in == NMFW_DROP) nmfw__put(&b, v6 ? "-A NETMAN -p ipv6-icmp --icmpv6-type echo-request -j ACCEPT\n" : "-A NETMAN -p icmp --icmp-type echo-request -j ACCEPT\n");
    if (c->pol_in == NMFW_DROP) nmfw__put(&b, "-A NETMAN -j DROP\n");
    /* forward */
    nmfw__put(&b, "-A NETMAN_FWD -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT\n-A NETMAN_FWD -m conntrack --ctstate INVALID -j DROP\n");
    if (c->masq) { char l[20], w[20]; nmfw__ipt_if(l, sizeof l, c->lan); nmfw__ipt_if(w, sizeof w, c->wan); nmfw__put(&b, "-A NETMAN_FWD -i %s -o %s -j ACCEPT\n", l, w); }
    for (int i = 0; i < c->nrules; i++) if (c->rules[i].dir == NMFW_FWD && !nmfw__ipt_skip(&c->rules[i], v6)) nmfw__ipt_rule(&b, "NETMAN_FWD", &c->rules[i], v6);
    if (c->pol_fwd == NMFW_DROP) nmfw__put(&b, "-A NETMAN_FWD -j DROP\n");
    /* output */
    for (int i = 0; i < c->nzones; i++) {
        const nmfw_zone *z = &c->zones[i]; char itf[20];
        if (z->level < NMFW_PUBLIC) continue;
        nmfw__ipt_if(itf, sizeof itf, z->iface);
        nmfw__put(&b, "-A NETMAN_OUT -o %s -p tcp -m multiport --dports 135,139,445 -j DROP\n-A NETMAN_OUT -o %s -p udp -m multiport --dports 137,138,1900,5353 -j DROP\n", itf, itf);
    }
    for (int i = 0; i < c->nrules; i++) if (c->rules[i].dir == NMFW_OUT && !nmfw__ipt_skip(&c->rules[i], v6)) nmfw__ipt_rule(&b, "NETMAN_OUT", &c->rules[i], v6);
    nmfw__put(&b, "-A NETMAN_LIMZ -m limit --limit 30/second --limit-burst 60 -j RETURN\n-A NETMAN_LIMZ -j DROP\n");
    if (c->pol_out == NMFW_DROP) {
        nmfw__put(&b, "-A NETMAN_OUT -o lo -j ACCEPT\n-A NETMAN_OUT -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT\n");
        nmfw__put(&b, v6 ? "-A NETMAN_OUT -p udp --sport 546 --dport 547 -j ACCEPT\n-A NETMAN_OUT -p ipv6-icmp --icmpv6-type neighbour-solicitation -j ACCEPT\n-A NETMAN_OUT -p ipv6-icmp --icmpv6-type neighbour-advertisement -j ACCEPT\n-A NETMAN_OUT -p ipv6-icmp --icmpv6-type router-solicitation -j ACCEPT\n"
                          : "-A NETMAN_OUT -p udp --sport 68 --dport 67 -j ACCEPT\n");
        nmfw__put(&b, "-A NETMAN_OUT -j DROP\n");
    }
    nmfw__put(&b, "COMMIT\n");
    if (!v6 && (c->masq || c->ndnat)) {
        nmfw__put(&b, "*nat\n:NETMAN_POST - [0:0]\n:NETMAN_PRE - [0:0]\n");
        if (c->masq) { char w[20]; nmfw__ipt_if(w, sizeof w, c->wan); nmfw__put(&b, "-A NETMAN_POST -o %s -j MASQUERADE\n", w); }
        for (int i = 0; i < c->ndnat; i++) {
            const nmfw_dnat *d = &c->dnat[i]; char itf[20]; nmfw__ipt_if(itf, sizeof itf, d->iface);
            nmfw__put(&b, "-A NETMAN_PRE -i %s -p %s --dport %u -j DNAT --to-destination %s:%u\n", itf, d->proto == NMFW_TCP ? "tcp" : "udp", d->port, d->to, d->toport);
        }
        nmfw__put(&b, "COMMIT\n");
    }
    return nmfw__done(&b);
}

/* ---------------------------------------------------------------- process helpers */
static int nmfw__run(char *const argv[], const char *infile, char *out, size_t cap) {
    int p[2]; pid_t pid; int st;
    if (out && cap) out[0] = 0;
    if (pipe2(p, O_CLOEXEC)) return -1;
    pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        if (infile) { int f = open(infile, O_RDONLY); if (f < 0) _exit(126); dup2(f, 0); } else if (dn >= 0) dup2(dn, 0);
        dup2(p[1], 1); dup2(p[1], 2);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0; char tmp[512]; ssize_t r;
    while ((r = read(p[0], tmp, sizeof tmp)) != 0) {
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (out && cap && n + 1 < cap) { size_t k = (size_t)r < cap - 1 - n ? (size_t)r : cap - 1 - n; memcpy(out + n, tmp, k); n += k; out[n] = 0; }
    }
    close(p[0]);
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static int nmfw__mkdirs(const char *d) {
    char b[512]; size_t n = strlen(d);
    if (n == 0 || n >= sizeof b) return -1;
    memcpy(b, d, n + 1);
    for (char *p = b + 1; *p; p++) if (*p == '/') { *p = 0; if (mkdir(b, 0700) && errno != EEXIST) return -1; *p = '/'; }
    return (mkdir(b, 0700) && errno != EEXIST) ? -1 : 0;
}
static int nmfw__path(char *b, size_t n, const char *name) { return snprintf(b, n, "%s/%s", nmfw_state_dir, name) >= (int)n ? -1 : 0; }
static int nmfw__write(const char *path, const char *s) {
    char tmp[560]; snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
    int f = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (f < 0) return -1;
    size_t n = strlen(s), o = 0;
    while (o < n) { ssize_t w = write(f, s + o, n - o); if (w < 0) { if (errno == EINTR) continue; close(f); unlink(tmp); return -1; } o += (size_t)w; }
    if (close(f)) { unlink(tmp); return -1; }
    if (rename(tmp, path)) { unlink(tmp); return -1; }
    return 0;
}
static char *nmfw__slurp(const char *path) {      /* chunked read: /proc files report size 0 */
    FILE *f = fopen(path, "r"); size_t n = 0, cap = 4096, r; char *b, *nb;
    if (!f) return NULL;
    b = malloc(cap);
    while (b && (r = fread(b + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) { cap *= 2; nb = realloc(b, cap); if (!nb) { free(b); b = NULL; break; } b = nb; }
    }
    fclose(f);
    if (b) b[n] = 0;
    return b;
}

/* ---------------------------------------------------------------- which tables a plan touches (and the guard that keeps us in our lane) */
#define NMFW_MAXTAB 6
typedef struct { char fam[16], name[48]; } nmfw__tab;
static int nmfw__tab_owned(const char *fam, const char *name) {
    static const char *fams[] = {"inet", "ip", "ip6", "arp", "bridge", "netdev"};
    int ok = 0;
    for (size_t i = 0; i < sizeof fams / sizeof *fams; i++) if (!strcmp(fam, fams[i])) ok = 1;
    if (!ok) return 0;
    return !strncmp(name, "netman", 6);
}
/* the plan text's own `table F N` / `delete table F N` lines and braces; refuses anything outside our names */
static int nmfw__plan_tables(const char *plan, nmfw__tab *t, int max) {
    int n = 0;
    for (const char *p = plan; *p; ) {
        const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        char line[256]; snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), p);
        char fam[16], name[48], extra[8]; const char *s = line;
        while (*s == ' ') s++;
        int is_decl = !strncmp(s, "table ", 6), is_del = !strncmp(s, "delete table ", 13);
        if (is_decl || is_del) {
            s += is_decl ? 6 : 13;
            int k = sscanf(s, "%15s %47s %7s", fam, name, extra);
            if (k < 2) return nmfw__fail("malformed table line in plan");
            if (k == 3 && strcmp(extra, "{")) return nmfw__fail("malformed table line in plan");
            if (!nmfw__tab_owned(fam, name)) return nmfw__fail("plan touches table '%s %s' which netman does not own", fam, name);
            int seen = 0;
            for (int i = 0; i < n; i++) if (!strcmp(t[i].fam, fam) && !strcmp(t[i].name, name)) seen = 1;
            if (!seen) { if (n >= max) return nmfw__fail("plan touches too many tables"); snprintf(t[n].fam, 16, "%s", fam); snprintf(t[n].name, 48, "%s", name); n++; }
        } else if (!strncmp(s, "flush ", 6) || !strncmp(s, "destroy ", 8) || !strncmp(s, "add table", 9)) {
            return nmfw__fail("plan contains a forbidden statement");
        }
        if (!e) break;
        p = e + 1;
    }
    return n;
}

/* ---------------------------------------------------------------- backend */
int nmfw_backend(void) {
    char o[256];
    if (nmfw_force_backend) return nmfw_force_backend;
    char *a[] = {"nft", "list", "tables", NULL};
    if (nmfw__run(a, NULL, o, sizeof o) == 0) return NMFW_B_NFT;
    char *b[] = {"iptables-restore", "--version", NULL};
    if (nmfw__run(b, NULL, o, sizeof o) == 0) return NMFW_B_IPT;
    return NMFW_B_NONE;
}

/* ---------------------------------------------------------------- apply / snapshot / rollback */
static int nmfw__ipt_bin(int v6, const char *base, char *out, size_t n) { snprintf(out, n, "%s%s", v6 ? "ip6tables" : "iptables", base); return 0; }
/* ensure/remove the jumps from the built-in chains into ours */
static const struct { const char *table, *from, *to; } nmfw__jumps[] = {
    {"filter", "INPUT", "NETMAN"}, {"filter", "OUTPUT", "NETMAN_OUT"}, {"filter", "FORWARD", "NETMAN_FWD"},
    {"nat", "POSTROUTING", "NETMAN_POST"}, {"nat", "PREROUTING", "NETMAN_PRE"}};
static void nmfw__ipt_jumps(int v6, int on, int with_nat) {
    char bin[24], o[256]; nmfw__ipt_bin(v6, "", bin, sizeof bin);
    for (size_t i = 0; i < sizeof nmfw__jumps / sizeof *nmfw__jumps; i++) {
        int nat = !strcmp(nmfw__jumps[i].table, "nat");
        if (nat && v6) continue;                        /* no IPv6 NAT here */
        if (nat && on && !with_nat) continue;
        char *chk[] = {bin, "-w", "-t", (char *)nmfw__jumps[i].table, "-C", (char *)nmfw__jumps[i].from, "-j", (char *)nmfw__jumps[i].to, NULL};
        int have = nmfw__run(chk, NULL, o, sizeof o) == 0;
        if (on && !have) {
            char *ins[] = {bin, "-w", "-t", (char *)nmfw__jumps[i].table, "-I", (char *)nmfw__jumps[i].from, "1", "-j", (char *)nmfw__jumps[i].to, NULL};
            nmfw__run(ins, NULL, o, sizeof o);
        } else if (!on) {
            while (have) {
                char *del[] = {bin, "-w", "-t", (char *)nmfw__jumps[i].table, "-D", (char *)nmfw__jumps[i].from, "-j", (char *)nmfw__jumps[i].to, NULL};
                if (nmfw__run(del, NULL, o, sizeof o)) break;
                have = nmfw__run(chk, NULL, o, sizeof o) == 0;
            }
        }
    }
}
static void nmfw__ipt_drop_chains(int v6) {
    char bin[24], o[256]; nmfw__ipt_bin(v6, "", bin, sizeof bin);
    nmfw__ipt_jumps(v6, 0, 1);
    static const struct { const char *t, *c; } ch[] = {{"filter", "NETMAN"}, {"filter", "NETMAN_OUT"}, {"filter", "NETMAN_FWD"}, {"filter", "NETMAN_LIM"}, {"filter", "NETMAN_LIMZ"}, {"nat", "NETMAN_POST"}, {"nat", "NETMAN_PRE"}};
    for (size_t i = 0; i < sizeof ch / sizeof *ch; i++) {
        if (v6 && !strcmp(ch[i].t, "nat")) continue;
        char *f[] = {bin, "-w", "-t", (char *)ch[i].t, "-F", (char *)ch[i].c, NULL}, *x[] = {bin, "-w", "-t", (char *)ch[i].t, "-X", (char *)ch[i].c, NULL};
        nmfw__run(f, NULL, o, sizeof o); nmfw__run(x, NULL, o, sizeof o);
    }
}
/* snapshot of OUR chains only, as iptables-restore text (empty = nothing existed) */
static char *nmfw__ipt_snapshot(int v6) {
    char bin[24]; nmfw__ipt_bin(v6, "-save", bin, sizeof bin);
    char *a[] = {bin, NULL}; static char o[1 << 16];
    if (nmfw__run(a, NULL, o, sizeof o) != 0) return strdup("");
    char *res = calloc(1, sizeof o + 64); if (!res) return NULL;
    size_t k = 0; int intable = 0; char tab[512] = "";
    for (char *p = o; *p; ) {
        char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
        char line[512]; snprintf(line, sizeof line, "%.*s", (int)(l < sizeof line - 1 ? l : sizeof line - 1), p);
        if (line[0] == '*') { snprintf(tab, sizeof tab, "%s", line); intable = 0; }
        int ours = !strncmp(line, ":NETMAN", 7) || !strncmp(line, "-A NETMAN", 9);
        if (ours) {
            if (!intable) { k += (size_t)snprintf(res + k, sizeof o + 64 - k, "%s\n", tab); intable = 1; }
            k += (size_t)snprintf(res + k, sizeof o + 64 - k, "%s\n", line);
        }
        if (!strcmp(line, "COMMIT") && intable) { k += (size_t)snprintf(res + k, sizeof o + 64 - k, "COMMIT\n"); intable = 0; }
        if (!e) break;
        p = e + 1;
    }
    return res;
}
static int nmfw__fwd_get(const char *path) { char *s = nmfw__slurp(path); int v = s ? atoi(s) : -1; free(s); return v; }
static void nmfw__fwd_set(const char *path, int v) { char b[16]; snprintf(b, sizeof b, "%d\n", v); FILE *f = fopen(path, "w"); if (f) { fputs(b, f); fclose(f); } }
#define NMFW_F4 "/proc/sys/net/ipv4/ip_forward"
#define NMFW_F6 "/proc/sys/net/ipv6/conf/all/forwarding"

static int nmfw__pid_start(long pid, unsigned long long *st) {
    char p[64], *s; snprintf(p, sizeof p, "/proc/%ld/stat", pid);
    s = nmfw__slurp(p); if (!s) return -1;
    char *rp = strrchr(s, ')'); int k = 0; unsigned long long v = 0;
    if (rp) { char *t = rp + 2; for (int f = 3; f < 22 && t; f++) { t = strchr(t, ' '); if (t) t++; } if (t) { v = strtoull(t, NULL, 10); k = 1; } }
    free(s); if (!k) return -1; *st = v; return 0;
}
static int nmfw__restore_files(void) {
    char pr[560], o[1024];
    if (nmfw__path(pr, sizeof pr, "fw.rollback")) return -1;
    char *s = nmfw__slurp(pr); if (!s) return nmfw__fail("no rollback snapshot");
    int rc = 0;
    if (!strncmp(s, "# backend nft\n", 14)) {
        char f[560]; nmfw__path(f, sizeof f, "fw.rollback.nft");
        if (nmfw__write(f, s + 14)) rc = -1;
        else { char *a[] = {"nft", "-f", f, NULL}; if (nmfw__run(a, NULL, o, sizeof o)) rc = nmfw__fail("nft restore failed: %.150s", o); }
    } else if (!strncmp(s, "# backend ipt\n", 14)) {
        /* "# v4" ... "# v6" ... each part restores our chains to what they were */
        for (int v6 = 0; v6 < 2; v6++) {
            char marker[8]; snprintf(marker, sizeof marker, "# v%d\n", v6 ? 6 : 4);
            char *m = strstr(s, marker); if (!m) continue;
            m += strlen(marker); char *end = strstr(m, "\n# v"); char save = 0;
            if (end) { save = end[1]; end[1] = 0; }
            nmfw__ipt_drop_chains(v6);
            if (m[0]) {
                char f[560]; nmfw__path(f, sizeof f, "fw.rollback.ipt");
                nmfw__write(f, m);
                char bin[24]; nmfw__ipt_bin(v6, "-restore", bin, sizeof bin);
                char *a[] = {bin, "-w", "--noflush", NULL};
                if (nmfw__run(a, f, o, sizeof o)) rc = nmfw__fail("iptables restore failed: %.150s", o);
                nmfw__ipt_jumps(v6, 1, 1);
            }
            if (end) end[1] = save;
        }
    } else rc = nmfw__fail("unreadable rollback snapshot");
    char f4[560], f6[560];
    if (!nmfw__path(f4, sizeof f4, "fw.fwd4")) { int v = nmfw__fwd_get(f4); if (v >= 0) nmfw__fwd_set(NMFW_F4, v); unlink(f4); }
    if (!nmfw__path(f6, sizeof f6, "fw.fwd6")) { int v = nmfw__fwd_get(f6); if (v >= 0) nmfw__fwd_set(NMFW_F6, v); unlink(f6); }
    free(s);
    return rc;
}
static void nmfw__clear_pending(void) {
    char p[560];
    static const char *names[] = {"fw.pid", "fw.rollback", "fw.rollback.nft", "fw.rollback.ipt", "fw.plan.nft", "fw.plan4.ipt", "fw.plan6.ipt", "fw.plan.arp"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!nmfw__path(p, sizeof p, names[i])) unlink(p);
}
static long nmfw__pending(void) {
    char p[560]; if (nmfw__path(p, sizeof p, "fw.pid")) return 0;
    char *s = nmfw__slurp(p); if (!s) return 0;
    long pid = 0; unsigned long long st = 0, cur = 0;
    if (sscanf(s, "%ld %llu", &pid, &st) != 2 || pid <= 1 || nmfw__pid_start(pid, &cur) || cur != st) pid = 0;
    free(s);
    return pid;
}
/* Claim the pending pidfile atomically: rename() of a name that exists succeeds for exactly one caller, so the confirm
 * and the watchdog's rollback can never both win (the check-then-unlink pair they used before could). 1 = claimed. */
static void (*nmfw__test_window)(void);           /* tests only: runs in the watchdog between its last poll and its claim */
static int nmfw__claim(void) {
    char p[560], q[600]; if (nmfw__path(p, sizeof p, "fw.pid")) return 0;
    snprintf(q, sizeof q, "%s.claim.%ld", p, (long)getpid());
    if (rename(p, q)) return 0;
    unlink(q);
    return 1;
}
int nmfw_confirm(void) {
    long pid = nmfw__pending();
    if (!pid) return nmfw__fail("nothing pending: nothing to confirm (already confirmed, or the timer already rolled back)");
    if (!nmfw__claim()) return nmfw__fail("too late: the timer already claimed the rollback");
    kill((pid_t)pid, SIGTERM);
    for (int i = 0; i < 50 && kill((pid_t)pid, 0) == 0; i++) { struct timespec ts = {0, 20000000}; nanosleep(&ts, NULL); }
    char f[560]; if (!nmfw__path(f, sizeof f, "fw.rollback")) unlink(f);
    return 0;
}
int nmfw_rollback(void) {
    long pid = nmfw__pending();
    char p[560];
    if (pid) { nmfw__path(p, sizeof p, "fw.pid"); unlink(p); kill((pid_t)pid, SIGTERM); }
    int rc = nmfw__restore_files();
    if (rc == 0) nmfw__clear_pending();
    return rc;
}
static void nmfw__watchdog(int secs) {
    pid_t me = getpid();
    setsid();
    for (int i = 0; i < 3; i++) close(i);
    open("/dev/null", O_RDWR); dup2(0, 1); dup2(0, 2);
    signal(SIGTERM, SIG_DFL);
    for (int s = 0; s < secs * 10; s++) {
        struct timespec ts = {0, 100000000}; nanosleep(&ts, NULL);
        long cur = nmfw__pending();
        if (cur != (long)me) _exit(0);            /* confirmed or superseded */
    }
    if (nmfw__test_window) nmfw__test_window();
    if (!nmfw__claim()) _exit(0);                 /* a confirm took the pidfile after the last poll: it won, no rollback */
    nmfw__restore_files();
    nmfw__clear_pending();
    _exit(0);
}

static int nmfw__apply_nft(const nmfw_cfg *c, char *plan, size_t cap) {
    char o[1024], pf[560], rf[560];
    int n = nmfw_plan(c, plan, cap); if (n < 0) return -1;
    char arp[1024]; int na = nmfw_plan_arp(c, arp, sizeof arp); if (na < 0) return -1;
    if (na == 0) { strcpy(arp, "table arp netman_arp\ndelete table arp netman_arp\n"); na = (int)strlen(arp); }   /* no pin now: a pin from an earlier apply must go */
    if ((size_t)n + (size_t)na + 2 < cap) { plan[n++] = '\n'; memcpy(plan + n, arp, (size_t)na + 1); n += na; }
    nmfw__tab tabs[NMFW_MAXTAB]; int nt = nmfw__plan_tables(plan, tabs, NMFW_MAXTAB);
    if (nt < 0) return -1;
    nmfw__path(pf, sizeof pf, "fw.plan.nft"); nmfw__path(rf, sizeof rf, "fw.rollback");
    if (nmfw__write(pf, plan)) return nmfw__fail("cannot write %s: %s", pf, strerror(errno));
    { char *a[] = {"nft", "-c", "-f", pf, NULL}; if (nmfw__run(a, NULL, o, sizeof o)) return nmfw__fail("nft rejected the plan: %.200s", o); }
    if (!nmfw__pending()) {                       /* keep the ORIGINAL snapshot if an earlier apply is still unconfirmed */
        size_t cp = 1 << 17; char *snap = malloc(cp); if (!snap) return nmfw__fail("out of memory");
        size_t k = (size_t)snprintf(snap, cp, "# backend nft\n");
        for (int i = 0; i < nt; i++) {
            char *a[] = {"nft", "list", "table", tabs[i].fam, tabs[i].name, NULL};
            char *cur = malloc(cp); if (!cur) { free(snap); return nmfw__fail("out of memory"); }
            int rc = nmfw__run(a, NULL, cur, cp);
            k += (size_t)snprintf(snap + k, cp - k, "table %s %s\ndelete table %s %s\n", tabs[i].fam, tabs[i].name, tabs[i].fam, tabs[i].name);
            if (rc == 0) k += (size_t)snprintf(snap + k, cp - k, "%s\n", cur);
            free(cur);
            if (k >= cp - 1) { free(snap); return nmfw__fail("snapshot too large"); }
        }
        int w = nmfw__write(rf, snap); free(snap);
        if (w) return nmfw__fail("cannot write snapshot: %s", strerror(errno));
        if (c->masq || c->ndnat) {
            char f[560];
            nmfw__path(f, sizeof f, "fw.fwd4"); { char b[16]; snprintf(b, sizeof b, "%d", nmfw__fwd_get(NMFW_F4)); nmfw__write(f, b); }
            nmfw__path(f, sizeof f, "fw.fwd6"); { char b[16]; snprintf(b, sizeof b, "%d", nmfw__fwd_get(NMFW_F6)); nmfw__write(f, b); }
        }
    }
    { char *a[] = {"nft", "-f", pf, NULL}; if (nmfw__run(a, NULL, o, sizeof o)) return nmfw__fail("nft failed to apply: %.200s", o); }
    if (c->masq || c->ndnat) { nmfw__fwd_set(NMFW_F4, 1); nmfw__fwd_set(NMFW_F6, 1); }
    return n;
}
static int nmfw__apply_ipt(const nmfw_cfg *c, char *plan, size_t cap) {
    char o[1024], f[560]; (void)cap;
    if (!nmfw__pending()) {
        char *s4 = nmfw__ipt_snapshot(0), *s6 = nmfw__ipt_snapshot(1);
        if (!s4 || !s6) { free(s4); free(s6); return nmfw__fail("out of memory"); }
        size_t cp = strlen(s4) + strlen(s6) + 64; char *snap = malloc(cp); if (!snap) { free(s4); free(s6); return nmfw__fail("out of memory"); }
        snprintf(snap, cp, "# backend ipt\n# v4\n%s# v6\n%s", s4, s6);
        nmfw__path(f, sizeof f, "fw.rollback"); int w = nmfw__write(f, snap);
        free(snap); free(s4); free(s6);
        if (w) return nmfw__fail("cannot write snapshot: %s", strerror(errno));
        if (c->masq || c->ndnat) {
            nmfw__path(f, sizeof f, "fw.fwd4"); { char b[16]; snprintf(b, sizeof b, "%d", nmfw__fwd_get(NMFW_F4)); nmfw__write(f, b); }
            nmfw__path(f, sizeof f, "fw.fwd6"); { char b[16]; snprintf(b, sizeof b, "%d", nmfw__fwd_get(NMFW_F6)); nmfw__write(f, b); }
        }
    }
    int total = 0;
    for (int v6 = 0; v6 < 2; v6++) {
        int n = nmfw_plan_ipt(c, v6, plan, cap); if (n < 0) return -1;
        total += n;
        nmfw__path(f, sizeof f, v6 ? "fw.plan6.ipt" : "fw.plan4.ipt");
        if (nmfw__write(f, plan)) return nmfw__fail("cannot write %s", f);
        char bin[24]; nmfw__ipt_bin(v6, "-restore", bin, sizeof bin);
        { char *t[] = {bin, "-w", "--noflush", "--test", NULL}; if (nmfw__run(t, f, o, sizeof o)) return nmfw__fail("%s rejected the plan: %.200s", bin, o); }
        { char *a[] = {bin, "-w", "--noflush", NULL}; if (nmfw__run(a, f, o, sizeof o)) return nmfw__fail("%s failed: %.200s", bin, o); }
        nmfw__ipt_jumps(v6, 1, c->masq || c->ndnat);
    }
    if (c->masq || c->ndnat) { nmfw__fwd_set(NMFW_F4, 1); nmfw__fwd_set(NMFW_F6, 1); }
    return total;
}

int nmfw_apply(const nmfw_cfg *c, int confirm_s) {
    if (confirm_s < 0 || confirm_s > 3600) return nmfw__fail("confirm timer must be 0..3600 seconds");
    if (nmfw_check(c)) return -1;
    if (nmfw__mkdirs(nmfw_state_dir)) return nmfw__fail("cannot create %s: %s", nmfw_state_dir, strerror(errno));
    int be = nmfw_backend();
    if (be == NMFW_B_NONE) return nmfw__fail("no firewall backend: need `nft` or iptables-restore (and root / CAP_NET_ADMIN)");
    size_t cap = 1 << 17; char *plan = malloc(cap); if (!plan) return nmfw__fail("out of memory");
    int rc = be == NMFW_B_NFT ? nmfw__apply_nft(c, plan, cap) : nmfw__apply_ipt(c, plan, cap);
    if (rc < 0) {
        /* a failed apply must not leave a half-state: put the snapshot back if we took one this time */
        if (!nmfw__pending()) { char rf[560]; nmfw__path(rf, sizeof rf, "fw.rollback"); if (access(rf, R_OK) == 0) { char keep[256]; snprintf(keep, sizeof keep, "%s", nmfw_error()); nmfw__restore_files(); nmfw__clear_pending(); nmfw__fail("%s", keep); } }
        free(plan); return -1;
    }
    { char lp[560]; if (!nmfw__path(lp, sizeof lp, "fw.last")) { nmfw__write(lp, plan); } }
    free(plan);
    if (confirm_s > 0) {
        char pf[560]; nmfw__path(pf, sizeof pf, "fw.pid");
        pid_t kid = fork();
        if (kid < 0) { nmfw_rollback(); return nmfw__fail("cannot start the rollback timer, rolled back"); }
        if (kid == 0) nmfw__watchdog(confirm_s);
        unsigned long long st = 0;
        for (int i = 0; i < 100 && nmfw__pid_start(kid, &st); i++) { struct timespec ts = {0, 5000000}; nanosleep(&ts, NULL); }
        char b[64]; snprintf(b, sizeof b, "%ld %llu\n", (long)kid, st);
        if (nmfw__write(pf, b)) { kill(kid, SIGKILL); waitpid(kid, NULL, 0); nmfw_rollback(); return nmfw__fail("cannot record the rollback timer, rolled back"); }
        signal(SIGCHLD, SIG_DFL);
    } else nmfw__clear_pending();
    return 0;
}

int nmfw_remove(void) {
    int be = nmfw_backend(); char o[256]; int rc = 0;
    if (be == NMFW_B_NFT) {
        static const char *t[][2] = {{"inet", "netman"}, {"arp", "netman_arp"}};
        for (int i = 0; i < 2; i++) {
            char *s[] = {"nft", "list", "table", (char *)t[i][0], (char *)t[i][1], NULL};
            if (nmfw__run(s, NULL, o, sizeof o) == 0) { char *d[] = {"nft", "delete", "table", (char *)t[i][0], (char *)t[i][1], NULL}; if (nmfw__run(d, NULL, o, sizeof o)) rc = nmfw__fail("nft delete failed: %.150s", o); }
        }
    } else if (be == NMFW_B_IPT) { nmfw__ipt_drop_chains(0); nmfw__ipt_drop_chains(1); }
    else return nmfw__fail("no firewall backend");
    long pid = nmfw__pending(); if (pid) kill((pid_t)pid, SIGTERM);
    nmfw__clear_pending();
    return rc;
}
int nmfw_diff(const nmfw_cfg *c, char *out, size_t cap) {
    size_t pc = 1 << 17; char *plan = malloc(pc), lp[560];
    if (!plan) return nmfw__fail("out of memory");
    int n = nmfw_plan(c, plan, pc);
    if (n < 0) { free(plan); return -1; }
    { char arp[1024]; int na = nmfw_plan_arp(c, arp, sizeof arp); if (na < 0) { free(plan); return -1; } if (na > 0 && (size_t)n + (size_t)na + 2 < pc) { plan[n++] = '\n'; memcpy(plan + n, arp, (size_t)na + 1); n += na; } }
    nmfw__path(lp, sizeof lp, "fw.last");
    char *old = nmfw__slurp(lp);
    nmfw__sb b = {out, 0, cap, 0};
    if (cap) out[0] = 0;
    if (!old) { nmfw__put(&b, "(nothing applied yet: all %d bytes are new)\n", n); free(plan); return nmfw__done(&b); }
    int changes = 0;
    /* line sets: '-' only in the applied plan, '+' only in the new one (order-insensitive, which is what matters for a ruleset review) */
    for (int pass = 0; pass < 2; pass++) {
        const char *a = pass ? plan : old, *other = pass ? old : plan;
        for (const char *p = a; *p; ) {
            const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
            int found = 0;
            for (const char *q = other; *q && l; ) {
                const char *qe = strchr(q, '\n'); size_t ql = qe ? (size_t)(qe - q) : strlen(q);
                if (ql == l && !memcmp(p, q, l)) { found = 1; break; }
                if (!qe) break;
                q = qe + 1;
            }
            if (!found && l) { nmfw__put(&b, "%c %.*s\n", pass ? '+' : '-', (int)l, p); changes++; }
            if (!e) break;
            p = e + 1;
        }
    }
    if (!changes) nmfw__put(&b, "(no change)\n");
    free(old); free(plan);
    return nmfw__done(&b);
}
int nmfw_killswitch(const char *tun, const nmfw_endpoint *eps, int n, int allow_lan, int on) {
    char o[512];
    if (!on) {
        char *a[] = {"nft", "delete", "table", "inet", "netman_vpn_ks", NULL};
        return nmfw__run(a, NULL, o, sizeof o) ? nmfw__fail("kill switch was not active (or nft failed)") : 0;
    }
    char plan[8192]; if (nmfw_plan_killswitch(tun, eps, n, allow_lan, plan, sizeof plan) < 0) return -1;
    if (nmfw__mkdirs(nmfw_state_dir)) return nmfw__fail("cannot create %s", nmfw_state_dir);
    char f[560]; nmfw__path(f, sizeof f, "fw.ks.nft");
    if (nmfw__write(f, plan)) return nmfw__fail("cannot write plan");
    char *a[] = {"nft", "-f", f, NULL};
    int rc = nmfw__run(a, NULL, o, sizeof o); unlink(f);
    return rc ? nmfw__fail("nft refused the kill switch: %.200s", o) : 0;
}
#endif /* NETMAN_FW_IMPL_ONCE */
#endif /* NETMAN_FW_IMPLEMENTATION */
