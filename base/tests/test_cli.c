#include "test_common.h"
#define CLI_IMPLEMENTATION
#include "cli.h"

static i32 streq_(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(void)
{
    print_cstr("test_cli\n");

    /* -a value, -b (flag), then a trailing non-option arg */
    {
        char *argv[] = { "prog", "-a", "hello", "-b", "file", 0 };
        i32 argc = 5;
        Cli_Getopt g = CLI_GETOPT_INIT;
        i32 r;

        r = cli_getopt(&g, argc, argv, "a:b");
        TEST_CHECK(r == 'a', "first call returns 'a'");
        TEST_CHECK(streq_(g.optarg, "hello"), "-a's optarg is the following argv entry");

        r = cli_getopt(&g, argc, argv, "a:b");
        TEST_CHECK(r == 'b', "second call returns 'b' (no argument needed)");

        r = cli_getopt(&g, argc, argv, "a:b");
        TEST_CHECK(r == -1, "third call returns -1 once options are exhausted");
        TEST_CHECK(streq_(argv[g.optind], "file"), "optind now points at the trailing non-option arg");
    }

    /* attached argument: -bvalue */
    {
        char *argv[] = { "prog", "-bvalue", 0 };
        Cli_Getopt g = CLI_GETOPT_INIT;
        i32 r = cli_getopt(&g, 2, argv, "b:");
        TEST_CHECK(r == 'b', "attached-argument form returns the option char");
        TEST_CHECK(streq_(g.optarg, "value"), "attached argument is read from the same argv word");
    }

    /* combined short flags: -xyz all as separate flags */
    {
        char *argv[] = { "prog", "-xyz", 0 };
        Cli_Getopt g = CLI_GETOPT_INIT;
        i32 rx, ry, rz, rend;
        rx = cli_getopt(&g, 2, argv, "xyz");
        ry = cli_getopt(&g, 2, argv, "xyz");
        rz = cli_getopt(&g, 2, argv, "xyz");
        rend = cli_getopt(&g, 2, argv, "xyz");
        TEST_CHECK(rx == 'x' && ry == 'y' && rz == 'z', "bundled flags -xyz yield x, then y, then z");
        TEST_CHECK(rend == -1, "bundled flags exhaust cleanly to -1");
    }

    /* unknown option -> '?' */
    {
        char *argv[] = { "prog", "-q", 0 };
        Cli_Getopt g = CLI_GETOPT_INIT;
        i32 r = cli_getopt(&g, 2, argv, "ab");
        TEST_CHECK(r == '?', "unrecognized option returns '?'");
    }

    /* required argument missing -> ':' */
    {
        char *argv[] = { "prog", "-a", 0 };
        Cli_Getopt g = CLI_GETOPT_INIT;
        i32 r = cli_getopt(&g, 2, argv, "a:");
        TEST_CHECK(r == ':', "missing required argument returns ':'");
    }

    /* -- terminates option parsing */
    {
        char *argv[] = { "prog", "--", "-a", 0 };
        Cli_Getopt g = CLI_GETOPT_INIT;
        i32 r = cli_getopt(&g, 3, argv, "a");
        TEST_CHECK(r == -1, "'--' stops option parsing immediately");
        TEST_CHECK(streq_(argv[g.optind], "-a"), "optind points past '--' at the literal '-a' argument");
    }

    print_flush();
    return test_failures_ != 0;
}
