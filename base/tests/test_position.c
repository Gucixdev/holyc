#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#include "base.h"
#define POSITION_IMPLEMENTATION
#include "position.c"

int main(void)
{
    Position p;

    print_cstr("test_position\n");

    position_init(&p, vec3_make(1.0f, 2.0f, 3.0f));
    TEST_CHECK(p.value.x == 1.0f && p.value.y == 2.0f && p.value.z == 3.0f, "position_init sets the initial value");

    position_set(&p, vec3_make(5.0f, 5.0f, 5.0f));
    TEST_CHECK(p.value.x == 5.0f, "position_set replaces the value");

    position_translate(&p, vec3_make(1.0f, -1.0f, 0.0f));
    TEST_CHECK(p.value.x == 6.0f && p.value.y == 4.0f && p.value.z == 5.0f, "position_translate adds a delta");

    print_flush();
    return test_failures_ != 0;
}
