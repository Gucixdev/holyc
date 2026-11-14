/* sstp.h - SSTP (Microsoft Secure Socket Tunneling Protocol): recognised, NOT supported. Real .c: sstp.c.
 *
 * `Type = sstp` profiles are accepted by import and listed, but `up` refuses with a clear message. The only client is sstp-client
 * (`sstpc`, driven through pppd); it is not packaged as a maintained component here and the glue could not be tested, so netman does
 * not pretend. Use OpenConnect or IPsec/IKEv2 where the server offers them. When sstpc exists on $PATH `info` says so.
 */
#ifndef NETMAN_SSTP_H
#define NETMAN_SSTP_H
#include "vpnmod.h"
#endif
