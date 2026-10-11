#include "test_common.h"
#define LAYOUT_IMPLEMENTATION
#include "layout.h"

int main(void)
{
    print_cstr("test_layout\n");

    /* 3 items, one fixed at 100, two flex-weight-1-each, packed
       horizontally into a 300-wide bounding box: after the fixed 100
       is subtracted, 200 remains, split evenly 100/100 across the two
       flex items. */
    {
        Layout_Item items[3];
        Layout_Rect out[3];
        Layout_Rect bounds;

        items[0].fixed_size = 100.0f; items[0].flex = 0.0f;
        items[1].fixed_size = 0.0f;   items[1].flex = 1.0f;
        items[2].fixed_size = 0.0f;   items[2].flex = 1.0f;

        bounds.x = 0.0f; bounds.y = 0.0f; bounds.w = 300.0f; bounds.h = 50.0f;

        layout_row(bounds, items, 3, out);

        TEST_CHECK(out[0].w == 100.0f, "fixed item gets exactly its fixed_size");
        TEST_CHECK(out[1].w > 99.9f && out[1].w < 100.1f, "first flex item gets its even share (100) of the 200 remaining");
        TEST_CHECK(out[2].w > 99.9f && out[2].w < 100.1f, "second flex item gets its even share (100) of the 200 remaining");
        TEST_CHECK(out[0].x == 0.0f, "first item starts at the bounding box's own x");
        TEST_CHECK(out[1].x == 100.0f, "second item starts right after the first item's width");
        TEST_CHECK(out[2].x == 200.0f, "third item starts right after the second item's width");
        TEST_CHECK(out[0].h == 50.0f && out[1].h == 50.0f && out[2].h == 50.0f, "every item's height matches the bounding box's height (row layout)");
    }

    /* unequal flex weights: a 2:1 split of the remaining space */
    {
        Layout_Item items[2];
        Layout_Rect out[2];
        Layout_Rect bounds;

        items[0].fixed_size = 0.0f; items[0].flex = 2.0f;
        items[1].fixed_size = 0.0f; items[1].flex = 1.0f;

        bounds.x = 0.0f; bounds.y = 0.0f; bounds.w = 300.0f; bounds.h = 10.0f;

        layout_row(bounds, items, 2, out);

        TEST_CHECK(out[0].w > 199.9f && out[0].w < 200.1f, "2:1 flex split: first item gets 200 of 300");
        TEST_CHECK(out[1].w > 99.9f && out[1].w < 100.1f, "2:1 flex split: second item gets 100 of 300");
    }

    /* vertical stacking (a column) works the same way, transposed */
    {
        Layout_Item items[2];
        Layout_Rect out[2];
        Layout_Rect bounds;

        items[0].fixed_size = 20.0f; items[0].flex = 0.0f;
        items[1].fixed_size = 0.0f;  items[1].flex = 1.0f;

        bounds.x = 5.0f; bounds.y = 10.0f; bounds.w = 40.0f; bounds.h = 100.0f;

        layout_column(bounds, items, 2, out);

        TEST_CHECK(out[0].h == 20.0f, "column: fixed item gets exactly its fixed_size as height");
        TEST_CHECK(out[1].h == 80.0f, "column: flex item fills the remaining height (100-20=80)");
        TEST_CHECK(out[0].y == 10.0f, "column: first item starts at the bounding box's own y");
        TEST_CHECK(out[1].y == 30.0f, "column: second item starts right after the first item's height");
        TEST_CHECK(out[0].w == 40.0f && out[1].w == 40.0f, "column: every item's width matches the bounding box's width");
        TEST_CHECK(out[0].x == 5.0f && out[1].x == 5.0f, "column: every item's x matches the bounding box's own x");
    }

    print_flush();
    return test_failures_ != 0;
}
