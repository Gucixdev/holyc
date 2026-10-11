#ifndef HOLYC_TIME_UTIL_H_
#define HOLYC_TIME_UTIL_H_

/* Monotonic timing + duration formatting. Renamed from an empty `time.h`
 * stub earlier this session because a project header literally named
 * `time.h` was silently shadowing the real system `<time.h>` - build's
 * `-I` auto-discovery makes every src/ subdirectory an include path, and
 * GCC's `-I` applies to <angle-bracket> includes too, not just "quoted"
 * ones. This file is unrelated to that bug, just inherited the safer
 * name.
 *
 * time_now_seconds() wraps CLOCK_MONOTONIC (not CLOCK_REALTIME - never
 * jumps backward on an NTP correction, the only sane choice for interval
 * timing), matching the pattern project/vecindex/vecindex_cli.c already
 * uses for its own benchmark timing.
 *
 * time_format_duration() writes a human-readable "1.234ms"-style string
 * into a CALLER buffer (unlike print.h, which only ever writes to its
 * own internal write(2) buffer) - hand-rolled fixed-point formatting,
 * no <stdio.h> sprintf, matching this project's "no format-string
 * parsing" stance. Picks s/ms/us/ns automatically from magnitude, always
 * 3 decimal places. */

#include "types/base.h"

f64 time_now_seconds(void);
void time_sleep_seconds(f64 seconds);

/* writes a NUL-terminated duration string into out (out_cap must be
 * >= 1); returns the number of characters written, NOT counting the
 * NUL, truncated to fit if out_cap is too small (same convention as
 * snprintf's return-value-vs-truncation split, minus the "would have
 * written" part - this project has no vsnprintf-based formatter to
 * measure that against). */
size_t time_format_duration(f64 seconds, char *out, size_t out_cap);

#endif /* HOLYC_TIME_UTIL_H_ */

#ifdef TIME_UTIL_IMPLEMENTATION
#ifndef TIME_UTIL_IMPLEMENTATION_
#define TIME_UTIL_IMPLEMENTATION_

#include <time.h>

f64 time_now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (f64)ts.tv_sec + (f64)ts.tv_nsec * 1e-9;
}

void time_sleep_seconds(f64 seconds)
{
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (i64)((seconds - (f64)ts.tv_sec) * 1e9); /* struct timespec's tv_nsec is `long`; i64 is the same width on this platform */
    nanosleep(&ts, 0);
}

static size_t time_util_write_u32_(char *out, size_t out_cap, size_t pos, u32 v, i32 min_digits)
{
    char tmp[10];
    size_t n = 0;
    u32 x = v;

    do {
        tmp[n++] = (char)('0' + (x % 10));
        x /= 10;
    } while (x > 0);
    while ((i32)n < min_digits && n < sizeof(tmp)) {
        tmp[n++] = '0';
    }

    while (n > 0 && pos < out_cap) {
        out[pos++] = tmp[--n];
    }
    return pos;
}

size_t time_format_duration(f64 seconds, char *out, size_t out_cap)
{
    f64 abs_s = seconds < 0.0 ? -seconds : seconds;
    f64 scaled;
    const char *unit;
    size_t unit_len;
    u32 whole, frac;
    i32 neg = seconds < 0.0;
    size_t pos = 0;

    if (out_cap == 0) return 0;

    if (abs_s >= 1.0)        { scaled = abs_s;        unit = "s";  unit_len = 1; }
    else if (abs_s >= 1e-3)  { scaled = abs_s * 1e3;   unit = "ms"; unit_len = 2; }
    else if (abs_s >= 1e-6)  { scaled = abs_s * 1e6;   unit = "us"; unit_len = 2; }
    else                     { scaled = abs_s * 1e9;   unit = "ns"; unit_len = 2; }

    whole = (u32)scaled;
    frac = (u32)((scaled - (f64)whole) * 1000.0 + 0.5);
    if (frac >= 1000) {
        frac -= 1000;
        whole += 1;
    }

    if (neg && pos < out_cap) out[pos++] = '-';
    pos = time_util_write_u32_(out, out_cap, pos, whole, 1);
    if (pos < out_cap) out[pos++] = '.';
    pos = time_util_write_u32_(out, out_cap, pos, frac, 3);
    {
        size_t i;
        for (i = 0; i < unit_len && pos < out_cap; i++) out[pos++] = unit[i];
    }

    if (pos < out_cap) {
        out[pos] = '\0';
    } else {
        out[out_cap - 1] = '\0';
        pos = out_cap - 1;
    }
    return pos;
}

#endif /* TIME_UTIL_IMPLEMENTATION_ */
#endif /* TIME_UTIL_IMPLEMENTATION */
