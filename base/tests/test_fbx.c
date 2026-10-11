#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define FBX_IMPLEMENTATION
#include "arena_alloc.h"
#include "fbx.h"
#include <string.h>

/* A minimal FBX ASCII snippet defining a single triangle mesh (4 vertices,
 * one quad face + one triangle face, to exercise fan-triangulation) - not
 * a real FBX exporter's full output, just the two arrays this file reads. */
static const char *fbx_sample_ =
    "FBXHeaderExtension:  {\n"
    "    FBXVersion: 6100\n"
    "}\n"
    "Objects:  {\n"
    "    Model: \"Model::pCube1\", \"Mesh\" {\n"
    "        Version: 232\n"
    "        Vertices: 0,0,0,1,0,0,1,1,0,0,1,0\n"
    "        PolygonVertexIndex: 0,1,2,-4\n"
    "        GeometryVersion: 124\n"
    "    }\n"
    "}\n";

static const char *fbx_no_mesh_ =
    "Objects:  {\n"
    "    Model: \"Model::Camera1\", \"Camera\" {\n"
    "        Vertices: 0,0,0\n"
    "    }\n"
    "}\n";

static const char *fbx_bad_pvi_ =
    "Objects:  {\n"
    "    Model: \"Model::pCube1\", \"Mesh\" {\n"
    "        Vertices: 0,0,0,1,0,0,1,1,0\n"
    "        PolygonVertexIndex: 0,1,99999999\n"
    "    }\n"
    "}\n";

int main(void)
{
    Arena a = {0};
    Fbx_Mesh mesh;
    i32 err;

    print_cstr("test_fbx\n");

    err = fbx_decode(&a, fbx_sample_, strlen(fbx_sample_), &mesh);
    TEST_CHECK(err == FBX_ESUCCESS, "decode: minimal FBX ASCII mesh parses successfully");
    TEST_CHECK(mesh.position_count == 4, "decode: 4 vertices read");
    TEST_CHECK(mesh.triangle_count == 2, "decode: quad face (0,1,2,-4 => 0,1,2,3) fan-triangulates into 2 triangles");

    TEST_CHECK(mesh.positions[3 * 0 + 0] == 0.0f && mesh.positions[3 * 0 + 1] == 0.0f,
               "decode: vertex 0 position correct");
    TEST_CHECK(mesh.positions[3 * 2 + 0] == 1.0f && mesh.positions[3 * 2 + 1] == 1.0f,
               "decode: vertex 2 position correct");

    TEST_CHECK(mesh.triangle_indices[0] == 0 && mesh.triangle_indices[1] == 1 && mesh.triangle_indices[2] == 2,
               "decode: first triangle is corners 0,1,2 (fan origin + first two)");
    TEST_CHECK(mesh.triangle_indices[3] == 0 && mesh.triangle_indices[4] == 2 && mesh.triangle_indices[5] == 3,
               "decode: second triangle is corners 0,2,3 (un-complemented last index)");

    err = fbx_decode(&a, fbx_no_mesh_, strlen(fbx_no_mesh_), &mesh);
    TEST_CHECK(err == FBX_ENOMESH, "decode: a file with no \"Mesh\"-typed Model block is rejected cleanly");

    err = fbx_decode(&a, fbx_bad_pvi_, strlen(fbx_bad_pvi_), &mesh);
    TEST_CHECK(err == FBX_EMALFORMED, "decode: an out-of-range vertex index is rejected, not read out of bounds");

    print_flush();
    return test_failures_ != 0;
}
