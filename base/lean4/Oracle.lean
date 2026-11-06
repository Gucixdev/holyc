/-
  allocoracle: replays the operations c/alloc_harness.c ran on the real arena and pool, computes every result from
  the model in Base.Arena and Base.Pool, and prints the lines the harness should have printed. `run` compares the
  two outputs byte for byte. Input: one operation per line, the harness's own output (its results are ignored).
-/
import Base
import Holyc.Oracle
open Base

structure S where
  arena : Arena.St := ⟨0, 0, 0⟩
  base : Nat := 0
  page : Nat := 4096
  marks : Array Nat := #[]
  blocks : Array (Nat × Nat) := #[]
  pool : Pool.St := Pool.init 0

open Holyc.Oracle (num)

def step (st : S) : List String → S × String
  | ["arena", b, cap, pg] =>
    ({ st with arena := ⟨0, num cap, 0⟩, base := num b, page := num pg, marks := #[] }, s!"arena {b} {cap} {pg}")
  | "a" :: n :: _ =>
    match Arena.alloc st.arena (num n) with
    | some (o, s') => ({ st with arena := s', blocks := st.blocks.push (o, num n) }, s!"a {n} {o}")
    | none => (st, s!"a {n} none")
  | "c" :: n :: _ =>
    match Arena.alloc st.arena (num n) with
    | some (o, s') => ({ st with arena := s', blocks := st.blocks.push (o, num n) }, s!"c {n} {o} ok")
    | none => (st, s!"c {n} none")
  | "g" :: j :: n :: _ =>
    let (o, old) := st.blocks.getD (num j) (0, 0)
    match Arena.realloc st.arena o old (num n) with
    | some (o', s') =>
      ({ st with arena := s', blocks := st.blocks.set! (num j) (o', num n) }, s!"g {j} {n} {o'} ok")
    | none => (st, s!"g {j} {n} none")
  | "d" :: _ =>
    let p := st.page
    let f := (st.base + 8 * st.arena.count + p - 1) / p * p - st.base
    let s' := Arena.decommit st.arena f
    ({ st with arena := s' }, s!"d {s'.high}")
  | "l" :: n :: k :: _ =>
    let a := 2 ^ num k
    match Arena.alloc st.arena (num n + a - 1) with
    | some (o, s') => ({ st with arena := s' }, s!"l {n} {k} {Holyc.alignUp (st.base + o) a - st.base}")
    | none => (st, s!"l {n} {k} none")
  | "s" :: _ => ({ st with marks := st.marks.push (Arena.snapshot st.arena) }, s!"s {Arena.snapshot st.arena}")
  | "r" :: j :: _ =>
    let s' := Arena.rewind st.arena (st.marks.getD (num j) 0)
    ({ st with arena := s', blocks := #[] }, s!"r {j} {8 * s'.count}")
  | "z" :: _ => ({ st with arena := Arena.reset st.arena, blocks := #[] }, "z 0")
  | "p" :: bs :: cs :: _ =>
    let acs := Holyc.alignUp (num cs) 8
    ({ st with pool := Pool.init (num bs / acs) }, s!"p {bs} {cs} {acs}")
  | "pa" :: _ =>
    match Pool.alloc st.pool with
    | some (i, p') => ({ st with pool := p' }, s!"pa {i}")
    | none => (st, "pa none")
  | "pd" :: i :: _ => ({ st with pool := Pool.dealloc (num i) st.pool }, s!"pd {i}")
  | "pf" :: _ => ({ st with pool := Pool.init st.pool.k }, "pf")
  | ws => (st, s!"? {" ".intercalate ws}")

def main : IO Unit := Holyc.Oracle.run step {}
