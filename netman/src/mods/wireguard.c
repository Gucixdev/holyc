/* wireguard.c - see wireguard.h. Own tiny netlink message builder so the file stands alone. */
#define _GNU_SOURCE
#include "wireguard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <netdb.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/genetlink.h>
#include <linux/if_link.h>

/* ---------------------------------------------------------------- endpoints */
int netman_wg_ep_split(const char *ep, char *host, size_t hn, int *port)
{
    const char *colon;
    if (ep[0] == '[') {
        const char *rb = strchr(ep, ']');
        if (!rb || rb[1] != ':') return -1;
        if ((size_t)(rb - ep - 1) >= hn || rb - ep - 1 == 0) return -1;
        memcpy(host, ep + 1, (size_t)(rb - ep - 1)); host[rb - ep - 1] = 0;
        colon = rb + 1;
    } else {
        colon = strrchr(ep, ':');
        if (!colon || colon == ep || (size_t)(colon - ep) >= hn || memchr(ep, ':', (size_t)(colon - ep))) return -1;
        memcpy(host, ep, (size_t)(colon - ep)); host[colon - ep] = 0;
    }
    char *e; long p = strtol(colon + 1, &e, 10);
    if (e == colon + 1 || *e || p < 1 || p > 65535) return -1;
    *port = (int)p;
    for (const char *h = host; *h; h++) if (!(isalnum((unsigned char)*h) || strchr(".-_:", *h))) return -1;
    return 0;
}
int netman_wg_resolve(const char *ep, netman_wg_ep *out)
{
    char host[256], ps[8]; int port;
    if (netman_wg_ep_split(ep, host, sizeof host, &port)) return -1;
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = {0}, *res;
    hints.ai_socktype = SOCK_DGRAM; hints.ai_flags = AI_NUMERICSERV;
    if (getaddrinfo(host, ps, &hints, &res)) return -1;
    for (struct addrinfo *r = res; r; r = r->ai_next)
        if (r->ai_family == AF_INET || r->ai_family == AF_INET6) { memset(out, 0, sizeof *out); memcpy(&out->ss, r->ai_addr, r->ai_addrlen); out->sl = r->ai_addrlen; freeaddrinfo(res); return 0; }
    freeaddrinfo(res);
    return -1;
}
const char *netman_wg_ep_str(const netman_wg_ep *e, char *buf, size_t n)
{
    char a[INET6_ADDRSTRLEN];
    if (e->ss.ss_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)&e->ss;
        if (inet_ntop(AF_INET, &s->sin_addr, a, sizeof a)) { snprintf(buf, n, "%s:%u", a, ntohs(s->sin_port)); return buf; }
    } else if (e->ss.ss_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)&e->ss;
        if (inet_ntop(AF_INET6, &s->sin6_addr, a, sizeof a)) { snprintf(buf, n, "[%s]:%u", a, ntohs(s->sin6_port)); return buf; }
    }
    snprintf(buf, n, "-"); return buf;
}

/* ---------------------------------------------------------------- netlink plumbing */
typedef struct { unsigned char b[1 << 16]; size_t len; int ovf; } wg_msg;
static unsigned wg_seq;

static void *wg_put(wg_msg *m, size_t n)
{
    size_t a = NLMSG_ALIGN(n);
    if (m->len + a > sizeof m->b) { m->ovf = 1; return NULL; }
    void *p = m->b + m->len; memset(p, 0, a); m->len += a; return p;
}
static void wg_hdr(wg_msg *m, int type, int flags, const void *body, size_t bl)
{
    m->len = 0; m->ovf = 0;
    struct nlmsghdr *h = wg_put(m, sizeof *h);
    h->nlmsg_type = (unsigned short)type; h->nlmsg_flags = (unsigned short)(NLM_F_REQUEST | flags);
    void *b = wg_put(m, bl);
    if (b && body) memcpy(b, body, bl);
}
static void wg_attr(wg_msg *m, int type, const void *d, size_t n)
{
    struct nlattr *a = wg_put(m, NLA_HDRLEN + n);
    if (!a) return;
    a->nla_type = (unsigned short)type; a->nla_len = (unsigned short)(NLA_HDRLEN + n);
    if (n) memcpy((char *)a + NLA_HDRLEN, d, n);
}
static void wg_attr_u32(wg_msg *m, int t, unsigned v) { wg_attr(m, t, &v, 4); }
static void wg_attr_u16(wg_msg *m, int t, unsigned short v) { wg_attr(m, t, &v, 2); }
static void wg_attr_str(wg_msg *m, int t, const char *s) { wg_attr(m, t, s, strlen(s) + 1); }
static size_t wg_nest(wg_msg *m, int type)
{
    size_t off = m->len; struct nlattr *a = wg_put(m, NLA_HDRLEN);
    if (a) a->nla_type = (unsigned short)(NLA_F_NESTED | type);
    return off;
}
static void wg_nend(wg_msg *m, size_t off) { if (!m->ovf) ((struct nlattr *)(m->b + off))->nla_len = (unsigned short)(m->len - off); }

typedef int (*wg_cb)(const struct nlmsghdr *, void *);
static int wg_open(int proto)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, proto);
    if (fd < 0) return -1;
    struct sockaddr_nl sa = {.nl_family = AF_NETLINK};
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return -1; }
    return fd;
}
/* 0 or -errno */
static int wg_talk(int fd, wg_msg *m, wg_cb cb, void *ud)
{
    if (m->ovf) return -E2BIG;
    struct nlmsghdr *h = (struct nlmsghdr *)m->b;
    h->nlmsg_len = (unsigned)m->len; h->nlmsg_seq = ++wg_seq;
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
#define WG_DUMP (NLM_F_ROOT | NLM_F_MATCH)

/* ---------------------------------------------------------------- rtnetlink: the link */
int netman_wg_link_add(const char *name)
{
    int fd = wg_open(NETLINK_ROUTE); if (fd < 0) return -errno;
    wg_msg *m = malloc(sizeof *m); if (!m) { close(fd); return -ENOMEM; }
    struct ifinfomsg ifi = {.ifi_family = AF_UNSPEC};
    wg_hdr(m, RTM_NEWLINK, NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL, &ifi, sizeof ifi);
    wg_attr_str(m, IFLA_IFNAME, name);
    size_t li = wg_nest(m, IFLA_LINKINFO);
    wg_attr_str(m, IFLA_INFO_KIND, "wireguard");
    wg_nend(m, li);
    int r = wg_talk(fd, m, NULL, NULL); free(m); close(fd); return r;
}
static int wg_kind_cb(const struct nlmsghdr *h, void *ud)
{
    int *is = ud; const struct ifinfomsg *ifi = NLMSG_DATA(h);
    const char *p = (const char *)ifi + NLMSG_ALIGN(sizeof *ifi), *end = (const char *)h + h->nlmsg_len;
    while (p + NLA_HDRLEN <= end) {
        const struct nlattr *a = (const struct nlattr *)p;
        if (a->nla_len < NLA_HDRLEN || p + a->nla_len > end) break;
        if ((a->nla_type & NLA_TYPE_MASK) == IFLA_LINKINFO) {
            const char *q = p + NLA_HDRLEN, *qe = p + a->nla_len;
            while (q + NLA_HDRLEN <= qe) {
                const struct nlattr *b = (const struct nlattr *)q;
                if (b->nla_len < NLA_HDRLEN || q + b->nla_len > qe) break;
                if ((b->nla_type & NLA_TYPE_MASK) == IFLA_INFO_KIND && strncmp(q + NLA_HDRLEN, "wireguard", b->nla_len - NLA_HDRLEN) == 0 && b->nla_len - NLA_HDRLEN >= 9) *is = 1;
                q += NLA_ALIGN(b->nla_len);
            }
        }
        p += NLA_ALIGN(a->nla_len);
    }
    return 0;
}
/* asks the kernel (rtnetlink) for the link kind, so it is right in any network namespace (sysfs may show another one) */
int netman_wg_is_link(const char *name)
{
    int fd = wg_open(NETLINK_ROUTE); if (fd < 0) return 0;
    wg_msg *m = malloc(sizeof *m); if (!m) { close(fd); return 0; }
    struct ifinfomsg ifi = {.ifi_family = AF_UNSPEC};
    wg_hdr(m, RTM_GETLINK, 0, &ifi, sizeof ifi);
    wg_attr_str(m, IFLA_IFNAME, name);
    int is = 0; wg_talk(fd, m, wg_kind_cb, &is);
    free(m); close(fd); return is;
}

/* ---------------------------------------------------------------- generic netlink: the device */
enum { WG_CMD_GET = 0, WG_CMD_SET = 1 };
enum { WGD_IFINDEX = 1, WGD_IFNAME, WGD_PRIVATE_KEY, WGD_PUBLIC_KEY, WGD_FLAGS, WGD_LISTEN_PORT, WGD_FWMARK, WGD_PEERS };
enum { WGP_PUBLIC_KEY = 1, WGP_PRESHARED_KEY, WGP_FLAGS, WGP_ENDPOINT, WGP_KEEPALIVE, WGP_LAST_HANDSHAKE, WGP_RX, WGP_TX, WGP_ALLOWEDIPS };
enum { WGA_FAMILY = 1, WGA_IPADDR, WGA_CIDR };
#define WGD_F_REPLACE_PEERS 1u
#define WGP_F_REMOVE_ME 1u
#define WGP_F_REPLACE_ALLOWEDIPS 2u

static int wg_fam_cb(const struct nlmsghdr *h, void *ud)
{
    unsigned short *id = ud; const struct genlmsghdr *g = NLMSG_DATA(h);
    const char *p = (const char *)g + GENL_HDRLEN, *end = (const char *)h + h->nlmsg_len;
    while (p + NLA_HDRLEN <= end) {
        const struct nlattr *a = (const struct nlattr *)p;
        if (a->nla_len < NLA_HDRLEN || p + a->nla_len > end) break;
        if ((a->nla_type & NLA_TYPE_MASK) == CTRL_ATTR_FAMILY_ID && a->nla_len >= NLA_HDRLEN + 2) memcpy(id, p + NLA_HDRLEN, 2);
        p += NLA_ALIGN(a->nla_len);
    }
    return 0;
}
static int wg_family(int fd)
{
    wg_msg *m = malloc(sizeof *m); if (!m) return -ENOMEM;
    struct genlmsghdr g = {.cmd = CTRL_CMD_GETFAMILY, .version = 1};
    wg_hdr(m, GENL_ID_CTRL, 0, &g, sizeof g);
    wg_attr_str(m, CTRL_ATTR_FAMILY_NAME, "wireguard");
    unsigned short id = 0;
    int r = wg_talk(fd, m, wg_fam_cb, &id); free(m);
    if (r) return r;
    return id ? id : -ENOENT;
}
/* open a generic netlink socket and resolve the wireguard family; *fam is the family id */
static int wg_gopen(int *fam)
{
    int fd = wg_open(NETLINK_GENERIC); if (fd < 0) return -errno ? -errno : -EIO;
    int f = wg_family(fd);
    if (f < 0) { close(fd); return f; }
    *fam = f; return fd;
}

static void wg_put_peer(wg_msg *m, int idx, const netman_wg_peer *p)
{
    size_t po = wg_nest(m, idx);
    wg_attr(m, WGP_PUBLIC_KEY, p->pub, 32);
    if (p->has_psk) wg_attr(m, WGP_PRESHARED_KEY, p->psk, 32);
    wg_attr_u32(m, WGP_FLAGS, WGP_F_REPLACE_ALLOWEDIPS);
    if (p->has_ep) wg_attr(m, WGP_ENDPOINT, &p->ep.ss, p->ep.sl);
    wg_attr_u16(m, WGP_KEEPALIVE, (unsigned short)p->keepalive);
    size_t an = wg_nest(m, WGP_ALLOWEDIPS);
    for (size_t j = 0; j < p->nallowed; j++) {
        size_t ao = wg_nest(m, (int)j);
        wg_attr_u16(m, WGA_FAMILY, (unsigned short)p->allowed[j].family);
        wg_attr(m, WGA_IPADDR, p->allowed[j].addr, p->allowed[j].family == AF_INET ? 4 : 16);
        unsigned char cm = (unsigned char)p->allowed[j].prefix; wg_attr(m, WGA_CIDR, &cm, 1);
        wg_nend(m, ao);
    }
    wg_nend(m, an);
    wg_nend(m, po);
}
/* send a prepared SET on a fresh socket, wipe the buffer */
static int wg_send_set(int fd, wg_msg *m)
{
    int r = wg_talk(fd, m, NULL, NULL);
    explicit_bzero(m->b, sizeof m->b);          /* the message may have carried the private key */
    return r;
}
static wg_msg *wg_set_msg(int fam, const char *ifname)
{
    wg_msg *m = malloc(sizeof *m); if (!m) return NULL;
    struct genlmsghdr g = {.cmd = WG_CMD_SET, .version = 1};
    wg_hdr(m, fam, NLM_F_ACK, &g, sizeof g);
    wg_attr_str(m, WGD_IFNAME, ifname);
    return m;
}

int netman_wg_set(const char *ifname, const netman_wg_conf *c)
{
    int fam, fd = wg_gopen(&fam); if (fd < 0) return fd;
    wg_msg *m = wg_set_msg(fam, ifname); if (!m) { close(fd); return -ENOMEM; }
    if (c->has_priv) wg_attr(m, WGD_PRIVATE_KEY, c->priv, 32);
    if (c->listen_port) wg_attr_u16(m, WGD_LISTEN_PORT, (unsigned short)c->listen_port);
    if (c->has_fwmark) wg_attr_u32(m, WGD_FWMARK, c->fwmark);
    wg_attr_u32(m, WGD_FLAGS, WGD_F_REPLACE_PEERS);
    size_t pn = wg_nest(m, WGD_PEERS);
    for (size_t i = 0; i < c->npeers; i++) wg_put_peer(m, (int)i, &c->peers[i]);
    wg_nend(m, pn);
    int r = wg_send_set(fd, m);
    free(m); close(fd); return r;
}

/* ---------------------------------------------------------------- reading the live device */
typedef struct {
    netman_wg_peerinfo *v; size_t n, cap; int oom;
    int listen_port; unsigned fwmark;
} wg_dump;

static int wg_dump_peer(wg_dump *d, const char *p, size_t len)
{
    const char *r = p, *re = p + len; netman_wg_peerinfo pi; int have_key = 0;
    memset(&pi, 0, sizeof pi);
    while (r + NLA_HDRLEN <= re) {
        const struct nlattr *x = (const struct nlattr *)r;
        if (x->nla_len < NLA_HDRLEN || r + x->nla_len > re) break;
        int t = x->nla_type & NLA_TYPE_MASK; const char *xd = r + NLA_HDRLEN; size_t xl = x->nla_len - NLA_HDRLEN;
        if (t == WGP_PUBLIC_KEY && xl >= 32) { memcpy(pi.pub, xd, 32); have_key = 1; }
        else if (t == WGP_ENDPOINT && xl >= sizeof(struct sockaddr_in) && xl <= sizeof pi.ep.ss) { memcpy(&pi.ep.ss, xd, xl); pi.ep.sl = (socklen_t)xl; pi.has_ep = 1; }
        else if (t == WGP_KEEPALIVE && xl >= 2) { unsigned short v; memcpy(&v, xd, 2); pi.keepalive = v; }
        else if (t == WGP_LAST_HANDSHAKE && xl >= 8) memcpy(&pi.last_handshake, xd, 8);     /* struct timespec: seconds first */
        else if (t == WGP_RX && xl >= 8) memcpy(&pi.rx, xd, 8);
        else if (t == WGP_TX && xl >= 8) memcpy(&pi.tx, xd, 8);
        else if (t == WGP_ALLOWEDIPS) {
            const char *q = xd, *qe = xd + xl;
            while (q + NLA_HDRLEN <= qe) {
                const struct nlattr *ia = (const struct nlattr *)q;
                if (ia->nla_len < NLA_HDRLEN || q + ia->nla_len > qe) break;
                netman_wg_ip ip; memset(&ip, 0, sizeof ip); int ok = 0;
                const char *s = q + NLA_HDRLEN, *se = q + ia->nla_len;
                while (s + NLA_HDRLEN <= se) {
                    const struct nlattr *f = (const struct nlattr *)s;
                    if (f->nla_len < NLA_HDRLEN || s + f->nla_len > se) break;
                    int ft = f->nla_type & NLA_TYPE_MASK; const char *fd = s + NLA_HDRLEN; size_t fl = f->nla_len - NLA_HDRLEN;
                    if (ft == WGA_FAMILY && fl >= 2) { unsigned short v; memcpy(&v, fd, 2); ip.family = v; }
                    else if (ft == WGA_IPADDR && fl >= 4 && fl <= 16) { memcpy(ip.addr, fd, fl); ok = 1; }
                    else if (ft == WGA_CIDR && fl >= 1) ip.prefix = (unsigned char)fd[0];
                    s += NLA_ALIGN(f->nla_len);
                }
                if (ok && (ip.family == AF_INET || ip.family == AF_INET6)) {
                    netman_wg_ip *na = realloc(pi.allowed, (pi.nallowed + 1) * sizeof *na);
                    if (!na) d->oom = 1; else { pi.allowed = na; pi.allowed[pi.nallowed++] = ip; }
                }
                q += NLA_ALIGN(ia->nla_len);
            }
        }
        r += NLA_ALIGN(x->nla_len);
    }
    if (!have_key) { free(pi.allowed); return 0; }
    /* a peer with many allowed ips is split over several messages; the pieces repeat only the key */
    if (d->n && !memcmp(d->v[d->n - 1].pub, pi.pub, 32)) {
        netman_wg_peerinfo *last = &d->v[d->n - 1];
        if (pi.nallowed) {
            netman_wg_ip *na = realloc(last->allowed, (last->nallowed + pi.nallowed) * sizeof *na);
            if (!na) d->oom = 1; else { last->allowed = na; memcpy(na + last->nallowed, pi.allowed, pi.nallowed * sizeof *na); last->nallowed += pi.nallowed; }
        }
        free(pi.allowed);
        return 0;
    }
    if (d->n == d->cap) {
        size_t nc = d->cap ? d->cap * 2 : 8; netman_wg_peerinfo *nv = realloc(d->v, nc * sizeof *nv);
        if (!nv) { d->oom = 1; free(pi.allowed); return 0; }
        d->v = nv; d->cap = nc;
    }
    d->v[d->n++] = pi;
    return 0;
}
static int wg_dump_cb(const struct nlmsghdr *h, void *ud)
{
    wg_dump *d = ud; const struct genlmsghdr *g = NLMSG_DATA(h);
    const char *p = (const char *)g + GENL_HDRLEN, *end = (const char *)h + h->nlmsg_len;
    while (p + NLA_HDRLEN <= end) {
        const struct nlattr *a = (const struct nlattr *)p;
        if (a->nla_len < NLA_HDRLEN || p + a->nla_len > end) break;
        int t = a->nla_type & NLA_TYPE_MASK; const char *dt = p + NLA_HDRLEN; size_t dl = a->nla_len - NLA_HDRLEN;
        if (t == WGD_LISTEN_PORT && dl >= 2) { unsigned short v; memcpy(&v, dt, 2); d->listen_port = v; }
        else if (t == WGD_FWMARK && dl >= 4) memcpy(&d->fwmark, dt, 4);
        else if (t == WGD_PEERS) {
            const char *q = dt, *qe = dt + dl;
            while (q + NLA_HDRLEN <= qe) {
                const struct nlattr *pa = (const struct nlattr *)q;
                if (pa->nla_len < NLA_HDRLEN || q + pa->nla_len > qe) break;
                wg_dump_peer(d, q + NLA_HDRLEN, pa->nla_len - NLA_HDRLEN);
                q += NLA_ALIGN(pa->nla_len);
            }
        }
        p += NLA_ALIGN(a->nla_len);
    }
    return 0;
}
static void wg_dump_free(wg_dump *d) { netman_wg_peers_free(d->v, d->n); d->v = NULL; d->n = d->cap = 0; }
static int wg_dump_run(const char *ifname, wg_dump *d)
{
    memset(d, 0, sizeof *d);
    if (!if_nametoindex(ifname)) return -ENODEV;
    int fam, fd = wg_gopen(&fam); if (fd < 0) return fd;
    wg_msg *m = malloc(sizeof *m); if (!m) { close(fd); return -ENOMEM; }
    struct genlmsghdr g = {.cmd = WG_CMD_GET, .version = 1};
    wg_hdr(m, fam, WG_DUMP, &g, sizeof g);
    wg_attr_str(m, WGD_IFNAME, ifname);
    int r = wg_talk(fd, m, wg_dump_cb, d);
    explicit_bzero(m->b, sizeof m->b);
    free(m); close(fd);
    if (r == 0 && d->oom) r = -ENOMEM;
    if (r) wg_dump_free(d);
    return r;
}

void netman_wg_peers_free(netman_wg_peerinfo *p, size_t n)
{
    if (!p) return;
    for (size_t i = 0; i < n; i++) free(p[i].allowed);
    free(p);
}
int netman_wg_peers(const char *ifname, netman_wg_peerinfo **out, size_t *n)
{
    wg_dump d; int r = wg_dump_run(ifname, &d);
    *out = NULL; *n = 0;
    if (r) return r;
    *out = d.v; *n = d.n;
    return 0;
}
int netman_wg_stats(const char *ifname, netman_wg_stat *st)
{
    wg_dump d; memset(st, 0, sizeof *st);
    if (wg_dump_run(ifname, &d)) return 1;
    st->listen_port = d.listen_port; st->fwmark = d.fwmark; st->npeers = (int)d.n;
    for (size_t i = 0; i < d.n; i++) {
        st->rx += d.v[i].rx; st->tx += d.v[i].tx;
        if (d.v[i].last_handshake > 0) { st->handshaked++; if (d.v[i].last_handshake > st->last_handshake) st->last_handshake = d.v[i].last_handshake; }
    }
    wg_dump_free(&d);
    st->present = 1; return 0;
}

/* ---------------------------------------------------------------- one peer on a live interface */
static int wg_has_peer(const char *ifname, const unsigned char pub[32])
{
    wg_dump d; int r = wg_dump_run(ifname, &d), found = 0;
    if (r) return r;
    for (size_t i = 0; i < d.n; i++) if (!memcmp(d.v[i].pub, pub, 32)) found = 1;
    wg_dump_free(&d);
    return found;
}
int netman_wg_peer_add(const char *ifname, const netman_wg_peer *p)
{
    int fam, fd = wg_gopen(&fam); if (fd < 0) return fd;
    wg_msg *m = wg_set_msg(fam, ifname); if (!m) { close(fd); return -ENOMEM; }
    size_t pn = wg_nest(m, WGD_PEERS);
    wg_put_peer(m, 0, p);
    wg_nend(m, pn);
    int r = wg_send_set(fd, m);
    free(m); close(fd); return r;
}
int netman_wg_peer_del(const char *ifname, const unsigned char pub[32])
{
    int h = wg_has_peer(ifname, pub);
    if (h < 0) return h;
    if (!h) return -ENOENT;
    int fam, fd = wg_gopen(&fam); if (fd < 0) return fd;
    wg_msg *m = wg_set_msg(fam, ifname); if (!m) { close(fd); return -ENOMEM; }
    size_t pn = wg_nest(m, WGD_PEERS);
    size_t po = wg_nest(m, 0);
    wg_attr(m, WGP_PUBLIC_KEY, pub, 32);
    wg_attr_u32(m, WGP_FLAGS, WGP_F_REMOVE_ME);
    wg_nend(m, po);
    wg_nend(m, pn);
    int r = wg_send_set(fd, m);
    free(m); close(fd); return r;
}
int netman_wg_peer_endpoint(const char *ifname, const unsigned char pub[32], const netman_wg_ep *ep)
{
    int h = wg_has_peer(ifname, pub);          /* a SET for an unknown key would create an empty peer */
    if (h < 0) return h;
    if (!h) return -ENOENT;
    int fam, fd = wg_gopen(&fam); if (fd < 0) return fd;
    wg_msg *m = wg_set_msg(fam, ifname); if (!m) { close(fd); return -ENOMEM; }
    size_t pn = wg_nest(m, WGD_PEERS);
    size_t po = wg_nest(m, 0);
    wg_attr(m, WGP_PUBLIC_KEY, pub, 32);
    wg_attr(m, WGP_ENDPOINT, &ep->ss, ep->sl);
    wg_nend(m, po);
    wg_nend(m, pn);
    int r = wg_send_set(fd, m);
    free(m); close(fd); return r;
}
