/* Tests for types.h. Only stdio besides the header itself. */
#define TYPES_IMPLEMENTATION
#include "core/base.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg)                                         \
    do {                                                         \
        if (!(cond)) { failures++; printf("FAIL  %s\n", msg); }  \
    } while (0)

/* a caller-defined instantiation of each shape */
typedef struct { i32 x, y; } Point;
OPTION_DEFINE(Point, Option_Point)
RESULT_DEFINE(i32, i32, Result_Div)
RESULT_DEFINE(const char *, i32, Result_Str)
FUTURE_DEFINE(i32, Future_i32)

static Result_Div safe_div(i32 a, i32 b)
{
    if (b == 0) return Result_Div_err(-1);
    return Result_Div_ok(a / b);
}

static Result_Div double_safe_div(i32 a, i32 b)
{
    Result_Div r;
    TRY(r, safe_div(a, b));
    return Result_Div_ok(r.as.ok * 2);
}

static Option_i32 first_even(const i32 *v, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (v[i] % 2 == 0) return Option_i32_some(v[i]);
    return Option_i32_none();
}

static Option_i32 double_first_even(const i32 *v, int n)
{
    Option_i32 o;
    TRY(o, first_even(v, n));
    return Option_i32_some(o.value * 2);
}

static i32 deferred;
static void mark(void) { deferred++; }

static void defer_fallthrough(void) { DEFER(mark); }
static int defer_early_return(int flag)
{
    DEFER(mark);
    if (flag) return 1;
    return 0;
}

static void *notifier(void *arg)
{
    static i32 payload = 99;
    async_signal_notify((Async_Signal *)arg, &payload);
    return NULL;
}

int main(void)
{
    Option_i32 a = Option_i32_some(42), b = Option_i32_none();
    Option_f64 c = Option_f64_some(3.5);
    Option_Point p = Option_Point_some((Point){3, 4});
    Result_Str s = Result_Str_ok("fine");
    Future_i32 pending = Future_i32_pending(), ready = Future_i32_ready(7), failed = Future_i32_failed();
    i32 evens[] = {1, 3, 8, 5}, odds[] = {1, 3};

    /* Option */
    CHECK(a.tag == Option_i32_SOME && a.value == 42, "some carries its value");
    CHECK(b.tag == Option_i32_NONE, "none carries the NONE tag");
    CHECK(c.tag == Option_f64_SOME && c.value == 3.5, "a second predefined type works on its own");
    CHECK(p.tag == Option_Point_SOME && p.value.x == 3 && p.value.y == 4, "a struct payload round trips");
    switch (a.tag) {
    case Option_i32_SOME: break;
    case Option_i32_NONE: CHECK(0, "switch must not reach NONE for a some value"); break;
    }

    /* Result and TRY */
    CHECK(double_safe_div(10, 2).tag == Result_Div_OK && double_safe_div(10, 2).as.ok == 10, "TRY continues past success");
    CHECK(double_safe_div(10, 0).tag == Result_Div_ERR && double_safe_div(10, 0).as.err == -1, "TRY returns the inner error unchanged");
    CHECK(s.tag == Result_Str_OK && strcmp(s.as.ok, "fine") == 0, "a pointer payload round trips");
    CHECK(Result_Str_err(7).tag == Result_Str_ERR && Result_Str_err(7).as.err == 7, "err carries its code");

    /* TRY on Option */
    CHECK(double_first_even(evens, 4).tag == Option_i32_SOME && double_first_even(evens, 4).value == 16, "TRY unwraps an Option");
    CHECK(double_first_even(odds, 2).tag == Option_i32_NONE, "TRY propagates NONE");

    /* Future */
    CHECK(pending.tag == Future_i32_PENDING && Future_i32_is_pending(&pending), "pending is pending");
    CHECK(ready.tag == Future_i32_READY && ready.value == 7 && !Future_i32_is_pending(&ready), "ready carries its value");
    CHECK(failed.tag == Future_i32_FAILED && !Future_i32_is_pending(&failed), "failed is not pending");

    /* DEFER */
    deferred = 0;
    defer_fallthrough();
    CHECK(deferred == 1, "DEFER runs at the end of a block");
    CHECK(defer_early_return(1) == 1 && deferred == 2, "DEFER runs on an early return");
    CHECK(defer_early_return(0) == 0 && deferred == 3, "DEFER runs on the normal path too");

    /* Async_Signal */
    {
        Async_Signal sig;
        pthread_t th;
        void *got = NULL, *probe = NULL;

        async_signal_init(&sig);
        CHECK(!async_signal_try_await(&sig, &probe), "try_await is not ready before notify");
        pthread_create(&th, NULL, notifier, &sig);
        got = async_signal_await(&sig);
        pthread_join(th, NULL);
        CHECK(got != NULL && *(i32 *)got == 99, "await returns the notifier's payload");
        CHECK(async_signal_try_await(&sig, &probe) && *(i32 *)probe == 99, "try_await is ready after notify");
        async_signal_destroy(&sig);
    }

    printf("%s\n", failures ? "FAILED" : "types: ok");
    return failures != 0;
}
