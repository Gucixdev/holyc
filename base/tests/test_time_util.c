#include "test_common.h"
#define TIME_UTIL_IMPLEMENTATION
#include "util/time_util.h"
#include <string.h>

int main(void)
{
    print_cstr("test_time_util\n");

    {
        f64 t0 = time_now_seconds();
        f64 t1 = time_now_seconds();
        TEST_CHECK(t1 >= t0, "time_now_seconds: monotonic (never goes backward)");
        TEST_CHECK(t0 > 0.0, "time_now_seconds: returns a positive value");
    }

    {
        f64 t0 = time_now_seconds();
        time_sleep_seconds(0.01);
        {
            f64 t1 = time_now_seconds();
            TEST_CHECK(t1 - t0 >= 0.008, "time_sleep_seconds: actually slept roughly the requested time");
        }
    }

    {
        char buf[32];
        size_t n;

        n = time_format_duration(1.234, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "1.234s") == 0, "format_duration: 1.234s");
        TEST_CHECK(n == strlen("1.234s"), "format_duration: return length matches");

        time_format_duration(0.001234, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "1.234ms") == 0, "format_duration: 1.234ms");

        time_format_duration(0.000001234, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "1.234us") == 0, "format_duration: 1.234us");

        time_format_duration(0.000000001234, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "1.234ns") == 0, "format_duration: 1.234ns");

        time_format_duration(0.0, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "0.000ns") == 0, "format_duration: zero");

        time_format_duration(2.0, buf, sizeof(buf));
        TEST_CHECK(strcmp(buf, "2.000s") == 0, "format_duration: whole-second value still gets 3 decimals");
    }

    {
        char buf[4];
        size_t n = time_format_duration(1.234, buf, sizeof(buf));
        TEST_CHECK(n == 3, "format_duration: truncates to fit a small buffer, returns the truncated length");
        TEST_CHECK(buf[3] == '\0', "format_duration: still NUL-terminates within out_cap when truncated");
    }

    print_flush();
    return test_failures_ != 0;
}
