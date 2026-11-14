/* tor.h - route this machine's traffic through Tor. Real code in tor.c (link it in; see readme.md).
 *
 * netman does not implement Tor. It manages a `tor` process the way it manages wpa_supplicant: it writes a torrc under
 * /run/netman/tor, loads the nftables rules that make the machine FAIL CLOSED, starts tor, and talks to its control port.
 *
 *   transparent mode  table `inet netman_tor`: TCP of the routed uids goes to TransPort, DNS to DNSPort, everything else
 *                     (UDP, ICMP, IPv6, QUIC ...) is DROPPED. The rules go in BEFORE tor starts and stay until `tor down`.
 *   socks mode        no firewall; `netman tor status` prints the SOCKS address for the apps that support it. Anything that
 *                     ignores the setting is NOT protected. Prefer transparent mode.
 *
 * THREAT MODEL, honestly: Tor hides which network you are on and where you are connecting FROM, from the sites and from your
 * ISP's view of the sites. It does not make you anonymous if you log in to accounts, reuse a name, run browser plug-ins,
 * download and open documents, or let an application reveal who you are in its own protocol. A global observer who watches
 * both ends can still correlate traffic. Transparent mode cannot stop an application that puts your real IP in the payload.
 * Use Tor Browser for web browsing: it also fights fingerprinting, which this does not. Real Tor is UNTESTED here (tor is not
 * installed on the development host): the torrc, the nft rules and the control protocol are tested against fakes.
 */
#ifndef NETMAN_TOR_ROUTE_H
#define NETMAN_TOR_ROUTE_H
#include <stddef.h>
#include <stdio.h>

typedef struct {
	char dir[96];                  /* /run/netman/tor */
	char persist[96];              /* /etc/netman/tor.conf: bridges, exit country, hidden services (survive reboots) */
	char mode[16];                 /* "transparent" | "socks" */
	char tor_user[32];             /* user tor drops to; its traffic is the only one allowed out */
	char route_uids[96];           /* "1000, 1001": only these are forced through Tor; empty = every uid except tor's */
	char route_ifaces[96];         /* "wlan0": traffic forwarded in from these ifaces (a router/hotspot) is forced through as well */
	char obfs4_bin[96], snowflake_bin[96];
	int socks_port, trans_port, dns_port;
	int control_port;              /* 0 = unix socket DIR/control (default), else TCP 127.0.0.1:PORT */
	char bridges[8][256]; int nbridges;       /* "obfs4 1.2.3.4:443 FPR cert=... iat-mode=0" or "snowflake ..." */
	char exit_cc[8]; int strict;              /* "de" and StrictNodes */
	struct { char name[32]; int port; char target[64]; } hs[4]; int nhs;   /* hidden services: virtual PORT -> TARGET host:port */
	FILE *out, *err;
} netman_tor_cfg;

void  netman_tor_defaults(netman_tor_cfg *c);
int   netman_tor_load(netman_tor_cfg *c);              /* persist file -> bridges/exit/hs (0 ok, also when it does not exist) */
int   netman_tor_save(const netman_tor_cfg *c);
int   netman_tor_validate(const netman_tor_cfg *c, char *err, size_t en);
int   netman_tor_torrc(const netman_tor_cfg *c, char *buf, size_t n);   /* the generated torrc (golden-tested); 0 ok */
int   netman_tor_nft(const netman_tor_cfg *c, unsigned tor_uid, char *buf, size_t n);   /* ruleset for `nft -f -` (golden-tested) */

/* control port client (plain C, tor control-spec): ADDR is "unix:/path" or "host:port" */
typedef struct { int fd; char buf[8192]; size_t have; } netman_tor_ctl;
int   netman_tor_ctl_open(netman_tor_ctl *t, const char *addr);
void  netman_tor_ctl_close(netman_tor_ctl *t);
int   netman_tor_ctl_cmd(netman_tor_ctl *t, const char *cmd, char *reply, size_t n);   /* status code, reply text; -1 io */
int   netman_tor_ctl_auth(netman_tor_ctl *t);          /* PROTOCOLINFO, then cookie AUTHENTICATE; 0 ok */
int   netman_tor_ctl_bootstrap(netman_tor_ctl *t, char *summary, size_t n);   /* percent, -1 error */
int   netman_tor_ctl_circuits(netman_tor_ctl *t, int *built, char *exit_fp, size_t n);   /* number of BUILT circuits, last exit fingerprint */
int   netman_tor_ctl_exitip(netman_tor_ctl *t, const char *fp, char *ip, size_t n);
int   netman_tor_ctl_newnym(netman_tor_ctl *t);
int   netman_tor_ctl_reload(netman_tor_ctl *t);

/* commands: up | down | status | newnym | bridge add LINE|clear | exit CC|off | hs add NAME PORT TARGET|list|del NAME */
int   netman_tor_main(netman_tor_cfg *c, int argc, char **argv);   /* argv after the word "tor"; exit status */
int   netman_tor_up(netman_tor_cfg *c);
int   netman_tor_down(netman_tor_cfg *c);
int   netman_tor_status(netman_tor_cfg *c);
int   netman_tor_running(const netman_tor_cfg *c);     /* 1 when our tor is alive */
#endif
