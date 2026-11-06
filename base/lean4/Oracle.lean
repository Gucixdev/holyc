/-
  allocoracle: replays the operations c/alloc_harness.c ran on the real arena and pool, computes every result from
  the model in Base.Arena and Base.Pool, and prints the lines the harness should have printed. `run` compares the
  two outputs byte for byte. Input: one operation per line, the harness's own output (its results are ignored).
-/
import Base
open Base

structure S where
  arena : Arena.St := ⟨0, 0⟩
  base : Nat := 0
  marks : Array Nat := #[]
  fl : List Nat := []
  chunks : Nat := 0

def num (w : String) : Nat := w.toNat?.getD 0

def step (st : S) : List String → S × String
  | ["arena", b, cap] => ({ st with arena := ⟨0, num cap⟩, base := num b, marks := #[] }, s!"arena {b} {cap}")
  | "a" :: n :: _ =>
    match Arena.alloc st.arena (num n) with
    | some (o, s') => ({ st with arena := s' }, s!"a {n} {o}")
    | none => (st, s!"a {n} none")
  | "l" :: n :: k :: _ =>
    let a := 2 ^ num k
    match Arena.alloc st.arena (num n + a - 1) with
    | some (o, s') => ({ st with arena := s' }, s!"l {n} {k} {Arena.alignUp (st.base + o) a - st.base}")
    | none => (st, s!"l {n} {k} none")
  | "s" :: _ => ({ st with marks := st.marks.push (Arena.snapshot st.arena) }, s!"s {Arena.snapshot st.arena}")
  | "r" :: j :: _ =>
    let s' := Arena.rewind st.arena (st.marks.getD (num j) 0)
    ({ st with arena := s' }, s!"r {j} {8 * s'.count}")
  | "z" :: _ => ({ st with arena := Arena.reset st.arena }, "z 0")
  | "p" :: bs :: cs :: _ =>
    let acs := Arena.alignUp (num cs) 8
    let k := num bs / acs
    ({ st with fl := Pool.freeAll k, chunks := k }, s!"p {bs} {cs} {acs}")
  | "pa" :: _ =>
    match Pool.alloc st.fl with
    | some (i, fl') => ({ st with fl := fl' }, s!"pa {i}")
    | none => (st, "pa none")
  | "pd" :: i :: _ => ({ st with fl := Pool.dealloc (num i) st.fl }, s!"pd {i}")
  | "pf" :: _ => ({ st with fl := Pool.freeAll st.chunks }, "pf")
  | ws => (st, s!"? {" ".intercalate ws}")

partial def loop (h : IO.FS.Stream) (o : IO.FS.Stream) (st : S) : IO Unit := do
  let line ← h.getLine
  if line.isEmpty then return
  let ws := (line.trimRight.splitOn " ").filter (· ≠ "")
  let (st', out) := step st ws
  o.putStrLn out
  loop h o st'

def main : IO Unit := do
  loop (← IO.getStdin) (← IO.getStdout) {}
