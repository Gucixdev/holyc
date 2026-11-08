/-! Kill switch ruleset (`nmbv_killswitch` in src/mods/vpn.h): nft table `inet nmbv_ks`,
    output chain, policy drop. First matching rule decides, otherwise the policy applies. -/
namespace Netman.KillSwitch

inductive Proto | tcp | udp | other
deriving DecidableEq

structure Pkt where
  oif : Nat
  dst : Nat           -- destination address (family folded into the value)
  proto : Proto
  sport : Nat
  dport : Nat

structure Ep where
  addr : Nat
  proto : Proto
  port : Nat

structure Cfg where
  lo : Nat                  -- loopback interface
  isTun : Nat → Bool        -- WireGuard: oif = profile name; OpenVPN: glob tun*
  eps : List Ep
  lan : Bool                -- NMBV_KS_LAN=1
  isLan : Nat → Bool        -- RFC1918 / link-local / ULA destinations

inductive Rule
  | oifLo | oifTun | ep (e : Ep) | dhcp4 | dhcp6 | dnsDrop | lanAccept

inductive V | accept | drop
deriving DecidableEq

/-- Verdict of one rule: `some v` if it matches. -/
def Rule.eval (c : Cfg) : Rule → Pkt → Option V
  | .oifLo, p => if p.oif = c.lo then some .accept else none
  | .oifTun, p => if c.isTun p.oif then some .accept else none
  | .ep e, p => if p.dst = e.addr ∧ p.proto = e.proto ∧ p.dport = e.port then some .accept else none
  | .dhcp4, p => if p.proto = .udp ∧ p.sport = 68 ∧ p.dport = 67 then some .accept else none
  | .dhcp6, p => if p.proto = .udp ∧ p.sport = 546 ∧ p.dport = 547 then some .accept else none
  | .dnsDrop, p => if (p.proto = .tcp ∨ p.proto = .udp) ∧ (p.dport = 53 ∨ p.dport = 853) then some .drop else none
  | .lanAccept, p => if c.isLan p.dst then some .accept else none

/-- The rules in the order the C code emits them. -/
def compile (c : Cfg) : List Rule :=
  [.oifLo, .oifTun] ++ c.eps.map Rule.ep ++ [.dhcp4, .dhcp6, .dnsDrop] ++ (if c.lan then [.lanAccept] else [])

def eval (c : Cfg) : List Rule → Pkt → V
  | [], _ => .drop                     -- policy drop
  | r :: t, p => match r.eval c p with | some v => v | none => eval c t p

def Accepted (c : Cfg) (p : Pkt) : Prop := eval c (compile c) p = .accept

def IsDhcp (p : Pkt) : Prop :=
  p.proto = .udp ∧ ((p.sport = 68 ∧ p.dport = 67) ∨ (p.sport = 546 ∧ p.dport = 547))

private theorem eval_accept (c : Cfg) (l : List Rule) (p : Pkt) (h : eval c l p = .accept) :
    ∃ r ∈ l, r.eval c p = some .accept := by
  induction l with
  | nil => simp [eval] at h
  | cons r t ih =>
    unfold eval at h
    cases hr : r.eval c p with
    | none => rw [hr] at h; simp at h; obtain ⟨r', h1, h2⟩ := ih h; exact ⟨r', List.mem_cons_of_mem _ h1, h2⟩
    | some v => rw [hr] at h; simp at h; subst h; exact ⟨r, by simp, hr⟩

/-- Main safety theorem: an accepted packet is loopback, via the tunnel, to an endpoint
    (address, protocol and port), DHCP, or (only when the LAN option is on) to a LAN address. -/
theorem accepted_cases (c : Cfg) (p : Pkt) (h : Accepted c p) :
    p.oif = c.lo ∨ c.isTun p.oif = true ∨
    (∃ e ∈ c.eps, p.dst = e.addr ∧ p.proto = e.proto ∧ p.dport = e.port) ∨
    IsDhcp p ∨ (c.lan = true ∧ c.isLan p.dst = true) := by
  obtain ⟨r, hr, hv⟩ := eval_accept c _ p h
  unfold compile at hr
  simp only [List.mem_append, List.mem_cons, List.mem_map, List.not_mem_nil, or_false] at hr
  rcases hr with ((hr | ⟨e, he, rfl⟩) | hr) | hr
  · rcases hr with rfl | rfl
    · left; simp [Rule.eval] at hv; exact hv
    · right; left; simp [Rule.eval] at hv; exact hv
  · right; right; left; refine ⟨e, he, ?_⟩; simp [Rule.eval] at hv; exact hv
  · rcases hr with rfl | rfl | rfl
    · right; right; right; left
      simp [Rule.eval] at hv; exact ⟨hv.1, Or.inl ⟨hv.2.1, hv.2.2⟩⟩
    · right; right; right; left
      simp [Rule.eval] at hv; exact ⟨hv.1, Or.inr ⟨hv.2.1, hv.2.2⟩⟩
    · exfalso; simp only [Rule.eval] at hv
      by_cases hq : (p.proto = .tcp ∨ p.proto = .udp) ∧ (p.dport = 53 ∨ p.dport = 853) <;> simp [hq] at hv
  · by_cases hl : c.lan = true
    · rw [if_pos hl] at hr; simp at hr; subst hr
      right; right; right; right; simp [Rule.eval] at hv; exact ⟨hl, hv⟩
    · rw [if_neg hl] at hr; simp at hr

/-- Contrapositive: anything that is not loopback, not via the tunnel, not to an endpoint, not DHCP
    (and not LAN when LAN is allowed) is dropped. With LAN off, no LAN clause at all. -/
theorem leak_dropped (c : Cfg) (p : Pkt) (hlo : p.oif ≠ c.lo) (htun : c.isTun p.oif = false)
    (hep : ∀ e ∈ c.eps, ¬ (p.dst = e.addr ∧ p.proto = e.proto ∧ p.dport = e.port))
    (hd : ¬ IsDhcp p) (hlan : c.lan = true → c.isLan p.dst = false) : ¬ Accepted c p := by
  intro h
  rcases accepted_cases c p h with h | h | ⟨e, he, h⟩ | h | ⟨h1, h2⟩
  · exact hlo h
  · simp [htun] at h
  · exact hep e he h
  · exact hd h
  · simp [hlan h1] at h2

/-- The tunnel and endpoint rules do accept what they should (the ruleset is not vacuous). -/
theorem tunnel_accepted (c : Cfg) (p : Pkt) (h : c.isTun p.oif = true) (hlo : p.oif ≠ c.lo) :
    Accepted c p := by
  simp [Accepted, compile, eval, Rule.eval, hlo, h]

/-! ### Atomic replace -/

/-- Kernel state: `none` = no table (everything passes), `some rs` = armed. -/
abbrev State := Option (List Rule × Cfg)

def verdict : State → Pkt → V
  | none, _ => .accept
  | some (rs, c), p => eval c rs p

/-- `nft -f -` runs `table X; delete table X; table X {..}` as ONE transaction:
    arm is a single step from any state to the new armed state. C refuses an empty endpoint list. -/
def arm (c : Cfg) (s : State) : State := if c.eps.isEmpty then s else some (compile c, c)
def disarm (_ : State) : State := none

/-- States reachable by arming: always some compiled ruleset (or the initial state). -/
inductive Reach (init : State) : State → Prop
  | start : Reach init init
  | arm (c : Cfg) {s} : Reach init s → Reach init (arm c s)
  | disarm {s} : Reach init s → Reach init (disarm s)

/-- An armed state is exactly `compile c` for some config. -/
def Armed (s : State) : Prop := ∃ c, s = some (compile c, c)

/-- arm never goes through the open state: from an armed state it stays armed. -/
theorem arm_preserves_armed (c : Cfg) (s : State) (h : Armed s) : Armed (arm c s) := by
  unfold arm; split
  · exact h
  · exact ⟨c, rfl⟩

/-- Safety of every armed state, including right after (and "during") an atomic replace. -/
theorem armed_safe (s : State) (h : Armed s) (p : Pkt) (hacc : verdict s p = .accept) :
    ∃ c, s = some (compile c, c) ∧
      (p.oif = c.lo ∨ c.isTun p.oif = true ∨
       (∃ e ∈ c.eps, p.dst = e.addr ∧ p.proto = e.proto ∧ p.dport = e.port) ∨
       IsDhcp p ∨ (c.lan = true ∧ c.isLan p.dst = true)) := by
  obtain ⟨c, rfl⟩ := h
  exact ⟨c, rfl, accepted_cases c p hacc⟩

theorem replace_safe (old new : Cfg) (p : Pkt) (_hold : Armed (some (compile old, old)))
    (hacc : verdict (arm new (some (compile old, old))) p = .accept) :
    (p.oif = old.lo ∨ old.isTun p.oif = true ∨
     (∃ e ∈ old.eps, p.dst = e.addr ∧ p.proto = e.proto ∧ p.dport = e.port) ∨
     IsDhcp p ∨ (old.lan = true ∧ old.isLan p.dst = true)) ∨
    (p.oif = new.lo ∨ new.isTun p.oif = true ∨
     (∃ e ∈ new.eps, p.dst = e.addr ∧ p.proto = e.proto ∧ p.dport = e.port) ∨
     IsDhcp p ∨ (new.lan = true ∧ new.isLan p.dst = true)) := by
  unfold arm at hacc
  split at hacc
  · left; exact accepted_cases old p hacc
  · right; exact accepted_cases new p hacc

/-- Why the single step matters: a delete-then-add replace has an open intermediate state in which a
    packet that is neither tunnelled nor to an endpoint is accepted. -/
theorem nonatomic_leaks (p : Pkt) : verdict none p = .accept := rfl

end Netman.KillSwitch
