/-
  The driver of a differential test's oracle: a C harness prints one line per operation with its result; the
  oracle reads those lines, recomputes every result from the Lean model with `step`, and prints the lines the
  harness should have printed. The run script compares the two outputs byte for byte. A project's oracle is its
  `step` function and `def main : IO Unit := Holyc.Oracle.run step init`.
-/
namespace Holyc.Oracle

/-- A word of a harness line as a number; 0 when it is not one. -/
def num (w : String) : Nat := w.toNat?.getD 0

partial def loop {σ : Type} (h o : IO.FS.Stream) (step : σ → List String → σ × String) (st : σ) : IO Unit := do
  let line ← h.getLine
  if line.isEmpty then return
  let (st', out) := step st ((line.trimRight.splitOn " ").filter (· ≠ ""))
  o.putStrLn out
  loop h o step st'

/-- Replays stdin through `step` from `init`, one output line per input line. -/
def run {σ : Type} (step : σ → List String → σ × String) (init : σ) : IO Unit := do
  loop (← IO.getStdin) (← IO.getStdout) step init

end Holyc.Oracle
