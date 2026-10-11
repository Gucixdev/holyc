#include "test_common.h"
#include "types/xmacro.h"
#include <string.h>

/* the one canonical list - everything below is generated from this,
 * not hand-maintained separately */
#define COLOR_LIST(X) \
    X(COLOR_RED)      \
    X(COLOR_GREEN)    \
    X(COLOR_BLUE)      \
    X(COLOR_YELLOW)

typedef enum { XM_ENUM(COLOR_LIST) COLOR_SENTINEL_UNUSED_ } Color;
static const char *color_names_[] = { XM_STRINGS(COLOR_LIST) };
#define COLOR_COUNT XM_COUNT(COLOR_LIST)

int main(void)
{
    print_cstr("test_xmacro\n");

    TEST_CHECK(COLOR_COUNT == 4, "XM_COUNT: matches the number of entries in the list");
    TEST_CHECK((i32)(sizeof(color_names_) / sizeof(color_names_[0])) == COLOR_COUNT,
               "XM_STRINGS: array size agrees with XM_COUNT (same list, can't disagree)");

    TEST_CHECK(COLOR_RED == 0 && COLOR_GREEN == 1 && COLOR_BLUE == 2 && COLOR_YELLOW == 3,
               "XM_ENUM: values assigned in list order, starting at 0");

    TEST_CHECK(strcmp(color_names_[COLOR_RED], "COLOR_RED") == 0, "XM_STRINGS: name matches enum identifier text");
    TEST_CHECK(strcmp(color_names_[COLOR_GREEN], "COLOR_GREEN") == 0, "XM_STRINGS: second entry too");
    TEST_CHECK(strcmp(color_names_[COLOR_BLUE], "COLOR_BLUE") == 0, "XM_STRINGS: third entry too");
    TEST_CHECK(strcmp(color_names_[COLOR_YELLOW], "COLOR_YELLOW") == 0, "XM_STRINGS: fourth entry too");

    {
        i32 i;
        i32 all_indexed_correctly = 1;
        for (i = 0; i < COLOR_COUNT; i++) {
            if (color_names_[i][0] != 'C') all_indexed_correctly = 0;
        }
        TEST_CHECK(all_indexed_correctly, "XM_COUNT used as a real loop bound over the generated string table");
    }

    print_flush();
    return test_failures_ != 0;
}
