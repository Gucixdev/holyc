/-! Session state and the active session of a seat. Model of `l1_seat_active`, `l1_refresh_active`,
    `l1_set_vt`/`m_activatesession`, `m_createsession` (append) and `l1_drop_session` (src/mods/login1.h).
    seat 0 = "no seat" (empty string in C). The state string is derived: closing / active / online. -/
namespace Qlogin.Session

structure Sess where
  id      : Nat
  seat    : Nat
  vtnr    : Nat
  closing : Bool
  active  : Bool
deriving Repr, DecidableEq

structure World where
  sess  : List Sess
  curvt : Nat
deriving Repr

/-- The `State` property. -/
inductive St | online | active | closing deriving DecidableEq, Repr
def Sess.state (s : Sess) : St := if s.closing then .closing else if s.active then .active else .online

/-- `l1_seat_active`: scan; skip other seats and closing sessions; a session on the current VT wins at once;
    otherwise the last VT-less one seen. -/
def pick (curvt seat : Nat) : List Sess → Option Nat → Option Nat
  | [], best => best
  | s :: t, best =>
    if s.seat ≠ seat ∨ s.closing = true then pick curvt seat t best
    else if s.vtnr ≠ 0 ∧ s.vtnr = curvt then some s.id
    else pick curvt seat t (if s.vtnr = 0 then some s.id else best)

def seatActive (w : World) (seat : Nat) : Option Nat := pick w.curvt seat w.sess none

theorem notskip {s : Sess} {seat : Nat} (h : ¬ (s.seat ≠ seat ∨ s.closing = true)) :
    s.seat = seat ∧ s.closing = false := by
  constructor
  · refine Classical.byContradiction fun hh => ?_; exact h (Or.inl hh)
  · refine Classical.byContradiction fun hh => ?_; exact h (Or.inr (by simpa using hh))

/-- `l1_refresh_active`: recompute every `active` flag. -/
def refresh (w : World) : World :=
  { w with sess := w.sess.map (fun s =>
      { s with active := decide (s.seat ≠ 0 ∧ s.closing = false ∧ seatActive w s.seat = some s.id) }) }

/-- The choice is always a live, non-closing session of that seat (or the initial `best`). -/
theorem pick_mem (curvt seat : Nat) (l : List Sess) (best : Option Nat) (i : Nat)
    (h : pick curvt seat l best = some i) :
    (∃ s ∈ l, s.id = i ∧ s.seat = seat ∧ s.closing = false) ∨ best = some i := by
  induction l generalizing best with
  | nil => right; simpa [pick] using h
  | cons s t ih =>
    unfold pick at h
    by_cases h1 : s.seat ≠ seat ∨ s.closing = true
    · rw [if_pos h1] at h
      rcases ih _ h with ⟨x, hx, r⟩ | r
      · exact Or.inl ⟨x, List.mem_cons_of_mem _ hx, r⟩
      · exact Or.inr r
    · rw [if_neg h1] at h
      obtain ⟨hs, hc⟩ := notskip h1
      by_cases h2 : s.vtnr ≠ 0 ∧ s.vtnr = curvt
      · rw [if_pos h2] at h
        left; exact ⟨s, List.mem_cons_self .., Option.some.inj h, hs, hc⟩
      · rw [if_neg h2] at h
        rcases ih _ h with ⟨x, hx, r⟩ | r
        · exact Or.inl ⟨x, List.mem_cons_of_mem _ hx, r⟩
        · by_cases h3 : s.vtnr = 0
          · rw [if_pos h3] at r
            left; exact ⟨s, List.mem_cons_self .., Option.some.inj r, hs, hc⟩
          · rw [if_neg h3] at r; exact Or.inr r

theorem seatActive_mem (w : World) (seat i : Nat) (h : seatActive w seat = some i) :
    ∃ s ∈ w.sess, s.id = i ∧ s.seat = seat ∧ s.closing = false := by
  rcases pick_mem _ _ _ _ _ h with r | r
  · exact r
  · simp at r

/-- After a refresh: an active session has a seat, is not closing, and is THE session `seatActive` names. -/
def Consistent (w : World) : Prop :=
  ∀ s ∈ w.sess, s.active = true → s.seat ≠ 0 ∧ s.closing = false ∧ seatActive w s.seat = some s.id

/-- At most one active session per seat (given as: two active sessions of one seat have the same id). -/
def AtMostOne (w : World) : Prop :=
  ∀ a ∈ w.sess, ∀ b ∈ w.sess, a.active = true → b.active = true → a.seat = b.seat → a.id = b.id

theorem refresh_sess_seat (w : World) : (refresh w).curvt = w.curvt := rfl

theorem pick_active (c seat : Nat) (g : Sess → Bool) (l : List Sess) (best : Option Nat) :
    pick c seat (l.map (fun s => { s with active := g s })) best = pick c seat l best := by
  induction l generalizing best with
  | nil => rfl
  | cons s t ih => simp only [List.map_cons, pick, ih]

/-- Refresh only rewrites `active`, so `seatActive` is the same afterwards. -/
theorem refresh_seatActive (w : World) (seat : Nat) : seatActive (refresh w) seat = seatActive w seat := by
  unfold seatActive refresh
  exact pick_active _ _ _ _ _

theorem refresh_consistent (w : World) : Consistent (refresh w) := by
  intro s hs ha
  unfold refresh at hs
  simp only [List.mem_map] at hs
  obtain ⟨s0, _, rfl⟩ := hs
  simp only [decide_eq_true_eq] at ha
  obtain ⟨h1, h2, h3⟩ := ha
  refine ⟨h1, h2, ?_⟩
  simpa [refresh_seatActive] using h3

theorem refresh_atMostOne (w : World) : AtMostOne (refresh w) := by
  intro a ha b hb haa hba hs
  have := refresh_consistent w
  have ca := this a ha haa
  have cb := this b hb hba
  rw [hs] at ca
  rw [ca.2.2] at cb
  exact (Option.some.inj cb.2.2)

/-- The state string never says "active" for a closing session, and every active session is on a seat. -/
theorem refresh_state_sound (w : World) : ∀ s ∈ (refresh w).sess, s.state = .active → s.seat ≠ 0 ∧ s.closing = false := by
  intro s hs h
  have c := refresh_consistent w s hs
  unfold Sess.state at h
  by_cases h1 : s.closing = true
  · simp [h1] at h
  · by_cases h2 : s.active = true
    · exact ⟨(c h2).1, (c h2).2.1⟩
    · simp [h1, h2] at h

/-- If a non-closing session of the seat is VT-less or sits on the current VT, the seat HAS an active session. -/
theorem pick_isSome_of_best (curvt seat : Nat) (l : List Sess) (best : Option Nat) (hb : best.isSome = true) :
    (pick curvt seat l best).isSome = true := by
  induction l generalizing best with
  | nil => simpa [pick] using hb
  | cons s t ih =>
    unfold pick
    by_cases h1 : s.seat ≠ seat ∨ s.closing = true
    · rw [if_pos h1]; exact ih _ hb
    · rw [if_neg h1]
      by_cases h2 : s.vtnr ≠ 0 ∧ s.vtnr = curvt
      · rw [if_pos h2]; rfl
      · rw [if_neg h2]
        apply ih
        by_cases h3 : s.vtnr = 0
        · rw [if_pos h3]; rfl
        · rw [if_neg h3]; exact hb

theorem seatActive_isSome (w : World) (seat : Nat)
    (h : ∃ s ∈ w.sess, s.seat = seat ∧ s.closing = false ∧ (s.vtnr = 0 ∨ s.vtnr = w.curvt)) :
    (seatActive w seat).isSome = true := by
  unfold seatActive
  have gen : ∀ (l : List Sess) (best : Option Nat),
      (∃ s ∈ l, s.seat = seat ∧ s.closing = false ∧ (s.vtnr = 0 ∨ s.vtnr = w.curvt)) →
      (pick w.curvt seat l best).isSome = true := by
    intro l
    induction l with
    | nil => intro b ⟨s, hs, _⟩; simp at hs
    | cons s t ih =>
      intro best ⟨x, hx, hseat, hcl, hv⟩
      unfold pick
      by_cases h1 : s.seat ≠ seat ∨ s.closing = true
      · rw [if_pos h1]
        apply ih
        rcases List.mem_cons.1 hx with hxs | hx'
        · exfalso; rw [hxs] at hseat hcl; rcases h1 with h | h
          · exact h hseat
          · rw [hcl] at h; exact Bool.false_ne_true h
        · exact ⟨x, hx', hseat, hcl, hv⟩
      · rw [if_neg h1]
        by_cases h2 : s.vtnr ≠ 0 ∧ s.vtnr = w.curvt
        · rw [if_pos h2]; rfl
        · rw [if_neg h2]
          rcases List.mem_cons.1 hx with hxs | hx'
          · apply pick_isSome_of_best
            rw [hxs] at hv
            have h0 : s.vtnr = 0 := by
              rcases hv with h | h
              · exact h
              · exact Classical.byContradiction fun hne => h2 ⟨hne, h⟩
            rw [if_pos h0]; rfl
          · exact ih _ ⟨x, hx', hseat, hcl, hv⟩
  exact gen w.sess none h

/-! ### Operations. Each one in the C ends with `l1_refresh_active()`. -/

/-- `l1_drop_session` (ReleaseSession): remove by id, keep order, refresh. -/
def drop (w : World) (x : Nat) : World := refresh { w with sess := w.sess.filter (fun s => s.id != x) }

/-- `m_createsession`: append a new session, refresh. -/
def create (w : World) (s : Sess) : World := refresh { w with sess := w.sess ++ [s] }

/-- `l1_set_vt` (ActivateSession / Seat.SwitchTo): vt 0 returns at once; otherwise the current VT changes. Refresh. -/
def activate (w : World) (vt : Nat) : World :=
  refresh (if vt = 0 then w else { w with curvt := vt })

theorem drop_inv (w : World) (x : Nat) : Consistent (drop w x) ∧ AtMostOne (drop w x) :=
  ⟨refresh_consistent _, refresh_atMostOne _⟩
theorem create_inv (w : World) (s : Sess) : Consistent (create w s) ∧ AtMostOne (create w s) :=
  ⟨refresh_consistent _, refresh_atMostOne _⟩
theorem activate_inv (w : World) (v : Nat) : Consistent (activate w v) ∧ AtMostOne (activate w v) :=
  ⟨refresh_consistent _, refresh_atMostOne _⟩

/-- Removal never leaves a seat pointing at a dead session: whatever `seatActive` names after a drop is a session
    that still exists, is not the removed one, and is not closing. -/
theorem drop_no_dangling (w : World) (x seat i : Nat) (h : seatActive (drop w x) seat = some i) :
    i ≠ x ∧ ∃ s ∈ (drop w x).sess, s.id = i ∧ s.seat = seat ∧ s.closing = false := by
  obtain ⟨s, hs, hid, hseat, hcl⟩ := seatActive_mem _ _ _ h
  refine ⟨?_, s, hs, hid, hseat, hcl⟩
  unfold drop refresh at hs
  simp only [List.mem_map, List.mem_filter, bne_iff_ne, ne_eq] at hs
  obtain ⟨s0, ⟨_, hne⟩, rfl⟩ := hs
  simpa [← hid] using hne

/-- ... and the active FLAGS likewise: an active session after a drop is a surviving one. -/
theorem drop_active_alive (w : World) (x : Nat) : ∀ s ∈ (drop w x).sess, s.active = true → s.id ≠ x := by
  intro s hs _
  unfold drop refresh at hs
  simp only [List.mem_map, List.mem_filter, bne_iff_ne, ne_eq] at hs
  obtain ⟨s0, ⟨_, hne⟩, rfl⟩ := hs
  exact hne

/-- If a survivor can take over (VT-less, or on the current VT), the seat still has an active session. -/
theorem drop_successor (w : World) (x seat : Nat)
    (h : ∃ s ∈ w.sess, s.id ≠ x ∧ s.seat = seat ∧ s.closing = false ∧ (s.vtnr = 0 ∨ s.vtnr = w.curvt)) :
    (seatActive (drop w x) seat).isSome = true := by
  rw [drop, refresh_seatActive]
  apply seatActive_isSome
  obtain ⟨s, hs, hne, r⟩ := h
  exact ⟨s, List.mem_filter.2 ⟨hs, by simpa using hne⟩, r⟩

/-- Activating a session without a VT is a no-op for the current VT (the C returns success and does nothing). -/
theorem activate_vtless (w : World) : (activate w 0).curvt = w.curvt := rfl

/-- Activating VT v: if some non-closing session of the seat sits on v, the seat's active session is one of them. -/
theorem activate_selects (w : World) (v seat : Nat) (hv : v ≠ 0)
    (h : ∃ s ∈ w.sess, s.seat = seat ∧ s.closing = false ∧ s.vtnr = v) :
    ∃ j ∈ w.sess, j.vtnr = v ∧ j.seat = seat ∧ j.closing = false ∧ seatActive (activate w v) seat = some j.id := by
  have gen : ∀ (l : List Sess) (best : Option Nat),
      (∃ s ∈ l, s.seat = seat ∧ s.closing = false ∧ s.vtnr = v) →
      ∃ j ∈ l, j.vtnr = v ∧ j.seat = seat ∧ j.closing = false ∧ pick v seat l best = some j.id := by
    intro l
    induction l with
    | nil => intro b ⟨s, hs, _⟩; simp at hs
    | cons s t ih =>
      intro best ⟨x, hx, hseat, hcl, hvt⟩
      unfold pick
      by_cases h1 : s.seat ≠ seat ∨ s.closing = true
      · rw [if_pos h1]
        have hx' : x ∈ t := by
          rcases List.mem_cons.1 hx with hxs | hx'
          · exfalso; rw [hxs] at hseat hcl; rcases h1 with h | h
            · exact h hseat
            · rw [hcl] at h; exact Bool.false_ne_true h
          · exact hx'
        obtain ⟨j, hj, r⟩ := ih best ⟨x, hx', hseat, hcl, hvt⟩
        exact ⟨j, List.mem_cons_of_mem _ hj, r⟩
      · rw [if_neg h1]
        obtain ⟨hs1, hs2⟩ := notskip h1
        by_cases h2 : s.vtnr ≠ 0 ∧ s.vtnr = v
        · rw [if_pos h2]
          exact ⟨s, List.mem_cons_self .., h2.2, hs1, hs2, rfl⟩
        · rw [if_neg h2]
          have hx' : x ∈ t := by
            rcases List.mem_cons.1 hx with hxs | hx'
            · exfalso; rw [hxs] at hvt; exact h2 ⟨by omega, hvt⟩
            · exact hx'
          obtain ⟨j, hj, r⟩ := ih _ ⟨x, hx', hseat, hcl, hvt⟩
          exact ⟨j, List.mem_cons_of_mem _ hj, r⟩
  obtain ⟨j, hj, r⟩ := gen w.sess none h
  refine ⟨j, hj, r.1, r.2.1, r.2.2.1, ?_⟩
  unfold activate
  simp only [hv, if_false]
  rw [refresh_seatActive]
  exact r.2.2.2

/-- Hence, when the VT is unique per seat among non-closing sessions, Activate makes exactly that session active. -/
theorem activate_unique (w : World) (v seat : Nat) (hv : v ≠ 0) (s : Sess) (hs : s ∈ w.sess)
    (hseat : s.seat = seat) (hcl : s.closing = false) (hvt : s.vtnr = v)
    (huniq : ∀ t ∈ w.sess, t.seat = seat → t.closing = false → t.vtnr = v → t.id = s.id) :
    seatActive (activate w v) seat = some s.id := by
  obtain ⟨j, hj, hjv, hjs, hjc, r⟩ := activate_selects w v seat hv ⟨s, hs, hseat, hcl, hvt⟩
  rw [r, huniq j hj hjs hjc hjv]

/-- Finding witness: two sessions on the same VT are allowed by `m_createsession`; Activate on the second leaves the first active. -/
theorem dup_vt_first_wins :
    let a : Sess := ⟨1, 1, 2, false, false⟩
    let b : Sess := ⟨2, 1, 2, false, false⟩
    seatActive (activate ⟨[a, b], 0⟩ 2) 1 = some 1 := by
  decide

/-! ### After the fix (2026-11-08): VT uniqueness at create, `closing` assigned before removal. -/

/-- `m_createsession` guard: a session with a VT is refused while a non-closing session already sits on that seat+VT. -/
def vtFree (w : World) (seat vt : Nat) : Bool :=
  vt == 0 || !(w.sess.any (fun s => s.closing == false && s.seat == seat && s.vtnr == vt))

def createChecked (w : World) (s : Sess) : Option World :=
  if vtFree w s.seat s.vtnr then some (create w s) else none

/-- The Finding-2 witness is now refused. -/
theorem dup_vt_refused :
    let a : Sess := ⟨1, 1, 2, false, false⟩
    let b : Sess := ⟨2, 1, 2, false, false⟩
    (createChecked ⟨[a], 0⟩ b).isNone = true := by
  decide

/-- Distinct VTs / VT-less sessions are still accepted. -/
theorem other_vt_accepted :
    let a : Sess := ⟨1, 1, 2, false, false⟩
    let b : Sess := ⟨2, 1, 3, false, false⟩
    (createChecked ⟨[a], 0⟩ b).isSome = true := by
  decide

/-- `l1_drop_session`, first half: `closing := true`, then `l1_refresh_active`. -/
def markClosing (w : World) (x : Nat) : World :=
  refresh { w with sess := w.sess.map (fun s => if s.id = x then { s with closing := true } else s) }

/-- A session being closed is never active and reads State "closing" (so the seat has already handed over before removal). -/
theorem markClosing_state (w : World) (x : Nat) :
    ∀ s ∈ (markClosing w x).sess, s.id = x → s.active = false ∧ s.state = .closing := by
  intro s hs hx
  unfold markClosing refresh at hs
  simp only [List.mem_map] at hs
  obtain ⟨s1, ⟨s0, _, rfl⟩, rfl⟩ := hs
  by_cases h : s0.id = x
  · simp [h, Sess.state]
  · simp [h] at hx

end Qlogin.Session
