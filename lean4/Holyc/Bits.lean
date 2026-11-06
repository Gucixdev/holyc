/-
  Bit tricks of C code and the arithmetic they stand for, proved once: masking off low bits, rounding up to a power
  of two. A model of a C function states its rounding with `alignUp` and cites these.
-/
namespace Holyc

/-- `x & ~(2^k - 1)` on a 64-bit word clears the low k bits: it is `x / 2^k * 2^k`. -/
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

/-- C's rounding idiom `(x + align - 1) & ~(align - 1)` on a 64-bit word, for align = 2^k. -/
def alignC (x k : Nat) : Nat := (x + 2 ^ k - 1) &&& (2 ^ 64 - 2 ^ k)

theorem alignC_eq (x k : Nat) (hk : k ≤ 64) (hx : x + 2 ^ k - 1 < 2 ^ 64) : alignC x k = alignUp x (2 ^ k) :=
  and_high_mask _ _ hk hx

/-- The rounded value is a multiple of the alignment, not below x, and `n` bytes from it stay inside the `n + a - 1`
    bytes an aligned allocation asks for. -/
theorem alignUp_spec (x a n : Nat) (ha : 0 < a) :
    alignUp x a % a = 0 ∧ x ≤ alignUp x a ∧ alignUp x a + n ≤ x + (n + a - 1) := by
  unfold alignUp
  have h1 := Nat.div_add_mod' (x + a - 1) a
  have h2 := Nat.mod_lt (x + a - 1) ha
  refine ⟨Nat.mul_mod_left _ _, ?_, ?_⟩ <;>
  · generalize (x + a - 1) / a * a = p at *
    generalize (x + a - 1) % a = r at *
    omega

end Holyc
