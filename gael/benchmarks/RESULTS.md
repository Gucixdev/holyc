# Benchmark results

What `./bench` measured, newest at the end: the median wall time of each program in ms (the whole process, start
included) and the most memory one run held, in MB. The programs are in this folder, the same algorithm in every
language, written the way that language is usually written; each one's output is checked against `NAME.expected`
before it is timed. `start` is an empty program. `gdscript` is Godot's time minus Godot's own start (`start` in the
godot column), so it is the script alone. A cloud machine is shared and noisy: compare numbers from one run, not
across runs.

## 2026-10-25 4021b68

Intel(R) Xeon(R) Processor @ 2.80GHz, 2 cores, 8 GB; 10 runs after 1 warmup, median wall time.

- gael: gael 4021b68
- c: gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- python: Python 3.13.16
- lua: Lua 5.4.6
- luajit: LuaJIT 2.1.1703358377
- guile: guile (GNU Guile) 3.0.9
- chez: Chez Scheme 9.5.8
- chicken: Chicken 5.3.0
- sbcl: SBCL 2.2.9.debian
- node: Node v22.22.0
- godot: Godot 4.7.2.stable.official.ed1daf0bf

| ms | gael | c | python | lua | luajit | guile | chez | chicken | sbcl | node | godot | gdscript |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| start | 1.6 | 1.4 | 17.5 | 2.3 | 2.3 | 14.7 | 81.5 | 3.6 | 6.4 | 29.2 | 873.9 | 0.0 |
| fib | 526.4 | 29.0 | 908.6 | 395.3 | 59.9 | 188.7 | 130.1 | 498.0 | 112.6 | 97.5 | 4102.8 | 3228.9 |
| loop | 618.2 | 29.2 | 2971.2 | 302.7 | 95.2 | 411.0 | 408.1 | 1554.8 | 224.8 | 95.6 | 2334.7 | 1460.8 |
| map | 834.8 | 102.0 | 834.5 | 215.4 | 59.8 | 1144.8 | 400.4 | - | 317.1 | 408.5 | 1792.8 | 918.9 |
| nbody | 587.5 | 14.1 | 1148.7 | 493.5 | 73.5 | 2441.7 | 473.2 | 1056.1 | 560.5 | 81.3 | 3716.9 | 2843.0 |
| particles | 577.5 | 12.2 | 1718.1 | 435.8 | 47.4 | 1752.5 | 284.9 | 822.5 | 451.3 | 81.2 | 4648.5 | 3774.6 |
| sieve | 724.9 | 24.1 | 1053.6 | 557.7 | 175.6 | 355.0 | 277.6 | 580.1 | 151.6 | 204.8 | 2050.8 | 1176.9 |

| MB | gael | c | python | lua | luajit | guile | chez | chicken | sbcl | node | godot | gdscript |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| start | 2 | 1 | 8 | 2 | 2 | 9 | 48 | 6 | 19 | 39 | 208 | 208 |
| fib | 2 | 2 | 9 | 3 | 3 | 9 | 48 | 9 | 23 | 47 | 208 | 208 |
| loop | 2 | 2 | 9 | 3 | 2 | 9 | 48 | 6 | 25 | 49 | 209 | 209 |
| map | 67 | 36 | 111 | 28 | 17 | 63 | 95 | - | 112 | 105 | 211 | 211 |
| nbody | 2 | 2 | 9 | 3 | 3 | 10 | 48 | 18 | 82 | 49 | 209 | 209 |
| particles | 2 | 2 | 10 | 4 | 4 | 12 | 48 | 18 | 84 | 53 | 209 | 209 |
| sieve | 104 | 6 | 47 | 130 | 66 | 47 | 58 | 45 | 66 | 86 | 210 | 210 |

The bytecode VM against the script alone in Godot 4.7.2: gael is faster on all six, 6.1 times on fib, 2.4 on loop,
1.1 on map, 4.8 on nbody, 6.5 on particles, 1.6 on sieve; faster than Python on five and level on map. Lua 5.4's
interpreter (registers, not a stack) is 1.2 to 3.9 times faster than gael, LuaJIT and Node much more; C is 8 to 47
times faster (8 on the map, whose work is in C in both), the distance a VM keeps and gael → C is for. gael starts
almost as fast as C (1.6 ms against 1.4); Godot's engine takes 0.9 s. Memory: as small as C, but for the sieve (a
bool takes a whole 8-byte cell in a vec, and the vec doubles: 104 MB against C's 6) and the map (67 MB against 36).

## 2026-10-25 88e71bd

Intel(R) Xeon(R) Processor @ 2.80GHz, 2 cores, 8 GB; 10 runs after 1 warmup, median wall time.

- gael: gael 88e71bd
- c: gcc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
- python: Python 3.13.16
- lua: Lua 5.4.6
- luajit: LuaJIT 2.1.1703358377
- guile: guile (GNU Guile) 3.0.9
- chez: Chez Scheme 9.5.8
- chicken: Chicken 5.3.0
- sbcl: SBCL 2.2.9.debian
- node: Node v22.22.0
- godot: Godot 4.7.2.stable.official.ed1daf0bf

| ms | gael | c | python | lua | luajit | guile | chez | chicken | sbcl | node | godot | gdscript |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| start | 1.5 | 1.4 | 18.6 | 2.1 | 1.8 | 13.6 | 72.6 | 4.0 | 6.9 | 36.0 | 944.4 | 0.0 |
| fib | 324.8 | 12.8 | 956.9 | 483.8 | 70.9 | 214.1 | 127.4 | 553.2 | 110.2 | 101.4 | 4156.0 | 3211.6 |
| loop | 281.3 | 27.7 | 2564.3 | 293.0 | 88.0 | 384.9 | 333.1 | 1413.4 | 232.1 | 105.2 | 2512.6 | 1568.2 |
| map | 340.1 | 81.9 | 689.7 | 223.0 | 54.6 | 1027.1 | 428.8 | - | 319.2 | 459.8 | 1729.7 | 785.3 |
| nbody | 421.9 | 19.8 | 1129.8 | 503.9 | 59.8 | 2468.9 | 458.6 | 1059.7 | 556.7 | 62.1 | 3634.1 | 2689.7 |
| particles | 320.4 | 11.4 | 1777.3 | 425.2 | 45.0 | 1719.8 | 297.3 | 890.3 | 473.0 | 73.1 | 4979.0 | 4034.6 |
| sieve | 320.9 | 26.3 | 1041.6 | 524.1 | 163.5 | 367.6 | 318.3 | 578.5 | 158.1 | 185.6 | 2152.9 | 1208.5 |

| MB | gael | c | python | lua | luajit | guile | chez | chicken | sbcl | node | godot | gdscript |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| start | 1 | 1 | 9 | 2 | 2 | 9 | 48 | 6 | 18 | 39 | 203 | 203 |
| fib | 1 | 2 | 8 | 3 | 2 | 9 | 48 | 9 | 22 | 47 | 203 | 203 |
| loop | 1 | 2 | 9 | 3 | 2 | 9 | 48 | 6 | 24 | 49 | 203 | 203 |
| map | 67 | 36 | 111 | 28 | 16 | 63 | 96 | - | 111 | 105 | 205 | 205 |
| nbody | 2 | 2 | 9 | 3 | 3 | 10 | 48 | 18 | 80 | 49 | 204 | 204 |
| particles | 2 | 2 | 11 | 4 | 4 | 12 | 48 | 18 | 82 | 53 | 203 | 203 |
| sieve | 3 | 6 | 47 | 130 | 66 | 47 | 58 | 45 | 64 | 86 | 205 | 205 |

Instructions each program ran, counted by callgrind (`valgrind --tool=callgrind`), for gael built at the first run's
commit and at this one; unlike wall time, the count does not change with the load on the machine:

| millions | 4021b68 | 88e71bd | fewer |
|---|---:|---:|---:|
| fib | 4956 | 2317 | 2.14x |
| loop | 8572 | 2410 | 3.56x |
| map | 1262 | 651 | 1.94x |
| nbody | 6673 | 3574 | 1.87x |
| particles | 7034 | 3297 | 2.13x |
| sieve | 6485 | 3475 | 1.87x |

After computed goto, superinstructions, conditions compiled to jumps, the new map and bools as bits, the VM runs 1.9
to 3.6 times fewer instructions. Against the script alone in Godot 4.7.2 gael is 2.3 (map) to 12.6 (particles) times
faster, against Python 2.0 to 9.1. Against Lua 5.4 gael is faster on five of the six (fib 1.5 times, nbody 1.2,
particles 1.3, sieve 1.6; loop level at 1.04, within the noise), and Lua is 1.5 times faster on the map, where most of
the keys land in its array part. LuaJIT and Node compile to machine code and are 1.7 to 7 times faster, but for Node on
the map, which is 1.35 times slower than gael; C is 4 to 28 times faster. Start: 1.5 ms (C 1.4, Lua 2.1). Memory:
the sieve takes 3 MB, half of C's 6 (C stores a byte per number, gael a bit); the map 67 MB, against C's 36 and Lua's
28.

## 2026-10-25 c74e9f7, gpu-each

`benchmarks/gpu-balls.gael`: 20 steps of a falling, bouncing ball (four f32 fields, one `when`) over a million balls,
as one `gpu-each` per step. Intel(R) Xeon(R) Processor @ 2.10GHz, 2 cores, 8 GB, no GPU: the "GPU" here is Mesa's
lavapipe (llvmpipe 20.1, Vulkan 1.4 on the CPU's two cores), through wgpu-native v29. hyperfine, 7 runs after 1 warmup.

| | mean ms | ± | per step |
|---|---:|---:|---:|
| making the million balls (0 steps) | 68.0 | 9.0 | |
| `GAEL_GPU=0`: the loop on the VM | 1437.6 | 35.6 | 68.5 |
| `GAEL_GPU=1`: lavapipe | 618.2 | 31.1 | 27.5 |

Both print the same last ball. Even with Vulkan on the CPU the kernel runs 2.5 times faster than the VM's loop, and
that time includes making the device, compiling the shader, and making, filling and reading back fresh buffers each
step (16 MB up, 12 MB back). On a real GPU the kernel itself costs next to nothing and the copies are what remain, so
keeping the data on the GPU between steps is the next thing to measure. Memory: 67 MB on the VM, 222 MB with wgpu
(its own allocations and lavapipe's).
