/-! Device control, pause and resume. Model of `m_s_takecontrol`, `m_s_releasecontrol`, `m_s_takedevice`,
    `m_s_releasedevice`, `l1_release_control` and the pause/resume loop of `l1_apply_active` (src/mods/login1.h).
    A bus sender is a Nat; a device is its (major, minor) key. -/
namespace Qlogin.Device

structure Dev where
  key    : Nat × Nat
  paused : Bool
deriving Repr, DecidableEq

structure DS where
  id         : Nat
  seat       : Nat
  active     : Bool
  controller : Option Nat
  devs       : List Dev
deriving Repr, DecidableEq

def held (s : DS) (k : Nat × Nat) : Bool := s.devs.any (fun d => d.key == k)

/-- `m_s_takecontrol(force)`: a different controller blocks (EBUSY) unless root forces; forcing drops its devices. -/
def takeControl (s : DS) (caller : Nat) (force root : Bool) : Option DS :=
  match s.controller with
  | some c =>
    if c ≠ caller then
      (if force && root then some { s with controller := some caller, devs := [] } else none)
    else some s
  | none => some { s with controller := some caller }

/-- `m_s_releasecontrol` / `l1_release_control`: only the controller; closes every device. -/
def releaseControl (s : DS) (caller : Nat) : Option DS :=
  if s.controller = some caller then some { s with controller := none, devs := [] } else none

/-- `m_s_takedevice`: controller only, no double take; the device starts paused iff the session is not active. -/
def takeDevice (s : DS) (caller : Nat) (k : Nat × Nat) : Option DS :=
  if s.controller ≠ some caller then none
  else if held s k then none
  else some { s with devs := s.devs ++ [⟨k, !s.active⟩] }

/-- `m_s_releasedevice`. -/
def releaseDevice (s : DS) (caller : Nat) (k : Nat × Nat) : Option DS :=
  if s.controller ≠ some caller then none
  else if held s k then some { s with devs := s.devs.filter (fun d => d.key != k) }
  else none

/-- `l1_apply_active(s, b)`: no-op if unchanged; else flip `active` and (with a controller) pause / resume every device. -/
def setActive (s : DS) (b : Bool) : DS :=
  if s.active = b then s
  else { s with active := b,
                devs := if s.controller.isSome then s.devs.map (fun d => { d with paused := !b }) else s.devs }

/-- Invariant: every device's paused flag is "session not active"; devices only with a controller; no duplicate keys. -/
def Inv (s : DS) : Prop :=
  (∀ d ∈ s.devs, d.paused = !s.active) ∧ (s.devs ≠ [] → s.controller.isSome = true) ∧ (s.devs.map Dev.key).Nodup

theorem takeDevice_needs_controller (s : DS) (c : Nat) (k : Nat × Nat) (r : DS) (h : takeDevice s c k = some r) :
    s.controller = some c := by
  unfold takeDevice at h
  by_cases h1 : s.controller ≠ some c
  · simp [h1] at h
  · exact Classical.byContradiction fun hh => h1 hh |>.elim

/-- A non-controller is refused (including a session nobody controls). -/
theorem takeDevice_refused (s : DS) (c : Nat) (k : Nat × Nat) (h : s.controller ≠ some c) : takeDevice s c k = none := by
  simp [takeDevice, h]

theorem takeDevice_twice_refused (s r : DS) (c : Nat) (k : Nat × Nat) (h : takeDevice s c k = some r) :
    takeDevice r c k = none := by
  unfold takeDevice at h ⊢
  by_cases h1 : s.controller ≠ some c
  · simp [h1] at h
  · by_cases h2 : held s k = true
    · simp [h1, h2] at h
    · simp only [h1, h2, if_false] at h
      have hr := Option.some.inj h
      subst hr
      have : held { s with devs := s.devs ++ [⟨k, !s.active⟩] } k = true := by
        simp [held]
      have hc : ¬ ({ s with devs := s.devs ++ [⟨k, !s.active⟩] } : DS).controller ≠ some c := h1
      simp [hc, this]

theorem inv_takeControl (s : DS) (c : Nat) (f r : Bool) (s' : DS) (hi : Inv s) (h : takeControl s c f r = some s') : Inv s' := by
  obtain ⟨h1, h2, h3⟩ := hi
  unfold takeControl at h
  rcases hc : s.controller with _ | x <;> simp only [hc] at h
  · obtain rfl := Option.some.inj h
    exact ⟨h1, fun _ => rfl, h3⟩
  · by_cases hx : x ≠ c
    · rw [if_pos hx] at h
      by_cases hf : (f && r) = true
      · rw [if_pos hf] at h
        obtain rfl := Option.some.inj h
        simp [Inv]
      · rw [if_neg hf] at h; simp at h
    · rw [if_neg hx] at h
      obtain rfl := Option.some.inj h
      exact ⟨h1, h2, h3⟩

theorem inv_releaseControl (s : DS) (c : Nat) (s' : DS) (h : releaseControl s c = some s') : Inv s' := by
  unfold releaseControl at h
  by_cases h1 : s.controller = some c
  · simp only [h1, if_true] at h
    have := Option.some.inj h; subst this; simp [Inv]
  · simp [h1] at h

theorem inv_takeDevice (s : DS) (c : Nat) (k : Nat × Nat) (s' : DS) (hi : Inv s) (h : takeDevice s c k = some s') : Inv s' := by
  obtain ⟨h1, h2, h3⟩ := hi
  unfold takeDevice at h
  by_cases a : s.controller ≠ some c
  · simp [a] at h
  · by_cases b : held s k = true
    · simp [a, b] at h
    · simp only [a, b, if_false] at h
      have := Option.some.inj h; subst this
      have ac : s.controller = some c := Classical.byContradiction fun hh => a hh
      refine ⟨?_, ?_, ?_⟩
      · intro d hd
        simp only [List.mem_append, List.mem_singleton] at hd
        rcases hd with hd | rfl
        · exact h1 d hd
        · rfl
      · intro _; simp [ac]
      · have hk : ∀ d ∈ s.devs, d.key ≠ k := by
          intro d hd e
          apply b
          simp [held, List.any_eq_true]
          exact ⟨d, hd, e⟩
        simp only [List.map_append, List.map_cons, List.map_nil]
        rw [List.nodup_append]
        refine ⟨h3, by simp, ?_⟩
        intro x hx y hy
        simp at hy; subst hy
        simp only [List.mem_map] at hx
        obtain ⟨d, hd, rfl⟩ := hx
        exact hk d hd

theorem inv_releaseDevice (s : DS) (c : Nat) (k : Nat × Nat) (s' : DS) (hi : Inv s) (h : releaseDevice s c k = some s') : Inv s' := by
  obtain ⟨h1, h2, h3⟩ := hi
  unfold releaseDevice at h
  by_cases a : s.controller ≠ some c
  · simp [a] at h
  · by_cases b : held s k = true
    · simp only [a, b, if_false, if_true] at h
      have := Option.some.inj h; subst this
      refine ⟨?_, ?_, ?_⟩
      · intro d hd; exact h1 d (List.mem_filter.1 hd).1
      · intro hne
        apply h2
        intro e; rw [e] at hne; simp at hne
      · exact h3.sublist ((List.filter_sublist).map Dev.key)
    · simp [a, b] at h

theorem inv_setActive (s : DS) (b : Bool) (hi : Inv s) : Inv (setActive s b) := by
  obtain ⟨h1, h2, h3⟩ := hi
  unfold setActive
  by_cases hb : s.active = b
  · rw [if_pos hb]; exact ⟨h1, h2, h3⟩
  · rw [if_neg hb]
    by_cases hc : s.controller.isSome = true
    · simp only [hc, if_true]
      refine ⟨?_, ?_, ?_⟩
      · intro d hd
        simp only [List.mem_map] at hd
        obtain ⟨e, _, rfl⟩ := hd; rfl
      · intro hne; exact hc
      · simpa [Function.comp_def] using h3
    · -- no controller: no devices (by the invariant), nothing to pause
      have hnil : s.devs = [] := Classical.byContradiction fun hne => hc (h2 hne)
      simp only [hc, if_false, hnil]
      simp [Inv]

/-- Taking a device while the session is paused (not active) reports "paused"; while active, "running". -/
theorem takeDevice_paused_flag (s s' : DS) (c : Nat) (k : Nat × Nat) (h : takeDevice s c k = some s') :
    s'.devs.getLast? = some ⟨k, !s.active⟩ := by
  unfold takeDevice at h
  by_cases a : s.controller ≠ some c
  · simp [a] at h
  · by_cases b : held s k = true
    · simp [a, b] at h
    · simp only [a, b, if_false] at h
      obtain rfl := Option.some.inj h
      simp

theorem setActive_ctrl (s : DS) (b : Bool) (hne : ¬ s.active = b) (hc : s.controller.isSome = true) :
    setActive s b = { s with active := b, devs := s.devs.map (fun d => { d with paused := !b }) } := by
  unfold setActive; rw [if_neg hne, if_pos hc]

theorem setActive_noctl (s : DS) (b : Bool) (hne : ¬ s.active = b) (hc : ¬ s.controller.isSome = true) :
    setActive s b = { s with active := b } := by
  unfold setActive; rw [if_neg hne, if_neg hc]

theorem remap (l : List Dev) (x y : Bool) (hl : ∀ d ∈ l, d.paused = y) :
    (l.map (fun d => { d with paused := x })).map (fun d => { d with paused := y }) = l := by
  rw [List.map_map]
  conv => rhs; rw [← List.map_id l]
  apply List.map_congr_left
  intro d hd
  have := hl d hd
  cases d; simp_all

/-- Pause then resume restores the session exactly (all devices back to running, same devices, same controller). -/
theorem pause_resume (s : DS) (hi : Inv s) (ha : s.active = true) : setActive (setActive s false) true = s := by
  obtain ⟨h1, h2, _⟩ := hi
  have hrun : ∀ d ∈ s.devs, d.paused = false := fun d hd => by have := h1 d hd; simpa [ha] using this
  have hne : ¬ (s.active = false) := by simp [ha]
  by_cases hc : s.controller.isSome = true
  · rw [setActive_ctrl s false hne hc]
    have hne2 : ¬ ({ s with active := false, devs := s.devs.map (fun d => { d with paused := !false }) } : DS).active = true := by simp
    rw [setActive_ctrl _ true hne2 hc]
    have := remap s.devs (!false) (!true) (by simpa using hrun)
    simp only [this]
    obtain ⟨i, se, ac, ct, dv⟩ := s
    simp only at ha; subst ha; rfl
  · have hnil : s.devs = [] := Classical.byContradiction fun hh => hc (h2 hh)
    rw [setActive_noctl s false hne hc]
    have hne2 : ¬ ({ s with active := false } : DS).active = true := by simp
    rw [setActive_noctl _ true hne2 hc]
    obtain ⟨i, se, ac, ct, dv⟩ := s
    simp only at ha; subst ha; rfl

/-- Resume then pause on an inactive session likewise restores it. -/
theorem resume_pause (s : DS) (hi : Inv s) (ha : s.active = false) : setActive (setActive s true) false = s := by
  obtain ⟨h1, h2, _⟩ := hi
  have hrun : ∀ d ∈ s.devs, d.paused = true := fun d hd => by have := h1 d hd; simpa [ha] using this
  have hne : ¬ (s.active = true) := by simp [ha]
  by_cases hc : s.controller.isSome = true
  · rw [setActive_ctrl s true hne hc]
    have hne2 : ¬ ({ s with active := true, devs := s.devs.map (fun d => { d with paused := !true }) } : DS).active = false := by simp
    rw [setActive_ctrl _ false hne2 hc]
    have := remap s.devs (!true) (!false) (by simpa using hrun)
    simp only [this]
    obtain ⟨i, se, ac, ct, dv⟩ := s
    simp only at ha; subst ha; rfl
  · have hnil : s.devs = [] := Classical.byContradiction fun hh => hc (h2 hh)
    rw [setActive_noctl s true hne hc]
    have hne2 : ¬ ({ s with active := true } : DS).active = false := by simp
    rw [setActive_noctl _ false hne2 hc]
    obtain ⟨i, se, ac, ct, dv⟩ := s
    simp only at ha; subst ha; rfl

/-- The device SET never changes on pause / resume, only the flags. -/
theorem setActive_keeps_keys (s : DS) (b : Bool) : (setActive s b).devs.map Dev.key = s.devs.map Dev.key := by
  unfold setActive
  by_cases hb : s.active = b
  · rw [if_pos hb]
  · rw [if_neg hb]
    by_cases hc : s.controller.isSome = true
    · simp [hc, Function.comp_def]
    · simp [hc]

/-! ### Across sessions. -/

/-- Unpaused holder => the session is active (from `Inv`). -/
theorem unpaused_is_active (s : DS) (hi : Inv s) (d : Dev) (hd : d ∈ s.devs) (hu : d.paused = false) : s.active = true := by
  have := hi.1 d hd; rw [hu] at this; simpa using this.symm

/-- Given at most one active session per seat (`Qlogin.Session.refresh_atMostOne`) and `Inv` per session, a device
    key is held UNPAUSED by at most one session of a seat. -/
theorem unpaused_unique (w : List DS) (hinv : ∀ s ∈ w, Inv s)
    (hone : ∀ a ∈ w, ∀ b ∈ w, a.active = true → b.active = true → a.seat = b.seat → a.id = b.id)
    (a b : DS) (ha : a ∈ w) (hb : b ∈ w) (hs : a.seat = b.seat) (k : Nat × Nat)
    (da : Dev) (hda : da ∈ a.devs) (hka : da.key = k) (hua : da.paused = false)
    (db : Dev) (hdb : db ∈ b.devs) (hkb : db.key = k) (hub : db.paused = false) : a.id = b.id :=
  hone a ha b hb (unpaused_is_active a (hinv a ha) da hda hua) (unpaused_is_active b (hinv b hb) db hdb hub) hs

/-- Finding witness: "held by at most one session" is NOT enforced: two sessions can both hold the same device
    (one of them paused). There is no cross-session registry in the C, only the per-session `l1_find_dev`. -/
theorem two_holders_possible :
    let a : DS := ⟨1, 0, true, some 10, []⟩
    let b : DS := ⟨2, 0, false, some 20, []⟩
    (takeDevice a 10 (226, 0)).map (fun r => r.devs.map Dev.paused) = some [false] ∧
    (takeDevice b 20 (226, 0)).map (fun r => r.devs.map Dev.paused) = some [true] := by
  decide

/-! ### After the fix (2026-11-08): one holder per device across ALL sessions (`l1_dev_holder`). -/

/-- `m_s_takedevice` with the system-wide check: refused while ANOTHER session holds the key. -/
def takeDeviceG (all : List DS) (s : DS) (caller : Nat) (k : Nat × Nat) : Option DS :=
  if all.any (fun o => o.id != s.id && held o k) then none else takeDevice s caller k

/-- The `two_holders_possible` witness is now refused (EBUSY): b cannot take what a holds. -/
theorem two_holders_refused :
    let a : DS := ⟨1, 0, true, some 10, [⟨(226, 0), false⟩]⟩
    let b : DS := ⟨2, 0, false, some 20, []⟩
    takeDeviceG [a, b] b 20 (226, 0) = none := by
  decide

/-- Whenever the global take succeeds, no other session held the key beforehand. -/
theorem takeDeviceG_free (all : List DS) (s : DS) (caller : Nat) (k : Nat × Nat) (r : DS)
    (h : takeDeviceG all s caller k = some r) : ∀ o ∈ all, o.id ≠ s.id → held o k = false := by
  intro o ho hne
  unfold takeDeviceG at h
  by_cases hc : all.any (fun o => o.id != s.id && held o k) = true
  · simp [hc] at h
  · have hc' : ∀ x ∈ all, ¬ x.id = s.id → held x k = false := by simpa using hc
    exact hc' o ho hne

end Qlogin.Device
