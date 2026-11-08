/* openvpn.h - OpenVPN as a netman_vpnmod (logic moved out of vpn.h, behaviour unchanged). Real .c: openvpn.c.
 *
 * Profile: NAME.ovpn (or NAME.conf holding client/remote/dev/proto lines), stock OpenVPN syntax. Started as
 * `openvpn --daemon --writepid RUN/NAME.pid --log RUN/NAME.log --auth-nocache`; credentials only from a root-only file
 * (`auth-user-pass FILE`, or NAME.auth for a bare `auth-user-pass`), never argv. State is read from the log.
 * Kill switch: the `remote` lines (proto-aware) are the only exits besides tun*.
 */
#ifndef NETMAN_OPENVPN_H
#define NETMAN_OPENVPN_H
#include "vpnmod.h"
int netman_openvpn_detect(const char *text);
int netman_openvpn_args(const char *name, const char *dir, const char *conf, const char *text, const netman_vm_ctx *cx, char *argv[], int max, char *auth, size_t an, int *bare);   /* the argv `up` runs (golden-tested); count or -1 */
#endif
