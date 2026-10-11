#include "test_common.h"
#define VECTOR_IMPLEMENTATION
#include "base.h"
#include "mathx.h"
#define SCALE_IMPLEMENTATION
#include "scale.c"

int main(void)
{
    Scale s;

    print_cstr("test_scale\n");

    scale_init(&s, vec3_make(1.0f, 1.0f, 1.0f));
    TEST_CHECK(s.value.x == 1.0f && s.value.y == 1.0f && s.value.z == 1.0f, "scale_init sets the initial value");

    scale_set_uniform(&s, 2.0f);
    TEST_CHECK(s.value.x == 2.0f && s.value.y == 2.0f && s.value.z == 2.0f, "scale_set_uniform sets all 3 axes equally");

    {
        vec3 v = scale_apply(&s, vec3_make(3.0f, 4.0f, 5.0f));
        TEST_CHECK(v.x == 6.0f && v.y == 8.0f && v.z == 10.0f, "scale_apply scales a vector component-wise");
    }

    print_flush();
    return test_failures_ != 0;
}
