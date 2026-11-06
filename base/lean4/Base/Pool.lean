/-
  core_pool_alloc.h: pool_init and pool_free_all empty the free list and set `fresh` to the buffer's start,
  pool_alloc pops the free list or, when it is empty, hands out the next chunk never used (`fresh`), and
  pool_dealloc pushes. A chunk is named by its index: chunk i starts at byte i * chunk_size of the buffer.
-/
import Base.Arena

namespace Base.Pool

/-- The pool: `fl` the chunks handed back (head first), chunks `fresh .. k-1` never handed out. -/
structure St where
  fl : List Nat
  fresh : Nat
  k : Nat
deriving Repr, DecidableEq

/-- pool_init and pool_free_all: O(1), no chunk touched. -/
def init (k : Nat) : St := ⟨[], 0, k⟩

/-- pool_alloc: the free list's head, else the next fresh chunk, `none` when neither is left. -/
def alloc (p : St) : Option (Nat × St) :=
  match p.fl with
  | i :: rest => some (i, { p with fl := rest })
  | [] => if p.fresh < p.k then some (p.fresh, { p with fresh := p.fresh + 1 }) else none

/-- pool_dealloc: chunk i goes on top of the free list. -/
def dealloc (i : Nat) (p : St) : St := { p with fl := i :: p.fl }

/-- Handed back, live, and never handed out: together every chunk 0 .. k-1 exactly once. -/
def Inv (p : St) (live : List Nat) : Prop :=
  (p.fl ++ live ++ List.range' p.fresh (p.k - p.fresh)).Perm (List.range p.k)

theorem init_inv (k : Nat) : Inv (init k) [] := by
  simp [Inv, init, List.range_eq_range']

theorem inv_nodup {p : St} {live : List Nat} (h : Inv p live) :
    (p.fl ++ live ++ List.range' p.fresh (p.k - p.fresh)).Nodup :=
  h.nodup_iff.mpr List.nodup_range

theorem inv_lt {p : St} {live : List Nat} (h : Inv p live) {i : Nat}
    (hi : i ∈ p.fl ++ live ++ List.range' p.fresh (p.k - p.fresh)) : i < p.k :=
  List.mem_range.mp (h.mem_iff.mp hi)

/-- pool_alloc hands out a chunk below k that is not live, and the invariant holds with it live. -/
theorem alloc_inv {p p' : St} {live : List Nat} {i : Nat} (h : Inv p live) (ha : alloc p = some (i, p')) :
    Inv p' (i :: live) ∧ i ∉ live ∧ i < p.k := by
  have hn := inv_nodup h
  rcases p with ⟨fl, fresh, k⟩
  cases fl with
  | cons j rest =>
    simp only [alloc, Option.some.injEq, Prod.mk.injEq] at ha
    obtain ⟨rfl, rfl⟩ := ha
    refine ⟨?_, ?_, inv_lt h (by simp)⟩
    · have : rest ++ j :: live ++ List.range' fresh (k - fresh) =
          rest ++ j :: (live ++ List.range' fresh (k - fresh)) := by simp
      simp only [Inv] at h ⊢
      rw [this]
      exact List.perm_middle.trans (by simpa using h)
    · simp only [List.cons_append, List.append_assoc, List.nodup_cons, List.mem_append, not_or] at hn
      exact hn.1.2.1
  | nil =>
    simp only [alloc] at ha
    split at ha
    · rename_i hlt
      simp only [Option.some.injEq, Prod.mk.injEq] at ha
      obtain ⟨rfl, rfl⟩ := ha
      have hr : List.range' fresh (k - fresh) = fresh :: List.range' (fresh + 1) (k - (fresh + 1)) := by
        rw [show k - fresh = (k - (fresh + 1)) + 1 by omega]; rfl
      refine ⟨?_, ?_, hlt⟩
      · simp only [Inv, List.nil_append] at h ⊢
        rw [hr] at h
        exact (List.perm_middle.symm.trans (by simpa using h))
      · rw [List.nil_append, hr] at hn
        simp only [List.nodup_append, List.mem_cons] at hn
        intro hi
        exact hn.2.2 fresh hi fresh (Or.inl rfl) rfl
    · cases ha

/-- pool_dealloc of a live chunk keeps the invariant. -/
theorem dealloc_inv {p : St} {live : List Nat} {i : Nat} (h : Inv p live) (hi : i ∈ live) :
    Inv (dealloc i p) (live.erase i) := by
  simp only [Inv, dealloc] at h ⊢
  refine List.Perm.trans ?_ h
  have := List.Perm.append_right (List.range' p.fresh (p.k - p.fresh))
    (List.perm_middle.symm.trans (List.Perm.append_left p.fl (List.perm_cons_erase hi).symm))
  simpa using this

/-- An empty free list with no fresh chunk left means every chunk is live. -/
theorem alloc_none {p : St} {live : List Nat} (h : Inv p live) (ha : alloc p = none) :
    live.Perm (List.range p.k) := by
  rcases p with ⟨fl, fresh, k⟩
  cases fl with
  | cons _ _ => simp [alloc] at ha
  | nil =>
    simp only [alloc] at ha
    split at ha
    · cases ha
    · rename_i hge
      simpa [Inv, show k - fresh = 0 by omega] using h

/-- pool_dealloc checks nothing: a chunk that is already free goes on the list a second time, so two later
    pool_alloc calls hand out the same chunk. -/
theorem double_dealloc {p : St} {live : List Nat} {i : Nat} (hi : i ∈ p.fl) :
    ¬ ((dealloc i p).fl ++ live ++ List.range' p.fresh (p.k - p.fresh)).Nodup := by
  simp [dealloc, List.nodup_cons, hi]

/-- Chunk i of a buffer of `bs` bytes cut into `bs / cs` chunks lies inside the buffer. -/
theorem chunk_in_buffer {bs cs i : Nat} (hi : i < bs / cs) : i * cs + cs ≤ bs := by
  rcases Nat.eq_zero_or_pos cs with rfl | hcs
  · simp at hi
  · have := (Nat.le_div_iff_mul_le hcs).mp (Nat.succ_le_of_lt hi)
    rwa [Nat.succ_mul] at this

/-- pool_init's chunk size, pool_align_up(chunk_size, sizeof(void *)): a multiple of 8, at least the request and
    less than 8 above it. -/
theorem chunk_size_spec (cs : Nat) :
    Holyc.alignUp cs 8 % 8 = 0 ∧ cs ≤ Holyc.alignUp cs 8 ∧ Holyc.alignUp cs 8 < cs + 8 := by
  have := Holyc.alignUp_spec cs 8 0 (by decide)
  omega

end Base.Pool
