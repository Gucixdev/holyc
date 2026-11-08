login zgodny z elogind ale terminalowy bez zbednego GUI to moze sie uruchomic pod spodem terminala juz XD czy cos nw q od quick bo ma dzialac ultra szybko 

---

# qlogin / qlogind

Two small C programs (gnu11, no GLib, no libdbus, no systemd libs) sharing one hand-written D-Bus library.

- `qlogind` - an org.freedesktop.login1 service for the subset real consumers use (exact list and per-row test status: `docs/login1-surface.md`).
- `qlogin` - a terminal login greeter: PAM, session picker, CreateSession, utmp/wtmp/lastlog, lockout. `qlogin -t` shows the time to the first prompt.

Design: `docs/design.md`. Man pages: `src/qlogin.1`, `src/qlogind.8`. Run everything: `./run` (no arguments).

## Tested (automatically, `./run`, all green)

- `tests/run_qdbus`: marshalling, SCM_RIGHTS fd passing, Hello on a private bus (ASAN/UBSAN).
- `tests/run_login1`: qlogind vs a fake client on a private `dbus-daemon`, 139 checks, plus a cross-check with real sd-bus (`busctl`: introspection, properties, ListSeats). Every row of docs/login1-surface.md except SeatNew/SeatRemoved: sessions, state files, GetSessionByPID/GetUserByPID, all Can* and power actions in dry-run, Lock/Unlock signals, Activate, KillSession (on a forked child), all Manager/Session properties, inhibitors (delay/block), TakeControl/TakeDevice with a real passed fd, Pause/ResumeDevice, controller leaving the bus, session end by closing the fifo.
- `tests/run_qlogin`: qlogin inside a private tmux with a fake PAM stack (pam_exec check, `PAM_CONFIG`): prompt, `-t`, wrong passwords with growing lockout, no echo, Ctrl-C, Ctrl-D, session picker, the session really running with XDG_SESSION_ID/XDG_RUNTIME_DIR from qlogind, wtmp/lastlog written, session released afterwards.

## Speed (`tests/run_bench`, -O2, this laptop, private session bus used as the fake system bus)

- qlogind: 10000 GetSession calls, one client: 291 ms total, mean 29 us, p50 28 us, p99 64 us, about 34000 calls/s. This is a full round trip through dbus-daemon (client, daemon, qlogind, daemon, client), so most of it is the bus.
- qlogin `-t`: 44-50 us from `main()` to the first prompt (5 runs). Excludes exec and dynamic linking (libpam, libc); the binary uses plain escape sequences, no ncurses, no locale init.

## Trying it for real without installing anything

Examples only, in `contrib/` (PAM stack, D-Bus policy, dinit/runit/systemd units, `NO-POLKIT.txt`); nothing is installed by this project.

- Do not do this on a machine that runs systemd-logind or elogind: both want the bus name org.freedesktop.login1.
- Safest: a VM (or a systemd-less container) with a user and a root console. Copy the two binaries, `contrib/pam.d/qlogin` (to /etc/pam.d/), and the D-Bus policy there, start a system bus, then `DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket qlogind &`.
- Then on a spare VT in that VM, as root: `openvt -c 3 -s -- /usr/bin/qlogin` (or `qlogin </dev/tty3 >/dev/tty3`), log in, and check `busctl call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager ListSessions`.
- Without a VM, the test mode (`PAM_CONFIG=<dir> qlogin` against a private `dbus-daemon`, see `tests/run_qlogin`) is the only supported way.

## NOT tested / not implemented (honest list)

- Never run on a real system, never as the real login service, never attached to the real system bus. This machine runs systemd-logind; nothing here replaces or touches it. `qlogind` refuses to start without `DBUS_SYSTEM_BUS_ADDRESS`.
- The real (non-test) path of qlogin is untested: setuid/initgroups, tty chown, real PAM modules (pam_unix etc.), `/etc/pam.d/qlogin` (not shipped), real utmp.
- X11 sessions (launched via `startx`) are untested; Wayland sessions are only tested with a fake `Exec`.
- No cgroups (sessions are tracked by a pipe and the /proc parent chain), no DRM master revoke / EVIOCREVOKE, no real VT switching (logged as a dry run), no polkit (authorization is by uid), no ScheduleShutdown, SeatNew/SeatRemoved never emitted, KillSession "all" behaves like "leader".
- Power actions are a dry run unless `real_power=1` and a `cmd_*` is configured; the real execution path is untested.
- D-Bus error names are written from memory of sd-bus conventions.
- Compatibility with elogind is limited to docs/login1-surface.md. It is not a drop-in replacement yet.
- Startup speed: `-t` measures from `main()` only, it excludes exec and dynamic linking. The password is copied by libpam during authentication; it wipes its copy, we cannot prove it.
- Lean models (lean4/) not written.
