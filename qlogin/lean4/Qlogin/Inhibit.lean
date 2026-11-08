/-! Inhibitors and the pending power action. Model of `l1_inh_mask`, `l1_inh_has`, `m_inhibit`, `l1_power` and
    `l1_tick_action` (src/mods/login1.h). A kind is an index 0..7 into `l1_what_names`
    (0 shutdown, 1 sleep, 2 idle, ...). The D-Bus string is the names of the mask joined by ":"; formatting is not modelled. -/
namespace Qlogin.Inhibit

abbrev Kind := Nat   -- valid kinds are < 8 (`m_inhibit` rejects the rest)

inductive Mode | block | delay deriving DecidableEq, Repr

structure Inh where
  id   : Nat
  what : List Kind
  mode : Mode
deriving Repr

/-- `l1_inh_mask(mode)`: kinds in canonical order that at least one inhibitor of that mode holds. -/
def mask (inhs : List Inh) (m : Mode) : List Kind :=
  (List.range 8).filter (fun k => inhs.any (fun i => i.mode = m && i.what.contains k))

/-- All held kinds valid (< 8): what `m_inhibit` enforces by rejecting unknown names. -/
def Valid (inhs : List Inh) : Prop := ∀ i ∈ inhs, ∀ k ∈ i.what, k < 8

/-- `BlockInhibited` is `mask inhs .block`; `DelayInhibited` is `mask inhs .delay`:
    a kind is in the mask iff some inhibitor of that mode holds it. -/
theorem mem_mask (inhs : List Inh) (m : Mode) (k : Kind) (hv : Valid inhs) :
    k ∈ mask inhs m ↔ ∃ i ∈ inhs, i.mode = m ∧ k ∈ i.what := by
  unfold mask
  simp only [List.mem_filter, List.mem_range, List.any_eq_true, Bool.and_eq_true, decide_eq_true_eq,
    List.contains_iff_mem]
  constructor
  · rintro ⟨_, i, hi, hm, hk⟩; exact ⟨i, hi, hm, hk⟩
  · rintro ⟨i, hi, hm, hk⟩; exact ⟨hv i hi k hk, i, hi, hm, hk⟩

/-- No duplicates, canonical increasing order. -/
theorem mask_nodup (inhs : List Inh) (m : Mode) : (mask inhs m).Nodup :=
  List.nodup_range.filter _

theorem mask_sorted (inhs : List Inh) (m : Mode) : (mask inhs m).Pairwise (· < ·) :=
  List.pairwise_lt_range.filter _

/-- `l1_inh_has(mode, tok)` is membership in the mask. -/
def has (inhs : List Inh) (m : Mode) (k : Kind) : Bool :=
  inhs.any (fun i => i.mode = m && i.what.contains k)

theorem has_iff_mem_mask (inhs : List Inh) (m : Mode) (k : Kind) (hv : Valid inhs) :
    has inhs m k = true ↔ k ∈ mask inhs m := by
  rw [mem_mask inhs m k hv]; unfold has
  simp [List.any_eq_true, List.contains_iff_mem]

/-- Taking an inhibitor adds exactly its kinds (of its mode) to the mask. -/
theorem acquire_mask (inhs : List Inh) (n : Inh) (m : Mode) (k : Kind) (hv : Valid (n :: inhs)) :
    k ∈ mask (n :: inhs) m ↔ k ∈ mask inhs m ∨ (n.mode = m ∧ k ∈ n.what) := by
  have hv' : Valid inhs := fun i hi => hv i (List.mem_cons_of_mem _ hi)
  rw [mem_mask _ _ _ hv, mem_mask _ _ _ hv']; simp [or_comm]

/-- Release by id (the fd closing): drop that inhibitor. -/
def release (inhs : List Inh) (x : Nat) : List Inh := inhs.filter (fun i => i.id != x)

theorem release_valid (inhs : List Inh) (x : Nat) (hv : Valid inhs) : Valid (release inhs x) :=
  fun i hi => hv i (List.mem_filter.1 hi).1

/-- After a release the mask is exactly what the REMAINING inhibitors hold. -/
theorem release_mask (inhs : List Inh) (x : Nat) (m : Mode) (k : Kind) (hv : Valid inhs) :
    k ∈ mask (release inhs x) m ↔ ∃ i ∈ inhs, i.id ≠ x ∧ i.mode = m ∧ k ∈ i.what := by
  rw [mem_mask _ _ _ (release_valid inhs x hv)]; unfold release
  simp only [List.mem_filter, bne_iff_ne, ne_eq]
  constructor
  · rintro ⟨i, ⟨h1, h2⟩, h3, h4⟩; exact ⟨i, h1, h2, h3, h4⟩
  · rintro ⟨i, h1, h2, h3, h4⟩; exact ⟨i, ⟨h1, h2⟩, h3, h4⟩

/-- A release never adds anything. -/
theorem release_subset (inhs : List Inh) (x : Nat) (m : Mode) (k : Kind) (hv : Valid inhs) :
    k ∈ mask (release inhs x) m → k ∈ mask inhs m := by
  intro h; rw [release_mask _ _ _ _ hv] at h; rw [mem_mask _ _ _ hv]
  obtain ⟨i, h1, _, h3, h4⟩ := h; exact ⟨i, h1, h3, h4⟩

/-- Releasing removes exactly its own contribution: a kind leaves the mask only if the released inhibitor held it. -/
theorem release_only_own (inhs : List Inh) (x : Nat) (m : Mode) (k : Kind) (hv : Valid inhs)
    (h1 : k ∈ mask inhs m) (h2 : k ∉ mask (release inhs x) m) :
    ∃ i ∈ inhs, i.id = x ∧ i.mode = m ∧ k ∈ i.what := by
  rw [mem_mask _ _ _ hv] at h1
  obtain ⟨i, hi, hm, hk⟩ := h1
  by_cases hx : i.id = x
  · exact ⟨i, hi, hx, hm, hk⟩
  · exact absurd ((release_mask inhs x m k hv).2 ⟨i, hi, hx, hm, hk⟩) h2

/-- ... and it does leave when no other inhibitor holds that kind in that mode. -/
theorem release_sole_holder (inhs : List Inh) (x : Nat) (m : Mode) (k : Kind) (hv : Valid inhs)
    (hsole : ∀ j ∈ inhs, j.mode = m → k ∈ j.what → j.id = x) :
    k ∉ mask (release inhs x) m := by
  rw [release_mask _ _ _ _ hv]
  rintro ⟨j, hj, hne, hm, hk⟩
  exact hne (hsole j hj hm hk)

/-- Releasing an id nobody holds changes nothing. -/
theorem release_absent (inhs : List Inh) (x : Nat) (h : ∀ i ∈ inhs, i.id ≠ x) : release inhs x = inhs := by
  unfold release
  apply List.filter_eq_self.2
  intro i hi; simpa using h i hi

/-- Delay inhibitors only exist for shutdown (0) and sleep (1): `m_inhibit` rejects "delay" with k > 1. -/
def DelayOk (inhs : List Inh) : Prop := ∀ i ∈ inhs, i.mode = .delay → ∀ k ∈ i.what, k ≤ 1

theorem delay_mask_small (inhs : List Inh) (hv : Valid inhs) (hd : DelayOk inhs) :
    ∀ k ∈ mask inhs .delay, k ≤ 1 := by
  intro k hk
  rw [mem_mask _ _ _ hv] at hk
  obtain ⟨i, hi, hm, hk⟩ := hk
  exact hd i hi hm k hk

/-! ### Power actions. Category: 0 shutdown (PowerOff, Reboot, Halt), 1 sleep (the rest). -/

/-- The single pending-operation slot `L1.pend`. -/
structure Pend where
  cat      : Kind
  deadline : Nat
deriving Repr

/-- `l1_power`: refused while another operation is pending or a BLOCK inhibitor holds the category. -/
def request (inhs : List Inh) (pend : Option Pend) (cat : Kind) (now delayMax : Nat) : Option Pend :=
  match pend with
  | some _ => none
  | none => if has inhs .block cat then none else some ⟨cat, now + delayMax⟩

theorem request_iff (inhs : List Inh) (cat now dm : Nat) (hv : Valid inhs) :
    (request inhs none cat now dm).isSome = true ↔ cat ∉ mask inhs .block := by
  rw [← has_iff_mem_mask _ _ _ hv]; unfold request; by_cases h : has inhs .block cat = true <;> simp [h]

theorem request_busy (inhs : List Inh) (p : Pend) (cat now dm : Nat) : request inhs (some p) cat now dm = none := rfl

/-- `l1_tick_action`: the operation runs unless a DELAY inhibitor holds its category and the deadline has not passed. -/
def runs (inhs : List Inh) (p : Pend) (now : Nat) : Bool :=
  !(has inhs .delay p.cat && decide (now < p.deadline))

theorem runs_iff (inhs : List Inh) (p : Pend) (now : Nat) (hv : Valid inhs) :
    runs inhs p now = true ↔ (p.cat ∉ mask inhs .delay ∨ p.deadline ≤ now) := by
  rw [← has_iff_mem_mask _ _ _ hv]; unfold runs
  by_cases h : has inhs .delay p.cat = true <;> simp [h]

/-- A delay inhibitor can postpone the action at most to the deadline: by then it always runs. -/
theorem runs_at_deadline (inhs : List Inh) (p : Pend) (now : Nat) (h : p.deadline ≤ now) : runs inhs p now = true := by
  unfold runs; simp; omega

/-- With no delay inhibitor on the category the action runs on the very next tick. -/
theorem runs_without_delay (inhs : List Inh) (p : Pend) (now : Nat) (hv : Valid inhs) (h : p.cat ∉ mask inhs .delay) :
    runs inhs p now = true := (runs_iff inhs p now hv).2 (Or.inl h)

/-- As in the C (and in logind): a BLOCK inhibitor taken after the request does not stop a pending operation,
    `runs` never looks at block locks. -/
theorem late_block_ignored (inhs : List Inh) (n : Inh) (p : Pend) (now : Nat) (hn : n.mode = .block) :
    runs (n :: inhs) p now = runs inhs p now := by
  unfold runs has; simp [hn]

end Qlogin.Inhibit
