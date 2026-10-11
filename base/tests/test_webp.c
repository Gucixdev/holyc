#include "test_common.h"
#define WEBP_IMPLEMENTATION
#include "webp.h"
#include <string.h>

/* hand-constructs a minimal valid RIFF/WEBP/VP8L file's bytes directly,
 * per the exact bit layout webp_decode_info implements - no real sample
 * file needed (see file header for why: only the container + VP8L's own
 * 5-byte header is implemented, not pixel decode). */
static size_t build_webp_(u8 *buf, u32 width, u32 height, i32 alpha, u32 version)
{
    u32 packed = ((width - 1) & 0x3FFFu) | (((height - 1) & 0x3FFFu) << 14) |
                 ((u32)(alpha ? 1 : 0) << 28) | (version << 29);
    u32 vp8l_chunk_size = 5;
    u32 riff_size;
    size_t n = 0;

    memcpy(buf + n, "RIFF", 4); n += 4;
    n += 4; /* size field patched below */
    memcpy(buf + n, "WEBP", 4); n += 4;
    memcpy(buf + n, "VP8L", 4); n += 4;
    buf[n + 0] = (u8)(vp8l_chunk_size & 0xFF);
    buf[n + 1] = (u8)((vp8l_chunk_size >> 8) & 0xFF);
    buf[n + 2] = (u8)((vp8l_chunk_size >> 16) & 0xFF);
    buf[n + 3] = (u8)((vp8l_chunk_size >> 24) & 0xFF);
    n += 4;
    buf[n] = 0x2F; n += 1;
    buf[n + 0] = (u8)(packed & 0xFF);
    buf[n + 1] = (u8)((packed >> 8) & 0xFF);
    buf[n + 2] = (u8)((packed >> 16) & 0xFF);
    buf[n + 3] = (u8)((packed >> 24) & 0xFF);
    n += 4;

    riff_size = (u32)(n - 8);
    buf[4] = (u8)(riff_size & 0xFF);
    buf[5] = (u8)((riff_size >> 8) & 0xFF);
    buf[6] = (u8)((riff_size >> 16) & 0xFF);
    buf[7] = (u8)((riff_size >> 24) & 0xFF);

    return n;
}

int main(void)
{
    u8 buf[64];
    size_t n;
    Webp_Info info;
    i32 err;

    print_cstr("test_webp\n");

    n = build_webp_(buf, 100, 200, 1, 0);
    err = webp_decode_info(buf, n, &info);
    TEST_CHECK(err == WEBP_ESUCCESS, "decode_info: hand-built minimal VP8L webp parses successfully");
    TEST_CHECK(info.width == 100 && info.height == 200, "decode_info: width/height read back correctly (+1 applied)");
    TEST_CHECK(info.has_alpha == 1, "decode_info: alpha flag read correctly");
    TEST_CHECK(info.version == 0, "decode_info: version field read correctly");

    n = build_webp_(buf, 1, 1, 0, 0);
    err = webp_decode_info(buf, n, &info);
    TEST_CHECK(err == WEBP_ESUCCESS, "decode_info: minimum 1x1 dimensions round-trip");
    TEST_CHECK(info.width == 1 && info.height == 1, "decode_info: 1x1 dimensions correct");
    TEST_CHECK(info.has_alpha == 0, "decode_info: alpha-off flag read correctly");

    n = build_webp_(buf, 16384, 16384, 0, 0);
    err = webp_decode_info(buf, n, &info);
    TEST_CHECK(err == WEBP_ESUCCESS, "decode_info: maximum 14-bit dimensions (16384x16384) round-trip");
    TEST_CHECK(info.width == 16384 && info.height == 16384, "decode_info: max dimensions correct");

    {
        const u8 bad_magic[12] = "NOTAWEBPFIL";
        err = webp_decode_info(bad_magic, sizeof(bad_magic), &info);
        TEST_CHECK(err == WEBP_EBADRIFF, "decode_info: non-RIFF input is rejected cleanly");
    }

    {
        u8 no_vp8l[20];
        memcpy(no_vp8l, "RIFF", 4);
        memset(no_vp8l + 4, 0, 4);
        memcpy(no_vp8l + 8, "WEBP", 4);
        memcpy(no_vp8l + 12, "VP8 ", 4); /* lossy - deliberately unsupported */
        memset(no_vp8l + 16, 0, 4);
        err = webp_decode_info(no_vp8l, sizeof(no_vp8l), &info);
        TEST_CHECK(err == WEBP_ENOVP8L, "decode_info: a lossy VP8 (not VP8L) file is rejected, not misread");
    }

    {
        n = build_webp_(buf, 100, 200, 0, 0);
        buf[20] = 0x00; /* corrupt the VP8L signature byte (was 0x2F) */
        err = webp_decode_info(buf, n, &info);
        TEST_CHECK(err == WEBP_EBADSIG, "decode_info: a corrupted VP8L signature byte is rejected");
    }

    {
        n = build_webp_(buf, 100, 200, 0, 0);
        err = webp_decode_info(buf, n - 3, &info); /* truncate mid-chunk */
        TEST_CHECK(err == WEBP_ETRUNCATED, "decode_info: a truncated VP8L chunk is rejected, not read out of bounds");
    }

    print_flush();
    return test_failures_ != 0;
}
