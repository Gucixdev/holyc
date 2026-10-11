# gtop

A system monitor for the terminal, in C99 with nothing but libc and libm. It reads `/proc` and
`/sys` directly; no curses, no monitoring library. Linux only.

```
┌ CPU ────────────────┐┌ RAM ────────────────┐┌ DISK ───────────────────────────┐┌ NET ──────────────────────────────┐
│███████████ 5% L0.7  ││██████████ 6.9GB/40.4││███████████████ 366.9GB/683.9GB  ││agg rx:860B/s tx:860B/s            │
│▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁  ││▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃▃  ││                ▁▂▂▃▃▄▄▅▅▆▆▇▇██  ││ ▁▁▁▁▁▁▂▂▂▂▂▂▃▃▃▃▃▄▄▄▅▅▅▆▆▆▇▇▇███  │
│user:1.6% sys:1.6% io││used  2.6GB/7.7GB    ││sda 119.2GB                      ││enp2s0  rx:0B/s tx:0B/s  link:?    │
│c0    5.6% 4490MHz 56││zram  502.6MB/7.7GB  ││█████████████████ 72.3GB/46.9GB  ││                                   │
│c1   10.5% 4373MHz 47││cache 2.1GB/7.7GB    ││                ▁▂▂▃▃▄▄▅▅▆▆▇▇██  ││  [hidden - anonymous mode]        │
│c2   13.2% 4367MHz 45││swap  1.7GB/39.7GB   ││├── sda1 2.0GB → /boot ████████ 3││wlan0  rx:1.4KB/s tx:317B/s  link:?│
│c3   10.8% 800MHz  45││RAM+swap capacity: 47││└── sda2 117.2GB → /var/tmp █████│└───────────────────────────────────┘
│c4    2.9% 4522MHz 46││free  40.4GB/47.3GB  ││sdb 931.5GB                      │
│c5    5.7% 800MHz  49│└─────────────────────┘│███████████████ 294.6GB/636.9GB  │
│c6    0.0% 800MHz  --│┌ GPU ────────────────┐│                ▁▂▂▃▃▄▄▅▅▆▆▇▇██  │
│c7    0.0% 800MHz  --││████████████████ 6%  ││└── sdb1 931.5GB → /data/distrobo│
│c8    0.0% 800MHz  --││▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁▁  ││sr0 [rm] 1024.0MB                │
│c9    2.7% 4300MHz --││vram ██████████ 9% 37││                                 │
│c10   2.7% 4402MHz --│└─────────────────────┘│                                 │
│c11   0.0% 4323MHz --│                       └─────────────────────────────────┘
└─────────────────────┘
┌ PROC [app] ────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│   NAME                    CPU%   MEM%       MEM  DISK%       DISK  GPU%      GPU PROCS ACTIVITY                    │
│   systemd                 0.0%   0.1%       10M   0.0%       0B/s  0.0%        0M     2                            │
│↑/↓ select [1-1/10]  ←→ mode  space: mark  k: kill  /: command  +/-: refresh 1000ms  r: record  a: anonymous        │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

A real frame from `gtop --snapshot 118 22 --keys a -f systemd` (anonymous mode, process list filtered).

## Build

```sh
make            # ./gtop
make test       # tests/run: 37 checks against the live system
sudo make install   # PREFIX=/usr/local: bin/gtop and share/gtop/commands/
```

C standard: `-std=c99`, plus `_POSIX_C_SOURCE=200809L` and `_DEFAULT_SOURCE` set inside the
source. Needs `-lm`.

## What is on screen

- **CPU**: bar split into user / system / iowait, trend graph, per-core percentage, frequency and
  temperature (Intel `Core N` sensors, AMD `Tctl`/`Tdie` for every core), 1-minute load.
- **RAM**: one bar for used / zram / cache / swap / free, trend graph and the numbers.
- **GPU**: `nvidia-smi`, else amdgpu sysfs (`gpu_busy_percent`, VRAM), else `intel_gpu_top`.
  A per-process GPU% and VRAM column appears with NVIDIA.
- **TPU / NPU**: boxes only when a Coral TPU (`/dev/apex_0`, USB) or `/dev/accel/*` exists.
- **DISK**: aggregate usage bar and activity graph; per device: model, size, usage bar, graph and
  a partition tree with mount points and usage.
- **NET**: aggregate rate graph; per interface: rx/tx, link speed bar (or a graph when the speed is
  unknown), addresses (IPv4/IPv6, link-local filtered).
- **PROC**: three views, switched with left/right: `app` (grouped by program), `raw` (one row per
  pid) and `user/system` (kernel threads in their own section). Columns:
  `NAME CPU% MEM% MEM DISK% DISK [GPU% GPU] ACTIVITY`. ACTIVITY is a CPU graph of the last two
  minutes that fills all the width left over. Rows of processes that just exited stay for 3
  seconds in grey. Order only re-sorts every 4 seconds so rows do not jump.
- **Process tree**: a program with several processes is one row, `▸ firefox (12)`, with the sums.
  Enter opens it into a tree, one row per process, biggest CPU first:

  ```
  ▾ firefox (3)             12.4%  ...
  ├── firefox [4101]         9.8%  ...
  ├── firefox [4177]         2.1%  ...
  └── firefox [4190]         0.5%  ...
  ```

  Every child row can be marked, killed or used as a palette target on its own. Enter on the
  program row (or on a child) closes the tree again.

## Keys

| key | action |
|---|---|
| `q` | quit |
| left / right | previous / next PROC view |
| up / down | move the cursor (dividers are skipped) |
| space | mark / unmark the row (several rows can be marked; a child row marks just that process) |
| Enter | open / close the process tree of the program under the cursor |
| `k` | SIGKILL every pid of the marked rows, or of the row under the cursor (a program row: all its processes; a child row: that one) |
| `/` | command palette: type a name, Enter runs `commands/<name>` |
| `r` | record all numbers to `gtop-record-<unix time>.csv` (toggle) |
| `a` | anonymous mode: IP addresses and disk models hidden, on screen and in recordings |
| `+` / `-` | refresh interval 100 ms ... 5 s, in steps of 100 ms |

## Command palette

`commands/` holds plain shell scripts, the file name is the command. They get the extra words you
typed first and then the pids of the marked rows (or the row under the cursor) as arguments:

```
/renice 10        ->  sh commands/renice 10 <pid> <pid> ...
```

Shipped: `kill`, `term`, `suspend`, `resume`, `renice`. A script is looked up in
`$GTOP_COMMANDS`, `./commands`, `<binary dir>/commands` and `<binary dir>/../share/gtop/commands`.
Add a file to add a command; there is nothing to register.

## Without a terminal

```
gtop --snapshot [W H] [--keys KEYS] [-f SUBSTR]
```

draws one frame as plain text. `KEYS` feeds keys first (`U D L R` arrows, `E` enter, anything else
literally; a refresh happens after them), `-f` keeps only processes whose name contains `SUBSTR`.
`tests/run` uses this for everything: views, marking, kill, palette, recording, anonymous mode.

## History

This is a port of an earlier Rust/ratatui version (`bsc`) with the same panels, layout, numbers and
keys; the C version is a single file, `gtop.c`.
