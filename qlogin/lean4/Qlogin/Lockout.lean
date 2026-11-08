/-! Login lockout. Model of `ql_lock_ms` and the failure counter in `ql_attempt` (src/mods/greeter.h). -/
namespace Qlogin.Lockout

/-- `ql_lock_ms` in units: failure n (1-based) waits 2^(n-1) units, capped at 30; 0 failures = 0. -/
def units (f : Nat) : Nat :=
  if f < 1 then 0 else min (if f > 5 then 30 else 2 ^ (f - 1)) 30

/-- `ql_lock_ms(failures, unit_ms)`. -/
def lockMs (f unit : Nat) : Nat := units f * unit

theorem units_zero : units 0 = 0 := by simp [units]
theorem units_values : units 1 = 1 ∧ units 2 = 2 ∧ units 3 = 4 ∧ units 4 = 8 ∧ units 5 = 16 ∧ units 6 = 30 := by
  simp [units]

/-- Bounded by the cap. -/
theorem units_le_cap (f : Nat) : units f ≤ 30 := by
  unfold units; split <;> omega

theorem lockMs_le_cap (f unit : Nat) : lockMs f unit ≤ 30 * unit := by
  unfold lockMs; exact Nat.mul_le_mul_right _ (units_le_cap f)

/-- Monotone non-decreasing in the number of failures. -/
theorem units_mono (a b : Nat) (h : a ≤ b) : units a ≤ units b := by
  by_cases hb : 6 ≤ b
  · have : units b = 30 := by unfold units; split <;> (try split) <;> omega
    have := units_le_cap a
    omega
  · have hb' : b = 0 ∨ b = 1 ∨ b = 2 ∨ b = 3 ∨ b = 4 ∨ b = 5 := by omega
    have ha' : a = 0 ∨ a = 1 ∨ a = 2 ∨ a = 3 ∨ a = 4 ∨ a = 5 := by omega
    rcases hb' with rfl | rfl | rfl | rfl | rfl | rfl <;>
    rcases ha' with rfl | rfl | rfl | rfl | rfl | rfl <;> first | omega | (simp [units])

theorem lockMs_mono (a b unit : Nat) (h : a ≤ b) : lockMs a unit ≤ lockMs b unit := by
  unfold lockMs; exact Nat.mul_le_mul_right _ (units_mono a b h)

/-- The counter in `ql_attempt`: a failure does `++*failures` and sleeps `lockMs`; a success sets it to 0. -/
inductive Ev | fail | ok

/-- (new counter, sleep in units after this event). -/
def step (f : Nat) : Ev → Nat × Nat
  | .fail => (f + 1, units (f + 1))
  | .ok => (0, 0)

/-- Success resets: the counter is 0 and the next failure waits the minimum (1 unit) again. -/
theorem success_resets (f : Nat) : (step f .ok).1 = 0 ∧ units (step (step f .ok).1 .fail).1 = 1 := by
  simp [step, units]

theorem fail_always_waits (f : Nat) : 1 ≤ (step f .fail).2 := by
  simp only [step]
  unfold units
  have := Nat.two_pow_pos f
  have h2 : f + 1 - 1 = f := by omega
  simp [h2]; split <;> omega

def run (f : Nat) : List Ev → Nat × List Nat
  | [] => (f, [])
  | e :: es => let r := step f e; let q := run r.1 es; (q.1, r.2 :: q.2)

/-- n consecutive failures from counter f leave the counter at f + n. -/
theorem counter_counts (n f : Nat) : (run f (List.replicate n .fail)).1 = f + n := by
  induction n generalizing f with
  | zero => simp [run]
  | succ k ih => rw [List.replicate_succ]; simp only [run, step]; rw [ih]; omega

/-- The wait after the n-th consecutive failure from a clean counter is `units n`, so (units_mono) the waits
    along a run of failures are non-decreasing. -/
theorem nth_fail_wait (n : Nat) : (step n .fail).2 = units (n + 1) := by simp [step]

theorem waits_nondecreasing (n : Nat) : (step n .fail).2 ≤ (step (n + 1) .fail).2 := by
  rw [nth_fail_wait, nth_fail_wait]; exact units_mono _ _ (by omega)

/-- No sleep ever exceeds the cap, whatever the event sequence. -/
theorem run_waits_capped (es : List Ev) (f : Nat) : ∀ w ∈ (run f es).2, w ≤ 30 := by
  induction es generalizing f with
  | nil => simp [run]
  | cons e es ih =>
    intro w hw
    simp only [run, List.mem_cons] at hw
    rcases hw with rfl | hw
    · cases e <;> simp [step, units_le_cap]
    · exact ih _ w hw

end Qlogin.Lockout
