#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#define BIT_IMPLEMENTATION
#define MORTON_IMPLEMENTATION
#include "base.h"
#include "curve.h"

int main(void)
{
    print_cstr("test_morton\n");

    /* 2D round trip across a dense sweep of small values plus edges */
    {
        u16 x, y;
        i32 all_ok = 1;
        for (x = 0; x < 200 && all_ok; x++) {
            for (y = 0; y < 200 && all_ok; y++) {
                u32 code = morton_encode_2d(x, y);
                u16 ox, oy;
                morton_decode_2d(code, &ox, &oy);
                if (ox != x || oy != y) all_ok = 0;
            }
        }
        TEST_CHECK(all_ok, "morton 2d: round trip holds for every (x,y) in [0,200)x[0,200)");
    }

    {
        u32 code;
        u16 ox, oy;

        code = morton_encode_2d(0, 0);
        TEST_CHECK(code == 0, "morton 2d: (0,0) encodes to 0");

        code = morton_encode_2d(0xffff, 0xffff);
        TEST_CHECK(code == 0xffffffffu, "morton 2d: (max,max) encodes to all-ones (uses the full 32 bits)");

        code = morton_encode_2d(1, 0);
        TEST_CHECK(code == 1, "morton 2d: x's bit 0 lands at code bit 0");

        code = morton_encode_2d(0, 1);
        TEST_CHECK(code == 2, "morton 2d: y's bit 0 lands at code bit 1 (interleaved right after x's)");

        morton_decode_2d(0x12345678u, &ox, &oy);
        code = morton_encode_2d(ox, oy);
        TEST_CHECK(code == 0x12345678u, "morton 2d: decode then re-encode an arbitrary code reproduces it");
    }

    /* the actual point of a Morton curve: axis-adjacent cells should
     * usually land close together in code-space, not arbitrarily far
     * apart the way a plain row-major index would put two vertically
     * adjacent cells a whole row-width apart. Checked as an average
     * over many sample points rather than every single pair, since the
     * property is statistical (locality "mostly" holds, not for
     * literally every neighbor pair - some worst-case jumps at
     * high-order bit boundaries are expected and fine). */
    {
        u16 x, y;
        u64 total_step = 0;
        i32 samples = 0;
        for (x = 1; x < 64; x += 3) {
            for (y = 1; y < 64; y += 3) {
                u32 c0 = morton_encode_2d(x, y);
                u32 c_right = morton_encode_2d((u16)(x + 1), y);
                u32 c_down = morton_encode_2d(x, (u16)(y + 1));
                u64 d_right = (c_right > c0) ? (c_right - c0) : (c0 - c_right);
                u64 d_down = (c_down > c0) ? (c_down - c0) : (c0 - c_down);
                total_step += d_right + d_down;
                samples += 2;
            }
        }
        /* a naive row-major index over the same 64-wide grid would put a
         * "move down one row" step 64 codes away; Morton's average step
         * for a single-cell move should be much smaller than that. */
        TEST_CHECK((total_step / (u64)samples) < 64,
                   "morton 2d: average code-distance for a single-cell step is well under a full row-width (locality holds)");
    }

    /* 3D round trip */
    {
        u32 x, y, z;
        i32 all_ok = 1;
        for (x = 0; x < 40 && all_ok; x += 3) {
            for (y = 0; y < 40 && all_ok; y += 3) {
                for (z = 0; z < 40 && all_ok; z += 3) {
                    u64 code = morton_encode_3d(x, y, z);
                    u32 ox, oy, oz;
                    morton_decode_3d(code, &ox, &oy, &oz);
                    if (ox != x || oy != y || oz != z) all_ok = 0;
                }
            }
        }
        TEST_CHECK(all_ok, "morton 3d: round trip holds across a sweep of (x,y,z) triples");
    }

    {
        u64 code;
        u32 ox, oy, oz;

        code = morton_encode_3d(0, 0, 0);
        TEST_CHECK(code == 0, "morton 3d: (0,0,0) encodes to 0");

        code = morton_encode_3d(0x1fffffu, 0x1fffffu, 0x1fffffu);
        TEST_CHECK(code == 0x7fffffffffffffffUL,
                   "morton 3d: (max,max,max) 21-bit inputs encode to all 63 usable bits set");

        code = morton_encode_3d(1, 0, 0);
        TEST_CHECK(code == 1, "morton 3d: x's bit 0 lands at code bit 0");
        code = morton_encode_3d(0, 1, 0);
        TEST_CHECK(code == 2, "morton 3d: y's bit 0 lands at code bit 1");
        code = morton_encode_3d(0, 0, 1);
        TEST_CHECK(code == 4, "morton 3d: z's bit 0 lands at code bit 2");

        /* only the low 21 bits of each axis are used per this file's own
         * documented contract - a coordinate with bit 21 set should
         * encode identically to the same value with that bit masked off. */
        code = morton_encode_3d((u32)1 << 21, 0, 0);
        TEST_CHECK(code == 0, "morton 3d: bit 21 and above of an input axis are outside the 21-bit contract and don't leak into the code");

        morton_decode_3d(0x123456789abcdefUL, &ox, &oy, &oz);
        code = morton_encode_3d(ox, oy, oz);
        TEST_CHECK(code == 0x123456789abcdefUL, "morton 3d: decode then re-encode an arbitrary code reproduces it");
    }

    print_flush();
    return test_failures_ != 0;
}
