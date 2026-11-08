/-! Firewall confirm-or-rollback. Model of `nmfw_apply(cfg, confirm_s)`, `nmfw_confirm`, `nmfw__watchdog`
    and the snapshot rule in `nmfw__apply_nft` (src/mods/firewall.h). Rulesets are opaque `Nat`s. -/
namespace Netman.Confirm

structure St where
  cur       : Nat            -- ruleset in force
  snap      : Option Nat     -- fw.rollback snapshot
  pending   : Bool           -- fw.pid names a live watchdog
  deadline  : Nat
  now       : Nat
  confirmed : Bool           -- outcome flags: set when the decision is taken
  restored  : Bool
deriving Repr

def init (r : Nat) : St := ⟨r, none, false, 0, 0, false, false⟩

/-- `nmfw_apply`: install `new`. Snapshot only if nothing is pending (the ORIGINAL is kept across re-applies).
    confirm_s > 0: pending with deadline now + confirm_s; confirm_s = 0: nothing pending, snapshot dropped. -/
def apply (s : St) (new : Nat) (confirmS : Nat) : St :=
  if confirmS > 0 then
    { s with cur := new, snap := (if s.pending then s.snap else some s.cur), pending := true,
             deadline := s.now + confirmS, confirmed := false, restored := false }
  else
    { s with cur := new, snap := none, pending := false, confirmed := false, restored := false }

inductive Ev | confirm | advance (t : Nat)

/-- `nmfw_confirm`: only works while pending (before the watchdog has fired). -/
def doConfirm (s : St) : St :=
  if s.pending then { s with pending := false, snap := none, confirmed := true } else s

/-- The watchdog: the clock moves to max(now, t); if still pending at/after the deadline the snapshot is restored. -/
def doAdvance (s : St) (t : Nat) : St :=
  let s := { s with now := max s.now t }
  if s.pending ∧ s.deadline ≤ s.now then
    { s with cur := s.snap.getD s.cur, snap := none, pending := false, restored := true }
  else s

def stepE (s : St) : Ev → St
  | .confirm => doConfirm s
  | .advance t => doAdvance s t

def run (s : St) (es : List Ev) : St := es.foldl stepE s

/-- Invariant: while pending there is a snapshot and no outcome yet; the two outcomes are exclusive. -/
def Inv (s : St) : Prop :=
  (s.pending = true → s.snap.isSome ∧ s.confirmed = false ∧ s.restored = false) ∧
  ¬ (s.confirmed = true ∧ s.restored = true)

theorem inv_init (r : Nat) : Inv (init r) := by simp [Inv, init]

theorem inv_apply (s : St) (n c : Nat) (h : Inv s) : Inv (apply s n c) := by
  unfold apply Inv at *
  by_cases hc : c > 0
  · simp only [hc, if_true]
    refine ⟨fun _ => ?_, by simp⟩
    by_cases hp : s.pending = true
    · simp [hp, h.1 hp |>.1]
    · simp [hp]
  · simp [hc]

theorem inv_confirm (s : St) (h : Inv s) : Inv (doConfirm s) := by
  unfold doConfirm Inv at *
  by_cases hp : s.pending = true
  · have := h.1 hp
    simp [hp, this.2.2]
  · simp [hp, h]

theorem inv_advance (s : St) (t : Nat) (h : Inv s) : Inv (doAdvance s t) := by
  unfold doAdvance Inv at *
  by_cases hp : s.pending = true ∧ s.deadline ≤ max s.now t
  · have := h.1 hp.1
    simp [hp.1, hp.2, this.2.1]
  · simp only [hp, if_false]
    refine ⟨fun hq => ?_, h.2⟩
    have hq' : s.pending = true := by simpa using hq
    exact h.1 hq'

theorem inv_step (s : St) (e : Ev) (h : Inv s) : Inv (stepE s e) := by
  cases e
  · exact inv_confirm s h
  · exact inv_advance s _ h

theorem inv_run (s : St) (es : List Ev) (h : Inv s) : Inv (run s es) := by
  induction es generalizing s with
  | nil => exact h
  | cons e es ih => exact ih _ (inv_step s e h)

/-- After an apply and ANY sequence of confirms / clock ticks, confirmed and restored are never both true. -/
theorem never_both (s : St) (n c : Nat) (hs : Inv s) (es : List Ev) :
    ¬ ((run (apply s n c) es).confirmed = true ∧ (run (apply s n c) es).restored = true) :=
  (inv_run _ es (inv_apply s n c hs)).2

/-- A confirm that arrives before the deadline (while pending) wins: the new ruleset stays, no rollback. -/
theorem confirm_in_time (s : St) (hp : s.pending = true) :
    (doConfirm s).cur = s.cur ∧ (doConfirm s).confirmed = true ∧ (doConfirm s).restored = s.restored := by
  simp [doConfirm, hp]

/-- No confirm: once the clock reaches the deadline the snapshot is restored (and only then). -/
theorem timeout_restores (s : St) (t : Nat) (hp : s.pending = true) (hd : s.deadline ≤ max s.now t)
    (hi : Inv s) :
    (doAdvance s t).restored = true ∧ (doAdvance s t).confirmed = false ∧ (doAdvance s t).pending = false ∧
    some (doAdvance s t).cur = some (s.snap.getD s.cur) := by
  have := (hi.1 hp)
  simp [doAdvance, hp, hd, this.2.1]

/-- Before the deadline the watchdog does nothing but move the clock. -/
theorem early_tick_noop (s : St) (t : Nat) (hd : max s.now t < s.deadline) :
    (doAdvance s t).cur = s.cur ∧ (doAdvance s t).pending = s.pending ∧ (doAdvance s t).restored = s.restored := by
  have : ¬ (s.pending = true ∧ s.deadline ≤ max s.now t) := by omega
  simp [doAdvance, this]

/-- A late confirm (after the watchdog restored) changes nothing: the old ruleset stays. -/
theorem late_confirm_noop (s : St) (hp : s.pending = false) : doConfirm s = s := by
  simp [doConfirm, hp]

/-- Apply with a timer from a clean state: new ruleset in force, previous one snapshotted, armed. -/
theorem apply_arms (s : St) (n c : Nat) (hc : 0 < c) (hp : s.pending = false) :
    (apply s n c).cur = n ∧ (apply s n c).snap = some s.cur ∧ (apply s n c).pending = true ∧
    (apply s n c).deadline = s.now + c := by
  simp [apply, hc, hp]

/-- Re-applying while unconfirmed keeps the ORIGINAL snapshot: a timeout goes back to before the first apply. -/
theorem reapply_keeps_original (s : St) (n1 n2 c1 c2 : Nat) (h1 : 0 < c1) (h2 : 0 < c2)
    (hp : s.pending = false) :
    (apply (apply s n1 c1) n2 c2).snap = some s.cur ∧ (apply (apply s n1 c1) n2 c2).cur = n2 := by
  simp [apply, h1, h2, hp]

/-- End to end: apply twice unconfirmed, then time passes: ruleset = the one before the first apply. -/
theorem double_apply_timeout (s : St) (n1 n2 c1 c2 t : Nat) (h1 : 0 < c1) (h2 : 0 < c2)
    (hp : s.pending = false) (hi : Inv s) (ht : (apply (apply s n1 c1) n2 c2).deadline ≤ max (apply (apply s n1 c1) n2 c2).now t) :
    (doAdvance (apply (apply s n1 c1) n2 c2) t).cur = s.cur := by
  have hk := reapply_keeps_original s n1 n2 c1 c2 h1 h2 hp
  have hpend : (apply (apply s n1 c1) n2 c2).pending = true := by simp [apply, h2]
  have hinv := inv_apply _ n2 c2 (inv_apply s n1 c1 hi)
  have := timeout_restores _ t hpend ht hinv
  rw [hk.1] at this
  simpa using this.2.2.2

/-! ### What the model does NOT give: the real watchdog is not atomic.
    `nmfw__watchdog` checks `nmfw__pending()` once per 100 ms; after the last check it unlinks fw.pid and restores,
    with no re-check. Model: `decide` = the final check passed, `restoreAct` = restore unconditionally. -/

structure RS where
  s : St
  committed : Bool

inductive REv | decide | confirm | restoreAct

def rstep (r : RS) : REv → RS
  | .decide => { r with committed := r.s.pending && decide (r.s.deadline ≤ r.s.now) }
  | .confirm => { r with s := doConfirm r.s }
  | .restoreAct =>
    if r.committed then { r with s := { r.s with cur := r.s.snap.getD r.s.cur, snap := none, pending := false, restored := true } }
    else r

def rrun (r : RS) (es : List REv) : RS := es.foldl rstep r

/-- Counterexample: a confirm landing between the final check and the unlink is accepted AND the rollback happens. -/
theorem race_both :
    let s0 : St := { cur := 2, snap := some 1, pending := true, deadline := 10, now := 10, confirmed := false, restored := false }
    let r := rrun ⟨s0, false⟩ [.decide, .confirm, .restoreAct]
    r.s.confirmed = true ∧ r.s.restored = true := by
  decide

end Netman.Confirm
