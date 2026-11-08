/* wireguard.h - native WireGuard control over generic netlink (no `wg` binary, no libmnl).
 *
 * Plain C, one .c file (wireguard.c). vpn.h keeps the profile files, wg-quick parser, routes, DNS and
 * kill switch and calls this for everything that talks to the kernel's wireguard device:
 *   - create / recognise a wireguard link
 *   - set the whole device (private key, port, fwmark, peer list)
 *   - read the live peers: endpoint, last handshake, rx/tx, allowed ips
 *   - change ONE peer on a running interface (add, delete, new endpoint) without touching the others
 *
 * Keys are raw 32-byte arrays here; base64 and key generation live in vpn.h (netman_vpn_genkey ...).
 * Every function returns 0 on success or -errno (netman_wg_stats: 0 / 1, like its old name).
 * All messages that carried key material are wiped before the buffer is freed.
 */
#ifndef NETMAN_WIREGUARD_H
#define NETMAN_WIREGUARD_H

#include <stddef.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { int family; unsigned char addr[16]; int prefix; } netman_wg_ip;     /* family AF_INET / AF_INET6 */
typedef struct { struct sockaddr_storage ss; socklen_t sl; } netman_wg_ep;

/* what you hand in */
typedef struct {
    unsigned char pub[32], psk[32]; int has_psk;
    int has_ep; netman_wg_ep ep;
    int keepalive;                                  /* seconds, 0 = off */
    const netman_wg_ip *allowed; size_t nallowed;
} netman_wg_peer;
typedef struct {
    unsigned char priv[32]; int has_priv;
    int listen_port;                                /* 0 = leave to the kernel */
    int has_fwmark; unsigned fwmark;
    const netman_wg_peer *peers; size_t npeers;
} netman_wg_conf;

/* what you get back (live state) */
typedef struct {
    unsigned char pub[32];
    int has_ep; netman_wg_ep ep;
    int keepalive;
    long long last_handshake;                       /* unix seconds, 0 = never */
    long long rx, tx;
    netman_wg_ip *allowed; size_t nallowed;
} netman_wg_peerinfo;
typedef struct { int present; int npeers, handshaked; long long rx, tx; long long last_handshake; int listen_port; unsigned fwmark; } netman_wg_stat;

/* endpoints: "host:port" / "[v6]:port" */
int  netman_wg_ep_split(const char *ep, char *host, size_t hn, int *port);       /* syntax only; 0 ok */
int  netman_wg_resolve(const char *ep, netman_wg_ep *out);                       /* 0 ok, -1 bad or unresolvable */
const char *netman_wg_ep_str(const netman_wg_ep *e, char *buf, size_t n);        /* "1.2.3.4:51820", "[::1]:51820"; buf on success, "-" otherwise */

/* the link itself (rtnetlink) */
int  netman_wg_link_add(const char *name);                                       /* kind "wireguard" */
int  netman_wg_is_link(const char *name);                                        /* 1 if NAME is a wireguard link in THIS network namespace */

/* the device (generic netlink) */
int  netman_wg_set(const char *ifname, const netman_wg_conf *c);                 /* replaces the peer list */
int  netman_wg_stats(const char *ifname, netman_wg_stat *st);                    /* 0 ok (st->present = 1), 1 not readable */
int  netman_wg_peers(const char *ifname, netman_wg_peerinfo **out, size_t *n);   /* live peer list; free with netman_wg_peers_free */
void netman_wg_peers_free(netman_wg_peerinfo *p, size_t n);

/* one peer on a running interface: the other peers, the key and the routes are not touched */
int  netman_wg_peer_add(const char *ifname, const netman_wg_peer *p);            /* new, or replaces that key's allowed ips/endpoint/keepalive */
int  netman_wg_peer_del(const char *ifname, const unsigned char pub[32]);        /* -ENOENT if there is no such peer */
int  netman_wg_peer_endpoint(const char *ifname, const unsigned char pub[32], const netman_wg_ep *ep);   /* -ENOENT if no such peer (never creates one) */

#ifdef __cplusplus
}
#endif
#endif
