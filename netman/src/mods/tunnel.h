/* tunnel.h - point-to-point and overlay tunnels: GRE, GRE-TAP, IP-in-IP, SIT (IPv6 in IPv4), VXLAN, Geneve (rtnetlink) and L2TPv3 (`ip l2tp`).
 * Real code in tunnel.c. Switch: tunnel.enabled (off by default: it creates links). Needs CAP_NET_ADMIN.
 *
 * A tunnel is a profile NAME.tun in tunnel.dir ("key = value"); `tunnel up` creates the link through rtnetlink (no `ip` binary) and
 * assigns the address, `tunnel apply` brings up every profile with `auto = yes` (boot), `tunnel down` deletes the link.
 *
 *   kind     gre gretap ipip sit vxlan geneve l2tp           (gre/gretap switch to ip6gre/ip6gretap when local/remote are IPv6)
 *   local, remote   endpoint addresses (remote may be empty for vxlan with a multicast group, and for gre/ipip "any")
 *   link     underlay interface to bind to (optional)        key  GRE key (number): both directions
 *   ttl      0..255 (default: inherit)                       mtu  auto | N
 *   addr     CIDR on the tunnel device                       auto yes|no
 *   vni, port, group     vxlan / geneve: VNI, UDP destination port (default 4789 / 6081), multicast group (vxlan)
 *   l2tp: tunnel_id peer_tunnel_id session_id peer_session_id encap=ip|udp lport rport
 *
 * MTU: `mtu = auto` (default) is the underlay MTU (link=IF, else 1500) minus the encapsulation overhead (tunnel_overhead()); the numbers
 * are the usual textbook sizes (GRE 24, +4 with key, GRE-TAP 38, IPIP/SIT 20, VXLAN/Geneve 50, +20 over IPv6), NOT measured on a real path.
 */
#ifndef NETMAN_TUNNEL_H
#define NETMAN_TUNNEL_H
#include <stdio.h>

typedef struct {
	char name[16], kind[8], local[46], remote[46], link[16], group[46], addr[64], encap[4];
	long key;                    /* -1 = no key */
	int ttl, mtu /* 0 = auto */, autostart, vni, port, lport, rport;
	long tid, ptid, sid, psid;   /* l2tp */
} netman_tun;

/* overhead in bytes of KIND's encapsulation; v6 = the outer header is IPv6, key = GRE key present */
int  netman_tunnel_overhead(const char *kind, int v6, int key);
int  netman_tunnel_parse(const char *text, netman_tun *t, char *err, size_t en);   /* 0 / -1 */
int  netman_tunnel_format(const netman_tun *t, char *buf, size_t n);
int  netman_tunnel_check(netman_tun *t, char *err, size_t en);                      /* validates, fills defaults */
int  netman_tunnel_mtu(const netman_tun *t, int *underlay, int *overhead);          /* the MTU `auto` picks */
int  netman_tunnel_up(const netman_tun *t, char *err, size_t en);
int  netman_tunnel_down(const netman_tun *t, char *err, size_t en);
int  netman_tunnel_status(FILE *out);                                                /* rows  NAME STATE kind local>remote mtu N rx tx */
int  netman_tunnel_main(FILE *out, FILE *err, int argc, char **argv);               /* argv after "tunnel" */
extern const char *netman_tunnel_dir;     /* profiles, default /etc/netman/tunnel */
extern const char *netman_tunnel_ip;      /* the `ip` binary used for l2tp only, default "ip" */
#endif
