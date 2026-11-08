# Enterprise networking: wiring patch (for the orchestrator)

The logic is done and tested in real `.c` modules; core netman only needs the interface lines below. Nothing in
`src/cli.h`, `src/config.h`, `src/config.c`, `src/help.c`, `src/netman.1` or `run` was touched by this work.

Modules: `src/mods/enterprise.{h,c}` (bonding/team, bridge+STP+vlan filtering, vlan/QinQ, macvlan, ipvlan, veth, dummy,
routes/rules/tables, ECMP failover, link settings, IPv6/PD, Wake-on-LAN, wired 802.1X, `.ent` profiles),
`src/mods/tether.c` (USB/BT detect, share-out, client mode, BT PAN), `src/mods/licence.{h,c}` (licences, hook renewal,
WireGuard key rotation), `src/mods/entcore.h` (private helpers, not an API). Test: `tests/enterprise_test.c`.

## 1. Build (run)

`netman.c` stays one translation unit; the three new modules are separate `.c` files. In `run` the cc line becomes

    "$cc" -std=gnu11 -Wall -Wextra -O2 -Isrc -Isrc/mods src/netman.c src/config.c src/mods/enterprise.c src/mods/tether.c src/mods/licence.c -o bin/netman.tmp

and in `src/netman.c`, after `#include "mods/net.h"` (net.h supplies the implementation the modules link against):

    #include "mods/enterprise.h"
    #include "mods/licence.h"

Bluetooth PAN additionally needs `-DNETMAN_ENT_DBUS` and dbus.h's implementation in the same translation unit as
tether.c, which a separate .c cannot get from netman.c's static dbus code. Until bluez PAN is wanted, leave the flag off:
`btpan` then returns -ENOSYS with a clear message. (To enable it, compile tether.c with `-DNETMAN_ENT_DBUS` and
`-DNETMAN_DBUS_IMPLEMENTATION` plus `_GNU_SOURCE`, and drop the dbus code from netman.c or expose its entry points.)

Test build and run (needs only a compiler; it makes its own user+net namespace and never touches the host network):

    cc -O2 -o tests/enterprise_test tests/enterprise_test.c src/mods/enterprise.c src/mods/tether.c src/mods/licence.c
    nice -n 19 tests/enterprise_test

## 2. Config: `src/config.h` (add inside NC_KEYS, before the TUI lines) and `src/config.c`

    X(ENT_ENABLED, "enterprise", "enabled", "bool", "no", "Enterprise networking: bonding, bridges, vlans, routes and rules, ECMP, tethering. Off by default: it changes links and routes.") \
    X(ENT_PROBE_MS, "enterprise", "probe_ms", "int:100:60000", "1000", "ECMP: per-uplink ICMP probe timeout and the tick between probes (ms).") \
    X(LIC_DIR, "enterprise", "licence_dir", "path", "/etc/netman/licences", "Where licence (.lic) and VPN key (.key) files live.") \
    X(LIC_WARN, "enterprise", "licence_warn_days", "int:0:365", "30", "`licence status` flags a licence expiring within this many days.") \
    X(LIC_RENEW_HOOK, "enterprise", "renew_hook", "path", "", "Command (split on spaces, no shell) run by `licence renew NAME`: gets NAME CURRENT_PATH NEW_PATH and must write the renewed licence to NEW_PATH. netman never contacts a network for renewal itself.") \
    X(LIC_APPLY_HOOK, "enterprise", "key_apply_hook", "path", "", "Command run after `key rotate NAME`: gets NAME NEW_PUBKEY; failing restores the old key.") \

`src/config.c`: add `"enterprise"` to `sections[]` (after `"firewall"`).

`src/cli.h` `nc_enabled()` table: add `{ "enterprise", NC_ENT_ENABLED }`.

Before any command: apply config to the module tunables once (e.g. in `main` after loading `nc_cfg`):

    netman_lic_dir = netman_config_str(&nc_cfg, NC_LIC_DIR);
    netman_lic_warn_days = netman_config_int(&nc_cfg, NC_LIC_WARN);
    netman_lic_renew_hook = netman_config_str(&nc_cfg, NC_LIC_RENEW_HOOK);
    netman_lic_apply_hook = netman_config_str(&nc_cfg, NC_LIC_APPLY_HOOK);

## 3. Handlers (`src/cli.h`, above the command table)

    /* ---- enterprise ---------------------------------------------------------------------------------------------- */
    static int nc_ent_off(void) { if (netman_config_bool(&nc_cfg, NC_ENT_ENABLED)) return 0; fputs("netman: enterprise changes are disabled in config (netman config set enterprise.enabled yes)\n", stderr); return 3; }
    static int nc_ent_err(int r) { if (r >= 0) return 0; fprintf(stderr, "netman: %s\n", r == -22 && *netman_ent_error() ? netman_ent_error() : *netman_ent_error() ? netman_ent_error() : strerror(-r)); return 1; }
    static int h_ent_list(const nc_cmd *c, int n, char **a)
    {
    	char names[64][64]; int k = netman_ent_list(names, 64);
    	(void)c; (void)n; (void)a;
    	for (int i = 0; i < k; i++) { netman_ent e; if (netman_ent_load(names[i], &e) == 0) printf("%-20s %s\n", names[i], e.kind); else printf("%-20s (invalid: %s)\n", names[i], netman_ent_error()); }
    	return 0;
    }
    static int h_ent_show(const nc_cmd *c, int n, char **a)
    {
    	netman_ent e; (void)c; (void)n;
    	if (nc_ent_err(netman_ent_load(a[0], &e))) return 1;
    	return nc_ent_err(netman_ent_show(&e, stdout));
    }
    static int h_ent_add(const nc_cmd *c, int n, char **a)
    {
    	netman_ent e; (void)c; (void)n;
    	if (nc_ent_off()) return 3;
    	if (nc_ent_err(netman_ent_load(a[0], &e))) return 1;
    	return nc_ent_err(netman_ent_add(&e));
    }
    static int h_ent_del(const nc_cmd *c, int n, char **a)
    {
    	netman_ent e; (void)c; (void)n;
    	if (nc_ent_off()) return 3;
    	if (nc_ent_err(netman_ent_load(a[0], &e))) return 1;
    	return nc_ent_err(netman_ent_del(&e));
    }
    static int h_ent_save(const nc_cmd *c, int n, char **a)   /* ent save NAME KIND [KEY=VALUE...] */
    {
    	char text[8192] = ""; netman_ent e; (void)c;
    	snprintf(text, sizeof text, "name=%s\nkind=%s\n", a[0], a[1]);
    	for (int i = 2; i < n; i++) { strncat(text, a[i], sizeof text - strlen(text) - 2); strcat(text, "\n"); }
    	if (nc_ent_err(netman_ent_parse(text, &e))) return 1;
    	return nc_ent_err(netman_ent_save(&e));
    }
    static int h_ent_apply(const nc_cmd *c, int n, char **a)
    {
    	(void)c; (void)n; (void)a;
    	if (nc_ent_off()) return 3;
    	return netman_ent_apply_all() ? 1 : 0;
    }
    static volatile sig_atomic_t nc_ent_stop;
    static void nc_ent_sig(int s) { (void)s; nc_ent_stop = 1; }
    static int h_ent_ecmp(const nc_cmd *c, int n, char **a)   /* foreground failover loop; the daemon can call the same */
    {
    	netman_ent e; int ms = netman_config_int(&nc_cfg, NC_ENT_PROBE_MS); (void)c; (void)n;
    	if (nc_ent_off()) return 3;
    	if (nc_ent_err(netman_ent_load(a[0], &e))) return 1;
    	if (strcmp(e.kind, "ecmp")) { fputs("netman: not an ecmp profile\n", stderr); return 1; }
    	signal(SIGINT, nc_ent_sig); signal(SIGTERM, nc_ent_sig);
    	return nc_ent_err(netman_ent_ecmp_run(&e, ms, &nc_ent_stop));
    }
    static int h_ent_tether(const nc_cmd *c, int n, char **a)
    {
    	netman_ent_tether_dev d[16]; int k = netman_ent_tether_detect(d, 16);
    	(void)c; (void)n; (void)a;
    	if (k <= 0) { puts("no USB or Bluetooth tethering device found"); return k < 0 ? 1 : 0; }
    	for (int i = 0; i < k; i++) printf("%-10s %-4s %s\n", d[i].ifn, d[i].kind, d[i].driver);
    	return 0;
    }
    static int h_wol(const nc_cmd *c, int n, char **a)
    {
    	(void)c; (void)n;
    	return nc_ent_err(netman_ent_wol_send(a[0], a[1]));
    }
    static int h_lic_status(const nc_cmd *c, int n, char **a)
    {
    	(void)c; (void)n; (void)a;
    	return netman_lic_status(stdout) > 0 ? 1 : 0;      /* exit 1 when anything is expiring/expired: usable in cron */
    }
    static int h_lic_renew(const nc_cmd *c, int n, char **a)
    {
    	(void)c; (void)n;
    	return nc_ent_err(netman_lic_renew(a[0]));
    }
    static int h_key_rotate(const nc_cmd *c, int n, char **a)
    {
    	char pub[45]; (void)c; (void)n;
    	if (nc_ent_err(netman_key_rotate(a[0], pub))) return 1;
    	printf("%s\n", pub);                              /* new public key: hand it to the peer */
    	return 0;
    }
    static int h_key_show(const nc_cmd *c, int n, char **a)
    {
    	char pub[45]; (void)c; (void)n;
    	if (nc_ent_err(netman_key_show(a[0], pub))) return 1;
    	printf("%s\n", pub);
    	return 0;
    }

## 4. Command table rows (`src/cli.h`, in `nc_table[]`; format `{ name, args, handler, root, module, summary, help }`)

    { "ent list", "", h_ent_list, R_NO, "enterprise", "saved enterprise profiles (bond, bridge, vlan, route, ecmp, tether ...)", "" },
    { "ent show", "NAME", h_ent_show, R_YES, "enterprise", "live state of a profile's device", "" },
    { "ent save", "NAME KIND [KEY=VALUE...]", h_ent_save, R_YES, "enterprise", "create or replace a profile", "KIND: bond team bridge vlan macvlan ipvlan veth dummy tether dot1x route rule ecmp link ipv6 wol. Keys are the profile keys, e.g. `ent save bond0 bond mode=802.3ad member=eth0 member=eth1 miimon=100`. Stored as NAME.ent in the netman config directory." },
    { "ent add", "NAME", h_ent_add, R_YES, "enterprise", "create the device / install the routes of a profile", "Needs enterprise.enabled = yes. Everything goes through rtnetlink, ethtool ioctls and /proc/sys; only nft, dnsmasq and wpa_supplicant are ever exec'd, and only for tether and 802.1X." },
    { "ent del", "NAME", h_ent_del, R_YES, "enterprise", "tear a profile's device / routes down", "" },
    { "ent apply", "", h_ent_apply, R_YES, "enterprise", "apply every saved profile in dependency order", "dummy/veth, bond/team, bridge, vlan/macvlan/ipvlan, link/ipv6/wol, route/rule/ecmp, tether/dot1x." },
    { "ent ecmp", "NAME", h_ent_ecmp, R_YES, "enterprise", "run the multi-uplink failover loop for an ecmp profile", "Probes each uplink with an ICMP echo bound to its interface; fail_after / ok_after consecutive results flip it, the default route (metric 50) follows. Ctrl-C stops." },
    { "ent tether", "", h_ent_tether, R_NO, "enterprise", "detected USB (RNDIS/NCM/ECM/ipheth) and Bluetooth PAN tethering devices", "" },
    { "wol", "IFACE MAC", h_wol, R_YES, "enterprise", "send a Wake-on-LAN magic packet out of IFACE", "UDP broadcast to port 9. Setting the NIC itself to wake on LAN is `ent save NAME wol wol=g`." },
    { "licence status", "", h_lic_status, R_NO, "enterprise", "licences and their expiry (exit 1 when any expires soon or has)", "Files NAME.lic in enterprise.licence_dir. Warns inside enterprise.licence_warn_days." },
    { "licence renew", "NAME", h_lic_renew, R_YES, "enterprise", "renew a licence through enterprise.renew_hook", "The hook gets NAME CURRENT_PATH NEW_PATH and writes the new licence; netman checks it names the same licence and expires later, then swaps atomically and keeps NAME.lic.prev. netman itself never contacts a network." },
    { "key rotate", "NAME", h_key_rotate, R_YES, "enterprise", "new WireGuard key for NAME; prints the new public key", "Writes NAME.key mode 0600, keeps the old one as NAME.key.prev, then runs enterprise.key_apply_hook (NAME PUBKEY); if the hook fails the old key is restored. No `wg` binary needed." },
    { "key show", "NAME", h_key_show, R_YES, "enterprise", "public key of NAME.key", "" },

(`R_NO` is whatever the table calls "no root needed"; use the existing constant.) `licence` and `key` are new command
groups for `help`; `wol` and `ent` are too.

## 5. Help topic text (`src/help.c`, in the group summary list, if groups carry a one-line description)

    ent      enterprise networking: bonding/teaming, bridges with STP and vlan filtering, vlans and QinQ, macvlan/ipvlan, veth, routes, rules and tables, ECMP with failover, link settings, IPv6 prefix delegation, USB/Bluetooth tethering, wired 802.1X
    licence  licence files, expiry check, hook-driven renewal
    key      WireGuard key rotation
    wol      Wake-on-LAN

Longer text for `netman help ent`:

    Profiles are NAME.ent files ("key = value", same syntax as net profiles) in the netman config directory.
    team is bonding: the kernel team driver is deprecated, so kind=team maps runner= (activebackup, lacp, loadbalance,
    broadcast, roundrobin) onto the bonding driver. Tethering share-out uses its own nft table `inet netman_tether`
    (firewall.h replaces `inet netman`, so the two never clobber each other); an accept there cannot override a drop in
    another table, so a strict firewall must allow the downstream interface itself.

## 6. Man page (`src/netman.1`) is generated from the table by help.c; nothing to edit by hand.

## What is and is not verified

Tested in `unshare -Urn` (177 checks): spec parse/format, dummy, bonding (active-backup, balance-xor, broadcast, 802.3ad
with lacp_rate and hash policy, primary), team mapping, bridge (STP, forward delay, vlan filtering, per-port vlans),
vlan, QinQ, macvlan, ipvlan, veth, routes (tables incl. >255, blackhole, v6), rules, ECMP (scripted hysteresis plus real
ICMP against a peer netns), link MTU/offloads, IPv6 sysctls and /64 carving, Wake-on-LAN packet and a real broadcast
received in a peer netns, tether detection against a fake sysfs, nft ruleset loaded into the kernel and listed back,
dnsmasq config syntax-checked by `dnsmasq --test`, 802.1X config text, licences, hook renewal, key rotation (RFC 7748
vector).

Skipped or unverifiable here: ethtool speed/duplex and Wake-on-LAN setting (dummy/lo drivers refuse); the dnsmasq and
wpa_supplicant daemons (cannot drop privileges / own /run in a single-uid user namespace); a real USB RNDIS/CDC phone;
Bluetooth PAN (needs bluez and the D-Bus build flag); 802.1X against a real authenticator.
