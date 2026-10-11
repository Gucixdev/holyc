#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define SVG_IMPLEMENTATION
#include "arena_alloc.h"
#include "svg.h"
#include <string.h>

static const char *svg_sample_ =
    "<?xml version=\"1.0\"?>\n"
    "<svg width=\"100\" height=\"200\" viewBox=\"0 0 100 200\">\n"
    "  <!-- a comment -->\n"
    "  <rect x=\"1\" y=\"2\" width=\"10\" height=\"20\" />\n"
    "  <circle cx=\"5\" cy=\"6\" r=\"3\"/>\n"
    "  <line x1=\"0\" y1=\"0\" x2=\"10\" y2=\"10\" />\n"
    "  <polyline points=\"0,0 1,1 2,4\" />\n"
    "  <path d=\"M0,0 L10,0 C10,10 20,10 20,20\" />\n"
    "</svg>\n";

static const char *svg_no_root_ = "<foo><bar/></foo>";
static const char *svg_bad_path_ = "<svg><path d=\"Q1,1 2,2\"/></svg>"; /* Q (quadratic) unsupported */

int main(void)
{
    Arena a = {0};
    Svg_Doc doc;
    i32 err;

    print_cstr("test_svg\n");

    err = svg_decode(&a, svg_sample_, strlen(svg_sample_), &doc);
    TEST_CHECK(err == SVG_ESUCCESS, "decode: sample SVG parses successfully");
    TEST_CHECK(doc.width == 100.0f && doc.height == 200.0f, "decode: root width/height read correctly");
    TEST_CHECK(doc.vb_min_x == 0.0f && doc.vb_width == 100.0f && doc.vb_height == 200.0f,
               "decode: viewBox parsed correctly");
    TEST_CHECK(doc.prim_count == 5, "decode: all 5 primitives found (comment correctly skipped)");

    TEST_CHECK(doc.prims[0].kind == SVG_PRIM_RECT, "decode: primitive 0 is a rect");
    TEST_CHECK(doc.prims[0].as.rect.x == 1.0f && doc.prims[0].as.rect.w == 10.0f, "decode: rect attributes correct");

    TEST_CHECK(doc.prims[1].kind == SVG_PRIM_CIRCLE, "decode: primitive 1 is a circle");
    TEST_CHECK(doc.prims[1].as.circle.r == 3.0f, "decode: circle radius correct");

    TEST_CHECK(doc.prims[2].kind == SVG_PRIM_LINE, "decode: primitive 2 is a line");
    TEST_CHECK(doc.prims[2].as.line.x2 == 10.0f && doc.prims[2].as.line.y2 == 10.0f, "decode: line endpoint correct");

    TEST_CHECK(doc.prims[3].kind == SVG_PRIM_POLYLINE, "decode: primitive 3 is a polyline");
    TEST_CHECK(doc.prims[3].as.polyline.point_count == 3, "decode: polyline has 3 points");
    TEST_CHECK(doc.prims[3].as.polyline.points[4] == 2.0f && doc.prims[3].as.polyline.points[5] == 4.0f,
               "decode: polyline's third point (2,4) correct");

    TEST_CHECK(doc.prims[4].kind == SVG_PRIM_PATH, "decode: primitive 4 is a path");
    TEST_CHECK(doc.prims[4].as.path.cmd_count == 3, "decode: path has 3 commands (M, L, C)");
    TEST_CHECK(doc.prims[4].as.path.cmds[0].kind == SVG_PATH_MOVETO, "decode: path command 0 is moveto");
    TEST_CHECK(doc.prims[4].as.path.cmds[1].kind == SVG_PATH_LINETO &&
               doc.prims[4].as.path.cmds[1].x == 10.0f, "decode: path command 1 is lineto to (10,0)");
    TEST_CHECK(doc.prims[4].as.path.cmds[2].kind == SVG_PATH_CUBICTO &&
               doc.prims[4].as.path.cmds[2].x2 == 20.0f && doc.prims[4].as.path.cmds[2].y == 20.0f,
               "decode: path command 2 is a cubic bezier with correct endpoint/control point");

    err = svg_decode(&a, svg_no_root_, strlen(svg_no_root_), &doc);
    TEST_CHECK(err == SVG_ENOSVG, "decode: a document with no <svg> root is rejected cleanly");

    err = svg_decode(&a, svg_bad_path_, strlen(svg_bad_path_), &doc);
    TEST_CHECK(err == SVG_EMALFORMED, "decode: an unsupported path command (Q) is rejected, not silently mishandled");

    print_flush();
    return test_failures_ != 0;
}
