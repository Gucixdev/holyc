# gtop <-> lazytui cross-pollination

gtop (`notcoreutils/gtop.c`, ~2500 lines, one file) draws its own cell grid and has no dependency on lazytui.
Line numbers below are for gtop.c as of 2026-11-08. gtop.c was NOT edited.

## gtop widgets and what became of them

| gtop (function, line)            | what it is                                           | lazytui                                   |
|----------------------------------|------------------------------------------------------|-------------------------------------------|
| `graph_v` (1795)                 | history graph interpolated to the width, 8 levels    | NEW `lt_histgraph` (monitor/histgraph.c)  |
| `segmented_bar` (1777)           | stacked bar, last segment takes the rounding rest    | NEW `lt_segbar` (monitor/segbar.c)        |
| `draw_cpu` per-core loop (1844)  | per-core rows: pct, MHz, temp, mini graph, tiers     | NEW `lt_corebars` (monitor/corebars.c)    |
| `draw_disk`/`draw_net` (1989/2039) | rate rows with peak and auto-scaled history        | NEW `lt_ratepanel` (monitor/ratepanel.c)  |
| `draw_proc` (2090)               | grouped process table, kid rows, cursor, scroll      | NEW `lt_proctable` (monitor/proctable.c)  |
| `human_bytes`/`human_rate` (104) | "1.5MB", "1.5MB/s"                                   | NEW `lt_human_bytes`/`lt_human_rate`      |
| `tier_color` (74)                | <=60 / <=85 / above load colours                     | NEW `lt_load_slot` (a theme slot, not RGB)|
| `block` (1746)                   | bordered box with accent title                       | already `lt_panel`/frame                  |
| `emit_ansi` (2295), `grid_init`  | own cell grid + full redraw                          | already `lt_canvas` (diff rendering)      |

Notes on the ports: `lt_segbar` only lets the last segment absorb the remainder when the values add up to `total`
(gtop's CPU bar fills completely even at 31% load because user+sys+io < 100; that is a latent gtop quirk, see item 3 below).
`lt_proctable` takes rows from the caller; it does the display, sort, tree and input, while the /proc and eBPF collection
(`procs_refresh` 933) stays in gtop.

## What gtop should adopt from lazytui

1. **`lt_canvas` instead of the hand-made grid** (gtop.c `grid_init` 1682, `cell_put` 1704, `emit_ansi` 2295).
   The canvas does diff rendering, so a 1 Hz refresh writes only changed cells; gtop currently re-emits the whole screen. It also
   brings colour degradation (truecolor -> 256 -> 16 -> console) for free, replacing the fixed `C_*` palette, and themes.
   `read_key` (2349) could become `lt_tty` input: that adds mouse (click a header to sort, wheel on the process list).
2. **`lt_search` / the search bar for the command palette** (`run_palette` 1588, `draw_palette` 2239, `find_command` 1563).
   Today the palette is a free-text `/cmd` line. `lt_search_auto(query, docs, ...)` gives fuzzy matching plus `tag:` / `regex:`
   modes over the PATH command list, and `lt_searchbar` + `lt_list` draw the result list: type `kil` and get `kill`, `killall`.
   Also useful as an in-list process filter (`/firefox` narrows the PROC table), which gtop does not have.
3. **`lt_proctable` itself** for `draw_proc` (2090, ~150 lines of column math and scroll arithmetic, `build_rows` 893,
   `cursor_move` 1180): feed it `lt_proc` rows built from `Procs.g[]`/`Group.kids[]` (parent = group index), keep `P->mode`
   (clean/raw) as the caller's choice of row set. Gains: header click sorting, keyboard sort cycle (`s`/`r`), mouse, scrollbar,
   and the tree expand state persisting across refreshes (it is kept by pid).
   Also swap gtop's `segmented_bar` CPU call for `lt_segbar` so the bar shows idle as a shade instead of stretching user/sys/io to 100%.
4. (smaller) `lt_spinner` for the "collecting..." state while `bpf_start` (632) or `info_gather` (1294, runs external commands)
   works, instead of a blank info panel; `lt_keybar` for the bottom key hints.
