# notcoreutils

Nine small command-line tools, one C source file and one man page each. Things GNU coreutils
already does well are deliberately not reimplemented here.

| tool | what it does |
|---|---|
| `cb` | clipboard manager: owns the X11 `CLIPBOARD` selection, keeps a history, lets you pick an entry back |
| `cloc` | counts lines of code per language: `cloc [--table \| --files] [ROOT]` |
| `colorado` | stdin colorizer for log-level words, quoted strings, IPv4 addresses and numbers (the idea of grc/ccze, no config file) |
| `dv` | disk view: sizes a directory recursively and prints the biggest entries first (a non-interactive slice of ncdu) |
| `man` | minimal manual-page viewer: finds the page under `$MANPATH` or the FHS paths, gunzips, renders the common troff macros as readable text |
| `netcat` | minimal TCP pipe: `netcat HOST PORT` connects, `netcat -l PORT` listens for one connection, stdin/stdout both ways |
| `sudo` | doas-style privilege escalation: asks for your own password, runs the command; not installed setuid by `make install` |
| `tgates` | ternary logic gates on the command line over balanced ternary {-1, 0, 1}: `neg`, `min`, `max`, `diff` |
| `tree` | `tree(1)`-compatible directory listing, written from scratch and tested against the real `tree` |

## Build and test

```sh
make              # bin/<tool> for every tool; `make cloc` builds one
make test         # tests/run, 57 checks (tree is compared with the real tree when installed)
make install      # PREFIX=/usr/local: binaries and man pages
```

C standard: `-std=gnu11 -D_GNU_SOURCE -U_FORTIFY_SOURCE` (see the Makefile). The tools share a
handful of single-file headers from `deps/` (the same family as [`../base`](../base), kept as their own copies so this directory builds on its own).
