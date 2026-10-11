#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define STRING_VIEW_IMPLEMENTATION
#define CSV_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "core/base.h"
#include "codec/csv.h"
#include <string.h>

static i32 field_eq_(String_View sv, const char *expected)
{
    size_t len = strlen(expected);
    return sv.count == len && memcmp(sv.data, expected, len) == 0;
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_csv\n");

    {
        const char *text = "a,b,c\n1,2,3\n";
        Csv_Table table;
        i32 err = csv_parse(&a, text, strlen(text), &table);

        TEST_CHECK(err == CSV_ESUCCESS, "plain CSV parses successfully");
        TEST_CHECK(table.count == 2, "two rows parsed");
        TEST_CHECK(table.items[0].count == 3, "header row has 3 fields");
        TEST_CHECK(field_eq_(table.items[0].items[0], "a") &&
                   field_eq_(table.items[0].items[1], "b") &&
                   field_eq_(table.items[0].items[2], "c"), "header fields match");
        TEST_CHECK(field_eq_(table.items[1].items[0], "1") &&
                   field_eq_(table.items[1].items[1], "2") &&
                   field_eq_(table.items[1].items[2], "3"), "data row fields match");
    }

    {
        const char *text = "name,note\n\"Smith, John\",\"a \"\"quoted\"\" word\"\r\n";
        Csv_Table table;
        i32 err = csv_parse(&a, text, strlen(text), &table);

        TEST_CHECK(err == CSV_ESUCCESS, "quoted CSV with embedded comma/escaped-quote parses successfully");
        TEST_CHECK(table.count == 2, "two rows parsed (quoted case)");
        TEST_CHECK(field_eq_(table.items[1].items[0], "Smith, John"),
                   "quoted field's embedded comma is preserved, not treated as a delimiter");
        TEST_CHECK(field_eq_(table.items[1].items[1], "a \"quoted\" word"),
                   "\"\" inside a quoted field unescapes to a single \"");
    }

    {
        const char *text = "a,\"multi\nline\"\nb,c\n";
        Csv_Table table;
        i32 err = csv_parse(&a, text, strlen(text), &table);

        TEST_CHECK(err == CSV_ESUCCESS, "quoted field containing a real newline parses successfully");
        TEST_CHECK(table.count == 2, "embedded newline did not create an extra row");
        TEST_CHECK(field_eq_(table.items[0].items[1], "multi\nline"),
                   "the embedded newline is preserved inside the field's value");
    }

    {
        const char *text = "a,\"unterminated";
        Csv_Table table;
        i32 err = csv_parse(&a, text, strlen(text), &table);
        TEST_CHECK(err == CSV_EUNCLOSED, "an unclosed quoted field is rejected cleanly, not crashed on");
    }

    {
        const char *text = "just,one,row";
        Csv_Table table;
        i32 err = csv_parse(&a, text, strlen(text), &table);
        TEST_CHECK(err == CSV_ESUCCESS && table.count == 1 && table.items[0].count == 3,
                   "a final row with no trailing newline still counts");
    }

    print_flush();
    return test_failures_ != 0;
}
