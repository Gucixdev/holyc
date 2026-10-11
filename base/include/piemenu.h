#ifndef HOLYC_PIEMENU_H_
#define HOLYC_PIEMENU_H_

/* Pure data model for a radial/pie menu - selection by ANGLE (mouse
 * direction from a center point, joystick vector, or a directly-given
 * angle), not by cursor-through-a-list navigation the way a normal
 * dropdown works. Deliberately just the model, same split as node.h/
 * sequencer.h: no rendering, no input handling, both live in their own
 * files on top of this one (ansii_piemenu.h for a tui.h terminal
 * renderer).
 *
 * Convention: item 0 sits at 12 o'clock (angle 0), items are placed
 * clockwise around the circle in the order they were added, each
 * getting an equal angular slice (2*pi / item_count wide, centered on
 * the item's own angle). Angles are radians, [0, 2*pi), clockwise from
 * "up" - matches how a screen-space (x right, y DOWN) mouse-offset
 * angle naturally falls out of atan2(dx, -dy), not the usual math-
 * convention counterclockwise-from-"right" angle. */

#include "base.h"

#ifndef PIEMENU_MAX_ITEMS
#define PIEMENU_MAX_ITEMS 16
#endif

typedef struct {
    const char *label; /* caller-owned, not copied */
} Piemenu_Item;

typedef struct {
    Piemenu_Item items[PIEMENU_MAX_ITEMS];
    i32 item_count;
    i32 selected; /* index into items[], -1 if nothing selected yet */
} Piemenu;

void piemenu_init(Piemenu *m);

/* returns the new item's index, or -1 if PIEMENU_MAX_ITEMS is already full. */
i32 piemenu_add_item(Piemenu *m, const char *label);

/* the angular center of item i's slice, in [0, 2*pi) - item 0 at 0
 * ("up"), each subsequent item item_count-th of a full turn further
 * clockwise. Returns 0.0f if i is out of range (no valid items to
 * center on). */
f32 piemenu_item_angle(const Piemenu *m, i32 i);

/* selects whichever item's slice contains angle_rad (wrapped into
 * [0, 2*pi) first, so both negative and >2*pi input behave the same as
 * their equivalent angle in range) and returns its index, or -1 if the
 * menu has no items. Updates m->selected. */
i32 piemenu_select_by_angle(Piemenu *m, f32 angle_rad);

/* convenience wrapper: dx,dy is a direction vector FROM the menu's
 * center (screen-space, y down - e.g. mouse_x - center_x, mouse_y -
 * center_y) - converts to an angle via atan2 and calls
 * piemenu_select_by_angle. (0,0) (no real direction) leaves the
 * selection unchanged and returns the CURRENT m->selected rather than
 * picking an arbitrary item for an undefined angle. */
i32 piemenu_select_by_direction(Piemenu *m, f32 dx, f32 dy);

const Piemenu_Item *piemenu_selected(const Piemenu *m); /* NULL if selected == -1 */

void piemenu_clear_selection(Piemenu *m);

#endif /* HOLYC_PIEMENU_H_ */

#ifdef PIEMENU_IMPLEMENTATION
#ifndef PIEMENU_IMPLEMENTATION_
#define PIEMENU_IMPLEMENTATION_

/* atan2f still comes from libm here - a real, documented gap (matches
 * easing.h's own slerp acos/sin precedent): hand-rolling a good atan2
 * approximation is real, separate scope, not attempted in this pass. */
#include <math.h>

#ifndef PIEMENU_PI_
#define PIEMENU_PI_ 3.14159265358979323846f
#endif
#ifndef PIEMENU_TWO_PI_
#define PIEMENU_TWO_PI_ 6.28318530717958647692f
#endif

void piemenu_init(Piemenu *m)
{
    m->item_count = 0;
    m->selected = -1;
}

i32 piemenu_add_item(Piemenu *m, const char *label)
{
    if (m->item_count >= PIEMENU_MAX_ITEMS) return -1;
    m->items[m->item_count].label = label;
    return m->item_count++;
}

f32 piemenu_item_angle(const Piemenu *m, i32 i)
{
    if (i < 0 || i >= m->item_count) return 0.0f;
    return (PIEMENU_TWO_PI_ * (f32)i) / (f32)m->item_count;
}

static f32 piemenu_wrap_angle_(f32 a)
{
    while (a < 0.0f) a += PIEMENU_TWO_PI_;
    while (a >= PIEMENU_TWO_PI_) a -= PIEMENU_TWO_PI_;
    return a;
}

i32 piemenu_select_by_angle(Piemenu *m, f32 angle_rad)
{
    f32 slice, a, half;
    i32 idx;

    if (m->item_count <= 0) return -1;

    slice = PIEMENU_TWO_PI_ / (f32)m->item_count;
    half = slice * 0.5f;
    a = piemenu_wrap_angle_(angle_rad + half); /* shift so item 0's slice starts at 0 after floor-divide */
    idx = (i32)(a / slice);
    if (idx >= m->item_count) idx = m->item_count - 1; /* guard float rounding at the wrap boundary */

    m->selected = idx;
    return idx;
}

i32 piemenu_select_by_direction(Piemenu *m, f32 dx, f32 dy)
{
    f32 angle;

    if (dx == 0.0f && dy == 0.0f) return m->selected;

    /* screen space is y-down, "up" (0 angle) is -y; atan2f(dx, -dy)
     * gives 0 for straight up and increases CLOCKWISE, matching this
     * file's own documented angle convention directly (no extra sign
     * flip or axis swap needed at call sites). */
    angle = atan2f(dx, -dy);
    return piemenu_select_by_angle(m, angle);
}

const Piemenu_Item *piemenu_selected(const Piemenu *m)
{
    if (m->selected < 0 || m->selected >= m->item_count) return NULL;
    return &m->items[m->selected];
}

void piemenu_clear_selection(Piemenu *m)
{
    m->selected = -1;
}

#endif /* PIEMENU_IMPLEMENTATION_ */
#endif /* PIEMENU_IMPLEMENTATION */
