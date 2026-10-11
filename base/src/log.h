#ifndef HOLYC_LOG_H_
#define HOLYC_LOG_H_

/* log - ported from rxi's log.c (MIT). split out of base.h (was base.h's
 * own "log" section, moved out, same move as
 * print.h's own split just above it in base.h's original layout).
 * Diverges from upstream in one deliberate way: no printf-style variadic
 * macros - build the message string with print.h's own calls (or a
 * literal) first, same "no format-string parsing" stance print.h itself
 * takes. Log_Level below is a plain hand-written enum + string table
 * (upstream's xmacro.h-generated version, without the xmacro.h
 * dependency). Same LOG_IMPLEMENTATION guard name as before the move. */

#include "base.h"

/* ============================================================
 * log (log.h) - ported from rxi's log.c (MIT). Diverges from upstream
 * in one deliberate way: no printf-style variadic macros - build the
 * message string with print.h's own calls (or a literal) first, same
 * "no format-string parsing" stance print.h itself takes. Log_Level
 * below is a plain hand-written enum + string table (upstream's
 * xmacro.h-generated version, without the xmacro.h dependency - see
 * this file's own top comment for why).
 * ============================================================ */

typedef enum {
    LOG_TRACE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
} Log_Level;

/* UTC hour/min/sec only (upstream's struct tm has a full Y/M/D/etc
 * breakdown, but log_format_ below only ever prints HH:MM:SS, so
 * that's all this carries - no need to replicate the rest of a struct
 * tm just to leave it unused). See this file's own top comment for
 * why UTC, not local time. */
typedef struct {
    i32 hour, min, sec;
} Log_Time;

typedef struct {
    const char *msg;
    const char *file;
    i32 line;
    Log_Level level;
    Log_Time time;
    void *udata;
} Log_Event;

typedef void (*Log_Fn)(const Log_Event *ev);
typedef void (*Log_Lock_Fn)(i32 lock, void *udata); /* lock: 1 to acquire, 0 to release */

#define log_trace(msg) log_log(LOG_TRACE, __FILE__, __LINE__, msg)
#define log_debug(msg) log_log(LOG_DEBUG, __FILE__, __LINE__, msg)
#define log_info(msg)  log_log(LOG_INFO,  __FILE__, __LINE__, msg)
#define log_warn(msg)  log_log(LOG_WARN,  __FILE__, __LINE__, msg)
#define log_error(msg) log_log(LOG_ERROR, __FILE__, __LINE__, msg)
#define log_fatal(msg) log_log(LOG_FATAL, __FILE__, __LINE__, msg)

/* name without the shared "LOG_" prefix ("TRACE", not "LOG_TRACE"). */
const char *log_level_string(Log_Level level);

void log_set_lock(Log_Lock_Fn fn, void *udata);
void log_set_level(Log_Level level); /* messages below this level are dropped from the default stdout sink */
void log_set_quiet(i32 enable);      /* stdout sink only - callbacks/fd sinks still run */

/* returns 0 on success, -1 if LOG_MAX_CALLBACKS is already full */
i32 log_add_callback(Log_Fn fn, void *udata, Log_Level level);
i32 log_add_fd(i32 fd, Log_Level level); /* convenience file-sink callback, write(2)-based */

void log_log(Log_Level level, const char *file, i32 line, const char *msg);
#endif /* HOLYC_LOG_H_ */

#ifdef LOG_IMPLEMENTATION
#ifndef LOG_IMPLEMENTATION_
#define LOG_IMPLEMENTATION_

/* raw Linux write(2)/time(2) syscalls, replacing unistd.h/time.h's
 * wrappers - own local copies (not shared with print's own copy
 * above) matching this project's standing "no cross-IMPLEMENTATION-
 * guard dependency" rule. */
#if defined(__x86_64__)
static i64 log_sys_write_(i32 fd, const void *buf, size_t n)
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

static i64 log_sys_time_(void)
{
    i64 ret;
    __asm__ volatile (
        "syscall"
        : "=a"(ret)
        : "a"(201), "D"(0)
        : "rcx", "r11", "memory"
    );
    return ret;
}
#elif defined(__i386__)
static i64 log_sys_write_(i32 fd, const void *buf, size_t n)
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

static i64 log_sys_time_(void)
{
    i32 ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(13), "b"(0)
        : "memory"
    );
    return (i64)ret;
}
#else
#error "log.h's raw write()/time() syscalls are only implemented for x86_64/i386 Linux"
#endif

#define LOG_MAX_CALLBACKS 32
#define LOG_MAX_FD_SINKS 8
#define LOG_LINE_MAX 1024

typedef struct {
    Log_Fn fn;
    void *udata;
    Log_Level level;
} Log_Callback_;

static struct {
    void *udata;
    Log_Lock_Fn lock;
    Log_Level level;
    i32 quiet;
    Log_Callback_ callbacks[LOG_MAX_CALLBACKS];
    size_t callback_count;
} log_state_;

static i32 log_fd_sinks_[LOG_MAX_FD_SINKS];
static size_t log_fd_sink_count_ = 0;

static const char *log_level_strings_[] = {
    "LOG_TRACE", "LOG_DEBUG", "LOG_INFO", "LOG_WARN", "LOG_ERROR", "LOG_FATAL"
};

const char *log_level_string(Log_Level level)
{
    return log_level_strings_[level] + 4; /* skip the shared "LOG_" prefix */
}

void log_set_lock(Log_Lock_Fn fn, void *udata)
{
    log_state_.lock = fn;
    log_state_.udata = udata;
}

void log_set_level(Log_Level level) { log_state_.level = level; }
void log_set_quiet(i32 enable) { log_state_.quiet = enable; }

i32 log_add_callback(Log_Fn fn, void *udata, Log_Level level)
{
    if (log_state_.callback_count >= LOG_MAX_CALLBACKS) return -1;
    log_state_.callbacks[log_state_.callback_count].fn = fn;
    log_state_.callbacks[log_state_.callback_count].udata = udata;
    log_state_.callbacks[log_state_.callback_count].level = level;
    log_state_.callback_count++;
    return 0;
}

static void log_append_str_(char **p, char *end, const char *s)
{
    while (*s && *p < end) *(*p)++ = *s++;
}

static void log_append_char_(char **p, char *end, char c)
{
    if (*p < end) *(*p)++ = c;
}

static void log_append_u32_(char **p, char *end, u32 v)
{
    char tmp[10];
    i32 n = 0;
    if (v == 0) { log_append_char_(p, end, '0'); return; }
    while (v > 0) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n > 0) log_append_char_(p, end, tmp[--n]);
}

static void log_append_u32_padded2_(char **p, char *end, i32 v)
{
    log_append_char_(p, end, (char)('0' + (v / 10) % 10));
    log_append_char_(p, end, (char)('0' + v % 10));
}

static size_t log_format_(const Log_Event *ev, char *buf, size_t cap)
{
    char *p = buf;
    char *end = buf + cap;

    log_append_u32_padded2_(&p, end, ev->time.hour);
    log_append_char_(&p, end, ':');
    log_append_u32_padded2_(&p, end, ev->time.min);
    log_append_char_(&p, end, ':');
    log_append_u32_padded2_(&p, end, ev->time.sec);
    log_append_char_(&p, end, ' ');

    log_append_str_(&p, end, log_level_string(ev->level));
    log_append_char_(&p, end, ' ');

    log_append_str_(&p, end, ev->file);
    log_append_char_(&p, end, ':');
    log_append_u32_(&p, end, (u32)ev->line);
    log_append_str_(&p, end, ": ");

    log_append_str_(&p, end, ev->msg);
    log_append_char_(&p, end, '\n');

    return (size_t)(p - buf);
}

static void log_stdout_sink_(const Log_Event *ev)
{
    char buf[LOG_LINE_MAX];
    size_t n = log_format_(ev, buf, sizeof(buf));
    log_sys_write_(1, buf, n);
}

static void log_fd_callback_(const Log_Event *ev)
{
    char buf[LOG_LINE_MAX];
    size_t n = log_format_(ev, buf, sizeof(buf));
    i32 fd = *(const i32 *)ev->udata;
    log_sys_write_(fd, buf, n);
}

i32 log_add_fd(i32 fd, Log_Level level)
{
    i32 rc;
    if (log_fd_sink_count_ >= LOG_MAX_FD_SINKS) return -1;
    log_fd_sinks_[log_fd_sink_count_] = fd;
    rc = log_add_callback(log_fd_callback_, &log_fd_sinks_[log_fd_sink_count_], level);
    if (rc == 0) log_fd_sink_count_++;
    return rc;
}

static void log_lock_(void)   { if (log_state_.lock) log_state_.lock(1, log_state_.udata); }
static void log_unlock_(void) { if (log_state_.lock) log_state_.lock(0, log_state_.udata); }

void log_log(Log_Level level, const char *file, i32 line, const char *msg)
{
    Log_Event ev;
    i64 epoch, secs_of_day;

    epoch = log_sys_time_();
    secs_of_day = epoch % 86400; /* epoch is always >= 0 post-1970, no need to handle negative */
    ev.time.hour = (i32)(secs_of_day / 3600);
    ev.time.min = (i32)((secs_of_day % 3600) / 60);
    ev.time.sec = (i32)(secs_of_day % 60);
    ev.msg = msg;
    ev.file = file;
    ev.line = line;
    ev.level = level;
    ev.udata = 0;

    log_lock_();

    if (!log_state_.quiet && level >= log_state_.level) {
        log_stdout_sink_(&ev);
    }

    {
        size_t i;
        for (i = 0; i < log_state_.callback_count; i++) {
            if (level >= log_state_.callbacks[i].level) {
                Log_Event cb_ev = ev;
                cb_ev.udata = log_state_.callbacks[i].udata;
                log_state_.callbacks[i].fn(&cb_ev);
            }
        }
    }

    log_unlock_();
}

#endif /* LOG_IMPLEMENTATION_ */
#endif /* LOG_IMPLEMENTATION */
