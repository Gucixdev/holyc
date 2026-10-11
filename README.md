# holyc

A collection of suckless-tool-style forks, each kept as its own subtree (full upstream history
preserved via `git subtree`, not squashed or copy-pasted in).

- **dwl** — fork of [dwl](https://codeberg.org/dwl/dwl) (itself built on dwm/sway/tinywl): own
  patches on top (i3-style layouts/workspaces/directional focus, own dmenu wired in as the
  launcher, window-geometry fixes). Licenses: `dwl/LICENSE`, `dwl/LICENSE.dwm`,
  `dwl/LICENSE.sway`, `dwl/LICENSE.tinywl`.
- **nnn** — fork of [nnn](https://github.com/jarun/nnn) (branch `debloat`): an arena-allocator
  fix for an O(n²) `dentfill()`, plus personal opener/smart-edit scripts (GUI Emacs integration
  that doesn't block nnn while open). License: `nnn/LICENSE`.

Each subtree keeps its own upstream remote inside its own git history (visible via
`git log <dir>/`); this top-level repo is just where they're published together.

## Updating from upstream

```sh
git subtree pull --prefix=<dir> <upstream-url> <branch> --squash
```

## License

Each subtree keeps its own upstream license (see each folder's own `LICENSE*` files) - nothing
here is unified under a single license.
