#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define PERFECTHASH_IMPLEMENTATION
#include "core/arena_alloc.h"
#include "data/hash/perfecthash.h"

/* C89 reserved keywords - the exact kind of fixed key set this is for */
static const char *const c89_keywords[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do",
    "double", "else", "enum", "extern", "float", "for", "goto", "if",
    "int", "long", "register", "return", "short", "signed", "sizeof", "static",
    "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while"
};
#define N_KEYWORDS (sizeof(c89_keywords) / sizeof(c89_keywords[0]))

int main(void)
{
    Arena a = {0};
    Perfect_Hash ph;
    u8 seen[N_KEYWORDS];
    const char *slot_to_key[N_KEYWORDS];
    i32 build_ok, all_distinct, all_roundtrip;
    size_t i;

    print_cstr("test_perfecthash\n");

    build_ok = perfect_hash_build(&a, &ph, c89_keywords, N_KEYWORDS);
    TEST_CHECK(build_ok, "perfect_hash_build succeeds for 32 C89 keywords");
    TEST_CHECK(ph.n == N_KEYWORDS, "hash table size matches key count");

    for (i = 0; i < N_KEYWORDS; i++) { seen[i] = 0; slot_to_key[i] = NULL; }

    all_distinct = 1;
    for (i = 0; i < N_KEYWORDS; i++) {
        size_t slot = perfect_hash_lookup(&ph, c89_keywords[i]);
        if (slot >= N_KEYWORDS || seen[slot]) { all_distinct = 0; break; }
        seen[slot] = 1;
        slot_to_key[slot] = c89_keywords[i];
    }
    TEST_CHECK(all_distinct, "every keyword maps to a distinct in-range slot (0 collisions)");

    all_roundtrip = 1;
    for (i = 0; i < N_KEYWORDS; i++) {
        if (slot_to_key[i] == NULL || perfect_hash_lookup(&ph, slot_to_key[i]) != i) {
            all_roundtrip = 0;
            break;
        }
    }
    TEST_CHECK(all_roundtrip, "lookup(key at slot i) == i for every slot (round-trip consistent)");

    /* n=1 edge case */
    {
        static const char *const one_key[] = { "solo" };
        Perfect_Hash ph1;
        i32 ok1 = perfect_hash_build(&a, &ph1, one_key, 1);
        TEST_CHECK(ok1, "build succeeds for a single-key set");
        TEST_CHECK(perfect_hash_lookup(&ph1, "solo") == 0, "single key maps to slot 0");
    }

    /* n=0 edge case */
    {
        Perfect_Hash ph0;
        i32 ok0 = perfect_hash_build(&a, &ph0, NULL, 0);
        TEST_CHECK(ok0, "build succeeds for an empty key set");
        TEST_CHECK(ph0.n == 0, "empty key set produces n == 0");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
