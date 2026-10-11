#ifndef HOLYC_STR_H_
#define HOLYC_STR_H_

/* str (own C-string/memory primitives) + String_View (non-owning
 * {data,count} view over existing text) + Arena_String (arena-allocated
 * growable string builder) - three independent pieces (str never needs
 * to view/own anything, String_View never needs to grow/own memory,
 * Arena_String never needs to parse/scan) merged into one file (was
 * string_view.h/arena_string.h, merged back) since together they're "this project's basic string
 * operations" - and per explicit follow-up direction, str itself is a
 * real, own hand-rolled replacement for the handful of <string.h>
 * functions this codebase actually needs (str_len/str_cmp/str_ncmp/
 * str_cpy/str_ncpy/str_chr/str_str/mem_cpy/mem_move/mem_cmp/mem_set),
 * so THIS file needs zero #include <string.h> of its own - matching
 * base.h's own "0 glibc" stance, just scoped to the string domain
 * instead of base types/math/print/log. Simple byte-at-a-time loops
 * throughout, no perf claim made or needed (unlike arena_alloc.h's
 * arena_memcpy, which deliberately calls libc's real memcpy on its own
 * documented, benchmarked hot-path grounds - a different, equally
 * deliberate tradeoff for a different file). Each of the three pieces
 * still stands alone under its OWN IMPLEMENTATION guard macro
 * (STR_IMPLEMENTATION/STRING_VIEW_IMPLEMENTATION/
 * ARENA_STRING_IMPLEMENTATION, unchanged from the split files for the
 * latter two) - a caller wanting only one still only pulls in that
 * one's code. String_View's own two tiny internal needs (a strlen/
 * memcmp equivalent) get their OWN distinctly-named private copies
 * inside STRING_VIEW_IMPLEMENTATION (sv_strlen_/sv_memcmp_) rather than
 * calling into STR_IMPLEMENTATION's public str_len/mem_cmp - the same
 * "own local copy, not a cross-IMPLEMENTATION-guard dependency" rule
 * base.h's vector_sqrt_/quat_sqrt_/matrix_sin_ already established,
 * needed here for the same reason: STRING_VIEW_IMPLEMENTATION must
 * still link standalone even if a caller never defines
 * STR_IMPLEMENTATION at all. */

#include "base.h"
#include "arena_alloc.h"

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

#endif /* HOLYC_STR_H_ */

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

#ifdef ARENA_STRING_IMPLEMENTATION
#ifndef ARENA_STRING_IMPLEMENTATION_
#define ARENA_STRING_IMPLEMENTATION_

void arena_string_init(Arena_String *s, Arena *a)
{
    s->a = a;
    s->items = 0;
    s->count = 0;
    s->capacity = 0;
}

void arena_string_clear(Arena_String *s)
{
    s->count = 0;
}

void arena_string_append_char(Arena_String *s, char c)
{
    arena_da_append(s->a, s, c);
}

void arena_string_append_buf(Arena_String *s, const char *buf, size_t n)
{
    arena_da_append_many(s->a, s, buf, n);
}

void arena_string_append_cstr(Arena_String *s, const char *cstr)
{
    arena_string_append_buf(s, cstr, arena_strlen(cstr));
}

/* same two-digits-per-division trick print.h's print_u64_base(v,10) uses -
 * reimplemented here rather than reused, since print.h's version writes
 * straight to its own internal write(2) buffer with no way to redirect
 * output into an arbitrary caller buffer. */
static const char arena_string_digit_pairs_[] =
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

void arena_string_append_u64(Arena_String *s, u64 v)
{
    char tmp[24];
    size_t n = 0;

    if (v == 0) {
        arena_string_append_char(s, '0');
        return;
    }

    while (v >= 100) {
        u32 pair = (u32)(v % 100) * 2;
        v /= 100;
        tmp[n++] = arena_string_digit_pairs_[pair + 1];
        tmp[n++] = arena_string_digit_pairs_[pair];
    }
    if (v >= 10) {
        u32 pair = (u32)v * 2;
        tmp[n++] = arena_string_digit_pairs_[pair + 1];
        tmp[n++] = arena_string_digit_pairs_[pair];
    } else {
        tmp[n++] = (char)('0' + v);
    }

    while (n > 0) arena_string_append_char(s, tmp[--n]);
}

void arena_string_append_i64(Arena_String *s, i64 v)
{
    u64 uv;
    if (v < 0) {
        arena_string_append_char(s, '-');
        uv = (u64)(-(v + 1)) + 1; /* safe negation, avoids overflow at i64 min - same idiom as print.h */
    } else {
        uv = (u64)v;
    }
    arena_string_append_u64(s, uv);
}

const char *arena_string_cstr(Arena_String *s)
{
    size_t saved_count = s->count;
    arena_da_append(s->a, s, '\0');
    s->count = saved_count; /* NUL written but not counted, same convention arena_sb_append_null documents */
    return s->items;
}

#endif /* ARENA_STRING_IMPLEMENTATION_ */
#endif /* ARENA_STRING_IMPLEMENTATION */
