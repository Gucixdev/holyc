#ifndef HOLYC_TEST_COMMON_H_
#define HOLYC_TEST_COMMON_H_

/* Every one of tests/test_*.c is its own single-TU executable (own
 * main, own IMPLEMENTATION defines - see test_util.h's header comment
 * for why test_failures_ being static-per-TU, not shared, is exactly
 * what's wanted here) and every single one of them needs print.h (as
 * the IMPLEMENTATION - each test is the only TU in its binary, so
 * there's never a multi-definition risk from always defining it here)
 * plus test_util.h's TEST_CHECK/test_failures_. That was 3 identical
 * lines repeated verbatim at the top of all 46 test files - centralized
 * here into one #include so a test file only states what's actually
 * SPECIFIC to it: the module(s) it's testing, and (for the ~half that
 * need it) arena_alloc.h. test_util.h already includes print.h itself,
 * so defining PRINT_IMPLEMENTATION before including it is sufficient -
 * no separate #include "types/base.h" needed. */

#define PRINT_IMPLEMENTATION
#include "test_util.h"

#endif /* HOLYC_TEST_COMMON_H_ */
