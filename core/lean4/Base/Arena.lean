/-
  core_arena_alloc.h with its default backend (ARENA_BACKEND_FLAT): arena_alloc, arena_snapshot, arena_rewind,
  arena_reset, arena_realloc, arena_calloc, arena_decommit and arena_alloc_aligned (its rounding is Holyc.alignC, proved in
  holyc/lean4/Holyc/Bits.lean). The model is one region:
  FLAT reserves 1 GiB for the first region and the differential test stays inside it, so the C never chains a
  second. Offsets are bytes from the region's data[0]; the C counts u64 granules.
-/
import Holyc.Bits

namespace Base.Arena

/-- The granules arena_alloc takes for `n` bytes, rounded up. -/
def granules (n : Nat) : Nat := (n + 7) / 8

/-- The C computes it as `n / 8 + (n % 8 != 0)`, which cannot wrap around near SIZE_MAX the way `(n + 7) / 8`
    does; the two are the same number. -/
theorem granules_c (n : Nat) : n / 8 + (if n % 8 = 0 then 0 else 1) = granules n := by
  unfold granules; split <;> omega

theorem granules_mono {m n : Nat} (h : m ≤ n) : granules m ≤ granules n := by unfold granules; omega

/-- One region: `count` granules handed out of `cap`; `high` is the C's lazily kept high-water mark (the most
    granules ever handed out is `max high count`). -/
structure St where
  count : Nat
  cap : Nat
  high : Nat := 0
deriving Repr, DecidableEq

/-- arena_alloc inside the region: the block's offset and the state after it; `none` when it does not fit (the C
    then chains a new region, which the model leaves out). -/
def alloc (s : St) (n : Nat) : Option (Nat × St) :=
  if s.count + granules n ≤ s.cap then some (8 * s.count, { s with count := s.count + granules n }) else none

/-- arena_snapshot keeps the count, arena_rewind puts it back and keeps the high-water mark, arena_reset empties
    the region. -/
def snapshot (s : St) : Nat := s.count
def rewind (s : St) (m : Nat) : St := { s with count := m, high := max s.high s.count }
def reset (s : St) : St := rewind s 0

/-- Allocations in a row: each block's (offset, size) and the state after the last. -/
def run : St → List Nat → Option (List (Nat × Nat) × St)
  | s, [] => some ([], s)
  | s, n :: ns =>
    match alloc s n with
    | none => none
    | some (o, s1) =>
      match run s1 ns with
      | none => none
      | some (bs, s2) => some ((o, n) :: bs, s2)

theorem granules_cover (n : Nat) : n ≤ 8 * granules n := by unfold granules; omega

theorem alloc_spec {s s' : St} {n o : Nat} (h : alloc s n = some (o, s')) :
    o = 8 * s.count ∧ s' = { s with count := s.count + granules n } ∧ s.count + granules n ≤ s.cap := by
  unfold alloc at h
  split at h
  · simp only [Option.some.injEq, Prod.mk.injEq] at h
    exact ⟨h.1.symm, h.2.symm, by assumption⟩
  · cases h

/-- Every block starts on an 8-byte boundary of the region (data[0] is 8-aligned in the C). -/
theorem alloc_aligned {s s' : St} {n o : Nat} (h : alloc s n = some (o, s')) : o % 8 = 0 := by
  obtain ⟨rfl, -, -⟩ := alloc_spec h
  omega

/-- The arena never runs past its region: all n bytes of a block lie inside it. -/
theorem alloc_in_region {s s' : St} {n o : Nat} (h : alloc s n = some (o, s')) : o + n ≤ 8 * s.cap := by
  obtain ⟨rfl, -, hle⟩ := alloc_spec h
  have := granules_cover n
  omega

theorem run_cap {s s' : St} {ns : List Nat} {bs : List (Nat × Nat)} (h : run s ns = some (bs, s')) :
    s'.cap = s.cap ∧ s.count ≤ s'.count ∧ s'.high = s.high := by
  induction ns generalizing s bs with
  | nil =>
    simp only [run, Option.some.injEq, Prod.mk.injEq] at h
    obtain ⟨-, rfl⟩ := h
    exact ⟨rfl, Nat.le_refl _, rfl⟩
  | cons n ns ih =>
    simp only [run] at h
    split at h
    · cases h
    · rename_i o s1 h1
      split at h
      · cases h
      · rename_i bs' s2 h2
        simp only [Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨-, rfl⟩ := h
        obtain ⟨-, rfl, -⟩ := alloc_spec h1
        obtain ⟨hc, hl, hh⟩ := ih h2
        exact ⟨hc, by simp only at hl; omega, hh⟩

/-- A run of allocations hands out blocks that lie between the counts before and after it, each ending before the
    next begins: no two blocks overlap. -/
theorem run_disjoint {s s' : St} {ns : List Nat} {bs : List (Nat × Nat)} (h : run s ns = some (bs, s')) :
    (∀ b ∈ bs, 8 * s.count ≤ b.1 ∧ b.1 + b.2 ≤ 8 * s'.count) ∧ bs.Pairwise (fun b c => b.1 + b.2 ≤ c.1) := by
  induction ns generalizing s bs with
  | nil =>
    simp only [run, Option.some.injEq, Prod.mk.injEq] at h
    obtain ⟨rfl, rfl⟩ := h
    simp
  | cons n ns ih =>
    simp only [run] at h
    split at h
    · cases h
    · rename_i o s1 h1
      split at h
      · cases h
      · rename_i bs' s2 h2
        simp only [Option.some.injEq, Prod.mk.injEq] at h
        obtain ⟨rfl, rfl⟩ := h
        obtain ⟨rfl, rfl, -⟩ := alloc_spec h1
        obtain ⟨hin, hpw⟩ := ih h2
        have hcov := granules_cover n
        have hmono := (run_cap h2).2.1
        simp only at hmono
        refine ⟨?_, List.Pairwise.cons ?_ hpw⟩
        · intro b hb
          rcases List.mem_cons.mp hb with rfl | hb
          · simp only
            omega
          · have := hin b hb
            simp only at this
            omega
        · intro c hc
          have := (hin c hc).1
          simp only at this ⊢
          omega

/-- Scope exit is the free: a snapshot taken before a run of allocations and rewound to after it gives back the
    position and capacity before the run ... -/
theorem rewind_run {s s' : St} {ns : List Nat} {bs : List (Nat × Nat)} (h : run s ns = some (bs, s')) :
    (rewind s' (snapshot s)).count = s.count ∧ (rewind s' (snapshot s)).cap = s.cap := by
  obtain ⟨hc, -⟩ := run_cap h
  exact ⟨rfl, hc⟩

/-- ... so the next allocation lands where the run's first one did. -/
theorem alloc_same_place {s t : St} (hc : s.count = t.count) (hp : s.cap = t.cap) (n : Nat) :
    (alloc s n).map Prod.fst = (alloc t n).map Prod.fst := by
  unfold alloc; rw [hc, hp]; split <;> rfl

theorem reset_empty (s : St) : (reset s).count = 0 ∧ (reset s).cap = s.cap := ⟨rfl, rfl⟩

/-- arena_realloc of the block at byte `o` from `oldsz` to `newsz` bytes: kept when it shrinks, grown where it is
    when it is the region's last block and the region has room, moved to a fresh block otherwise. -/
def realloc (s : St) (o oldsz newsz : Nat) : Option (Nat × St) :=
  if newsz ≤ oldsz then some (o, s)
  else if o + 8 * granules oldsz = 8 * s.count ∧ s.count + (granules newsz - granules oldsz) ≤ s.cap then
    some (o, { s with count := s.count + (granules newsz - granules oldsz) })
  else alloc s newsz

/-- Growing in place is safe: the block keeps its start, its new bytes begin at the old top of the region (where
    nothing is handed out), and all `newsz` bytes stay inside the region. -/
theorem realloc_in_place {s : St} {o oldsz newsz : Nat} (hgrow : oldsz < newsz)
    (htop : o + 8 * granules oldsz = 8 * s.count)
    (hroom : s.count + (granules newsz - granules oldsz) ≤ s.cap) :
    realloc s o oldsz newsz = some (o, { s with count := s.count + (granules newsz - granules oldsz) }) ∧
    o + newsz ≤ 8 * (s.count + (granules newsz - granules oldsz)) := by
  refine ⟨by simp [realloc, Nat.not_le.mpr hgrow, htop, hroom], ?_⟩
  have := granules_cover newsz
  have := granules_mono (Nat.le_of_lt hgrow)
  omega

/-- Otherwise a grown block is a fresh allocation at the top: it overlaps no block handed out before. -/
theorem realloc_moved {s s' : St} {o oldsz newsz o' : Nat} (hgrow : oldsz < newsz)
    (hnot : ¬ (o + 8 * granules oldsz = 8 * s.count ∧ s.count + (granules newsz - granules oldsz) ≤ s.cap))
    (h : realloc s o oldsz newsz = some (o', s')) : alloc s newsz = some (o', s') ∧ o' = 8 * s.count := by
  simp only [realloc, Nat.not_le.mpr hgrow, if_false, hnot] at h
  exact ⟨h, (alloc_spec h).1⟩

/-! ### Zeroing: arena_calloc and arena_decommit

  `dirty b` says byte b of the region may be nonzero. Users write only inside blocks, below `8 * count`, and the
  region starts zero from mmap; `Clean` is the invariant the C relies on to skip zeroing. -/

def Clean (s : St) (dirty : Nat → Bool) : Prop := ∀ b, dirty b = true → b < 8 * max s.high s.count

theorem clean_start (cap : Nat) : Clean ⟨0, cap, 0⟩ (fun _ => false) := by intro b h; cases h

/-- A user write inside the blocks handed out keeps the invariant. -/
theorem clean_write {s : St} {dirty : Nat → Bool} {w : Nat} (h : Clean s dirty) (hw : w < 8 * s.count) :
    Clean s (fun b => dirty b || b == w) := by
  intro b hb
  rcases Bool.or_eq_true _ _ ▸ hb with hb | hb
  · exact h b hb
  · have := beq_iff_eq.mp hb; omega

theorem clean_alloc {s s' : St} {dirty : Nat → Bool} {n o : Nat} (h : Clean s dirty)
    (ha : alloc s n = some (o, s')) : Clean s' dirty := by
  obtain ⟨-, rfl, -⟩ := alloc_spec ha
  intro b hb; have := h b hb; simp only; omega

theorem clean_rewind {s : St} {dirty : Nat → Bool} (h : Clean s dirty) (m : Nat) : Clean (rewind s m) dirty := by
  intro b hb; have := h b hb; simp only [rewind]; omega

/-- arena_calloc: the bytes the C clears, `[o, o + z)` with z the part of the rounded block below `8 * high`. -/
def callocZeroed (s : St) (o n : Nat) : Nat :=
  if 8 * s.high > o then min (8 * s.high - o) (8 * granules n) else 0

/-- The optimized arena_calloc returns the same memory as a full memset: every byte of the block is zero, though
    the C clears only the part below the high-water mark. -/
theorem calloc_zero {s s' : St} {dirty : Nat → Bool} {n o : Nat} (h : Clean s dirty)
    (ha : alloc s n = some (o, s')) :
    ∀ b, o ≤ b → b < o + n → (dirty b && !(o ≤ b && b < o + callocZeroed s o n)) = false := by
  obtain ⟨rfl, -, -⟩ := alloc_spec ha
  intro b hlo hhi
  have hcov := granules_cover n
  cases hd : dirty b
  · simp
  · have := h b hd
    simp only [callocZeroed, Bool.true_and, Bool.not_eq_false', Bool.and_eq_true, decide_eq_true_eq]
    split <;> omega

/-- arena_decommit: from a page boundary `f` at or above the position, every byte reads zero again and the
    high-water mark drops to f; when f is not below the top nothing changes. The invariant holds after it. -/
def decommit (s : St) (f : Nat) : St := if f < 8 * max s.high s.count then { s with high := f / 8 } else s

theorem clean_decommit {s : St} {dirty : Nat → Bool} {f : Nat} (h : Clean s dirty) (hf : 8 * s.count ≤ f)
    (h8 : f % 8 = 0) :
    Clean (decommit s f) (if f < 8 * max s.high s.count then fun b => dirty b && b < f else dirty) := by
  unfold decommit
  split
  · intro b hb
    simp only [Bool.and_eq_true, decide_eq_true_eq] at hb
    simp only
    omega
  · exact h

end Base.Arena
