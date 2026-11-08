/* fx.c - visual effects of the compositor. dwl has no animations or blur; the only effect is the window border colour
 * (focus / urgent / normal), set here in one place so a future effect (fade, rounded corners) has a home.
 * Part of the dwl compositor, see src/LICENSES/. */
#include "dwl.h"

void
client_set_border_color(Client *c, const float color[static 4])
{
	int i;
	for (i = 0; i < 4; i++)
		wlr_scene_rect_set_color(c->border[i], color);
}
