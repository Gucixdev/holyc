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
    src/mods/           stb-style libraries: wifi.h btth.h dbus.h net.h roaming.h scoring.h metrics.h vpn.h firewall.h wireguard.h (+ wireguard.c, enterprise.c, tether.c, licence.c: plain .c, not stb) (+ empty mesh/ssh/onion_route)
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

## tests (`tests/`)

    cc -std=gnu11 -Wall -Wextra -O2 -Isrc -Isrc/mods -DNETMAN_NO_MAIN -o /tmp/nm_logic tests/logic_test.c src/config.c src/mods/enterprise.c src/mods/tether.c src/mods/licence.c src/mods/wireguard.c && /tmp/nm_logic   76 checks (parsers, width, render, modals)
    tests/pty                                    the real binary in tmux against fake iwd / wpa_supplicant / bluez (tests/fakes.c): 105 checks
    tests/cli_test                               help, man sync, config, module gating, exit codes (41)
    cc -std=gnu11 -Isrc -o /tmp/nm_priv tests/priv_test.c; cc ... tests/config_test.c src/config.c [src/netman.conf.example]   15 / 77
    tests/pty_ent (Enterprise tab, 80x24 and 40x12), tests/pty_vpn (real WireGuard in unshare -Urn, drives the VPN tab), tests/wifi_bt_test.c (75), net_test (+ `selftest` under unshare -Urn), vpn_test (+ live WireGuard peer edits under ns), fw_test, net_score_test (unshare -Urn)

Not tested: real iwd / wpa_supplicant / bluez and real hardware, real networks, a real sudo prompt, vpn up/down as root from the TUI, the DHCP lease-cache
key used by the details pane (a guess: the SSID), the future daemon.
