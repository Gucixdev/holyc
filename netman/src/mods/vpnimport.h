/* vpnimport.h - turn a foreign VPN profile into a netman profile. Real .c: vpnimport.c. `netman vpn import FILE [NAME]` calls this.
 *
 * Accepted input (detected from the text, not the file name):
 *   WireGuard wg-quick .conf          kept as is                              -> NAME.conf
 *   OpenVPN .ovpn                      kept as is                              -> NAME.ovpn
 *   netman NAME.vpn                    kept as is                              -> NAME.vpn
 *   NetworkManager .nmconnection       type=wireguard  -> wg-quick .conf
 *                                      type=vpn: openvpn -> .ovpn; strongswan, l2tp, openconnect, sstp -> .vpn
 *   strongSwan swanctl.conf / ipsec.conf (the first connection) -> NAME.vpn (type ipsec)
 * PPTP (NetworkManager pptp keyfiles) is REFUSED: MS-CHAPv2/MPPE is broken. Anything it could not carry over is listed in `note`
 * ("not converted: ..."); passwords kept in NetworkManager's secret agent or ipsec.secrets are not imported.
 */
#ifndef NETMAN_VPNIMPORT_H
#define NETMAN_VPNIMPORT_H
#include <stddef.h>
typedef struct { char *text; size_t len; char ext[8]; char type[16]; char note[400]; } netman_vpnimp;
int  netman_vpnimp_convert(const char *text, size_t len, netman_vpnimp *o, char *err, size_t en);   /* 0 ok, -1 + reason */
void netman_vpnimp_free(netman_vpnimp *o);
#endif
