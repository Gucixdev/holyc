# lean4 - base, machine-checked

Lean 4 proofs about base's memory allocators: a Lean model of the arena (`core_arena_alloc.h`, default FLAT backend)
and of the pool (`core_pool_alloc.h`), theorems about the models, and a differential test that holds the C code to
them. The parts that skip work for speed - realloc in place, calloc that clears only memory handed out before,
the pool's lazy init - are proved to give what the plain versions give..

## Run

```sh
core/lean4/run          # build, axiom audit, source scan, differential test, mutants (~35 s)
SEEDS=1000 core/lean4/run
```

Needs elan with `leanprover/lean4:v4.26.0` (core Lean only, no Mathlib), gcc, and clang if present. Output on
2026-11-06 (2 vCPU Xeon at 2.80 GHz, gcc 13.3, clang 18):

```
  ok   lake build: 5 jobs
  ok   axiom audit: 92 theorems (31 written in the sources)
  ok   source scan: no sorry, admit, native_decide, bv_decide or axiom
  ok   differential: 400 runs of 3000 operations (seeds 1-200, gcc and clang), arena and pool equal the model
  ok   mutants: 9 of 9 caught
```

## What is proved, tested and trusted

1. **Model.** `Base/Arena.lean` follows arena_alloc, arena_snapshot, arena_rewind, arena_reset, arena_realloc,
   arena_calloc, arena_decommit and arena_alloc_aligned in one region (FLAT reserves 1 GiB for the first region;
   the test stays inside it, so the C never chains a second). Memory contents are modelled as a "may be nonzero"
   predicate over the region's bytes. `Base/Pool.lean` follows pool_init, pool_free_all, pool_alloc and
   pool_dealloc, chunks named by index.
2. **Theorems** on the model, listed below; the axiom audit checks that none rests on more than `propext`,
   `Classical.choice` and `Quot.sound`.
3. **Differential test.** `c/alloc_harness.c` runs 3000 seeded random operations on the real arena and pool and
   prints every result; it fills every block it gets with a nonzero byte, as a user would, and checks that each
   arena_calloc block reads zero and each arena_realloc block keeps its bytes. `allocoracle` (`Oracle.lean`)
   replays the operations on the model; the outputs must be identical. 200 seeds, built with gcc and with clang.
   Seeded random cases, not every input.
4. **Mutants.** Nine one-line changes to a copy of `src/` (granule rounding, alignment mask, rewind, the kept
   high-water mark, calloc's clearing, realloc's last-block test, decommit, the pool's chunk rounding and its fresh
   chunks); the differential test must fail on each.

Trusted: Lean's kernel, the Lean compiler (it builds the oracle), gcc, clang, libc, and the step from the model to
the C code, which is tested, not proved.

| C | Lean | Theorem |
|---|---|---|
| arena_alloc | `Arena.alloc` | `alloc_aligned` (8-byte aligned), `alloc_in_region` (all n bytes inside the region) |
| arena_alloc in a row | `Arena.run` | `run_disjoint` (blocks in order, none overlaps another) |
| arena_snapshot, arena_rewind | `Arena.snapshot`, `Arena.rewind` | `rewind_run` (rewinding after any run gives back the state before it) |
| arena_reset | `Arena.reset` | `reset_empty` |
| arena_alloc's overflow-free rounding | `Arena.granules` | `granules_c` (`n / 8 + (n % 8 != 0)` is `(n + 7) / 8`) |
| arena_realloc | `Arena.realloc` | `realloc_in_place` (grown where it is: same start, new bytes at the old top, inside the region), `realloc_moved` (otherwise a fresh block at the top) |
| arena_calloc | `Arena.callocZeroed` | `calloc_zero` (clearing only below the high-water mark leaves every byte of the block zero), `clean_alloc`, `clean_write`, `clean_rewind` |
| arena_decommit | `Arena.decommit` | `clean_decommit` (the invariant calloc relies on survives giving pages back) |
| arena_alloc_aligned, pool_align_up | `Arena.alignC`, `Arena.alignUp` | `and_high_mask` (`x & ~(2^k - 1)` on 64 bits is `x / 2^k * 2^k`), `alignC_eq`, `alignUp_spec` (aligned, not before the raw block, inside it) |
| pool_init, pool_free_all | `Pool.init` | `init_inv` (every chunk free, once, none touched) |
| pool_alloc | `Pool.alloc` | `alloc_inv` (a chunk that is not live, below k, from the free list or the fresh ones), `alloc_none` (none left only when every chunk is live) |
| pool_dealloc | `Pool.dealloc` | `dealloc_inv` (of a live chunk), `double_dealloc` (of a free one: the list holds it twice) |
| chunk addresses | | `chunk_in_buffer` (chunk i of `bs / cs` lies inside the buffer), `chunk_size_spec` |

## Findings

- pool_dealloc checks nothing: a chunk freed twice is on the free list twice, and two later pool_alloc calls return
  the same chunk (`double_dealloc`). A pointer outside the buffer is linked in as well. Callers keep the
  precondition `dealloc_inv` states; the harness only frees live chunks.
- pool_init aligns the chunk size to 8 bytes but not the buffer: chunks are aligned only when the buffer is (an
  arena block is).
- arena_alloc rounded `(size_bytes + 7) / 8`, which wraps around for a size near SIZE_MAX and returned a block of
  0 granules; it computes `n / 8 + (n % 8 != 0)` now (`granules_c`), and a size no region can hold returns NULL.
- Not covered: the arena's second and later regions (past 1 GiB under FLAT), the LINUX_MMAP backend, the string
  helpers, the persistent arena and pool. Huge pages change no address or value, so the model has no part for them.
