/-
  core_arena_alloc.h with its default backend (ARENA_BACKEND_FLAT): arena_alloc, arena_snapshot, arena_rewind,
  arena_reset and arena_alloc_aligned, plus the rounding pool_align_up shares with it. The model is one region:
  FLAT reserves 1 GiB for the first region and the differential test stays inside it, so the C never chains a
  second. Offsets are bytes from the region's data[0]; the C counts u64 granules.
-/
namespace Base.Arena

/-- `(size_bytes + sizeof(u64) - 1) / sizeof(u64)`: the granules arena_alloc takes for `n` bytes. -/
def granules (n : Nat) : Nat := (n + 7) / 8

/-- One region: `count` granules handed out of `cap`. -/
structure St where
  count : Nat
  cap : Nat
deriving Repr, DecidableEq

/-- arena_alloc inside the region: the block's offset and the state after it; `none` when it does not fit (the C
    then chains a new region, which the model leaves out). -/
def alloc (s : St) (n : Nat) : Option (Nat × St) :=
  if s.count + granules n ≤ s.cap then some (8 * s.count, ⟨s.count + granules n, s.cap⟩) else none

/-- arena_snapshot keeps the count, arena_rewind puts it back, arena_reset empties the region. -/
def snapshot (s : St) : Nat := s.count
def rewind (s : St) (m : Nat) : St := ⟨m, s.cap⟩
def reset (s : St) : St := ⟨0, s.cap⟩

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
    o = 8 * s.count ∧ s' = ⟨s.count + granules n, s.cap⟩ ∧ s.count + granules n ≤ s.cap := by
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
    s'.cap = s.cap ∧ s.count ≤ s'.count := by
  induction ns generalizing s bs with
  | nil =>
    simp only [run, Option.some.injEq, Prod.mk.injEq] at h
    obtain ⟨-, rfl⟩ := h
    exact ⟨rfl, Nat.le_refl _⟩
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
        obtain ⟨hc, hl⟩ := ih h2
        exact ⟨hc, by simp only at hl; omega⟩

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
        have hmono := (run_cap h2).2
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
    state before the run, so the next allocation lands where the run's first one did. -/
theorem rewind_run {s s' : St} {ns : List Nat} {bs : List (Nat × Nat)} (h : run s ns = some (bs, s')) :
    rewind s' (snapshot s) = s := by
  obtain ⟨hc, -⟩ := run_cap h
  cases s
  cases s'
  simp_all [rewind, snapshot]

theorem reset_empty (s : St) : (reset s).count = 0 ∧ (reset s).cap = s.cap := ⟨rfl, rfl⟩

/-- `x & ~(2^k - 1)` on a 64-bit size_t, the mask arena_alloc_aligned and pool_align_up apply, clears the low k
    bits: it is `x / 2^k * 2^k`. -/
theorem and_high_mask (x k : Nat) (hk : k ≤ 64) (hx : x < 2 ^ 64) :
    x &&& (2 ^ 64 - 2 ^ k) = x / 2 ^ k * 2 ^ k := by
  have hmask : 2 ^ 64 - 2 ^ k = (2 ^ (64 - k) - 1) <<< k := by
    rw [Nat.shiftLeft_eq, Nat.sub_mul, Nat.one_mul, ← Nat.pow_add, Nat.sub_add_cancel hk]
  have hdiv : x / 2 ^ k * 2 ^ k = (x >>> k) <<< k := by
    rw [Nat.shiftLeft_eq, Nat.shiftRight_eq_div_pow]
  rw [hmask, hdiv]
  apply Nat.eq_of_testBit_eq
  intro i
  rw [Nat.testBit_and, Nat.testBit_shiftLeft, Nat.testBit_shiftLeft, Nat.testBit_shiftRight,
    Nat.testBit_two_pow_sub_one]
  by_cases hi : i ≥ k
  · rw [show k + (i - k) = i by omega]
    by_cases h64 : i < 64
    · simp [hi, show i - k < 64 - k by omega]
    · have : x.testBit i = false :=
        Nat.testBit_lt_two_pow (Nat.lt_of_lt_of_le hx (Nat.pow_le_pow_right (by decide) (by omega)))
      simp [this]
  · simp [hi]

/-- The arithmetic the C rounding stands for. -/
def alignUp (x a : Nat) : Nat := (x + a - 1) / a * a

/-- arena_alloc_aligned's and pool_align_up's `(x + align - 1) & ~(align - 1)` for align = 2^k. -/
def alignC (x k : Nat) : Nat := (x + 2 ^ k - 1) &&& (2 ^ 64 - 2 ^ k)

theorem alignC_eq (x k : Nat) (hk : k ≤ 64) (hx : x + 2 ^ k - 1 < 2 ^ 64) : alignC x k = alignUp x (2 ^ k) :=
  and_high_mask _ _ hk hx

/-- The rounded address is a multiple of the alignment, not before x, and `n` bytes from it stay inside the
    `n + a - 1` bytes arena_alloc_aligned asks arena_alloc for. -/
theorem alignUp_spec (x a n : Nat) (ha : 0 < a) :
    alignUp x a % a = 0 ∧ x ≤ alignUp x a ∧ alignUp x a + n ≤ x + (n + a - 1) := by
  unfold alignUp
  have h1 := Nat.div_add_mod' (x + a - 1) a
  have h2 := Nat.mod_lt (x + a - 1) ha
  refine ⟨Nat.mul_mod_left _ _, ?_, ?_⟩ <;>
  · generalize (x + a - 1) / a * a = p at *
    generalize (x + a - 1) % a = r at *
    omega

end Base.Arena
