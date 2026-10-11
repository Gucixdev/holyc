#include "test_common.h"
#define MARMUR_IMPLEMENTATION
#include "data/hash/marmur.h"
#include <string.h>

/* every expected value below was cross-checked against the real
 * `mmh3` Python package (pip install mmh3 - a widely-used, independent
 * MurmurHash3 implementation), not recalled/assumed - see
 * docs/progress.md for how these were generated. */

int main(void)
{
    print_cstr("test_marmur\n");

    TEST_CHECK(murmur3_32("", 0, 0) == 0x00000000u, "murmur3_32(\"\", seed=0) == 0x00000000");
    TEST_CHECK(murmur3_32("", 0, 1) == 0x514e28b7u, "murmur3_32(\"\", seed=1) == 0x514e28b7");
    TEST_CHECK(murmur3_32("test", 4, 0) == 0xba6bd213u, "murmur3_32(\"test\", seed=0) == 0xba6bd213");
    TEST_CHECK(murmur3_32("test", 4, 1) == 0x99c02ae2u, "murmur3_32(\"test\", seed=1) == 0x99c02ae2");
    TEST_CHECK(murmur3_32("Hello, world!", 13, 0) == 0xc0363e43u, "murmur3_32(\"Hello, world!\", seed=0) == 0xc0363e43");
    TEST_CHECK(murmur3_32("Hello, world!", 13, 42) == 0x2c8c8533u, "murmur3_32(\"Hello, world!\", seed=42) == 0x2c8c8533");
    TEST_CHECK(murmur3_32("a", 1, 0) == 0x3c2569b2u, "murmur3_32(\"a\", seed=0) == 0x3c2569b2 (1-byte tail)");
    TEST_CHECK(murmur3_32("abc", 3, 0) == 0xb3dd93fau, "murmur3_32(\"abc\", seed=0) == 0xb3dd93fa (3-byte tail)");
    TEST_CHECK(murmur3_32("abcdefghijklmnopqrstuvwxyz", 26, 0) == 0xa34e036du,
               "murmur3_32(26-byte string, seed=0) == 0xa34e036d (multiple full blocks)");
    TEST_CHECK(murmur3_32("The quick brown fox jumps over the lazy dog", 43, 0) == 0x2e4ff723u,
               "murmur3_32(44-byte string, seed=0) == 0x2e4ff723");

    {
        u8 zeros[4] = { 0, 0, 0, 0 };
        TEST_CHECK(murmur3_32(zeros, 4, 0) == 0x2362f9deu, "murmur3_32(4 zero bytes, seed=0) == 0x2362f9de");
    }
    {
        u8 ones[4] = { 0xff, 0xff, 0xff, 0xff };
        TEST_CHECK(murmur3_32(ones, 4, 0) == 0x76293b50u, "murmur3_32(4x 0xff bytes, seed=0) == 0x76293b50");
    }

    TEST_CHECK(murmur3_32("test", 4, 0) != murmur3_32("test", 4, 1), "different seeds produce different hashes for the same input");
    TEST_CHECK(murmur3_32("test", 4, 0) != murmur3_32("tset", 4, 0), "different inputs produce different hashes for the same seed");

    print_flush();
    return test_failures_ != 0;
}
