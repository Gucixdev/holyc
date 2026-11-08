# Hacking desktop

Everything below works on the compiled binary: no rebuild. Config lives in `~/.config/desktop/` (or `$XDG_CONFIG_HOME/desktop`,
or `$DESKTOP_CONFIG_DIR`). `desktop --dump-api` prints the same reference from the running binary (commands, events, hooks,
settings, files), so it never goes stale.

## 1. The line protocol

The compositor listens on `$XDG_RUNTIME_DIR/dwl.sock` (a unix socket; one request line, one reply) and reads commands from the
fifo `$XDG_RUNTIME_DIR/dwl.cmd` (no reply). Key bindings use the same command language.

    echo 'view 3' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/dwl.sock       # reply: ok
    echo 'list'   | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/dwl.sock       # one line per window
    echo 'set border_width 4' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/dwl.sock

Replies are `ok`, `error: message`, or text lines. Commands: focus, view, toggleview, tag, toggletag, layout, mfact, nmaster, kill,
float, fullscreen, zoom, focusmon, tagmon, spawn, quit, list, state, save, restore, get, set, settings, schema, reload, bind,
unbind, binds, emit, subscribe, events, commands. `desktop --dump-api` has the argument forms.

### subscribe: events as text lines

    printf 'subscribe\n' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/dwl.sock
    ok subscribed
    open 3 HDMI-A-1 1 foot foot
    focus 3 foot foot
    tag HDMI-A-1 2 3 0
    layout HDMI-A-1 [M] monocle

Event types: open, close, focus, title, move, float, urgent, tag, layout, output, set, reload, user. The first word is the type and
the rest are space separated; the last field (a title) may contain spaces. `tag OUTPUT VIEW OCCUPIED URGENT` carry bitmasks.
`emit NAME TEXT` sends your own `user NAME TEXT` event, which is how scripts talk to each other.

A tiny status bar feed:

    printf 'subscribe\n' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/dwl.sock |
      while read -r type rest; do [ "$type" = focus ] && echo "now: $rest"; done

## 2. Hooks

Executable files in `~/.config/desktop/hooks/`, started in the background (a slow hook never blocks the compositor):
`on-start on-window-open on-window-close on-focus on-title-change on-window-move on-float-change on-urgent on-tag-change
on-layout-change on-output on-setting-change on-reload on-user`.

Environment: `DESKTOP_EVENT DESKTOP_LINE DESKTOP_WINDOW DESKTOP_APPID DESKTOP_TITLE DESKTOP_OUTPUT DESKTOP_TAGS DESKTOP_OCCUPIED
DESKTOP_URGENT DESKTOP_LAYOUT DESKTOP_KEY DESKTOP_VALUE DESKTOP_SOCKET` (only the ones that make sense for the event are set).

    #!/bin/sh   # hooks/on-window-open: videos go to tag 4 and float
    case "$DESKTOP_APPID" in mpv) { echo 'tag 4'; } | socat - "UNIX-CONNECT:$DESKTOP_SOCKET"; echo float | socat - "UNIX-CONNECT:$DESKTOP_SOCKET" ;; esac

    #!/bin/sh   # hooks/on-urgent
    notify-send "needs you: $DESKTOP_APPID"

## 3. Key bindings and rules from text

`keys.conf` (checked before the compiled table in `config.c`, which stays the fallback):

    Super+Shift+Return = spawn foot
    Super+F6 = view 3
    Super+q = none               # swallow a key the compiled table binds

`rules.conf`, applied after the compiled rules: `app=firefox title=Private tags=3 float=1 monitor=0` (all keys optional, but one
of app/title is needed). `reload` re-reads `settings.schema`, `settings.conf`, `keys.conf` and `rules.conf` without a restart.
`bind CHORD = COMMAND` / `unbind CHORD` do the same for one key at runtime. Mouse buttons and scroll bindings are still compiled-only.

## 4. Settings

`desktop settings` opens the panel (Tab / Shift-Tab, arrows, wheel, Space or Return to act, Esc closes). It reads the schema
from the compositor (`schema`), applies changes with `set` and writes only values that differ from the default to `settings.conf`.
Each setting says whether it applies `live`, on `new` outputs, or after a `restart` and the panel shows that.

Add your own setting with one line in `settings.schema`:

    key|type|extra|apply|default|label
    my_gap|number|0:40|live|8|Gap used by my hook
    my_mode|choice|a,b,c|live|a|My mode

Types: bool, number (extra `min:max[:step]`), text, choice (extra `a,b,c`), color. Values are validated on `set`, readable with
`get`, and arrive in `on-setting-change` hooks; a hook or script is what acts on a free-form setting.

Animation: `enable_animation` (default off), `animation_stiffness`, `animation_damping`. A spring-damper moves window positions
(not sizes) toward their layout target, stepped at a fixed 1/240 s from the output frame callback. Off is the old instant path.

## 5. Bar modules

`bar.conf`:

    left  = tags layout windows
    right = mymail cpu ram vol bat clock
    segment mymail 60 sh -c 'notmuch count tag:unread'

Built-in modules: tags layout windows outputs cpu gpu ram vol bat clock. `segment NAME SECONDS COMMAND` runs the command
(`timeout 2 sh -c`) at most every SECONDS and shows the first line of its output. No file means the old default bar.
`bar_position` and `bar_font` are read by `session` and need a restart of the bar.

## 5b. Volume

`volume ARGS` is a command like any other (socket, fifo, key bindings), so it works whatever has the keyboard, a dmenu included:

    volume up|down|micup|micdown [PCT]    step the speaker / microphone (setting volume_step, default 5; raising unmutes)
    volume mute|micmute [on|off]          toggle or set mute
    volume set spk|mic PCT                absolute, clamped to volume_max
    volume get                            spk PCT MUTED mic PCT MUTED seq N osd MS step PCT max PCT
    volume osd                            show the panel without changing anything

Every change emits `user volume spk PCT MUTED mic PCT MUTED` (a hook `on-user` or a bar module can follow it) and starts the volume
panel as an OSD. The compiled keys: XF86AudioRaiseVolume / LowerVolume / Mute / MicMute and Super+h / Super+l use it; your own:
`XF86AudioMicMute = volume micmute` in `keys.conf`.

`desktop settings volume` opens the panel for use: two bars (speaker, microphone), Tab switches row, Left/Right or -/+ step,
PgUp/PgDn x5, Home/End, m or Space mutes, Esc closes; mouse: click or drag a bar to set it, wheel steps the row under the pointer,
click the percent / MUTE text to mute. `desktop settings volume --osd` is the same without taking the keyboard, bottom centre; it closes
`volume_osd_ms` (default 1500) after the last change or pointer movement. The pointer on it keeps it up. One panel at a time
(lock `$XDG_RUNTIME_DIR/desktop-volume.lock`), it polls the compositor 10x a second, so changes from keys or scripts show at once.
Settings (in `desktop settings` too): `volume_step`, `volume_osd_ms` (0 = no OSD), `volume_max` (100..150).

Backend (`src/audio.c`, no library): `wpctl` against `@DEFAULT_AUDIO_SINK@` / `@DEFAULT_AUDIO_SOURCE@` (PipeWire/WirePlumber); a failure
is an `error:` reply. `DESKTOP_AUDIO_BACKEND=fake` (set in the compositor's environment) keeps the two channels in a text file
(`DESKTOP_AUDIO_FILE`, default `$XDG_RUNTIME_DIR/desktop-audio-fake`: `spk 0.50 0` / `mic 0.30 0`) and never touches the sound server;
the tests use it. `tests/volume` checks the command, event, panel keys, mouse paths, OSD timeout and the OSD over an open dmenu.
Test hooks of the panel: `DESKTOP_PANEL_KEYS="Right Tab m click:spk:70 mute:mic wheelup"`, `DESKTOP_PANEL_DUMP=1`.

## 6. Layouts, workspaces and monitors

Layouts: `tile` (master/stack), `float` (every window floating, free move/resize), `monocle`, `deck`, `grid`, `stack` and `tabbed`.
`tabbed` shows one window and a tab bar on top with the title of every tiled window of the workspace (oldest left); `stack` is the same
with one title row per window stacked above the shown one (i3 stacking). Floating windows are not tabs. A click on a tab, `tab
next|prev|N`, Super+[ / Super+] (focusstack) or any focus change shows that window. The bar is one scene buffer per output drawn with
drw.c (fontconfig/FreeType) in theme.c colours (focus_color active, border_color others, urgent_color urgent, root_color behind);
settings `tab_bar_height` (px, 0 = no bar) and `tab_font`.

    layout tabbed            # also: tile float monocle deck grid stack  (or next = swap with the previous one)
    tab next                 # tab prev, tab 2

Keys: Super+t tile, f float, m monocle, g tabbed, s stack, Shift+s deck, Shift+t grid, Space = previous layout.
The bar shows one letter: `=` tile, `f`, `m`, `d`, `g`, `s`, `t`.

The layout is remembered per (output, workspace): `layout tabbed` on workspace 2 of HDMI-A-1 changes only that pair, and coming
back to it (view) restores it; a view of several tags uses the slot of its lowest tag. `state` lists it per output
(`"layouts":[tile,...]` for tags 1..9, `"tabs"`, `"tabactive"`) and each window has `"visible"`. The `layout` event fires whenever the
shown layout of an output changes, including by switching workspace.

Outputs are independent: view, toggleview, layout, mfact, nmaster, Super+z/x (shiftview) and the matching ctl commands act on the
focused output only (`focusmon` / pointer decides which); the other outputs keep their workspace, layout and windows. The settings
`default_layout`, `mfact` and `nmaster` also change only the focused output (new outputs start from them). Not present in this
build: `follow_pointer_focus` (nothing to add).

## 7. Tests

    make BUILD=build-new OUT=build-new/desktop LINKDIR=build-new/bin
    tests/headless      # api, events, hooks, keys, rules, animation, bar, panel, layouts (needs socat, foot, grim, wtype, jq)
    tests/layouts       # two headless outputs: per-output independence, layout per (output, tag), tabbed/stack bar (also run by tests/headless)
    tests/protocols     # advertised wayland globals, default and legacy_protocols = 0
    tests/volume        # volume command, panel and OSD with the fake audio backend (needs socat, grim, bin/wtype)

Both start `build-new/desktop` with `WLR_BACKENDS=headless`, their own runtime and config dirs, and never touch a running session.
