/* keys.c - input: keyboards, key bindings, pointer, cursor, tablets, mouse bindings, drag and drop, spawn/quit/chvt actions.
 * Part of the dwl compositor, split out of dwl.c (dwl: https://codeberg.org/dwl/dwl, see src/LICENSES/). */
#include "dwl.h"


/* bindings: key, mouse button and scroll axis tables (edit and rebuild) */

#define TAGKEYS(KEY,TAG) \
	{ MODKEY,                    KEY,            view,            {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL,  KEY,            toggleview,      {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_SHIFT, KEY,            tag,             {.ui = 1 << TAG} }, \
	{ MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT,KEY,toggletag,  {.ui = 1 << TAG} }

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

/* commands */
static const char *termcmd[] = { "foot", NULL };
static const char *menucmd[] = { "menu", NULL };   /* apps, games, scripts, calculator, clipboard */
static const char *runcmd[]  = { "dmenu_run", NULL };    /* every executable in PATH */
static const char *kbdcmd[]  = { "pkill", "-RTMIN", "wvkbd-mobintl", NULL };   /* toggle the on-screen keyboard */
static const char *emacscmd[] = { "emacsclient", "-c", "-a", "", NULL };
static const char *shotcmd[]  = { "shot", NULL };
static const char *shotareacmd[] = { "shot", "area", NULL };

const Key keys[] = {
	/* modifier                  key                  function          argument */
	{ MODKEY,                    XKB_KEY_w,           spawn,            {.v = menucmd} },
	{ MODKEY,                    XKB_KEY_p,           spawn,            {.v = menucmd} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_P,           spawn,            {.v = runcmd} },
	{ MODKEY,                    XKB_KEY_b,           spawn,            {.v = kbdcmd} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_W,           spawn,            {.v = emacscmd} },
	{ 0,                         XKB_KEY_Print,       spawn,            {.v = shotcmd} },
	{ MODKEY,                    XKB_KEY_Print,       spawn,            {.v = shotareacmd} },
	{ 0,                         XKB_KEY_XF86AudioRaiseVolume, spawn,   SHCMD("wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+") },
	{ 0,                         XKB_KEY_XF86AudioLowerVolume, spawn,   SHCMD("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-") },
	{ 0,                         XKB_KEY_XF86AudioMute,        spawn,   SHCMD("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle") },
	{ MODKEY,                    XKB_KEY_Return,      spawn,            {.v = termcmd} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Return,      zoom,             {0} },
	{ MODKEY,                    XKB_KEY_j,           focusstack,       {.i = +1} },
	{ MODKEY,                    XKB_KEY_k,           focusstack,       {.i = -1} },
	{ MODKEY,                    XKB_KEY_i,           incnmaster,       {.i = +1} },
	{ MODKEY,                    XKB_KEY_d,           incnmaster,       {.i = -1} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_h,           setmfact,         {.f = -0.05f} },
	{ MODKEY,                    XKB_KEY_h,           spawn,            SHCMD("wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+") },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_l,           setmfact,         {.f = +0.05f} },
	{ MODKEY,                    XKB_KEY_l,           spawn,            SHCMD("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-") },
	{ MODKEY,                    XKB_KEY_Tab,         view,             {0} },
	/* i3 layouts: split / floating / tabbed are t f m above; stacking and grid: */
	{ MODKEY,                    XKB_KEY_s,          setlayout,      {.v = &layouts[3]} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_T,          setlayout,      {.v = &layouts[4]} },
	/* directional focus / move (all four directions - Up and Down were missing before) */
	{ MODKEY,                    XKB_KEY_Left,       focusdir,       {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY,                    XKB_KEY_Right,      focusdir,       {.i = WLR_DIRECTION_RIGHT} },
	{ MODKEY,                    XKB_KEY_Up,         focusdir,       {.i = WLR_DIRECTION_UP} },
	{ MODKEY,                    XKB_KEY_Down,       focusdir,       {.i = WLR_DIRECTION_DOWN} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Left,       movedir,        {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Right,      movedir,        {.i = WLR_DIRECTION_RIGHT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Up,         movedir,        {.i = WLR_DIRECTION_UP} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Down,       movedir,        {.i = WLR_DIRECTION_DOWN} },
	/* workspaces: previous / next tag, and move the window along */
	{ MODKEY,                    XKB_KEY_z,          shiftview,      {.i = -1} },
	{ MODKEY,                    XKB_KEY_x,          shiftview,      {.i = +1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Z,          tagshiftview,   {.i = -1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_X,          tagshiftview,   {.i = +1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_c,           killclient,       {0} },
	{ MODKEY,                    XKB_KEY_q,           killclient,       {0} },
	{ MODKEY,                    XKB_KEY_t,           setlayout,        {.v = &layouts[0]} },
	{ MODKEY,                    XKB_KEY_f,           setlayout,        {.v = &layouts[1]} },
	{ MODKEY,                    XKB_KEY_m,           setlayout,        {.v = &layouts[2]} },
	{ MODKEY,                    XKB_KEY_space,       setlayout,        {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_space,       togglefloating,   {0} },
	{ MODKEY,                    XKB_KEY_e,           togglefullscreen, {0} },
	{ MODKEY,                    XKB_KEY_0,           view,             {.ui = ~0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_0,           tag,              {.ui = ~0} },
	{ MODKEY,                    XKB_KEY_comma,       focusmon,         {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY,                    XKB_KEY_period,      focusmon,         {.i = WLR_DIRECTION_RIGHT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_comma,       tagmon,           {.i = WLR_DIRECTION_LEFT} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_period,      tagmon,           {.i = WLR_DIRECTION_RIGHT} },
	TAGKEYS(                     XKB_KEY_1,           0),
	TAGKEYS(                     XKB_KEY_2,           1),
	TAGKEYS(                     XKB_KEY_3,           2),
	TAGKEYS(                     XKB_KEY_4,           3),
	TAGKEYS(                     XKB_KEY_5,           4),
	TAGKEYS(                     XKB_KEY_6,           5),
	TAGKEYS(                     XKB_KEY_7,           6),
	TAGKEYS(                     XKB_KEY_8,           7),
	TAGKEYS(                     XKB_KEY_9,           8),
	{ MODKEY|WLR_MODIFIER_CTRL|WLR_MODIFIER_SHIFT, XKB_KEY_Q, quit,        {0} },

	/* Ctrl-Alt-Backspace and Ctrl-Alt-Fx used to be handled by X server */
	{ WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_BackSpace, quit, {0} },
	/* Ctrl-Alt-Fx is used to switch to another VT, if you don't know what a VT is
	 * do not remove them.
	 */
#define CHVT(n) { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_F##n, chvt, {.ui = (n)} }
	CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
	CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

const Button buttons[] = {
	{ MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
	{ MODKEY, BTN_MIDDLE, togglefloating, {0} },
	{ MODKEY, BTN_RIGHT,  moveresize,     {.ui = CurResize} },
};

const Axis axes[] = {
	{ MODKEY, AxisUp,   spawn, SHCMD("volume-up_EXAMPLE") },
	{ MODKEY, AxisDown, spawn, SHCMD("volume-down_EXAMPLE") },
};

const size_t nkeys = LENGTH(keys);
const size_t nbuttons = LENGTH(buttons);
const size_t naxes = LENGTH(axes);

void
axisnotify(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an axis event,
	 * for example when you move the scroll wheel. */
	struct wlr_pointer_axis_event *event = data;
	const Axis *a;
	unsigned int adir;
	struct wlr_keyboard *keyboard;
	uint32_t mods;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	if (event->orientation == WL_POINTER_AXIS_VERTICAL_SCROLL)
		adir = event->delta > 0 ? AxisDown : AxisUp;
	else
		adir = event->delta > 0 ? AxisRight : AxisLeft;
	keyboard = wlr_seat_get_keyboard(seat);
	mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
	for (a = axes; a < (axes + naxes); a++) {
		if (CLEANMASK(mods) == CLEANMASK(a->mod) && adir == a->dir && a->func) {
			a->func(&a->arg);
			return;
		}
	}

	/* Notify the client with pointer focus of the axis event. */
	wlr_seat_pointer_notify_axis(seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

void
buttonpress(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_button_event *event = data;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	Client *c = NULL;
	LayerSurface *l = NULL;
	const Button *b;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	switch (event->state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		cursor_mode = CurPressed;
		selmon = xytomon(cursor->x, cursor->y);
		if (locked)
			break;

		/* Change focus if the button was _pressed_ over a client
		   or a layer surface with on-demand keyboard interactivity */
		xytonode(cursor->x, cursor->y, NULL, &c, &l, NULL, NULL);
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c))) {
			focusclient(c, 1);
		} else if (l && l->layer_surface->current.keyboard_interactive) {
			focusclient(NULL, 0);
			client_notify_enter(l->layer_surface->surface, wlr_seat_get_keyboard(seat));
		}

		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		for (b = buttons; b < (buttons + nbuttons); b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					event->button == b->button && b->func) {
				b->func(&b->arg);
				return;
			}
		}
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		/* If you released any buttons, we exit interactive move/resize mode. */
		if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
			cursor_mode = CurNormal;
			/* Update the cursor */
			wlr_seat_pointer_clear_focus(seat);
			motionnotify(0, NULL, 0, 0, 0, 0);
			/* Drop the window off on its new monitor */
			selmon = xytomon(cursor->x, cursor->y);
			setmon(grabc, selmon, 0);
			grabc = NULL;
			return;
		}
		cursor_mode = CurNormal;
		break;
	}
	/* If the event wasn't handled by the compositor, notify the client with
	 * pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(seat,
			event->time_msec, event->button, event->state);
}

void
chvt(const Arg *arg)
{
	wlr_session_change_vt(session, arg->ui);
}

void
createkeyboard(struct wlr_keyboard *keyboard)
{
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(keyboard, kb_group->wlr_group->keyboard.keymap);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(kb_group->wlr_group, keyboard);
}

KeyboardGroup *
createkeyboardgroup(void)
{
	KeyboardGroup *group = ecalloc(1, sizeof(*group));
	struct xkb_context *context;
	struct xkb_keymap *keymap;

	group->wlr_group = wlr_keyboard_group_create();
	group->wlr_group->data = group;

	/* Prepare an XKB keymap and assign it to the keyboard group. */
	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!(keymap = xkb_keymap_new_from_names(context, &xkb_rules,
				XKB_KEYMAP_COMPILE_NO_FLAGS)))
		die("failed to compile keymap");

	wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard, repeat_rate, repeat_delay);

	/* Set up listeners for keyboard events */
	LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
	LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers, keypressmod);

	group->key_repeat_source = wl_event_loop_add_timer(event_loop, keyrepeat, group);

	/* A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
	 * all of them. Set this combined wlr_keyboard as the seat keyboard.
	 */
	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	return group;
}

/* every pointer device we configured, so a changed setting (tap_to_click ...) reaches devices that are already plugged in */
static struct PtrDev { struct wlr_pointer *p; struct wl_listener destroy; struct wl_list link; } *ptrdevs[32];

static void
ptr_configure(struct wlr_pointer *pointer)
{
	struct libinput_device *device;
	if (wlr_input_device_is_libinput(&pointer->base)
			&& (device = wlr_libinput_get_device_handle(&pointer->base))) {

		if (libinput_device_config_tap_get_finger_count(device)) {
			libinput_device_config_tap_set_enabled(device, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(device, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(device, drag_lock);
			libinput_device_config_tap_set_button_map(device, button_map);
		}

		if (libinput_device_config_scroll_has_natural_scroll(device))
			libinput_device_config_scroll_set_natural_scroll_enabled(device, natural_scrolling);

		if (libinput_device_config_dwt_is_available(device))
			libinput_device_config_dwt_set_enabled(device, disable_while_typing);

		if (libinput_device_config_left_handed_is_available(device))
			libinput_device_config_left_handed_set(device, left_handed);

		if (libinput_device_config_middle_emulation_is_available(device))
			libinput_device_config_middle_emulation_set_enabled(device, middle_button_emulation);

		if (libinput_device_config_scroll_get_methods(device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method(device, scroll_method);

		if (libinput_device_config_click_get_methods(device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
			libinput_device_config_click_set_method(device, click_method);

		if (libinput_device_config_send_events_get_modes(device))
			libinput_device_config_send_events_set_mode(device, send_events_mode);

		if (libinput_device_config_accel_is_available(device)) {
			libinput_device_config_accel_set_profile(device, accel_profile);
			libinput_device_config_accel_set_speed(device, accel_speed);
		}
	}
}

static void
ptr_gone(struct wl_listener *listener, void *data)
{
	for (int i = 0; i < 32; i++)
		if (ptrdevs[i] && &ptrdevs[i]->destroy == listener) {
			wl_list_remove(&ptrdevs[i]->destroy.link);
			free(ptrdevs[i]);
			ptrdevs[i] = NULL;
		}
}

void
pointers_reconfigure(void)
{
	for (int i = 0; i < 32; i++)
		if (ptrdevs[i]) ptr_configure(ptrdevs[i]->p);
}

void
createpointer(struct wlr_pointer *pointer)
{
	ptr_configure(pointer);
	for (int i = 0; i < 32; i++)
		if (!ptrdevs[i]) {
			ptrdevs[i] = ecalloc(1, sizeof **ptrdevs);
			ptrdevs[i]->p = pointer;
			ptrdevs[i]->destroy.notify = ptr_gone;
			wl_signal_add(&pointer->base.events.destroy, &ptrdevs[i]->destroy);
			break;
		}
	wlr_cursor_attach_input_device(cursor, &pointer->base);
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = ecalloc(1, sizeof(*pointer_constraint));
	pointer_constraint->constraint = data;
	LISTEN(&pointer_constraint->constraint->events.destroy,
			&pointer_constraint->destroy, destroypointerconstraint);
}

void
cursorconstrain(struct wlr_pointer_constraint_v1 *constraint)
{
	if (active_constraint == constraint)
		return;

	if (active_constraint)
		wlr_pointer_constraint_v1_send_deactivated(active_constraint);

	active_constraint = constraint;
	wlr_pointer_constraint_v1_send_activated(constraint);
}

void
cursorframe(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(seat);
}

void
cursorwarptohint(void)
{
	Client *c = NULL;
	double sx = active_constraint->current.cursor_hint.x;
	double sy = active_constraint->current.cursor_hint.y;

	toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
	if (c && active_constraint->current.cursor_hint.enabled) {
		wlr_cursor_warp(cursor, NULL, sx + c->geom.x + c->bw, sy + c->geom.y + c->bw);
		wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
	}
}

void
destroydragicon(struct wl_listener *listener, void *data)
{
	/* Focus enter isn't sent during drag, so refocus the focused node. */
	focusclient(focustop(selmon), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroypointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);

	if (active_constraint == pointer_constraint->constraint) {
		cursorwarptohint();
		active_constraint = NULL;
	}

	wl_list_remove(&pointer_constraint->destroy.link);
	free(pointer_constraint);
}

void
destroykeyboardgroup(struct wl_listener *listener, void *data)
{
	KeyboardGroup *group = wl_container_of(listener, group, destroy);
	wl_event_source_remove(group->key_repeat_source);
	wl_list_remove(&group->key.link);
	wl_list_remove(&group->modifiers.link);
	wl_list_remove(&group->destroy.link);
	wlr_keyboard_group_destroy(group->wlr_group);
	free(group);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(wlr_pointer_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_TABLET:
		wlr_cursor_attach_input_device(cursor, device);
		device->data = wlr_tablet_create(tablet_mgr, seat, device);
		break;
	default:
		/* TODO handle other input device types */
		break;
	}

	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In dwl we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	/* TODO do we actually require a cursor? */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&kb_group->wlr_group->devices))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(seat, caps);
}

const Key *
keybinding(uint32_t mods, xkb_keysym_t sym)
{
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 */
	const Key *k = api_keybinding(mods, sym);
	if (k)
		return k;
	for (k = keys; k < (keys + nkeys); k++) {
		if (CLEANMASK(mods) == CLEANMASK(k->mod) && sym == k->keysym && k->func)
			return k;
	}
	return NULL;
}

void
keypress(struct wl_listener *listener, void *data)
{
	/* This event is raised when a key is pressed or released. */
	KeyboardGroup *group = wl_container_of(listener, group, key);
	struct wlr_keyboard_key_event *event = data;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	struct wlr_keyboard *kb = &group->wlr_group->keyboard;
	xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, keycode);
	int handled = 0;

	// Get the keysyms for level 0 (normal) and level 1 (shifted)
	// Only one keysym for each level:
	// multiple keysyms per keycode don't really exist in the real world
	for (int i = 0; i < 2; i++)
		group->keysyms[i] = keymap_get_one_sym_by_level(kb->keymap, keycode, layout, i);
	group->mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	if (!locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		for (int i = 0; i < 2; i++) {
			const Key *key = keybinding(group->mods, group->keysyms[i]);
			if (key) {
				consumed[event->keycode] = 1;
				key->func(&key->arg);
				handled = 1;
				break;
			}
		}
	}

	if (handled && group->wlr_group->keyboard.repeat_info.delay > 0)
		wl_event_source_timer_update(group->key_repeat_source,
				group->wlr_group->keyboard.repeat_info.delay);
	else
		wl_event_source_timer_update(group->key_repeat_source, 0);

	if (handled || input_method_keyboard_grab_forward_key(group, event))
		return;

	if (consumed[event->keycode]) {
		if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED)
			consumed[event->keycode] = 0;
		return; /* Don't pass to the client the release event of a handled key-press */
	}
	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Pass unhandled keycodes along to the client. */
	wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
}

void
keypressmod(struct wl_listener *listener, void *data)
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	KeyboardGroup *group = wl_container_of(listener, group, modifiers);

	if (input_method_keyboard_grab_forward_modifiers(group))
		return;

	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(seat,
			&group->wlr_group->keyboard.modifiers);
}

int
keyrepeat(void *data)
{
	KeyboardGroup *group = data;
	if (group->wlr_group->keyboard.repeat_info.rate <= 0)
		return 0;

	wl_event_source_timer_update(group->key_repeat_source,
			1000 / group->wlr_group->keyboard.repeat_info.rate);

	for (int i = 0; i < 2; i++) {
		const Key *key = keybinding(group->mods, group->keysyms[i]);
		if (key) {
			key->func(&key->arg);
			break;
		}
	}

	return 0;
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. Also, some hardware emits these events. */
	struct wlr_pointer_motion_absolute_event *event = data;
	double lx, ly, dx, dy;

	if (!event->time_msec) /* this is 0 with virtual pointers */
		wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);

	wlr_cursor_absolute_to_layout_coords(cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
	dx = lx - cursor->x;
	dy = ly - cursor->y;
	motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

/* Drawing tablets. A client that binds zwp_tablet_v2 gets the full tool (pressure, tilt, distance, ...);
 * any other client gets plain pointer motion and a left click for the pen tip, so every surface stays usable. */
static struct wlr_tablet_v2_tablet_tool *
tablettool(struct wlr_tablet_tool *tool)
{
	if (!tool->data)
		tool->data = wlr_tablet_tool_create(tablet_mgr, seat, tool);
	return tool->data;
}

/* Move the cursor to the tool position (0..1 on the tablet) and tell whoever is under it.
 * Returns 1 when a tablet-aware client received it, 0 when it fell back to the pointer. */
static int
tabletmove(struct wlr_tablet *tablet, struct wlr_tablet_tool *tool, uint32_t time, double x, double y)
{
	struct wlr_tablet_v2_tablet *t = tablet->base.data;
	struct wlr_tablet_v2_tablet_tool *t2 = tablettool(tool);
	struct wlr_surface *surface = NULL;
	double sx, sy;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	wlr_cursor_warp_absolute(cursor, &tablet->base, x, y);
	xytonode(cursor->x, cursor->y, &surface, NULL, NULL, &sx, &sy);
	if (surface && t && wlr_surface_accepts_tablet_v2(surface, t)) {
		if (t2->focused_surface != surface)
			wlr_tablet_v2_tablet_tool_notify_proximity_in(t2, t, surface);
		wlr_tablet_v2_tablet_tool_notify_motion(t2, sx, sy);
		return 1;
	}
	if (t2->focused_surface)
		wlr_tablet_v2_tablet_tool_notify_proximity_out(t2);
	motionnotify(time, &tablet->base, 0, 0, 0, 0);
	return 0;
}

void
tabletproximity(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_tool_proximity_event *event = data;

	if (event->state == WLR_TABLET_TOOL_PROXIMITY_IN) {
		tabletmove(event->tablet, event->tool, event->time_msec, event->x, event->y);
	} else {
		struct wlr_tablet_v2_tablet_tool *t2 = tablettool(event->tool);
		if (t2->focused_surface)
			wlr_tablet_v2_tablet_tool_notify_proximity_out(t2);
	}
}

void
tabletaxis(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_tool_axis_event *event = data;
	struct wlr_tablet_v2_tablet_tool *t2 = tablettool(event->tool);
	int tablet_client = t2->focused_surface != NULL;

	if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y))
		tablet_client = tabletmove(event->tablet, event->tool, event->time_msec, event->x, event->y);
	if (!tablet_client)
		return;
	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_PRESSURE)
		wlr_tablet_v2_tablet_tool_notify_pressure(t2, event->pressure);
	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_DISTANCE)
		wlr_tablet_v2_tablet_tool_notify_distance(t2, event->distance);
	if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y))
		wlr_tablet_v2_tablet_tool_notify_tilt(t2, event->tilt_x, event->tilt_y);
	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_ROTATION)
		wlr_tablet_v2_tablet_tool_notify_rotation(t2, event->rotation);
	if (event->updated_axes & WLR_TABLET_TOOL_AXIS_SLIDER)
		wlr_tablet_v2_tablet_tool_notify_slider(t2, event->slider);
}

void
tablettip(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_tool_tip_event *event = data;
	struct wlr_tablet_v2_tablet_tool *t2 = tablettool(event->tool);
	int down = event->state == WLR_TABLET_TOOL_TIP_DOWN;
	int tablet_client = tabletmove(event->tablet, event->tool, event->time_msec, event->x, event->y);
	Client *c = NULL;
	LayerSurface *l = NULL;

	if (down && !locked) {
		/* same click-to-focus as a mouse press */
		selmon = xytomon(cursor->x, cursor->y);
		xytonode(cursor->x, cursor->y, NULL, &c, &l, NULL, NULL);
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
			focusclient(c, 1);
		else if (l && l->layer_surface->current.keyboard_interactive) {
			focusclient(NULL, 0);
			client_notify_enter(l->layer_surface->surface, wlr_seat_get_keyboard(seat));
		}
	}
	if (tablet_client) {
		if (down)
			wlr_tablet_v2_tablet_tool_notify_down(t2);
		else
			wlr_tablet_v2_tablet_tool_notify_up(t2);
	} else {
		cursor_mode = down ? CurPressed : CurNormal;
		wlr_seat_pointer_notify_button(seat, event->time_msec, BTN_LEFT,
				down ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
		wlr_seat_pointer_notify_frame(seat);
	}
}

void
tabletbutton(struct wl_listener *listener, void *data)
{
	struct wlr_tablet_tool_button_event *event = data;
	struct wlr_tablet_v2_tablet_tool *t2 = tablettool(event->tool);

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	if (t2->focused_surface)
		wlr_tablet_v2_tablet_tool_notify_button(t2, event->button,
				(enum zwp_tablet_pad_v2_button_state)event->state);
}

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel)
{
	int nx, ny;
	double sx = 0, sy = 0, sx_confined, sy_confined;
	Client *c = NULL, *w = NULL;
	LayerSurface *l = NULL, *focusedl = NULL;
	struct wlr_surface *surface = NULL;
	struct wlr_pointer_constraint_v1 *constraint;

	/* Find the client under the pointer and send the event along. */
	xytonode(cursor->x, cursor->y, &surface, &c, &l, &sx, &sy);

	if (cursor_mode == CurPressed && !seat->drag
			&& surface != seat->pointer_state.focused_surface
			&& toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w, &focusedl) >= 0) {
		c = w;
		surface = seat->pointer_state.focused_surface;
		sx = cursor->x - (focusedl ? focusedl->scene->node.x : w->geom.x);
		sy = cursor->y - (focusedl ? focusedl->scene->node.y : w->geom.y);
	}

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
				relative_pointer_mgr, seat, (uint64_t)time * 1000,
				dx, dy, dx_unaccel, dy_unaccel);

		wl_list_for_each(constraint, &pointer_constraints->constraints, link)
			cursorconstrain(constraint);

		if (active_constraint && cursor_mode != CurResize && cursor_mode != CurMove) {
			toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
			if (c && active_constraint->surface == seat->pointer_state.focused_surface) {
				sx = cursor->x - c->geom.x - c->bw;
				sy = cursor->y - c->geom.y - c->bw;
				if (wlr_region_confine(&active_constraint->region, sx, sy,
						sx + dx, sy + dy, &sx_confined, &sy_confined)) {
					dx = sx_confined - sx;
					dy = sy_confined - sy;
				}

				if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
					return;
			}
		}

		wlr_cursor_move(cursor, device, dx, dy);
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

		/* Update selmon (even while dragging a window) */
		if (sloppyfocus)
			selmon = xytomon(cursor->x, cursor->y);
	}

	/* Update drag icon's position */
	wlr_scene_node_set_position(&drag_icon->node, (int)round(cursor->x), (int)round(cursor->y));

	/* If we are currently grabbing the mouse, handle and return */
	if (cursor_mode == CurMove) {
		/* Move the grabbed client to the new position. */
		nx = (int)round(cursor->x) - grabcx;
		ny = (int)round(cursor->y) - grabcy;
		if (abs(selmon->w.x - nx) < (int)snap)
			nx = selmon->w.x;
		else if (abs((selmon->w.x + selmon->w.width) - (nx + grabc->geom.width)) < (int)snap)
			nx = selmon->w.x + selmon->w.width - grabc->geom.width;
		if (abs(selmon->w.y - ny) < (int)snap)
			ny = selmon->w.y;
		else if (abs((selmon->w.y + selmon->w.height) - (ny + grabc->geom.height)) < (int)snap)
			ny = selmon->w.y + selmon->w.height - grabc->geom.height;
		resize(grabc, (struct wlr_box){.x = nx, .y = ny,
			.width = grabc->geom.width, .height = grabc->geom.height}, 1);
		return;
	} else if (cursor_mode == CurResize) {
		resize(grabc, (struct wlr_box){.x = grabc->geom.x, .y = grabc->geom.y,
			.width = (int)round(cursor->x) - grabc->geom.x, .height = (int)round(cursor->y) - grabc->geom.y}, 1);
		return;
	}

	/* If there's no client surface under the cursor, set the cursor image to a
	 * default. This is what makes the cursor image appear when you move it
	 * off of a client or over its border. */
	if (!surface && !seat->drag)
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	pointerfocus(c, l, surface, sx, sy, time);
}

void
motionrelative(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	motionnotify(event->time_msec, &event->pointer->base, event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);
}

void
moveresize(const Arg *arg)
{
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen)
		return;

	/* Float the window and tell motionnotify to grab it */
	setfloating(grabc, 1);
	switch (cursor_mode = arg->ui) {
	case CurMove:
		grabcx = (int)round(cursor->x) - grabc->geom.x;
		grabcy = (int)round(cursor->y) - grabc->geom.y;
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "all-scroll");
		break;
	case CurResize:
		/* Doesn't work for X11 output - the next absolute motion event
		 * returns the cursor to where it started */
		wlr_cursor_warp_closest(cursor, NULL,
				grabc->geom.x + grabc->geom.width,
				grabc->geom.y + grabc->geom.height);
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "se-resize");
		break;
	}
}

void
pointerfocus(Client *c, LayerSurface *l, struct wlr_surface *surface, double sx, double sy,
		uint32_t time)
{
	struct timespec now;

	if (surface != seat->pointer_state.focused_surface && sloppyfocus && time) {
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c))) {
			focusclient(c, 0);
		} else if (l && l->layer_surface->current.keyboard_interactive) {
			focusclient(NULL, 0);
			client_notify_enter(l->layer_surface->surface, wlr_seat_get_keyboard(seat));
		}
	}

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}

	if (!time) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

void
quit(const Arg *arg)
{
	wl_display_terminate(dpy);
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
			event->serial))
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

void
setcursor(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* If we're "grabbing" the cursor, don't use the client's image, we will
	 * restore it after "grabbing" sending a leave event, followed by a enter
	 * event, which will result in the client requesting set the cursor surface */
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_surface(cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided cursor shape. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_xcursor(cursor, cursor_mgr,
				wlr_cursor_shape_v1_name(event->shape));
}

void
setpsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor them
	 */
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in dwl we always honor them
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

void
spawn(const Arg *arg)
{
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		die("dwl: execvp %s failed:", ((char **)arg->v)[0]);
	}
}

void
startdrag(struct wl_listener *listener, void *data)
{
	struct wlr_drag *drag = data;
	if (!drag->icon)
		return;

	drag->icon->data = &wlr_scene_drag_icon_create(drag_icon, drag->icon)->node;
	LISTEN_STATIC(&drag->icon->events.destroy, destroydragicon);
}

void
virtualkeyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *kb = data;
	/* virtual keyboards shouldn't share keyboard group */
	KeyboardGroup *group = createkeyboardgroup();
	group->virtual_keyboard = &kb->keyboard;
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
	LISTEN(&kb->keyboard.base.events.destroy, &group->destroy, destroykeyboardgroup);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
	wlr_seat_set_capabilities(seat, seat->capabilities | WL_SEAT_CAPABILITY_KEYBOARD);
}

void
virtualpointer(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_input_device *device = &event->new_pointer->pointer.base;

	wlr_cursor_attach_input_device(cursor, device);
	if (event->suggested_output)
		wlr_cursor_map_input_to_output(cursor, device, event->suggested_output);
	wlr_seat_set_capabilities(seat, seat->capabilities | WL_SEAT_CAPABILITY_POINTER);
}

xkb_keysym_t
keymap_get_one_sym_by_level(struct xkb_keymap *keymap, xkb_keycode_t key,
		xkb_layout_index_t layout, xkb_level_index_t level)
{
	const xkb_keysym_t *syms;
	int count = xkb_keymap_key_get_syms_by_level(keymap, key, layout, level, &syms);
	return count ? syms[0] : XKB_KEY_NoSymbol;
}
