#ifndef HOLYC_TEST_UTIL_H_
#define HOLYC_TEST_UTIL_H_

/* Tiny shared test helper. Each test .c is its own single-TU executable
 * (own main, own IMPLEMENTATION defines) - test_failures_ being static
 * per-TU is exactly what we want here, not a shared global. */

#include "util/print.h"

static i32 test_failures_ = 0;

#define TEST_CHECK(cond, desc)                                     \
    do {                                                           \
        if (cond) {                                                \
            print_cstr("  PASS: "); print_cstr(desc); print_char('\n'); \
        } else {                                                   \
            print_cstr("  FAIL: "); print_cstr(desc); print_char('\n'); \
            test_failures_++;                                      \
        }                                                          \
    } while (0)

#endif /* HOLYC_TEST_UTIL_H_ */
