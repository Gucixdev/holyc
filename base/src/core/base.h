#ifndef HOLYC_BASE_H_
#define HOLYC_BASE_H_

/* THE base header: constants, base types, Option/Result/TRY/Future/DEFER/
 * Async_Signal, str + String_View, Arena_String, print - one include,
 * "core/base.h". Each part keeps its own *_IMPLEMENTATION macro
 * (BASE_TYPE_, TYPES_, STR_, STRING_VIEW_, ARENA_STRING_, PRINT_).
 *
 * Older notes: base types (formerly base_type.h) + bit tricks
 * (formerly bit.h) + SIMD (formerly avx_math.h/sse_math.h), with their
 * REAL bodies pasted in directly below - not pulled in via #include.
 * The original split files are not deleted, just retired to
 * src/unused/ (kept around in case a future need for the smaller,
 * individually-swappable pieces comes back) - this file is now the one
 * every consumer actually includes for base types/bit/SIMD.
 *
 * vec2/vec3/vec4/Quat/Mat4 (`src/math/mathx.h`), print (`src/core/
 * print.h`), and log (`src/core/log.h`) used to live here too - all
 * three moved out to their own files, own scope: base.h stays scoped to base types/bit/SIMD only, a
 * consumer needing math types/print/log includes those files
 * separately (they each #include "core/base.h" themselves for the types
 * they need, so a consumer never has to include base.h AND one of
 * those directly for the shared types to resolve correctly).
 *
 * Implementation guard macros keep their ORIGINAL per-component names
 * (BASE_TYPE_IMPLEMENTATION, BIT_IMPLEMENTATION, AVX_MATH_IMPLEMENTATION,
 * SSE_MATH_IMPLEMENTATION) - a consumer that only ever needed base types
 * still only pays for base types, not SIMD being compiled in too.
 *
 * No libc includes for base types, str, print and constants (the only
 * system include is <pthread.h> for Async_Signal in the types section).
 * size_t comes from the compiler builtin __SIZE_TYPE__ (same trick
 * base_type.h already used for i8/u8/etc via __INTn_TYPE__); NULL/
 * offsetof are the standard's own textbook definitions, not glibc
 * calls. `immintrin.h` further below is a COMPILER-vendored intrinsics
 * header (ships with gcc/clang itself), not glibc, so it doesn't count
 * against this and is kept as-is. */

/* ============================================================
 * base types (base_type.h)
 * o void   u unsigned   i int   f float   fp fixed-point   a array
 * sizes: 8 16 32 64
 * ============================================================ */

/* TODO: pelna implementacja xmacro tu w tym pliku koniecznie i operacje na
 * stringach tez tu (str_stream, arena_str, etc) - podstawowe operacje
 * stringowe. */

#if !defined(__INT8_TYPE__) || !defined(__INT64_TYPE__) || !defined(__SIZE_TYPE__)
#error "compiler does not expose __INTn_TYPE__/__SIZE_TYPE__ builtins, base.h needs a different backend on this toolchain"
#endif

typedef __SIZE_TYPE__ size_t;

/* NULL/offsetof are stddef.h's other two "not really libc, just
 * compiler-level tricks" facilities - same reasoning as size_t above,
 * defined here rather than pulled in via #include. offsetof's
 * ((type*)0)->member trick is the standard's own textbook
 * implementation, not a shortcut version of it. */
#ifndef NULL
#define NULL ((void *)0)
#endif
#define offsetof(type, member) ((size_t)&((type *)0)->member)

/* alignof(type) (C11 <stdalign.h>, no libc dependency needed): the
 * classic struct-offset trick built directly on offsetof above - a
 * struct with a single leading `char` gets padded before its second
 * member exactly up to that member's own natural alignment, so that
 * padding amount (== the second member's offset) IS alignof(type).
 * This exact technique already lived privately as arena_alloc.h's own
 * ARENA_ALIGNOF; this is the same trick promoted to a public, base.h-
 * level facility so every consumer gets it without needing arena_alloc.h. */
#define HOLYC_ALIGNOF(type) offsetof(struct { char c_; type d_; }, d_)

/* alignas(n) (C11 <stdalign.h>): no portable C89 way to request a
 * specific alignment - GCC/Clang's own `__attribute__((aligned))`
 * extension is the real, standard-in-practice way every C89 codebase
 * targeting these compilers actually does this (same "GCC extension
 * where the standard has nothing" stance mem/raii.h's cleanup
 * attribute and this project's own SIMD intrinsics below already
 * take). Place BEFORE the declaration, same position C11's real
 * `alignas` keyword uses: `HOLYC_ALIGNAS(16) u8 buf[64];` */
#define HOLYC_ALIGNAS(n) __attribute__((aligned(n)))

/* static_assert(cond, msg) (C11 <assert.h> / C23 keyword): the
 * classic C89 emulation - a typedef to an array of size [1] on
 * success or [-1] (a compile error, negative array size) on failure.
 * Needs a unique type name per use site (__LINE__-based, same
 * two-level token-paste trick help/async.h's own DEFER macro uses)
 * since two static_asserts on different lines would otherwise redeclare
 * the same typedef name. `msg` is NOT a string here (C89 has no way to
 * surface an arbitrary message text into a compile error without a
 * real _Static_assert) - it must be a bare identifier fragment
 * (e.g. `size_must_be_4`, not `"size must be 4"`), token-pasted
 * directly into the generated type's own name so it's still visible
 * in the compiler's error output (the "negative array size" message
 * will mention `holyc_static_assert_..._size_must_be_4_42`, line
 * number included) - a real, if cruder, substitute for a proper
 * message string. */
#define HOLYC_STATIC_ASSERT_CONCAT_(a, b) a##b
#define HOLYC_STATIC_ASSERT_CONCAT(a, b) HOLYC_STATIC_ASSERT_CONCAT_(a, b)
#define HOLYC_STATIC_ASSERT(cond, msg) \
    typedef char HOLYC_STATIC_ASSERT_CONCAT(HOLYC_STATIC_ASSERT_CONCAT(holyc_static_assert_##msg##_, __LINE__), _)[(cond) ? 1 : -1]

typedef void o;

typedef __INT8_TYPE__  i8;
typedef __INT16_TYPE__ i16;
typedef __INT32_TYPE__ i32;
typedef __INT64_TYPE__ i64;

typedef __UINT8_TYPE__  u8;
typedef __UINT16_TYPE__ u16;
typedef __UINT32_TYPE__ u32;
typedef __UINT64_TYPE__ u64;

typedef float  f32;
typedef double f64;

/* fp: Q-format fixed point. add/sub are just + and - on the underlying int,
 * no wrapper needed for those - that is the whole point of fixed point. */
typedef i16 fp16; /* Q8.8   */
typedef i32 fp32; /* Q16.16 */
typedef i64 fp64; /* Q32.32 */

#define FP16_FRAC_BITS 8
#define FP32_FRAC_BITS 16
#define FP64_FRAC_BITS 32

fp16 fp16_from_f32(f32 v);
f32  fp16_to_f32(fp16 v);
fp16 fp16_mul(fp16 a, fp16 b);
fp16 fp16_div(fp16 a, fp16 b);

fp32 fp32_from_f32(f32 v);
f32  fp32_to_f32(fp32 v);
fp32 fp32_mul(fp32 a, fp32 b);
fp32 fp32_div(fp32 a, fp32 b);

fp64 fp64_from_f64(f64 v);
f64  fp64_to_f64(fp64 v);
/* fp64_mul/fp64_div need a 64x64->128 bit widen that plain C89 does not have
 * (no __int128 without a compiler extension) - not implemented yet. */

/* a: bounds-carrying, non-owning view over existing memory (pointer + count). */
typedef struct { u8  *data; size_t count; } a8;
typedef struct { u16 *data; size_t count; } a16;
typedef struct { u32 *data; size_t count; } a32;
typedef struct { u64 *data; size_t count; } a64;

a8  a8_make(u8 *data, size_t count);
a16 a16_make(u16 *data, size_t count);
a32 a32_make(u32 *data, size_t count);
a64 a64_make(u64 *data, size_t count);

/* FNV-1a over a byte buffer of known length. */
u32 fnv1a_hash(const void *data, size_t len, u32 seed);

/* memset_explicit (C23 <string.h>): unlike a plain memset, a compiler
 * is explicitly FORBIDDEN from optimizing this write away even if it
 * can prove the memory is never read afterward - the real problem
 * plain memset has for zeroing secrets (a password, a key) right
 * before freeing/returning: dead-store elimination sees the write as
 * pointless and deletes it, leaving the secret sitting in memory. The
 * `volatile` pointer here is what defeats that optimization - the
 * standard, portable-in-practice technique (the same one glibc's own
 * explicit_bzero and OpenBSD's explicit_bzero use under the hood). */
void *memset_explicit(void *dst, i32 val, size_t n);

/* ============================================================
 * bit (bit.h)
 * ============================================================ */

#define BIT(n)              ((u64)1 << (n))
#define FLAG_SET(x, m)      ((x) |= (m))
#define FLAG_CLEAR(x, m)    ((x) &= ~(m))
#define FLAG_TOGGLE(x, m)   ((x) ^= (m))
#define FLAG_HAS(x, m)      (((x) & (m)) == (m))

u32 bit_extract32(u32 x, i32 pos, i32 width);
u32 bit_insert32(u32 x, u32 value, i32 pos, i32 width); /* replace */
u64 bit_extract64(u64 x, i32 pos, i32 width);
u64 bit_insert64(u64 x, u64 value, i32 pos, i32 width);

/* software PDEP/PEXT (BMI2's actual instructions, portable). */
u32 bit_pdep32(u32 src, u32 mask);
u32 bit_pext32(u32 src, u32 mask);

/* bitboard: lowest-set-bit tricks */
u64 bit_isolate_lsb64(u64 x);  /* the lowest set bit alone, 0 if x == 0 */
u64 bit_clear_lsb64(u64 x);    /* x with its lowest set bit cleared */
i32 bit_lsb_index64(u64 x);    /* index of the lowest set bit, -1 if x == 0 */

u8  bit_reverse8(u8 x);
u32 bit_reverse32(u32 x);

/* align must be a power of 2 - not checked. */
size_t align_up(size_t x, size_t align);
size_t align_down(size_t x, size_t align);
i32 is_pow2(size_t x); /* 0 counts as NOT a power of 2 */

/* branchless select/min/max/abs/clamp on plain integers */
i32 bit_select_i32(i32 cond, i32 a, i32 b); /* cond must be 0 or 1: a if 1, b if 0 */
i32 min_i32(i32 a, i32 b);
i32 max_i32(i32 a, i32 b);
i32 clamp_i32(i32 x, i32 lo, i32 hi);
i32 abs_i32(i32 x);

u16 byteswap16(u16 x);
u32 byteswap32(u32 x);
u64 byteswap64(u64 x);
i32 is_little_endian(void); /* runtime check - portable */

/* UB on zero input for clz/ctz, matching the underlying compiler builtins. */
i32 popcount32(u32 x);
i32 popcount64(u64 x);
i32 clz32(u32 x); /* undefined for x == 0 */
i32 ctz32(u32 x); /* undefined for x == 0 */

u32 next_pow2_32(u32 x); /* x itself if already a power of 2; 1 for x == 0 */

i32 sign_extend32(u32 x, i32 bits); /* bits in 1..32 */

/* Trap-based assert, replacing assert.h's - __builtin_trap() is a
 * compiler intrinsic (illegal-instruction trap), not glibc, so this
 * stays inside the "0 glibc" rule. No printed file/line/expression
 * message the way glibc's assert() gives you: this project's own
 * print.h needs PRINT_IMPLEMENTATION opted into separately, which
 * would make this macro's behavior depend on whether the includer
 * happened to also want print - simpler and more predictable to just
 * trap (a debugger or core dump still tells you exactly where).
 * Compiles out under NDEBUG, matching assert()'s own convention. The
 * various *_ASSERT macros across the codebase (ARENA_ASSERT,
 * POOL_ASSERT, REF_COUNT_ASSERT, etc.) default to this instead of
 * glibc's assert now. */
#ifdef NDEBUG
#define HOLYC_ASSERT(cond) ((void)0)
#else
#define HOLYC_ASSERT(cond) ((cond) ? (void)0 : __builtin_trap())
#endif

/* ============================================================
 * SIMD (avx_math.h / sse_math.h) - x86/x86_64 only, no-op elsewhere.
 * Call *_math_available() before using the rest - none of these fall
 * back to scalar internally, that's the caller's job.
 * ============================================================ */

#if defined(__x86_64__) || defined(__i386__)

i32 avx_math_available(void);
f32 avx_sum8(const f32 *data);
f32 avx_dot8(const f32 *a, const f32 *b);
f32 avx_min8(const f32 *data);
f32 avx_max8(const f32 *data);

i32 sse_math_available(void);
f32 sse_sum4(const f32 *data);
f32 sse_dot4(const f32 *a, const f32 *b);
f32 sse_min4(const f32 *data);
f32 sse_max4(const f32 *data);

#endif /* x86 */


/* ################ constants ################ */


/* Common mathematical constants as f64 #defines, nothing else - no
 * function bodies, so no IMPLEMENTATION guard needed (matches
 * base_type.h's own `#define FP16_FRAC_BITS` style for a values-only
 * header). This is the raw numeric CONSTANTS other files pull from
 * instead of each hand-writing "3.14159265358979323846" in isolation -
 * NOT the deg/rad/turn conversion FUNCTIONS (that's trigonometry.h's
 * job, unchanged here). HOLYC_PI is defined guarded the same way
 * trigonometry.h already guards its own copy (`#ifndef HOLYC_PI`) so
 * whichever of the two files gets included first "wins" without a
 * conflict or duplicate value, no matter the include order. */

#ifndef HOLYC_PI
#define HOLYC_PI 3.14159265358979323846
#endif

#define HOLYC_TAU (2.0 * HOLYC_PI) /* one full turn, in radians */
#define HOLYC_E 2.71828182845904523536
#define HOLYC_SQRT2 1.41421356237309504880
#define HOLYC_GOLDEN_RATIO 1.61803398874989484820 /* phi = (1+sqrt(5))/2 */

#define HOLYC_DEG_TO_RAD (HOLYC_PI / 180.0)
#define HOLYC_RAD_TO_DEG (180.0 / HOLYC_PI)


/* inline works in every -std mode on GCC/Clang (__inline__), is plain `inline` in C99+, and vanishes in strict C89 */
#ifndef HC_INLINE
#if defined(__GNUC__)
#define HC_INLINE __inline__
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#define HC_INLINE inline
#else
#define HC_INLINE
#endif
#endif

/* ################ types: Option, Result, TRY, Future, DEFER, Async_Signal ################ */
#include <pthread.h>

#if defined(ASYNC_IMPLEMENTATION) && !defined(TYPES_IMPLEMENTATION)
#define TYPES_IMPLEMENTATION   /* the name this header used to have */
#endif

/* ---- Option(T) ---- */
#define OPTION_DEFINE(T, prefix)                                              \
typedef enum { prefix##_SOME, prefix##_NONE } prefix##_Tag;                    \
typedef struct { prefix##_Tag tag; T value; } prefix;                          \
static HC_INLINE prefix prefix##_some(T v)                                        \
{                                                                              \
    prefix o;                                                                  \
    o.tag = prefix##_SOME;                                                     \
    o.value = v;                                                               \
    return o;                                                                  \
}                                                                              \
static HC_INLINE prefix prefix##_none(void)                                       \
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
static HC_INLINE prefix prefix##_ok(OkType v)                                     \
{                                                                              \
    prefix r;                                                                  \
    r.tag = prefix##_OK;                                                       \
    r.as.ok = v;                                                               \
    return r;                                                                  \
}                                                                              \
static HC_INLINE prefix prefix##_err(ErrType e)                                   \
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
static HC_INLINE prefix prefix##_pending(void)                                    \
{                                                                              \
    prefix f;                                                                  \
    f.tag = prefix##_PENDING;                                                  \
    return f;                                                                  \
}                                                                              \
static HC_INLINE prefix prefix##_ready(T v)                                       \
{                                                                              \
    prefix f;                                                                  \
    f.tag = prefix##_READY;                                                    \
    f.value = v;                                                               \
    return f;                                                                  \
}                                                                              \
static HC_INLINE prefix prefix##_failed(void)                                     \
{                                                                              \
    prefix f;                                                                  \
    f.tag = prefix##_FAILED;                                                   \
    return f;                                                                  \
}                                                                              \
static HC_INLINE i32 prefix##_is_pending(const prefix *f)                         \
{                                                                              \
    return f->tag == prefix##_PENDING;                                         \
}

/* ---- DEFER(fn): run a `void (*)(void)` when the enclosing block exits ----
 * Any exit path counts: fall-through, return, break. One DEFER per source line. */
#define TYPES_CONCAT_(a, b) a##b
#define TYPES_CONCAT(a, b) TYPES_CONCAT_(a, b)

static HC_INLINE void types_defer_call_(void (**fn)(void))
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


/* ################ str, String_View ################ */
/* ============================================================
 * str (own C-string/memory primitives) - a real, if intentionally
 * minimal, hand-rolled <string.h> replacement: only the functions this
 * codebase actually calls somewhere (see the grep this was scoped
 * from), not an attempt at every libc string function that exists.
 * Named str_ and mem_ (not strlen/memcmp/etc's real names) - a
 * project file literally defining global `strlen`/`memcmp` would
 * collide with libc's own real symbols of the same name the moment
 * anything in the same binary still links against libc for something
 * else (which this project's binaries currently do - see base.h's own
 * top comment on the "0 glibc" claim being source-level, not a
 * -nostdlib freestanding binary), the same reasoning base.h's own
 * print_u64/log_log etc. already follow instead of shadowing printf/
 * syslog. Migrating the REST of the codebase's existing direct
 * <string.h> call sites onto these is real, separate, much bigger
 * scope (95 strlen / 71 strcmp / 55 memset / 48 memcpy / 39 memcmp call
 * sites project-wide at last count) - not attempted in this pass,
 * tracked the same incremental way base.h's own system-header sweep
 * documents its own remaining gap.
 * ============================================================ */

size_t str_len(const char *s);
i32 str_cmp(const char *a, const char *b);
i32 str_ncmp(const char *a, const char *b, size_t n);
char *str_cpy(char *dst, const char *src);
char *str_ncpy(char *dst, const char *src, size_t n);
char *str_chr(const char *s, i32 c);
char *str_str(const char *haystack, const char *needle);

void *mem_cpy(void *dst, const void *src, size_t n);
void *mem_move(void *dst, const void *src, size_t n);
i32 mem_cmp(const void *a, const void *b, size_t n);
void *mem_set(void *dst, i32 val, size_t n);

/* ============================================================
 * String_View (string_view.h) - ported from tsoding's sv.h (MIT), the
 * string-specific sibling to base_type.h's a8/a16/a32/a64 byte/word
 * array views (those are untyped-length views over numeric arrays; this
 * is specifically for parsing/scanning text without copying or
 * NUL-terminating every intermediate slice). bool -> i32, uint64_t ->
 * u64, C99 mid-block declarations moved to the top of their blocks -
 * otherwise the same algorithms, they were already correct and this
 * project has no reason to redesign a working chop/trim/compare API.
 * ============================================================ */

typedef struct {
    size_t count;
    const char *data;
} String_View;

#define SV(cstr_lit) sv_from_parts((cstr_lit), sizeof(cstr_lit) - 1)
#define SV_NULL sv_from_parts((const char *)0, 0)

/* printf-family helper: printf("name: " SV_FMT "\n", SV_ARG(name)); - this
 * project's own print.h has no %-style formatting to hook into, kept
 * anyway since project/-level .c files (which DO use stdio's printf, see
 * project/vecindex/vecindex_cli.c) can still use it. */
#define SV_FMT "%.*s"
#define SV_ARG(sv) (i32)(sv).count, (sv).data

String_View sv_from_parts(const char *data, size_t count);
String_View sv_from_cstr(const char *cstr);
String_View sv_trim_left(String_View sv);
String_View sv_trim_right(String_View sv);
String_View sv_trim(String_View sv);
String_View sv_take_left_while(String_View sv, i32 (*predicate)(char x));
String_View sv_chop_left_while(String_View *sv, i32 (*predicate)(char x));
String_View sv_chop_by_delim(String_View *sv, char delim);
String_View sv_chop_by_sv(String_View *sv, String_View delim);
i32 sv_try_chop_by_delim(String_View *sv, char delim, String_View *chunk);
String_View sv_chop_left(String_View *sv, size_t n);
String_View sv_chop_right(String_View *sv, size_t n);
i32 sv_index_of(String_View sv, char c, size_t *index);
i32 sv_eq(String_View a, String_View b);
i32 sv_eq_ignorecase(String_View a, String_View b);
i32 sv_starts_with(String_View sv, String_View prefix);
i32 sv_ends_with(String_View sv, String_View suffix);
u64 sv_to_u64(String_View sv);
u64 sv_chop_u64(String_View *sv);


/* ################ Arena_String: growable text on an Arena (arena_alloc.h) ################ */
typedef struct Arena Arena; /* the struct is in arena_alloc.h */

/* ============================================================
 * Arena_String (arena_string.h) - the gap between print.h (write(2)-
 * direct, no buffer-building API at all) and something that needs to
 * actually ACCUMULATE mutable text before handing it somewhere else
 * (im_gui.h's text-input widgets, react_gui.h's rendered labels, any
 * caller building up a line before it's ready to submit). Not a
 * replacement for print.h - print.h is still the right tool for "emit
 * this now and forget it"; this is for "build this up, read it back,
 * maybe clear and reuse it". Uses arena_alloc.h's arena_da_append
 * growable-array convention directly (same doubling-capacity growth
 * every other dynamic array in this codebase uses) rather than
 * inventing a separate growth policy.
 * ============================================================ */

typedef struct {
    Arena *a;
    char *items;
    size_t count;
    size_t capacity;
} Arena_String;

void arena_string_init(Arena_String *s, Arena *a);
void arena_string_clear(Arena_String *s);
void arena_string_append_char(Arena_String *s, char c);
void arena_string_append_cstr(Arena_String *s, const char *cstr);
void arena_string_append_buf(Arena_String *s, const char *buf, size_t n);
void arena_string_append_u64(Arena_String *s, u64 v);
void arena_string_append_i64(Arena_String *s, i64 v);

/* NUL-terminates (appending a trailing NUL that does NOT count toward
 * ->count, matching arena_alloc.h's own arena_sb_append_null convention)
 * and returns the string's start - safe to call more than once, safe to
 * keep appending afterward (the next append just grows over the NUL). */
const char *arena_string_cstr(Arena_String *s);


/* ################ print ################ */


/* ============================================================
 * print (print.h) - "own print without printf, just write and itoa": no
 * stdio, no format-string parsing, a small line buffer flushed
 * through write(2).
 * ============================================================ */

#ifndef PRINT_BUF_SIZE
#define PRINT_BUF_SIZE 4096
#endif

void print_flush(void);
void print_buf(const char *buf, size_t n);
void print_char(char c);
void print_cstr(const char *s);
void print_u64_base(u64 v, u32 base);
void print_i64_base(i64 v, u32 base);
void print_u64(u64 v);
void print_i64(i64 v);
#endif /* HOLYC_BASE_H_ */


/* Implementation guards below are kept as the SAME per-component macro
 * names the original split files used (BASE_TYPE_IMPLEMENTATION,
 * BIT_IMPLEMENTATION, AVX_MATH_IMPLEMENTATION, SSE_MATH_IMPLEMENTATION)
 * rather than one combined macro - a consumer that only needs base
 * types still only pays for base types, not SIMD being compiled in too. */

#ifdef BASE_TYPE_IMPLEMENTATION
#ifndef BASE_TYPE_IMPLEMENTATION_
#define BASE_TYPE_IMPLEMENTATION_

fp16 fp16_from_f32(f32 v) { return (fp16)(v * (f32)(1 << FP16_FRAC_BITS)); }
f32  fp16_to_f32(fp16 v)  { return (f32)v / (f32)(1 << FP16_FRAC_BITS); }
fp16 fp16_mul(fp16 a, fp16 b) { return (fp16)(((i32)a * (i32)b) >> FP16_FRAC_BITS); }
fp16 fp16_div(fp16 a, fp16 b) { return (fp16)(((i32)a << FP16_FRAC_BITS) / b); }

fp32 fp32_from_f32(f32 v) { return (fp32)(v * (f32)(1 << FP32_FRAC_BITS)); }
f32  fp32_to_f32(fp32 v)  { return (f32)v / (f32)(1 << FP32_FRAC_BITS); }
fp32 fp32_mul(fp32 a, fp32 b) { return (fp32)(((i64)a * (i64)b) >> FP32_FRAC_BITS); }
fp32 fp32_div(fp32 a, fp32 b) { return (fp32)(((i64)a << FP32_FRAC_BITS) / b); }

fp64 fp64_from_f64(f64 v) { return (fp64)(v * (f64)((i64)1 << FP64_FRAC_BITS)); }
f64  fp64_to_f64(fp64 v)  { return (f64)v / (f64)((i64)1 << FP64_FRAC_BITS); }

u32 fnv1a_hash(const void *data, size_t len, u32 seed)
{
    const u8 *p = (const u8 *)data;
    u32 h = 2166136261u ^ seed;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

void *memset_explicit(void *dst, i32 val, size_t n)
{
    volatile u8 *p = (volatile u8 *)dst;
    size_t i;
    for (i = 0; i < n; i++) p[i] = (u8)val;
    return dst;
}

a8  a8_make(u8 *data, size_t count)  { a8  s; s.data = data; s.count = count; return s; }
a16 a16_make(u16 *data, size_t count) { a16 s; s.data = data; s.count = count; return s; }
a32 a32_make(u32 *data, size_t count) { a32 s; s.data = data; s.count = count; return s; }
a64 a64_make(u64 *data, size_t count) { a64 s; s.data = data; s.count = count; return s; }

#endif /* BASE_TYPE_IMPLEMENTATION_ */
#endif /* BASE_TYPE_IMPLEMENTATION */

#ifdef BIT_IMPLEMENTATION
#ifndef BIT_IMPLEMENTATION_
#define BIT_IMPLEMENTATION_

u32 bit_extract32(u32 x, i32 pos, i32 width)
{
    u32 mask = (width >= 32) ? (u32)0xffffffffu : (((u32)1 << width) - 1);
    return (x >> pos) & mask;
}

u32 bit_insert32(u32 x, u32 value, i32 pos, i32 width)
{
    u32 mask = (width >= 32) ? (u32)0xffffffffu : (((u32)1 << width) - 1);
    return (x & ~(mask << pos)) | ((value & mask) << pos);
}

u64 bit_extract64(u64 x, i32 pos, i32 width)
{
    u64 mask = (width >= 64) ? ~(u64)0 : (((u64)1 << width) - 1);
    return (x >> pos) & mask;
}

u64 bit_insert64(u64 x, u64 value, i32 pos, i32 width)
{
    u64 mask = (width >= 64) ? ~(u64)0 : (((u64)1 << width) - 1);
    return (x & ~(mask << pos)) | ((value & mask) << pos);
}

u32 bit_pdep32(u32 src, u32 mask)
{
    u32 result = 0;
    u32 bit = 1;
    while (mask != 0) {
        u32 lsb = mask & (~mask + 1); /* isolate mask's lowest set bit */
        if (src & bit) result |= lsb;
        mask &= mask - 1; /* clear that bit from mask */
        bit <<= 1;
    }
    return result;
}

u32 bit_pext32(u32 src, u32 mask)
{
    u32 result = 0;
    u32 bit = 1;
    while (mask != 0) {
        u32 lsb = mask & (~mask + 1);
        if (src & lsb) result |= bit;
        mask &= mask - 1;
        bit <<= 1;
    }
    return result;
}

u64 bit_isolate_lsb64(u64 x)
{
    return x & (~x + 1);
}

u64 bit_clear_lsb64(u64 x)
{
    return x & (x - 1);
}

i32 bit_lsb_index64(u64 x)
{
    i32 not_zero = (x != 0);
    u64 safe_x = x | (u64)(!not_zero); /* avoid UB from ctzll(0) below */
    return (i32)(__builtin_ctzl((unsigned long)safe_x)) * not_zero + (-1) * (1 - not_zero);
}

u8 bit_reverse8(u8 x)
{
    x = (u8)(((x & 0xf0u) >> 4) | ((x & 0x0fu) << 4));
    x = (u8)(((x & 0xccu) >> 2) | ((x & 0x33u) << 2));
    x = (u8)(((x & 0xaau) >> 1) | ((x & 0x55u) << 1));
    return x;
}

u32 bit_reverse32(u32 x)
{
    x = ((x & 0xffff0000u) >> 16) | ((x & 0x0000ffffu) << 16);
    x = ((x & 0xff00ff00u) >> 8)  | ((x & 0x00ff00ffu) << 8);
    x = ((x & 0xf0f0f0f0u) >> 4)  | ((x & 0x0f0f0f0fu) << 4);
    x = ((x & 0xccccccccu) >> 2)  | ((x & 0x33333333u) << 2);
    x = ((x & 0xaaaaaaaau) >> 1)  | ((x & 0x55555555u) << 1);
    return x;
}

size_t align_up(size_t x, size_t align)
{
    return (x + align - 1) & ~(align - 1);
}

size_t align_down(size_t x, size_t align)
{
    return x & ~(align - 1);
}

i32 is_pow2(size_t x)
{
    return (x != 0) && ((x & (x - 1)) == 0);
}

i32 bit_select_i32(i32 cond, i32 a, i32 b)
{
    i32 mask = -cond; /* cond==1 -> all-1s, cond==0 -> 0 */
    return (a & mask) | (b & ~mask);
}

i32 min_i32(i32 a, i32 b)
{
    return b ^ ((a ^ b) & -(a < b));
}

i32 max_i32(i32 a, i32 b)
{
    return a ^ ((a ^ b) & -(a < b));
}

i32 clamp_i32(i32 x, i32 lo, i32 hi)
{
    return min_i32(max_i32(x, lo), hi);
}

i32 abs_i32(i32 x)
{
    i32 mask = x >> 31; /* all-1s if negative, 0 if non-negative (arithmetic shift) */
    return (x ^ mask) - mask;
}

u16 byteswap16(u16 x)
{
    return (u16)((x >> 8) | (x << 8));
}

u32 byteswap32(u32 x)
{
    return ((x >> 24) & 0x000000ffu) | ((x >> 8) & 0x0000ff00u) |
           ((x << 8) & 0x00ff0000u) | ((x << 24) & 0xff000000u);
}

u64 byteswap64(u64 x)
{
    u64 lo = byteswap32((u32)(x & 0xffffffffu));
    u64 hi = byteswap32((u32)(x >> 32));
    return (lo << 32) | hi;
}

i32 is_little_endian(void)
{
    const u32 one = 1;
    return *(const u8 *)&one == 1;
}

i32 popcount32(u32 x)
{
    return __builtin_popcount(x);
}

i32 popcount64(u64 x)
{
    return __builtin_popcountl((unsigned long)x);
}

i32 clz32(u32 x)
{
    return __builtin_clz(x);
}

i32 ctz32(u32 x)
{
    return __builtin_ctz(x);
}

u32 next_pow2_32(u32 x)
{
    i32 is_zero = (x == 0);
    u32 v = x - 1;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return (v + 1) | (u32)(-is_zero & 1); /* x==0 -> 1, same as calling this on 1 */
}

i32 sign_extend32(u32 x, i32 bits)
{
    u32 m = (u32)1 << (bits - 1);
    return (i32)((x ^ m) - m);
}

#endif /* BIT_IMPLEMENTATION_ */
#endif /* BIT_IMPLEMENTATION */


#ifdef AVX_MATH_IMPLEMENTATION
#ifndef AVX_MATH_IMPLEMENTATION_
#define AVX_MATH_IMPLEMENTATION_

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

i32 avx_math_available(void)
{
    static i32 cached = -1;
    if (cached < 0) cached = __builtin_cpu_supports("avx2") ? 1 : 0;
    return cached;
}

__attribute__((target("avx2")))
f32 avx_sum8(const f32 *data)
{
    __m256 v = _mm256_loadu_ps(data);
    f32 buf[8];
    _mm256_storeu_ps(buf, v);
    return buf[0] + buf[1] + buf[2] + buf[3] + buf[4] + buf[5] + buf[6] + buf[7];
}

__attribute__((target("avx2")))
f32 avx_dot8(const f32 *a, const f32 *b)
{
    __m256 va = _mm256_loadu_ps(a);
    __m256 vb = _mm256_loadu_ps(b);
    __m256 prod = _mm256_mul_ps(va, vb);
    f32 buf[8];
    _mm256_storeu_ps(buf, prod);
    return buf[0] + buf[1] + buf[2] + buf[3] + buf[4] + buf[5] + buf[6] + buf[7];
}

__attribute__((target("avx2")))
f32 avx_min8(const f32 *data)
{
    __m256 v = _mm256_loadu_ps(data);
    f32 buf[8];
    f32 m;
    i32 i;
    _mm256_storeu_ps(buf, v);
    m = buf[0];
    for (i = 1; i < 8; i++) if (buf[i] < m) m = buf[i];
    return m;
}

__attribute__((target("avx2")))
f32 avx_max8(const f32 *data)
{
    __m256 v = _mm256_loadu_ps(data);
    f32 buf[8];
    f32 m;
    i32 i;
    _mm256_storeu_ps(buf, v);
    m = buf[0];
    for (i = 1; i < 8; i++) if (buf[i] > m) m = buf[i];
    return m;
}

#endif /* x86 */

#endif /* AVX_MATH_IMPLEMENTATION_ */
#endif /* AVX_MATH_IMPLEMENTATION */

#ifdef SSE_MATH_IMPLEMENTATION
#ifndef SSE_MATH_IMPLEMENTATION_
#define SSE_MATH_IMPLEMENTATION_

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>

i32 sse_math_available(void)
{
    static i32 cached = -1;
    if (cached < 0) cached = __builtin_cpu_supports("sse2") ? 1 : 0;
    return cached;
}

__attribute__((target("sse2")))
f32 sse_sum4(const f32 *data)
{
    __m128 v = _mm_loadu_ps(data);
    f32 buf[4];
    _mm_storeu_ps(buf, v);
    return buf[0] + buf[1] + buf[2] + buf[3];
}

__attribute__((target("sse2")))
f32 sse_dot4(const f32 *a, const f32 *b)
{
    __m128 va = _mm_loadu_ps(a);
    __m128 vb = _mm_loadu_ps(b);
    __m128 prod = _mm_mul_ps(va, vb);
    f32 buf[4];
    _mm_storeu_ps(buf, prod);
    return buf[0] + buf[1] + buf[2] + buf[3];
}

__attribute__((target("sse2")))
f32 sse_min4(const f32 *data)
{
    __m128 v = _mm_loadu_ps(data);
    f32 buf[4];
    f32 m;
    i32 i;
    _mm_storeu_ps(buf, v);
    m = buf[0];
    for (i = 1; i < 4; i++) if (buf[i] < m) m = buf[i];
    return m;
}

__attribute__((target("sse2")))
f32 sse_max4(const f32 *data)
{
    __m128 v = _mm_loadu_ps(data);
    f32 buf[4];
    f32 m;
    i32 i;
    _mm_storeu_ps(buf, v);
    m = buf[0];
    for (i = 1; i < 4; i++) if (buf[i] > m) m = buf[i];
    return m;
}

#endif /* x86 */

#endif /* SSE_MATH_IMPLEMENTATION_ */
#endif /* SSE_MATH_IMPLEMENTATION */

/* ################ implementation: types ################ */
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

/* ################ implementation: str, String_View ################ */
#ifdef STR_IMPLEMENTATION
#ifndef STR_IMPLEMENTATION_
#define STR_IMPLEMENTATION_

size_t str_len(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

i32 str_cmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (i32)(u8)*a - (i32)(u8)*b;
}

i32 str_ncmp(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0') return (i32)(u8)a[i] - (i32)(u8)b[i];
    }
    return 0;
}

char *str_cpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0') { /* copy including the NUL */ }
    return dst;
}

char *str_ncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0'; /* strncpy's own convention: zero-pad the remainder */
    return dst;
}

char *str_chr(const char *s, i32 c)
{
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (*s == '\0') return NULL;
    }
}

char *str_str(const char *haystack, const char *needle)
{
    size_t needle_len;
    const char *h;

    if (*needle == '\0') return (char *)haystack;

    needle_len = str_len(needle);
    for (h = haystack; *h; h++) {
        if (str_ncmp(h, needle, needle_len) == 0) return (char *)h;
    }
    return NULL;
}

void *mem_cpy(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *mem_move(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;

    if (d == s || n == 0) return dst;

    if (d < s) {
        for (i = 0; i < n; i++) d[i] = s[i];
    } else {
        /* dst overlaps past src's start - copy back-to-front so the
         * write side never clobbers bytes the read side hasn't
         * consumed yet (the classic overlapping-memmove hazard memcpy
         * is explicitly allowed to get wrong). */
        for (i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

i32 mem_cmp(const void *a, const void *b, size_t n)
{
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    size_t i;
    for (i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (i32)pa[i] - (i32)pb[i];
    }
    return 0;
}

void *mem_set(void *dst, i32 val, size_t n)
{
    u8 *d = (u8 *)dst;
    size_t i;
    for (i = 0; i < n; i++) d[i] = (u8)val;
    return dst;
}

#endif /* STR_IMPLEMENTATION_ */
#endif /* STR_IMPLEMENTATION */

#ifdef STRING_VIEW_IMPLEMENTATION
#ifndef STRING_VIEW_IMPLEMENTATION_
#define STRING_VIEW_IMPLEMENTATION_

/* own local copies of str_len/mem_cmp, under their own names (sv_strlen_/
 * sv_memcmp_, not str_len/mem_cmp) - deliberately NOT a cross-
 * IMPLEMENTATION-guard reference: this block must stand alone under
 * STRING_VIEW_IMPLEMENTATION even if a caller never defines
 * STR_IMPLEMENTATION at all, the same rule base.h's vector_sqrt_/
 * quat_sqrt_ already established. */
static size_t sv_strlen_(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static i32 sv_memcmp_(const void *a, const void *b, size_t n)
{
    const u8 *pa = (const u8 *)a;
    const u8 *pb = (const u8 *)b;
    size_t i;
    for (i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (i32)pa[i] - (i32)pb[i];
    }
    return 0;
}

#include <ctype.h>

String_View sv_from_parts(const char *data, size_t count)
{
    String_View sv;
    sv.count = count;
    sv.data = data;
    return sv;
}

String_View sv_from_cstr(const char *cstr)
{
    return sv_from_parts(cstr, sv_strlen_(cstr));
}

String_View sv_trim_left(String_View sv)
{
    size_t i = 0;
    while (i < sv.count && isspace((u8)sv.data[i])) i++;
    return sv_from_parts(sv.data + i, sv.count - i);
}

String_View sv_trim_right(String_View sv)
{
    size_t i = 0;
    while (i < sv.count && isspace((u8)sv.data[sv.count - 1 - i])) i++;
    return sv_from_parts(sv.data, sv.count - i);
}

String_View sv_trim(String_View sv)
{
    return sv_trim_right(sv_trim_left(sv));
}

String_View sv_chop_left(String_View *sv, size_t n)
{
    String_View result;
    /* branchless min(n, sv->count): b ^ ((a^b) & -(a<b)) - a<b as 0/1
     * turns into an all-0s or all-1s mask, no comparison the CPU has to
     * predict on every chop call (this runs inside every line-oriented
     * parser's hot loop - bdf.h, ini.h). */
    n = sv->count ^ ((n ^ sv->count) & (size_t)-(size_t)(n < sv->count));
    result = sv_from_parts(sv->data, n);
    sv->data += n;
    sv->count -= n;
    return result;
}

String_View sv_chop_right(String_View *sv, size_t n)
{
    String_View result;
    /* branchless min(n, sv->count): b ^ ((a^b) & -(a<b)) - a<b as 0/1
     * turns into an all-0s or all-1s mask, no comparison the CPU has to
     * predict on every chop call (this runs inside every line-oriented
     * parser's hot loop - bdf.h, ini.h). */
    n = sv->count ^ ((n ^ sv->count) & (size_t)-(size_t)(n < sv->count));
    result = sv_from_parts(sv->data + sv->count - n, n);
    sv->count -= n;
    return result;
}

i32 sv_index_of(String_View sv, char c, size_t *index)
{
    size_t i = 0;
    while (i < sv.count && sv.data[i] != c) i++;
    if (i < sv.count) {
        if (index) *index = i;
        return 1;
    }
    return 0;
}

i32 sv_try_chop_by_delim(String_View *sv, char delim, String_View *chunk)
{
    String_View result;
    size_t i = 0;
    while (i < sv->count && sv->data[i] != delim) i++;
    result = sv_from_parts(sv->data, i);
    if (i < sv->count) {
        sv->count -= i + 1;
        sv->data += i + 1;
        if (chunk) *chunk = result;
        return 1;
    }
    return 0;
}

String_View sv_chop_by_delim(String_View *sv, char delim)
{
    String_View result;
    size_t i = 0;
    while (i < sv->count && sv->data[i] != delim) i++;
    result = sv_from_parts(sv->data, i);
    if (i < sv->count) {
        sv->count -= i + 1;
        sv->data += i + 1;
    } else {
        sv->count -= i;
        sv->data += i;
    }
    return result;
}

String_View sv_chop_by_sv(String_View *sv, String_View delim)
{
    String_View window, result;
    size_t i = 0;

    window = sv_from_parts(sv->data, delim.count);
    while (i + delim.count < sv->count && !sv_eq(window, delim)) {
        i++;
        window.data++;
    }

    result = sv_from_parts(sv->data, i);
    if (i + delim.count == sv->count) {
        result.count += delim.count;
    }

    sv->data += i + delim.count;
    sv->count -= i + delim.count;
    return result;
}

i32 sv_starts_with(String_View sv, String_View prefix)
{
    String_View actual;
    if (prefix.count > sv.count) return 0;
    actual = sv_from_parts(sv.data, prefix.count);
    return sv_eq(prefix, actual);
}

i32 sv_ends_with(String_View sv, String_View suffix)
{
    String_View actual;
    if (suffix.count > sv.count) return 0;
    actual = sv_from_parts(sv.data + sv.count - suffix.count, suffix.count);
    return sv_eq(suffix, actual);
}

i32 sv_eq(String_View a, String_View b)
{
    if (a.count != b.count) return 0;
    return sv_memcmp_(a.data, b.data, a.count) == 0;
}

i32 sv_eq_ignorecase(String_View a, String_View b)
{
    size_t i;
    char x, y;

    if (a.count != b.count) return 0;
    for (i = 0; i < a.count; i++) {
        /* ('A'<=c && c<='Z') is already 0/1 as a boolean expression - add
         * 32*that instead of a ternary lowercase-or-not select, so
         * mixed-case comparisons over long strings don't pay a
         * per-character mispredictable branch. */
        x = a.data[i];
        x = (char)(x + 32 * ('A' <= x && x <= 'Z'));
        y = b.data[i];
        y = (char)(y + 32 * ('A' <= y && y <= 'Z'));
        if (x != y) return 0;
    }
    return 1;
}

u64 sv_to_u64(String_View sv)
{
    u64 result = 0;
    size_t i;
    for (i = 0; i < sv.count && isdigit((u8)sv.data[i]); i++) {
        result = result * 10 + (u64)(sv.data[i] - '0');
    }
    return result;
}

u64 sv_chop_u64(String_View *sv)
{
    u64 result = 0;
    while (sv->count > 0 && isdigit((u8)*sv->data)) {
        result = result * 10 + (u64)(*sv->data - '0');
        sv->count--;
        sv->data++;
    }
    return result;
}

String_View sv_chop_left_while(String_View *sv, i32 (*predicate)(char x))
{
    size_t i = 0;
    while (i < sv->count && predicate(sv->data[i])) i++;
    return sv_chop_left(sv, i);
}

String_View sv_take_left_while(String_View sv, i32 (*predicate)(char x))
{
    size_t i = 0;
    while (i < sv.count && predicate(sv.data[i])) i++;
    return sv_from_parts(sv.data, i);
}

#endif /* STRING_VIEW_IMPLEMENTATION_ */
#endif /* STRING_VIEW_IMPLEMENTATION */


/* ################ implementation: Arena_String ################ */
#if defined(ARENA_STRING_IMPLEMENTATION) && !defined(HOLYC_ARENA_ALLOC_IN_PROGRESS_)
#include "core/arena_alloc.h" /* the Arena_String bodies sit at the end of arena_alloc.h, after Arena is defined */
#endif /* ARENA_STRING_IMPLEMENTATION */

/* ################ implementation: print ################ */
#ifdef PRINT_IMPLEMENTATION
#ifndef PRINT_IMPLEMENTATION_
#define PRINT_IMPLEMENTATION_

/* raw Linux write(2) syscall, replacing unistd.h's wrapper - own local
 * copy (not shared with log's own copy below) matching this project's
 * standing "no cross-IMPLEMENTATION-guard dependency" rule. */
#if defined(__x86_64__)
static i64 print_sys_write_(i32 fd, const void *buf, size_t n)
{
    i64 ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(1), "D"(fd), "S"(buf), "d"(n)
        : "rcx", "r11", "memory"
    );
    return ret;
}
#elif defined(__i386__)
static i64 print_sys_write_(i32 fd, const void *buf, size_t n)
{
    i32 ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(4), "b"(fd), "c"(buf), "d"(n)
        : "memory"
    );
    return (i64)ret;
}
#else
#error "print.h's raw write() syscall is only implemented for x86_64/i386 Linux"
#endif

static char print_buffer_[PRINT_BUF_SIZE];
static size_t print_len_ = 0;

void print_flush(void)
{
    if (print_len_ > 0) {
        print_sys_write_(1, print_buffer_, print_len_);
        print_len_ = 0;
    }
}

void print_buf(const char *buf, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (print_len_ >= PRINT_BUF_SIZE) print_flush();
        print_buffer_[print_len_++] = buf[i];
    }
}

void print_char(char c) { print_buf(&c, 1); }
void print_cstr(const char *s) { while (*s) print_char(*s++); }

static const char print_digit_pairs_[] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

void print_u64_base(u64 v, u32 base)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[64];
    size_t n = 0;

    if (v == 0) {
        print_char('0');
        return;
    }

    if (base == 10) {
        while (v >= 100) {
            u32 pair = (u32)(v % 100) * 2;
            v /= 100;
            tmp[n++] = print_digit_pairs_[pair + 1];
            tmp[n++] = print_digit_pairs_[pair];
        }
        if (v >= 10) {
            u32 pair = (u32)v * 2;
            tmp[n++] = print_digit_pairs_[pair + 1];
            tmp[n++] = print_digit_pairs_[pair];
        } else {
            tmp[n++] = digits[v];
        }
    } else if (base != 0 && (base & (base - 1)) == 0) {
        u32 shift = (u32)__builtin_ctz(base);
        u32 mask = base - 1;
        while (v > 0) {
            tmp[n++] = digits[v & mask];
            v >>= shift;
        }
    } else {
        while (v > 0) {
            tmp[n++] = digits[v % base];
            v /= base;
        }
    }
    while (n > 0) print_char(tmp[--n]);
}

void print_i64_base(i64 v, u32 base)
{
    u64 uv;
    if (v < 0) {
        print_char('-');
        uv = (u64)(-(v + 1)) + 1; /* safe negation, avoids overflow at i64 min */
    } else {
        uv = (u64)v;
    }
    print_u64_base(uv, base);
}

void print_u64(u64 v) { print_u64_base(v, 10); }
void print_i64(i64 v) { print_i64_base(v, 10); }

#endif /* PRINT_IMPLEMENTATION_ */
#endif /* PRINT_IMPLEMENTATION */
