/-! Roaming / autoconnect policy: candidate ranking, per-network backoff, the chooser loop.
    Model of `nmbn__best`, `nmbn__penalize` and the retry loop in `nmbn__pass` (src/mods/net.h). -/
namespace Netman.Roaming

structure Cand where
  id : Nat
  prio : Int
  lastOk : Nat
  signal : Int
  backoff : Nat       -- not eligible while backoff > now
deriving Repr

/-- Ranking by (priority, last success): what `nmbn__best` compares. -/
def le2 (a b : Cand) : Prop := a.prio < b.prio ∨ (a.prio = b.prio ∧ a.lastOk ≤ b.lastOk)
/-- The ranking extended with signal as third key (the C code does not use signal). -/
def le3 (a b : Cand) : Prop :=
  a.prio < b.prio ∨ (a.prio = b.prio ∧ (a.lastOk < b.lastOk ∨ (a.lastOk = b.lastOk ∧ a.signal ≤ b.signal)))

theorem le2_refl (a : Cand) : le2 a a := by unfold le2; omega
theorem le2_trans (a b c : Cand) : le2 a b → le2 b c → le2 a c := by unfold le2; omega
theorem le2_total (a b : Cand) : le2 a b ∨ le2 b a := by unfold le2; omega
theorem le3_refl (a : Cand) : le3 a a := by unfold le3; omega
theorem le3_trans (a b c : Cand) : le3 a b → le3 b c → le3 a c := by unfold le3; omega
theorem le3_total (a b : Cand) : le3 a b ∨ le3 b a := by unfold le3; omega
/-- le3 refines le2. -/
theorem le3_of_le2_lt (a b : Cand) (h : ¬ le2 b a) : le3 a b := by unfold le2 le3 at *; omega

/-- The C replacement condition: `s->priority > b->priority || (== && s->last_ok > b->last_ok)`. -/
def better (s b : Cand) : Prop := s.prio > b.prio ∨ (s.prio = b.prio ∧ s.lastOk > b.lastOk)

theorem better_iff (s b : Cand) : better s b ↔ ¬ le2 s b := by unfold better le2; omega

instance (s b : Cand) : Decidable (better s b) := by unfold better; infer_instance

def eligible (now : Nat) (c : Cand) : Prop := c.backoff ≤ now
instance (now : Nat) (c : Cand) : Decidable (eligible now c) := by unfold eligible; infer_instance

/-- `nmbn__best`: skip candidates in backoff, earlier candidate wins ties (strict `better` to replace). -/
def best (now : Nat) : List Cand → Option Cand
  | [] => none
  | s :: t =>
    match best now t with
    | none => if eligible now s then some s else none
    | some b => if eligible now s ∧ ¬ better b s then some s else some b

/-- Never selects a network in backoff. -/
theorem best_mem (now : Nat) (l : List Cand) (b : Cand) (h : best now l = some b) :
    b ∈ l ∧ eligible now b := by
  induction l generalizing b with
  | nil => simp [best] at h
  | cons s t ih =>
    unfold best at h
    cases hb : best now t with
    | none =>
      rw [hb] at h; dsimp only at h
      by_cases he : eligible now s
      · rw [if_pos he] at h; cases h; exact ⟨by simp, he⟩
      · rw [if_neg he] at h; simp at h
    | some b' =>
      rw [hb] at h; dsimp only at h
      by_cases hc : eligible now s ∧ ¬ better b' s
      · rw [if_pos hc] at h; rw [← Option.some.inj h]; exact ⟨by simp, hc.1⟩
      · rw [if_neg hc] at h; rw [← Option.some.inj h]
        obtain ⟨h1, h2⟩ := ih b' hb; exact ⟨List.mem_cons_of_mem _ h1, h2⟩

/-- If any candidate is eligible, one is chosen (and only then). -/
theorem best_isSome (now : Nat) (l : List Cand) :
    (best now l).isSome ↔ ∃ s ∈ l, eligible now s := by
  induction l with
  | nil => simp [best]
  | cons s t ih =>
    unfold best
    split
    · rename_i hn
      rw [hn] at ih; simp at ih
      split <;> simp_all
    · rename_i b hb
      rw [hb] at ih; simp at ih
      split <;> simp_all

/-- The chosen candidate is a maximum of the ranking among eligible ones. -/
theorem best_max (now : Nat) (l : List Cand) (b : Cand) (h : best now l = some b) :
    ∀ s ∈ l, eligible now s → le2 s b := by
  induction l generalizing b with
  | nil => simp [best] at h
  | cons s t ih =>
    intro x hx hxe
    unfold best at h
    cases hb : best now t with
    | none =>
      rw [hb] at h; dsimp only at h
      by_cases he : eligible now s
      · rw [if_pos he] at h; rw [← Option.some.inj h]
        have hno : ∀ y ∈ t, ¬ eligible now y := by
          intro y hy hye
          have := (best_isSome now t).2 ⟨y, hy, hye⟩; rw [hb] at this; simp at this
        rcases List.mem_cons.1 hx with e | hx
        · rw [e]; exact le2_refl _
        · exact absurd hxe (hno x hx)
      · rw [if_neg he] at h; simp at h
    | some b' =>
      rw [hb] at h; dsimp only at h
      by_cases hc : eligible now s ∧ ¬ better b' s
      · rw [if_pos hc] at h; rw [← Option.some.inj h]
        rcases List.mem_cons.1 hx with e | hx
        · rw [e]; exact le2_refl _
        · have h1 := ih b' hb x hx hxe
          have h3 : le2 b' s := by have := hc.2; unfold better le2 at *; omega
          exact le2_trans x b' s h1 h3
      · rw [if_neg hc] at h; rw [← Option.some.inj h]
        rcases List.mem_cons.1 hx with e | hx
        · rw [e] at hxe ⊢
          have : better b' s := by
            by_cases hh : better b' s
            · exact hh
            · exact absurd ⟨hxe, hh⟩ hc
          unfold better le2 at *; omega
        · exact ih _ hb x hx hxe

/-! ### Backoff (`nmbn__penalize`): 1, 2, 4 s, then 300 s -/

def delay (fails : Nat) : Nat := if fails < 3 then 2 ^ fails else 300

theorem delay_pos (n : Nat) : 1 ≤ delay n := by
  unfold delay; split
  · exact Nat.one_le_two_pow
  · omega
theorem delay_le (n : Nat) : delay n ≤ 300 := by
  unfold delay; split
  · have : n = 0 ∨ n = 1 ∨ n = 2 := by omega
    rcases this with rfl | rfl | rfl <;> decide
  · omega
/-- Monotone in the number of consecutive failures. -/
theorem delay_mono (m n : Nat) (h : m ≤ n) : delay m ≤ delay n := by
  unfold delay; split <;> split
  · exact Nat.pow_le_pow_right (by omega) h
  · rename_i h1 _
    have : m = 0 ∨ m = 1 ∨ m = 2 := by omega
    rcases this with rfl | rfl | rfl <;> decide
  · omega
  · omega

/-- After a penalty, the slot is in backoff at `now`, for at most 300 s. -/
def penalize (now fails : Nat) (c : Cand) : Cand := { c with backoff := now + delay fails }

theorem penalize_ineligible (now fails : Nat) (c : Cand) : ¬ eligible now (penalize now fails c) := by
  unfold eligible penalize; have := delay_pos fails; simp; omega
theorem penalize_bounded (now fails : Nat) (c : Cand) : (penalize now fails c).backoff ≤ now + 300 := by
  unfold penalize; have := delay_le fails; simp; omega

/-! ### The chooser loop (time frozen; fuel = number of slots + 1 as in `tries <= ns`) -/

inductive Res | connected (id : Nat) | none | outOfFuel
deriving DecidableEq

/-- `ok id` = whether connecting to that network succeeds. A failure penalizes it and falls through. -/
def countElig (now : Nat) : List Cand → Nat
  | [] => 0
  | c :: t => (if eligible now c then 1 else 0) + countElig now t

def pen (now : Nat) (fails : Nat → Nat) (b : Cand) (c : Cand) : Cand :=
  if c.id = b.id then penalize now (fails c.id) c else c

theorem pen_elig (now : Nat) (fails : Nat → Nat) (b c : Cand) (h : eligible now (pen now fails b c)) :
    eligible now c ∧ c.id ≠ b.id := by
  unfold pen at h
  split at h
  · exact absurd h (penalize_ineligible _ _ _)
  · rename_i hn; exact ⟨h, hn⟩

theorem count_le (now : Nat) (fails : Nat → Nat) (b : Cand) (l : List Cand) :
    countElig now (l.map (pen now fails b)) ≤ countElig now l := by
  induction l with
  | nil => simp [countElig]
  | cons c t ih =>
    simp only [List.map, countElig]
    by_cases h : eligible now (pen now fails b c)
    · have := (pen_elig now fails b c h).1; simp [h, this]; omega
    · by_cases h2 : eligible now c <;> simp [h, h2] <;> omega

theorem count_lt (now : Nat) (fails : Nat → Nat) (b : Cand) (l : List Cand)
    (hm : b ∈ l) (he : eligible now b) :
    countElig now (l.map (pen now fails b)) < countElig now l := by
  induction l with
  | nil => simp at hm
  | cons c t ih =>
    simp only [List.map, countElig]
    rcases List.mem_cons.1 hm with e | hm
    · subst e
      have hp : ¬ eligible now (pen now fails b b) := by
        unfold pen; simp; exact penalize_ineligible _ _ _
      have := count_le now fails b t
      simp [hp, he]; omega
    · have := ih hm
      by_cases h : eligible now (pen now fails b c)
      · have := (pen_elig now fails b c h).1; simp [h, this]; omega
      · by_cases h2 : eligible now c <;> simp [h, h2] <;> omega

def run (now : Nat) (ok : Nat → Bool) (fails : Nat → Nat) : Nat → List Cand → Res
  | 0, _ => .outOfFuel
  | n + 1, l =>
    match best now l with
    | none => .none
    | some b =>
      if ok b.id then .connected b.id
      else run now ok fails n (l.map (pen now fails b))

/-- Termination: with fuel > #eligible the loop never runs out; it connects or reports that
    no eligible candidate is left (each failed attempt strictly shrinks the eligible set). -/
theorem run_terminates (now : Nat) (ok : Nat → Bool) (fails : Nat → Nat) :
    ∀ (n : Nat) (l : List Cand), countElig now l < n → run now ok fails n l ≠ .outOfFuel := by
  intro n
  induction n with
  | zero => intro l h; omega
  | succ n ih =>
    intro l h
    unfold run
    split
    · simp
    · rename_i b hb
      split
      · simp
      · have hm := best_mem now l b hb
        exact ih _ (by have := count_lt now fails b l hm.1 hm.2; omega)

/-- A connection is only ever made to a network that was eligible (not in backoff) at that time. -/
theorem run_connected_eligible (now : Nat) (ok : Nat → Bool) (fails : Nat → Nat) :
    ∀ (n : Nat) (l : List Cand) (i : Nat), run now ok fails n l = .connected i →
      ∃ c ∈ l, c.id = i ∧ eligible now c := by
  intro n
  induction n with
  | zero => intro l i h; simp [run] at h
  | succ n ih =>
    intro l i h
    unfold run at h
    split at h
    · simp at h
    · rename_i b hb
      split at h
      · cases h; exact ⟨b, (best_mem now l b hb).1, rfl, (best_mem now l b hb).2⟩
      · obtain ⟨c', hc', hid, hel⟩ := ih _ i h
        obtain ⟨c, hc, rfl⟩ := List.mem_map.1 hc'
        have := pen_elig now fails b c hel
        refine ⟨c, hc, ?_, this.1⟩
        unfold pen at hid; split at hid <;> simpa [penalize] using hid

/-- If something is eligible and connecting works, the best candidate is connected on the first try. -/
theorem run_first_try (now : Nat) (ok : Nat → Bool) (fails : Nat → Nat) (n : Nat) (l : List Cand)
    (hex : ∃ s ∈ l, eligible now s) :
    ∃ b, best now l = some b ∧ b ∈ l ∧ eligible now b ∧ (∀ s ∈ l, eligible now s → le2 s b) ∧
      (ok b.id = true → run now ok fails (n + 1) l = .connected b.id) := by
  have hs := (best_isSome now l).2 hex
  cases hb : best now l with
  | none => rw [hb] at hs; simp at hs
  | some b =>
    refine ⟨b, rfl, (best_mem now l b hb).1, (best_mem now l b hb).2, best_max now l b hb, ?_⟩
    intro hok; simp [run, hb, hok]

end Netman.Roaming
