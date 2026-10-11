#ifndef HOLYC_SHA256_HASH_H_
#define HOLYC_SHA256_HASH_H_

/* SHA-256 (FIPS 180-4), one-shot: sha256_hash(data, len, out_digest)
 * writes the 32-byte digest. Named sha256_hash.h (not sha256.h) to
 * avoid the exact same real-libc-header collision this project's own
 * ./check hygiene pass already flagged once for this file (a real
 * /usr/include/sha256.h exists on this machine, from libbsd) - same
 * "shadows a real header, rename it" fix qoi.h got for the same
 * reason. Not constant-time - this is for content hashing/integrity
 * checks (e.g. a real payload for hash/merkletree.h/hash/hashtree.h to
 * use), not secret material where timing side channels matter; a
 * constant-time build would be a real, separate, documented decision,
 * not silently assumed here. */

#include "types/base.h"

#define SHA256_DIGEST_SIZE 32

void sha256_hash(const void *data, size_t len, u8 out_digest[SHA256_DIGEST_SIZE]);

#endif /* HOLYC_SHA256_HASH_H_ */

#ifdef SHA256_HASH_IMPLEMENTATION
#ifndef SHA256_HASH_IMPLEMENTATION_
#define SHA256_HASH_IMPLEMENTATION_

static const u32 sha256_k_[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static u32 sha256_rotr_(u32 x, i32 n) { return (x >> n) | (x << (32 - n)); }

static void sha256_process_block_(u32 h[8], const u8 block[64])
{
    u32 w[64];
    u32 a, b, c, d, e, f, g, hh;
    i32 i;

    for (i = 0; i < 16; i++) {
        w[i] = ((u32)block[i * 4] << 24) | ((u32)block[i * 4 + 1] << 16)
             | ((u32)block[i * 4 + 2] << 8) | (u32)block[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        u32 s0 = sha256_rotr_(w[i - 15], 7) ^ sha256_rotr_(w[i - 15], 18) ^ (w[i - 15] >> 3);
        u32 s1 = sha256_rotr_(w[i - 2], 17) ^ sha256_rotr_(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];

    for (i = 0; i < 64; i++) {
        u32 s1 = sha256_rotr_(e, 6) ^ sha256_rotr_(e, 11) ^ sha256_rotr_(e, 25);
        u32 ch = (e & f) ^ (~e & g);
        u32 t1 = hh + s1 + ch + sha256_k_[i] + w[i];
        u32 s0 = sha256_rotr_(a, 2) ^ sha256_rotr_(a, 13) ^ sha256_rotr_(a, 22);
        u32 maj = (a & b) ^ (a & c) ^ (b & c);
        u32 t2 = s0 + maj;

        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void sha256_hash(const void *data, size_t len, u8 out_digest[SHA256_DIGEST_SIZE])
{
    u32 h[8];
    const u8 *bytes = (const u8 *)data;
    u8 block[64];
    size_t full_blocks = len / 64;
    size_t rem = len % 64;
    size_t i, j;
    u64 bit_len = (u64)len * 8;

    h[0] = 0x6a09e667u; h[1] = 0xbb67ae85u; h[2] = 0x3c6ef372u; h[3] = 0xa54ff53au;
    h[4] = 0x510e527fu; h[5] = 0x9b05688cu; h[6] = 0x1f83d9abu; h[7] = 0x5be0cd19u;

    for (i = 0; i < full_blocks; i++) {
        sha256_process_block_(h, bytes + i * 64);
    }

    /* final padded block(s): remaining bytes + 0x80 + zeros + 8-byte
     * big-endian bit length, spilling into a second block if the
     * remainder leaves no room for the length field */
    for (j = 0; j < rem; j++) block[j] = bytes[full_blocks * 64 + j];
    block[rem] = 0x80;

    if (rem < 56) {
        for (j = rem + 1; j < 56; j++) block[j] = 0;
        for (j = 0; j < 8; j++) block[56 + j] = (u8)(bit_len >> (56 - j * 8));
        sha256_process_block_(h, block);
    } else {
        for (j = rem + 1; j < 64; j++) block[j] = 0;
        sha256_process_block_(h, block);

        for (j = 0; j < 56; j++) block[j] = 0;
        for (j = 0; j < 8; j++) block[56 + j] = (u8)(bit_len >> (56 - j * 8));
        sha256_process_block_(h, block);
    }

    for (i = 0; i < 8; i++) {
        out_digest[i * 4 + 0] = (u8)(h[i] >> 24);
        out_digest[i * 4 + 1] = (u8)(h[i] >> 16);
        out_digest[i * 4 + 2] = (u8)(h[i] >> 8);
        out_digest[i * 4 + 3] = (u8)(h[i]);
    }
}

#endif /* SHA256_HASH_IMPLEMENTATION_ */
#endif /* SHA256_HASH_IMPLEMENTATION */
