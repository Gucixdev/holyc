/-! Network quality score. Model of `netman_score` and `netman__lin` (src/mods/scoring.h).
    Unknown inputs (the C sentinel NETMAN_UNKNOWN = -1000) are `Option.none` here. Ints are mathematical;
    the C `int` division truncates toward zero, identical to `Int` division on the non-negative operands used. -/
namespace Netman.Scoring

/-- `netman__lin`: v mapped to 0..100, `worst` -> 0, `best` -> 100. -/
def lin (v worst best : Int) : Int :=
  if best > worst then
    (if v ≥ best then 100 else if v ≤ worst then 0 else (v - worst) * 100 / (best - worst))
  else
    (if v ≤ best then 100 else if v ≥ worst then 0 else (worst - v) * 100 / (worst - best))

theorem lin_range (v w b : Int) (_h : w ≠ b) : 0 ≤ lin v w b ∧ lin v w b ≤ 100 := by
  unfold lin
  by_cases h1 : b > w
  · simp only [h1, if_true]
    by_cases h2 : v ≥ b
    · simp [h2]
    · by_cases h3 : v ≤ w
      · simp [h2, h3]
      · simp only [h2, h3, if_false]
        have hp : 0 < b - w := by omega
        constructor
        · exact (Int.ediv_nonneg_iff_of_pos hp).mpr (by
            have : 0 < v - w := by omega
            omega)
        · exact Int.ediv_le_of_le_mul hp (by
            have : (v - w) * 100 ≤ 100 * (b - w) := by
              have : v - w ≤ b - w := by omega
              omega
            omega)
  · simp only [h1, if_false]
    by_cases h2 : v ≤ b
    · simp [h2]
    · by_cases h3 : v ≥ w
      · simp [h2, h3]
      · simp only [h2, h3, if_false]
        have hp : 0 < w - b := by omega
        constructor
        · exact (Int.ediv_nonneg_iff_of_pos hp).mpr (by omega)
        · exact Int.ediv_le_of_le_mul hp (by omega)

/-- `lin` is monotone in v when best > worst (signal: -90 -> -50). -/
theorem lin_mono_up (a b w t : Int) (hwt : t > w) (hab : a ≤ b) : lin a w t ≤ lin b w t := by
  unfold lin
  simp only [hwt, if_true]
  have hp : 0 < t - w := by omega
  by_cases h1 : a ≥ t
  · have : b ≥ t := by omega
    simp [h1, this]
  · by_cases h2 : a ≤ w
    · simp only [h1, h2, if_true, if_false]
      by_cases h3 : b ≥ t
      · simp [h3]
      · by_cases h4 : b ≤ w
        · simp [h3, h4]
        · simp only [h3, h4, if_false]
          exact (Int.ediv_nonneg_iff_of_pos hp).mpr (by omega)
    · simp only [h1, h2, if_false]
      by_cases h3 : b ≥ t
      · simp only [h3, if_true]
        exact Int.ediv_le_of_le_mul hp (by omega)
      · have h4 : ¬ b ≤ w := by omega
        simp only [h3, h4, if_false]
        exact Int.ediv_le_ediv hp (by omega)

structure In where
  wired  : Bool
  signal : Option Int   -- dBm
  rtt    : Option Int   -- ms; < 0 = no answer
  loss   : Option Int   -- percent
  probe  : Option Int   -- 0 internet, 1 captive, < 0 none
deriving Repr

/-- (weighted value, weight); the invariant 0 <= value <= 100 * weight is `part_ok`. -/
def sigPart (i : In) : Int × Int :=
  if i.wired then (30 * 100, 30)
  else match i.signal with
    | some d => (30 * lin d (-90) (-50), 30)
    | none => (0, 0)

def rttPart (i : In) : Int × Int :=
  match i.rtt with
  | some r => (30 * (if r < 0 then 0 else lin r 500 20), 30)
  | none => (0, 0)

def clampLoss (l : Int) : Int := if l < 0 then 0 else if l > 100 then 100 else l

def lossPart (i : In) : Int × Int :=
  match i.loss with
  | some l => (40 * lin (clampLoss l) 25 0, 40)
  | none => (0, 0)

def capLoss (i : In) : Int :=
  match i.loss with
  | some l => if clampLoss l ≥ 100 then 0 else 100
  | none => 100

def capProbe (i : In) : Int :=
  match i.probe with
  | some p => if p = 0 then 100 else if p = 1 then 20 else 10
  | none => 100

def cap (i : In) : Int := min (capLoss i) (capProbe i)

def sumW (i : In) : Int := (sigPart i).1 + (rttPart i).1 + (lossPart i).1
def wTot (i : In) : Int := (sigPart i).2 + (rttPart i).2 + (lossPart i).2

def raw (i : In) : Int :=
  if wTot i = 0 then
    (match i.probe with | some 0 => 60 | none => 50 | some _ => 0)
  else sumW i / wTot i

/-- `netman_score`. -/
def score (i : In) : Int := min (raw i) (cap i)

theorem clamp_range (l : Int) : 0 ≤ clampLoss l ∧ clampLoss l ≤ 100 := by
  unfold clampLoss; split <;> (try split) <;> omega

theorem sig_ok (i : In) : 0 ≤ (sigPart i).1 ∧ (sigPart i).1 ≤ 100 * (sigPart i).2 := by
  unfold sigPart
  by_cases hw : i.wired = true
  · simp [hw]
  · have hf : i.wired = false := by simpa using hw
    cases hs : i.signal with
    | none => simp [hf, hs]
    | some d =>
      have := lin_range d (-90) (-50) (by omega)
      simp [hf, hs]; omega

theorem rtt_ok (i : In) : 0 ≤ (rttPart i).1 ∧ (rttPart i).1 ≤ 100 * (rttPart i).2 := by
  unfold rttPart
  cases hr : i.rtt with
  | none => simp
  | some r =>
    have := lin_range r 500 20 (by omega)
    simp only
    by_cases h : r < 0 <;> simp [h] <;> omega

theorem loss_ok (i : In) : 0 ≤ (lossPart i).1 ∧ (lossPart i).1 ≤ 100 * (lossPart i).2 := by
  unfold lossPart
  cases hl : i.loss with
  | none => simp
  | some l =>
    have c := clamp_range l
    have := lin_range (clampLoss l) 25 0 (by omega)
    simp only; omega

theorem w_nonneg (i : In) : 0 ≤ wTot i := by
  have a := sig_ok i; have b := rtt_ok i; have c := loss_ok i
  unfold wTot; omega

theorem raw_range (i : In) : 0 ≤ raw i ∧ raw i ≤ 100 := by
  unfold raw
  split
  · split <;> (try rename_i p; split) <;> omega
  · rename_i hw
    have hpos : 0 < wTot i := by have := w_nonneg i; omega
    have a := sig_ok i; have b := rtt_ok i; have c := loss_ok i
    constructor
    · exact (Int.ediv_nonneg_iff_of_pos hpos).mpr (by unfold sumW; omega)
    · exact Int.ediv_le_of_le_mul hpos (by unfold sumW wTot; omega)

theorem cap_range (i : In) : 0 ≤ cap i ∧ cap i ≤ 100 := by
  unfold cap capLoss capProbe
  cases i.loss with
  | none => cases i.probe with
    | none => simp
    | some p => by_cases h0 : p = 0 <;> by_cases h1 : p = 1 <;> simp [h0, h1] <;> omega
  | some l => cases i.probe with
    | none => by_cases h : clampLoss l ≥ 100 <;> simp [h] <;> omega
    | some p =>
      by_cases h : clampLoss l ≥ 100 <;> by_cases h0 : p = 0 <;> by_cases h1 : p = 1 <;> simp [h, h0, h1] <;> omega

/-- The score is always within 0..100. -/
theorem score_range (i : In) : 0 ≤ score i ∧ score i ≤ 100 := by
  have := raw_range i; have := cap_range i; unfold score; omega

theorem score_le_cap (i : In) : score i ≤ cap i := by unfold score; omega

/-- Captive portal: score <= 20. -/
theorem captive_cap (i : In) (h : i.probe = some 1) : score i ≤ 20 := by
  have := score_le_cap i
  have : cap i ≤ 20 := by unfold cap capProbe; rw [h]; simp; omega
  omega

/-- Any other non-zero probe result (no connectivity, probe < 0): score <= 10. -/
theorem no_internet_cap (i : In) (p : Int) (h : i.probe = some p) (h0 : p ≠ 0) (h1 : p ≠ 1) : score i ≤ 10 := by
  have := score_le_cap i
  have : cap i ≤ 10 := by unfold cap capProbe; rw [h]; simp [h0, h1]; omega
  omega

/-- 100% (or more) measured loss: score 0. -/
theorem full_loss_zero (i : In) (l : Int) (h : i.loss = some l) (hl : 100 ≤ l) : score i = 0 := by
  have := score_le_cap i
  have := score_range i
  have : cap i ≤ 0 := by
    unfold cap capLoss; rw [h]; simp only
    have : clampLoss l = 100 := by unfold clampLoss; split <;> (try split) <;> omega
    simp [this]; omega
  omega

/-- Monotone in signal: everything else fixed, a stronger signal never lowers the score. -/
theorem score_mono_signal (i j : In) (hw : i.wired = false) (a b : Int) (hab : a ≤ b)
    (ha : i.signal = some a) (hj : j = { i with signal := some b }) :
    score i ≤ score j := by
  have key : ∀ j : In, j.wired = false → ∀ d : Int, j.signal = some d →
      sigPart j = (30 * lin d (-90) (-50), 30) := by
    intro j hj d hd; unfold sigPart; rw [hj, hd]; simp
  have e1 := key i hw a ha
  have e2 := key j (by simp [hj, hw]) b (by simp [hj])
  have hmono := lin_mono_up a b (-90) (-50) (by omega) hab
  have hr : rttPart j = rttPart i := by simp [hj, rttPart]
  have hl : lossPart j = lossPart i := by simp [hj, lossPart]
  have hc : cap j = cap i := by simp [hj, cap, capLoss, capProbe]
  have hpr : j.probe = i.probe := by simp [hj]
  have hs : sumW i ≤ sumW j := by unfold sumW; rw [e1, e2, hr, hl]; simp; omega
  have hW : wTot j = wTot i := by unfold wTot; rw [e1, e2, hr, hl]
  have hpos : 0 < wTot i := by
    have r := rtt_ok i; have l := loss_ok i
    unfold wTot; rw [e1]; simp only; omega
  have hraw : raw i ≤ raw j := by
    unfold raw
    have h1 : wTot i ≠ 0 := by omega
    have h2 : wTot j ≠ 0 := by omega
    simp only [h1, h2, if_false]
    rw [hW]
    exact Int.ediv_le_ediv hpos hs
  unfold score; rw [hc]; omega

/-- A wired link ignores any signal reading. -/
theorem wired_ignores_signal (i : In) (hw : i.wired = true) (s : Option Int) :
    score { i with signal := s } = score i := by
  have : sigPart { i with signal := s } = sigPart i := by simp [sigPart, hw]
  simp [score, raw, cap, capLoss, capProbe, wTot, sumW, this, rttPart, lossPart]

end Netman.Scoring
