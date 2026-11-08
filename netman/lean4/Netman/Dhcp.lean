/-! DHCPv4 lease timers and the renew / rebind / expiry state machine.
    Model of `nmbn__finalize` and `nmbn_dhcpc_step` in src/mods/net.h.
    Finite leases only (lease_s = 0xffffffff never expires and is not modelled). -/
namespace Netman.Dhcp

structure Timers where
  lease : Nat
  t1 : Nat
  t2 : Nat
deriving Repr

def fL (lease : Nat) : Nat := if lease = 0 then 3600 else lease
def fA (l t1 : Nat) : Nat := if t1 = 0 ∨ t1 ≥ l then l / 2 else t1
def fB (l a t2 : Nat) : Nat := if t2 = 0 ∨ t2 ≥ l ∨ t2 ≤ a then l * 7 / 8 else t2

theorem fL_spec (x : Nat) : 0 < fL x ∧ (fL x = x ∨ x = 0) := by unfold fL; split <;> omega
theorem fA_spec (l t : Nat) : fA l t = l / 2 ∨ (fA l t = t ∧ 0 < t ∧ t < l) := by
  unfold fA; split <;> omega
theorem fB_spec (l a t : Nat) : fB l a t = l * 7 / 8 ∨ (fB l a t = t ∧ a < t ∧ t < l) := by
  unfold fB; split <;> omega

/-- `nmbn__finalize` faithfully: server-offered t1/t2 (0 = absent) are kept when they look sane. -/
def finalize (lease t1 t2 : Nat) : Timers :=
  let l := fL lease
  let a := fA l t1
  ⟨l, a, fB l a t2⟩

/-- Proposed fix: additionally clamp t1 down to t2. -/
def finalizeFixed (lease t1 t2 : Nat) : Timers :=
  let r := finalize lease t1 t2
  ⟨r.lease, min r.t1 r.t2, r.t2⟩

theorem finalize_lease_pos (l a b : Nat) : 0 < (finalize l a b).lease := (fL_spec l).1

theorem finalize_t2_lt (l a b : Nat) : (finalize l a b).t2 < (finalize l a b).lease := by
  have h := (fL_spec l).1
  have := fB_spec (fL l) (fA (fL l) a) b
  show fB (fL l) (fA (fL l) a) b < fL l
  omega

theorem finalize_t1_lt (l a b : Nat) : (finalize l a b).t1 < (finalize l a b).lease := by
  have h := (fL_spec l).1
  have := fA_spec (fL l) a
  show fA (fL l) a < fL l
  omega

/-- Exactly when the C code can violate T1 <= T2: a valid server T1 above 7/8 of the lease and no valid T2. -/
theorem finalize_order (l a b : Nat) :
    (finalize l a b).t1 ≤ (finalize l a b).t2 ∨
    (0 < a ∧ a < (finalize l a b).lease ∧ (finalize l a b).lease * 7 / 8 < a) := by
  have h := (fL_spec l).1
  have h1 := fA_spec (fL l) a
  have h2 := fB_spec (fL l) (fA (fL l) a) b
  show fA (fL l) a ≤ fB (fL l) (fA (fL l) a) b ∨ (0 < a ∧ a < fL l ∧ fL l * 7 / 8 < a)
  omega

/-- The bug is real: T1 = 95, no T2, lease 100 gives T1 = 95 > T2 = 87. -/
theorem finalize_counterexample :
    (finalize 100 95 0).t1 = 95 ∧ (finalize 100 95 0).t2 = 87 := by decide

theorem finalizeFixed_ok (l a b : Nat) :
    0 < (finalizeFixed l a b).lease ∧ (finalizeFixed l a b).t1 ≤ (finalizeFixed l a b).t2 ∧
    (finalizeFixed l a b).t2 < (finalizeFixed l a b).lease := by
  have h1 := finalize_lease_pos l a b
  have h2 := finalize_t2_lt l a b
  unfold finalizeFixed; simp only []; omega

/-- Without any server-supplied timers (the common case) the C code already gives T1 <= T2 < lease. -/
theorem finalize_defaults_ok (l : Nat) :
    (finalize l 0 0).t1 ≤ (finalize l 0 0).t2 := by
  rcases finalize_order l 0 0 with h | h
  · exact h
  · omega

/-- Well-formed timers. -/
def Wf (t : Timers) : Prop := 0 < t.lease ∧ t.t1 ≤ t.t2 ∧ t.t2 < t.lease

/-! ### What to do at a given lease age (the thresholds in `nmbn_dhcpc_step`) -/

inductive Act | wait | renew | rebind | expire
deriving DecidableEq, Repr

def Act.rank : Act → Nat
  | .wait => 0 | .renew => 1 | .rebind => 2 | .expire => 3

def act (t : Timers) (age : Nat) : Act :=
  if age ≥ t.lease then .expire
  else if age < t.t1 then .wait
  else if age ≥ t.t2 then .rebind
  else .renew

/-- renew happens before rebind happens before expiry: the action only moves forward with age. -/
theorem act_monotone (t : Timers) (a b : Nat) (h : a ≤ b) :
    (act t a).rank ≤ (act t b).rank := by
  unfold act
  by_cases ha1 : a ≥ t.lease <;> by_cases ha2 : a < t.t1 <;> by_cases ha3 : a ≥ t.t2 <;>
  by_cases hb1 : b ≥ t.lease <;> by_cases hb2 : b < t.t1 <;> by_cases hb3 : b ≥ t.t2 <;>
  first | omega | (simp [ha1, ha2, ha3, hb1, hb2, hb3, Act.rank]; try omega)

/-- renew happens before rebind: the action only moves forward with age. -/
theorem renew_before_rebind (t : Timers) (a b : Nat)
    (h1 : act t a = .rebind) (h2 : act t b = .renew) : b < a := by
  by_cases h : a ≤ b
  · have := act_monotone t a b h; rw [h1, h2] at this; simp [Act.rank] at this
  · omega

theorem rebind_before_expire (t : Timers) (a b : Nat)
    (h1 : act t a = .expire) (h2 : act t b = .rebind) : b < a := by
  by_cases h : a ≤ b
  · have := act_monotone t a b h; rw [h1, h2] at this; simp [Act.rank] at this
  · omega

theorem renew_before_expire (t : Timers) (a b : Nat)
    (h1 : act t a = .expire) (h2 : act t b = .renew) : b < a := by
  by_cases h : a ≤ b
  · have := act_monotone t a b h; rw [h1, h2] at this; simp [Act.rank] at this
  · omega

/-- All three phases occur, in order, when T1 < T2 < lease. -/
theorem phases (t : Timers) (h1 : t.t1 < t.t2) (h2 : t.t2 < t.lease) :
    act t t.t1 = .renew ∧ act t t.t2 = .rebind ∧ act t t.lease = .expire := by
  have a1 : ¬ t.t1 ≥ t.lease := by omega
  have a2 : ¬ t.t1 < t.t1 := by omega
  have a3 : ¬ t.t1 ≥ t.t2 := by omega
  have b1 : ¬ t.t2 ≥ t.lease := by omega
  have b2 : ¬ t.t2 < t.t1 := by omega
  have b3 : t.t2 ≥ t.t2 := by omega
  have c1 : t.lease ≥ t.lease := by omega
  exact ⟨by simp [act, a1, a2, a3], by simp [act, b1, b2], by simp [act]⟩

/-! ### The state machine -/

inductive St | init | bound
deriving DecidableEq, Repr

structure M where
  st : St
  held : Bool       -- an address is configured on the interface
  tm : Timers
  got : Nat
  nextTry : Nat

inductive ConnectOut | ack (tm : Timers) | fail
inductive RenewOut | ack (tm : Timers) | nak | timeout

def retryDelay (rest : Nat) : Nat := max 1 (rest / 2)

/-- Renew (unicast, T1) or rebind (broadcast, T2) attempt. -/
def renewStep (m : M) (now : Nat) (a : Act) (ren : RenewOut) : M :=
  if now < m.nextTry then m else
  match ren with
  | .nak => ⟨.init, false, m.tm, m.got, 0⟩
  | .ack tm => ⟨.bound, true, tm, now, 0⟩
  | .timeout =>
    let rest := (if a = Act.rebind then m.tm.lease else m.tm.t2) - (now - m.got)
    { m with nextTry := now + retryDelay rest }

def step (m : M) (now : Nat) (conn : ConnectOut) (ren : RenewOut) : M :=
  match m.st with
  | .init =>
    if now < m.nextTry then m else
    match conn with
    | .ack tm => ⟨.bound, true, tm, now, 0⟩
    | .fail => { m with nextTry := now + 5 }
  | .bound =>
    match act m.tm (now - m.got) with
    | .expire => ⟨.init, false, m.tm, m.got, 0⟩
    | .wait => m
    | a => renewStep m now a ren

/-- Invariant: address held exactly when bound, and bound timers are well-formed. -/
def Inv (m : M) : Prop := (m.st = .bound ↔ m.held = true) ∧ (m.st = .bound → Wf m.tm)

def OutOk (conn : ConnectOut) (ren : RenewOut) : Prop :=
  (∀ tm, conn = .ack tm → Wf tm) ∧ (∀ tm, ren = .ack tm → Wf tm)

/-- Never BOUND without an ACK. -/
theorem bound_needs_ack (m : M) (now : Nat) (c : ConnectOut) (r : RenewOut)
    (h0 : m.st = .init) (h : (step m now c r).st = .bound) : ∃ tm, c = .ack tm := by
  obtain ⟨st, held, tm, got, nt⟩ := m
  simp only [] at h0; subst h0
  cases c with
  | ack tm' => exact ⟨tm', rfl⟩
  | fail => simp only [step] at h; split at h <;> simp at h

theorem step_inv (m : M) (now : Nat) (c : ConnectOut) (r : RenewOut)
    (hi : Inv m) (ho : OutOk c r) : Inv (step m now c r) := by
  obtain ⟨st, held, tm, got, nt⟩ := m
  obtain ⟨hc, hr⟩ := ho
  have hc' : ∀ tm', c = .ack tm' → Wf tm' := hc
  have hr' : ∀ tm', r = .ack tm' → Wf tm' := hr
  clear hc hr
  cases st <;> cases c <;> cases r <;> simp only [step, renewStep] <;>
    first
    | exact hi
    | (split <;> first | exact hi | skip)
    | skip
  all_goals (try split)
  all_goals (try split)
  all_goals first | exact hi | simp_all [Inv, Wf] | skip

/-- NAK on a renew/rebind returns to INIT and drops the address. -/
theorem nak_returns_init (m : M) (now : Nat) (c : ConnectOut)
    (hb : m.st = .bound)
    (hact : act m.tm (now - m.got) = .renew ∨ act m.tm (now - m.got) = .rebind)
    (hn : ¬ now < m.nextTry) :
    (step m now c .nak).st = .init ∧ (step m now c .nak).held = false := by
  obtain ⟨st, held, tm, got, nt⟩ := m
  simp only [] at hb hact hn; subst hb
  rcases hact with h | h <;> simp [step, renewStep, h, hn]

/-- No address kept after expiry: a step at or after got+lease unconfigures and goes to INIT. -/
theorem expiry_drops (m : M) (now : Nat) (c : ConnectOut) (r : RenewOut)
    (hb : m.st = .bound) (he : m.got + m.tm.lease ≤ now) :
    (step m now c r).st = .init ∧ (step m now c r).held = false := by
  obtain ⟨st, held, tm, got, nt⟩ := m
  simp only [] at hb he; subst hb
  have : act tm (now - got) = .expire := by simp [act]; omega
  simp [step, this]

/-- After any step, a bound lease is strictly inside its lifetime (monotone clock: got <= now). -/
theorem bound_not_expired (m : M) (now : Nat) (c : ConnectOut) (r : RenewOut)
    (hg : m.got ≤ now) (hi : Inv m) (ho : OutOk c r)
    (h : (step m now c r).st = .bound) :
    now < (step m now c r).got + (step m now c r).tm.lease := by
  obtain ⟨st, held, tm, got, nt⟩ := m
  obtain ⟨hc, hr⟩ := ho
  obtain ⟨⟨hb1, hb2⟩, hw⟩ := hi
  simp only [] at hg hb1 hb2 hw
  have hc' : ∀ tm', c = .ack tm' → Wf tm' := hc
  have hr' : ∀ tm', r = .ack tm' → Wf tm' := hr
  clear hc hr
  have key : ∀ a, act tm (now - got) = a → a ≠ .expire → now - got < tm.lease := by
    intro a ha hne; unfold act at ha; split at ha
    · exact absurd ha.symm hne
    · omega
  cases st with
  | init =>
    cases c with
    | ack tm' =>
      have := (hc' tm' rfl).1
      simp only [step] at h ⊢
      by_cases hn : now < nt
      · rw [if_pos hn] at h; simp at h
      · rw [if_neg hn] at h ⊢; simp; omega
    | fail => simp only [step] at h; split at h <;> simp at h
  | bound =>
    have hwf := hw rfl
    simp only [step] at h ⊢
    cases hact : act tm (now - got) with
    | expire => simp [hact] at h
    | wait => have := key _ hact (by simp); simp [hact] at h ⊢; omega
    | renew =>
      have := key _ hact (by simp)
      simp only [hact, renewStep] at h ⊢
      by_cases hn : now < nt
      · rw [if_pos hn] at h ⊢; simp; omega
      · rw [if_neg hn] at h ⊢
        cases r with
        | nak => simp at h
        | ack tm' => have := (hr' tm' rfl).1; simp; omega
        | timeout => simp; omega
    | rebind =>
      have := key _ hact (by simp)
      simp only [hact, renewStep] at h ⊢
      by_cases hn : now < nt
      · rw [if_pos hn] at h ⊢; simp; omega
      · rw [if_neg hn] at h ⊢
        cases r with
        | nak => simp at h
        | ack tm' => have := (hr' tm' rfl).1; simp; omega
        | timeout => simp; omega

/-- A failed renew always schedules its retry strictly in the future (no busy loop). -/
theorem retry_in_future (rest now : Nat) : now < now + retryDelay rest := by
  unfold retryDelay; omega

end Netman.Dhcp
