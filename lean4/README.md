# lean4 - what every project's proofs share

A Lean 4 package (`holyc`, library `Holyc`) and a bash library for the `lean4/` folder of every holyc project, so
that a project writes its model and its theorems and nothing else twice.

| file | what it gives a project |
|---|---|
| `Holyc/Audit.lean` | `#audit_axioms Root`: every theorem of the modules under Root rests on `propext`, `Classical.choice` and `Quot.sound` only (no `sorry`, no `native_decide`, no library axiom); a project's `Audit.lean` is three lines |
| `Holyc/Bits.lean` | C's bit tricks against the arithmetic they stand for: `and_high_mask` (`x & ~(2^k - 1)` on 64 bits is `x / 2^k * 2^k`), `alignUp`, `alignC`, `alignC_eq`, `alignUp_spec` |
| `Holyc/Oracle.lean` | the driver of a differential test's oracle: `Holyc.Oracle.run step init` replays a C harness's lines through the model's `step` |
| `lib.sh` | the steps of a project's `lean4/run`: `lean_build`, `lean_audit`, `lean_scan`, `diff_test HARNESS ORACLE` (gcc and clang, seeded runs), `mutant NAME FILE SED`, `lean_end` |

A project takes it with `require holyc from "../../lean4"` in its `lakefile.lean` (`lake update` once after
adding it) and `. "$(dirname "$0")/../../lean4/lib.sh"` in its `run`; `tools/leaninit` writes both. Core Lean only:
it builds in seconds and adds no dependency. `lean4/run` checks the library itself the same way.

core/lean4 is the example: its run is a list of steps, its oracle a `step` function.
