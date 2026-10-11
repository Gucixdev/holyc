#include "test_common.h"
#define ARGS_IMPLEMENTATION
#include "core/args.h"

static char *fake_argv_(char *s) { return s; } /* silences a string-literal-to-char* warning at each call site below */

int main(void)
{
    Args_Spec specs[4];
    specs[0].name = "verbose"; specs[0].short_name = 'v'; specs[0].takes_value = 0;
    specs[1].name = "output";  specs[1].short_name = 'o'; specs[1].takes_value = 1;
    specs[2].name = "count";   specs[2].short_name = 'c'; specs[2].takes_value = 1;
    specs[3].name = "help";    specs[3].short_name = 'h'; specs[3].takes_value = 0;

    print_cstr("test_args\n");

    /* long flag, long option with '=', positional args */
    {
        char *argv[] = {
            fake_argv_("prog"), fake_argv_("--verbose"), fake_argv_("--output=out.txt"),
            fake_argv_("file1.txt"), fake_argv_("file2.txt")
        };
        Args_Result r;
        args_parse(5, argv, specs, 4, &r);

        TEST_CHECK(r.error == NULL, "a well-formed command line parses with no error");
        TEST_CHECK(args_has(&r, "verbose") == 1, "a long flag with no value is recorded as present");
        TEST_CHECK(args_get(&r, "verbose") == NULL, "a flag's own value is NULL (it never takes one)");
        TEST_CHECK(args_get(&r, "output") != NULL, "a long option with '=value' has a non-NULL value");
        {
            const char *v = args_get(&r, "output");
            TEST_CHECK(v[0] == 'o' && v[1] == 'u' && v[2] == 't', "the '=value' form's value is exactly what followed the '='");
        }
        TEST_CHECK(r.positional_count == 2, "two positional (non-option) arguments were collected");
        TEST_CHECK(r.positional[0][0] == 'f' && r.positional[1][0] == 'f', "positional args keep their original order");
    }

    /* long option with a separate-token value, short flag, short option (separate token) */
    {
        char *argv[] = {
            fake_argv_("prog"), fake_argv_("--output"), fake_argv_("result.bin"),
            fake_argv_("-v"), fake_argv_("-c"), fake_argv_("42")
        };
        Args_Result r;
        args_parse(6, argv, specs, 4, &r);

        TEST_CHECK(r.error == NULL, "separate-token option values parse without error");
        {
            const char *v = args_get(&r, "output");
            TEST_CHECK(v != NULL && v[0] == 'r', "'--output result.bin' (separate tokens) captures the next token as the value");
        }
        TEST_CHECK(args_has(&r, "verbose") == 1, "the short flag -v maps back to its long name \"verbose\"");
        {
            const char *v = args_get(&r, "count");
            TEST_CHECK(v != NULL && v[0] == '4' && v[1] == '2', "'-c 42' (separate tokens) captures \"42\" as the value");
        }
    }

    /* short option with an attached value */
    {
        char *argv[] = { fake_argv_("prog"), fake_argv_("-oquick.txt") };
        Args_Result r;
        args_parse(2, argv, specs, 4, &r);

        TEST_CHECK(r.error == NULL, "an attached short-option value parses without error");
        {
            const char *v = args_get(&r, "output");
            TEST_CHECK(v != NULL && v[0] == 'q', "'-oquick.txt' captures \"quick.txt\" as the attached value");
        }
    }

    /* unknown option is a real, reported error - not silently ignored or fatal */
    {
        char *argv[] = { fake_argv_("prog"), fake_argv_("--nonsense") };
        Args_Result r;
        args_parse(2, argv, specs, 4, &r);

        TEST_CHECK(r.error != NULL, "an unrecognized long option sets a non-NULL error");
    }

    /* an option requiring a value with nothing left is a real, reported error */
    {
        char *argv[] = { fake_argv_("prog"), fake_argv_("--output") };
        Args_Result r;
        args_parse(2, argv, specs, 4, &r);

        TEST_CHECK(r.error != NULL, "an option that takes a value, given with nothing after it, sets a non-NULL error");
    }

    /* the last occurrence of a repeated option wins */
    {
        char *argv[] = { fake_argv_("prog"), fake_argv_("--count=1"), fake_argv_("--count=2"), fake_argv_("--count=3") };
        Args_Result r;
        args_parse(4, argv, specs, 4, &r);

        {
            const char *v = args_get(&r, "count");
            TEST_CHECK(v != NULL && v[0] == '3', "when an option is given multiple times, the LAST occurrence wins");
        }
        TEST_CHECK(r.match_count == 1, "repeating the same option does not create duplicate match entries");
    }

    /* argv[0] (the program name itself) is never treated as an argument */
    {
        char *argv[] = { fake_argv_("prog"), fake_argv_("positional_only") };
        Args_Result r;
        args_parse(2, argv, specs, 4, &r);

        TEST_CHECK(r.positional_count == 1, "argv[0] is skipped - only the real argument becomes positional");
        TEST_CHECK(r.positional[0][0] == 'p', "the one positional argument is the real one, not the program name");
    }

    print_flush();
    return test_failures_ != 0;
}
