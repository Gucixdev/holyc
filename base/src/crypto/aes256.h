#ifndef HOLYC_AES256_H_
#define HOLYC_AES256_H_

/* AES-256 (FIPS-197), from-scratch: key expansion (14 round keys),
 * single-block encrypt/decrypt (SubBytes/ShiftRows/MixColumns/
 * AddRoundKey and their inverses), plus CBC mode (PKCS#7 padding) on
 * top since a raw 16-byte-block cipher alone isn't directly usable for
 * arbitrary-length data. S-box/inverse S-box are the standard FIPS-197
 * tables (256 bytes each, not derived at runtime - the derivation
 * itself is real but not worth the code size here). */

#include "types/base.h"
#include "alloc/arena_alloc.h"

#define AES256_BLOCK_SIZE 16
#define AES256_KEY_SIZE 32
#define AES256_ROUNDS 14
#define AES256_ROUND_KEYS_SIZE ((AES256_ROUNDS + 1) * 16) /* 15 round keys, 16 bytes each */

typedef struct {
    u8 round_keys[AES256_ROUND_KEYS_SIZE];
} Aes256_Key;

void aes256_key_expand(const u8 key[AES256_KEY_SIZE], Aes256_Key *out);

/* single 16-byte block, in place is fine (in == out allowed). */
void aes256_encrypt_block(const Aes256_Key *key, const u8 in[16], u8 out[16]);
void aes256_decrypt_block(const Aes256_Key *key, const u8 in[16], u8 out[16]);

/* CBC mode with PKCS#7 padding. iv is 16 bytes, consumed not modified.
 * Encrypt: returns the arena-owned ciphertext (always a multiple of 16
 * bytes, at least 16 - padding always adds 1-16 bytes, even for empty
 * input). *out_size is its length.
 * Decrypt: returns 1 on success (padding validated - a corrupted
 * ciphertext with bad padding is rejected, not silently truncated
 * wrong), 0 on failure. *out_data/*out_size are arena-owned on success. */
u8 *aes256_cbc_encrypt(Arena *a, const u8 key[AES256_KEY_SIZE], const u8 iv[16],
                        const u8 *plaintext, size_t plaintext_len, size_t *out_size);
i32 aes256_cbc_decrypt(Arena *a, const u8 key[AES256_KEY_SIZE], const u8 iv[16],
                        const u8 *ciphertext, size_t ciphertext_len, u8 **out_data, size_t *out_size);

#endif /* HOLYC_AES256_H_ */

#ifdef AES256_IMPLEMENTATION
#ifndef AES256_IMPLEMENTATION_
#define AES256_IMPLEMENTATION_

static const u8 aes256_sbox_[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const u8 aes256_inv_sbox_[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const u8 aes256_rcon_[15] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36,0x6c,0xd8,0xab,0x4d
};

static u8 aes256_xtime_(u8 x) { return (u8)((x << 1) ^ ((x & 0x80) ? 0x1B : 0x00)); }

static u8 aes256_gmul_(u8 a, u8 b)
{
    u8 p = 0;
    i32 i;
    for (i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        a = aes256_xtime_(a);
        b >>= 1;
    }
    return p;
}

void aes256_key_expand(const u8 key[AES256_KEY_SIZE], Aes256_Key *out)
{
    /* Nk=8 words of key, Nr=14 rounds -> 60 words (4*(Nr+1)) total,
     * standard FIPS-197 key schedule with the Nk>6 extra SubWord step
     * every 4th word (specific to AES-256, not present in AES-128). */
    u8 w[60][4];
    i32 i;

    for (i = 0; i < 8; i++) {
        w[i][0] = key[4 * i + 0]; w[i][1] = key[4 * i + 1];
        w[i][2] = key[4 * i + 2]; w[i][3] = key[4 * i + 3];
    }

    for (i = 8; i < 60; i++) {
        u8 temp[4];
        temp[0] = w[i - 1][0]; temp[1] = w[i - 1][1]; temp[2] = w[i - 1][2]; temp[3] = w[i - 1][3];

        if (i % 8 == 0) {
            u8 t0 = temp[0];
            temp[0] = aes256_sbox_[temp[1]] ^ aes256_rcon_[i / 8];
            temp[1] = aes256_sbox_[temp[2]];
            temp[2] = aes256_sbox_[temp[3]];
            temp[3] = aes256_sbox_[t0];
        } else if (i % 8 == 4) {
            temp[0] = aes256_sbox_[temp[0]];
            temp[1] = aes256_sbox_[temp[1]];
            temp[2] = aes256_sbox_[temp[2]];
            temp[3] = aes256_sbox_[temp[3]];
        }

        w[i][0] = (u8)(w[i - 8][0] ^ temp[0]);
        w[i][1] = (u8)(w[i - 8][1] ^ temp[1]);
        w[i][2] = (u8)(w[i - 8][2] ^ temp[2]);
        w[i][3] = (u8)(w[i - 8][3] ^ temp[3]);
    }

    for (i = 0; i < 60; i++) {
        out->round_keys[4 * i + 0] = w[i][0];
        out->round_keys[4 * i + 1] = w[i][1];
        out->round_keys[4 * i + 2] = w[i][2];
        out->round_keys[4 * i + 3] = w[i][3];
    }
}

static void aes256_add_round_key_(u8 state[16], const u8 *round_key)
{
    i32 i;
    for (i = 0; i < 16; i++) state[i] ^= round_key[i];
}

static void aes256_sub_bytes_(u8 state[16])
{
    i32 i;
    for (i = 0; i < 16; i++) state[i] = aes256_sbox_[state[i]];
}

static void aes256_inv_sub_bytes_(u8 state[16])
{
    i32 i;
    for (i = 0; i < 16; i++) state[i] = aes256_inv_sbox_[state[i]];
}

/* state is column-major (state[col*4+row], the FIPS-197 convention). */
static void aes256_shift_rows_(u8 state[16])
{
    u8 t;
    t = state[1]; state[1] = state[5]; state[5] = state[9]; state[9] = state[13]; state[13] = t;
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    t = state[3]; state[3] = state[15]; state[15] = state[11]; state[11] = state[7]; state[7] = t;
}

static void aes256_inv_shift_rows_(u8 state[16])
{
    u8 t;
    t = state[13]; state[13] = state[9]; state[9] = state[5]; state[5] = state[1]; state[1] = t;
    t = state[2]; state[2] = state[10]; state[10] = t;
    t = state[6]; state[6] = state[14]; state[14] = t;
    t = state[7]; state[7] = state[11]; state[11] = state[15]; state[15] = state[3]; state[3] = t;
}

static void aes256_mix_columns_(u8 state[16])
{
    i32 c;
    for (c = 0; c < 4; c++) {
        u8 *col = state + c * 4;
        u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = (u8)(aes256_gmul_(a0, 2) ^ aes256_gmul_(a1, 3) ^ a2 ^ a3);
        col[1] = (u8)(a0 ^ aes256_gmul_(a1, 2) ^ aes256_gmul_(a2, 3) ^ a3);
        col[2] = (u8)(a0 ^ a1 ^ aes256_gmul_(a2, 2) ^ aes256_gmul_(a3, 3));
        col[3] = (u8)(aes256_gmul_(a0, 3) ^ a1 ^ a2 ^ aes256_gmul_(a3, 2));
    }
}

static void aes256_inv_mix_columns_(u8 state[16])
{
    i32 c;
    for (c = 0; c < 4; c++) {
        u8 *col = state + c * 4;
        u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = (u8)(aes256_gmul_(a0, 14) ^ aes256_gmul_(a1, 11) ^ aes256_gmul_(a2, 13) ^ aes256_gmul_(a3, 9));
        col[1] = (u8)(aes256_gmul_(a0, 9) ^ aes256_gmul_(a1, 14) ^ aes256_gmul_(a2, 11) ^ aes256_gmul_(a3, 13));
        col[2] = (u8)(aes256_gmul_(a0, 13) ^ aes256_gmul_(a1, 9) ^ aes256_gmul_(a2, 14) ^ aes256_gmul_(a3, 11));
        col[3] = (u8)(aes256_gmul_(a0, 11) ^ aes256_gmul_(a1, 13) ^ aes256_gmul_(a2, 9) ^ aes256_gmul_(a3, 14));
    }
}

void aes256_encrypt_block(const Aes256_Key *key, const u8 in[16], u8 out[16])
{
    u8 state[16];
    i32 round;
    i32 i;
    for (i = 0; i < 16; i++) state[i] = in[i];

    aes256_add_round_key_(state, key->round_keys);

    for (round = 1; round < AES256_ROUNDS; round++) {
        aes256_sub_bytes_(state);
        aes256_shift_rows_(state);
        aes256_mix_columns_(state);
        aes256_add_round_key_(state, key->round_keys + round * 16);
    }

    aes256_sub_bytes_(state);
    aes256_shift_rows_(state);
    aes256_add_round_key_(state, key->round_keys + AES256_ROUNDS * 16);

    for (i = 0; i < 16; i++) out[i] = state[i];
}

void aes256_decrypt_block(const Aes256_Key *key, const u8 in[16], u8 out[16])
{
    u8 state[16];
    i32 round;
    i32 i;
    for (i = 0; i < 16; i++) state[i] = in[i];

    aes256_add_round_key_(state, key->round_keys + AES256_ROUNDS * 16);

    for (round = AES256_ROUNDS - 1; round >= 1; round--) {
        aes256_inv_shift_rows_(state);
        aes256_inv_sub_bytes_(state);
        aes256_add_round_key_(state, key->round_keys + round * 16);
        aes256_inv_mix_columns_(state);
    }

    aes256_inv_shift_rows_(state);
    aes256_inv_sub_bytes_(state);
    aes256_add_round_key_(state, key->round_keys);

    for (i = 0; i < 16; i++) out[i] = state[i];
}

u8 *aes256_cbc_encrypt(Arena *a, const u8 key[AES256_KEY_SIZE], const u8 iv[16],
                        const u8 *plaintext, size_t plaintext_len, size_t *out_size)
{
    Aes256_Key rk;
    size_t pad_len = 16 - (plaintext_len % 16);
    size_t total = plaintext_len + pad_len;
    u8 *buf = (u8 *)arena_alloc(a, total);
    u8 prev[16];
    size_t block;
    i32 i;

    aes256_key_expand(key, &rk);
    for (i = 0; i < 16; i++) prev[i] = iv[i];

    for (block = 0; block < total; block += 16) {
        u8 in_block[16];
        for (i = 0; i < 16; i++) {
            size_t src_i = block + (size_t)i;
            u8 byte = (src_i < plaintext_len) ? plaintext[src_i] : (u8)pad_len; /* PKCS#7 */
            in_block[i] = (u8)(byte ^ prev[i]);
        }
        aes256_encrypt_block(&rk, in_block, buf + block);
        for (i = 0; i < 16; i++) prev[i] = buf[block + i];
    }

    *out_size = total;
    return buf;
}

i32 aes256_cbc_decrypt(Arena *a, const u8 key[AES256_KEY_SIZE], const u8 iv[16],
                        const u8 *ciphertext, size_t ciphertext_len, u8 **out_data, size_t *out_size)
{
    Aes256_Key rk;
    u8 *buf;
    u8 prev[16];
    size_t block;
    i32 i;
    u8 pad_len;

    if (ciphertext_len == 0 || ciphertext_len % 16 != 0) return 0;

    aes256_key_expand(key, &rk);
    buf = (u8 *)arena_alloc(a, ciphertext_len);
    for (i = 0; i < 16; i++) prev[i] = iv[i];

    for (block = 0; block < ciphertext_len; block += 16) {
        u8 decrypted[16];
        aes256_decrypt_block(&rk, ciphertext + block, decrypted);
        for (i = 0; i < 16; i++) {
            buf[block + i] = (u8)(decrypted[i] ^ prev[i]);
            prev[i] = ciphertext[block + i];
        }
    }

    pad_len = buf[ciphertext_len - 1];
    if (pad_len == 0 || pad_len > 16 || (size_t)pad_len > ciphertext_len) return 0;
    for (i = 0; i < pad_len; i++) {
        if (buf[ciphertext_len - 1 - (size_t)i] != pad_len) return 0; /* corrupt padding - reject, not silently truncate wrong */
    }

    *out_data = buf;
    *out_size = ciphertext_len - pad_len;
    return 1;
}

#endif /* AES256_IMPLEMENTATION_ */
#endif /* AES256_IMPLEMENTATION */
