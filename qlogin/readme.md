login zgodny z elogind ale terminalowy bez zbednego GUI to moze sie uruchomic pod spodem terminala juz XD czy cos nw q od quick bo ma dzialac ultra szybko 

---

# qlogin / qlogind

Two small C programs (gnu11, no GLib, no libdbus, no systemd libs) sharing one hand-written D-Bus library.

- `qlogind` - an org.freedesktop.login1 service for the subset real consumers use (exact list and per-row test status: `docs/login1-surface.md`).
- `qlogin` - a terminal login greeter: PAM, session picker, CreateSession, utmp/wtmp/lastlog, lockout. `qlogin -t` shows the time to the first prompt.

Design: `docs/design.md`. Man pages: `src/qlogin.1`, `src/qlogind.8`. Run everything: `./run` (no arguments).

## Tested (automatically, `./run`, all green)

- `tests/run_qdbus`: marshalling, SCM_RIGHTS fd passing, Hello on a private bus (ASAN/UBSAN).
- `tests/run_login1`: qlogind vs a fake client on a private `dbus-daemon`, 93 checks, plus a cross-check with real sd-bus (`busctl`: introspection, properties, ListSeats). Sessions, state files, GetSessionByPID, inhibitors (delay/block), power actions in dry-run, TakeControl/TakeDevice with a real passed fd, Pause/ResumeDevice, controller leaving the bus, session end by closing the fifo.
- `tests/run_qlogin`: qlogin inside a private tmux with a fake PAM stack (pam_exec check, `PAM_CONFIG`): prompt, `-t`, wrong passwords with growing lockout, no echo, Ctrl-C, Ctrl-D, session picker, the session really running with XDG_SESSION_ID/XDG_RUNTIME_DIR from qlogind, wtmp/lastlog written, session released afterwards.

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
