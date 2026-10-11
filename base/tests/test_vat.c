#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#define VAT_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#include "vat.h"

int main(void)
{
    Vat_Layout layout;
    layout.texture_width = 100;
    layout.texture_height = 10;

    print_cstr("test_vat\n");

    {
        vec2 uv = vat_uv(&layout, 0, 0);
        TEST_CHECK(uv.x == 0.005f, "vertex 0's u coordinate lands at half a texel in (0.5/100)");
        TEST_CHECK(uv.y == 0.05f, "frame 0's v coordinate lands at half a texel in (0.5/10)");
    }
    {
        vec2 uv = vat_uv(&layout, 99, 9);
        TEST_CHECK(uv.x > 0.99f && uv.x < 1.0f, "the last vertex column lands just under 1.0, never exactly at the texture edge");
        TEST_CHECK(uv.y > 0.94f && uv.y < 0.96f, "the last frame row lands at its own texel center too");
    }
    {
        vec2 a = vat_uv(&layout, 5, 3);
        vec2 b = vat_uv(&layout, 6, 3);
        TEST_CHECK(b.x > a.x, "consecutive vertex indices advance strictly along u");
        TEST_CHECK(a.y == b.y, "the same frame index gives the same v regardless of vertex index");
    }

    print_flush();
    return test_failures_ != 0;
}
