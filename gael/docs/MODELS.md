# gael for models

gael is a statically typed Lisp for games and tools: s-expressions, every type checked before anything runs, no
undefined behavior. Write a file, run `gael check FILE`, fix what it says (each message gives the file, line and
column, what is wrong and what to write), run `gael test FILE`, then `gael FILE`. `gael -m FILE` shows the code after
macros; `docs/REFERENCE.md` has every detail; `examples/` has a working program for each part.

## Mistakes to avoid

- Count the parens. When they do not balance, gael names the line where a `)` is missing or one too many - keep the
  indentation honest, it reads it.
- Nothing converts on its own: `(+ 1 2.0)` is an error; write `(+ (float 1) 2.0)`. A literal takes the type expected
  where it stands. A float has digits on both sides of its point: `0.5`, `1.0e-9`.
- A struct is a value. `(set p.hp 5)` changes the local copy only; give it back: `(defn hurt ((p : Player)) : Player
  (set p.hp (- p.hp 1)) p)` and `(set hero (hurt hero))`. A `vec`, a `map` and a `pool` are references: changing one
  changes it for everyone who has it.
- `none` needs to know its type: from a declared binding, a parameter, a return type or the other branch.
- Methods: `(v.len)` is `(len v)`, `(p.hurt 3)` is `(Player.hurt p 3)`, `((get ps 0).hurt 3)` on an expression's
  value - always in parens; `(expr).field` without them reads a field.
- A local nothing reads, a value thrown away, code after `return` are warnings: fix them. A name that starts with `_`
  is unused on purpose; `(for (_ 0 3) ...)` repeats without a name.
- Nothing waits: there is no `async` or `await`. Coroutines pause themselves; `(poll co)` looks at a result at once.
- The last form of a body is its value. `return` leaves early: `(when bad (return none))`.
- Ints wrap around; division by zero, an index out of a vec, a stale handle are runtime errors with the line.
- Formats are checked: `%d` ints, `%x` ints in hex, `%f` floats, `%s` strings, `%b` bools, `%v` anything, `%%` a %.

## The shape of a program

```lisp
; a comment
(use "player.gael")                        ; compile another file of this program first
(enable ecs)                               ; turn a gem on
(struct Vec2 (x : float 0.0) (y : float 0.0)) ; fields with optional defaults
(enum Dir north east south west)           ; Dir.north; north in a match
(def speed : float 4.5)                    ; a global; the type may be left to the value
(defn add ((a : Vec2) (b : Vec2)) : Vec2   ; parameters and return type; no return type is unit
  (Vec2 (+ a.x b.x) (+ a.y b.y)))          ; (Vec2 1.0 2.0) in order, (Vec2 :y 2.0) by name
(defn Vec2.len2 ((v : Vec2)) : float (+ (* v.x v.x) (* v.y v.y))) ; a method: (v.len2)
(println "%v" (add (Vec2 1.0 2.0) (Vec2)))
```

Types: `int` (i64), `float` (f64), `bool` (`true`, `false`), `string`, `unit`; `i8` `i16` `i32` `i64` `u8` `u16`
`u32` `u64` `f32` `f64`, each also a conversion: `(u8 x)`, `(int f)`; `(option T)`, `(vec T)`, `(map K V)`,
`(pool T)`, `(handle T)`, `(fn (A B) R)`, `(coroutine T)`, `NodePath` (`$Player/Sprite`). `:` gives a type;
`:keyword` names a field: `(Vec2 :x 1.0)`.

## Control

```lisp
(let ((x 1) (y : float 2.0)) body...)      ; locals; _ as a name binds nothing
(set x 5) (set p.pos.x 1.0)                ; a variable or a field gets a new value
(if c then else)  (when c body...)  (while c body...)  (do a b c)
(for (i 0 10) body...)                     ; 0 to 9
(for (x v) body...)                        ; each element of a vec; a map's keys; a pool's handles
(match n (0 "zero") (1 "one") (_ "many")) ; every case covered: (some v) and none, enum values, _
(and a b) (or a b) (not a) (nand a b) (nor a b) (xor a b) (xnor a b)
(return x) (return)                        ; leave the function now
(chain x (f a) g)                          ; (g (f x a)): each step gets the value so far first
(assert (> hp 0)) (assert ok "why it must hold") ; a runtime error when false
```

Compare with `=` `!=` `<` `<=` `>` `>=`; arithmetic `+` `-` `*` `/` `%`; `(- x)` negates. `(min a b...)` `(max a b...)`
`(abs x)` `(clamp x lo hi)` on anything `<` orders; a local or field may still be called `min` or `max`.

## Data

```lisp
(vec 1 2 3)  (let ((v : (vec int) (vec))) ...) ; (push v x) (get v i) (put v i x) (pop v) gives an option, (len v)
(map ("a" 1) ("b" 2))                      ; (insert m k v) (lookup m k) an option (has m k) (remove m k) (keys m) (values m)
(def ps : (pool Particle) (pool 1000))     ; fixed slots: (spawn ps x) gives a (handle Particle)
(get ps h) (put ps h x) (lookup ps h) (has ps h) (remove ps h) (len ps) ; a stale handle never reaches a new element
(some 5) none                              ; (option int)
(get bodies 0).pos.x                       ; a field of an expression's value; also (. expr pos.x)
```

A vec of structs is stored as columns (one per field, bools as bits). A struct or enum can give hooks, functions named
`Type.name`: `add` `sub` `mul` `div` `rem` `negate` (for `+ - * / %` and `(- x)`), `equal`, `compare`, `text` (for `%v`
and `%s`), `len`, `get`, `put` (and so `for`), `init` (for `(Type args...)`).

## Strings and printing

```lisp
(print "fmt" x...) (println "fmt" x...) (eprint "fmt" x...) (eprintln "fmt" x...) (format "fmt" x...)
(len s) (substr s from to) (byte s i) (char 65) (trim s) (split s ",") (join v ",")
(find s "part") (parse-int s) (parse-float s) ; options: none when not found or not a number
```

## Numbers, math, bits

`sqrt` `sin` `cos` `tan` `asin` `acos` `atan` `atan2` `exp` `ln` `pow` `floor` `ceil` on floats;
`(bit-and a b)` `(bit-or a b)` `(bit-xor a b)` `(bit-not a)` `(shl a n)` `(shr a n)` on ints.

## Files and the system

```lisp
(read-file path) (read-line) (getenv "HOME") ; options
(write-file path text) (append-file path text) ; bools: true when it worked
(args) (env "HOME") (pid) (exit 1)
(sh "ls") (run "make") (system "make") (pipe "ls" "wc -l") (pipe-in text "sort")
```

`sh` gives the output, `run` a bool (exit 0), `system` the exit status; `(and (run "a") (run "b"))` runs b only after a
succeeded, as `&&` does.

## Functions, memory

```lisp
(fn ((x : int)) : int (* x 2))             ; a closure: keeps copies of the locals it uses
(def double : (fn (int) int) (fn ((x : int)) : int (* x 2)))
(arena body...)                            ; what the body makes is freed at once at its end
(keep x)                                   ; x copied out of the arena, to outlive it
```

Strings, vecs, maps, pools and closures count their owners and are freed when the last one lets go.

## Coroutines

```lisp
(def c (coroutine (patrol 0)))             ; starts patrol at once, until it first pauses
(sleep 0.5) (yield)                        ; inside it: pause for coroutine time, or until the next step
(step-coroutines dt)                       ; the game loop, once a frame; gives how many are left
(run-coroutines)                           ; a script: run until none is left
(poll c) (done? c) (coroutine-time)        ; (some result) or none, never waits; ended?; the clock
```

## GPU

```lisp
(struct Ball (x : f32) (y : f32) (vy : f32))  ; GPU fields are f32, i32 or u32
(def dt : f32 0.016)
(gpu-each (b balls) (set b.vy (- b.vy (* 9.8 dt))) (set b.y (+ b.y (* b.vy dt)))) ; once per element, all at once
```

The body sets only its element's fields and its own `let`s, reads `f32` `i32` `u32` variables from around, and has
arithmetic, comparisons, `if` `when` `let` `do`, math, `min` `max` `abs` `clamp`; no calls, prints or loops. A `let`
from a literal says its type: `(k : f32 0.5)`. With no GPU it runs on the VM, with the same result.

## Drawing

```lisp
(def c (new-canvas 640 480))               ; on the GPU; colors are 0xRRGGBBAA
(draw-clear c 0x202030ff) (draw-rect c 10.0 10.0 50.0 20.0 0xff0000ff) (draw-circle c 320.0 240.0 8.0 0xffcc00ff)
(draw-line c 0.0 0.0 640.0 480.0 2.0 0xffffffff) (show-frame c) (canvas-pixel c 5 5) (save-png c "out.png")
(def w (open-window "game" 640 480))       ; a canvas on the screen (Wayland)
(while (show-frame w) (when (key-down? w "space") (jump)) (draw-circle w (mouse-x w) (mouse-y w) 4.0 0xffffffff))
```

`show-frame` on a window waits for the screen and gives false once it is closed. Keys by place: `"a"`, `"1"`,
`"f1"`, `"space"`, `"enter"`, `"escape"`, `"up"`, `"shift"`. `(mouse-down? w 1)`: 1 left, 2 right, 3 middle.

## Macros

```lisp
(defmacro swap ((_ a b) (let ((tmp a)) (set a b) (set b tmp))))  ; hygienic: tmp is the macro's own
(defmacro unless ((_ c body ...) (when (not c) body ...)))        ; ... repeats
(string-of x) (name-of "print-" x) (compile-error "what is wrong") ; in a template: #, ## and an error
```

In a pattern a symbol matches one form, `_` anything, a keyword, number, string or built-in name only itself. At the
top level a macro can make several definitions with `(do ...)`.

## Tests and marks

```lisp
(@test "adds" (assert (= (add 1 2) 3)))    ; top level; runs only under gael test
@test (defn sample () : int 21)            ; a top-level form only the tests use
(@debug (println "x %d" x))                ; code for while working
(@warn "slow for big n") (@err "fix before shipping") ; marks for the tools: gael -x lists them
@export (def speed : float 10.0)           ; for an editor's inspector
```

## Gems

A gem is a package: `(gem name :version "0.1.0" :requires (other))` first in its file; `(enable name...)` turns gems
on, `(disable name)` off. These come with gael:

- `ecs`: `(ecs-world Game (components (pos : Vec2 (Vec2)) (hp : int 100)) (tags enemy burning))` makes `Game.spawn`,
  `(e.despawn)` `(e.alive?)` `Game.count`, `(e.pos)` `(e.set-pos v)` `(e.remove-pos)`, bits `Game-pos` `Game-enemy`,
  `(e.has? bits)` `(e.tag bits)` `(e.untag bits)`, `(Game.bit "burning")`; a system is a function with
  `(ecs-each Game (e pos hp) :with (enemy) :not (burning) body...)` - changes to the named components are kept.
- `hfsm`: `(hfsm Mood (e : Ctx) (alive -) (calm alive) (dead -) :enter ((calm code)) :exit ((calm code)))` makes the
  enum and `(Mood.start s e)`, `(s.go to e)`, `(s.in? t)`, `(s.parent)`.
- `pqueue`: `(PQ)`, `(q.push id priority)` (1 first, 0 last), `(q.pop)` and `(q.peek)` options, `(len q)`.
- `events`: `(EventLog)` with `(log.add kind tick a b)`, `(log.at tick)`, `(log.since tick)`; `(Schedule)` with
  `(s.add (Event kind tick a b))` and `(s.due tick)`.
- `rng`: `(Rng.seed seed stream)`, `(r.next)`, `(r.float)`, `(r.range lo hi)`, `(r.chance p)`.
- `noise`: `(noise2 seed x y)`, `(fbm2 seed x y octaves)`. `easing`: `lerp`, `smoothstep`, `ease-in-out-cubic`...
  `curve`: `bezier3`, `hermite`, `catmull-rom`, `morton2`.
- `geometry`: `(Vec2 x y)` `(Vec3 x y z)` with `+ - * =`, `(a.dot b)` `(v.length)` `(v.normal)` `(a.distance b)`
  `(a.lerp b t)` `(Vec3.cross a b)`; `(Rect min max)` with `(r.width)` `(r.height)` `(r.center)` `(r.contains? p)`.
- `wfc` (wave function collapse): `(Wfc.new w h tiles)`, `(w.allow-all a b)`, `(w.allow a wfc-right b)`,
  `(w.weight t 3.0)`, `(w.fix x y t)`, `(w.solve (Rng.seed 1 0) 20)` true when solved, `(w.tile x y)`.

## Commands

`gael FILE ARGS...` runs; `gael check FILE` compiles only; `gael test FILE` runs the defs and each `@test` (of a gem
with `gael test gems/NAME.gael`); `gael -m
FILE` prints it with macros expanded; `gael -d FILE` the bytecode; `gael -x FILE` the exports and marks; `gael` alone
is the REPL, where a function can be given a new body while the program runs.
