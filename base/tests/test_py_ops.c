#include "test_common.h"
#include "py_ops.h"

typedef struct {
    i32 x, y;
} Point;

static void Point_init(Point *p, i32 x, i32 y)
{
    p->x = x;
    p->y = y;
}

static i32 Point_eq(const Point *a, const Point *b)
{
    return a->x == b->x && a->y == b->y;
}

static void Point_str(const Point *p, char *buf)
{
    /* tiny hand-rolled i32-to-decimal, avoiding a sprintf dependency
     * just for this test */
    i32 v = p->x;
    i32 i = 0;
    char tmp[12];
    i32 j;
    if (v == 0) { buf[i++] = '0'; }
    else {
        i32 n = 0;
        i32 neg = v < 0;
        u32 uv = neg ? (u32)(-v) : (u32)v;
        while (uv > 0) { tmp[n++] = (char)('0' + uv % 10); uv /= 10; }
        if (neg) buf[i++] = '-';
        for (j = n - 1; j >= 0; j--) buf[i++] = tmp[j];
    }
    buf[i++] = ',';
    buf[i] = '\0';
}

static i32 Point_len(const Point *p)
{
    return (p->x != 0) + (p->y != 0); /* number of nonzero components - a contrived but real, checkable definition */
}

static i32 Point_del_called_ = 0;
static void Point_del(Point *p)
{
    (void)p;
    Point_del_called_ = 1;
}

int main(void)
{
    Point a, b, c;

    print_cstr("test_py_ops\n");

    PY_INIT2(Point, &a, 3, 4);
    TEST_CHECK(a.x == 3 && a.y == 4, "PY_INIT2 expands to Point_init and passes both args through correctly");

    PY_INIT2(Point, &b, 3, 4);
    TEST_CHECK(PY_EQ(Point, &a, &b), "PY_EQ expands to Point_eq and reports equal points as equal");

    PY_INIT2(Point, &c, 3, 5);
    TEST_CHECK(!PY_EQ(Point, &a, &c), "PY_EQ correctly reports unequal points as unequal");

    {
        char buf[8];
        PY_STR1(Point, &a, buf);
        TEST_CHECK(buf[0] == '3' && buf[1] == ',', "PY_STR1 expands to Point_str and produces the expected leading digits");
    }

    TEST_CHECK(PY_LEN(Point, &a) == 2, "PY_LEN expands to Point_len - both x and y nonzero, so len is 2");

    {
        Point zero;
        PY_INIT2(Point, &zero, 0, 5);
        TEST_CHECK(PY_LEN(Point, &zero) == 1, "PY_LEN correctly reflects only one nonzero component");
    }

    PY_DEL(Point, &a);
    TEST_CHECK(Point_del_called_ == 1, "PY_DEL expands to Point_del and actually invokes it");

    print_flush();
    return test_failures_ != 0;
}
