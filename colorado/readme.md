# colorado

One theme file, applied everywhere. You define a theme once (semantic roles + 16 ANSI colours + fonts, cursor, icons, radius, gaps),
and `colorado set NAME` writes it into every toolkit's real config: GTK2/3/4 (incl. libadwaita names), Qt5/Qt6 (qt5ct/qt6ct, Kvantum),
terminals, our own desktop (dwl/dmenu/lemonbar), Firefox, cursor/icon/font settings, fontconfig, xsettingsd, with notes for Electron and flatpak.
Plain C (gnu11), libc + libm only; an image tool (ImageMagick) is optional and only used by `extract` for non-PPM images.

Not to be confused with `notcoreutils/colorado.c` (the old 90-line stdin log colorizer). That file is untouched; it may become `colorado colorize` later.

## use

    ./run                      build bin/colorado (cc/gcc/clang, nice -n 19) and run all tests; no arguments
    bin/colorado set NAME      THE command: show the diff, back up every touched file, write, remember NAME as active
    bin/colorado current       what is active; per toolkit "in sync" / DRIFTED / MISSING; exit 1 on drift
    bin/colorado watch NAME    re-apply (with backup) whenever the theme file changes
    bin/colorado rollback      restore the newest backup, byte for byte (`rollback list`, `rollback DIR`)
    bin/colorado apply NAME    same as set, but a dry run (prints the diff) unless --apply
    bin/colorado list | show NAME | new | edit NAME | export NAME GEN [--out DIR] | check NAME | extract IMAGE | contrast A B | generators
    bin/colorado -tui [NAME]   live TUI (also `preview`)

Themes: `themes/*.theme` next to the binary (dark, light, high-contrast, gruvbox-dark, nord, catppuccin-mocha), then `~/.config/colorado/themes/`,
then `$COLORADO_THEMES`. `new` derives a full theme from 1-3 colours (`new --accent '#4f9cff' [--bg ..] [--fg ..] [--light]`, or interactively);
`extract photo.ppm` runs k-means (k=8, Lab) and maps clusters to roles. `check` is WCAG: fg/bg, text on panels, accent text, selection, links, focus ring,
status colours and ANSI colours; `dark`, `light`, `high-contrast` pass fully, gruvbox-dark honestly fails ANSI red (its upstream palette does).

## safety rules (all enforced by tests)

* `apply` is a dry run unless `--apply`; `set` always shows the diff first and has `--dry-run`. Nothing is written without a backup first.
* Backups: `$XDG_DATA_HOME/colorado/backups/<timestamp>-<pid>/` (manifest + copies). `rollback` restores bytes, modes, symlink targets and removes directories colorado created; files it created are moved into the backup's `created/`, not deleted.
* Existing files are merged, not replaced: ini files key by key (your other keys stay), css/rc/Xresources through a `colorado:begin/end` block, symlinked (dotfiles) files are written through the link.
* colorado never calls gsettings/dconf, never restarts or kills an application. `--reload` only runs the hooks below.
* Tests set HOME, XDG_CONFIG_HOME, XDG_DATA_HOME, TMUX_TMPDIR to a temp dir. The real `~/.config` is only read (qt5ct example, format reference).
* The default set is the verified generators; unverified ones need `--only NAME` or `--all`.

## what picks the theme up (live vs restart)

`set --reload` (or `watch --reload`) does only the safe live part: `tmux source-file` into a running server (tested), SIGHUP to a running `xsettingsd`
(delivery tested with a stand-in process; effect on real xsettingsd not seen), `xrdb -merge` when DISPLAY is set (xrdb not installed here, untested).

| toolkit | takes effect | status |
|---|---|---|
| tmux | live with `--reload` | verified |
| xsettingsd (GTK2/3 via XSETTINGS) | live with `--reload` | unverified |
| foot | new windows after you add `include=` to foot.ini (colours reload only on restart; foot's SIGUSR1/2 switch dark/light, not used) | file verified with `foot --check-config` |
| GTK3 / GTK4 / libadwaita | next app start (GTK may re-read settings.ini live; not relied on) | css parsed by real GTK3 and GTK4; libadwaita not installed, named colours unverified |
| GTK2 | next app start, only themes reading gtk-color-scheme | unverified |
| Qt5/Qt6 via qt5ct/qt6ct | next app start, needs `QT_QPA_PLATFORMTHEME=qt5ct` / `qt6ct` in your session (colorado prints it, never sets it) | scheme layout identical to the installed qt5ct files; not rendered in a Qt app |
| Kvantum | next app start | unverified, opt-in |
| our desktop (dwl, dmenu, lemonbar) | compiled in: paste `colorado/desktop-theme.h` into theme.c/menu.c/bar.c and rebuild | theme.c part compiles against the desktop's real COLOR macro; not rebuilt into the desktop |
| kitty, alacritty, Xresources, ansi OSC script | next terminal / xrdb | unverified, opt-in |
| Firefox | copy `colorado/firefox/*.css` into `<profile>/chrome/`, set `toolkit.legacyUserProfileCustomizations.stylesheets`, restart | unverified, opt-in |
| Electron / Chromium | note only (`--force-dark-mode ...`) | unverified, nothing written |
| flatpak | printed `flatpak override` lines, never run | flag names checked against `flatpak override --help`; effect unverified |
| fontconfig | next app start | verified with real `fc-match` |
| cursor theme (`~/.icons/default/index.theme`), xsettingsd | next session / app start | unverified |

## layout

    src/colorado.c        thin main: commands, theme lookup, set/current/watch/hooks
    src/mods/             stb-style single headers (header-only, static): color.h theme.h gen.h apply.h image.h tui.h,
                          generators gen_gtk.h gen_qt.h gen_term.h gen_desktop.h gen_web.h gen_misc.h, table gens.h
    themes/               built-in themes (plain text)
    tests/                all (runner), unit(+unit_test.c), golden(+expected/), apply, set, image, verify(+gtkcss.c), tui
    colorado.1            man page

## tested vs not tested

Tested (about 355 checks): WCAG math against published values, hex/HSL/Lab round trips, theme parse errors and emit round trip, derivation passing WCAG for 9 accents x dark/light,
golden files for all 19 generators, ini/block merge idempotence, apply into a temp HOME with pre-existing files and symlinks, rollback byte-identical (twice, newest first),
drift detection, hooks, watch, extract on PPM/PAM/PNG (hostile file names, junk, no tool), the TUI in a real tmux pane (edits, hex entry, help, save, resize, Ctrl-C, 44x12), real foot/tmux/fc-match/GTK css parsers.

Not tested: any of the "unverified" rows above; rendering in a real Qt, libadwaita, Firefox or Electron app; the desktop rebuilt with the fragment; `apply --apply` against the real home (never done by the tests, by design);
Qt font strings (not written); Wayland cursor env vars; `watch` uses mtime polling, not inotify.

## gaps / next

Qt fonts, `colorado colorize` (old log colorizer), Kvantum SVG shapes, a `set` that also edits foot.ini/kitty.conf to add the `include` line, inotify for `watch`, libadwaita check once it is installed.
