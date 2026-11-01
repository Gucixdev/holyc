# gael reference

Every form, operator, function and type of the language, what it takes, what it gives and what it reports. The README
says what gael is and how to build it; the spec (in Polish) says why it is the way it is. `gael -n` lists every
built-in name, and `./test` fails when one of them is missing from the index below, so this file covers the whole
language.

How the entries read:

- `T`, `U` stand for any type, `K` for a key type, `N` for a number type (an int type or a float type), `I` for an int
  type. An int type is `int` (= `i64`), `i8`, `i16`, `i32`, `u8`, `u16`, `u32` or `u64`; a float type is `float`
  (= `f64`) or `f32`.
- `body...` is zero or more forms run in order; its value is the last one's (unit when there is none, or when the
  body's value is dropped, as in `when` and `while`).
- `x...` is one or more of `x`.
- **Compile error** means the program does not run at all, and the message names the line and column. **Runtime
  error** stops the program with the line it happened on (exit status 1).

## Index

| name | what it is | section |
|---|---|---|
| `!=` | not equal | [Comparing](#comparing) |
| `%` | remainder of ints | [Numbers](#numbers) |
| `*` | multiply | [Numbers](#numbers) |
| `+` | add | [Numbers](#numbers) |
| `-` | subtract, negate | [Numbers](#numbers) |
| `.` | a field of an expression's value | [Structs](#structs) |
| `...` | any number of the element before it, in a macro | [Macros](#macros) |
| `/` | divide | [Numbers](#numbers) |
| `:` | gives a type: `(x : int)` | [Reading](#reading) |
| `<` | less than | [Comparing](#comparing) |
| `<=` | less than or equal | [Comparing](#comparing) |
| `=` | equal | [Comparing](#comparing) |
| `>` | greater than | [Comparing](#comparing) |
| `>=` | greater than or equal | [Comparing](#comparing) |
| `NodePath` | the type of `$Player/Sprite` | [Types](#types) |
| `_` | no name: matches anything in `match` arms and macro patterns; a `for` that only repeats | [match](#match), [for](#for), [Macros](#macros) |
| `abs` | a value without its sign | [Comparing](#comparing) |
| `acos` | arc cosine | [Math](#math) |
| `and` | all true, stops at the first false | [Logic](#logic) |
| `append-file` | add text to the end of a file | [Files and the system](#files-and-the-system) |
| `arena` | memory handed back at once when the body ends | [Memory](#memory) |
| `args` | the command line after the file | [Files and the system](#files-and-the-system) |
| `asin` | arc sine | [Math](#math) |
| `assert` | a runtime error when a condition is false | [Control](#control) |
| `atan` | arc tangent | [Math](#math) |
| `atan2` | the angle of a point (y, x) | [Math](#math) |
| `bit-and` | the bits set in all | [Bits](#bits) |
| `bit-not` | every bit flipped | [Bits](#bits) |
| `bit-or` | the bits set in any | [Bits](#bits) |
| `bit-xor` | the bits set in an odd number | [Bits](#bits) |
| `bool` | `true` or `false` | [Types](#types) |
| `byte` | one byte of a string, as an int | [Strings](#strings) |
| `canvas-pixel` | a pixel of a canvas | [Drawing](#drawing) |
| `ceil` | round up to a whole number | [Math](#math) |
| `chain` | hand a value through steps: `(chain x (f a) g)` | [Control](#control) |
| `char` | the one-byte string of a byte | [Strings](#strings) |
| `clamp` | a value kept between two bounds | [Comparing](#comparing) |
| `compile-error` | in a macro's template: a compile error with these words where the macro is used | [Macros](#macros) |
| `coroutine` | start a function in a coroutine, a function on a stack of its own; its type | [Coroutines](#coroutines) |
| `coroutine-time` | the coroutines' clock | [Coroutines](#coroutines) |
| `cos` | cosine | [Math](#math) |
| `def` | a global | [Top level](#top-level) |
| `defmacro` | a macro | [Macros](#macros) |
| `defn` | a function | [Top level](#top-level) |
| `disable` | turn a gem off | [Gems](#gems) |
| `do` | forms in order, the last one's value | [Control](#control) |
| `done?` | whether a coroutine has ended | [Coroutines](#coroutines) |
| `draw-circle` | a circle on a canvas | [Drawing](#drawing) |
| `draw-clear` | a canvas all one color | [Drawing](#drawing) |
| `draw-line` | a line on a canvas | [Drawing](#drawing) |
| `draw-rect` | a rectangle on a canvas | [Drawing](#drawing) |
| `enable` | turn gems on, loading them | [Gems](#gems) |
| `enum` | a type with named values | [Enums](#enums) |
| `env` | an environment variable, `""` when unset | [Files and the system](#files-and-the-system) |
| `eprint` | print to stderr | [Printing](#printing) |
| `eprintln` | print a line to stderr | [Printing](#printing) |
| `exit` | end the program with a status | [Files and the system](#files-and-the-system) |
| `exp` | e to a power | [Math](#math) |
| `f32` | a 32-bit float; converts to it | [Types](#types), [Numbers](#numbers) |
| `f64` | the same as `float` | [Types](#types) |
| `false` | the bool | [Types](#types) |
| `find` | where a part of a string starts | [Strings](#strings) |
| `float` | a 64-bit float; converts to it | [Types](#types), [Numbers](#numbers) |
| `floor` | round down to a whole number | [Math](#math) |
| `fn` | a function without a name, a closure; a function type | [Functions](#functions) |
| `for` | go over a range, a vec, a map's keys or a pool | [Control](#control) |
| `format` | print into a new string | [Printing](#printing) |
| `gem` | a gem's manifest | [Gems](#gems) |
| `get` | an element of a vec or a pool | [Vecs](#vecs), [Pools](#pools) |
| `getenv` | an environment variable as an option | [Files and the system](#files-and-the-system) |
| `gpu-each` | a body run once per element of a vec, on the GPU when there is one | [GPU](#gpu) |
| `handle` | the type of a pool element's name | [Pools](#pools) |
| `has` | whether a map has a key, a pool an element | [Maps](#maps), [Pools](#pools) |
| `i16` `i32` `i64` `i8` | signed int types; convert to them | [Types](#types), [Numbers](#numbers) |
| `if` | choose a branch | [Control](#control) |
| `insert` | add or replace a map entry | [Maps](#maps) |
| `int` | a 64-bit int; converts to it | [Types](#types), [Numbers](#numbers) |
| `join` | strings put together with a separator | [Strings](#strings) |
| `keep` | copy a value out of an arena | [Memory](#memory) |
| `key-down?` | whether a key is down over a window | [Drawing](#drawing) |
| `keys` | a map's keys, in order | [Maps](#maps) |
| `len` | the length of a string, vec, map or pool | [Strings](#strings), [Vecs](#vecs) |
| `let` | locals for a body | [Control](#control) |
| `ln` | natural logarithm | [Math](#math) |
| `lookup` | a map's value or a pool's element, as an option | [Maps](#maps), [Pools](#pools) |
| `map` | a map; the map type | [Maps](#maps) |
| `match` | choose by a value's shape | [match](#match) |
| `max` | the largest of values | [Comparing](#comparing) |
| `min` | the smallest of values | [Comparing](#comparing) |
| `mouse-down?` | whether a mouse button is down over a window | [Drawing](#drawing) |
| `mouse-x` | the pointer's x over a window | [Drawing](#drawing) |
| `mouse-y` | the pointer's y over a window | [Drawing](#drawing) |
| `name-of` | a name joined from a macro's parts, like C's `##` | [Macros](#macros) |
| `nand` | not all true | [Logic](#logic) |
| `new-canvas` | an image the GPU draws on | [Drawing](#drawing) |
| `none` | the empty option | [Options](#options) |
| `nor` | none true | [Logic](#logic) |
| `not` | the opposite bool | [Logic](#logic) |
| `open-window` | a window to draw on | [Drawing](#drawing) |
| `option` | the option type | [Options](#options) |
| `or` | any true, stops at the first true | [Logic](#logic) |
| `parse-float` | a float from a string | [Strings](#strings) |
| `parse-int` | an int from a string | [Strings](#strings) |
| `pid` | the process id | [Files and the system](#files-and-the-system) |
| `pipe` | commands joined like bash's `\|`, their output | [Files and the system](#files-and-the-system) |
| `pipe-in` | the same, with text on the first one's stdin | [Files and the system](#files-and-the-system) |
| `poll` | a coroutine's result if it has ended, at once: an option | [Coroutines](#coroutines) |
| `pool` | a fixed number of slots with generational handles | [Pools](#pools) |
| `pop` | take a vec's last element off | [Vecs](#vecs) |
| `pow` | a number to a power | [Math](#math) |
| `print` | print to stdout | [Printing](#printing) |
| `println` | print a line to stdout | [Printing](#printing) |
| `push` | add at a vec's end | [Vecs](#vecs) |
| `put` | replace an element of a vec or a pool | [Vecs](#vecs), [Pools](#pools) |
| `read-file` | a whole file | [Files and the system](#files-and-the-system) |
| `read-line` | the next line of stdin | [Files and the system](#files-and-the-system) |
| `remove` | take out a map entry or a pool element | [Maps](#maps), [Pools](#pools) |
| `return` | leave the function now, with a value | [Control](#control) |
| `run` | run a command; true when it succeeds | [Files and the system](#files-and-the-system) |
| `run-coroutines` | run the coroutines until none is left | [Coroutines](#coroutines) |
| `save-png` | a canvas as a PNG file | [Drawing](#drawing) |
| `set` | give a variable or a field a new value | [Control](#control) |
| `sh` | run a command, get its output | [Files and the system](#files-and-the-system) |
| `shl` | shift left | [Bits](#bits) |
| `show-frame` | draw what is batched for a canvas | [Drawing](#drawing) |
| `shr` | shift right | [Bits](#bits) |
| `sin` | sine | [Math](#math) |
| `sleep` | pause a coroutine for seconds; in the main program, sleep | [Coroutines](#coroutines) |
| `some` | an option with a value | [Options](#options) |
| `spawn` | put an element into a pool | [Pools](#pools) |
| `split` | the pieces between separators | [Strings](#strings) |
| `sqrt` | square root | [Math](#math) |
| `step-coroutines` | one step of the coroutines | [Coroutines](#coroutines) |
| `string` | the string type | [Types](#types) |
| `string-of` | a macro's parts as a string, like C's `#` | [Macros](#macros) |
| `struct` | a type with fields | [Structs](#structs) |
| `substr` | bytes from one index up to another | [Strings](#strings) |
| `system` | run a command, its output passing through; its status | [Files and the system](#files-and-the-system) |
| `tan` | tangent | [Math](#math) |
| `trim` | a string without the space around it | [Strings](#strings) |
| `true` | the bool | [Types](#types) |
| `u16` `u32` `u64` `u8` | unsigned int types; convert to them | [Types](#types), [Numbers](#numbers) |
| `unit` | no value | [Types](#types) |
| `use` | compile another file first | [Top level](#top-level) |
| `values` | a map's values, in order | [Maps](#maps) |
| `vec` | a growable array; the vec type | [Vecs](#vecs) |
| `when` | a body when a condition holds | [Control](#control) |
| `while` | a body while a condition holds | [Control](#control) |
| `write-file` | write a whole file | [Files and the system](#files-and-the-system) |
| `xnor` | an even number true | [Logic](#logic) |
| `xor` | an odd number true | [Logic](#logic) |
| `yield` | a coroutine pauses until the next step | [Coroutines](#coroutines) |

## Reading

- `;` starts a comment that runs to the end of the line.
- A list is `(` forms `)`, nested at most 256 deep. `()` is an error where a form is expected. When the parens do not
  balance, gael reads the indentation the way parinfer does - a code line's first column ends the lists opened at that
  column or right of it - and names the line where a `)` is missing or one too many:
  `a ')' is missing at the end of line 4: line 5 is indented as if the ( at 4:8 ended there`. When the indentation
  does not add up either, it says `unexpected ')'` and `'(' not closed`.
- An **int** literal is decimal (`42`, `-7`, `+3`), hex (`0xff`, `0XFF`) or binary (`0b1010`), with an optional sign.
  It must fit in 64 bits; one above 2^63 - 1 fits only a `u64`. Its type is the one expected where it stands - a
  declared type, a parameter, a field, the other operand of an operator - and it must fit that type: `(def b : u8 300)`
  is a compile error. With nothing expected it is an `int`.
- A **float** literal has digits on both sides of the point and an optional exponent: `4.2`, `-0.5`, `4.2e3`, `1.5e-3`.
  `1e3`, `.5` and `5.` are compile errors. Its type is `f32` where an `f32` is expected, else `float`.
- A **string** is `"..."` and may span lines. Escapes: `\n`, `\t`, `\\`, `\"`, `\0`; any other `\x` is a compile error.
  A string not closed is a compile error.
- A **symbol** is any run of characters other than space, tab, newline, `(`, `)`, `"` and `;`. A token that starts
  like a number but is not one (`3x`, `1.2.3`) is a compile error.
- A **keyword** is a symbol that starts with `:` and has more after it: `:name`. It names a field in
  `(Vec2 :x 1.0 :y 2.0)`, a manifest key in `(gem ...)`, and a literal in a macro pattern.
- `x.y.z` is one symbol: the field `z` of the field `y` of the variable `x` (also `Dir.north`, a value of an enum).
  Right after a closing parenthesis, `.y.z` reads as `(. (...) y.z)`, so `(get bodies 0).pos.x` is the field of an
  expression's value.
- `$Player/Sprite` is a node path, a value of type `NodePath`.
- `@` starts what is for the tools and for while working on the program, never part of a release: `@export` before a
  `def` (see [Top level](#top-level)), and the forms `(@warn "...")`, `(@err "...")`, `(@debug ...)` and
  `(@test "name" ...)` (see [Tests and marks](#tests-and-marks)).
- `:` gives a type: `(def hp : int 100)`, `(x : float)`, `(defn f () : int ...)`.

## Types

| type | cells | values | `=` and `!=` | `<` ... | map key | `match` |
|---|---|---|---|---|---|---|
| `int` (`i64`) | 1 | 64-bit signed, wraps | yes | yes | yes | int literals, `_` |
| `i8` `i16` `i32` | 1 | signed, wraps in its size | yes | yes | yes | int literals, `_` |
| `u8` `u16` `u32` `u64` | 1 | unsigned, wraps in its size | yes | yes (unsigned) | yes | int literals, `_` |
| `float` (`f64`) | 1 | 64-bit IEEE | yes (IEEE: NaN is not equal to itself) | yes | no | no |
| `f32` | 1 | 32-bit IEEE, kept rounded | yes | yes | no | no |
| `bool` | 1 | `true`, `false` | yes | no | yes | `true`, `false` |
| `string` | 1 | bytes, any length; compared by content | yes | no | yes | string literals, `_` |
| `NodePath` | 1 | `$Player/Sprite` | yes | no | yes | no |
| `unit` | 0 | none: the "value" of `println`, `set`, `while`... | - | - | - | - |
| struct | its fields | see [Structs](#structs) | no | no | no | no |
| enum | 1 | see [Enums](#enums) | yes | no | yes | its values |
| `(option T)` | 1 + T's | `(some x)`, `none` | no | no | no | `(some x)`, `none` |
| `(vec T)` | 1 | a reference to a growable array | no | no | no | no |
| `(map K V)` | 1 | a reference to a hash map, in insertion order | no | no | no | no |
| `(pool T)` | 1 | a reference to a fixed number of slots | no | no | no | no |
| `(handle T)` | 1 | a slot of a `(pool T)` and its generation | yes | no | yes | no |
| `(fn (T...) R)` | 1 | a function or a closure | no | no | no | no |
| `(coroutine T)` | 1 | a running function | no | no | no | no |

- Nothing converts on its own: `(+ 1 2.0)` and `(+ x y)` with an `int` and an `i32` are compile errors. The number
  types convert on purpose: `(float x)`, `(int x)`, `(u8 x)`... (see [Numbers](#numbers)).
- A struct is a value: assigning, passing or putting it in a vec copies it. A vec, a map, a pool, a function value
  and a coroutine are references: copying one shares what it refers to.
- A `(vec ...)`, `(map ...)`, `(pool ...)`, `(option ...)` or `(handle ...)` of `unit` is a compile error;
  `(coroutine unit)` is fine (a coroutine that returns nothing).
- An unknown type name is a compile error: `unknown type X`.

## Top level

These forms are only allowed at the top level of a file; elsewhere they are a compile error. Everything else at the
top level runs, in order, when the program starts (after the files it uses).

### def

```
(def name value)
(def name : T value)
@export (def name : T value)
```

A global. Without `: T` its type is the value's. A global's value is set when its `def` runs; a function that reads
it before that sees zero (`0`, `""`, an empty function value - calling one is a runtime error).

Compile errors: no name or no value, a dot in the name, a built-in name, a name already a function, a type, a macro or
a global (the REPL may define a global again with the same type), a value of another type than declared, a unit
value.

`@export` before a `def` marks the global for an editor's inspector; `gael -x FILE` lists them as
`name : type = value`, with the value when it is a literal. An unknown annotation, or `@export` before anything but a
`def`, is a compile error.

### defn

```
(defn name ((param : T)...) body...)
(defn name ((param : T)...) : R body...)
```

A function. `()` when it takes nothing; without `: R` it returns `unit`. The value of its last form is its result
and must have type `R`. A function can be called above its definition and can call itself. At most 8 parameters; a
parameter cannot be `unit`.

A call that is the function's last act - the last form of the body, and from there both branches of an `if` and the
last form of a `do`, `let`, `when` or `match` arm - is a tail call: it reuses the frame, so recursion there runs any
number of times. Other calls nest; about 4000 deep the program stops with `stack overflow`.

Compile errors: no name or no parameter list, a parameter that is not `(name : type)`, a parameter twice, a dot or a
built-in name, the name of a type, a macro or a global, a body of another type than `R`; defining one twice (the REPL
may define it again with the same signature, and the new body replaces the old).

### use

```
(use "path/file.gael")
```

Compile that file first, its path relative to this file's folder. All files of a program share one set of names.
Each file is compiled once; the program runs only when every file compiled without an error, the used files' top
levels first. A file that ends up using itself is a compile error, and so is a missing file. A macro cannot make a
`use`.

### struct, enum, defmacro, gem, enable, disable

See [Structs](#structs), [Enums](#enums), [Macros](#macros) and [Gems](#gems).

## Control

### do

```
(do body...)
```

Runs the forms in order; its value is the last one's. `(do)` is unit.

### let

```
(let ((name value) (name : T value)...) body...)
```

Locals for the body. Each binding is made in turn, so a later value can use an earlier name. Without `: T` a binding's
type is its value's; an empty `(vec)`, `(map)` or `none` needs the type: `(let ((v : (vec int) (vec))) ...)`. A local
hides a global, a function or a macro of the same name (one namespace). Its value is the body's.

Compile errors: a binding that is not `(name value)` or `(name : T value)`, a built-in name or a dot, a value of
another type than declared, a unit value.

### set

```
(set name value)
(set name.field.field value)
```

A new value for a local, a global or a field of one; the value must have the variable's or field's type. Its value
is unit. In a closure, setting a local of the code around it changes the closure's own copy (see
[Functions](#functions)).

Compile errors: not a name, an unknown name, a value of another type, and a value made in an `arena` going to a
variable from outside it.

### if, when, while

```
(if condition then)
(if condition then else)
(when condition body...)
(while condition body...)
```

The condition must be a `bool`. `if` with both branches has their type, and they must match (an `else` like `none`
takes the then branch's type); without `else` it is unit. `when` runs the body when the condition is true; `while`
runs it while the condition is true. Both are unit.

### for

```
(for (i from to) body...)      ; i = from, from + 1, ..., to - 1; ints
(for (x v) body...)            ; each element of a vec; x is a copy of it
(for (k m) body...)            ; each key of a map, in insertion order (the keys as they were when the loop began)
(for (h p) body...)            ; the handle of each element of a pool, in slot order
(for (x s) body...)            ; a struct or an enum with len and get: (get s 0), (get s 1)... up to (len s)
```

Unit. `x`, `k`, `h` and `i` are locals of the body; `_` in their place gives no name, for a loop that only repeats:
`(for (_ 0 3) (println "hi"))`. Compile errors: not one of these shapes, a range end or start that is not an int,
something else to go over ([Methods and hooks](#methods-and-hooks)).

### match

```
(match value (pattern body...)...)
```

Runs the body of the first arm whose pattern fits the value; the arms' values must have one type, which is the
match's (an arm's body may be empty: unit). Patterns, by the value's type:

- `bool`: `true`, `false`;
- an int type: int literals;
- `string`: string literals;
- an enum: its values, `north` or `Dir.north`;
- an option: `(some name)` - the value inside, as a local of the arm (`(some _)` takes nothing) - and `none`;
- any of them: `_`, which fits everything.

Every value must be covered (an int or a string needs a `_` arm), and an arm that can never run - after `_`, or a
value already covered - is a compile error. Matching a struct, a float, a vec, a map or a pool is a compile error.

### Logic

```
(and b...)   (or b...)   (nand b...)   (nor b...)   (xor b...)   (xnor b...)   (not b)
```

On `bool`s. `and` is true when all are, `or` when any is, `nand` and `nor` are their opposites; these four stop at the
first value that decides. `xor` is true when an odd number are true and `xnor` when an even number are; every value is
evaluated. `not` takes exactly one. A value that is not a bool is a compile error (the message says when `(run "cmd")`
was meant).

### return

```
(return value)
(return)                 ; in a function that returns unit
```

Leaves the function (a `defn` or a `fn`) at once with `value`, which must have the function's return type; what the
function's locals, the matches and the loops around hold is let go on the way out. It stands where a statement goes -
`(when (< hp 0) (return false))`, an arm of a `match`, a body of a `for` - or anywhere no value is waiting for the rest
of an expression. Since it never gives a value where it stands, it fits in any branch: `(if bad (return none) x)`.
`(return (f x))` is not a tail call; a call as the function's last form is.

Compile errors: `return` outside a function, inside an `arena` (let it end first), in the middle of an expression with
values already worked out (`(+ 1 (return 2))`), among the fields of a struct being made by name, more than one value,
a value in a unit function or none in another, a value of another type.

### assert

```
(assert condition)
(assert condition "what went wrong")
```

Unit. When the condition is false, the program stops with a runtime error: `assertion failed: (> hp 0)` - the
condition as written - or the message given. It costs one test when the condition holds. Compile errors: a condition
that is not a `bool`, a message that is not a string literal, more than two values.

### chain

```
(chain x step...)
```

Hands a value through steps, like a bash pipe: each step gets the value so far as its first argument. A step is a
function name or value `f` - called as `(f value)` - or a call `(f a b)` - called as `(f value a b)`. So
`(chain x (f a) g)` is `(g (f x a))`.

## Functions

```
(fn ((param : T)...) body...)
(fn ((param : T)...) : R body...)
(fn (T...) R)                 ; the type
```

A function without a name, as a value of type `(fn (T...) R)`. A function's name is such a value too (`inc`), as is
any variable, field or expression of a function type, and each is called the same way: `(f 3)`, `(b.on-click 2)`,
`((get handlers 0) 5)`. A local of the same name hides a function.

A `fn` can use the locals of the code around it (a closure): it keeps a copy of each one it uses, made when the `fn`
is made. Setting one inside changes the fn's own copy, which stays from call to call; the code around it keeps its own.
A vec, a map or a pool is a reference, so what is in it is shared. A `fn` made in a loop keeps that turn's values.

Compile errors: as for `defn`; calling a value that is not a function (`n is int, not a function`); the wrong number
or types of arguments. Runtime error: calling a function value that was never set (a global read before its `def`
ran).

## Numbers

```
(+ a b...)   (- a b...)   (- a)   (* a b...)   (/ a b...)   (% a b...)
(int x)   (float x)   (i8 x)   (i16 x)   (i32 x)   (i64 x)   (u8 x)   (u16 x)   (u32 x)   (u64 x)   (f32 x)   (f64 x)
```

Arithmetic takes two or more numbers of one type (`-` also one, which negates), works left to right and gives that
type; `%` takes ints only. Ints wrap around in their size; `/` rounds toward zero and `%` has the sign of the left
operand (C's rules); `u64` divides and compares unsigned. Dividing an int by zero is a runtime error (`division by zero`); the smallest signed int divided by
-1 wraps. Floats follow IEEE: dividing by zero gives `inf` or `nan`; `f32` results are rounded to 32 bits.

A conversion `(T x)` takes a number of any type: between ints the value wraps into the new size; an int to a float is
the nearest float; a float to an int must fit (else a runtime error, `u8 of a float that does not fit in it (0 to
255)`) and drops the fraction. `(i64 x)` is `(int x)`, `(f64 x)` is `(float x)`.

When the first value is a struct or an enum, the operator calls a function of its type instead: `add`, `sub`, `mul`,
`div`, `rem`, and `negate` for `(- a)` ([Methods and hooks](#methods-and-hooks)).

Compile errors: mixing types (`int and float do not mix; convert one`), a non-number, `%` on floats, the wrong number
of operands.

### Comparing

```
(< a b)   (<= a b)   (> a b)   (>= a b)   (= a b)   (!= a b)
```

Exactly two values of one type; a `bool`. `<` and the like compare numbers. `=` and `!=` also compare bools, strings
(by content), enum values, handles and node paths. For a struct or an enum with an `equal` or a `compare` function
they call that ([Methods and hooks](#methods-and-hooks)). Anything else is a compile error.

```
(min a b...)   (max a b...)   (abs x)   (clamp x lo hi)
```

The smallest and the largest of values of one type that `<` orders (numbers, or a type with a `compare` hook), the
first one on a tie; `x` without its sign; `x` kept between `lo` and `hi`. Each value is worked out once, and a literal
takes the type of the others, as in `<`: `(min hp 100)` with `hp` a `u8`. The four names are built in but free: a
variable, a field, a function or a macro named `min` comes first, so `(let ((max 0)) ...)` and a `Rect` with fields
`min` and `max` are fine.

### Math

```
(sqrt x)   (sin x)   (cos x)   (tan x)   (asin x)   (acos x)   (atan x)   (atan2 y x)
(exp x)    (ln x)    (pow x y)  (floor x)  (ceil x)
```

Functions of `float`s, giving a `float`, the C library's - and of `f32`s, giving an `f32` (worked out as a float,
then rounded): `atan2` is the angle of the point `(x, y)` from the x axis,
between -pi and pi; `ln` is the natural logarithm (the name `log` stays free for the program); `floor` and `ceil` round
down and up and stay floats (`(int (floor x))` makes an int). Angles are in radians. Outside a function's domain the
answer is `nan` or `inf`, as IEEE says (`(sqrt -1.0)` is `nan`), not an error.

### Bits

```
(bit-and a b...)   (bit-or a b...)   (bit-xor a b...)   (bit-not a)   (shl a n)   (shr a n)
```

On ints of one type. A shift count must be 0 to the type's bits - 1, else a runtime error (`a shift by 64 does not fit
a 64-bit int`); `shr` keeps the sign of a signed type.

## Strings

A string is bytes; indexes count bytes from 0. The program's own strings are never copied; one made at run time is
freed when its last owner lets go.

| form | gives | |
|---|---|---|
| `(len s)` | `int` | its length in bytes (also of a vec, a map or a pool) |
| `(format "fmt" x...)` | `string` | see [Printing](#printing) |
| `(substr s from to)` | `string` | the bytes from `from` up to (not including) `to`; runtime error unless 0 <= from <= to <= length |
| `(byte s i)` | `int` | byte `i`, 0 to 255; runtime error when `i` is outside |
| `(char n)` | `string` | the one-byte string of byte `n`; runtime error unless 0 <= n <= 255 |
| `(find s part)` | `(option int)` | where `part` first starts, or `none` |
| `(split s sep)` | `(vec string)` | the pieces between the separators, empty ones too: `(split "a,,b" ",")` is `(vec "a" "" "b")`; runtime error when `sep` is empty |
| `(join v sep)` | `string` | the strings of a `(vec string)` with `sep` between them |
| `(trim s)` | `string` | without the spaces, tabs, carriage returns and newlines at both ends |
| `(parse-int s)` | `(option int)` | `(some n)` when the whole string, spaces around it aside, is an int (decimal); else `none` |
| `(parse-float s)` | `(option float)` | the same for a float |

## Options

```
(some x)                     ; (option T) when x is a T
none                         ; (option T): its type comes from where it goes
(option T)                   ; the type
```

`none` takes its type from where it goes: a declared binding or global, a parameter, a field, a function's return
type, or the else branch of an `if` whose then branch has one. Where nothing says it, it is a compile error. `(none)`
is a compile error (write `none`). The value inside an option comes out with `match`.

## Structs

```
(struct Name (field : T)... (field : T default)...)
(Name value...)              ; the fields in order; the last ones may be left out when they have defaults
(Name :field value...)       ; by name, in any order; a field with a default may be left out
v.field   v.field.field   (. expr field.field)   (expr).field
```

A struct type, declared at the top level, usable anywhere in the program. A field's type can be any type declared
above it. The struct itself can be in its fields only inside a `vec`, a `pool` or a `handle` (an option of it inside
those too: `(vec (option T))`), which makes a tree; directly, or in a plain option, its size would have no end, a
compile error. Named and positional values cannot mix. Field access works on variables (`b.pos.x`) and on any
expression's value (`(get bodies 0).pos.x`); `set` works on a variable's fields.

A default is any expression of the field's type: `(hp : int 100)`, `(items : (vec Item) (vec))`,
`(pos : Vec2 (Vec2 0.0 0.0))`, `(hp : int (* 2 base-hp))`. It is worked out each time a struct is made without that
field, so each one gets its own `(vec)`; one made of constants is copied in like a literal, without a call (in code
that comes after the struct). Its names mean
what they mean at the top level, wherever the struct is made. When the struct has an `init` function,
`(Name value...)` calls it instead, everywhere but in the struct's own functions
([Methods and hooks](#methods-and-hooks)); `(Name :field value...)` always makes the struct directly.

Compile errors: a field twice, an unknown field type, a struct that holds itself directly, the wrong number of values
(a field left out with no default), a missing or unknown field name, a value of another type, a default of another
type, a field that does not exist (the message lists the fields).

## Enums

```
(enum Name value...)
Name.value
```

A type with named values. In a `match`, a value can be written bare (`north`). Enum values compare with `=`, can be map
keys and print as their name.

## Methods and hooks

```
(defn Type.name ((x : Type) ...) ...)   ; a function of a struct or an enum
(Type.name x ...)                       ; called like any function; Type.name is a function value too
(x.name ...)                            ; a method call: (Type.name x ...)
((expr).name ...)                       ; the same on an expression's value: ((get ps 0).hurt 3)
(v.push 4)   (s.len)                    ; no function of the type: (push v 4), (len s)
```

A function whose name is a type, a dot and a name belongs to that struct or enum. It is declared in the gem (or the
program) that declares the type, and it takes no hidden `self`: the value is its first parameter, written out.
`(x.name args...)`, where `x` is a variable or a field path like `p.pos` and `name` is none of its fields, calls
`(Type.name x args...)` when `x`'s type has that function, and else `(name x args...)` with a builtin or a function
of that name. A field of the same name comes first. `((expr).name args...)` does the same with the value of any
expression, worked out once, before the arguments. Values are copied into a call, so a method cannot change the
variable it was called on; it gives a new value, which `set` keeps: `(set p (p.hurt 10))`. Calling such a method as a
statement, where its new value would be dropped, is a compile error.

Hooks are functions with these names; the forms call them for values of the type, chosen when compiling, so they cost
what a call costs:

| hook | called by | it takes | it gives |
|---|---|---|---|
| `add` `sub` `mul` `div` `rem` | `+` `-` `*` `/` `%` with a value of the type first | the value and the next one, of any one type | any value; `(+ a b c)` is `(T.add (T.add a b) c)` |
| `negate` | `(- x)` | the value | any value |
| `equal` | `=`, and `!=` as its opposite | two values of the type | `bool` |
| `compare` | `<` `<=` `>` `>=`, and `=` when there is no `equal` | two values of the type | `int`: below 0 when the first comes first, 0 when equal |
| `text` | `%v` and `%s` in `print`, `println`, `format`... | the value | `string` |
| `len` | `(len x)`; `for` | the value | `int` |
| `get` | `(get x i)`; `for`, with an `int` index from 0 up to `(len x)` | the value and an index of any type | the element |
| `put` | `(put x i e)` | the value, an index and an element | unit |
| `init` | `(Type args...)` outside Type's own functions | anything | the struct |

Inside Type's own functions (and its fields' defaults) `(Type values...)` makes the struct itself, so `init` can:
`(defn Player.init ((name : string)) : Player (Player :name name :hp 50))`. `examples/methods.gael` has all of them.

Compile errors: a dot in a function name that does not come after a struct or an enum declared in the same gem, two
dots, a name that is one of the enum's variants; a hook missing for a form used on the type (the message shows its
signature), or with the wrong parameters or result; a method that neither the type nor a builtin or a function has;
dropping a method's new value; an `init` that does not give the struct.

## Vecs

```
(vec x...)          ; (vec T)
(vec)               ; empty: its type comes from where it goes
(push v x)   (get v i)   (put v i x)   (pop v)   (len v)
```

A growable array; a reference, so copies share it. `push` adds at the end (unit); `get` gives a copy of element `i`;
`put` replaces it (unit); `pop` takes the last one off and gives `(some x)`, or `none` when it is empty; `len` counts.
An index outside is a runtime error (`index 5 is outside a vec of 3`). A vec of structs is kept as columns, one per
field (SoA); the program sees ordinary values. A column of bools is a run of bits, 64 to a cell: `(vec bool)` and a
bool field of a struct in a vec (or a pool) take one bit per element instead of a cell, so a million flags are 125 KB.

## Maps

```
(map (key value)...)   (map)
(insert m key value)   (lookup m key)   (has m key)   (remove m key)   (keys m)   (values m)   (len m)
(map K V)              ; the type
```

A hash map that keeps its entries in insertion order; a reference. The key type is an int type, `bool`, `string`,
an enum, a handle or `NodePath`. `insert` adds or replaces (unit); `lookup` gives `(some value)` or `none`; `has`
says whether the key is there; `remove` takes the entry out and gives `(some old-value)` or `none`; `keys` and
`values` give vecs in order; `len` counts the entries.

## Pools

```
(pool n)                          ; (pool T): n slots, which never grow; T from where it goes
(spawn p x)   (get p h)   (put p h x)   (lookup p h)   (has p h)   (remove p h)   (len p)
(pool T)   (handle T)             ; the types
```

A fixed number of slots for things that come and go - entities, particles - stored as columns like a vec. `spawn`
puts `x` in a free slot and gives its `(handle T)`: the slot and the generation the slot had. `get` and `put` read and
replace by handle; `lookup` gives `(some x)`, or `none` when the element was removed; `has` says whether it is still
there; `remove` takes it out (`(some x)` or `none`) and frees the slot for a new generation; `len` counts the elements.
A handle kept after its element was removed never reaches the element that took the slot later.

Runtime errors: `(pool n)` with n below 1 or above 2147483647; spawning into a full pool (`the pool is full (100
elements)`); `get` or `put` with a stale handle (`a stale handle: its element was removed from the pool`).
Compile error: a handle of another pool's element type.

## Memory

Strings made at run time, vecs, maps, pools, closures and coroutines count their owners; the compiler puts the counting
in, and when the last owner lets go the object is freed on the spot, with what it holds. A read that only looks
(`len`, `get`, `=`, `lookup`) borrows and counts nothing. A cycle - a vec that holds itself - is never freed.

```
(arena body...)
(keep x)
```

An `arena` gives the strings, vecs, maps and pools that the body makes - also in the functions it calls - one block,
handed back at once when the body ends; its value is the body's last one. Its objects are counted too, so an arena
that ends while one of them is still held (in a global, in a cycle) is a runtime error naming the arena's line, and a
value made in the arena going to a variable from outside it - by `set`, `push`, `put`, `spawn`, `insert`, or as the
arena's own value - is a compile error. `keep` copies what a value holds from the innermost arena onto the heap, so
it outlives the arena. Arenas nest up to 64 deep.

With `GAEL_CHECK=1` freed memory is overwritten, and a program that runs to its end reports what it never freed.

## Printing

```
(print "fmt" x...)   (println "fmt" x...)   (eprint "fmt" x...)   (eprintln "fmt" x...)   (format "fmt" x...)
```

The format must be a string literal; its codes are checked against the values when compiling. `%d` takes any int
type, `%x` too and writes it in hex (`ff8800ff`, as many bits as its type has), `%f` any float type, `%s` a string, `%b` a bool, `%v` any value but unit (printed the way the REPL shows it),
`%%` is a `%`. A struct or an enum with a `text` function prints as what it gives, with `%v` and with `%s`
([Methods and hooks](#methods-and-hooks)); inside a vec, a map or another struct values print the way the REPL shows
them. `println` and `eprintln` add a newline; `eprint` and `eprintln` write to stderr; `format` gives the
text as a new string. Compile errors: a format that is not a literal, an unknown code, more or fewer values than codes,
a value of the wrong type for its code. A float prints with at least one decimal (`3.0`), an `f32` with the fewest
digits that read back as the same value.

## Files and the system

| form | gives | |
|---|---|---|
| `(read-file path)` | `(option string)` | the whole file, or `none` when it cannot be read |
| `(write-file path text)` | `bool` | the file's contents become `text`; true when it worked |
| `(append-file path text)` | `bool` | `text` added at the end; true when it worked |
| `(read-line)` | `(option string)` | the next line of stdin without its newline; `none` at the end |
| `(args)` | `(vec string)` | the arguments after the program's file name |
| `(getenv name)` | `(option string)` | an environment variable |
| `(env name)` | `string` | an environment variable, `""` when it is not set |
| `(pid)` | `int` | the process id |
| `(sh "command")` | `string` | run with `/bin/sh -c`; what it printed, without the newlines at the end |
| `(system "command")` | `int` | run it, its output going where gael's goes; its exit status |
| `(run "command")` | `bool` | run it; true when it exits with 0: `(and (run "make") (run "make install"))` |
| `(pipe "cmd" "cmd"...)` | `string` | the commands joined like bash's `\|`; the last one's output |
| `(pipe-in text "cmd"...)` | `string` | the same, with `text` on the first command's stdin |
| `(exit status)` | unit | end the program with this exit status |

## Coroutines

```
(coroutine (f x...))      ; start f in a coroutine: a (coroutine R) when f returns R
(poll co)   (done? co)   (sleep seconds)   (yield)   (step-coroutines dt)   (run-coroutines)   (coroutine-time)
(coroutine R)             ; the type
```

`(coroutine (f x...))` runs a named function as a coroutine: a function with a stack of its own, which can pause
anywhere - also deep in the functions it calls - without them saying so. It runs at once, until it first pauses, and
gives a `(coroutine R)`. Nothing ever waits for a coroutine:

- `(poll co)` gives `(some result)` once the coroutine has ended and `none` before, at once - an `(option R)`. A
  coroutine whose function returns unit has no result to poll; `(done? co)` says whether one has ended.
- `(sleep s)` pauses a coroutine for `s` seconds (a float) of coroutine time. In the main program it sleeps in real
  time, running the coroutines meanwhile.
- `(yield)` pauses a coroutine until the next step. Only in a coroutine.
- `(step-coroutines dt)` - from the main program, e.g. once a frame - moves the coroutine time `dt` seconds on and lets
  every coroutine that can go on run until it pauses again; it gives how many coroutines are left.
- `(run-coroutines)` runs the coroutines until none is left, sleeping in real time when they all sleep: the loop of a
  script or a server.
- `(coroutine-time)` gives the coroutine time in seconds.

One coroutine that needs another's result looks each step: `(while (not (done? co)) (yield))`, then `(poll co)`.
A coroutine does not make its strings and vecs in the arenas of the code that resumed it. Runtime errors: a coroutine
pausing inside an `arena`, `yield` outside a coroutine, `step-coroutines` or `run-coroutines` from a coroutine, a
coroutine read from a global before its def ran, polling one stopped by a runtime error (in the REPL). Compile errors:
`(coroutine ...)` of anything but a call of a function by its name, `poll` or `done?` of a non-coroutine, `poll` of a
coroutine that returns unit, `sleep` or `step-coroutines` of a non-float; `await` and `async` say what to write
instead. A coroutine cannot be cancelled from outside yet; one that never ends holds what it holds (and
`GAEL_CHECK=1` counts that as not freed).

## GPU

```
(gpu-each (p particles) body...)       ; body once per element, all at once, on the GPU when there is one
(gpu-each (p particles i) body...)     ; i: the element's index, a u32
```

The body is a GPU kernel written in gael: it runs once per element of the vec, every element at the same time, and
each run writes only its own element. It reads and sets the fields of `p` that are `f32`, `i32` or `u32` (nested ones
too, `p.pos.x`), reads the variables around it of those types (they reach the GPU once per run, as constants), and
has `let`, `set`, `if`, `when`, `do`, arithmetic, comparisons, logic, bits, the math functions, `min`, `max`, `abs`,
`clamp` and the conversions `(f32 x)`, `(i32 x)`, `(u32 x)`. A variable of the body is one of those types or `bool`;
one made from a literal says its type, `(let ((k : f32 0.5)) ...)`, since a literal alone would be 64-bit. Other
fields of the element may be anything; the GPU never sees them. The vec is in a variable: a local or a global.

gael writes the body as WGSL (`gael -g FILE` prints it) and runs it through wgpu-native, which it loads when a
`gpu-each` first runs: `GAEL_WGPU=FILE` names the library, else `lib/libwgpu_native.so` next to the gael binary, in
gael's own folder, or wherever the system finds `libwgpu_native.so`. wgpu runs it on Vulkan (Linux with Wayland or X,
Android), Metal (macOS, iOS), Direct3D 12 or OpenGL. Without the library or a GPU the same body runs on the VM as a
`for` over the vec that puts each element back, so a program works with or without one; `GAEL_GPU=0` keeps
everything on the VM, `GAEL_GPU=1` makes a missing GPU a runtime error. Each field the body uses goes to the GPU as
it is, since a vec keeps each field as a column already, and the fields it sets come back.

A run that would stop the VM's loop with a runtime error - an int divided by zero, a float too big for the int it is
made into, a shift past the type's bits - stops the program the same way on the GPU: the kernel marks the run failed,
nothing of it comes back, and the VM runs the loop, which says where. Floats may differ: WGSL lets division and the
math functions be a few ulps off.

Compile errors, each with what to write: a field or a variable of another type (`float`, `int`, `string`, a struct as a
whole, a `bool` field, which a vec keeps as one bit); setting anything but `p`'s fields and the body's own variables; a
call of a function of the program, printing, a loop, a string, a `gpu-each` inside another; more than 8 fields or 16
values from around in one body.

## Drawing

```
(def c (new-canvas 640 480))         ; an image of that many pixels on the GPU, black; its number
(draw-clear c 0x202030ff)            ; all of it one color, 0xRRGGBBAA
(draw-rect c x y width height color) ; x and y the top left, in pixels from the top left of the canvas
(draw-circle c x y radius color)     ; smoothed over a pixel at its edge
(draw-line c x1 y1 x2 y2 width color)
(show-frame c)                       ; draws what is batched; true while the canvas is there
(canvas-pixel c x y)                 ; a pixel, 0xRRGGBBAA
(save-png c "frame.png")             ; true when the file was written

(def w (open-window "game" 640 480)) ; a window on the screen, a canvas too: the same draw- calls draw into it
(while (show-frame w) ...)           ; shows the frame at the screen's rate; false once the window was closed
(key-down? w "space")                ; true while the key is down
(mouse-x w) (mouse-y w)              ; the pointer over the window, floats in pixels from its top left
(mouse-down? w 1)                    ; 1 the left button, 2 the right, 3 the middle
```

A canvas is an image that the GPU draws on, through wgpu as `gpu-each` runs ([GPU](#gpu)). The shapes are batched and
drawn when the frame is shown, when the batch is full (16384 shapes), or when the program reads a pixel or saves the
canvas; a color's alpha blends it over what is there, and `draw-clear` starts a new frame. Coordinates are floats or
f32s, so a `gpu-each` vec's fields go in as they are. There can be 16 canvases, each up to 8192 pixels each way.
`(format "%x" color)` writes a color the way it is written here.

A window is a canvas on the screen, on Wayland for now (libwayland-client, loaded when the first window opens, finds
the compositor through `WAYLAND_DISPLAY`). Each frame starts as one color, the last `draw-clear`'s (black at first),
because the screen's images do not keep what was drawn before. `show-frame` presents the frame, waiting for the screen
as a game loop wants, then takes in what happened since the last frame: keys, the mouse, a new size from the
compositor, the close button. The keys are named by their place on the keyboard, so `"w"` `"a"` `"s"` `"d"` are the
same four keys on any layout: the letters, the digits, `"f1"` to `"f10"`, `"space"` `"enter"` `"escape"` `"tab"`
`"backspace"` `"minus"` `"equal"` `"up"` `"down"` `"left"` `"right"` `"shift"` `"ctrl"` `"alt"`.

Runtime errors: no GPU (drawing has no VM version), a canvas number that `new-canvas` did not give, a size or a pixel
outside the canvas; no compositor to open a window on, a key name not in the list, a window's keys or mouse asked of a
canvas, a window's pixels read (draw into a canvas to read them), a mouse button other than 1, 2, 3.

## Macros

```
(defmacro name (pattern template)...)
```

A macro rewrites code before its types are checked, as Scheme's `syntax-rules`. The first rule whose pattern matches
a use rewrites it; no match is a compile error that lists the patterns. In a pattern, the first element stands for the
macro's name (write `_`); after it a symbol matches any one form and names it (a pattern variable), `_` matches
anything, a keyword, a number, a string or a built-in name (the `:` of `(name : type)`, `if`) matches only itself, a
list matches a list, and `...` after an element
matches it zero or more times - one `...` per list, with other elements allowed after it. In the template each
pattern variable becomes a copy of what it matched, `x ...` repeats `x` once for each match of the variables in it,
and everything else is copied as it is.

Hygiene: a name the template binds (the `tmp` of a swap) is never the same variable as one of the code that uses the
macro, and a name the template uses without binding it (`if`, a global function, another macro) means what it means at
the top level, even where the use has a local of that name. A macro can be used anywhere in its file (also above its
definition) and in the files that use it. At the top level it can make globals, functions, types and macros; what it
makes with `(do ...)` there is spliced in as separate forms.

A template that turns a use down writes `(compile-error "what is wrong")`: when the use expands to it, that is a compile
error with these words at the place of the use. `gael -m FILE` prints the file with every macro expanded, those inside
functions as well, laid out to read - the code the compiler checks.

Compile errors at the definition: a rule that is not `(pattern template)`, a pattern variable twice or with a dot in
its name, `...` with nothing before it or twice in a list, a variable used under fewer `...` than it was matched under,
a `...` that repeats nothing. At a use: no rule matches, variables under one `...` that matched different numbers of
forms, a macro that keeps expanding into itself.

### string-of, name-of

```
(string-of part...)   -> string
(name-of part...)     -> a name
```

C's `#` and `##`, for templates. Each joins the text of its parts: a string gives what it holds, a name, a keyword or
a number gives itself as written, and `string-of` writes a list as source text, so `(string-of (+ a b))` is
`"(+ a b)"`. `(name-of "print-" v)` with `v` matched to `red` is the name `print-red`; one that starts with `:` is a
keyword, and `(name-of p "." f)` is a field path like `p.hp`. Both are worked out right after the macro expands, the
outer one first (a `string-of` writes its parts as they are, unexpanded), wherever they stand: as a function's name in
a `defn`, as a format string, as a pattern. A `defmacro` in what a macro makes keeps its own for its own uses. Written
by hand outside a macro they work too, where an expression goes.

A made name belongs to the scope of the first name among its parts, as if the code that used the macro had written it:
`(name-of v "-squared")` bound in a `let` of the template is visible to the body the use handed in. With no name among
the parts it is the template's own, hidden from the use like any name the template binds.

X-macros: a list written once, as a macro that hands its entries to whatever macro it is given, and macros that each
make one thing from the entries:

```
(defmacro colors ((_ m more ...) (m more ... (red 0xff0000) (green 0x00ff00) (blue 0x0000ff))))
(defmacro def-enum ((_ name (v rgb) ...) (enum name v ...)))
(defmacro def-names ((_ name (v rgb) ...) (def name (vec (string-of v) ...))))
(defmacro def-printers ((_ (v rgb) ...) (do (defn (name-of "print-" v) () (println "%s is %d" (string-of v) rgb)) ...)))
(colors def-enum Color)            ; (enum Color red green blue)
(colors def-names color-names)     ; (def color-names (vec "red" "green" "blue"))
(colors def-printers)              ; print-red, print-green, print-blue
```

Compile errors: no parts; a list or a float as part of a name; a name that would not read as one (a space,
parenthesis, quote or `;` in it, or it starts like a number); text longer than 4096 bytes.

## Gems

```
(gem name :version "1.0.0" :requires (name...) :provides (name...) :excludes (name...))
(enable name...)
(disable name...)
```

A gem is a package by name: `gems/NAME.gael`, or a folder `gems/NAME/` whose `main.gael` uses the rest, looked for
next to the program, then in gael's own folder, then in the `gems/` that comes with gael, next to its binary. Its main
file starts with the manifest; the keys are optional.

The gems that come with gael:

| gem | what it is |
|---|---|
| `ecs` | an entity component system the way flecs and the C ones go: 64-bit entity ids with a generation, a column per component, tags and components as bits of one mask per entity, queries `(ecs-each World (e c...) :with (tags) :not (tags) body...)`. The header of `gems/ecs.gael` has the whole of it; `examples/ecs.gael` uses it |
| `pqueue` | a priority queue, a binary heap in a vec: `(PQ)`, `(q.push id priority)`, `(q.pop)` and `(q.peek)` give an option, `(len q)`, `(q.clear)`. Priority 1 comes out first, 0 or less after everything; ties in the order they went in. Ported from holyc |
| `events` | events with their tick: `(EventLog)` keeps what happened in order (`add`, `at`, `since`, a `for` over it), to record a run and play it back; `(Schedule)` holds what will happen, and `(s.due tick)` takes out everything up to a tick, by tick. Ported from holyc; `examples/queues.gael` uses both |
| `hfsm` | a hierarchical state machine: `(hfsm Mood (e : Ctx) (alive -) (calm alive) ... :enter ((calm code)...) :exit (...))` makes the enum `Mood` and `(Mood.start s e)`, `(s.go to e)` - exits up to the state the two share, enters down to `to` - `(s.in? t)` and `(s.parent)`. A flat machine is one where every parent is `-`. `examples/guards.gael` gives each entity of an ecs world one |
| `rng` | PCG32: `(Rng.seed seed stream)`, `(r.next)` (0 to 2^32 - 1), `(r.float)` ([0, 1)), `(r.range lo hi)`, `(r.chance p)`. The same seed gives the same numbers everywhere. Ported from holyc |
| `noise` | Perlin's improved gradient noise in 2D: `(noise2 seed x y)`, about -1 to 1, 0 at whole points; `(fbm2 seed x y octaves)`. Ported from holyc |
| `easing` | `lerp`, `lerp-clamped`, `unlerp`, `step`, `smooth`, `smoothstep`, and `ease-in-`, `ease-out-`, `ease-in-out-` `quad`, `cubic`, `sine`, `back`. Ported from holyc |
| `wfc` | wave function collapse, the simple tiled model, up to 64 tiles: `(Wfc.new w h tiles)`, `(w.allow a dir b)` and `(w.allow-all a b)` for which tile may stand by which, `(w.weight t x)`, `(w.fix x y t)`, `(w.solve rng tries)` (true when solved), `(w.tile x y)`. Requires `rng`; `examples/wfc.gael` grows an island |
| `curve` | on one number at a time: `bezier2`, `bezier3`, `hermite`, `catmull-rom`; `morton2` for a Z-order index. Ported from holyc; `examples/procgen.gael` uses these four |
| `geometry` | `(Vec2 x y)` and `(Vec3 x y z)` (0.0 for what is left out) with `+ - *` (by a float) `=` and `(- v)`, `(a.dot b)`, `(v.length)`, `(v.length2)`, `(v.normal)`, `(a.distance b)`, `(a.lerp b t)`, `(Vec3.cross a b)`; `(Rect min max)`, a box from two corners: `(r.width)`, `(r.height)`, `(r.center)`, `(r.contains? p)`, `(r.overlaps? s)`. After holyc's mathx.h; `examples/ecs.gael` and `examples/pool.gael` use it |

`(enable a b)` in the program reads only the manifests of the gems named and loads each one after the gems it
requires. A requirement is met by the gem of that name or by one that provides it; when none is on, the gem of that
name is loaded. A gem's code uses only its own names and those of the gems it requires, checked at each use; the
program uses every gem that is on. In the REPL `(enable x)` compiles a gem into the running program, and
`(disable x)` turns one off: its names cannot be used until it is on again.

Compile errors: no such gem; a requirement that no gem provides; gems that require each other; two gems on that
provide the same thing (`gems ecs-archetype and ecs-sparse both provide ecs; turn on one of them`); a gem that excludes
another one that is on; a manifest that is not first, names another gem, or has an unknown key; a file of one gem used
by another; a gem using a name of a gem it does not require; disabling a gem another gem on needs; `enable` in a gem.

## Tests and marks

```
(@test "name" body...)     ; at the top level: a test, with assert in it
@test (defn sample () ...) ; a top-level form that only the tests use
(@debug body...)           ; code for while working on the program
(@warn "text") (@err "text")   ; marks for the tools
```

A test is compiled with the rest of the file - its errors show in every run - and runs only under `gael test FILE...`:
that runs the files' defs (but not the rest of their top level, the program's own run), then each test in a run of its
own, and prints `ok name` or `FAIL name (file:line)` for each and a count; the exit status is 1 when one failed. A
test fails on a runtime error, an `assert` above all. `@test` before a top-level form (a function, a global, a world
of the ecs gem) keeps it for the tests: a release build leaves it out with them.

A gem keeps its tests in its files the same way. `gael test gems/NAME.gael` (or `gems/NAME/main.gael`) turns that gem
on from that file, the gems it requires looked for first in the same folder, and runs its tests; a program that turns
the gem on does not compile them. Each gem that comes with gael has its tests, and `./test` runs them. `(@debug ...)` runs its forms while you work on the program;
`(@warn "...")` and `(@err "...")` give no code at all: `gael -x` lists them, as `file:line: @warn: text`, and an
editor shows them at their line. A release build leaves out everything that starts with `@`.

Compile errors: an annotation gael does not know, `@test` anywhere but the top level or without a name or a form
after it, a mark without one string.

## Warnings

A warning is code that runs but is almost surely a mistake. The program still runs, and stderr gets
`FILE:LINE:COL: warning: MESSAGE` with the source line under it, like an error. The set is fixed, with no flags to
turn one on or off; a release build will stop at any of them.

| message | |
|---|---|
| `x is never used; remove it, or call it _x if it must be there` | a `let` binding nothing reads; `x is given values but never read` when only `set` writes it |
| `i is never used; write _ in its place when only the repeating matters, like (for (_ 0 3) ...)` | the variable of a `for` |
| `x is never used; write (some _) when only the some matters` | the `x` of a `(some x)` pattern |
| `the value of (+ x 1) is thrown away; use it, or remove it` | a value worked out and dropped: a name, a literal, arithmetic, a comparison, a read such as `get` or `len`. A call is not one, since it may do something, nor an `and` or an `or` of commands, which run as bash's `&&` and `\|\|` |
| `this never runs: the return on line N comes first; remove it, or move it above` | forms after a `return` in the same body, or after an `exit` |

Never warned about: a name that starts with `_`, a parameter (a hook or a callback has a fixed shape), what a macro
writes, and a form whose own compile error was reported.

## Runtime errors

Each stops the program (in the REPL, the form) with the line: `FILE:LINE: runtime error: MESSAGE`, exit status 1.

| message | |
|---|---|
| `division by zero` | an int `/` or `%` by 0 |
| `index I is outside a vec of N` | `get` or `put` on a vec |
| `substr F T does not fit a string of N bytes`, `byte I is outside a string of N bytes`, `char takes 0 to 255, this is N` | string indexes |
| `split: the separator is empty` | |
| `T of a float that does not fit in it (RANGE)` | a float to an int conversion |
| `a shift by N does not fit a B-bit int` | `shl`, `shr` |
| `stack overflow (recursion too deep?)`, `stack overflow` | too many nested calls, or a frame too big |
| `this vec, map or pool was never made: a global read before its def ran` | and the same for a function value or a coroutine |
| `a stale handle: its element was removed from the pool`, `the pool is full (N elements)`, `a pool holds 1 to 2147483647 elements, not N` | pools |
| `N strings ... made in this arena are still held when it ends ...`, `arenas nested deeper than 64` | arenas |
| `out of memory (the heap is full)`, `out of memory for strings (the text arena is full)`, `keep: out of memory ...` | memory; `GAEL_HEAP` sets the sizes |
| coroutine errors | see [Coroutines](#coroutines) |

## Command line

```
gael                   the REPL
gael FILE ARGS...      compile FILE and run it when there are no errors; (args) gives ARGS
gael run FILE ARGS...  the same
gael check FILE        only compile: report every error and warning, run nothing (also gael -c)
gael test FILE...      run the files' defs, then each of their (@test "name" ...): ok or FAIL each
gael doc               print docs/MODELS.md, the cheat sheet for writing gael, from next to the binary
gael -d FILE           compile and print the bytecode
gael -m FILE           print FILE with its macros expanded
gael -e CODE ARGS...   compile and run CODE
gael -x FILE           list FILE's @export globals and its (@warn ...) and (@err ...) marks
gael -g FILE           print the WGSL that each gpu-each of FILE runs on the GPU
gael -n                the built-in names, one a line
gael -h                the help
```

Exit status: 0 fine, 1 a compile or runtime error, 2 bad usage; `(exit n)` ends with `n`.

Environment: `GAEL_HEAP=SIZE` sets the heap (and as much again for arenas and for strings), like `64M` or `2G`
(default 1G of address space, given pages only where written). `GAEL_CHECK=1` overwrites freed memory and reports what
a program that ran to its end never freed. `GAEL_HOME=DIR` is gael's own folder (default `~/.gael`); empty, gael uses
none. `GAEL_GPU=0` runs every `gpu-each` on the VM, `GAEL_GPU=1` makes a missing GPU an error; `GAEL_WGPU=FILE` is the
wgpu-native library to load ([GPU](#gpu)).

The REPL: a form may span lines; `:q` quits. Each form is compiled and run on its own, and the value of the last one
is shown as `=> value : type`. A function or a global can be defined again with the same type (the new body or value
replaces the old), a struct or an enum with the same fields or values, a macro freely. It first runs `main.gael` from
gael's folder, and appends every form that ran without an error to `repldump.gael` there, each after a line with its
date and time and the functions of files it gave new bodies,
`;; 2026-10-25 16:28:57, replaces hurt (game.gael:12)`; the files are never written.
`gael < ~/.gael/repldump.gael` replays a session.

## Limits

8 parameters per function; 65536 functions, globals and types; 2^24 instructions; in one file 2^21 forms and 64 MiB of
source and names; 256 nested lists; at run time a stack of 65536 cells and 4096 nested calls (tail calls do not nest),
the same for each coroutine; 64 nested arenas; 64 nested `fn`s and 64 locals a `fn` keeps; 63 pattern variables a macro
rule; 1024 gems. Running out of one is an error that names it.
