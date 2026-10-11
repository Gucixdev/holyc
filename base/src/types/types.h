#ifndef HOLYC_TYPES_H_
#define HOLYC_TYPES_H_

/* types.h - the small vocabulary types, in one header: fixed-width integers,
 * Option, Result, TRY, Future, DEFER and Async_Signal.
 *
 * Option and Result are tagged unions. A value is taken apart with a plain
 * `switch` on its `.tag`, not with a boolean flag:
 *
 *   OPTION_DEFINE(i32, Option_i32)           // Option_i32 is predefined, this is the pattern
 *   Option_i32 x = Option_i32_some(5);
 *   switch (x.tag) {
 *   case Option_i32_SOME: use(x.value); break;
 *   case Option_i32_NONE: break;
 *   }
 *
 *   RESULT_DEFINE(Wav, Wav_Error, Result_Wav)
 *   Result_Wav r = Result_Wav_ok(wav);
 *   switch (r.tag) {
 *   case Result_Wav_OK:  use(r.as.ok);  break;
 *   case Result_Wav_ERR: fail(r.as.err); break;
 *   }
 *
 * C has no generics, so each concrete type is one macro invocation that takes a
 * caller-chosen `prefix` identifier and generates the type, its tag enum and
 * its constructors.
 *
 * TRY(dst, expr) returns early with the failure, like Rust's `?`. It works for
 * Option and Result alike because both enums list the success case first, so
 * `.tag != 0` always means "failure". The enclosing function must return the
 * same concrete type as `expr`.
 *
 * Future(T) is a poll-based value with a third state, PENDING. Async_Signal is
 * the blocking half: a mutex and condition variable that a producer thread
 * notifies and a consumer thread waits on.
 *
 * Header only. Exactly one translation unit defines TYPES_IMPLEMENTATION before
 * including this file to get the Async_Signal functions. Needs GCC or Clang for
 * DEFER (__attribute__((cleanup))) and pthreads for Async_Signal. */

#if defined(ASYNC_IMPLEMENTATION) && !defined(TYPES_IMPLEMENTATION)
#define TYPES_IMPLEMENTATION   /* the name this header used to have */
#endif

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* ---- fixed-width integers and floats (base.h defines the same names) ---- */
#ifndef HOLYC_BASE_H_
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef float    f32;
typedef double   f64;
#endif

/* ---- Option(T) ---- */
#define OPTION_DEFINE(T, prefix)                                              \
typedef enum { prefix##_SOME, prefix##_NONE } prefix##_Tag;                    \
typedef struct { prefix##_Tag tag; T value; } prefix;                          \
static inline prefix prefix##_some(T v)                                        \
{                                                                              \
    prefix o;                                                                  \
    o.tag = prefix##_SOME;                                                     \
    o.value = v;                                                               \
    return o;                                                                  \
}                                                                              \
static inline prefix prefix##_none(void)                                       \
{                                                                              \
    prefix o;                                                                  \
    o.tag = prefix##_NONE;                                                     \
    return o;                                                                  \
}

/* the common instantiations, so a caller does not have to write them */
OPTION_DEFINE(i32, Option_i32)
OPTION_DEFINE(i64, Option_i64)
OPTION_DEFINE(f32, Option_f32)
OPTION_DEFINE(f64, Option_f64)
OPTION_DEFINE(void *, Option_ptr)

/* ---- Result(Ok, Err) ---- */
#define RESULT_DEFINE(OkType, ErrType, prefix)                                 \
typedef enum { prefix##_OK, prefix##_ERR } prefix##_Tag;                       \
typedef struct { prefix##_Tag tag; union { OkType ok; ErrType err; } as; } prefix; \
static inline prefix prefix##_ok(OkType v)                                     \
{                                                                              \
    prefix r;                                                                  \
    r.tag = prefix##_OK;                                                       \
    r.as.ok = v;                                                               \
    return r;                                                                  \
}                                                                              \
static inline prefix prefix##_err(ErrType e)                                   \
{                                                                              \
    prefix r;                                                                  \
    r.tag = prefix##_ERR;                                                      \
    r.as.err = e;                                                              \
    return r;                                                                  \
}

/* ---- TRY: early return on failure, for Option and Result ---- */
#define TRY(dst, expr)                          \
    do {                                        \
        (dst) = (expr);                         \
        if ((dst).tag != 0) return (dst);       \
    } while (0)

/* ---- Future(T): poll-based, PENDING / READY / FAILED ---- */
#define FUTURE_DEFINE(T, prefix)                                               \
typedef enum { prefix##_PENDING, prefix##_READY, prefix##_FAILED } prefix##_Tag; \
typedef struct { prefix##_Tag tag; T value; } prefix;                          \
static inline prefix prefix##_pending(void)                                    \
{                                                                              \
    prefix f;                                                                  \
    f.tag = prefix##_PENDING;                                                  \
    return f;                                                                  \
}                                                                              \
static inline prefix prefix##_ready(T v)                                       \
{                                                                              \
    prefix f;                                                                  \
    f.tag = prefix##_READY;                                                    \
    f.value = v;                                                               \
    return f;                                                                  \
}                                                                              \
static inline prefix prefix##_failed(void)                                     \
{                                                                              \
    prefix f;                                                                  \
    f.tag = prefix##_FAILED;                                                   \
    return f;                                                                  \
}                                                                              \
static inline i32 prefix##_is_pending(const prefix *f)                         \
{                                                                              \
    return f->tag == prefix##_PENDING;                                         \
}

/* ---- DEFER(fn): run a `void (*)(void)` when the enclosing block exits ----
 * Any exit path counts: fall-through, return, break. One DEFER per source line. */
#define TYPES_CONCAT_(a, b) a##b
#define TYPES_CONCAT(a, b) TYPES_CONCAT_(a, b)

static inline void types_defer_call_(void (**fn)(void))
{
    if (*fn) (*fn)();
}

#define DEFER(fn) \
    void (*TYPES_CONCAT(types_defer_fn_, __LINE__))(void) __attribute__((cleanup(types_defer_call_))) = (fn)

/* ---- Async_Signal: a blocking "await" ----
 * notify() stores a payload and wakes every waiter; await() blocks until
 * notify() has been called at least once and returns the payload. */
typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cond;
    i32 signaled;
    void *payload;
} Async_Signal;

void async_signal_init(Async_Signal *s);
void async_signal_destroy(Async_Signal *s);
void async_signal_notify(Async_Signal *s, void *payload);   /* safe to call again, overwrites */
void *async_signal_await(Async_Signal *s);                  /* blocks, returns immediately if signaled */
i32 async_signal_try_await(Async_Signal *s, void **out);    /* 1 and *out set if signaled, else 0 */

#endif /* HOLYC_TYPES_H_ */

#if defined(TYPES_IMPLEMENTATION) && !defined(TYPES_IMPLEMENTATION_DONE_)
#define TYPES_IMPLEMENTATION_DONE_

void async_signal_init(Async_Signal *s)
{
    s->signaled = 0;
    s->payload = NULL;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cond, NULL);
}

void async_signal_destroy(Async_Signal *s)
{
    pthread_mutex_destroy(&s->mu);
    pthread_cond_destroy(&s->cond);
}

void async_signal_notify(Async_Signal *s, void *payload)
{
    pthread_mutex_lock(&s->mu);
    s->payload = payload;
    s->signaled = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->mu);
}

void *async_signal_await(Async_Signal *s)
{
    void *result;
    pthread_mutex_lock(&s->mu);
    while (!s->signaled)
        pthread_cond_wait(&s->cond, &s->mu);
    result = s->payload;
    pthread_mutex_unlock(&s->mu);
    return result;
}

i32 async_signal_try_await(Async_Signal *s, void **out)
{
    i32 was_signaled;
    pthread_mutex_lock(&s->mu);
    was_signaled = s->signaled;
    if (was_signaled) *out = s->payload;
    pthread_mutex_unlock(&s->mu);
    return was_signaled;
}

#endif /* TYPES_IMPLEMENTATION */
