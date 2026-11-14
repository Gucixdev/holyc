/* enterprise.h - API of the "enterprise level" networking mods: enterprise.c (bonding/team, bridge, vlan/QinQ,
 * macvlan/ipvlan, veth, dummy, routes/rules/tables, ECMP failover, link settings, IPv6, Wake-on-LAN, wired 802.1X,
 * spec profiles) and tether.c (USB/BT tethering, share-out). Real .c files: link them, no implementation macro.
 *
 * Every feature is a function pair plus show, all taking one netman_ent spec:
 *   netman_ent_<x>_add(const netman_ent *e)   netman_ent_<x>_del(e)   netman_ent_<x>_show(e, FILE *f)
 *   <x> = bond team bridge vlan macvlan ipvlan veth dummy tether dot1x route rule ecmp link ipv6 wol
 * netman_ent_add/del/show dispatch on e->kind. Specs persist as profiles NAME.ent in netman_conf_dir (net.h): same
 * "key = value" syntax as net.h NAME.conf, but a separate extension so net.h's strict parser never sees the extra keys.
 *
 * Everything goes through rtnetlink (links, bonding, bridge incl. vlan filtering, vlan, macvlan, ipvlan, veth, routes,
 * ECMP multipath, rules), ioctl SIOCETHTOOL (offloads, speed, Wake-on-LAN) and /proc/sys (IPv6, forwarding).
 * The ONLY things ever exec'd, all optional helpers: nft (tether ruleset), dnsmasq (tether DHCP/DNS), wpa_supplicant
 * (wired 802.1X). Bluetooth PAN is a bluez D-Bus call, compiled in with -DNETMAN_ENT_DBUS (needs dbus.h's
 * implementation in the same program); without it btpan returns -ENOSYS.
 *
 * "team": the kernel team driver is deprecated, so kind=team is bonding with the team runner names mapped
 * (activebackup, lacp, loadbalance, broadcast, roundrobin). There is no teamd.
 *
 * Needs CAP_NET_ADMIN (+CAP_NET_RAW for ECMP probes). Return 0/count or a negative errno; netman_ent_error() has text.
 * Not thread-safe.
 */
#ifndef NETMAN_ENT_H
#define NETMAN_ENT_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define NETMAN_ENT_MEMBERS 8
#define NETMAN_ENT_VLANS 16
#define NETMAN_ENT_UPLINKS 8

typedef struct {
	int vid, pvid, untagged;
} netman_ent_vlan;

typedef struct {
	char ifn[16], gw[46], probe[46]; /* probe: IPv4 pinged out of ifn ("" = the gateway) */
	int weight;			 /* 1..255 */
	int up, fails, oks;		 /* runtime health (up starts 1) */
} netman_ent_uplink;

typedef struct {
	char kind[12]; /* bond team bridge vlan macvlan ipvlan veth dummy tether dot1x route rule ecmp link ipv6 wol */
	char name[16]; /* device (or profile) name; the downstream iface for tether */
	char parent[16], peer[16], inner[16];
	char member[NETMAN_ENT_MEMBERS][16]; /* bond slaves, bridge ports, ipv6 PD downstream ifaces */
	int nmember;
	char mode[24], primary[16], lacp_rate[8], hash[20], runner[16];
	int miimon, stp, fwd_delay /* seconds */, vlan_filtering, vid, inner_vid, mtu, netns_pid;
	char proto[8]; /* vlan: 802.1q (default) or 802.1ad (QinQ outer tag) */
	netman_ent_vlan vlan[NETMAN_ENT_VLANS];
	int nvlan;
	char addr[64], gateway[46], dns[46];
	int table, metric;
	char dst[64], src[46], rtype[12]; /* rtype: unicast blackhole unreachable prohibit */
	char from[64], to[64], iif[16], oif[16];
	int fwmark, prio;
	netman_ent_uplink up[NETMAN_ENT_UPLINKS];
	int nup, probe_ms, fail_after, ok_after;
	char upstream[16], dhcp_start[46], dhcp_end[46], btmac[20]; /* tether; mode = share (default) | client | panu | nap; btmac = phone for panu */
	char eap[16], identity[64], anon_identity[64], password[64], ca_cert[128], client_cert[128], private_key[128], phase2[24];
	int tso, gso, gro, sg, rxcsum, txcsum, speed, autoneg; /* -1 = leave alone */
	char duplex[8];
	char wol[12]; /* ethtool letters: p u m b a g s, or d */
	int accept_ra, autoconf, tempaddr, forwarding; /* -1 = leave alone */
	char pd_prefix[64];			       /* delegated prefix handed to member[] ifaces, one /64 each */
} netman_ent;

typedef struct { /* what the kernel reports for one link (netman_ent_getinfo); -1 = not applicable */
	char kind[16];
	int idx, master, link, mtu, up;
	int vid, proto, mode, miimon, lacp, hash, primary, stp, fwd_delay, vlan_filtering;
} netman_ent_info;

typedef struct {
	char ifn[16], driver[24], kind[4]; /* kind "usb" or "bt" */
} netman_ent_tether_dev;

extern const char *netman_ent_sys;	 /* "/sys"  (tests point it at a fake tree) */
extern const char *netman_ent_proc;	 /* "/proc" */
extern const char *netman_ent_rt_tables; /* "/etc/iproute2/rt_tables" */
extern int (*netman_ent_probe_fn)(const char *ifn, const char *target, int timeout_ms); /* 0 = alive; default ICMP echo */

const char *netman_ent_error(void);
void netman_ent_init(netman_ent *e);
int netman_ent_parse(const char *text, netman_ent *e); /* 0 or -EINVAL */
int netman_ent_format(const netman_ent *e, char *out, size_t cap);
int netman_ent_check(const netman_ent *e);
int netman_ent_load(const char *name, netman_ent *e);
int netman_ent_save(const netman_ent *e);
int netman_ent_list(char names[][64], int max);
int netman_ent_apply_all(void); /* load every NAME.ent, apply in dependency order; returns number of failures */
int netman_ent_table_id(const char *name_or_number); /* routing table by number or /etc/iproute2/rt_tables name; -1 unknown */

int netman_ent_add(const netman_ent *e);
int netman_ent_del(const netman_ent *e);
int netman_ent_show(const netman_ent *e, FILE *f);

#define NETMAN_ENT_DECL(x)                                         \
	int netman_ent_##x##_add(const netman_ent *e);             \
	int netman_ent_##x##_del(const netman_ent *e);             \
	int netman_ent_##x##_show(const netman_ent *e, FILE *f);
NETMAN_ENT_DECL(bond)
NETMAN_ENT_DECL(team)
NETMAN_ENT_DECL(bridge)
NETMAN_ENT_DECL(vlan)
NETMAN_ENT_DECL(macvlan)
NETMAN_ENT_DECL(ipvlan)
NETMAN_ENT_DECL(veth)
NETMAN_ENT_DECL(dummy)
NETMAN_ENT_DECL(tether)
NETMAN_ENT_DECL(dot1x)
NETMAN_ENT_DECL(route)
NETMAN_ENT_DECL(rule)
NETMAN_ENT_DECL(ecmp)
NETMAN_ENT_DECL(link)
NETMAN_ENT_DECL(ipv6)
NETMAN_ENT_DECL(wol)

int netman_ent_bond_slave(const char *bond, const char *slave, int add); /* add (1) or release (0) one slave of a live bond */
int netman_ent_getinfo(const char *ifn, netman_ent_info *i);
int netman_ent_members(const char *master, char out[][16], int max);	      /* ifaces enslaved to a bond/bridge */
int netman_ent_bridge_vlans(const char *port, netman_ent_vlan *out, int max); /* vlan filter table of a bridge port */
int netman_ent_route_list(int table, int family, char *out, size_t cap);      /* one line per route, multipath inline; table 0 = all */
int netman_ent_rule_list(int family, char *out, size_t cap);

/* ECMP: probe every uplink, update health (fail_after / ok_after consecutive results flip it), reinstall the default
 * route when the healthy set changed (or force). Returns the number of state changes, <0 on error. */
int netman_ent_ecmp_tick(netman_ent *e, int force);
int netman_ent_ecmp_run(netman_ent *e, int interval_ms, volatile sig_atomic_t *stop);

/* tether.c */
int netman_ent_tether_detect(netman_ent_tether_dev *out, int max);	 /* USB RNDIS/NCM/ECM/ipheth + bnep ifaces */
int netman_ent_tether_nft(const netman_ent *e, char *out, size_t cap);	 /* share-out ruleset text (golden-testable) */
int netman_ent_tether_dnsmasq(const netman_ent *e, char *out, size_t cap);
int netman_ent_btpan_connect(const char *mac, const char *role, char *ifn, size_t n); /* role panu|nap|gn */
int netman_ent_btpan_server(int on, const char *bridge);		 /* NAP: register/unregister a bridge with bluez */
/* 802.1X */
int netman_ent_dot1x_conf(const netman_ent *e, char *out, size_t cap);
/* IPv6 */
int netman_ent_pd_subnet(const char *prefix, int idx, char *cidr, size_t n); /* idx-th /64 of a delegated prefix, as ADDR::1/64 */
int netman_ent_pd_from_env(const char *prefixes, netman_ent *e);	      /* odhcp6c PREFIXES="p/len,pref,valid ..." -> e->pd_prefix */
int netman_ent_pd_helper_conf(const netman_ent *e, char *out, size_t cap);    /* dnsmasq RA/stateless config for the downstream ifaces */
/* Wake-on-LAN */
int netman_ent_wol_packet(const unsigned char mac[6], unsigned char out[102]);
int netman_ent_wol_send(const char *ifn, const char *mac); /* UDP broadcast :9 out of ifn */

#endif /* NETMAN_ENT_H */
