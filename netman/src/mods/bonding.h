/* bonding.h - link aggregation (bonding): the plain, non-enterprise front end. Real code in bonding.c.
 *
 * ONE implementation: the rtnetlink code that creates bonds, enslaves and releases ports lives in enterprise/enterprise.c
 * (netman_ent_bond_add / _del / _show / netman_ent_bond_slave). This module re-exports it as `netman bond ...` with live status read
 * from sysfs, and keeps the bond as an enterprise profile NAME.ent (kind bond), so `netman ent apply` and `netman bond apply` rebuild it at boot.
 *
 * Modes: balance-rr active-backup balance-xor broadcast 802.3ad balance-tlb balance-alb (numbers 0..6 accepted). 802.3ad needs an LACP
 * partner and a miimon; active-backup needs nothing from the switch. Needs CAP_NET_ADMIN. Switch: bonding.enabled.
 *
 *   bond status | list           one row per live bond:  NAME STATE detail   (STATE up | degraded | down)
 *   bond show NAME               mode, miimon, slaves, 802.3ad aggregator when there is one
 *   bond modes
 *   bond add NAME [mode=M] [miimon=MS] [primary=IF] [lacp_rate=slow|fast] [hash=H] [addr=CIDR] [mtu=N] SLAVE...
 *   bond del NAME                remove the device and the profile
 *   bond slave add|del NAME IF   change the slaves of a live bond (the profile follows)
 *   bond apply                   (re)create every saved bond profile
 */
#ifndef NETMAN_BONDING_H
#define NETMAN_BONDING_H
#include <stdio.h>

int netman_bond_status(FILE *out);                              /* rows for the TUI / `bond status`; number of bonds, <0 error */
int netman_bond_main(FILE *out, FILE *err, int argc, char **argv);   /* argv after the word "bond"; exit status */
extern const char *netman_bond_modes[7];
#endif
