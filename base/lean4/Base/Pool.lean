/-
  core_pool_alloc.h: pool_init and pool_free_all thread every chunk onto the free list, pool_alloc pops the head,
  pool_dealloc pushes. A chunk is named by its index: chunk i starts at byte i * chunk_size of the buffer.
-/
import Base.Arena

namespace Base.Pool

/-- pool_free_all pushes chunk 0, 1, ..., k-1 in turn, so chunk k-1 is the head. -/
def freeAll (k : Nat) : List Nat := (List.range k).reverse

/-- pool_alloc: the head of the free list, `none` when it is empty. -/
def alloc : List Nat → Option (Nat × List Nat)
  | [] => none
  | i :: fl => some (i, fl)

/-- pool_dealloc: chunk i goes on top of the free list. -/
def dealloc (i : Nat) (fl : List Nat) : List Nat := i :: fl

/-- Free list and live chunks together hold every chunk 0 .. k-1 exactly once. -/
def Inv (k : Nat) (fl live : List Nat) : Prop := (fl ++ live).Perm (List.range k)

theorem freeAll_inv (k : Nat) : Inv k (freeAll k) [] := by
  simp [Inv, freeAll, List.reverse_perm]

theorem inv_nodup {k : Nat} {fl live : List Nat} (h : Inv k fl live) : (fl ++ live).Nodup :=
  h.nodup_iff.mpr List.nodup_range

theorem inv_lt {k : Nat} {fl live : List Nat} (h : Inv k fl live) {i : Nat} (hi : i ∈ fl ++ live) : i < k :=
  List.mem_range.mp (h.mem_iff.mp hi)

/-- pool_alloc hands out a chunk below k that is not live, and the invariant holds with it live. -/
theorem alloc_inv {k : Nat} {fl live fl' : List Nat} {i : Nat} (h : Inv k fl live)
    (ha : alloc fl = some (i, fl')) : Inv k fl' (i :: live) ∧ i ∉ live ∧ i < k := by
  cases fl with
  | nil => cases ha
  | cons j fl0 =>
    simp only [alloc, Option.some.injEq, Prod.mk.injEq] at ha
    obtain ⟨rfl, rfl⟩ := ha
    refine ⟨List.perm_middle.trans h, ?_, inv_lt h (by simp)⟩
    have hn := inv_nodup h
    simp only [List.cons_append, List.nodup_cons, List.mem_append, not_or] at hn
    exact hn.1.2

/-- pool_dealloc of a live chunk keeps the invariant. -/
theorem dealloc_inv {k : Nat} {fl live : List Nat} {i : Nat} (h : Inv k fl live) (hi : i ∈ live) :
    Inv k (dealloc i fl) (live.erase i) :=
  (List.perm_middle.symm.trans (List.Perm.append_left fl (List.perm_cons_erase hi).symm)).trans h

/-- An empty free list means every chunk is live. -/
theorem alloc_none {k : Nat} {fl live : List Nat} (h : Inv k fl live) (ha : alloc fl = none) :
    live.Perm (List.range k) := by
  cases fl with
  | nil => simpa [Inv] using h
  | cons _ _ => cases ha

/-- pool_dealloc checks nothing: a chunk that is already free goes on the list a second time, so two later
    pool_alloc calls hand out the same chunk. -/
theorem double_dealloc {k : Nat} {fl live : List Nat} {i : Nat} (hi : i ∈ fl) :
    ¬ (dealloc i fl ++ live).Nodup := by
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
    Arena.alignUp cs 8 % 8 = 0 ∧ cs ≤ Arena.alignUp cs 8 ∧ Arena.alignUp cs 8 < cs + 8 := by
  have := Arena.alignUp_spec cs 8 0 (by decide)
  omega

end Base.Pool
