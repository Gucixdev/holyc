#define PRINT_IMPLEMENTATION
#define BASE_TYPE_IMPLEMENTATION
#define BIT_IMPLEMENTATION
#define MORTON_IMPLEMENTATION
#include "util/print.h"
#include "types/base.h"
#include "math/curve.h"
#include "bench_util.h"

/* morton.h's real encode uses the "spread bits by magic-number AND/shift"
 * trick - O(log n) shifts per axis. The comparison baseline here is the
 * obvious alternative anyone reaches for first: walk each axis one bit at
 * a time and OR it into its interleaved slot - correct, but a 16-iteration
 * loop (times 2 axes) per single 2D encode instead of 4 shift/AND pairs. */
static u32 morton_encode_2d_naive_(u16 x, u16 y)
{
    u32 code = 0;
    i32 i;
    for (i = 0; i < 16; i++) {
        code |= (u32)((x >> i) & 1) << (2 * i);
        code |= (u32)((y >> i) & 1) << (2 * i + 1);
    }
    return code;
}

int main(void)
{
    i64 n = 20000000;
    i64 i;
    u32 acc_fast = 0, acc_naive = 0;
    f64 t0, t1, t_fast, t_naive;

    print_cstr("bench_morton: 2D encode, magic-number bit-spread vs naive per-bit loop\n");
    print_cstr("naive = OR each of 32 source bits into its interleaved slot one at a time\n");
    print_cstr("fast  = the shipped morton_encode_2d (4 shift/AND pairs per axis)\n\n");

    /* accumulate the result into acc_* and print it, so the compiler can't
     * prove the loop's output is unused and fold the whole thing away */

    t0 = bench_now();
    for (i = 0; i < n; i++) {
        acc_naive ^= morton_encode_2d_naive_((u16)i, (u16)(i * 2654435761u));
    }
    t1 = bench_now();
    t_naive = t1 - t0;

    t0 = bench_now();
    for (i = 0; i < n; i++) {
        acc_fast ^= morton_encode_2d((u16)i, (u16)(i * 2654435761u));
    }
    t1 = bench_now();
    t_fast = t1 - t0;

    print_cstr("n="); print_u64((u64)n);
    print_cstr("  naive="); print_u64((u64)(t_naive * 1e9 / (f64)n)); print_cstr(" ns/op");
    print_cstr("  fast="); print_u64((u64)(t_fast * 1e9 / (f64)n)); print_cstr(" ns/op");
    print_cstr("  speedup="); print_u64(bench_speedup_pct(t_naive, t_fast)); print_cstr("% (100 = same)");
    print_cstr("  [checksum "); print_u64((u64)(acc_naive == acc_fast)); print_cstr(" - both paths agree]\n");

    print_flush();
    return 0;
}
