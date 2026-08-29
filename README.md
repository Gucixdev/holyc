# bsc

A lightweight system-monitor TUI, loosely inspired by GNOME Resources
but for the terminal, built with ratatui.

## Run

```
cargo run --release
```

## Layout

- CPU (with GPU below it), RAM (with TPU/NPU below it when present),
  DISK, NET side by side across the top.
- PROC below: three view modes (app-grouped / raw / user-system split),
  cycled with ←/→ while focused. ↑/↓ moves the cursor, Space marks/
  unmarks a row (nnn-style multi-select), `k` kills the marked rows (or
  the row under the cursor if nothing's marked), `/` opens a command
  palette that dispatches to a real file under `commands/` named after
  what you type.
- CONTAINERS & SERVERS below that: docker/podman (all containers,
  running or stopped, with their real status), qemu VMs, BSD jails,
  and real listening ports. `Tab` switches focus between PROC and this
  panel; `s`/`x`/`r` start/stop/restart the selected docker/podman
  container.

## commands/

Plain files, no `.sh` extension - filename is the command name. Typed
into the `/` palette in PROC, run with the selected/marked pid(s) as
real positional arguments.
