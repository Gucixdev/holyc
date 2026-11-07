# lean4 - lazytui, machine-checked

Lean 4 proofs about lazytui's C code: a Lean model of each C function, theorems about the model, and a test that
holds the C code to the model. The shared parts (the audit command, bit lemmas, the oracle driver, the run steps)
are in [holyc/lean4](../../lean4/README.md); [base/lean4](../../base/lean4/README.md) shows a full differential
test on them.

## Status

Nothing is proved yet: the folder builds, and the audit and the source scan pass on an empty library.

## Run

```sh
lazytui/lean4/run     # build, axiom audit, source scan (needs elan; core Lean only, no Mathlib)
```
