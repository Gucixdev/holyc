/* config.h - netman.conf: THE settings file. Plain key=value, # comments, [sections]. Implementation: config.c (no module dependencies).
 *
 * Files, later wins:  /etc/netman/netman.conf, then $XDG_CONFIG_HOME/netman/netman.conf (default ~/.config/netman/netman.conf; under sudo
 * the invoking user's, via SUDO_USER). NETMAN_CONFIG=FILE reads only that file (it must exist). Environment variables NETMAN_<SECTION>_<KEY>
 * (upper case, e.g. NETMAN_WIFI_BACKEND, NETMAN_METRICS_DIR) override the files; NETMAN_SUDO and NM_SUDO are old names of NETMAN_PRIVILEGES_SUDO.
 *
 * The keys are defined ONCE, in NC_KEYS below (section, name, type, default, documentation). Everything else is generated from that
 * table: the enum, validation, `netman config show/set/toggle`, `netman config example` and the shipped src/netman.conf.example
 * (a test keeps the file in sync). A key can be written as `[wifi]` + `backend = iwd` or at the top as `wifi.backend = iwd`.
 *
 * Strict: an unknown key or section (with a "did you mean"), a bad value, a duplicate key in one file, a NUL byte or a line over 1023
 * bytes is an error "FILE:LINE: message". Types: bool (yes|no|true|false|on|off|1|0, stored as yes/no), enum:a|b|c, int:LO:HI, path
 * (absolute, ~/ allowed), url (http://HOST[:PORT]/PATH or empty).
 */
#ifndef NETMAN_CONFIG_H
#define NETMAN_CONFIG_H
#include <stddef.h>
#include <stdio.h>

/* X(ID, section, name, type, default, documentation) */
#define NC_KEYS(X) \
	X(WIFI_ENABLED, "wifi", "enabled", "bool", "yes", "Wi-Fi support: the wifi commands, the Wi-Fi tab. no = skipped everywhere (CLI says 'wifi disabled in config').") \
	X(WIFI_BACKEND, "wifi", "backend", "enum:auto|iwd|wpa", "auto", "Which daemon to talk to. auto = wpa_supplicant if its control socket exists for the interface, else iwd.") \
	X(WIFI_AUTOCONNECT, "wifi", "autoconnect", "bool", "yes", "The daemon joins known networks by itself (per-network switch: wifi known autoconnect SSID on|off).") \
	X(WIFI_RSSI_GOOD, "wifi", "rssi_good", "int:-120:0", "-55", "dBm at or above which a signal is 'good' (green in the TUI, 4 quality steps).") \
	X(WIFI_RSSI_OK, "wifi", "rssi_ok", "int:-120:0", "-65", "dBm at or above which a signal is 'ok'. Must be lower than rssi_good.") \
	X(WIFI_RSSI_WEAK, "wifi", "rssi_weak", "int:-120:0", "-75", "dBm at or above which a signal is 'weak' (below it: bad). Must be lower than rssi_ok.") \
	X(BT_ENABLED, "bluetooth", "enabled", "bool", "yes", "Bluetooth support: the bt commands, the Bluetooth tab.") \
	X(BT_AGENT, "bluetooth", "agent", "bool", "yes", "Register netman's own pairing agent (PIN / passkey / confirmation prompts). no = rely on another agent such as your desktop's.") \
	X(NET_ENABLED, "net", "enabled", "bool", "yes", "Ethernet / IP management: the net and roam commands, the Ethernet/IP tab.") \
	X(NET_DHCP, "net", "dhcp", "bool", "yes", "Built-in DHCPv4 client. no = `net dhcp` is refused and dhcp profiles are not applied (static profiles only).") \
	X(NET_DNS_MODE, "net", "dns_mode", "enum:auto|resolved|resolvconf", "auto", "Where DNS servers go: resolved = systemd-resolved, resolvconf = write /etc/resolv.conf, auto = resolved when it runs.") \
	X(NET_STATE_DIR, "net", "state_dir", "path", "/var/lib/netman", "Lease cache and the per-machine MAC secret.") \
	X(ROAM_ENABLED, "roaming", "enabled", "bool", "yes", "Autoconnect policy and the `roam` commands.") \
	X(ROAM_PROBE, "roaming", "probe", "bool", "yes", "Check connectivity after a network comes up (needs probe_url); a captive portal or no internet counts as a failure.") \
	X(ROAM_PROBE_URL, "roaming", "probe_url", "url", "http://connectivitycheck.gstatic.com/generate_204", "Plain-http URL fetched by the probe: 204 = internet, any other answer = captive portal. Empty = no probing.") \
	X(ROAM_RANDOM_MAC, "roaming", "random_mac", "bool", "no", "Use a stable random (locally administered) MAC per profile instead of the hardware address.") \
	X(ROAM_SCORE, "roaming", "score_threshold", "int:0:100", "20", "A network scoring below this (0..100: signal, gateway RTT, loss, captive portal) is dropped for another candidate.") \
	X(VPN_ENABLED, "vpn", "enabled", "bool", "yes", "WireGuard / OpenVPN / provider support: the vpn commands, the VPN tab.") \
	X(VPN_KILLSWITCH, "vpn", "killswitch", "bool", "no", "`vpn up` also turns the nftables kill switch on (traffic only through the tunnel).") \
	X(VPN_WATCHDOG, "vpn", "watchdog", "bool", "yes", "The daemon reconnects a tunnel that died or whose handshake went stale.") \
	X(VPN_OPENVPN, "vpn", "openvpn", "bool", "yes", "OpenVPN profiles (NAME.ovpn). no = `vpn up` refuses them.") \
	X(VPN_IPSEC, "vpn", "ipsec", "bool", "yes", "IPsec / IKEv2 profiles (Type = ipsec, strongSwan swanctl). no = `vpn up` refuses them.") \
	X(VPN_L2TP, "vpn", "l2tp", "bool", "yes", "L2TP/IPsec profiles (Type = l2tp, xl2tpd + pppd). no = `vpn up` refuses them.") \
	X(VPN_OPENCONNECT, "vpn", "openconnect", "bool", "yes", "OpenConnect profiles: AnyConnect, GlobalProtect, Pulse, ... (Type = openconnect). no = `vpn up` refuses them.") \
	X(VPN_SSTP, "vpn", "sstp", "bool", "yes", "SSTP profiles are recognised but not supported (no maintained client): this only changes the message.") \
	X(FW_ENABLED, "firewall", "enabled", "bool", "no", "Firewall management. no = fw apply / confirm / rollback are refused (show, plan, diff and preset still work). Off by default: it changes your rules.") \
	X(FW_PRESET, "firewall", "preset", "enum:workstation|lockdown", "workstation", "Ruleset `fw plan` and `fw apply` start from when you name no preset.") \
	X(FW_CONFIRM, "firewall", "confirm_s", "int:0:3600", "30", "fw apply rolls back by itself unless you run `fw confirm` within this many seconds. 0 = apply for good at once.") \
	X(FW_ZONE, "firewall", "default_zone", "enum:trusted|home|public|untrusted", "public", "Zone of interfaces that have none: trusted = accept all, home, public (stealth + limits), untrusted (nothing in).") \
	X(METRICS_ENABLED, "metrics", "enabled", "bool", "yes", "Interface counters and rates: the metrics commands, the rates in the TUI.") \
	X(METRICS_DIR, "metrics", "dir", "path", "/var/lib/netman/metrics", "Where `metrics record` appends one TSV file per interface (rotated at 1 MiB).") \
	X(METRICS_INTERVAL, "metrics", "interval_s", "int:1:3600", "5", "Seconds between samples for `metrics watch` and `metrics record --loop`.") \
	X(ENT_ENABLED, "enterprise", "enabled", "bool", "no", "Enterprise networking: bonding, bridges, vlans, routes and rules, ECMP, tethering. Off by default: it changes links and routes.") \
	X(ENT_PROBE_MS, "enterprise", "probe_ms", "int:100:60000", "1000", "ECMP: per-uplink ICMP probe timeout and the tick between probes (ms).") \
	X(LIC_DIR, "enterprise", "licence_dir", "path", "/etc/netman/licences", "Where licence (.lic) and VPN key (.key) files live.") \
	X(LIC_WARN, "enterprise", "licence_warn_days", "int:0:365", "30", "`licence status` flags a licence expiring within this many days.") \
	X(LIC_RENEW_HOOK, "enterprise", "renew_hook", "cmd", "", "Command (split on spaces, no shell) run by `licence renew NAME`: gets NAME CURRENT_PATH NEW_PATH and must write the renewed licence to NEW_PATH. netman never contacts a network for renewal itself.") \
	X(LIC_APPLY_HOOK, "enterprise", "key_apply_hook", "cmd", "", "Command run after `key rotate NAME`: gets NAME NEW_PUBKEY; failing restores the old key.") \
	X(LICENCE_ENABLED, "licence", "enabled", "bool", "yes", "Licence files, expiry check, renewal hook and WireGuard key rotation: the licence and key commands, the licence line in the Enterprise tab.") \
	X(TETHER_ENABLED, "tether", "enabled", "bool", "no", "USB / Bluetooth tethering detection and share-out: `ent tether`, tether profile adds, the tether rows in the Enterprise tab. Off by default: sharing changes firewall and forwarding.") \
	X(WG_ENABLED, "wireguard", "enabled", "bool", "yes", "Native WireGuard (generic netlink): the wg commands and WireGuard profiles of the vpn commands. no = only OpenVPN profiles work.") \
	X(NMAPI_ENABLED, "nmapi", "enabled", "bool", "yes", "NetworkManager D-Bus API (org.freedesktop.NetworkManager) served by `netman daemon`, so nm-applet / libnm / nmcli style clients work against netman.") \
	X(NMAPI_REFRESH, "nmapi", "refresh_s", "int:1:3600", "5", "Seconds between the daemon's looks at the links and the Wi-Fi state (it emits the D-Bus signals for whatever changed).") \
	X(TOR_ENABLED, "tor", "enabled", "bool", "no", "Tor routing (`netman tor ...`, the Tor tab). Off by default: it loads a fail-closed firewall table and runs a tor process. Needs tor installed.") \
	X(TOR_MODE, "tor", "mode", "enum:transparent|socks", "transparent", "transparent: nftables sends the routed users' TCP and DNS through Tor and DROPS everything else (no leaks). socks: only prints the SOCKS address; apps that ignore it are not protected.") \
	X(TOR_SOCKS, "tor", "socks_port", "int:1:65535", "9050", "Tor SocksPort on 127.0.0.1 (per-app mode).") \
	X(TOR_TRANS, "tor", "trans_port", "int:1:65535", "9040", "Tor TransPort on 127.0.0.1 (transparent mode).") \
	X(TOR_DNS, "tor", "dns_port", "int:1:65535", "5353", "Tor DNSPort on 127.0.0.1 (transparent mode).") \
	X(TOR_CONTROL, "tor", "control_port", "int:0:65535", "0", "Tor ControlPort on 127.0.0.1. 0 = a unix socket in tor.dir (default, safer). Cookie authentication either way.") \
	X(TOR_DIR, "tor", "dir", "path", "/run/netman/tor", "Where the generated torrc, data directory, cookie and hidden-service keys live (tmpfs by default).") \
	X(TOR_PROFILE, "tor", "profile", "path", "/etc/netman/tor.conf", "Persisted bridges, exit country and hidden services (what `netman tor bridge|exit|hs` edit).") \
	X(TOR_USER, "tor", "user", "list", "tor", "Account tor runs as. Its traffic is the only one the firewall lets out.") \
	X(TOR_UIDS, "tor", "route_uids", "list", "", "Numeric uids forced through Tor, comma separated. Empty = everybody but tor's own user.") \
	X(TOR_IFACES, "tor", "route_ifaces", "list", "", "Interfaces whose forwarded traffic (hotspot, router) is also forced through Tor, e.g. wlan1. Empty = none.") \
	X(TOR_OBFS4, "tor", "obfs4_bin", "cmd", "/usr/bin/obfs4proxy", "obfs4 pluggable transport binary, used when an obfs4 bridge is set.") \
	X(TOR_SNOW, "tor", "snowflake_bin", "cmd", "/usr/bin/snowflake-client", "Snowflake pluggable transport binary, used when a snowflake bridge is set.") \
	X(TUI_THEME, "tui", "theme", "enum:default|mono|light", "default", "TUI colours.") \
	X(TUI_UNICODE, "tui", "unicode", "enum:auto|yes|no", "auto", "Box drawing and glyphs. auto = by locale; no = plain ASCII.") \
	X(PRIV_SUDO, "privileges", "sudo", "enum:auto|never|always", "auto", "When netman re-runs itself through sudo: auto = only when the command needs root, never, always.")

enum {
#define X(id, sec, name, type, def, doc) NC_##id,
	NC_KEYS(X)
#undef X
	NC_N
};

typedef struct {
	char v[NC_N][256];        /* normalised value of each key (bools are "yes"/"no") */
	char origin[NC_N][160];   /* where it came from: "default", "FILE:LINE", "env NAME", "set" */
} netman_config;

void netman_config_defaults(netman_config *c);
const char *netman_config_section(int k);
const char *netman_config_name(int k);          /* bare key name */
const char *netman_config_type(int k);
const char *netman_config_default(int k);
const char *netman_config_doc(int k);
void netman_config_fullname(int k, char *out, size_t n);   /* "wifi.backend" */
int netman_config_find(const char *dotted);     /* index or -1 */
int netman_config_suggest(const char *name, char *out, size_t n);   /* closest known "sec.key" (or section); 1 if one is close enough */

const char *netman_config_str(const netman_config *c, int k);
int netman_config_bool(const netman_config *c, int k);
int netman_config_int(const netman_config *c, int k);

/* check VALUE for key K; norm gets the stored form (yes/no, ~ expanded). 0 ok, -1 + reason in msg */
int netman_config_check_value(int k, const char *value, char *norm, size_t nn, char *msg, size_t mn);
/* parse TEXT (len bytes) as one file called FNAME; 0 or -1 with "FNAME:LINE: message" in err. On error c may be half-updated. */
int netman_config_parse(netman_config *c, const char *text, size_t len, const char *fname, char *err, size_t errsz);
int netman_config_load_file(netman_config *c, const char *path, int must_exist, char *err, size_t errsz);
int netman_config_env(netman_config *c, char *err, size_t errsz);              /* NETMAN_* overrides */
int netman_config_load(netman_config *c, char *err, size_t errsz);             /* defaults + files + env */
void netman_config_show(const netman_config *c, FILE *o);                      /* "sec.key = value   # origin" lines, re-parseable */
void netman_config_example(FILE *o);                                           /* the documented example file (generated from NC_KEYS) */
const char *netman_config_user_path(char *buf, size_t n);                      /* the file `config set` edits by default */
/* edit PATH atomically (mkstemp + rename in the same directory), comments and layout kept; the result is re-parsed before it replaces the file */
int netman_config_set_file(const char *path, const char *dotted, const char *value, char *err, size_t errsz);
int netman_config_toggle_file(const char *path, const char *dotted, char *err, size_t errsz);   /* bool keys: flips the effective value */
int netman_config_split_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn);   /* roaming.probe_url pieces; 0 ok */
#endif
