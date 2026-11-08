# ssh, mesh, bonding, tunnel

Four optional mods. Each is a plain `.c` (`src/mods/{ssh,mesh,bonding,tunnel}.c`) with all of its logic; the core only holds the config keys,
the command-table rows, the help text and a read-only TUI tab. All are **off by default** (`netman config toggle ssh.enabled` etc.). Shared
helpers: `src/mods/modkit.h` (profile files, argv-only process runner, name/value validation). Profiles are `key = value` files; names are
`[A-Za-z0-9_.-]`, no leading dot or dash. Nothing runs through a shell.

Build: append `ssh.c mesh.c bonding.c tunnel.c` to the netman cc line (already done in `run`, the tests and the qemu harness).

## ssh (`netman ssh ...`)

Profile `NAME.ssh` in `ssh.dir`: `host user port key jump forward keepalive keepalive_count connect_timeout control reconnect reconnect_delay tun tun_addr strict known_hosts auto`.
- `forward = L:8080:db:5432`, `R:9000:localhost:22`, `D:1080` (repeat, max 8). `L` and `D` bind to 127.0.0.1 only.
- `strict` accepts `yes` or `accept-new` and nothing else. A profile asking for `no` is refused; netman never turns host key checking off.
  `UserKnownHostsFile` is `ssh.known_hosts`. `netman ssh hostkey NAME` shows the fingerprint; `netman ssh trust NAME SHA256:FP` stores the key only when it matches.
- Values are validated (no leading `-`, no spaces, quotes or shell characters) so a profile cannot inject ssh options.
- `up` starts a detached supervisor (state, pid and log in `ssh.state_dir`) that runs `ssh -N` with `ExitOnForwardFailure`, restarts it with backoff,
  and for `tun = L:R` assigns `tun_addr` once the tunN device exists. `down` sends SIGTERM (SIGKILL after 8 s).
- `check`/`exec` use the ControlMaster socket. `argv NAME` prints the exact command line.

## mesh (`netman mesh ...`)

- `kind = batman`: `ip link add NAME type batadv`, ports joined by `batctl meshif IF interface add X` (or `ip link set X master IF` without batctl), then optional `orig_interval`, `hop_penalty`, `gw_mode` via batctl, `mtu`, `addr`.
- `kind = wg`: one peer list `NAME.peers` (`name pubkey endpoint|- address` per line) is the single source of truth. `mesh gen NAME [node]` prints the wg-quick config of any node
  (every other node a peer, AllowedIPs = that node's mesh address only); `mesh up` programs this node through `wireguard.c` (netlink, no `wg`); `mesh init` makes the private key (mode 0600, never overwritten).
- It is a full mesh of direct links (n*(n-1)/2). No routing protocol runs over it; nodes with endpoint `-` must connect to you first.

## bonding (`netman bond ...`)

Re-exports the enterprise bond code (`src/mods/enterprise`), same `.ent` profiles, so `ent save NAME bond ...` and `bond add` are the same thing. Modes and their switch requirements: `netman bond modes`.

## tunnel (`netman tunnel ...`)

Profile `NAME.tun`: kinds `gre gretap ipip sit vxlan geneve l2tp` (gre/gretap become ip6gre/ip6gretap for IPv6 endpoints), keys `local remote link key ttl mtu addr auto vni port group`, l2tp ids/encap/ports.
Devices are created through rtnetlink (L2TPv3 through `ip l2tp`). `mtu = auto` is the underlay MTU minus the encapsulation overhead (`tunnel mtu NAME` shows the numbers).

## Not verified (honest list)

- ssh: only against fake `ssh`/`ssh-keyscan`/`ssh-keygen` scripts. No real server, no real jump host, no real `-L/-R/-D` traffic, no real `ssh -w` session (the tun address step is tested against a tun device made with `ip tuntap` in a private netns).
- mesh/batman: the stand-in device is a `dummy` created by a fake `ip`; batman-adv itself (kernel module, batctl output, real multi-hop routing) has not run. mesh/wg: device, peers, address and link state are checked in a private netns; no handshake between real nodes, no traffic, NAT traversal not tried.
- bonding: shared with the enterprise code; modes needing switch support (802.3ad) not tried against a real switch.
- tunnel: devices are created and read back in a private netns (kinds the kernel refuses there are skipped); no traffic through them, MTU numbers are textbook overheads, not measured on a path. L2TPv3 depends on the `l2tp_*` modules.
- TUI tabs are checked in tests/pty_mods with profile files only (no live connections).
