# fzf (C)

Own implementation of fzf in C (libc + pthreads only), compatible with fzf 0.74's command line, scoring
(FuzzyMatchV2 port), extended search syntax and default look. `make` builds, `make install` puts it in
`~/.local/bin/fzf` (the Go one stays at /usr/bin/fzf as fallback: remove the installed file to go back).

Files: `opts.c` options · `pattern.c`/`algo.c` matching + tiebreaks · `term.c` raw tty, keys, mouse ·
`keys.c` `--bind` grammar and actions table · `theme.c` `--color` · `draw.c` layout + rendering (cell grid) ·
`ui.c` event loop, search results, selection · `act.c` actions · `proc.c` feed/reload/preview/execute/placeholders/`--listen`.

Implemented: multi, preview (+`--preview-window` incl. alternatives `<N(...)`, offsets, wrap, follow), bindings with
events (start load change focus one zero result multi jump resize), `transform*`, `reload[-sync]`, `execute[-silent]`,
`become`, `--listen` (POST actions / GET state), `--track`/`--id-nth`, `--expect`, `--print-query`, `-1/-0`,
`--history`, mouse (click, double-click, wheel, right-click toggle), `--height`, borders/margin/padding, header/footer,
`--layout`, `--info` styles, `--ansi`.

Not done: `--wrap` for list items, per-section borders (`--list-border` etc. are accepted and ignored), `--gap`,
`--tail`, sixel/kitty image previews, `--tmux`, shell key-binding scripts (use `fzf --bash` from the Go one).

## Tests

`make test` runs `tests/run` (no dependencies): golden cases generated from fzf 0.74.4, exit codes,
`--read0`/`--print0`/`--ansi`, and a query x option matrix compared line by line with the real fzf when
one is installed (`/usr/bin/fzf` or `REAL_FZF=...`). The interactive UI was compared with a pty+pyte
harness (screenshots of both binaries with identical arguments, compared cell by cell), which is not
part of the repo.
