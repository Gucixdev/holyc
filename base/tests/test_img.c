#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define DEFLATE_IMPLEMENTATION
#define PNG_DEC_IMPLEMENTATION
#define JPEG_DEC_IMPLEMENTATION
#define IMG_IMPLEMENTATION
#include "arena_alloc.h"
#include "img.h"
#include <string.h>

/* writes a minimal, valid uncompressed 24-bit BI_RGB BMP (BITMAPFILEHEADER
 * + BITMAPINFOHEADER + row-padded pixel data) for the given width/height,
 * bottom-up (positive height) or top-down (negative height) row order.
 * pixels is width*height RGB triples, row-major, top-to-bottom in SOURCE
 * order (this helper handles the on-disk row-order flip itself). */
static size_t make_bmp_(u8 *buf, i32 width, i32 height_signed, const u8 *rgb)
{
    i32 height = height_signed < 0 ? -height_signed : height_signed;
    i32 flip = height_signed < 0; /* negative height on disk = already top-down */
    u32 row_bytes_src = (u32)width * 3u;
    u32 row_bytes_padded = (row_bytes_src + 3u) & ~3u;
    u32 pixel_data_size = row_bytes_padded * (u32)height;
    u32 file_size = 14u + 40u + pixel_data_size;
    u32 data_offset = 14u + 40u;
    i32 y;

    memset(buf, 0, file_size);

    buf[0] = 'B'; buf[1] = 'M';
    buf[2] = (u8)file_size; buf[3] = (u8)(file_size >> 8);
    buf[4] = (u8)(file_size >> 16); buf[5] = (u8)(file_size >> 24);
    buf[10] = (u8)data_offset;

    buf[14] = 40; /* BITMAPINFOHEADER size */
    buf[18] = (u8)width; buf[19] = (u8)(width >> 8);
    buf[20] = (u8)(width >> 16); buf[21] = (u8)(width >> 24);
    buf[22] = (u8)height_signed; buf[23] = (u8)(height_signed >> 8);
    buf[24] = (u8)(height_signed >> 16); buf[25] = (u8)(height_signed >> 24);
    buf[28] = 24; /* bpp */
    /* compression (offset 30) left as 0 = BI_RGB */

    for (y = 0; y < height; y++) {
        /* row y of the SOURCE image (top-to-bottom) goes at this disk row */
        i32 disk_row = flip ? y : (height - 1 - y);
        u8 *dst = buf + data_offset + (size_t)disk_row * row_bytes_padded;
        i32 x;
        for (x = 0; x < width; x++) {
            const u8 *src = rgb + ((size_t)y * (size_t)width + (size_t)x) * 3;
            dst[x * 3 + 0] = src[2]; /* R,G,B -> B,G,R on disk */
            dst[x * 3 + 1] = src[1];
            dst[x * 3 + 2] = src[0];
        }
    }

    return file_size;
}

int main(void)
{
    Arena a = {0};

    print_cstr("test_img\n");

    /* format detection */
    {
        u8 png_bytes[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        u8 jpeg_bytes[2] = { 0xFF, 0xD8 };
        u8 bmp_bytes[2] = { 'B', 'M' };
        u8 garbage[4] = { 'x', 'x', 'x', 'x' };

        TEST_CHECK(img_detect_format(png_bytes, sizeof(png_bytes)) == IMG_FORMAT_PNG,
                   "detect: PNG signature recognized");
        TEST_CHECK(img_detect_format(jpeg_bytes, sizeof(jpeg_bytes)) == IMG_FORMAT_JPEG,
                   "detect: JPEG SOI marker recognized");
        TEST_CHECK(img_detect_format(bmp_bytes, sizeof(bmp_bytes)) == IMG_FORMAT_BMP,
                   "detect: BMP 'BM' magic recognized");
        TEST_CHECK(img_detect_format(garbage, sizeof(garbage)) == IMG_FORMAT_UNKNOWN,
                   "detect: unrecognized bytes report IMG_FORMAT_UNKNOWN");
    }

    /* BMP decode: bottom-up (the common on-disk case) */
    {
        u8 rgb[2 * 2 * 3] = {
            255, 0, 0,    0, 255, 0,
            0, 0, 255,    255, 255, 255
        };
        u8 bmp[14 + 40 + 16];
        size_t bmp_size;
        Img_Result out;
        i32 ok;

        bmp_size = make_bmp_(bmp, 2, 2, rgb);
        ok = img_decode(&a, bmp, bmp_size, &out);
        TEST_CHECK(ok == 1, "BMP decode: bottom-up 24-bit image succeeds");
        TEST_CHECK(out.width == 2 && out.height == 2 && out.channels == 4,
                   "BMP decode: dimensions and channel count correct");
        TEST_CHECK(out.pixels[0] == 255 && out.pixels[1] == 0 && out.pixels[2] == 0 && out.pixels[3] == 255,
                   "BMP decode: top-left pixel (red) decoded correctly, alpha forced opaque");
        TEST_CHECK(out.pixels[4] == 0 && out.pixels[5] == 255 && out.pixels[6] == 0,
                   "BMP decode: second pixel (green) decoded correctly");
    }

    /* BMP decode: top-down (negative height) row order */
    {
        u8 rgb[2 * 1 * 3] = { 10, 20, 30,   40, 50, 60 };
        u8 bmp[14 + 40 + 8];
        size_t bmp_size;
        Img_Result out;
        i32 ok;

        bmp_size = make_bmp_(bmp, 2, -1, rgb);
        ok = img_decode(&a, bmp, bmp_size, &out);
        TEST_CHECK(ok == 1, "BMP decode: top-down (negative height) image succeeds");
        TEST_CHECK(out.pixels[0] == 10 && out.pixels[1] == 20 && out.pixels[2] == 30,
                   "BMP decode: top-down row order preserved correctly");
    }

    /* dispatch reaches the right decoder even when the payload itself is bad -
       proves img_decode() actually routes by format rather than always
       taking one path (png_dec.h/jpeg_dec.h's own decode correctness is
       covered by their own dedicated tests, not re-tested here). */
    {
        u8 png_garbage[16] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0, 0, 0, 0, 0 };
        u8 jpeg_garbage[4] = { 0xFF, 0xD8, 0, 0 };
        Img_Result out;
        i32 ok;

        ok = img_decode(&a, png_garbage, sizeof(png_garbage), &out);
        TEST_CHECK(ok == 0, "dispatch: PNG-signed garbage reaches png_decode and fails cleanly, not crashed on");

        ok = img_decode(&a, jpeg_garbage, sizeof(jpeg_garbage), &out);
        TEST_CHECK(ok == 0, "dispatch: JPEG-signed garbage reaches jpeg_decode and fails cleanly, not crashed on");
    }

    /* malformed/unsupported BMP rejected cleanly */
    {
        u8 rgb[1 * 1 * 3] = { 1, 2, 3 };
        u8 bmp[14 + 40 + 4];
        size_t bmp_size;
        Img_Result out;
        i32 ok;

        bmp_size = make_bmp_(bmp, 1, 1, rgb);
        bmp[30] = 1; /* set compression to BI_RLE8 (unsupported) */
        ok = img_decode(&a, bmp, bmp_size, &out);
        TEST_CHECK(ok == 0, "BMP decode: compressed (non-BI_RGB) BMP is rejected, not crashed on");

        ok = img_decode(&a, bmp, 10, &out); /* truncated, shorter than a full header */
        TEST_CHECK(ok == 0, "BMP decode: truncated input is rejected, not crashed on");

        ok = img_decode(&a, (const u8 *)"not an image", 12, &out);
        TEST_CHECK(ok == 0, "img_decode: unrecognized magic bytes rejected cleanly");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
