/* dwl.h - shared types, prototypes and globals of the compositor (compositor.c, client.c, keys.c, settings.c, fx.c, ime.c, config.c, theme.c).
 * Derived from dwl (https://codeberg.org/dwl/dwl), which is a fork of tinywl and dwm; see src/LICENSES/. */
#ifndef DWL_H
#define DWL_H
#include <getopt.h>
#include <libinput.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_output_swapchain_manager.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>
#endif

#include "xdg-shell-protocol.h"
#include "util.h"

#include <wlr/types/wlr_input_method_v2.h>
#include <wlr/types/wlr_text_input_v3.h>

/* macros */
#define CLEANMASK(mask)         (mask & ~WLR_MODIFIER_CAPS)
#define VISIBLEON(C, M)         ((M) && (C)->mon == (M) && ((C)->tags & (M)->tagset[(M)->seltags]))
#define TAGMASK                 ((1u << TAGCOUNT) - 1)
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define LISTEN_STATIC(E, H)     do { struct wl_listener *_l = ecalloc(1, sizeof(*_l)); _l->notify = (H); wl_signal_add((E), _l); } while (0)

/* enums */
enum { CurNormal, CurPressed, CurMove, CurResize }; /* cursor */
enum { XDGShell, LayerShell, X11 }; /* client types */
enum { LyrBg, LyrBottom, LyrTile, LyrFloat, LyrTop, LyrFS, LyrUnmanaged, LyrOverlay,
	LyrIMPopup, LyrBlock, NUM_LAYERS }; /* scene layers */
enum { AxisUp, AxisRight, AxisDown, AxisLeft };

typedef union {
	int i;
	uint32_t ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	unsigned int dir;
	void (*func)(const Arg *);
	const Arg arg;
} Axis;

typedef struct {
	unsigned int mod;
	unsigned int button;
	void (*func)(const Arg *);
	const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct Client Client;
struct Client {
	/* Must keep this field first */
	unsigned int type; /* XDGShell or X11* */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_rect *border[4]; /* top, bottom, left, right */
	struct wlr_scene_tree *scene_surface;
	struct wl_list link;
	struct wl_list flink;
	struct wlr_box geom; /* layout-relative, includes border */
	struct wlr_box prev; /* layout-relative, includes border */
	struct wlr_box bounds; /* only width and height are used */
	union {
		struct wlr_xdg_surface *xdg;
		struct wlr_xwayland_surface *xwayland;
	} surface;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wlr_ext_foreign_toplevel_handle_v1 *foreign_toplevel_handle;
	struct wlr_scene *image_capture_scene;
	struct wlr_ext_image_capture_source_v1 *image_capture_source;
	union {
		struct wlr_scene_tree *image_capture_tree;
		struct wlr_scene_surface *image_capture_scene_surface;
	} capture;
	struct wl_listener commit;
	struct wl_listener map;
	struct wl_listener maximize;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener fullscreen;
	struct wl_listener set_decoration_mode;
	struct wl_listener destroy_decoration;
#ifdef XWAYLAND
	struct wl_listener activate;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener configure;
	struct wl_listener set_geometry;
	struct wl_listener set_hints;
#endif
	unsigned int bw;
	uint32_t tags;
	int isfloating, isurgent, isfullscreen;
	double fx_x, fx_y, fx_vx, fx_vy, fx_t; /* spring state, fx.c */
	int fx_init, fx_moving;
};

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	struct wlr_keyboard_group *wlr_group;
	struct wlr_keyboard *virtual_keyboard;

	xkb_keysym_t keysyms[2];
	uint32_t mods;
	struct wl_event_source *key_repeat_source;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
} KeyboardGroup;

typedef struct {
	/* Must keep this field first */
	unsigned int type; /* LayerShell */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *popups;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_list link;
	int mapped;
	struct wlr_layer_surface_v1 *layer_surface;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
} LayerSurface;

typedef struct {
	const char *symbol;
	void (*arrange)(Monitor *);
} Layout;

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *fullscreen_bg; /* See createmon() for info */
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_box m; /* monitor area, layout-relative */
	struct wlr_box w; /* window area, layout-relative */
	struct wl_list layers[4]; /* LayerSurface.link */
	const Layout *lt[2];
	const Layout *tlt[31]; /* layout per tag of THIS monitor (index = tag number - 1); lt[sellt] is the one of the shown view */
	const Layout *prevlt;  /* layout before the last change, for setlayout with no argument */
	struct wlr_scene_buffer *tabbuf; /* tab bar of tabbed / stack, tabs.c */
	Client *tabsel;           /* window shown by tabbed / stack */
	int ntabs;
	unsigned int seltags;
	unsigned int sellt;
	uint32_t tagset[2];
	float mfact;
	int gamma_lut_changed;
	int nmaster;
	char ltsymbol[16];
	int asleep;
};

typedef struct {
	const char *name;
	float mfact;
	int nmaster;
	float scale;
	const Layout *lt;
	enum wl_output_transform rr;
	int x, y;
	unsigned int deftag; /* 1-based default tag for this output, 0 = tag 1 (unset) */
} MonitorRule;

typedef struct {
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
} PointerConstraint;

typedef struct {
	const char *id;
	const char *title;
	uint32_t tags;
	int isfloating;
	int monitor;
} Rule;

typedef struct {
	struct wlr_scene_tree *scene;

	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
} SessionLock;


/* tagging - TAGCOUNT must be no greater than 31 */
#define TAGCOUNT (9)

/* If you want to use the windows key for MODKEY, use WLR_MODIFIER_LOGO */
#define MODKEY WLR_MODIFIER_LOGO   /* Super, same as Hyprland; Alt stays free for Emacs */

/* function declarations */
void applybounds(Client *c, struct wlr_box *bbox);
void applyrules(Client *c);
void arrange(Monitor *m);
void arrangelayer(Monitor *m, struct wl_list *list,
		struct wlr_box *usable_area, int exclusive);
void arrangelayers(Monitor *m);
void axisnotify(struct wl_listener *listener, void *data);
void buttonpress(struct wl_listener *listener, void *data);
void capturerequest(struct wl_listener *listener, void *data);
void chvt(const Arg *arg);
void checkidleinhibitor(struct wlr_surface *exclude);
void cleanup(void);
void cleanupmon(struct wl_listener *listener, void *data);
void cleanuplisteners(void);
void closemon(Monitor *m);
void commitlayersurfacenotify(struct wl_listener *listener, void *data);
void commitnotify(struct wl_listener *listener, void *data);
void commitpopup(struct wl_listener *listener, void *data);
void createdecoration(struct wl_listener *listener, void *data);
void createidleinhibitor(struct wl_listener *listener, void *data);
void createkeyboard(struct wlr_keyboard *keyboard);
KeyboardGroup *createkeyboardgroup(void);
void createlayersurface(struct wl_listener *listener, void *data);
void createlocksurface(struct wl_listener *listener, void *data);
void createmon(struct wl_listener *listener, void *data);
void createnotify(struct wl_listener *listener, void *data);
void createpointer(struct wlr_pointer *pointer);
void createpointerconstraint(struct wl_listener *listener, void *data);
void createpopup(struct wl_listener *listener, void *data);
void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
void cursorframe(struct wl_listener *listener, void *data);
void cursorwarptohint(void);
void destroydecoration(struct wl_listener *listener, void *data);
void destroydragicon(struct wl_listener *listener, void *data);
void destroyidleinhibitor(struct wl_listener *listener, void *data);
void destroylayersurfacenotify(struct wl_listener *listener, void *data);
void destroylock(SessionLock *lock, int unlocked);
void destroylocksurface(struct wl_listener *listener, void *data);
void destroynotify(struct wl_listener *listener, void *data);
void destroypointerconstraint(struct wl_listener *listener, void *data);
void destroysessionlock(struct wl_listener *listener, void *data);
void destroykeyboardgroup(struct wl_listener *listener, void *data);
Monitor *dirtomon(enum wlr_direction dir);
void focusclient(Client *c, int lift);
void focusmon(const Arg *arg);
void focusstack(const Arg *arg);
void focusdir(const Arg *arg);
void movedir(const Arg *arg);
Client *focustop(Monitor *m);
void fullscreennotify(struct wl_listener *listener, void *data);
void gpureset(struct wl_listener *listener, void *data);
void handlesig(int signo);
void incnmaster(const Arg *arg);
void inputdevice(struct wl_listener *listener, void *data);
const Key *keybinding(uint32_t mods, xkb_keysym_t sym);
void keypress(struct wl_listener *listener, void *data);
void keypressmod(struct wl_listener *listener, void *data);
int keyrepeat(void *data);
void killclient(const Arg *arg);
void locksession(struct wl_listener *listener, void *data);
void mapnotify(struct wl_listener *listener, void *data);
void maximizenotify(struct wl_listener *listener, void *data);
void monocle(Monitor *m);
void motionabsolute(struct wl_listener *listener, void *data);
void tabletaxis(struct wl_listener *listener, void *data);
void tabletbutton(struct wl_listener *listener, void *data);
void tabletproximity(struct wl_listener *listener, void *data);
void tablettip(struct wl_listener *listener, void *data);
void motionnotify(uint32_t time, struct wlr_input_device *device, double sx,
		double sy, double sx_unaccel, double sy_unaccel);
void motionrelative(struct wl_listener *listener, void *data);
void moveresize(const Arg *arg);
void outputmgrapply(struct wl_listener *listener, void *data);
void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);
void outputmgrtest(struct wl_listener *listener, void *data);
void pointerfocus(Client *c, LayerSurface* l, struct wlr_surface *surface,
		double sx, double sy, uint32_t time);
void printstatus(void);
void ctlmapped(Client *c);
void api_init(void); void api_start(void); void api_sync(void); void api_exec(char *line, FILE *out); int api_subscribe(int fd);
const Key *api_keybinding(uint32_t mods, xkb_keysym_t sym);
void api_rules(Client *c, const char *appid, const char *title, uint32_t *newtags, Monitor **mon);
void api_monitor_defaults(Monitor *m);
void pointers_reconfigure(void);
void dwl_dumpapi(void);
void powermgrsetmode(struct wl_listener *listener, void *data);
void quit(const Arg *arg);
void rendermon(struct wl_listener *listener, void *data);
void requestdecorationmode(struct wl_listener *listener, void *data);
void requeststartdrag(struct wl_listener *listener, void *data);
void requestmonstate(struct wl_listener *listener, void *data);
void resize(Client *c, struct wlr_box geo, int interact);
void run(char *startup_cmd);
void setcursor(struct wl_listener *listener, void *data);
void setcursorshape(struct wl_listener *listener, void *data);
void setfloating(Client *c, int floating);
void setfullscreen(Client *c, int fullscreen);
void setlayout(const Arg *arg);
void setmfact(const Arg *arg);
void setmon(Client *c, Monitor *m, uint32_t newtags);
void setpsel(struct wl_listener *listener, void *data);
void setsel(struct wl_listener *listener, void *data);
void setup(void);
void spawn(const Arg *arg);
void startdrag(struct wl_listener *listener, void *data);
void tag(const Arg *arg);
void tagmon(const Arg *arg);
void tile(Monitor *m);
void deck(Monitor *m);
void grid(Monitor *m);
void tabbed(Monitor *m);
void stack(Monitor *m);
void monsetlt(Monitor *m, const Layout *l, int all);
void tabs_hide(Monitor *m);
void tabs_refresh(Monitor *m);
int tabs_layout(Monitor *m);
int tabs_wants(Monitor *m, Client *c);
Client *tabs_hit(Monitor *m, double lx, double ly);
void tabs_select(Monitor *m, int delta, int idx);
void tabs_forget(Monitor *m);
extern unsigned int tab_bar_height;
extern char tab_font[];
void togglefloating(const Arg *arg);
void togglefullscreen(const Arg *arg);
void toggletag(const Arg *arg);
void toggleview(const Arg *arg);
void unlocksession(struct wl_listener *listener, void *data);
void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
void unmapnotify(struct wl_listener *listener, void *data);
void updatemons(struct wl_listener *listener, void *data);
void updatetitle(struct wl_listener *listener, void *data);
void urgent(struct wl_listener *listener, void *data);
void view(const Arg *arg);
void shiftview(const Arg *arg);
void tagshiftview(const Arg *arg);
void virtualkeyboard(struct wl_listener *listener, void *data);
void virtualpointer(struct wl_listener *listener, void *data);
Monitor *xytomon(double x, double y);
void xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny);
void zoom(const Arg *arg);

/* not in the original list: used across the split files */
#ifdef XWAYLAND
void activatex11(struct wl_listener *listener, void *data);
void associatex11(struct wl_listener *listener, void *data);
void configurex11(struct wl_listener *listener, void *data);
void createnotifyx11(struct wl_listener *listener, void *data);
void dissociatex11(struct wl_listener *listener, void *data);
void setgeometryx11(struct wl_listener *listener, void *data);
void sethints(struct wl_listener *listener, void *data);
void xwaylandready(struct wl_listener *listener, void *data);
#endif
int fd_set_nonblock(int fd);
xkb_keysym_t keymap_get_one_sym_by_level(struct xkb_keymap *keymap, xkb_keycode_t key, xkb_layout_index_t layout, xkb_level_index_t level);


/* variables (defined in compositor.c) */
extern pid_t child_pid;
extern int locked;
extern void *exclusive_focus;
extern Client *focused_client;
extern struct wl_display *dpy;
extern struct wl_event_loop *event_loop;
extern struct wlr_backend *backend;
extern struct wlr_scene *scene;
extern struct wlr_scene_tree *layers[NUM_LAYERS];
extern struct wlr_scene_tree *drag_icon;
extern const int layermap[4];
extern struct wlr_renderer *drw;
extern struct wlr_allocator *alloc;
extern struct wlr_compositor *compositor;
extern struct wlr_session *session;
extern struct wlr_xdg_shell *xdg_shell;
extern struct wlr_xdg_activation_v1 *activation;
extern struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
extern struct wl_list clients;
extern struct wl_list fstack;
extern struct wlr_idle_notifier_v1 *idle_notifier;
extern struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
extern struct wlr_layer_shell_v1 *layer_shell;
extern struct wlr_output_manager_v1 *output_mgr;
extern struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
extern struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
extern struct wlr_tablet_manager_v2 *tablet_mgr;
extern struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
extern struct wlr_output_power_manager_v1 *power_mgr;
extern struct wlr_ext_foreign_toplevel_list_v1 *foreign_toplevel_list;
extern struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1 *ext_foreign_toplevel_image_capture_source_manager_v1;
extern struct wlr_pointer_constraints_v1 *pointer_constraints;
extern struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
extern struct wlr_pointer_constraint_v1 *active_constraint;
extern struct wlr_cursor *cursor;
extern struct wlr_xcursor_manager *cursor_mgr;
extern struct wlr_scene_rect *root_bg;
extern struct wlr_session_lock_manager_v1 *session_lock_mgr;
extern struct wlr_scene_rect *locked_bg;
extern struct wlr_session_lock_v1 *cur_lock;
extern struct wlr_seat *seat;
extern struct wlr_tearing_control_manager_v1 *tearing_mgr;
extern KeyboardGroup *kb_group;
extern unsigned int cursor_mode;
extern Client *grabc;
extern int grabcx, grabcy;
extern struct wlr_output_layout *output_layout;
extern struct wlr_box sgeom;
extern struct wl_list mons;
extern Monitor *selmon;
extern int consumed[KEY_MAX + 1];
#ifdef XWAYLAND
extern struct wlr_xwayland *xwayland;
#endif
/* config.c / theme.c / keys.c */
extern int legacy_protocols, sloppyfocus, adaptive_sync, tearing_allowed; extern const int bypass_surface_visibility;
extern unsigned int borderpx, snap;
extern float rootcolor[4], bordercolor[4], focuscolor[4], urgentcolor[4]; extern const float fullscreen_bg[4];
extern int log_level;
extern int enable_animation; extern float animation_stiffness, animation_damping;
void fx_place(Client *c, int interact); void fx_tick(Monitor *m); void fx_snap_all(void); void fx_forget(Client *c); int fx_moving_count(void);
extern const Rule rules[]; extern const size_t nrules;
extern const Layout layouts[]; extern const size_t nlayouts;
extern const MonitorRule monrules[]; extern const size_t nmonrules;
extern struct xkb_rule_names xkb_rules;
extern int repeat_rate, repeat_delay;
extern int tap_to_click, tap_and_drag, drag_lock, natural_scrolling, disable_while_typing, left_handed, middle_button_emulation;
extern const enum libinput_config_scroll_method scroll_method;
extern const enum libinput_config_click_method click_method;
extern const uint32_t send_events_mode;
extern const enum libinput_config_accel_profile accel_profile;
extern const double accel_speed;
extern const enum libinput_config_tap_button_map button_map;
extern const Key keys[]; extern const size_t nkeys;
extern const Button buttons[]; extern const size_t nbuttons;
extern const Axis axes[]; extern const size_t naxes;

/* ime.c */
struct input_method_relay;
extern struct wlr_input_method_manager_v2 *input_method_manager;
extern struct wlr_text_input_manager_v3 *text_input_manager;
extern struct input_method_relay *input_method_relay;
int input_method_keyboard_grab_forward_key(KeyboardGroup *keyboard, struct wlr_keyboard_key_event *event);
int input_method_keyboard_grab_forward_modifiers(KeyboardGroup *keyboard);
struct input_method_relay *input_method_relay_create(void);
void input_method_relay_finish(struct input_method_relay *relay);
void input_method_relay_set_focus(struct input_method_relay *relay, struct wlr_surface *surface);

/* settings.c */
void cmdinit(void);
void ctlinit(void);
void settingscleanup(void);

/* fx.c */
void client_set_border_color(Client *c, const float color[static 4]);

#include "client.h"
#endif
