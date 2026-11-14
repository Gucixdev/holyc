/* l2tp.h - L2TP (optionally over IPsec/PSK) as a netman_vpnmod: xl2tpd + pppd, IPsec transport via strongSwan. Real .c: l2tp.c.
 *
 * Profile NAME.vpn:
 *   Type = l2tp
 *   Server = vpn.example.com           the LNS                                  [required]
 *   Username / Password                PPP authentication (CHAP / MS-CHAPv2; PAP and EAP are refused)   [required]
 *   PSK = secret                       L2TP/IPsec: IPsec transport-mode SA with this pre-shared key (needs swanctl)
 *   RemoteId = id                      IPsec identity of the server (default Server)
 *   Routes = a, b                      split tunnel; absent = default route through the tunnel
 *   MTU = 1280
 * Written to RUN: NAME.xl2tpd.conf, NAME.ppp (credentials, 0600), NAME.swanctl.conf (with a PSK). xl2tpd is started with its own
 * config/control/pid files, so a system-wide xl2tpd is untouched. The PPP interface is called l2-NAME (cut to 15 chars).
 */
#ifndef NETMAN_L2TP_H
#define NETMAN_L2TP_H
#include "vpnmod.h"
int netman_l2tp_gen(const char *name, const char *text, char *out, size_t n, char *err, size_t en);   /* xl2tpd.conf (uses the real RUN dir given by netman_l2tp_rundir) */
int netman_l2tp_ppp(const char *name, const char *text, char *out, size_t n, char *err, size_t en);    /* pppd options file */
int netman_l2tp_ipsec(const char *name, const char *text, char *out, size_t n, char *err, size_t en);  /* swanctl.conf for the transport SA ("" when no PSK) */
extern const char *netman_l2tp_rundir;                                                                    /* what netman_l2tp_gen writes into paths (set by up; tests set it) */
#endif
