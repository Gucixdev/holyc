#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define OBJ_IMPLEMENTATION
#include "arena_alloc.h"
#include "obj.h"
#include <string.h>

int main(void)
{
    Arena a = {0};

    print_cstr("test_obj\n");

    /* a single triangle, plain "v"/"f" only */
    {
        const char *src =
            "# a comment, should be skipped\n"
            "v 0.0 0.0 0.0\n"
            "v 1.0 0.0 0.0\n"
            "v 0.0 1.0 0.0\n"
            "f 1 2 3\n";
        Obj_Mesh mesh;
        i32 err;

        err = obj_decode(&a, src, strlen(src), &mesh);
        TEST_CHECK(err == OBJ_ESUCCESS, "decode: single triangle succeeds");
        TEST_CHECK(mesh.position_count == 3, "decode: 3 positions parsed");
        TEST_CHECK(mesh.triangle_count == 1, "decode: 1 triangle parsed");
        TEST_CHECK(mesh.triangle_vertices[0].pos == 0 &&
                   mesh.triangle_vertices[1].pos == 1 &&
                   mesh.triangle_vertices[2].pos == 2,
                   "decode: 1-based face indices converted to 0-based correctly");
        TEST_CHECK(mesh.positions[3] == 1.0f && mesh.positions[4] == 0.0f,
                   "decode: second vertex's xy coordinates are correct");
    }

    /* a quad face (fan-triangulated into 2 triangles) with vt/vn, all 4 index forms */
    {
        const char *src =
            "v 0 0 0\n"
            "v 1 0 0\n"
            "v 1 1 0\n"
            "v 0 1 0\n"
            "vt 0 0\n"
            "vt 1 0\n"
            "vt 1 1\n"
            "vt 0 1\n"
            "vn 0 0 1\n"
            "f 1/1/1 2/2/1 3/3/1 4/4/1\n";
        Obj_Mesh mesh;
        i32 err;

        err = obj_decode(&a, src, strlen(src), &mesh);
        TEST_CHECK(err == OBJ_ESUCCESS, "decode: quad face succeeds");
        TEST_CHECK(mesh.triangle_count == 2, "decode: quad fan-triangulated into 2 triangles");
        TEST_CHECK(mesh.texcoord_count == 4, "decode: 4 texcoords parsed");
        TEST_CHECK(mesh.normal_count == 1, "decode: 1 normal parsed");
        TEST_CHECK(mesh.triangle_vertices[0].tex == 0 && mesh.triangle_vertices[0].nrm == 0,
                   "decode: v/vt/vn index form parsed correctly");
        TEST_CHECK(mesh.triangle_vertices[3].pos == 0 && mesh.triangle_vertices[4].pos == 2,
                   "decode: second triangle of the fan reuses vertex 0 and vertex 2 correctly");
    }

    /* v//vn form (texcoord absent) */
    {
        const char *src =
            "v 0 0 0\n"
            "v 1 0 0\n"
            "v 0 1 0\n"
            "vn 0 0 1\n"
            "f 1//1 2//1 3//1\n";
        Obj_Mesh mesh;
        i32 err;

        err = obj_decode(&a, src, strlen(src), &mesh);
        TEST_CHECK(err == OBJ_ESUCCESS, "decode: v//vn face form succeeds");
        TEST_CHECK(mesh.triangle_vertices[0].tex == -1, "decode: absent texcoord index is -1");
        TEST_CHECK(mesh.triangle_vertices[0].nrm == 0, "decode: normal index still parsed correctly");
    }

    /* malformed input rejected cleanly */
    {
        const char *src1 = "v not a number 0.0 0.0\n";
        const char *src2 = "v 0 0 0\nv 1 0 0\nf 1 2\n"; /* face with only 2 verts */
        Obj_Mesh mesh;
        i32 err;

        err = obj_decode(&a, src1, strlen(src1), &mesh);
        TEST_CHECK(err == OBJ_EMALFORMED, "decode: unparseable vertex line is rejected, not crashed on");

        err = obj_decode(&a, src2, strlen(src2), &mesh);
        TEST_CHECK(err == OBJ_EMALFORMED, "decode: a face with fewer than 3 vertices is rejected");
    }

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
