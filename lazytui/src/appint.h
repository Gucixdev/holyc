/* appint.h - the internals of the app layer shared by app.c and the widget kinds registered in kinds.c. Not part of the public API.
 * A kind is a table of callbacks (kind_ops); adding a widget to the spec language is one row in kinds.c and no change to app.c. */
#ifndef LT_APPINT_H
#define LT_APPINT_H
#include "internal.h"

typedef struct anode anode;
struct anode {
	char *kind, *id;
	int line;
	char **ak, **av; int nattr;
	anode *parent, **kid; int nkid;
	lt_node *ln;                     /* the layout node (in the layout that owns this subtree) */
	lt_layout *own;                  /* scroll: the layout of its content */
	int content_h;
	lt_scrollview sv;
	int layout_id;                   /* 0 = main, else which scroll owns it (for origin lookup) */
	anode *scroll;                   /* the nearest scroll ancestor */
	lt_rect rect, inner;             /* screen cells of the last draw */
	bool drawn, focusable;
	int content_y;                   /* top of the node inside its scroll's content (for scroll-into-view) */
	/* widget state */
	lt_buttonbar *bb; lt_input in; bool has_in; char *last_text;
	lt_list *list; lt_tree *tree; lt_dropdown *dd; lt_keybar *keys; int *hint_ids; int nhints;
	lt_pacman_state pst; double last_done;
	lt_zone *zones; char **zkeys; int nzones;
	char **items; int nitems;
	const struct kind_ops *ops;      /* registered kinds (kinds.c): their state lives behind st */
	void *st;
};

typedef struct { char *key; double n; char *s; } mval;
typedef struct { char *id, *kind; lt_app_fn fn; void *ctx; } handler;
typedef struct { char *key, *event; } hotkey;
typedef struct { double every, next; char *event; } timer;
typedef struct { char *type; lt_custom_draw draw; lt_custom_event event; void *ctx; } custom;

struct lt_app {
	anode *root;
	lt_layout *lay;
	mval *m; int nm, capm;
	handler *h; int nh;
	hotkey *hk; int nhk;
	timer *tm; int ntm;
	custom *cu; int ncu;
	anode **foc; int nfoc;           /* focusable nodes in reading order */
	anode *focus;
	anode **hit; int nhit, caphit;   /* interactive nodes drawn this frame, in draw order */
	double time;
	lt_ui *ui;
	bool quit;
	const lt_caps *caps; int held;
	int canvas_w, canvas_h;
	anode *modal, *cap;
	bool dirty, animated;            /* dirty: the model changed since the last tick; animated: something drawn last frame depends on time */
};


typedef struct kind_ops {
	const char *name;
	bool focusable;
	void (*init)(lt_app *a, anode *n);
	void (*destroy)(anode *n);
	void (*draw)(lt_app *a, anode *n, lt_canvas *c, const lt_theme *t, lt_rect w, bool focused);   /* w: the widget's own area; set n->inner to what events should see */
	lt_wres (*event)(lt_app *a, anode *n, const lt_event *ev);
	void (*report)(lt_app *a, anode *n, lt_wres r);                                                 /* CHANGED / ACTIVATE / ...: turn them into model writes + events */
} kind_ops;
const kind_ops *app_kind_ops(const char *name);

const char *app_attr(const anode *n, const char *key);
const char *app_attr_or(const anode *n, const char *key, const char *dflt);
double app_attr_num(const anode *n, const char *key, double dflt);
void app_mark(lt_app *a, anode *n);                       /* make the node mouse-interactive this frame (call from draw) */
int app_canvas_h(const lt_app *a);

#endif
