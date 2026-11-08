/* ipsec.h - IPsec / IKEv2 through strongSwan as a netman_vpnmod. Real .c: ipsec.c.
 *
 * Profile NAME.vpn (key = value, # comments; keys ignore case, spaces, _ and -):
 *   Type = ipsec
 *   Server = vpn.example.com            remote address (host or IP)           [required]
 *   RemoteId = vpn.example.com          the server's identity (default: Server)
 *   LocalId = alice@example.com         our identity (psk / cert; EAP uses Username)
 *   Auth = eap | psk | cert             default eap
 *   Username / Password                 EAP (the profile is root-only, mode 0600)
 *   PSK = secret                        Auth = psk
 *   Cert / Key / CA = /abs/path.pem     Auth = cert (absolute paths; unencrypted key)
 *   Routes = 10.0.0.0/8, 192.168.1.0/24 split tunnel; absent = full tunnel (0.0.0.0/0, ::/0)
 *   VirtualIP = yes|no                  ask the server for an inner address (default yes)
 *   Proposals / EspProposals            strongSwan proposal strings (default: strongSwan's own defaults)
 *   Version = 2|1                       IKE version (default 2)
 * netman writes RUN/NAME.swanctl.conf (0600), then `swanctl --load-all --file` and `swanctl --initiate`. Secrets are written as
 * hex in that file, never on a command line. charon-cmd is NOT driven: it asks for the password on a terminal.
 */
#ifndef NETMAN_IPSEC_H
#define NETMAN_IPSEC_H
#include "vpnmod.h"
int netman_ipsec_gen(const char *name, const char *text, char *out, size_t n, char *err, size_t en);   /* swanctl.conf text; 0 ok */
#endif
