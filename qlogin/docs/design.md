# qlogin / qlogind design

Goal (from the author): a login that is compatible with elogind but terminal-based, no needless GUI, can start
directly under the terminal. q = quick: it must start ultra fast. Drop-in for the org.freedesktop.login1 API as far
as docs/login1-surface.md says, no further.

## Pieces

- `src/mods/qdbus.h` - D-Bus wire library (client+server, SCM_RIGHTS fds). No libdbus/GLib/systemd. Copied in spirit from
  netman's dbus.h but rewritten: heap buffers (no static message store), fd passing, server-side reply/error/signal.
- `src/mods/login1.h` - the model (sessions/seats/users/inhibitors/devices) and the object tables. ONE table per
  interface drives dispatch, property access and generated introspection XML.
- `src/qlogind.c` - thin main: config, bus connect, RequestName, poll loop.
- `src/qlogin.c` - thin main: the terminal greeter (phase 5).
- `tests/` - wire test, daemon-vs-fake-client tests on a private dbus-daemon.

## Safety rules (the machine runs real systemd-logind)

Never owns `org.freedesktop.login1` on the real system bus: the daemon only connects to the address in
`DBUS_SYSTEM_BUS_ADDRESS` (tests: private `dbus-daemon --session`). State goes to `$QLOGIND_RUN` (default `/run`).
Power actions are dry-run unless `real_power=1` in the config. VT switching and device opening go through a backend
that the tests replace by files (`QLOGIND_DEVDIR`, `QLOGIND_VTFILE`). Nothing writes to /etc or /usr.

## Config (qlogind.conf, `QLOGIND_CONF`, default /etc/qlogind.conf, KEY=VALUE)

`real_power=0|1`, `cmd_poweroff=...` (and reboot/halt/suspend/hibernate), `inhibit_delay_max=5`, `seat0_vts=1`.

## Speed budget

qlogin: one static-ish binary, no allocation before the prompt, no D-Bus until after authentication, no config parse
other than the session list; the time to first prompt is measured by `qlogin -t` (prints microseconds to stderr).
