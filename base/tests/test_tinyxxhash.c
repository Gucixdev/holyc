#include "test_common.h"
#define TINYXXHASH_IMPLEMENTATION
#include "tinyxxhash.h"
#include <string.h>

/* every expected value below was cross-checked against TWO independent
 * real reference implementations - the `xxhash` Python package (pip
 * install xxhash) AND the `xxhsum` reference CLI binary (both agree
 * exactly on every value here) - not recalled/assumed. See
 * docs/progress.md for how these were generated. */

int main(void)
{
    print_cstr("test_tinyxxhash\n");

    TEST_CHECK(xxhash32("", 0, 0) == 0x02cc5d05u, "xxhash32(\"\", seed=0) == 0x02cc5d05");
    TEST_CHECK(xxhash32("", 0, 1) == 0x0b2cb792u, "xxhash32(\"\", seed=1) == 0x0b2cb792");
    TEST_CHECK(xxhash32("test", 4, 0) == 0x3e2023cfu, "xxhash32(\"test\", seed=0) == 0x3e2023cf (cross-checked via xxhsum CLI too)");
    TEST_CHECK(xxhash32("test", 4, 1) == 0xf782311eu, "xxhash32(\"test\", seed=1) == 0xf782311e");
    TEST_CHECK(xxhash32("Hello, world!", 13, 0) == 0x31b7405du, "xxhash32(\"Hello, world!\", seed=0) == 0x31b7405d (cross-checked via xxhsum CLI too)");
    TEST_CHECK(xxhash32("Hello, world!", 13, 42) == 0x01d7440cu, "xxhash32(\"Hello, world!\", seed=42) == 0x01d7440c");
    TEST_CHECK(xxhash32("a", 1, 0) == 0x550d7456u, "xxhash32(\"a\", seed=0) == 0x550d7456 (1-byte input, no full 16-byte stripe)");
    TEST_CHECK(xxhash32("abc", 3, 0) == 0x32d153ffu, "xxhash32(\"abc\", seed=0) == 0x32d153ff");
    TEST_CHECK(xxhash32("abcdefghijklmnopqrstuvwxyz", 26, 0) == 0x63a14d5fu,
               "xxhash32(26-byte string, seed=0) == 0x63a14d5f (more than one 16-byte stripe)");
    TEST_CHECK(xxhash32("The quick brown fox jumps over the lazy dog", 43, 0) == 0xe85ea4deu,
               "xxhash32(44-byte string, seed=0) == 0xe85ea4de");

    TEST_CHECK(xxhash32("test", 4, 0) != xxhash32("test", 4, 1), "different seeds produce different hashes for the same input");
    TEST_CHECK(xxhash32("test", 4, 0) != xxhash32("tset", 4, 0), "different inputs produce different hashes for the same seed");

    print_flush();
    return test_failures_ != 0;
}
