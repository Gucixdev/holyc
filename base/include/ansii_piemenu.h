#ifndef HOLYC_ANSII_PIEMENU_H_
#define HOLYC_ANSII_PIEMENU_H_

/* Terminal renderer for a piemenu.h Piemenu - draws onto a tui.h
 * Tui_Buffer, mirroring ansii_node.h/ansii_sequencer.h's established
 * "all state lives in the plain data-model file, this file only turns
 * it into cells" split.
 *
 * Layout: each item's label is centered on a point at (radius,
 * item_angle) from (cx, cy) - screen-space, so item 0 (angle 0, "up")
 * lands ABOVE center, and later items sweep clockwise around it,
 * exactly matching piemenu.h's own angle convention. A terminal cell is
 * roughly twice as tall as it's wide, so the x offset is doubled
 * (2*radius*sin) relative to the y offset (radius*cos) to make the
 * drawn layout look like an actual circle rather than a flattened
 * ellipse - a real, deliberate correction, not an arbitrary constant.
 * The currently-selected item (Piemenu.selected) is drawn with '[' ']'
 * brackets around its label; every other item is drawn plain. A small
 * '+' marks the menu's own center point. */

#include "piemenu.h"
#include "tui.h"

void ansii_piemenu_render(const Piemenu *m, Tui_Buffer *buf, i32 cx, i32 cy, i32 radius, u16 fg, u16 selected_fg);

#endif /* HOLYC_ANSII_PIEMENU_H_ */

#ifdef ANSII_PIEMENU_IMPLEMENTATION
#ifndef ANSII_PIEMENU_IMPLEMENTATION_
#define ANSII_PIEMENU_IMPLEMENTATION_

#include <math.h> /* sinf/cosf - see piemenu.h's own top comment on the same, still-libm, documented gap */

static void ansii_piemenu_draw_label_(Tui_Buffer *buf, i32 cx, i32 cy, const char *label, u16 fg, i32 bracketed)
{
    size_t len = 0;
    i32 start_x;
    i32 x, i;

    while (label[len]) len++;
    if (bracketed) len += 2; /* the '[' ']' */

    start_x = cx - (i32)(len / 2);

    x = start_x;
    if (bracketed) tui_buffer_set_cell(buf, x++, cy, '[', fg, 0);
    for (i = 0; label[i]; i++) tui_buffer_set_cell(buf, x++, cy, (u32)(u8)label[i], fg, 0);
    if (bracketed) tui_buffer_set_cell(buf, x, cy, ']', fg, 0);
}

void ansii_piemenu_render(const Piemenu *m, Tui_Buffer *buf, i32 cx, i32 cy, i32 radius, u16 fg, u16 selected_fg)
{
    i32 i;

    tui_buffer_set_cell(buf, cx, cy, '+', fg, 0);

    for (i = 0; i < m->item_count; i++) {
        f32 angle = piemenu_item_angle(m, i);
        /* screen-space: "up" (angle 0) is -y, clockwise sweep - see this
         * file's own top comment for the 2x x-scale rationale. */
        i32 ix = cx + (i32)(2.0f * (f32)radius * sinf(angle) + (sinf(angle) >= 0.0f ? 0.5f : -0.5f));
        i32 iy = cy - (i32)((f32)radius * cosf(angle) + (cosf(angle) >= 0.0f ? 0.5f : -0.5f));
        i32 is_selected = (i == m->selected);

        ansii_piemenu_draw_label_(buf, ix, iy, m->items[i].label, is_selected ? selected_fg : fg, is_selected);
    }
}

#endif /* ANSII_PIEMENU_IMPLEMENTATION_ */
#endif /* ANSII_PIEMENU_IMPLEMENTATION */
