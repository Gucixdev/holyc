# netman

A small replacement for NetworkManager + bluez tooling, in plain C: no Python, no GLib, no libdbus. Backends are iwd (or
wpa_supplicant) for Wi-Fi and bluez for Bluetooth. Short alias in prose: `netmgr`. The library is the stb-style header
`netmanager_blue.h` (still in `../notcoreutils`; the `nmb_` -> `nm_` rename and the move into this folder come later), with
`nmb_net.h` (IP / DHCP / DNS / roaming) and `nmb_vpn.h` (WireGuard / OpenVPN / providers) next to it. This folder holds the
command line (`netman.c`), the privilege logic (`priv.h`) and the terminal UI (`tui.h`).

## use

    ./run                       build (gcc -Wall -Wextra -O2) and open the TUI
    bin/netman                  status of links, wifi and bluetooth
    bin/netman -tui             the TUI (`--tui` and `-t` also work)
    bin/netman help             plain-text usage
    bin/netman wifi list | connect SSID [PASS] | disconnect | forget SSID | known [autoconnect SSID on|off]
    bin/netman wifi wps pbc|pin [PIN]|cancel     wifi hotspot start SSID [PASS]|stop
    bin/netman bt list|scan|connect|disconnect|pair|trust|remove NAME|power on|off|agent     (pair accepts --pin PIN)
    bin/netman fw               nft ruleset

## privileges

netman asks for the password itself, before the screen is drawn, and only when the backend needs it: it re-runs
`sudo -p "[netman] password for %p: " -- netman --priv-done ARGS` (the password never touches argv; `--priv-done` stops loops).
`NM_SUDO=never|always|auto` (default auto) and `NM_SUDO_CMD=/path/to/sudo` override it.

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

Spinner while a call runs (esc / ctrl-c cancels it), modal dialogs for prompts, confirmations, WPS and busy states, a scrollbar on
long lists, and a signal sparkline for the connected network. `?` opens a scrollable help popup (keys per tab, privileges,
backends); esc, q or ? closes it.

    tab / shift-tab, h l, 1-4   switch tab (Wi-Fi | Ethernet/IP | Bluetooth | VPN)
    j k, arrows, g G, pgup/dn   move            /  filter (esc clears)      r  refresh      ?  help      q  quit
    Wi-Fi       enter connect (masked passphrase prompt when needed)   d disconnect   x forget (asks)   a autoconnect on/off
                w WPS (p button, i pin, c cancel)   H hotspot (empty SSID stops it)
    Bluetooth   enter connect/disconnect   p pair (asks a PIN, enter = 0000)   t trust   x remove (asks)   s scan 5 s   o adapter power

Layout follows the terminal (SIGWINCH). Colours live in one `nm_theme` struct with ASCII fallbacks when the locale is not UTF-8.

## tests (`tests/`)

    cc -std=gnu11 -Wall -Wextra -O2 -I../notcoreutils -I. -o /tmp/nm_logic tests/logic_test.c && /tmp/nm_logic    111 checks
    cc -std=gnu11 -Wall -Wextra -I. -o /tmp/nm_priv tests/priv_test.c && /tmp/nm_priv                             15 checks (sudo decision + re-exec, recording fake sudo)
    tests/pty                                                                      the real binary in tmux against a fake iwd + bluez on a private dbus (50 checks)

Not tested: real iwd / wpa_supplicant / bluez, real hardware, a real sudo prompt, esc-cancel of a running call, the Ethernet and VPN tabs
(placeholders until the libraries are wired in).
