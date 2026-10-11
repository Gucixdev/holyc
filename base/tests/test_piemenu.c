#include "test_common.h"
#define PIEMENU_IMPLEMENTATION
#include "piemenu.h"

int main(void)
{
    Piemenu m;

    print_cstr("test_piemenu\n");
    piemenu_init(&m);

    TEST_CHECK(m.item_count == 0, "init starts with no items");
    TEST_CHECK(piemenu_selected(&m) == NULL, "nothing selected before any item is added");
    TEST_CHECK(piemenu_select_by_angle(&m, 0.0f) == -1, "select on an empty menu returns -1");

    TEST_CHECK(piemenu_add_item(&m, "north") == 0, "first item gets index 0");
    TEST_CHECK(piemenu_add_item(&m, "east") == 1, "second item gets index 1");
    TEST_CHECK(piemenu_add_item(&m, "south") == 2, "third item gets index 2");
    TEST_CHECK(piemenu_add_item(&m, "west") == 3, "fourth item gets index 3");
    TEST_CHECK(m.item_count == 4, "4 items added");

    /* 4 items -> 90 degree slices, item 0 centered on angle 0 ("up") */
    TEST_CHECK(piemenu_item_angle(&m, 0) < 0.001f, "item 0 is centered at angle 0");
    {
        f32 expected = 3.14159265f * 0.5f; /* pi/2 */
        f32 got = piemenu_item_angle(&m, 1);
        TEST_CHECK(got > expected - 0.001f && got < expected + 0.001f, "item 1 is centered at pi/2 (90 degrees clockwise from up)");
    }
    TEST_CHECK(piemenu_item_angle(&m, 99) == 0.0f, "out-of-range index returns 0.0 rather than garbage");

    TEST_CHECK(piemenu_select_by_angle(&m, 0.0f) == 0, "angle 0 (dead center of item 0's slice) selects item 0");
    TEST_CHECK(piemenu_selected(&m) != NULL, "selected() is non-NULL once something is selected");
    {
        const Piemenu_Item *sel = piemenu_selected(&m);
        TEST_CHECK(sel->label[0] == 'n', "selected() returns the actual selected item (north)");
    }

    TEST_CHECK(piemenu_select_by_angle(&m, 3.14159265f * 0.5f) == 1, "angle pi/2 selects item 1 (east)");
    TEST_CHECK(piemenu_select_by_angle(&m, 3.14159265f) == 2, "angle pi selects item 2 (south)");
    TEST_CHECK(piemenu_select_by_angle(&m, 3.14159265f * 1.5f) == 3, "angle 3pi/2 selects item 3 (west)");

    /* slice boundaries: just inside item 0's slice on either side of the wrap */
    TEST_CHECK(piemenu_select_by_angle(&m, 0.39f) == 0, "just under +45deg still selects item 0");
    TEST_CHECK(piemenu_select_by_angle(&m, -0.39f) == 0, "just under -45deg (wraps near 2pi) still selects item 0");

    /* negative and >2pi angles wrap correctly rather than erroring */
    TEST_CHECK(piemenu_select_by_angle(&m, -3.14159265f * 0.5f) == 3, "negative angle -pi/2 wraps to the same slice as 3pi/2 (west)");
    TEST_CHECK(piemenu_select_by_angle(&m, 3.14159265f * 2.5f) == 1, "angle > 2pi wraps to the same slice as pi/2 (east)");

    piemenu_clear_selection(&m);
    TEST_CHECK(piemenu_selected(&m) == NULL, "clear_selection resets to nothing selected");

    /* direction-vector convenience wrapper, screen-space y-down */
    TEST_CHECK(piemenu_select_by_direction(&m, 0.0f, -1.0f) == 0, "direction straight up selects item 0 (north)");
    TEST_CHECK(piemenu_select_by_direction(&m, 1.0f, 0.0f) == 1, "direction straight right selects item 1 (east)");
    TEST_CHECK(piemenu_select_by_direction(&m, 0.0f, 1.0f) == 2, "direction straight down selects item 2 (south)");
    TEST_CHECK(piemenu_select_by_direction(&m, -1.0f, 0.0f) == 3, "direction straight left selects item 3 (west)");

    {
        i32 before = m.selected;
        i32 result = piemenu_select_by_direction(&m, 0.0f, 0.0f);
        TEST_CHECK(result == before, "a zero direction vector leaves the current selection unchanged");
    }

    /* filling to capacity and rejecting beyond it */
    {
        Piemenu full;
        i32 i;
        i32 ok = 1;
        piemenu_init(&full);
        for (i = 0; i < PIEMENU_MAX_ITEMS; i++) {
            if (piemenu_add_item(&full, "x") != i) ok = 0;
        }
        TEST_CHECK(ok, "adding exactly PIEMENU_MAX_ITEMS items succeeds with sequential indices");
        TEST_CHECK(piemenu_add_item(&full, "overflow") == -1, "adding one more than capacity returns -1");
        TEST_CHECK(full.item_count == PIEMENU_MAX_ITEMS, "item_count did not grow past capacity on the rejected add");
    }

    print_flush();
    return test_failures_ != 0;
}
