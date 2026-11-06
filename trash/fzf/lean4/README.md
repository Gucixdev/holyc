# lean4 - fzf, machine-checked

Lean 4 proofs about fzf's C code: a Lean model of each C function, theorems about the model, and a test that
holds the C code to the model. The method is the one [vecindex/lean4](../../vecindex/lean4/README.md) uses.

## Status

Nothing is proved yet: the folder builds, and the audit and the source scan pass on an empty library.

## Run

```sh
fzf/lean4/run     # build, axiom audit, source scan (needs elan; core Lean only, no Mathlib)
```
