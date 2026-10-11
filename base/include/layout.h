#ifndef HOLYC_LAYOUT_H_
#define HOLYC_LAYOUT_H_

/* Generic flexbox-lite rect layout - deliberately standalone, does NOT
 * depend on or touch `src/uiux/im_gui.h`'s own `Im_Gui_Layout`/
 * `im_gui_layout_row` etc, which is a different, tightly-coupled,
 * per-frame immediate-mode system already fully implemented there.
 * This file is usable by ANY renderer (ansii_node.h/ansii_sequencer.h/
 * tui.h-based UIs, a future real GUI, or im_gui.h itself if a caller
 * wants to feed this algorithm's OUTPUT rects into
 * `im_gui_layout_set_next` - that's a caller-level choice, not
 * something this file arranges).
 *
 * The algorithm every real flexbox-style engine uses: each item is
 * either a FIXED size (`fixed_size` > 0, `flex` == 0) or a FLEX item
 * (`fixed_size` == 0, `flex` > 0, a weight) - sum the fixed sizes,
 * subtract from the bounding box's total extent along the stacking
 * axis, then split what's left among the flex items proportional to
 * their own weight relative to the sum of all weights. Cross-axis
 * size (height for a row, width for a column) always matches the
 * bounding box's own cross-axis extent - no per-item cross-axis
 * control in this minimal version, a documented gap not a silent one
 * (a real flexbox also has per-item alignment/wrap/grow-vs-shrink
 * distinctions this file doesn't attempt). */

#include "base.h"

typedef struct {
    f32 x, y, w, h;
} Layout_Rect;

typedef struct {
    f32 fixed_size; /* > 0 for a fixed-size item, 0 for a flex item */
    f32 flex;        /* > 0 for a flex item's weight, 0 for a fixed-size item */
} Layout_Item;

/* stacks `items[0..count)` left-to-right within `bounds`, writing each
 * item's resulting sub-rect into `out[0..count)` (caller-owned,
 * `count` entries). Every item's height matches `bounds.h`. */
void layout_row(Layout_Rect bounds, const Layout_Item *items, size_t count, Layout_Rect *out);

/* same algorithm, stacked top-to-bottom instead - every item's width
 * matches `bounds.w`. */
void layout_column(Layout_Rect bounds, const Layout_Item *items, size_t count, Layout_Rect *out);

#endif /* HOLYC_LAYOUT_H_ */

#ifdef LAYOUT_IMPLEMENTATION
#ifndef LAYOUT_IMPLEMENTATION_
#define LAYOUT_IMPLEMENTATION_

static void layout_stack_(f32 bounds_pos, f32 bounds_extent, f32 cross_pos, f32 cross_extent,
                           const Layout_Item *items, size_t count, Layout_Rect *out, i32 is_row)
{
    f32 fixed_total = 0.0f;
    f32 flex_total = 0.0f;
    f32 remaining;
    f32 cursor = bounds_pos;
    size_t i;

    for (i = 0; i < count; i++) {
        fixed_total += items[i].fixed_size;
        flex_total += items[i].flex;
    }

    remaining = bounds_extent - fixed_total;
    if (remaining < 0.0f) remaining = 0.0f; /* over-subscribed fixed sizes - clamp rather than produce a negative extent */

    for (i = 0; i < count; i++) {
        f32 extent = items[i].fixed_size;
        if (items[i].flex > 0.0f && flex_total > 0.0f) {
            extent = remaining * (items[i].flex / flex_total);
        }

        if (is_row) {
            out[i].x = cursor;
            out[i].y = cross_pos;
            out[i].w = extent;
            out[i].h = cross_extent;
        } else {
            out[i].x = cross_pos;
            out[i].y = cursor;
            out[i].w = cross_extent;
            out[i].h = extent;
        }

        cursor += extent;
    }
}

void layout_row(Layout_Rect bounds, const Layout_Item *items, size_t count, Layout_Rect *out)
{
    layout_stack_(bounds.x, bounds.w, bounds.y, bounds.h, items, count, out, 1);
}

void layout_column(Layout_Rect bounds, const Layout_Item *items, size_t count, Layout_Rect *out)
{
    layout_stack_(bounds.y, bounds.h, bounds.x, bounds.w, items, count, out, 0);
}

#endif /* LAYOUT_IMPLEMENTATION_ */
#endif /* LAYOUT_IMPLEMENTATION */
