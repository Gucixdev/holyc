# netman QEMU autotest

`./run` (no arguments) boots a throw-away VM and runs netman's risky, root-needing tests inside it, so nothing touches the live machine.
Exit 0 = every stage that ran passed (SKIPs do not fail); exit 1 = a FAIL/TIMEOUT or the VM never reached the summary; exit 2 = host prerequisite missing.
Takes about 45 s. Output is `QTEST <name> PASS|FAIL|SKIP <detail>` lines plus `QTEST summary done`; on failure the temp dir (serial.log, `out/<stage>.log`) is kept and its path printed.

## How it works
- `run` (host): compiles the netman tests (static when possible) from `netman/tests/*.c`, builds an initramfs (host bash/ip/nft/wpa_supplicant/dbus-daemon/... plus `ldd`-copied libs, no busybox needed) and the needed kernel modules from `/usr/lib/modules/$(uname -r)` (dependency order from `modprobe --show-depends`, `.zst` decompressed on the host), then boots the **host kernel** with `-enable-kvm -nographic -m 1G -smp 2`, 120 s hard timeout (`QT_TIMEOUT`), `nice -n 19`.
- `init` (guest PID 1): mounts, loads modules, mounts the repo, runs `stages/*` in order as uid 0, prints the summary, powers off via sysrq.
- Isolation: NIC is `-nic user,restrict=on` (no `hostfwd`, guest cannot reach host or internet; stage 3 proves it with TCP probes); the repo is a **read-only** 9p share (`/repo`, stage 1 proves a write fails); the only writable share is a scratch dir in the temp dir (`/out`). The host is never changed: no sysctl, modprobe, network config or install.
- `mkroot.sh` (generic helpers `qt_add_bin/qt_add_mod/qt_add_file/qt_add_libs`), `guestlib.sh` (`pass/fail/skip/need/loadmod/run_logged`) are project independent.

## Stages (each independent; missing binary/module = `SKIP <reason>`)
1. `10-boot`: uid 0, 9p repo mounted and read-only, scratch writable, modules dummy/veth/wireguard/nf_tables/mac80211_hwsim/cfg80211/virtio_net load, dummy+veth links can be created.
2. `20-netman-tests`: `net_test selftest`, `priv_test`, `vpn_test` (pure + `ns` live with a real WireGuard interface), `fw_test` (pure incl. `nft -c` vetting + `ns` live: real nft apply/diff/rollback/kill switch). The live parts of these tests insist on running inside `unshare -Urn` (they refuse the initial namespace by design), so they run in a user+net namespace *inside* the VM kernel, as root of the VM. Needs `nft`, `ip`, `unshare`, `nsenter`.
3. `30-dhcp`: `netman net dhcp` on the virtio NIC against slirp's DHCP: asserts 10.0.2.x address, server 10.0.2.2, lease time, renew (second run), isolation. Gateway and DNS 10.0.2.3 assertions are in place but SKIP: with `restrict=on` slirp sends **no** router/DNS options (verified in a `filter-dump` pcap), and loosening `restrict` would break the no-host-network rule.
4. `40-wifi`: two `mac80211_hwsim` radios. AP = hostapd if installed, otherwise netman's own `wifi hotspot start` (wpa_supplicant AP mode). Station = wpa_supplicant driven by `netman wifi scan` / `netman wifi connect` (WPA2-PSK, verified with `iw link`). Needs CCMP crypto modules (ccm, gcm, cmac, ... loaded automatically). iwd is not exercised (see status).
5. `50-qlogind`: VM-local `dbus-daemon` bus + `qlogind` + `busctl ListSeats` + qlogin's `login1_test` client. SKIPs when the qlogin sources are not next to netman in the repo.

## Requirements (host)
`qemu-system-x86_64` (with KVM, virtio-9p), `/dev/kvm` rw, `cpio`, `zstd`, `gcc` (+ static glibc preferred), `kmod` (`modprobe`), a readable kernel (`/usr/lib/modules/$(uname -r)/vmlinuz`; `/boot` is tried too), plus the binaries copied into the VM: `bash ip nft unshare nsenter wpa_supplicant iw dbus-daemon busctl` (optional: `hostapd`, `iwd`). Nothing is installed by the script; if something is missing it says so (`! sudo pacman -S ...`).

## Reuse from qlogin (or any project)
Call `netman/tests/qemu/run` with env: `QT_STAGES_DIR=/path/to/extra/stages` (scripts that `. /qt/lib.sh`, see `stages/50-qlogind`), `QT_PREP=/path/prep.sh` (sourced on the host after the rootfs is built; may call `qt_add_bin`, `qt_add_mod`, `qt_add_file`, put build outputs in `$ROOT/t/`), `QT_ONLY='regex'` (run only matching stage file names), `QT_TIMEOUT`, `QT_KEEP=1`. Note the repo share is the git root of this repo (`/repo` in the VM), and the netman stages always run unless filtered with `QT_ONLY`. qlogin already has stage 5; a qlogin-only run is `QT_ONLY='10-|50-' netman/tests/qemu/run`.

## Honest status (last run, CachyOS, kernel 7.3.1, this host)
34 PASS, 0 FAIL, 2 SKIP (dhcp.gateway, dhcp.dns: slirp restrict limitation above). Stage 4 ran via the wpa_supplicant-AP fallback because `hostapd` is not installed (`! sudo pacman -S hostapd` to use it); `iwd` is installed but its stage (iwd AP/station through D-Bus) is not written. Not covered: iptables-legacy, OpenVPN live (no `openvpn`/tun in the VM), real hardware radios. Some "ok-lines" counts print 0 because those tests only print failures.
