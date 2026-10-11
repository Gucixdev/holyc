#ifndef HOLYC_UUID4_H_
#define HOLYC_UUID4_H_

/* Ported from uuid4 (rxi, MIT). Their version seeds a private
 * xorshift128+ from /dev/urandom; this project already has one general-
 * purpose PRNG (rng.h's PCG32) and the project rule is not to carry two
 * generators doing the same job, so the OS-entropy step now seeds an
 * ordinary Rng instead of a second, uuid-specific generator. Everything
 * else (RFC 4122 version/variant bit placement, the dashed hex string
 * layout) is the same shape as the original. */

#include "base.h"
#include "rng.h"

#define UUID4_STR_LEN 37 /* 8-4-4-4-12 hex + 4 dashes + NUL */

typedef struct {
    u8 bytes[16];
} Uuid4;

/* reads 16 bytes from /dev/urandom to seed *out_rng - real OS entropy is
 * what makes generated ids actually unlikely to collide; a fixed/weak
 * seed here would defeat the whole point. Returns 0 on success, -1 if
 * the entropy source couldn't be read (out_rng is left untouched). */
i32 uuid4_seed_from_os(Rng *out_rng);

Uuid4 uuid4_generate(Rng *rng);

/* dst must be >= UUID4_STR_LEN bytes */
void uuid4_to_string(Uuid4 id, char *dst);

#endif /* HOLYC_UUID4_H_ */

#ifdef UUID_IMPLEMENTATION
#ifndef UUID_IMPLEMENTATION_
#define UUID_IMPLEMENTATION_

#include <fcntl.h>
#include <unistd.h>

i32 uuid4_seed_from_os(Rng *out_rng)
{
    i32 fd;
    u64 seed[2];
    i64 got;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    got = read(fd, seed, sizeof(seed));
    close(fd);
    if (got != (i64)sizeof(seed)) return -1;

    pcg32_seed(out_rng, seed[0], seed[1]);
    return 0;
}

Uuid4 uuid4_generate(Rng *rng)
{
    Uuid4 id;
    u32 words[4];
    size_t i;

    words[0] = pcg32_u32(rng);
    words[1] = pcg32_u32(rng);
    words[2] = pcg32_u32(rng);
    words[3] = pcg32_u32(rng);

    for (i = 0; i < 16; i++) {
        u32 w = words[i / 4];
        size_t shift = (i % 4) * 8;
        id.bytes[i] = (u8)((w >> shift) & 0xffu);
    }

    /* RFC 4122: version nibble in byte 6's high bits, variant bits (10xx)
     * in byte 8's top 2 bits - the only structure a "random" UUIDv4
     * actually has. */
    id.bytes[6] = (u8)((id.bytes[6] & 0x0fu) | 0x40u);
    id.bytes[8] = (u8)((id.bytes[8] & 0x3fu) | 0x80u);

    return id;
}

void uuid4_to_string(Uuid4 id, char *dst)
{
    static const char hex[] = "0123456789abcdef";
    /* byte index (or -1 for a literal dash) for each of the 36 output
     * chars, two hex digits per byte - written out flat rather than
     * looped with per-group counters, the 8-4-4-4-12 layout doesn't
     * simplify into a clean loop without more state than it's worth. */
    static const i32 layout[36] = {
         0,  0,  1,  1,  2,  2,  3,  3, -1,
         4,  4,  5,  5, -1,
         6,  6,  7,  7, -1,
         8,  8,  9,  9, -1,
        10, 10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15
    };
    size_t i;

    for (i = 0; i < 36; i++) {
        i32 b = layout[i];
        if (b < 0) {
            dst[i] = '-';
        } else {
            u8 byte = id.bytes[b];
            /* even output positions for a given byte are its high
             * nibble, odd are its low nibble - matches the pairing
             * above (each byte appears twice in a row in `layout`).
             * high is already 0/1 - using it as a shift AMOUNT (4 or 0)
             * selects the nibble arithmetically instead of choosing
             * between two precomputed values. */
            i32 high = (i == 0 || layout[i - 1] != b);
            dst[i] = hex[(byte >> (4 * high)) & 0x0f];
        }
    }
    dst[36] = '\0';
}

#endif /* UUID_IMPLEMENTATION_ */
#endif /* UUID_IMPLEMENTATION */
