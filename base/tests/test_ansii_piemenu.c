#include "test_common.h"
#define PIEMENU_IMPLEMENTATION
#include "piemenu.h"
#define TUI_IMPLEMENTATION
#include "tui.h"
#define ANSII_PIEMENU_IMPLEMENTATION
#include "ansii_piemenu.h"

static u32 cell_ch_(const Tui_Buffer *buf, i32 x, i32 y)
{
    return buf->cells[y * buf->width + x].ch;
}

static i32 row_contains_(const Tui_Buffer *buf, i32 y, i32 x0, const char *text)
{
    i32 i;
    for (i = 0; text[i]; i++) {
        if (cell_ch_(buf, x0 + i, y) != (u32)(u8)text[i]) return 0;
    }
    return 1;
}

int main(void)
{
    Piemenu m;
    Tui_Buffer buf;

    print_cstr("test_ansii_piemenu\n");

    piemenu_init(&m);
    piemenu_add_item(&m, "north");
    piemenu_add_item(&m, "east");
    piemenu_add_item(&m, "south");
    piemenu_add_item(&m, "west");

    tui_buffer_init(&buf, 40, 20);
    tui_buffer_clear(&buf, 0, 0);
    ansii_piemenu_render(&m, &buf, 20, 10, 5, 1, 2);

    TEST_CHECK(cell_ch_(&buf, 20, 10) == '+', "center marker is drawn at (cx, cy)");

    /* item 0 ("north", angle 0) lands directly above center */
    TEST_CHECK(row_contains_(&buf, 5, 18, "north"), "item 0 (north) is drawn above the center point");

    /* item 1 ("east", angle pi/2) lands to the right, x doubled per this
     * file's own terminal-aspect-ratio correction */
    TEST_CHECK(row_contains_(&buf, 10, 28, "east"), "item 1 (east) is drawn to the right of center, on the center's own row");

    /* item 2 ("south", angle pi) lands directly below center */
    TEST_CHECK(row_contains_(&buf, 15, 18, "south"), "item 2 (south) is drawn below the center point");

    /* item 3 ("west", angle 3pi/2) lands to the left */
    TEST_CHECK(row_contains_(&buf, 10, 8, "west"), "item 3 (west) is drawn to the left of center, on the center's own row");

    /* nothing selected yet - no brackets anywhere */
    TEST_CHECK(cell_ch_(&buf, 17, 5) != '[', "unselected menu draws no bracket before item 0's label");

    /* re-render with item 1 (east) selected - its label should now be bracketed */
    piemenu_select_by_angle(&m, 3.14159265f * 0.5f);
    tui_buffer_init(&buf, 40, 20);
    tui_buffer_clear(&buf, 0, 0);
    ansii_piemenu_render(&m, &buf, 20, 10, 5, 1, 2);

    TEST_CHECK(row_contains_(&buf, 10, 27, "[east]"), "the selected item (east) is drawn wrapped in brackets");
    TEST_CHECK(!row_contains_(&buf, 5, 18, "[north]") && row_contains_(&buf, 5, 18, "north"),
               "the non-selected item (north) is still drawn plain, no brackets");

    {
        const Tui_Cell *c = &buf.cells[10 * buf.width + 28]; /* the 'e' in "[east]" */
        TEST_CHECK(c->fg == 2, "the selected item's label uses selected_fg, not the plain fg");
    }
    {
        const Tui_Cell *c = &buf.cells[5 * buf.width + 18]; /* the 'n' in "north" */
        TEST_CHECK(c->fg == 1, "a non-selected item's label uses the plain fg");
    }

    print_flush();
    return test_failures_ != 0;
}
