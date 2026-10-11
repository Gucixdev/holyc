# types

One header with the small vocabulary types, so they live together: fixed-width integers,
`Option`, `Result`, `TRY`, `Future`, `DEFER` and `Async_Signal`.

```c
#define TYPES_IMPLEMENTATION      // in exactly one .c file, for Async_Signal
#include "types.h"

RESULT_DEFINE(i32, i32, Result_Div)

static Result_Div div(i32 a, i32 b) {
    if (b == 0) return Result_Div_err(-1);
    return Result_Div_ok(a / b);
}

static Result_Div twice(i32 a, i32 b) {
    Result_Div r;
    TRY(r, div(a, b));                       // returns r on failure, like Rust's `?`
    return Result_Div_ok(r.as.ok * 2);
}

switch (twice(10, 2).tag) {                  // taken apart with a switch, not a flag
case Result_Div_OK:  /* ... */ break;
case Result_Div_ERR: /* ... */ break;
}
```

| piece | what it is |
|---|---|
| `OPTION_DEFINE(T, Name)` | tagged `SOME` / `NONE`; `Option_i32`, `Option_i64`, `Option_f32`, `Option_f64`, `Option_ptr` are predefined |
| `RESULT_DEFINE(Ok, Err, Name)` | tagged `OK` / `ERR` with a union payload |
| `TRY(dst, expr)` | early return of the failure, works for Option and Result |
| `FUTURE_DEFINE(T, Name)` | `PENDING` / `READY` / `FAILED`, poll-based |
| `DEFER(fn)` | runs `void fn(void)` when the block exits, on any path |
| `Async_Signal` | mutex + condvar: `notify` from a producer, `await` blocks a consumer |

C has no generics, so every concrete type is one macro invocation with a name you choose.
The generated functions are `static inline`, so the header compiles clean under
`-Wall -Wextra -Werror`. Needs GCC or Clang (`DEFER` uses `__attribute__((cleanup))`) and pthreads.

`make test` builds and runs `test.c`.
