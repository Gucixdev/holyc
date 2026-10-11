#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define STL_IMPLEMENTATION
#include "arena_alloc.h"
#include "stl.h"

#include <string.h>

static void write_u32le_(u8 *p, u32 v)
{
    p[0] = (u8)(v & 0xFF);
    p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF);
    p[3] = (u8)((v >> 24) & 0xFF);
}

static void write_f32le_(u8 *p, f32 v)
{
    u32 bits;
    memcpy(&bits, &v, sizeof(bits));
    write_u32le_(p, bits);
}

/* one triangle, binary STL: 80-byte header + u32 count=1 + 50-byte record */
static size_t make_binary_single_triangle_(u8 *buf)
{
    u8 *p = buf;
    memset(p, 0, 80);
    p += 80;
    write_u32le_(p, 1);
    p += 4;

    write_f32le_(p + 0, 0.0f);
    write_f32le_(p + 4, 0.0f);
    write_f32le_(p + 8, 1.0f); /* normal */

    write_f32le_(p + 12, 0.0f); write_f32le_(p + 16, 0.0f); write_f32le_(p + 20, 0.0f); /* v0 */
    write_f32le_(p + 24, 1.0f); write_f32le_(p + 28, 0.0f); write_f32le_(p + 32, 0.0f); /* v1 */
    write_f32le_(p + 36, 0.0f); write_f32le_(p + 40, 1.0f); write_f32le_(p + 44, 0.0f); /* v2 */

    p[48] = 0; p[49] = 0; /* attribute byte count */

    return 80 + 4 + 50;
}

/* a binary file whose 80-byte header text happens to start with "solid" -
 * the exact misdetection hazard the file header describes. */
static size_t make_binary_with_solid_prefix_(u8 *buf)
{
    size_t n = make_binary_single_triangle_(buf);
    memcpy(buf, "solid exported_by_some_tool", 27);
    return n;
}

static const char *ascii_triangle_ =
    "solid test\n"
    "facet normal 0.0 0.0 1.0\n"
    "  outer loop\n"
    "    vertex 0.0 0.0 0.0\n"
    "    vertex 1.0 0.0 0.0\n"
    "    vertex 0.0 1.0 0.0\n"
    "  endloop\n"
    "endfacet\n"
    "endsolid test\n";

int main(void)
{
    Arena a = {0};
    u8 buf[512];
    size_t n;
    i32 err;
    Stl_Mesh mesh;

    print_cstr("test_stl\n");

    /* --- binary --- */
    n = make_binary_single_triangle_(buf);
    err = stl_decode(&a, buf, n, &mesh);
    TEST_CHECK(err == STL_ESUCCESS, "binary: decode succeeds on a single-triangle file");
    TEST_CHECK(mesh.triangle_count == 1, "binary: triangle_count is 1");
    TEST_CHECK(mesh.normals[2] == 1.0f, "binary: normal z component correct");
    TEST_CHECK(mesh.positions[3] == 1.0f && mesh.positions[4] == 0.0f, "binary: second vertex x/y correct");

    /* --- binary with a header that starts with "solid" (misdetection hazard) --- */
    n = make_binary_with_solid_prefix_(buf);
    err = stl_decode(&a, buf, n, &mesh);
    TEST_CHECK(err == STL_ESUCCESS, "binary with 'solid'-prefixed header: still decoded as binary, not misdetected as ASCII");
    TEST_CHECK(mesh.triangle_count == 1, "binary with 'solid'-prefixed header: triangle_count still correct");

    /* --- ASCII --- */
    err = stl_decode(&a, (const u8 *)ascii_triangle_, strlen(ascii_triangle_), &mesh);
    TEST_CHECK(err == STL_ESUCCESS, "ascii: decode succeeds");
    TEST_CHECK(mesh.triangle_count == 1, "ascii: triangle_count is 1");
    TEST_CHECK(mesh.positions[3] == 1.0f, "ascii: second vertex x correct");
    TEST_CHECK(mesh.normals[2] == 1.0f, "ascii: normal z correct");

    /* --- malformed / truncated input rejected cleanly (not crashed on,
     * not read out of bounds) - a truncated binary record no longer
     * matches its own claimed size, so it's correctly treated as "not
     * binary" and falls to the ASCII path, which then fails there
     * instead: EMALFORMED, not EBADINPUT. Either way the important
     * property is "some error, no crash", so these check for failure
     * generically rather than pinning one specific error code. --- */
    n = make_binary_single_triangle_(buf);
    err = stl_decode(&a, buf, n - 10, &mesh);
    TEST_CHECK(err != STL_ESUCCESS, "truncated binary record is rejected, not read out of bounds");

    {
        const char *bad = "solid test\nfacet normal 0 0 1\n  outer loop\n    vertex 0 0\n";
        err = stl_decode(&a, (const u8 *)bad, strlen(bad), &mesh);
        TEST_CHECK(err == STL_EMALFORMED, "ascii: incomplete vertex line is rejected cleanly");
    }

    {
        const u8 tiny[4] = {0, 0, 0, 0};
        err = stl_decode(&a, tiny, sizeof(tiny), &mesh);
        TEST_CHECK(err == STL_EMALFORMED, "input far too short for either format is rejected cleanly");
    }

    {
        err = stl_decode(&a, (const u8 *)"", 0, &mesh);
        TEST_CHECK(err == STL_EBADINPUT, "zero-length input is rejected cleanly");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
