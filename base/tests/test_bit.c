#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define BIT_IMPLEMENTATION
#include "base.h"

int main(void)
{
    print_cstr("test_bit\n");

    /* bitflag (moved from base_type.h - re-verified here, not just left
     * to test_base_type.c, since this is bit.h's actual home now) */
    {
        u32 flags = 0;
        FLAG_SET(flags, BIT(1) | BIT(3));
        TEST_CHECK(FLAG_HAS(flags, BIT(1)) && FLAG_HAS(flags, BIT(3)), "FLAG_SET/FLAG_HAS: bits 1,3 set");
        TEST_CHECK(!FLAG_HAS(flags, BIT(2)), "bit 2 not set");
        FLAG_CLEAR(flags, BIT(1));
        TEST_CHECK(!FLAG_HAS(flags, BIT(1)), "FLAG_CLEAR: bit 1 cleared");
        FLAG_TOGGLE(flags, BIT(3));
        TEST_CHECK(!FLAG_HAS(flags, BIT(3)), "FLAG_TOGGLE: bit 3 flipped off");
    }

    /* bitfield extract/insert */
    {
        u32 x = 0xdeadbeefu;
        TEST_CHECK(bit_extract32(x, 0, 8) == 0xef, "extract32: low byte");
        TEST_CHECK(bit_extract32(x, 8, 8) == 0xbe, "extract32: second byte");
        TEST_CHECK(bit_extract32(x, 16, 16) == 0xdead, "extract32: high 16 bits");

        {
            u32 y = bit_insert32(x, 0xff, 8, 8);
            TEST_CHECK(bit_extract32(y, 8, 8) == 0xff, "insert32: replaced byte reads back");
            TEST_CHECK(bit_extract32(y, 0, 8) == 0xef, "insert32: untouched byte still reads back");
            TEST_CHECK(bit_extract32(y, 16, 16) == 0xdead, "insert32: untouched high bits still read back");
        }

        {
            u64 x64 = 0x1122334455667788UL;
            TEST_CHECK(bit_extract64(x64, 0, 16) == 0x7788, "extract64: low 16 bits");
            TEST_CHECK(bit_extract64(x64, 48, 16) == 0x1122, "extract64: high 16 bits");
            {
                u64 y64 = bit_insert64(x64, 0xabcd, 16, 16);
                TEST_CHECK(bit_extract64(y64, 16, 16) == 0xabcd, "insert64: replaced field reads back");
                TEST_CHECK(bit_extract64(y64, 0, 16) == 0x7788, "insert64: untouched low bits still read back");
            }
        }
    }

    /* software PDEP/PEXT - pext(pdep(src, mask), mask) == src & low-bits(popcount(mask)) */
    {
        u32 mask = 0x0f0f0f0fu; /* 16 set bits, scattered */
        u32 src = 0x0000ffffu;  /* low 16 bits set - exactly popcount(mask) bits */
        u32 deposited = bit_pdep32(src, mask);
        TEST_CHECK(deposited == mask, "pdep: all-1s src deposited into every mask bit reproduces the mask");
        TEST_CHECK(bit_pext32(deposited, mask) == src, "pext: inverts pdep for the same mask");

        {
            u32 mask2 = 0xf0f0f0f0u;
            u32 src2 = 0xa5a5a5a5u;
            u32 dep2 = bit_pdep32(src2, mask2);
            TEST_CHECK((dep2 & ~mask2) == 0, "pdep: never sets a bit outside the mask");
            TEST_CHECK(bit_pext32(dep2, mask2) == (src2 & 0xffff), "pext(pdep(src,mask),mask) == low popcount(mask) bits of src");
        }
    }

    /* bitboard lsb tricks */
    {
        u64 b = 0x2c; /* bits 2,3,5 set */
        TEST_CHECK(bit_isolate_lsb64(b) == 0x04, "isolate_lsb64: lowest set bit alone");
        TEST_CHECK(bit_clear_lsb64(b) == 0x28, "clear_lsb64: lowest set bit cleared");
        TEST_CHECK(bit_lsb_index64(b) == 2, "lsb_index64: index of lowest set bit");
        TEST_CHECK(bit_lsb_index64(0) == -1, "lsb_index64: -1 for zero input");
        TEST_CHECK(bit_isolate_lsb64(0) == 0, "isolate_lsb64: 0 for zero input");
    }

    /* bit reverse */
    {
        TEST_CHECK(bit_reverse8(0x01) == 0x80, "reverse8: 0b00000001 -> 0b10000000");
        TEST_CHECK(bit_reverse8(0xf0) == 0x0f, "reverse8: 0b11110000 -> 0b00001111");
        TEST_CHECK(bit_reverse32(0x00000001u) == 0x80000000u, "reverse32: bit 0 -> bit 31");
        TEST_CHECK(bit_reverse32(0x12345678u) == 0x1e6a2c48u, "reverse32: arbitrary pattern matches hand-computed value");
    }

    /* align up/down, is_pow2 */
    {
        TEST_CHECK(align_up(0, 16) == 0, "align_up: already-aligned zero stays zero");
        TEST_CHECK(align_up(1, 16) == 16, "align_up: 1 rounds up to 16");
        TEST_CHECK(align_up(16, 16) == 16, "align_up: exact multiple stays put");
        TEST_CHECK(align_up(17, 16) == 32, "align_up: 17 rounds up to 32");
        TEST_CHECK(align_down(31, 16) == 16, "align_down: 31 rounds down to 16");
        TEST_CHECK(align_down(32, 16) == 32, "align_down: exact multiple stays put");

        TEST_CHECK(is_pow2(1) && is_pow2(2) && is_pow2(1024), "is_pow2: true for powers of two");
        TEST_CHECK(!is_pow2(0) && !is_pow2(3) && !is_pow2(1023), "is_pow2: false for zero and non-powers");
    }

    /* branchless select/min/max/clamp/abs */
    {
        TEST_CHECK(bit_select_i32(1, 10, 20) == 10, "select_i32: cond=1 picks a");
        TEST_CHECK(bit_select_i32(0, 10, 20) == 20, "select_i32: cond=0 picks b");

        TEST_CHECK(min_i32(3, 7) == 3 && min_i32(7, 3) == 3, "min_i32: symmetric");
        TEST_CHECK(max_i32(3, 7) == 7 && max_i32(7, 3) == 7, "max_i32: symmetric");
        TEST_CHECK(min_i32(-5, -1) == -5 && max_i32(-5, -1) == -1, "min_i32/max_i32: negative numbers");

        TEST_CHECK(clamp_i32(5, 0, 10) == 5, "clamp_i32: within range unchanged");
        TEST_CHECK(clamp_i32(-5, 0, 10) == 0, "clamp_i32: below range clamps to lo");
        TEST_CHECK(clamp_i32(15, 0, 10) == 10, "clamp_i32: above range clamps to hi");

        TEST_CHECK(abs_i32(5) == 5 && abs_i32(-5) == 5, "abs_i32: positive and negative");
        TEST_CHECK(abs_i32(0) == 0, "abs_i32: zero");
        TEST_CHECK(abs_i32(-2147483647 - 1) == -2147483647 - 1,
                   "abs_i32: INT32_MIN has no positive representation, returns itself (matches abs()/labs() UB-free convention here)");
    }

    /* endianness */
    {
        TEST_CHECK(byteswap16(0x1234) == 0x3412, "byteswap16");
        TEST_CHECK(byteswap32(0x12345678u) == 0x78563412u, "byteswap32");
        TEST_CHECK(byteswap64(0x1122334455667788UL) == 0x8877665544332211UL, "byteswap64");
        TEST_CHECK(byteswap16(byteswap16(0xbeef)) == 0xbeef, "byteswap16: involution (swap twice == identity)");
        TEST_CHECK(is_little_endian() == 0 || is_little_endian() == 1, "is_little_endian: returns a real 0/1, doesn't crash");
    }

    /* popcount/clz/ctz */
    {
        TEST_CHECK(popcount32(0) == 0, "popcount32: zero has zero bits");
        TEST_CHECK(popcount32(0xffffffffu) == 32, "popcount32: all-ones has 32 bits");
        TEST_CHECK(popcount32(0x0f0f0f0fu) == 16, "popcount32: known pattern");
        TEST_CHECK(popcount64(~(u64)0) == 64, "popcount64: all-ones has 64 bits");

        TEST_CHECK(clz32(1) == 31, "clz32: leading zeros before the lowest bit");
        TEST_CHECK(clz32(0x80000000u) == 0, "clz32: no leading zeros when the top bit is set");
        TEST_CHECK(ctz32(0x80000000u) == 31, "ctz32: trailing zeros before the top bit");
        TEST_CHECK(ctz32(1) == 0, "ctz32: no trailing zeros when bit 0 is set");
    }

    /* next power of two */
    {
        TEST_CHECK(next_pow2_32(0) == 1, "next_pow2_32: zero maps to 1");
        TEST_CHECK(next_pow2_32(1) == 1, "next_pow2_32: 1 is already a power of 2");
        TEST_CHECK(next_pow2_32(5) == 8, "next_pow2_32: 5 rounds up to 8");
        TEST_CHECK(next_pow2_32(1024) == 1024, "next_pow2_32: exact power of 2 stays put");
        TEST_CHECK(next_pow2_32(1025) == 2048, "next_pow2_32: just above a power of 2 rounds up to the next one");
    }

    /* sign extension */
    {
        TEST_CHECK(sign_extend32(0x0f, 4) == -1, "sign_extend32: 4-bit 0b1111 sign-extends to -1");
        TEST_CHECK(sign_extend32(0x07, 4) == 7, "sign_extend32: 4-bit 0b0111 (sign bit clear) stays positive");
        TEST_CHECK(sign_extend32(0x1ff, 9) == -1, "sign_extend32: 9-bit all-ones sign-extends to -1");
        TEST_CHECK(sign_extend32(0x0ff, 9) == 0xff, "sign_extend32: 9-bit with sign bit clear stays positive");
    }

    print_flush();
    return test_failures_ != 0;
}
