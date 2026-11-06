# lean4 - base, machine-checked

Lean 4 proofs about base's memory allocators: a Lean model of the arena (`core_arena_alloc.h`, default FLAT backend)
and of the pool (`core_pool_alloc.h`), theorems about the models, and a differential test that holds the C code to
them. The method is the one [vecindex/lean4](../../vecindex/lean4/README.md) uses.

## Run

```sh
base/lean4/run          # build, axiom audit, source scan, differential test, mutants (~20 s)
SEEDS=1000 base/lean4/run
```

Needs elan with `leanprover/lean4:v4.26.0` (core Lean only, no Mathlib), gcc, and clang if present. Output on
2026-11-06 (2 vCPU Xeon at 2.80 GHz, gcc 13.3, clang 18):

```
  ok   lake build: 5 jobs
  ok   axiom audit: 62 theorems (20 written in the sources)
  ok   source scan: no sorry, admit, native_decide, bv_decide or axiom
  ok   differential: 400 runs of 3000 operations (seeds 1-200, gcc and clang), arena and pool equal the model
  ok   mutants: 5 of 5 caught
```

## What is proved, tested and trusted

1. **Model.** `Base/Arena.lean` follows arena_alloc, arena_snapshot, arena_rewind, arena_reset and
   arena_alloc_aligned in one region (FLAT reserves 1 GiB for the first region; the test stays inside it, so the C
   never chains a second). `Base/Pool.lean` follows pool_init, pool_free_all, pool_alloc and pool_dealloc, chunks
   named by index.
2. **Theorems** on the model, listed below; the axiom audit checks that none rests on more than `propext`,
   `Classical.choice` and `Quot.sound`.
3. **Differential test.** `c/alloc_harness.c` runs 3000 seeded random operations on the real arena and pool and
   prints every result; `allocoracle` (`Oracle.lean`) replays the operations on the model; the outputs must be
   identical. 200 seeds, built with gcc and with clang. Seeded random cases, not every input.
4. **Mutants.** Five one-line changes to a copy of `src/` (the granule rounding, the alignment mask, the rewind,
   the pool's chunk rounding, pool_free_all's loop); the differential test must fail on each.

Trusted: Lean's kernel, the Lean compiler (it builds the oracle), gcc, clang, libc, and the step from the model to
the C code, which is tested, not proved.

| C | Lean | Theorem |
|---|---|---|
| arena_alloc | `Arena.alloc` | `alloc_aligned` (8-byte aligned), `alloc_in_region` (all n bytes inside the region) |
| arena_alloc in a row | `Arena.run` | `run_disjoint` (blocks in order, none overlaps another) |
| arena_snapshot, arena_rewind | `Arena.snapshot`, `Arena.rewind` | `rewind_run` (rewinding after any run gives back the state before it) |
| arena_reset | `Arena.reset` | `reset_empty` |
| arena_alloc_aligned, pool_align_up | `Arena.alignC`, `Arena.alignUp` | `and_high_mask` (`x & ~(2^k - 1)` on 64 bits is `x / 2^k * 2^k`), `alignC_eq`, `alignUp_spec` (aligned, not before the raw block, inside it) |
| pool_init, pool_free_all | `Pool.freeAll` | `freeAll_inv` (every chunk free, once) |
| pool_alloc | `Pool.alloc` | `alloc_inv` (a chunk that is not live, below k), `alloc_none` (empty only when every chunk is live) |
| pool_dealloc | `Pool.dealloc` | `dealloc_inv` (of a live chunk), `double_dealloc` (of a free one: the list holds it twice) |
| chunk addresses | | `chunk_in_buffer` (chunk i of `bs / cs` lies inside the buffer), `chunk_size_spec` |

## Findings

- pool_dealloc checks nothing: a chunk freed twice is on the free list twice, and two later pool_alloc calls return
  the same chunk (`double_dealloc`). A pointer outside the buffer is linked in as well. Callers keep the
  precondition `dealloc_inv` states; the harness only frees live chunks.
- pool_init aligns the chunk size to 8 bytes but not the buffer: chunks are aligned only when the buffer is (an
  arena block is).
- Not covered: the arena's second and later regions (past 1 GiB under FLAT), the LINUX_MMAP backend, arena_realloc
  and the string helpers, the persistent arena and pool.
