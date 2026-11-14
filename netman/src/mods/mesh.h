/* mesh.h - mesh networks: batman-adv (layer 2, via `ip` and `batctl`) and a WireGuard full mesh generated from one peer list.
 * Real code in mesh.c. Switch: mesh.enabled (off by default: it creates links). Needs CAP_NET_ADMIN.
 *
 * A mesh is a profile NAME.mesh in mesh.dir ("key = value"):
 *   kind      batman | wg
 *   iface     the mesh device (default bat0 / wgmesh0)             addr  CIDR on the mesh device       auto yes|no
 *   batman:   ifaces (comma list of radios/ports to enslave)  orig_interval (ms)  gw_mode off|client|server  hop_penalty 0..255  mtu N
 *             `ip link add IF type batadv`, `ip link set X master IF` (or `batctl meshif IF interface add X` when batctl exists).
 *             batman-adv adds 32 bytes of header: give the underlay MTU 1532 or leave mtu at 1500 and accept fragmentation (batman does it).
 *   wg:       self (this node's name in the peer list)  port (default 51820)  prefix (default 24 / 64)  keepalive (default 25)
 *             key (private key file, default DIR/NAME.key, mode 0600)
 * The WireGuard mesh keeps ONE peer list, NAME.peers, one node per line:  name  public-key  endpoint|-  mesh-address
 * `mesh gen NAME [node]` prints the wg-quick config of any node (every other node as a peer, AllowedIPs = its mesh address only);
 * `mesh up NAME` programs the local node through wireguard.c (netlink), no `wg` binary. A node with endpoint `-` is reached only when it connects first.
 * Mesh routing is NOT done for you: the full mesh is direct links between all nodes (n*(n-1)/2), fine for tens of nodes, not hundreds.
 */
#ifndef NETMAN_MESH_H
#define NETMAN_MESH_H
#include <stdio.h>

typedef struct {
	char name[16], kind[8], iface[16], addr[64], ifaces[96], gw_mode[8], self[32], key[200];
	int autostart, orig_interval /* 0 = leave */, hop_penalty /* -1 = leave */, mtu /* 0 = leave */, port, prefix, keepalive;
} netman_mesh;

typedef struct { char name[32], pub[48], ep[80], addr[46]; } netman_mesh_node;

int  netman_mesh_parse(const char *text, netman_mesh *m, char *err, size_t en);
int  netman_mesh_format(const netman_mesh *m, char *buf, size_t n);
int  netman_mesh_check(netman_mesh *m, char *err, size_t en);                          /* validates, fills defaults */
int  netman_mesh_nodes_parse(const char *text, netman_mesh_node *n, int max, char *err, size_t en);   /* returns the count or -1 */
int  netman_mesh_gen(const netman_mesh *m, const netman_mesh_node *n, int cnt, const char *node, const char *privkey_b64, char *buf, size_t len);
int  netman_mesh_up(const netman_mesh *m, char *err, size_t en);
int  netman_mesh_down(const netman_mesh *m, char *err, size_t en);
int  netman_mesh_status(FILE *out);                                                     /* rows  NAME STATE kind iface  peers h/n */
int  netman_mesh_main(FILE *out, FILE *err, int argc, char **argv);                    /* argv after "mesh" */
extern const char *netman_mesh_dir;      /* default /etc/netman/mesh */
extern const char *netman_mesh_ip;       /* default "ip" */
extern const char *netman_mesh_batctl;   /* default "batctl" (optional) */
#endif
