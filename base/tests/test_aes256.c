#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define AES256_IMPLEMENTATION
#include "crypto/aes256.h"

int main(void)
{
    Arena arena = {0};

    print_cstr("test_aes256\n");

    /* FIPS-197 Appendix C.3 official AES-256 known-answer test vector -
     * cross-checked directly against Python's `cryptography` library's
     * own AES-256-ECB encrypt of the same key/plaintext. */
    {
        static const u8 key[32] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,
            0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f
        };
        static const u8 plaintext[16] = {
            0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
        };
        static const u8 expected_ct[16] = {
            0x8e,0xa2,0xb7,0xca,0x51,0x67,0x45,0xbf,0xea,0xfc,0x49,0x90,0x4b,0x49,0x60,0x89
        };
        Aes256_Key rk;
        u8 ct[16], decrypted[16];

        aes256_key_expand(key, &rk);
        aes256_encrypt_block(&rk, plaintext, ct);
        TEST_CHECK(memcmp(ct, expected_ct, 16) == 0, "single-block encrypt matches the official FIPS-197 AES-256 known-answer test vector");

        aes256_decrypt_block(&rk, ct, decrypted);
        TEST_CHECK(memcmp(decrypted, plaintext, 16) == 0, "decrypting that same ciphertext recovers the exact original plaintext");
    }

    /* CBC/PKCS#7 cross-check against Python's cryptography library -
     * independent real encoder, block-aligned plaintext (exercises the
     * "pad a full extra block" PKCS#7 edge case, not just a partial
     * final block). */
    {
        static const u8 key[32] = {
            187,2,236,7,75,254,99,142,157,73,179,181,74,53,226,187,
            243,73,199,105,213,230,190,131,162,104,123,249,147,165,201,114
        };
        static const u8 iv[16] = { 253,108,40,149,110,90,4,122,238,74,161,181,121,233,227,203 };
        static const u8 plaintext[32] = {
            72,101,108,108,111,44,32,65,69,83,45,50,53,54,45,67,
            66,67,33,32,67,114,111,115,115,45,99,104,101,99,107,46
        };
        static const u8 expected_ct[48] = {
            99,29,201,200,11,135,196,115,73,11,155,245,186,114,67,75,
            233,235,4,92,57,128,188,152,193,50,36,233,95,219,67,180,
            236,138,205,202,149,225,243,200,94,16,145,159,93,149,6,33
        };
        u8 *ct;
        size_t ct_size;
        u8 *decrypted;
        size_t decrypted_size;

        ct = aes256_cbc_encrypt(&arena, key, iv, plaintext, sizeof(plaintext), &ct_size);
        TEST_CHECK(ct_size == 48, "CBC ciphertext length: 32-byte (block-aligned) plaintext gets a full extra PKCS#7 padding block");
        TEST_CHECK(memcmp(ct, expected_ct, 48) == 0, "CBC ciphertext matches Python cryptography's own AES-256-CBC output byte-for-byte");

        TEST_CHECK(aes256_cbc_decrypt(&arena, key, iv, ct, ct_size, &decrypted, &decrypted_size) == 1, "decrypt of that ciphertext succeeds");
        TEST_CHECK(decrypted_size == sizeof(plaintext) && memcmp(decrypted, plaintext, sizeof(plaintext)) == 0, "decrypted plaintext matches exactly, padding stripped correctly");
    }

    /* our own round-trips across lengths that don't land on a real
     * external test vector - empty, one byte, exactly one block, and
     * several blocks plus a partial one. */
    {
        static const u8 key[32] = {
            1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
            17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32
        };
        static const u8 iv[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
        size_t lens[] = { 0, 1, 15, 16, 17, 33, 100 };
        size_t li;

        for (li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
            size_t n = lens[li];
            u8 plaintext[100];
            size_t i;
            u8 *ct;
            size_t ct_size;
            u8 *decrypted;
            size_t decrypted_size;
            i32 ok;

            for (i = 0; i < n; i++) plaintext[i] = (u8)(i * 7 + li);

            ct = aes256_cbc_encrypt(&arena, key, iv, plaintext, n, &ct_size);
            TEST_CHECK(ct_size % 16 == 0 && ct_size >= 16, "CBC output is always a non-empty multiple of 16 bytes");

            ok = aes256_cbc_decrypt(&arena, key, iv, ct, ct_size, &decrypted, &decrypted_size);
            TEST_CHECK(ok == 1, "round-trip decrypt succeeds for this length");
            TEST_CHECK(decrypted_size == n && (n == 0 || memcmp(decrypted, plaintext, n) == 0), "round-tripped plaintext matches exactly for this length");
        }
    }

    /* corrupted padding is rejected, not silently mis-truncated */
    {
        static const u8 key[32] = {0};
        static const u8 iv[16] = {0};
        static const u8 plaintext[5] = { 1, 2, 3, 4, 5 };
        u8 *ct;
        size_t ct_size;
        u8 *decrypted;
        size_t decrypted_size;

        ct = aes256_cbc_encrypt(&arena, key, iv, plaintext, sizeof(plaintext), &ct_size);
        ct[ct_size - 1] ^= 0xFF; /* corrupt the last byte, which is part of the padding */
        TEST_CHECK(aes256_cbc_decrypt(&arena, key, iv, ct, ct_size, &decrypted, &decrypted_size) == 0, "corrupted PKCS#7 padding is rejected, not silently accepted");
    }
    {
        static const u8 key[32] = {0};
        static const u8 iv[16] = {0};
        u8 *decrypted;
        size_t decrypted_size;
        u8 bad_len_ct[10] = {0};
        TEST_CHECK(aes256_cbc_decrypt(&arena, key, iv, bad_len_ct, sizeof(bad_len_ct), &decrypted, &decrypted_size) == 0, "ciphertext length not a multiple of 16 is rejected");
    }

    print_flush();
    return test_failures_ != 0;
}
