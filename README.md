# desktop

ONE executable, `./desktop`, built from the C in `src/`: the compositor (dwl, static wlroots 0.20, XWayland), the menu (dmenu + `menu`),
the status bar (`bar`, C, no forks in the hot path: /proc + NVML via dlopen) and lemonbar. Multi-call like busybox: `desktop dwl`, or a
symlink named `dwl`/`dmenu`/`lemonbar`/`bar`/`menu`/`stest` (`make` puts them in `bin/`). `make` builds (needs `holyc/dwl/.wlroots`, see
`holyc/dwl/wlroots-static`), `make install` installs the binary and the applet links.

How one binary holds programs that were separate: each is compiled with `-Dmain=<name>_main`, its objects are joined with `ld -r`, and
`objcopy --keep-global-symbol=<name>_main` makes every other symbol local, so the copies of drw.c, util.c and the generated protocol code
never clash and no program had to change. Config is compiled in, dwm style: `src/dwl/config.h`, `src/dmenu/config.h`.

Not in the binary yet: wvkbd (on-screen keyboard) and wtype (cloned third-party C, built by `./run` into `bin/`), the `shot` and `clip`
scripts (grim/slurp/cliphist wrappers), foot (the terminal) and frost (the editor).

| piece | where | note |
|---|---|---|
| dwl, static wlroots 0.20 | holyc/dwl | tablet support added 2026-11-07; config.h is local, not tracked |
| wvkbd (on-screen keyboard) | built by `./run` | Alt+b toggles (SIGRTMIN) |
| wtype | built by `./run` | |
| shot, clip | bin/ | grim+slurp, cliphist+dmenu |

Tested headless only: dwl starts, advertises tablet-v2, virtual keyboard and pointer, screencopy; wvkbd stays up and toggles,
wtype exits 0. Not tested: a real pen, a real Deck, the keyboard on a touch screen.
No brightnessctl installed, so no brightness keys yet. XWayland still off.

## Session (log in to dwl)

`session` starts dwl with the bar and the hidden keyboard (Alt+b toggles). `dwl.desktop` is the display-manager entry,
installed with `sudo cp dwl.desktop /usr/share/wayland-sessions/` (done on this machine 2026-11-07); pick "dwl" in SDDM at login.
From a TTY: `/data/dev/desktop/session`. dwl's stderr goes to `~/.cache/dwl.log`. Not yet run on the real screen, only headless.
