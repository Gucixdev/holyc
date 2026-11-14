# netman

## edit 
Glib chcemy i Gobject jezeli wprowadzaja naprawde duza wartosc XD 
pythona nie chcemy napewno 
ale to samo dbus api by bylo dropin i zgodne z standardami 
ma pokrywac cala funkcjonalnosc networkmanager ale tak jak nie snilo sie robic tego w pythonie 
a tui trzeba odswierzac recznie za pomoca r poprostu XD 


A small replacement for NetworkManager + bluez tooling, in plain C: no Python, no GLib, no libdbus. Backends are iwd or wpa_supplicant
for Wi-Fi and bluez for Bluetooth, plus own netlink/DHCP/DNS, WireGuard/OpenVPN, nftables and interface metrics. Short alias in prose: `netmgr`.

## layout

    src/netman.c        the one translation unit (main); includes the modules below
    src/cli.h           the command table: dispatch, root flag, module, summary, help of every command (one source)
    src/help.c          `netman help [topic]`, the man page (`help --man` = src/netman.1) and the TUI key text (`nc_tui_help`, shown by `?`)
    src/config.c/.h     settings file + env (NC_KEYS table); src/netman.conf.example is documented and kept in sync by a test
    src/priv.h          when to re-run through sudo
    src/tui.h           the terminal UI
    src/mods/           stb-style libraries: wifi.h btth.h dbus.h net.h roaming.h scoring.h metrics.h vpn.h firewall.h wireguard.h (+ wireguard.c, enterprise.c, tether.c, licence.c: plain .c, not stb) (+ vpnmod/openvpn/ipsec/l2tp/openconnect/sstp/vpnimport/tor .c: the VPN client modules, profile import, Tor; + ssh.c mesh.c bonding.c tunnel.c: the optional network mods below, each off by default, shared helpers in modkit.h; + enterprise/: the enterprise mods)
    src/daemon.c, daemonless.c   placeholders for the future daemon (it must respect the same config)

## use

    ./run                       build (cc/gcc/clang, one command, no make) and open the TUI; says so if there is no compiler
    bin/netman                  status of links, wifi, bluetooth and vpn
    bin/netman help [TOPIC]     every command grouped; `help wifi`, `help wifi connect`, `help tui`, `help config`, `help --man`
    bin/netman -tui             the TUI (`--tui` and `-t` also work)

`netman help` lists the table (wifi, bt, net, roam, vpn, fw, metrics, config). A module switched off in the config is skipped with
a message (`wifi disabled in config (netman config toggle wifi.enabled)`, exit 3) and its tab is hidden in the TUI.

## configuration

One file format, `/etc/netman/netman.conf` then `~/.config/netman/netman.conf` (`NETMAN_CONFIG` = only that file), then env
`NETMAN_<SECTION>_<KEY>`. `netman config show|check|set KEY VALUE|toggle KEY|example`; typos get a did-you-mean; a broken file stops
every command except `help`, `config` and `fw preset`. See `src/netman.conf.example` (every key documented).

## privileges

netman re-runs itself through `sudo -p "[netman] password for %p: " -- netman --priv-done ARGS` before the screen is drawn, only when
the command needs it (a per-command flag in the table: no / yes / only with a closed wpa_supplicant socket / never). `privileges.sudo =
auto|always|never`, `NETMAN_SUDO`, `NM_SUDO_CMD` (the sudo binary).

| action                              | needs root?                                                                 |
|-------------------------------------|-----------------------------------------------------------------------------|
| Bluetooth (bluez calls our agent)   | no                                                                          |
| Wi-Fi via iwd                       | no if you are in `wheel` or `network` (iwd's D-Bus policy)                  |
| Wi-Fi via wpa_supplicant            | yes, unless its socket dir is group-readable (see below)                    |
| netlink / DHCP / DNS / nft / VPN    | yes                                                                         |

Group route instead of sudo (wpa_supplicant only; run these yourself, netman never touches /etc):

    ! sudo groupadd -f netdev && sudo usermod -aG netdev "$USER"
    # in wpa_supplicant.conf:  ctrl_interface=DIR=/run/wpa_supplicant GROUP=netdev
    # then log out and in again

## TUI

Tabs Wi-Fi | Ethernet/IP | Bluetooth | VPN | Enterprise (read-only, only with enterprise.enabled = yes). Calls that can block run in a worker (spinner, esc cancels). `?` shows the same text as `netman help tui`.

- Wi-Fi rows: `lock-glyph  name  TAG  -52 dBm  96%  saved auto`. Hidden networks show `<hidden> BSSID`, control bytes are escaped, long/CJK names are
  cut by display cells with an ellipsis. Security is a lock plus a text tag (WPA2/WPA3/WEP/OPEN/802.1X); without Unicode it is `[*]` / `open`;
  OPEN gets a warning colour. Signal is numbers only, coloured by `wifi.rssi_good/ok/weak`; the connected (and selected) network also shows its last readings and a trend arrow.
- `s` cycles sort (signal / name / secured first / open first), `o` the filter (all / only open / only secured), `/` filters by name, `r` rescans.
- `i` details: SSID, BSSIDs, security, channel/band, signal, noise, rates, saved/autoconnect, IP, gateway, DNS, lease, link speed, rx/tx, backend; for
  Bluetooth address, class, RSSI, paired/trusted/connected, battery, profiles.
- Enter on a Wi-Fi row: saved networks connect at once; secured ones open a masked passphrase modal (network + security shown, tab shows/hides, a wrong
  password is reported with a retry, backend errors appear in the modal); 802.1X asks identity then password; OPEN asks for confirmation; a hidden row asks for the SSID.
- Ethernet/IP: per-interface state, speed, live rx/tx, addresses, gateway (refreshes every second). VPN: profiles with state, enter = up/down.

## settings (one schema, three front ends)

`src/settings.c` derives the schema (section, key, kind bool/int/enum/string/path/cmd, default, range/choices, one-line description,
needs-root and restart-needed flags, tab, hidden-while-the-module-is-off) from the `NC_KEYS` table, so no key is listed twice;
`tests/settings_test.c` fails when a key has no schema entry. `help config`, the man page and `config show` read the same data.

    netman settings list|get KEY|set KEY VALUE|toggle KEY|reset KEY|check [--json]
    netman settings --dump-api      JSON: schema + current values + source (default/system/user/env)
    netman settings schema          desktop's settings.schema pipe format (key|type|extra|apply|default|label)
    netman settings menu | dmenu | netman settings pick     (NETMAN_MENU overrides the value menu, default dmenu)

Writes go to `~/.config/netman/netman.conf` only (atomic rename, comments kept; a bad value leaves the file unchanged). In the TUI the panel
is key `S`: tabs, checkboxes, enum cycling, inline editing with validation, `/` search, `d` resets, mouse (click, wheel); changes apply live
where safe and are flagged "restart needed" otherwise; module tabs appear only while the module is enabled. Works at 80x24 and 40x12.

## architecture: no resident daemon by default

Every command is a one-shot process: it reads the config, does its netlink/D-Bus/socket work and exits. `netman daemon` is optional (autoconnect
policy, watchdog, the NetworkManager D-Bus API); nothing needs it for status, settings or the TUI. Measured on this machine (release build, one
process, config in a temp dir): `netman status` peak RSS about 4.6 MiB and first output after about 3 ms (median of 20, it talks netlink);
`netman settings list` peak RSS about 2.4 MiB (the C runtime alone is 2.3 MiB) and first output after under 1 ms.

## VPN types, import, Tor

`netman vpn up NAME` picks the client from the profile in `/etc/netman/vpn` (root-owned, 0600): `NAME.conf` WireGuard (native, no wg-quick),
`NAME.ovpn` OpenVPN, `NAME.vpn` everything else, a `Key = value` file whose `Type =` selects the module (each is its own .c/.h in `src/mods`).
Every type gets the kill switch (`vpn killswitch`, `vpn.killswitch`), PreUp/PostUp/PreDown/PostDown hooks and a config toggle
(`netman config toggle vpn.ipsec` ...). Secrets never go on a command line: root-only files or the client's stdin.

    Type = ipsec        strongSwan: writes swanctl.conf, drives `swanctl --load-all/--initiate/--terminate`. Auth = eap | psk | cert
    Type = l2tp         xl2tpd + pppd (+ a swanctl transport SA when PSK is set; a PSK profile without swanctl is refused, never plain L2TP)
    Type = openconnect  the `openconnect` binary, Protocol = anyconnect|gp|nc|pulse|f5|fortinet|array, password via --passwd-on-stdin, Routes = split tunnel
    Type = sstp         recognised, NOT supported (no maintained client); `up` says so

Common keys: `Server`, `Username`, `Password`, `Routes` (empty = full tunnel), `Cert`/`Key`/`CA`; see `netman vpn info NAME` for what a profile resolved to.
PPTP is deliberately not implemented: its MS-CHAPv2/MPPE encryption is broken (an import of a PPTP profile is refused with that reason).
charon-cmd is not driven (it prompts on a terminal); swanctl is.

`netman vpn import FILE [NAME]` takes a WireGuard wg-quick `.conf`, an OpenVPN `.ovpn`, a NetworkManager `.nmconnection` keyfile
(`wireguard`, and `vpn` with openvpn / strongswan / l2tp / openconnect), a strongSwan `swanctl.conf` or `ipsec.conf`, or a native `.vpn`. What could not be
carried over (passwords in NM's secret agent or ipsec.secrets, unknown options) is printed as `note:`.

`netman tor up|down|status|newnym|bridge add|clear|exit CC|off|hs add|list|del` (`tor.enabled = yes`, off by default; Tor tab in the TUI): manages a `tor`
process from a generated torrc in `/run/netman/tor` (SocksPort, TransPort, DNSPort, control socket + cookie auth, bridges obfs4/snowflake, ExitNodes/StrictNodes,
IsolateDestAddr). `tor.mode = transparent` loads nft table `inet netman_tor` BEFORE starting tor: TCP of the routed uids goes to the TransPort, DNS to the DNSPort,
everything else (UDP/QUIC, ICMP, IPv6) is dropped, and it stays so if tor dies (fail closed) until `tor down`. `mode = socks` only prints the SOCKS address.
Threat model, honestly: Tor hides your network location from the sites you visit and the sites from your ISP. It does not anonymise you if you log in to accounts,
reuse identities, open downloaded files, or use apps that put your identity in the payload; transparent routing does not stop browser fingerprinting (use Tor Browser)
and a global observer of both ends can still correlate. Local services on 127.0.0.1 other than Tor's ports are blocked while it is up.

## mods (all off by default; each is one `.c` with all the logic, core only wires config, CLI rows, help and a TUI tab)

| mod | switch | CLI | what it does | files |
|---|---|---|---|---|
| ssh | `ssh.enabled` | `netman ssh ...` | profiles (host, user, key, jump hosts, `-L/-R/-D`, keepalive, ControlMaster, `ssh -w`), supervised background connection with reconnect, host keys only through `hostkey`/`trust` (host key checking is never turned off) | `ssh.h/.c`, `tests/ssh_test.c` |
| mesh | `mesh.enabled` | `netman mesh ...` | batman-adv (`ip` + optional `batctl`) and a WireGuard full mesh generated from one peer list (reuses `wireguard.c`) | `mesh.h/.c`, `tests/mesh_test.c` |
| bonding | `bonding.enabled` | `netman bond ...` | link bonding, the same code and `.ent` profiles as the enterprise bond (one implementation) | `bonding.h/.c`, `tests/bonding_test.c` |
| tunnel | `tunnel.enabled` | `netman tunnel ...` | GRE, gretap, ipip, sit, VXLAN, Geneve, L2TPv3 with keys and automatic MTU | `tunnel.h/.c`, `tests/tunnel_test.c` |

Each has a tab in the TUI (only when enabled, read-only: the CLI changes things). Details and what is untested: `docs/ssh-mesh-bond-tunnel.md`.

## tests (`tests/`)

    cc -std=gnu11 -Wall -Wextra -O2 -Isrc -Isrc/mods -DNETMAN_NO_MAIN -o /tmp/nm_logic tests/logic_test.c src/config.c src/mods/enterprise/enterprise.c src/mods/enterprise/tether.c src/mods/enterprise/licence.c src/mods/wireguard.c && /tmp/nm_logic   76 checks (parsers, width, render, modals)
    tests/pty                                    the real binary in tmux against fake iwd / wpa_supplicant / bluez (tests/fakes.c): 105 checks
    tests/cli_test                               help, man sync, config, module gating, exit codes (57)
    cc -std=gnu11 -Isrc -o /tmp/nm_priv tests/priv_test.c; cc ... tests/config_test.c src/config.c [src/netman.conf.example]   15 / 77
    tests/vpnmod_test.c (VPN module goldens, fake swanctl/openconnect, import samples in tests/vpn_samples, xl2tpd parses the config), tests/tor_test.c (torrc/nft goldens, fake control server, nft rules loaded in `unshare -Urn`: redirect, leak dropped, fail closed), tests/pty_tor (Tor tab + VPN types, 80x24 and 40x12), tests/pty_ent (Enterprise tab, 80x24 and 40x12), tests/pty_vpn (real WireGuard in unshare -Urn, drives the VPN tab), tests/wifi_bt_test.c (75), net_test (+ `selftest` under unshare -Urn), vpn_test (+ live WireGuard peer edits under ns), fw_test, net_score_test (unshare -Urn)

    tests/settings_test.c (schema consistency: every config key has an entry; 616 checks), tests/settings_cli_test (settings goldens, JSON validity, atomic writes, pick through a fake menu), tests/pty_settings (the S panel in tmux under `unshare -Urn`: keys, mouse, 80x24 and 40x12)

    tests/ssh_test.c, mesh_test.c, bonding_test.c, tunnel_test.c (fake ssh / ssh-keyscan / ip / batctl, real netlink in `unshare -Urn`), tests/pty_mods (the four tabs in the real TUI)

Not tested: real tor (not installed here: bootstrap, circuits, bridges, hidden services), a live strongSwan / L2TP / OpenConnect connection (binaries absent or no server: config and argv only), real iwd / wpa_supplicant / bluez and real hardware, real networks, a real sudo prompt, vpn up/down as root from the TUI, the DHCP lease-cache
key used by the details pane (a guess: the SSID), the future daemon.
