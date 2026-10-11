#include "test_common.h"
#define PROOF_IMPLEMENTATION
#include "verify.h"
#include <string.h>

int main(void)
{
    i32 r32;
    i64 r64;
    Proof_Error err;

    print_cstr("test_proof\n");

    /* division: normal cases still compute the right answer */
    err = verify_div_i32(10, 2, &r32);
    TEST_CHECK(err == PROOF_ESUCCESS && r32 == 5, "verify_div_i32: 10/2 = 5, success");

    err = verify_div_i64(100, 4, &r64);
    TEST_CHECK(err == PROOF_ESUCCESS && r64 == 25, "verify_div_i64: 100/4 = 25, success");

    /* division by zero: the case the spec explicitly asked for */
    err = verify_div_i32(7, 0, &r32);
    TEST_CHECK(err == PROOF_EDIV_BY_ZERO, "verify_div_i32: 7/0 caught, not executed");

    err = verify_div_i64(7, 0, &r64);
    TEST_CHECK(err == PROOF_EDIV_BY_ZERO, "verify_div_i64: 7/0 caught, not executed");

    /* division overflow: i32/i64 min divided by -1 has no representable result */
    err = verify_div_i32((i32)((u32)1 << 31), -1, &r32);
    TEST_CHECK(err == PROOF_EOVERFLOW, "verify_div_i32: i32_min/-1 caught as overflow");

    err = verify_div_i64((i64)((u64)1 << 63), -1, &r64);
    TEST_CHECK(err == PROOF_EOVERFLOW, "verify_div_i64: i64_min/-1 caught as overflow");

    /* add/sub/mul: normal cases still compute the right answer */
    err = verify_add_i32(2, 3, &r32);
    TEST_CHECK(err == PROOF_ESUCCESS && r32 == 5, "verify_add_i32: 2+3=5, success");

    err = verify_sub_i32(5, 3, &r32);
    TEST_CHECK(err == PROOF_ESUCCESS && r32 == 2, "verify_sub_i32: 5-3=2, success");

    err = verify_mul_i32(6, 7, &r32);
    TEST_CHECK(err == PROOF_ESUCCESS && r32 == 42, "verify_mul_i32: 6*7=42, success");

    /* add/sub/mul: real overflow is caught, not silently wrapped */
    err = verify_add_i32((i32)(((u32)1 << 31) - 1), -1, &r32); /* i32_max + -1 = i32_max-1, fits */
    TEST_CHECK(err == PROOF_ESUCCESS, "verify_add_i32: does not false-positive near the boundary");

    err = verify_add_i32(2000000000, 2000000000, &r32);
    TEST_CHECK(err == PROOF_EOVERFLOW, "verify_add_i32: 2e9+2e9 caught as overflow");

    err = verify_sub_i32((i32)((u32)1 << 31), 1, &r32);
    TEST_CHECK(err == PROOF_EOVERFLOW, "verify_sub_i32: i32_min-1 caught as overflow");

    err = verify_mul_i32(100000, 100000, &r32);
    TEST_CHECK(err == PROOF_EOVERFLOW, "verify_mul_i32: 100000*100000 caught as overflow");

    {
        i64 half_range = (i64)1 << 62; /* ~4.611e18, half of i64's ~9.223e18 max */
        err = verify_add_i64(half_range, half_range, &r64);
        TEST_CHECK(err == PROOF_EOVERFLOW, "verify_add_i64: 2^62 + 2^62 = 2^63 caught as overflow");
    }

    {
        i64 four_billion = (i64)1 << 32;
        err = verify_mul_i64(four_billion, four_billion, &r64);
        TEST_CHECK(err == PROOF_EOVERFLOW, "verify_mul_i64: 2^32 * 2^32 = 2^64 caught as overflow");
    }

    /* mathematical correctness: finite results pass, non-finite results are caught */
    TEST_CHECK(verify_finite_f32(1.5f) == PROOF_ESUCCESS, "verify_finite_f32: 1.5 is finite");
    TEST_CHECK(verify_finite_f32(0.0f) == PROOF_ESUCCESS, "verify_finite_f32: 0.0 is finite");
    TEST_CHECK(verify_finite_f32(-42.25f) == PROOF_ESUCCESS, "verify_finite_f32: -42.25 is finite");

    {
        f32 inf32 = 1.0f;
        f32 zero32 = 0.0f;
        f32 produced_inf = inf32 / zero32; /* IEEE-754 +Infinity, not a trap */
        f32 produced_nan = zero32 / zero32; /* IEEE-754 NaN, not a trap */
        TEST_CHECK(verify_finite_f32(produced_inf) == PROOF_EOVERFLOW, "verify_finite_f32: 1/0 = +Inf caught as non-finite");
        TEST_CHECK(verify_finite_f32(produced_nan) == PROOF_EOVERFLOW, "verify_finite_f32: 0/0 = NaN caught as non-finite");
    }

    TEST_CHECK(verify_finite_f64(1.5) == PROOF_ESUCCESS, "verify_finite_f64: 1.5 is finite");

    {
        f64 inf64 = 1.0;
        f64 zero64 = 0.0;
        f64 produced_inf = inf64 / zero64;
        f64 produced_nan = zero64 / zero64;
        TEST_CHECK(verify_finite_f64(produced_inf) == PROOF_EOVERFLOW, "verify_finite_f64: 1/0 = +Inf caught as non-finite");
        TEST_CHECK(verify_finite_f64(produced_nan) == PROOF_EOVERFLOW, "verify_finite_f64: 0/0 = NaN caught as non-finite");
    }

    TEST_CHECK(strcmp(verify_strerror(PROOF_ESUCCESS), "success") == 0, "verify_strerror: success message");
    TEST_CHECK(strcmp(verify_strerror(PROOF_EDIV_BY_ZERO), "division by zero") == 0, "verify_strerror: div-by-zero message");
    TEST_CHECK(strcmp(verify_strerror(PROOF_EOVERFLOW), "result overflows the target type") == 0, "verify_strerror: overflow message");

    print_flush();
    return test_failures_ != 0;
}
