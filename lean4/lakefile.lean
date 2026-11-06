import Lake
open Lake DSL

/- holyc's shared Lean library: what every PROJECT/lean4 would otherwise write again. Core Lean only, so it builds
   in seconds; a project takes it with `require holyc from "../../lean4"`. -/
package «holyc» where
  leanOptions := #[⟨`autoImplicit, false⟩]

@[default_target]
lean_lib «Holyc» where
  roots := #[`Holyc]
