/-! Roaming hysteresis. Model of `netman_score_should_leave` (src/mods/scoring.h) and a two-AP chooser built on it.
    The C function is now called by the roaming chooser (`netman__may_switch` in src/mods/roaming.h, when both slots
    have a measured score), so the two-AP chooser below mirrors real use. C margins: NETMAN_HYST_DEAD = 6 (cand >=
    cur + 6, i.e. cand > cur + 5) below 20, NETMAN_HYST = 15 for 20..69, never above 69: exactly the numbers proven. -/
namespace Netman.Hysteresis

/-- `netman_score_should_leave(cur, cand)`: Q_WEAK = 20, Q_GOOD = 70, HYST = 15. -/
def shouldLeave (cur cand : Int) : Bool :=
  if cur < 20 then decide (cand > cur + 5)
  else if cur ≥ 70 then false
  else decide (cand ≥ cur + 15)

/-- Leaving needs a real gap: > 5 from a dead network, >= 15 from any other. -/
theorem leave_gap (cur cand : Int) (h : shouldLeave cur cand = true) :
    cand ≥ cur + 6 ∧ (20 ≤ cur → cand ≥ cur + 15 ∧ cur < 70) := by
  unfold shouldLeave at h
  by_cases h1 : cur < 20 <;> by_cases h2 : cur ≥ 70 <;> simp [h1, h2] at h <;> omega

/-- Scores closer than the margin never trigger a switch. -/
theorem close_no_leave (cur cand : Int) (h : cand < cur + 6) : shouldLeave cur cand = false := by
  have := leave_gap cur cand
  cases hh : shouldLeave cur cand
  · rfl
  · have := this hh; omega

theorem close_no_leave_ok (cur cand : Int) (hcur : 20 ≤ cur) (h : cand < cur + 15) : shouldLeave cur cand = false := by
  have := leave_gap cur cand
  cases hh : shouldLeave cur cand
  · rfl
  · have := this hh; omega

/-- A good network (>= 70) is never left. -/
theorem good_never_leaves (cur cand : Int) (h : 70 ≤ cur) : shouldLeave cur cand = false := by
  have h1 : ¬ cur < 20 := by omega
  simp [shouldLeave, h1, h]

/-- No ping-pong in one observation: a and b can not both want to leave for the other. -/
theorem no_pingpong (a b : Int) : ¬ (shouldLeave a b = true ∧ shouldLeave b a = true) := by
  intro ⟨h1, h2⟩
  have := leave_gap a b h1; have := leave_gap b a h2; omega

/-- Two-AP chooser: `onA` = currently on A; given the observed scores, stay or flip. -/
def step (sa sb : Int) (onA : Bool) : Bool :=
  if onA then !(shouldLeave sa sb) else shouldLeave sb sa

/-- With unchanged scores the chooser flips at most once: it is a fixed point after one step. -/
theorem step_idem (sa sb : Int) (c : Bool) : step sa sb (step sa sb c) = step sa sb c := by
  have np := no_pingpong sa sb
  unfold step
  cases c <;> cases h1 : shouldLeave sa sb <;> cases h2 : shouldLeave sb sa <;> simp_all

/-- n rounds on frozen scores. -/
def iter (sa sb : Int) : Nat → Bool → Bool
  | 0, c => c
  | n + 1, c => step sa sb (iter sa sb n c)

/-- Iterating on frozen scores: from the first round on nothing changes (at most ONE flip, ever). -/
theorem run_stable (sa sb : Int) (c : Bool) (n : Nat) : iter sa sb (n + 1) c = step sa sb c := by
  induction n with
  | zero => rfl
  | succ k ih => show step sa sb (iter sa sb (k + 1) c) = _; rw [ih]; exact step_idem sa sb c

/-- Scores within 5 of each other (any range): never any flip, from either side. -/
theorem close_scores_stay (sa sb : Int) (h : sa - sb < 6 ∧ sb - sa < 6) (c : Bool) : step sa sb c = c := by
  have h1 := close_no_leave sa sb (by omega)
  have h2 := close_no_leave sb sa (by omega)
  unfold step; cases c <;> simp [h1, h2]

/-- Every flip moves to an AP scoring strictly (by >= 6) higher than the one left. -/
theorem flip_improves (sa sb : Int) (c : Bool) (h : step sa sb c ≠ c) :
    (c = true ∧ sb ≥ sa + 6) ∨ (c = false ∧ sa ≥ sb + 6) := by
  unfold step at h
  cases c
  · simp at h ⊢; have := leave_gap sb sa h; omega
  · simp at h ⊢; have := leave_gap sa sb h; omega

end Netman.Hysteresis
