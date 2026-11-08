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

## 6. Tests

    make BUILD=build-new OUT=build-new/desktop LINKDIR=build-new/bin
    tests/headless      # api, events, hooks, keys, rules, animation, bar, panel (needs socat, foot, grim, wtype)
    tests/protocols     # advertised wayland globals, default and legacy_protocols = 0

Both start `build-new/desktop` with `WLR_BACKENDS=headless`, their own runtime and config dirs, and never touch a running session.
