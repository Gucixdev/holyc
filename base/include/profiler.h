#ifndef HOLYC_PROFILER_H_
#define HOLYC_PROFILER_H_

/* Lightweight named-scope timing profiler: profiler_begin(p, "name") /
 * profiler_end(p, "name") around a region, accumulating total elapsed
 * time + call count per distinct name across a run, plus a summary
 * report printed via print.h (not <stdio.h>, matching this project's
 * "no format-string parsing" stance - see time_util.h's own duration
 * formatter, which this reuses for the report's numbers).
 *
 * Each named scope gets its own fixed slot with its own "currently
 * running" start-time field (found/created by linear search+strcmp over
 * a small fixed-capacity array - PROFILER_MAX_SCOPES, real profiling
 * runs have a handful of hot named regions, not thousands, so this
 * beats pulling in hashtable.h for something this size). That means
 * NESTING SCOPES WITH DIFFERENT NAMES works correctly (each has its own
 * start-time slot), but re-entering the SAME name before its matching
 * end (recursion profiling the same named region) does NOT - the second
 * begin() overwrites the first's start time. A real, deliberate scope
 * cut: correct recursive-region profiling needs a real per-name stack,
 * not just one slot, and this project's design rules ban recursion in its
 * own code anyway (the one place this would bite is instrumenting
 * someone else's recursive code, not a normal use case here). */

#include "base.h"
#include "print.h"
#include "time_util.h"

/* own local strcmp_ - own copy per this project's standing "no cross-
 * IMPLEMENTATION-guard dependency" rule, avoiding a <string.h>
 * dependency for one small scope-name comparison. */
static i32 profiler_strcmp_(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (i32)(u8)*a - (i32)(u8)*b;
}

#ifndef PROFILER_MAX_SCOPES
#define PROFILER_MAX_SCOPES 64
#endif

typedef struct {
    const char *name;
    f64 start_time;
    f64 total_seconds;
    u64 call_count;
    i32 running;
} Profiler_Scope;

typedef struct {
    Profiler_Scope scopes[PROFILER_MAX_SCOPES];
    size_t scope_count;
} Profiler;

void profiler_init(Profiler *p);
void profiler_begin(Profiler *p, const char *name);
void profiler_end(Profiler *p, const char *name);

/* NULL if that name was never begun. */
const Profiler_Scope *profiler_get(const Profiler *p, const char *name);

/* prints "name: total=X.XXXms calls=N avg=X.XXXus" per scope via
 * print.h, in the order scopes were first seen. */
void profiler_report(const Profiler *p);

#endif /* HOLYC_PROFILER_H_ */

#ifdef PROFILER_IMPLEMENTATION
#ifndef PROFILER_IMPLEMENTATION_
#define PROFILER_IMPLEMENTATION_

void profiler_init(Profiler *p)
{
    p->scope_count = 0;
}

static Profiler_Scope *profiler_find_or_create_(Profiler *p, const char *name)
{
    size_t i;
    for (i = 0; i < p->scope_count; i++) {
        if (profiler_strcmp_(p->scopes[i].name, name) == 0) return &p->scopes[i];
    }
    if (p->scope_count >= PROFILER_MAX_SCOPES) return 0; /* out of slots - silently drop, not a crash */

    {
        Profiler_Scope *s = &p->scopes[p->scope_count++];
        s->name = name;
        s->start_time = 0.0;
        s->total_seconds = 0.0;
        s->call_count = 0;
        s->running = 0;
        return s;
    }
}

void profiler_begin(Profiler *p, const char *name)
{
    Profiler_Scope *s = profiler_find_or_create_(p, name);
    if (!s) return;
    s->start_time = time_now_seconds();
    s->running = 1;
}

void profiler_end(Profiler *p, const char *name)
{
    f64 now = time_now_seconds();
    Profiler_Scope *s = profiler_find_or_create_(p, name);
    if (!s || !s->running) return;
    s->total_seconds += now - s->start_time;
    s->call_count++;
    s->running = 0;
}

const Profiler_Scope *profiler_get(const Profiler *p, const char *name)
{
    size_t i;
    for (i = 0; i < p->scope_count; i++) {
        if (profiler_strcmp_(p->scopes[i].name, name) == 0) return &p->scopes[i];
    }
    return 0;
}

void profiler_report(const Profiler *p)
{
    size_t i;
    char buf[32];

    for (i = 0; i < p->scope_count; i++) {
        const Profiler_Scope *s = &p->scopes[i];
        f64 avg = s->call_count > 0 ? s->total_seconds / (f64)s->call_count : 0.0;

        print_cstr(s->name);
        print_cstr(": total=");
        time_format_duration(s->total_seconds, buf, sizeof(buf));
        print_cstr(buf);
        print_cstr(" calls=");
        print_u64(s->call_count);
        print_cstr(" avg=");
        time_format_duration(avg, buf, sizeof(buf));
        print_cstr(buf);
        print_char('\n');
    }
    print_flush();
}

#endif /* PROFILER_IMPLEMENTATION_ */
#endif /* PROFILER_IMPLEMENTATION */
