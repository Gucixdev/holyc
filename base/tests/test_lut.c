#include "test_common.h"
#define BIT_IMPLEMENTATION
#define LUT_IMPLEMENTATION
#include "core/base.h"
#include "data/lut.h"

static f32 square_gen_(i32 index, void *ctx)
{
    (void)ctx;
    return (f32)(index * index);
}

static u8 double_clamped_gen_(i32 index, void *ctx)
{
    (void)ctx;
    return (u8)(index * 2);
}

int main(void)
{
    print_cstr("test_lut\n");

    {
        f32 storage[10];
        Lut_F32 lut;
        lut_f32_build(&lut, storage, 10, square_gen_, 0);

        TEST_CHECK(lut.count == 10, "lut_f32_build: count stored correctly");
        TEST_CHECK(lut_f32_get(&lut, 0) == 0.0f, "lut_f32_get: index 0 -> 0*0");
        TEST_CHECK(lut_f32_get(&lut, 3) == 9.0f, "lut_f32_get: index 3 -> 3*3");
        TEST_CHECK(lut_f32_get(&lut, 9) == 81.0f, "lut_f32_get: last valid index -> 9*9");

        TEST_CHECK(lut_f32_get(&lut, -5) == 0.0f, "lut_f32_get: negative index clamps to entry 0");
        TEST_CHECK(lut_f32_get(&lut, 100) == 81.0f, "lut_f32_get: past-the-end index clamps to the last entry");
    }

    {
        u8 storage[5];
        Lut_U8 lut;
        lut_u8_build(&lut, storage, 5, double_clamped_gen_, 0);

        TEST_CHECK(lut.count == 5, "lut_u8_build: count stored correctly");
        TEST_CHECK(lut_u8_get(&lut, 0) == 0, "lut_u8_get: index 0");
        TEST_CHECK(lut_u8_get(&lut, 4) == 8, "lut_u8_get: last valid index -> 4*2");
        TEST_CHECK(lut_u8_get(&lut, -1) == 0, "lut_u8_get: negative index clamps to entry 0");
        TEST_CHECK(lut_u8_get(&lut, 50) == 8, "lut_u8_get: past-the-end index clamps to the last entry");
    }

    print_flush();
    return test_failures_ != 0;
}
