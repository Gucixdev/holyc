# netmanager

A small replacement for NetworkManager + bluez tooling, in plain C: no Python, no GLib, no libdbus. The library is the stb-style
header `netmanager_blue.h` (still in `../notcoreutils` for now; the `nmb_` -> `nm_` rename and the move into this folder come later),
with `nmb_net.h` (IP / DHCP / DNS / roaming) and `nmb_vpn.h` (WireGuard / OpenVPN / providers) next to it. This folder holds the
command line and the terminal UI.

## use

    ./run                       build (gcc -Wall -Wextra -O2) and open the TUI
    bin/netmanager              status of links, wifi and bluetooth
    bin/netmanager -tui         the TUI (`--tui` and `-t` also work)
    bin/netmanager wifi list | connect SSID [PASS] | disconnect | forget SSID | known [autoconnect SSID on|off]
    bin/netmanager wifi wps pbc|pin [PIN]|cancel     wifi hotspot start SSID [PASS]|stop
    bin/netmanager bt list|scan|connect|disconnect|pair|trust|remove NAME|power on|off|agent     (pair accepts --pin PIN)
    bin/netmanager fw           nft ruleset

`tui.h` is one stb-style header (`#define NM_TUI_IMPLEMENTATION`, entry `int nm_tui(void)`); it talks to the library only through its
public API and parses what the library prints, so nothing changes on the library side.

## TUI keys

    tab / shift-tab, h l, 1-4   switch tab (Wi-Fi | Ethernet/IP | Bluetooth | VPN)
    j k, arrows, g G, pgup/dn   move            /  filter (esc clears)      r  refresh      ?  help      q  quit
    Wi-Fi       enter connect (masked passphrase prompt when needed)   d disconnect   x forget (asks)   a autoconnect on/off
                w WPS (p button, i pin, c cancel)   H hotspot (empty SSID stops it)
    Bluetooth   enter connect/disconnect   p pair (asks a PIN, enter = 0000)   t trust   x remove (asks)   s scan 5 s   o adapter power

Layout follows the terminal (SIGWINCH): tab names shorten under 60 columns, the summary and right-hand columns shed fields, the key
bar always keeps `? help  q quit`. Colours live in one `nm_theme` struct; the signal bars fall back to ASCII when the locale is not UTF-8.

## tests (`tests/`)

    cc -std=gnu11 -Wall -Wextra -O2 -I.. -I../../notcoreutils -o /tmp/nm_logic tests/logic_test.c && /tmp/nm_logic   parsers, renderer at 80x24 and 40x12, keys (86 checks)
    tests/pty                                                                                                       the real binary in a tmux pane against tests/fakes.c (50 checks)

`tests/fakes.c` is a stateful fake iwd + bluez on a private `dbus-daemon`; nothing real is connected, paired or forgotten. `tests/pty` drives
connect / disconnect / forget / autoconnect / filter / help / pairing / power / tab switch / resize / clean exit and checks the rendered frame
and the fake's call log.

## status (honest)

- Done and tested against the fakes: Wi-Fi and Bluetooth tabs, every key above, rendering at 80x24 and 40x12, terminal restore on quit.
- **Not tested on real hardware**: real iwd, wpa_supplicant, bluez, routers (WPS), phones and headsets. The fakes follow my reading of those protocols.
- Ethernet/IP tab only lists links (from `status`); the profile editor (DHCP / static / DNS) waits for `nmb_net`. The VPN tab is a placeholder until `nmb_vpn` is wired in.
- Library calls block: a Wi-Fi scan under wpa_supplicant takes ~4 s, WPS waits up to 30 s (`NMB_WPS_SECS` overrides); the UI does not redraw or react meanwhile.
- A forget under wpa_supplicant is refused by the library (iwd only); the TUI shows that message.
- The pairing agent never reads the terminal inside the TUI (`nmb_in` is /dev/null): the PIN you type at `p` answers every prompt and confirmations are accepted.
