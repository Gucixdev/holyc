#include "test_common.h"
#define SHA256_HASH_IMPLEMENTATION
#include "sha256_hash.h"

static i32 hex_eq_(const u8 digest[SHA256_DIGEST_SIZE], const char *hex)
{
    i32 i;
    for (i = 0; i < SHA256_DIGEST_SIZE; i++) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        u8 hi_v = (u8)(hi <= '9' ? hi - '0' : hi - 'a' + 10);
        u8 lo_v = (u8)(lo <= '9' ? lo - '0' : lo - 'a' + 10);
        u8 expected = (u8)((hi_v << 4) | lo_v);
        if (digest[i] != expected) return 0;
    }
    return 1;
}

int main(void)
{
    u8 digest[SHA256_DIGEST_SIZE];

    print_cstr("test_sha256_hash\n");

    sha256_hash("", 0, digest);
    TEST_CHECK(hex_eq_(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
               "SHA256(\"\") matches the well-known NIST test vector");

    sha256_hash("abc", 3, digest);
    TEST_CHECK(hex_eq_(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
               "SHA256(\"abc\") matches the well-known NIST test vector");

    sha256_hash("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, digest);
    TEST_CHECK(hex_eq_(digest, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
               "SHA256 of the 56-byte NIST multi-block vector matches (spans the second-block padding path)");

    {
        u8 d1[SHA256_DIGEST_SIZE], d2[SHA256_DIGEST_SIZE];
        i32 i, same = 1;
        sha256_hash("hello", 5, d1);
        sha256_hash("hello", 5, d2);
        for (i = 0; i < SHA256_DIGEST_SIZE; i++) if (d1[i] != d2[i]) same = 0;
        TEST_CHECK(same, "hashing the same input twice is deterministic");
    }

    print_flush();
    return test_failures_ != 0;
}
