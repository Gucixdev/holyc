#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#include "core/base.h"
#include "core/flags.c"
#include <stdlib.h>

int main(void)
{
    Flags f;

    print_cstr("test_flags\n");

    unsetenv("TEST_VERBOSE");
    unsetenv("TEST_QUIET");
    unsetenv("TEST_NO_COLOR");
    unsetenv("TEST_DRY_RUN");
    unsetenv("NO_COLOR");

    flags_init(&f, "TEST");
    TEST_CHECK(!f.verbose && !f.quiet && !f.no_color && !f.dry_run, "with nothing set, every flag defaults to off");

    setenv("TEST_VERBOSE", "1", 1);
    flags_init(&f, "TEST");
    TEST_CHECK(f.verbose == 1, "a prefixed env var turns its flag on");
    unsetenv("TEST_VERBOSE");

    setenv("TEST_DRY_RUN", "0", 1);
    flags_init(&f, "TEST");
    TEST_CHECK(f.dry_run == 0, "an env var explicitly set to \"0\" does NOT count as enabled");
    unsetenv("TEST_DRY_RUN");

    setenv("NO_COLOR", "1", 1);
    flags_init(&f, "TEST");
    TEST_CHECK(f.no_color == 1, "the bare NO_COLOR convention (no-color.org) is honored even without the tool prefix");
    unsetenv("NO_COLOR");

    {
        char *argv[] = { "prog", "positional", "--verbose", "-q", "--dry-run", NULL };
        flags_init(&f, "TEST");
        flags_apply_argv(&f, 5, argv);
        TEST_CHECK(f.verbose && f.quiet && f.dry_run, "argv flags are recognized regardless of position, alongside an unrelated positional arg");
        TEST_CHECK(!f.no_color, "a flag not present in argv and not set in env stays off");
    }

    {
        char *argv[] = { "prog", NULL };
        flags_init(&f, "TEST");
        f.verbose = 1; /* simulating env already having set it */
        flags_apply_argv(&f, 1, argv);
        TEST_CHECK(f.verbose == 1, "flags_apply_argv never turns an already-set flag back off");
    }

    print_flush();
    return test_failures_ != 0;
}
