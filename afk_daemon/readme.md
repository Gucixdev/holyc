afk daemon wykrywa afk i wykonuje skrypty 
albo najlepiej bez daemona albo by ten daemon nie zajmowal nic XD 
---

# afkd

Tiny C daemon, zero polling: sleeps in `poll()` on the Wayland fd; the compositor
wakes it only at your timeouts (`ext-idle-notify-v1`, which already honours
idle-inhibitors: video players, fullscreen apps that inhibit, etc.).

    make && make test
    ./afk_run            # idempotent start from compositor autostart (flock), any init
    build/afkd --check   # validate ~/.config/afkd.conf
    build/afkd --once healthcheck

Config `~/.config/afkd.conf` (scripts looked up in `../scripts` next to the binary, or `$AFKD_SCRIPTS`):

    idle   300  screensleep-nolock
    idle   600  screensleep-lock
    idle   3600 sys_upd_upg
    resume 0    wake      # on any activity
    resume 600  healthcheck  # only if the idle stage reached was >= 600s

Scripts are fork+exec'd (never blocks, children auto-reaped), one instance per
script via flock in `$XDG_RUNTIME_DIR`. Non-executable scripts run via `/bin/sh`.

## Status (honest)
- Tested only against `tests/fakecomp` (libwayland-server fake with idled/resumed),
  with fake scripts. NOT run on a real compositor, no real lock/suspend/update ever run.
- Cost idle: 0 CPU ticks in 0.5s, RssAnon ~160 kB, total RSS ~2.4 MB (shared libc/libwayland pages; the
  "<1 MB" goal is only true for private memory).
- No X11 fallback (not implemented); no ext_idle_notifier => exits with a message.
- `resume` hooks are not run for idle stages that were never reached.
- `screensleep-lock` / `screensleep-nolock` are still empty stubs (yours).
- `sys_upd_upg` default is `paru -Syu --noconfirm` (override `$AFKD_UPD`); skipped on battery.
