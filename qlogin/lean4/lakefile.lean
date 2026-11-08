import Lake
open Lake DSL

/- qlogin's machine-checked model proofs: login lockout, inhibitors, session/seat state, device pause/resume.
   Core Lean only, no sorry: `lake build`. -/
package «qlogin» where
  leanOptions := #[⟨`autoImplicit, false⟩]

@[default_target]
lean_lib «Qlogin» where
  roots := #[`Qlogin]
