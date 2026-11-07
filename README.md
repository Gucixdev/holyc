# desktop

dwl + dmenu + lemonbar (in /data/dev/setup/holyc) with the small pieces around them.

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
