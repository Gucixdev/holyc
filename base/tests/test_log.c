#include "test_common.h"
#define LOG_IMPLEMENTATION
#include "log.h"
#include <unistd.h>
#include <string.h>

static i32 g_callback_calls_ = 0;
static Log_Level g_last_level_ = LOG_TRACE;
static char g_last_msg_[256];

static void test_callback_(const Log_Event *ev)
{
    g_callback_calls_++;
    g_last_level_ = ev->level;
    strncpy(g_last_msg_, ev->msg, sizeof(g_last_msg_) - 1);
    g_last_msg_[sizeof(g_last_msg_) - 1] = '\0';
}

static i32 g_lock_calls_[2]; /* [0]=unlock count, [1]=lock count */

static void test_lock_(i32 lock, void *udata)
{
    (void)udata;
    g_lock_calls_[lock]++;
}

int main(void)
{
    print_cstr("test_log\n");

    TEST_CHECK(strcmp(log_level_string(LOG_TRACE), "TRACE") == 0, "log_level_string: TRACE (LOG_ prefix stripped)");
    TEST_CHECK(strcmp(log_level_string(LOG_INFO), "INFO") == 0, "log_level_string: INFO");
    TEST_CHECK(strcmp(log_level_string(LOG_FATAL), "FATAL") == 0, "log_level_string: FATAL");

    log_set_quiet(1); /* keep this test's own stdout clean of log lines */

    log_add_callback(test_callback_, 0, LOG_INFO);
    log_info("hello callback");
    TEST_CHECK(g_callback_calls_ == 1, "log_add_callback: invoked once per matching log call");
    TEST_CHECK(g_last_level_ == LOG_INFO, "callback received the correct level");
    TEST_CHECK(strcmp(g_last_msg_, "hello callback") == 0, "callback received the exact message string, unmodified");

    log_debug("below the callback's registered level");
    TEST_CHECK(g_callback_calls_ == 1, "callback NOT invoked for a level below its own registered floor");

    log_set_lock(test_lock_, 0);
    log_info("locked call");
    TEST_CHECK(g_lock_calls_[1] == 1 && g_lock_calls_[0] == 1, "lock and unlock each called exactly once per log call");

    {
        i32 fds[2];
        char buf[512];
        ssize_t n;

        TEST_CHECK(pipe(fds) == 0, "pipe() for the fd-sink test succeeds");
        TEST_CHECK(log_add_fd(fds[1], LOG_WARN) == 0, "log_add_fd registers successfully");

        log_warn("goes to the pipe");
        n = read(fds[0], buf, sizeof(buf) - 1);
        TEST_CHECK(n > 0, "log_add_fd: something was actually written to the fd");
        if (n > 0) {
            buf[n] = '\0';
            TEST_CHECK(strstr(buf, "WARN") != 0, "fd-sink line contains the level name");
            TEST_CHECK(strstr(buf, "goes to the pipe") != 0, "fd-sink line contains the exact message");
        }
        close(fds[0]);
        close(fds[1]);
    }

    {
        i32 rc = -99;
        size_t i;
        for (i = 0; i < 40; i++) rc = log_add_callback(test_callback_, 0, LOG_TRACE);
        TEST_CHECK(rc == -1, "log_add_callback: fails once LOG_MAX_CALLBACKS is exhausted, doesn't overflow silently");
    }

    print_flush();
    return test_failures_ != 0;
}
