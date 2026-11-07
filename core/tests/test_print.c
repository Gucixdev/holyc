#include "test_common.h"

#include <unistd.h>
#include <string.h>
#include <stdio.h>

/* print.h always writes to fd 1 via write(2) - redirect it to a pipe for
 * the duration of one call so the exact bytes can be checked against an
 * independent oracle (snprintf) instead of eyeballing terminal output. */
static i32 capture_matches_(void (*fn)(void), const char *expected)
{
    int pipefd[2];
    int saved_stdout;
    /* u64_max in base 2 is exactly 64 characters - a 64-byte buffer with
     * `sizeof(buf)-1` read room silently truncated it to 63 and produced a
     * false mismatch (found by the base-2 test itself failing). Room for
     * the longest possible output (64 binary digits) plus a safety margin. */
    char buf[128];
    ssize_t n;

    /* print.h's buffer is one shared global - drain anything already
     * sitting in it (e.g. our own PASS/FAIL lines from a previous check)
     * to the real terminal BEFORE redirecting fd 1, or that stale text
     * leaks into THIS capture instead */
    print_flush();

    pipe(pipefd);
    saved_stdout = dup(1);
    dup2(pipefd[1], 1);
    close(pipefd[1]);

    fn();
    print_flush();

    dup2(saved_stdout, 1);
    close(saved_stdout);

    n = read(pipefd[0], buf, sizeof(buf) - 1);
    close(pipefd[0]);
    if (n < 0) n = 0;
    buf[n] = '\0';

    return strcmp(buf, expected) == 0;
}

static u64 case_u64_;
static void call_print_u64_(void) { print_u64(case_u64_); }

static i64 case_i64_;
static void call_print_i64_(void) { print_i64(case_i64_); }

static i32 check_u64_(u64 v)
{
    char expected[32];
    /* plain `long` is 64-bit on this (x86_64 Linux) platform - avoids the
     * `long long`/-Wlong-long that -pedantic would flag under C89 */
    sprintf(expected, "%lu", (unsigned long)v);
    case_u64_ = v;
    return capture_matches_(call_print_u64_, expected);
}

static i32 check_i64_(i64 v)
{
    char expected[32];
    sprintf(expected, "%ld", (long)v);
    case_i64_ = v;
    return capture_matches_(call_print_i64_, expected);
}

static u64 case_hex_;
static void call_print_hex_(void) { print_u64_base(case_hex_, 16); }

static i32 check_hex_(u64 v)
{
    char expected[32];
    sprintf(expected, "%lx", (unsigned long)v);
    case_hex_ = v;
    return capture_matches_(call_print_hex_, expected);
}

static u64 case_pow2_;
static u32 case_pow2_base_;
static void call_print_pow2_(void) { print_u64_base(case_pow2_, case_pow2_base_); }

/* independent oracle for ANY power-of-2 base, not tied to print_u64_base's
 * own mask/shift so it can't share a bug with the thing it's checking -
 * sprintf has %o but nothing for base 2, so this covers both from one
 * hand-rolled reference instead of splitting into a %o-based check plus a
 * separate ad-hoc binary one */
static void oracle_pow2_(char *out, u64 v, u32 base)
{
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[64];
    size_t n = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (v > 0) { tmp[n++] = digits[v % base]; v /= base; }
    while (n > 0) *out++ = tmp[--n];
    *out = '\0';
}

static i32 check_pow2_(u64 v, u32 base)
{
    char expected[80];
    oracle_pow2_(expected, v, base);
    case_pow2_ = v;
    case_pow2_base_ = base;
    return capture_matches_(call_print_pow2_, expected);
}

int main(void)
{
    i32 all_u64_ok, all_i64_ok;
    /* deliberately spans the 1/2-digit and 2/3-digit boundaries the new
     * two-digits-at-a-time base-10 path branches on */
    u64 u64_cases[] = {
        0, 1, 9, 10, 11, 99, 100, 101, 999, 1000, 1001,
        9999, 10000, 12345, 99999999UL,
        (u64)1 << 32, (u64)1 << 63,
        18446744073709551615UL /* u64 max - `long` is 64-bit here, no need for a `long long` literal */
    };
    i64 i64_cases[] = {
        0, 1, -1, 9, -9, 10, -10, 99, -99, 100, -100,
        12345, -12345,
        (i64)((u64)1 << 62), -(i64)((u64)1 << 62)
    };
    size_t i, n_u64 = sizeof(u64_cases) / sizeof(u64_cases[0]);
    size_t n_i64 = sizeof(i64_cases) / sizeof(i64_cases[0]);
    /* i64 min needs its own case: -(-9223372036854775808) is not
     * representable as a positive i64 literal at all */
    i64 i64_min = (i64)((u64)1 << 63);

    print_cstr("test_print\n");

    all_u64_ok = 1;
    for (i = 0; i < n_u64; i++) {
        if (!check_u64_(u64_cases[i])) all_u64_ok = 0;
    }
    TEST_CHECK(all_u64_ok, "print_u64 matches snprintf(%lu) across boundary cases");

    all_i64_ok = 1;
    for (i = 0; i < n_i64; i++) {
        if (!check_i64_(i64_cases[i])) all_i64_ok = 0;
    }
    TEST_CHECK(all_i64_ok, "print_i64 matches snprintf(%ld) across boundary cases");

    TEST_CHECK(check_i64_(i64_min), "print_i64 handles the most negative i64 without overflow");

    case_u64_ = 255;
    TEST_CHECK(capture_matches_(call_print_u64_, "255"), "sanity: base-10 fast path itself (255)");

    {
        /* boundary-spans the base-16 mask+shift fast path the same way
         * u64_cases spans the base-10 one - 1/2-hex-digit and
         * 2/3-hex-digit transitions, plus 0xff/0x100 (the byte-pair LUT
         * boundary itself) */
        u64 hex_cases[] = {
            0, 1, 0xf, 0x10, 0x11, 0xff, 0x100, 0x101, 0xfff, 0x1000,
            0xdeadbeefUL, (u64)1 << 32, (u64)1 << 63, 18446744073709551615UL
        };
        size_t n_hex = sizeof(hex_cases) / sizeof(hex_cases[0]);
        i32 all_hex_ok = 1;

        for (i = 0; i < n_hex; i++) {
            if (!check_hex_(hex_cases[i])) all_hex_ok = 0;
        }
        TEST_CHECK(all_hex_ok, "print_u64_base(v,16) matches snprintf(%lx) across boundary cases");
    }

    {
        /* base=16 already covered above via snprintf(%lx); this proves the
         * generalized power-of-2 path (any base, not just 16) via an
         * independent from-scratch oracle, base=8 and base=2 specifically
         * since neither had a fast path before this pass */
        u64 pow2_cases[] = {
            0, 1, 7, 8, 9, 63, 64, 65, 255, 256, 257, (u64)1 << 32, 18446744073709551615UL
        };
        u32 pow2_bases[] = { 2, 4, 8, 32 };
        size_t n_cases = sizeof(pow2_cases) / sizeof(pow2_cases[0]);
        size_t n_bases = sizeof(pow2_bases) / sizeof(pow2_bases[0]);
        i32 all_pow2_ok = 1;
        size_t bi;

        for (bi = 0; bi < n_bases; bi++) {
            for (i = 0; i < n_cases; i++) {
                if (!check_pow2_(pow2_cases[i], pow2_bases[bi])) all_pow2_ok = 0;
            }
        }
        TEST_CHECK(all_pow2_ok, "print_u64_base matches an independent oracle for every power-of-2 base (2,4,8,32)");
    }

    print_flush();
    return test_failures_ != 0;
}
