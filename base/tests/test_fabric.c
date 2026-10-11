#include "test_common.h"
#include "fabric.h"

typedef struct { i32 kind; f32 value; } Shape;

enum { SHAPE_CIRCLE = 1, SHAPE_SQUARE = 2 };

static Shape g_circle_, g_square_;

static Shape *make_circle_(void *ctx) { g_circle_.kind = SHAPE_CIRCLE; g_circle_.value = *(f32 *)ctx; return &g_circle_; }
static Shape *make_square_(void *ctx) { g_square_.kind = SHAPE_SQUARE; g_square_.value = *(f32 *)ctx; return &g_square_; }

#define SHAPE_FACTORY_LIST(X)     \
    X(SHAPE_CIRCLE, make_circle_) \
    X(SHAPE_SQUARE, make_square_)

FABRIC_DEFINE(Shape, shape_factory, SHAPE_FACTORY_LIST)

int main(void)
{
    f32 radius = 2.5f;
    f32 side = 4.0f;
    Shape *circle, *square, *bad;

    print_cstr("test_fabric\n");

    circle = shape_factory_create(SHAPE_CIRCLE, &radius);
    TEST_CHECK(circle != 0, "factory dispatches SHAPE_CIRCLE to a non-NULL shape");
    TEST_CHECK(circle->kind == SHAPE_CIRCLE, "dispatched shape has the right kind");
    TEST_CHECK(circle->value == 2.5f, "ctx (radius) reached the constructor unchanged");

    square = shape_factory_create(SHAPE_SQUARE, &side);
    TEST_CHECK(square != 0, "factory dispatches SHAPE_SQUARE to a non-NULL shape");
    TEST_CHECK(square->kind == SHAPE_SQUARE, "dispatched shape has the right kind");
    TEST_CHECK(square->value == 4.0f, "ctx (side) reached the constructor unchanged");

    bad = shape_factory_create(999, &side);
    TEST_CHECK(bad == 0, "an unrecognized id returns NULL, not garbage or a crash");

    print_flush();
    return test_failures_ != 0;
}
