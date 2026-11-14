/* openconnect.h - Cisco AnyConnect / GlobalProtect / Pulse / NetScaler / F5 / Fortinet / Array through the `openconnect` binary. Real .c: openconnect.c.
 *
 * Profile NAME.vpn:
 *   Type = openconnect
 *   Server = vpn.example.com[:port] | https://vpn.example.com/path          [required]
 *   Protocol = anyconnect | gp | nc | pulse | f5 | fortinet | array          default anyconnect
 *   Username = alice
 *   Password = secret                 sent on the child's STDIN (--passwd-on-stdin), never on argv; omit for certificate login
 *   AuthGroup = group                 (AnyConnect --authgroup)
 *   ServerCert = pin-sha256:BASE64    pin the server certificate (--servercert)
 *   CA = /abs/ca.pem   Cert = /abs/c.pem   Key = /abs/k.pem
 *   Routes = 10.0.0.0/8, ...          split tunnel: a generated wrapper around vpnc-script sets CISCO_SPLIT_INC*, overriding the server's list
 *   Script = /path/vpnc-script        default: first of the usual locations
 * Run as `openconnect --background --non-inter --pid-file RUN/NAME.pid --interface oc-NAME ...`, output in RUN/NAME.log.
 */
#ifndef NETMAN_OPENCONNECT_H
#define NETMAN_OPENCONNECT_H
#include "vpnmod.h"
int netman_oc_args(const char *name, const char *text, const netman_vm_ctx *cx, char *argv[], int max, char *err, size_t en);   /* the argv (golden-tested; static storage); count or -1 */
int netman_oc_script(const char *text, char *out, size_t n, char *err, size_t en);   /* split-tunnel wrapper script; "" when no Routes */
#endif
