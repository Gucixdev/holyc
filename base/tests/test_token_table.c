#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define ARENA_ALLOC_IMPLEMENTATION
#define HASHTABLE_IMPLEMENTATION
#define HASH_MAP_IMPLEMENTATION
#define TOKEN_TABLE_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "data/hash/hashmap.h"
#include "data/token_table.h"

#include <string.h>

int main(void)
{
    Arena a = {0};
    Token_Table t;
    i32 id_foo, id_bar, id_foo2;

    print_cstr("test_token_table\n");

    token_table_init(&a, &t, 8);

    id_foo = token_table_intern(&a, &t, "foo", 3);
    TEST_CHECK(id_foo == 0, "the first interned string gets id 0");

    id_bar = token_table_intern(&a, &t, "bar", 3);
    TEST_CHECK(id_bar == 1, "the second distinct string gets id 1");

    id_foo2 = token_table_intern(&a, &t, "foo", 3);
    TEST_CHECK(id_foo2 == id_foo, "interning the same string again returns the SAME id, not a new one");

    TEST_CHECK(strcmp(token_table_str(&t, id_foo), "foo") == 0, "token_table_str(id_foo) round-trips back to \"foo\"");
    TEST_CHECK(strcmp(token_table_str(&t, id_bar), "bar") == 0, "token_table_str(id_bar) round-trips back to \"bar\"");

    TEST_CHECK(token_table_str(&t, 99) == NULL, "an out-of-range id returns NULL, not garbage");
    TEST_CHECK(token_table_str(&t, -1) == NULL, "a negative id returns NULL too");

    /* enough distinct strings to force the internal `strings` array to
     * grow past its initial capacity, proving growth doesn't corrupt
     * earlier ids */
    {
        char buf[16];
        i32 i;
        i32 ids[40];
        for (i = 0; i < 40; i++) {
            buf[0] = 'x'; buf[1] = (char)('0' + i / 10); buf[2] = (char)('0' + i % 10); buf[3] = '\0';
            ids[i] = token_table_intern(&a, &t, buf, 3);
        }
        {
            i32 ok = 1;
            for (i = 0; i < 40; i++) {
                buf[0] = 'x'; buf[1] = (char)('0' + i / 10); buf[2] = (char)('0' + i % 10); buf[3] = '\0';
                if (strcmp(token_table_str(&t, ids[i]), buf) != 0) { ok = 0; break; }
            }
            TEST_CHECK(ok, "all 40 growth-forcing strings still round-trip correctly by id after the array grew");
        }
        TEST_CHECK(strcmp(token_table_str(&t, id_foo), "foo") == 0, "the original \"foo\" id still resolves correctly after growth");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
