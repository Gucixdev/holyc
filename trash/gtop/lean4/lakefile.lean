import Lake
open Lake DSL

/- Core Lean only, so a build takes seconds. When a proof needs Mathlib, add it the way vecindex/lean4 does:
   require mathlib from git "https://github.com/leanprover-community/mathlib4.git" @ "v4.26.0" -/
package «gtop_lean» where
  leanOptions := #[⟨`autoImplicit, false⟩]

@[default_target]
lean_lib «Gtop» where
  roots := #[`Gtop]
