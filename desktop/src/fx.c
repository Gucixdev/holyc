/* fx.c - visual effects of the compositor: the window border colour (focus / urgent / normal) and an optional spring-damper
 * animation of window position. dwl itself has no animations; this one is ours and is off by default (setting enable_animation).
 * With it off, fx_place() is one set_position call and fx_tick() returns on its first line: the old instant behaviour.
 * With it on, a window that is moved (relayout, layout change, tag switch) is not teleported: its scene node follows
 *     a = k*(target-pos) - d*vel        (semi-implicit Euler, fixed step 1/240 s, driven by the output frame callback)
 * until it is within half a pixel and nearly at rest, then it lands exactly on the target. Only the position is animated;
 * the window size changes at once. Part of the dwl compositor, see src/LICENSES/. */
#include "dwl.h"

int enable_animation = 0;
float animation_stiffness = 180.0f, animation_damping = 26.0f; /* 26 is a little under critical damping (2*sqrt(180)=26.8) */
static int fx_active; /* windows still moving */

void
client_set_border_color(Client *c, const float color[static 4])
{
	int i;
	for (i = 0; i < 4; i++)
		wlr_scene_rect_set_color(c->border[i], color);
}

static double
fx_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* put the window's scene node where c->geom says; animated when enabled and the window has been placed before */
void
fx_place(Client *c, int interact)
{
	if (!enable_animation || interact || !c->fx_init || c->mon == NULL) {
		if (c->fx_moving) { c->fx_moving = 0; fx_active--; }
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		c->fx_x = c->geom.x; c->fx_y = c->geom.y; c->fx_vx = c->fx_vy = 0; c->fx_init = 1;
		return;
	}
	if (!c->fx_moving && c->scene->node.x == c->geom.x && c->scene->node.y == c->geom.y)
		return;
	if (!c->fx_moving) { c->fx_moving = 1; fx_active++; c->fx_t = fx_now(); }
	wlr_output_schedule_frame(c->mon->wlr_output);
}

/* called at the top of every output frame: advance the springs of this output's windows */
void
fx_tick(Monitor *m)
{
	Client *c;
	double now;
	if (!fx_active)
		return;
	now = fx_now();
	wl_list_for_each(c, &clients, link) {
		double dt, tx, ty;
		if (!c->fx_moving || c->mon != m || !c->scene)
			continue;
		tx = c->geom.x; ty = c->geom.y;
		dt = now - c->fx_t;
		if (dt > 0.05) dt = 0.05;
		c->fx_t = now;
		for (; dt > 0 && enable_animation; dt -= 1.0 / 240) {
			double h = dt < 1.0 / 240 ? dt : 1.0 / 240;
			c->fx_vx += (animation_stiffness * (tx - c->fx_x) - animation_damping * c->fx_vx) * h;
			c->fx_vy += (animation_stiffness * (ty - c->fx_y) - animation_damping * c->fx_vy) * h;
			c->fx_x += c->fx_vx * h; c->fx_y += c->fx_vy * h;
		}
		if (!enable_animation || (fabs(tx - c->fx_x) < 0.5 && fabs(ty - c->fx_y) < 0.5 &&
				fabs(c->fx_vx) < 5 && fabs(c->fx_vy) < 5)) {
			c->fx_x = tx; c->fx_y = ty; c->fx_vx = c->fx_vy = 0;
			c->fx_moving = 0; fx_active--;
		} else
			wlr_output_schedule_frame(m->wlr_output);
		wlr_scene_node_set_position(&c->scene->node, (int)lround(c->fx_x), (int)lround(c->fx_y));
	}
}

/* the setting was switched off: land every window now */
void
fx_snap_all(void)
{
	Client *c;
	wl_list_for_each(c, &clients, link)
		if (c->fx_moving) {
			c->fx_moving = 0; fx_active--;
			c->fx_x = c->geom.x; c->fx_y = c->geom.y; c->fx_vx = c->fx_vy = 0;
			wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		}
}

int fx_moving_count(void) { return fx_active; }

/* the window is going away */
void
fx_forget(Client *c)
{
	if (c->fx_moving) { c->fx_moving = 0; fx_active--; }
	c->fx_init = 0;
}
