# desktop

ONE executable, `./desktop`, built from the C in `src/`: the compositor (dwl, static wlroots 0.20, XWayland), the menu (dmenu + `menu`),
the status bar (`bar`, C, no forks in the hot path: /proc + NVML via dlopen) and lemonbar. Multi-call like busybox: `desktop dwl`, or a
symlink named `dwl`/`dmenu`/`lemonbar`/`bar`/`menu`/`stest` (`make` puts them in `bin/`). `make` builds (needs `holyc/dwl/.wlroots`, see
`holyc/dwl/wlroots-static`), `make install` installs the binary and the applet links.

How one binary holds programs that were separate: each program is a group of objects joined with `ld -r`, and
`objcopy --keep-global-symbol=<name>_main` makes every other symbol of the group local, so programs never clash over globals. Shared
code exists once: `src/util.c`, and `src/drw.c` plus the client protocol glue (`src/protocols/client/`) inside the dmenu/menu/lemonbar group.
Config is compiled in, dwm style: `src/config.c`, `src/theme.c`, `src/keys.c` (bindings) for the compositor, the top of `src/menu.c` for dmenu.

Layout of `src/` (everything is our own code, forked from dwl, dmenu and lemonbar; licences in `src/LICENSES/`):

| file | what |
|---|---|
| `dwl.h`, `client.h` | shared types, prototypes, globals of the compositor |
| `compositor.c` | display, outputs, layer shell, lock, popups, setup/run/cleanup, `dwl_main` |
| `client.c` | windows: map/unmap, focus, tags/views, layouts tile/float/monocle/deck/grid, XWayland |
| `keys.c` | keyboards, key and mouse binding tables, pointer, cursor, tablets, drag, spawn/quit/chvt |
| `config.c`, `theme.c` | rules, layouts, monitor/keyboard/trackpad settings; colours, borders, focus behaviour |
| `settings.c`, `ime.c` | control fifo/socket and session restore; input-method relay |
| `api.c` | command table, settings registry, event stream (`subscribe`), hooks, runtime keys/rules, `--dump-api` |
| `panel.c` | `desktop settings`: layer-shell settings panel (a Wayland client) |
| `fx.c` | border colour and the optional spring-damper window animation (`enable_animation`) |
| `menu.c`, `stest.c` | dmenu picker + `menu` launcher; stest |
| `lemonbar.c`, `bar.c` | the panel, and the status lines for it |
| `drw.c/h`, `util.c/h`, `arg.h` | shared drawing, helpers |
| `protocols/{server,client}/` | wayland-scanner output (server headers for dwl, client glue for dmenu/lemonbar) |
| `resources/{wlroots,wtype,wvkbd}` | vendored third-party: wlroots is built in, wtype and wvkbd stay separate programs |

`make BUILD=build-new OUT=build-new/desktop LINKDIR=build-new/bin` builds off to the side without touching `./desktop` or `bin/`.

Not in the binary yet: wvkbd (on-screen keyboard) and wtype (cloned third-party C, built by `./run` into `bin/`), the `shot` and `clip`
scripts (grim/slurp/cliphist wrappers), foot (the terminal) and Emacs.

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

`session` starts dwl with the bar, the Emacs daemon and the hidden keyboard (Alt+b toggles). `dwl.desktop` is the display-manager entry,
installed with `sudo cp dwl.desktop /usr/share/wayland-sessions/` (done on this machine 2026-11-07); pick "dwl" in SDDM at login.
From a TTY: `/data/dev/desktop/session`. dwl's stderr goes to `~/.cache/dwl.log`. Not yet run on the real screen, only headless.

More: `docs/hacking.md` (socket protocol, events, hooks, runtime keys/rules, bar modules, settings), `docs/protocols.md` (wayland protocol audit), `tests/headless` and `tests/protocols` (headless checks of the new build).
