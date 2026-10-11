#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#include "core/base.h"

/* a real, file-scope static_assert - if this ever failed to compile,
 * that failure IS the test (a runtime TEST_CHECK can't exercise a
 * compile-time-only facility). */
HOLYC_STATIC_ASSERT(sizeof(i32) == 4, i32_must_be_4_bytes);
HOLYC_STATIC_ASSERT(sizeof(i64) == 8, i64_must_be_8_bytes);

/* two static_asserts on the SAME line (separated by ';') exercises
 * that __LINE__-based uniqueness actually works even then - each
 * still needs a name that doesn't collide with the other. */
HOLYC_STATIC_ASSERT(1 == 1, first_on_this_line); HOLYC_STATIC_ASSERT(2 == 2, second_on_this_line);

typedef struct {
    char c;
    f64 d;
} Align_Test_Struct;

HOLYC_ALIGNAS(16) static u8 aligned_buffer_[64];

int main(void)
{
    fp32 a, b, prod, quot;
    u32 buf[4];
    a32 view;
    size_t i;
    u32 flags;

    print_cstr("test_base_type\n");

    a = fp32_from_f32(3.5f);
    b = fp32_from_f32(2.0f);
    prod = fp32_mul(a, b);
    quot = fp32_div(a, b);
    TEST_CHECK(fp32_to_f32(prod) == 7.0f, "fp32_mul 3.5*2.0 == 7.0");
    TEST_CHECK(fp32_to_f32(quot) == 1.75f, "fp32_div 3.5/2.0 == 1.75");

    for (i = 0; i < 4; i++) buf[i] = (u32)(i * 10);
    view = a32_make(buf, 4);
    TEST_CHECK(view.count == 4, "a32_make count == 4");
    TEST_CHECK(view.data[2] == 20, "a32 view sees underlying buffer (data[2] == 20)");

    flags = 0;
    FLAG_SET(flags, BIT(1) | BIT(3));
    TEST_CHECK(FLAG_HAS(flags, BIT(1)), "FLAG_SET/FLAG_HAS: bit 1 set");
    TEST_CHECK(FLAG_HAS(flags, BIT(3)), "FLAG_SET/FLAG_HAS: bit 3 set");
    TEST_CHECK(!FLAG_HAS(flags, BIT(2)), "bit 2 not set");
    FLAG_CLEAR(flags, BIT(1));
    TEST_CHECK(!FLAG_HAS(flags, BIT(1)), "FLAG_CLEAR: bit 1 cleared");
    FLAG_TOGGLE(flags, BIT(3));
    TEST_CHECK(!FLAG_HAS(flags, BIT(3)), "FLAG_TOGGLE: bit 3 flipped off");

    TEST_CHECK(HOLYC_ALIGNOF(u8) == 1, "alignof(u8) is 1, the trivial case");
    TEST_CHECK(HOLYC_ALIGNOF(i32) == 4, "alignof(i32) matches its own known natural alignment (4)");
    TEST_CHECK(HOLYC_ALIGNOF(f64) == 8, "alignof(f64) matches its own known natural alignment (8) on this platform");
    TEST_CHECK(HOLYC_ALIGNOF(Align_Test_Struct) == HOLYC_ALIGNOF(f64), "a struct's alignment is (at least) its strictest member's - here that's the f64");

    TEST_CHECK(((size_t)aligned_buffer_ % 16) == 0, "HOLYC_ALIGNAS(16) actually produces a 16-byte-aligned address at runtime, not just a no-op attribute");

    {
        u8 buf[16];
        for (i = 0; i < 16; i++) buf[i] = 0xFF;
        memset_explicit(buf, 0, 16);
        {
            i32 all_zero = 1;
            for (i = 0; i < 16; i++) if (buf[i] != 0) all_zero = 0;
            TEST_CHECK(all_zero, "memset_explicit actually zeroes the buffer, same observable result a normal memset would give");
        }

        memset_explicit(buf, 0xAB, 16);
        {
            i32 all_ab = 1;
            for (i = 0; i < 16; i++) if (buf[i] != 0xAB) all_ab = 0;
            TEST_CHECK(all_ab, "memset_explicit with a non-zero value fills every byte with that value's low 8 bits");
        }
    }

    print_flush();
    return test_failures_ != 0;
}
