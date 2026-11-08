import Lake
open Lake DSL

/- netman's machine-checked model proofs: DHCP lease timers, roaming policy, kill switch ruleset.
   Core Lean only, no sorry: `lake build`. -/
package «netman» where
  leanOptions := #[⟨`autoImplicit, false⟩]

@[default_target]
lean_lib «Netman» where
  roots := #[`Netman]
