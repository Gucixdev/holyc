#ifndef HOLYC_IM_GUI_H_
#define HOLYC_IM_GUI_H_

/* Immediate-mode GUI, ported from rxi's microui (MIT, github.com/rxi/microui,
 * ~1500 lines total across microui.h/microui.c). Chosen over Nuklear (also
 * reviewed): same job, but Nuklear is a much larger, more general-purpose
 * library (skinning, multiple backend abstractions, vertex-buffer output) -
 * microui's whole design fits this project's "minimum kompozycji" line
 * better, and its own retained state is already fixed-size stacks/pools,
 * never malloc/free, which is exactly this project's arena/pool philosophy
 * without needing to force an Arena onto anything.
 *
 * API kept faithful to the original (init/begin/end frame, begin/end
 * window, button/checkbox/slider/textbox, layout_row) - this is a mature,
 * well-tested design, not something to redesign on the way through.
 * Renamed mu_/MU_ -> im_gui_/IM_GUI_ and PascalCase struct names to match
 * this project's naming, and mu_Real (configurable float/double) collapsed
 * to plain f32 - this project doesn't need that axis of configurability.
 * Rendering is NOT part of this port: microui emits an opaque command list
 * (rect/text/icon/clip) that a backend walks to actually draw - no backend
 * exists in this project yet (the sokol/opengl backends are still planned), so im_gui_next_command() and the Im_Gui_Command union
 * are as far as this goes; a renderer consumes them, not this file.
 *
 * Known libc dependency, kept deliberately rather than worked around:
 * mu_slider_ex/mu_number_ex's numeric text entry needs float<->string
 * (sprintf/strtod) - print.h has no float formatting at all (a gap noted
 * while porting nn.h), and hand-rolling one just for this would be a
 * detour from the actual port. qsort (root-container z-index sort, at
 * most IM_GUI_ROOTLIST_SIZE items) is standard C89 stdlib too - kept
 * rather than hand-writing an insertion sort for 32 items. */

#include "base.h"

/* own local memcpy_/memset_/strlen_ - own copy per this project's
 * standing "no cross-IMPLEMENTATION-guard dependency" rule, avoiding a
 * <string.h> dependency for a handful of small byte loops. */
static void *im_gui_memcpy_(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

static void *im_gui_memset_(void *dst, i32 val, size_t n)
{
    u8 *d = (u8 *)dst;
    size_t i;
    for (i = 0; i < n; i++) d[i] = (u8)val;
    return dst;
}

static size_t im_gui_strlen_(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

#ifndef IM_GUI_ASSERT
#define IM_GUI_ASSERT HOLYC_ASSERT
#endif

#define IM_GUI_VERSION "2.02" /* upstream microui version this was ported from */

#define IM_GUI_COMMANDLIST_SIZE     (256 * 1024)
#define IM_GUI_ROOTLIST_SIZE        32
#define IM_GUI_CONTAINERSTACK_SIZE  32
#define IM_GUI_CLIPSTACK_SIZE       32
#define IM_GUI_IDSTACK_SIZE         32
#define IM_GUI_LAYOUTSTACK_SIZE     16
#define IM_GUI_CONTAINERPOOL_SIZE   48
#define IM_GUI_TREENODEPOOL_SIZE    48
#define IM_GUI_MAX_WIDTHS           16
#define IM_GUI_REAL_FMT             "%.3g"
#define IM_GUI_SLIDER_FMT           "%.2f"
#define IM_GUI_MAX_FMT              127

#define im_gui_stack(T, n)          struct { i32 idx; T items[n]; }
#define im_gui_min(a, b)            ((a) < (b) ? (a) : (b))
#define im_gui_max(a, b)            ((a) > (b) ? (a) : (b))
#define im_gui_clamp(x, a, b)       im_gui_min(b, im_gui_max(a, x))

enum {
    IM_GUI_CLIP_PART = 1,
    IM_GUI_CLIP_ALL
};

enum {
    IM_GUI_COMMAND_JUMP = 1,
    IM_GUI_COMMAND_CLIP,
    IM_GUI_COMMAND_RECT,
    IM_GUI_COMMAND_TEXT,
    IM_GUI_COMMAND_ICON,
    IM_GUI_COMMAND_MAX
};

enum {
    IM_GUI_COLOR_TEXT,
    IM_GUI_COLOR_BORDER,
    IM_GUI_COLOR_WINDOWBG,
    IM_GUI_COLOR_TITLEBG,
    IM_GUI_COLOR_TITLETEXT,
    IM_GUI_COLOR_PANELBG,
    IM_GUI_COLOR_BUTTON,
    IM_GUI_COLOR_BUTTONHOVER,
    IM_GUI_COLOR_BUTTONFOCUS,
    IM_GUI_COLOR_BASE,
    IM_GUI_COLOR_BASEHOVER,
    IM_GUI_COLOR_BASEFOCUS,
    IM_GUI_COLOR_SCROLLBASE,
    IM_GUI_COLOR_SCROLLTHUMB,
    IM_GUI_COLOR_MAX
};

enum {
    IM_GUI_ICON_CLOSE = 1,
    IM_GUI_ICON_CHECK,
    IM_GUI_ICON_COLLAPSED,
    IM_GUI_ICON_EXPANDED,
    IM_GUI_ICON_MAX
};

enum {
    IM_GUI_RES_ACTIVE = BIT(0),
    IM_GUI_RES_SUBMIT = BIT(1),
    IM_GUI_RES_CHANGE = BIT(2)
};

enum {
    IM_GUI_OPT_ALIGNCENTER = BIT(0),
    IM_GUI_OPT_ALIGNRIGHT  = BIT(1),
    IM_GUI_OPT_NOINTERACT  = BIT(2),
    IM_GUI_OPT_NOFRAME     = BIT(3),
    IM_GUI_OPT_NORESIZE    = BIT(4),
    IM_GUI_OPT_NOSCROLL    = BIT(5),
    IM_GUI_OPT_NOCLOSE     = BIT(6),
    IM_GUI_OPT_NOTITLE     = BIT(7),
    IM_GUI_OPT_HOLDFOCUS   = BIT(8),
    IM_GUI_OPT_AUTOSIZE    = BIT(9),
    IM_GUI_OPT_POPUP       = BIT(10),
    IM_GUI_OPT_CLOSED      = BIT(11),
    IM_GUI_OPT_EXPANDED    = BIT(12)
};

enum {
    IM_GUI_MOUSE_LEFT   = BIT(0),
    IM_GUI_MOUSE_RIGHT  = BIT(1),
    IM_GUI_MOUSE_MIDDLE = BIT(2)
};

enum {
    IM_GUI_KEY_SHIFT     = BIT(0),
    IM_GUI_KEY_CTRL      = BIT(1),
    IM_GUI_KEY_ALT       = BIT(2),
    IM_GUI_KEY_BACKSPACE = BIT(3),
    IM_GUI_KEY_RETURN    = BIT(4)
};

typedef struct Im_Gui Im_Gui;
typedef u32 Im_Gui_Id;
typedef void *Im_Gui_Font;

typedef struct { i32 x, y; } Im_Vec2;
typedef struct { i32 x, y, w, h; } Im_Rect;
typedef struct { u8 r, g, b, a; } Im_Color;
typedef struct { Im_Gui_Id id; i32 last_update; } Im_Gui_Pool_Item;

typedef struct { i32 type, size; } Im_Gui_Base_Command;
typedef struct { Im_Gui_Base_Command base; void *dst; } Im_Gui_Jump_Command;
typedef struct { Im_Gui_Base_Command base; Im_Rect rect; } Im_Gui_Clip_Command;
typedef struct { Im_Gui_Base_Command base; Im_Rect rect; Im_Color color; } Im_Gui_Rect_Command;
typedef struct { Im_Gui_Base_Command base; Im_Gui_Font font; Im_Vec2 pos; Im_Color color; char str[1]; } Im_Gui_Text_Command;
typedef struct { Im_Gui_Base_Command base; Im_Rect rect; i32 id; Im_Color color; } Im_Gui_Icon_Command;

typedef union {
    i32 type;
    Im_Gui_Base_Command base;
    Im_Gui_Jump_Command jump;
    Im_Gui_Clip_Command clip;
    Im_Gui_Rect_Command rect;
    Im_Gui_Text_Command text;
    Im_Gui_Icon_Command icon;
} Im_Gui_Command;

typedef struct {
    Im_Rect body;
    Im_Rect next;
    Im_Vec2 position;
    Im_Vec2 size;
    Im_Vec2 max;
    i32 widths[IM_GUI_MAX_WIDTHS];
    i32 items;
    i32 item_index;
    i32 next_row;
    i32 next_type;
    i32 indent;
} Im_Gui_Layout;

typedef struct {
    Im_Gui_Command *head, *tail;
    Im_Rect rect;
    Im_Rect body;
    Im_Vec2 content_size;
    Im_Vec2 scroll;
    i32 zindex;
    i32 open;
} Im_Gui_Container;

typedef struct {
    Im_Gui_Font font;
    Im_Vec2 size;
    i32 padding;
    i32 spacing;
    i32 indent;
    i32 title_height;
    i32 scrollbar_size;
    i32 thumb_size;
    Im_Color colors[IM_GUI_COLOR_MAX];
} Im_Gui_Style;

struct Im_Gui {
    /* callbacks */
    i32 (*text_width)(Im_Gui_Font font, const char *str, i32 len);
    i32 (*text_height)(Im_Gui_Font font);
    void (*draw_frame)(Im_Gui *ctx, Im_Rect rect, i32 colorid);
    /* core state */
    Im_Gui_Style _style;
    Im_Gui_Style *style;
    Im_Gui_Id hover;
    Im_Gui_Id focus;
    Im_Gui_Id last_id;
    Im_Rect last_rect;
    i32 last_zindex;
    i32 updated_focus;
    i32 frame;
    Im_Gui_Container *hover_root;
    Im_Gui_Container *next_hover_root;
    Im_Gui_Container *scroll_target;
    char number_edit_buf[IM_GUI_MAX_FMT];
    Im_Gui_Id number_edit;
    /* stacks */
    im_gui_stack(char, IM_GUI_COMMANDLIST_SIZE) command_list;
    im_gui_stack(Im_Gui_Container*, IM_GUI_ROOTLIST_SIZE) root_list;
    im_gui_stack(Im_Gui_Container*, IM_GUI_CONTAINERSTACK_SIZE) container_stack;
    im_gui_stack(Im_Rect, IM_GUI_CLIPSTACK_SIZE) clip_stack;
    im_gui_stack(Im_Gui_Id, IM_GUI_IDSTACK_SIZE) id_stack;
    im_gui_stack(Im_Gui_Layout, IM_GUI_LAYOUTSTACK_SIZE) layout_stack;
    /* retained state pools */
    Im_Gui_Pool_Item container_pool[IM_GUI_CONTAINERPOOL_SIZE];
    Im_Gui_Container containers[IM_GUI_CONTAINERPOOL_SIZE];
    Im_Gui_Pool_Item treenode_pool[IM_GUI_TREENODEPOOL_SIZE];
    /* input state */
    Im_Vec2 mouse_pos;
    Im_Vec2 last_mouse_pos;
    Im_Vec2 mouse_delta;
    Im_Vec2 scroll_delta;
    i32 mouse_down;
    i32 mouse_pressed;
    i32 key_down;
    i32 key_pressed;
    char input_text[32];
};

Im_Vec2 im_gui_vec2(i32 x, i32 y);
Im_Rect im_gui_rect(i32 x, i32 y, i32 w, i32 h);
Im_Color im_gui_color(i32 r, i32 g, i32 b, i32 a);

void im_gui_init(Im_Gui *ctx);
void im_gui_begin(Im_Gui *ctx);
void im_gui_end(Im_Gui *ctx);
void im_gui_set_focus(Im_Gui *ctx, Im_Gui_Id id);
Im_Gui_Id im_gui_get_id(Im_Gui *ctx, const void *data, i32 size);
void im_gui_push_id(Im_Gui *ctx, const void *data, i32 size);
void im_gui_pop_id(Im_Gui *ctx);
void im_gui_push_clip_rect(Im_Gui *ctx, Im_Rect rect);
void im_gui_pop_clip_rect(Im_Gui *ctx);
Im_Rect im_gui_get_clip_rect(Im_Gui *ctx);
i32 im_gui_check_clip(Im_Gui *ctx, Im_Rect r);
Im_Gui_Container *im_gui_get_current_container(Im_Gui *ctx);
Im_Gui_Container *im_gui_get_container(Im_Gui *ctx, const char *name);
void im_gui_bring_to_front(Im_Gui *ctx, Im_Gui_Container *cnt);

i32 im_gui_pool_init(Im_Gui *ctx, Im_Gui_Pool_Item *items, i32 len, Im_Gui_Id id);
i32 im_gui_pool_get(Im_Gui *ctx, Im_Gui_Pool_Item *items, i32 len, Im_Gui_Id id);
void im_gui_pool_update(Im_Gui *ctx, Im_Gui_Pool_Item *items, i32 idx);

void im_gui_input_mousemove(Im_Gui *ctx, i32 x, i32 y);
void im_gui_input_mousedown(Im_Gui *ctx, i32 x, i32 y, i32 btn);
void im_gui_input_mouseup(Im_Gui *ctx, i32 x, i32 y, i32 btn);
void im_gui_input_scroll(Im_Gui *ctx, i32 x, i32 y);
void im_gui_input_keydown(Im_Gui *ctx, i32 key);
void im_gui_input_keyup(Im_Gui *ctx, i32 key);
void im_gui_input_text(Im_Gui *ctx, const char *text);

Im_Gui_Command *im_gui_push_command(Im_Gui *ctx, i32 type, i32 size);
i32 im_gui_next_command(Im_Gui *ctx, Im_Gui_Command **cmd);
void im_gui_set_clip(Im_Gui *ctx, Im_Rect rect);
void im_gui_draw_rect(Im_Gui *ctx, Im_Rect rect, Im_Color color);
void im_gui_draw_box(Im_Gui *ctx, Im_Rect rect, Im_Color color);
void im_gui_draw_text(Im_Gui *ctx, Im_Gui_Font font, const char *str, i32 len, Im_Vec2 pos, Im_Color color);
void im_gui_draw_icon(Im_Gui *ctx, i32 id, Im_Rect rect, Im_Color color);

void im_gui_layout_row(Im_Gui *ctx, i32 items, const i32 *widths, i32 height);
void im_gui_layout_width(Im_Gui *ctx, i32 width);
void im_gui_layout_height(Im_Gui *ctx, i32 height);
void im_gui_layout_begin_column(Im_Gui *ctx);
void im_gui_layout_end_column(Im_Gui *ctx);
void im_gui_layout_set_next(Im_Gui *ctx, Im_Rect r, i32 relative);
Im_Rect im_gui_layout_next(Im_Gui *ctx);

void im_gui_draw_control_frame(Im_Gui *ctx, Im_Gui_Id id, Im_Rect rect, i32 colorid, i32 opt);
void im_gui_draw_control_text(Im_Gui *ctx, const char *str, Im_Rect rect, i32 colorid, i32 opt);
i32 im_gui_mouse_over(Im_Gui *ctx, Im_Rect rect);
void im_gui_update_control(Im_Gui *ctx, Im_Gui_Id id, Im_Rect rect, i32 opt);

#define im_gui_button(ctx, label)             im_gui_button_ex(ctx, label, 0, IM_GUI_OPT_ALIGNCENTER)
#define im_gui_textbox(ctx, buf, bufsz)        im_gui_textbox_ex(ctx, buf, bufsz, 0)
#define im_gui_slider(ctx, value, lo, hi)      im_gui_slider_ex(ctx, value, lo, hi, 0, IM_GUI_SLIDER_FMT, IM_GUI_OPT_ALIGNCENTER)
#define im_gui_number(ctx, value, step)        im_gui_number_ex(ctx, value, step, IM_GUI_SLIDER_FMT, IM_GUI_OPT_ALIGNCENTER)
#define im_gui_header(ctx, label)              im_gui_header_ex(ctx, label, 0)
#define im_gui_begin_treenode(ctx, label)      im_gui_begin_treenode_ex(ctx, label, 0)
#define im_gui_begin_window(ctx, title, rect)  im_gui_begin_window_ex(ctx, title, rect, 0)
#define im_gui_begin_panel(ctx, name)          im_gui_begin_panel_ex(ctx, name, 0)

void im_gui_text(Im_Gui *ctx, const char *text);
void im_gui_label(Im_Gui *ctx, const char *text);
i32 im_gui_button_ex(Im_Gui *ctx, const char *label, i32 icon, i32 opt);
i32 im_gui_checkbox(Im_Gui *ctx, const char *label, i32 *state);
i32 im_gui_textbox_raw(Im_Gui *ctx, char *buf, i32 bufsz, Im_Gui_Id id, Im_Rect r, i32 opt);
i32 im_gui_textbox_ex(Im_Gui *ctx, char *buf, i32 bufsz, i32 opt);
i32 im_gui_slider_ex(Im_Gui *ctx, f32 *value, f32 low, f32 high, f32 step, const char *fmt, i32 opt);
i32 im_gui_number_ex(Im_Gui *ctx, f32 *value, f32 step, const char *fmt, i32 opt);
i32 im_gui_header_ex(Im_Gui *ctx, const char *label, i32 opt);
i32 im_gui_begin_treenode_ex(Im_Gui *ctx, const char *label, i32 opt);
void im_gui_end_treenode(Im_Gui *ctx);
i32 im_gui_begin_window_ex(Im_Gui *ctx, const char *title, Im_Rect rect, i32 opt);
void im_gui_end_window(Im_Gui *ctx);
void im_gui_open_popup(Im_Gui *ctx, const char *name);
i32 im_gui_begin_popup(Im_Gui *ctx, const char *name);
void im_gui_end_popup(Im_Gui *ctx);
void im_gui_begin_panel_ex(Im_Gui *ctx, const char *name, i32 opt);
void im_gui_end_panel(Im_Gui *ctx);

#endif /* HOLYC_IM_GUI_H_ */

#ifdef IM_GUI_IMPLEMENTATION
#ifndef IM_GUI_IMPLEMENTATION_
#define IM_GUI_IMPLEMENTATION_

#include <stdio.h>
#include <stdlib.h>


#define im_gui_push(stk, val) do {                                                \
        IM_GUI_ASSERT((stk).idx < (i32)(sizeof((stk).items) / sizeof(*(stk).items))); \
        (stk).items[(stk).idx] = (val);                                             \
        (stk).idx++;                                                                \
    } while (0)

#define im_gui_pop(stk) do {         \
        IM_GUI_ASSERT((stk).idx > 0); \
        (stk).idx--;                  \
    } while (0)

static Im_Rect im_gui_unclipped_rect_ = { 0, 0, 0x1000000, 0x1000000 };

static Im_Gui_Style im_gui_default_style_ = {
    NULL, { 68, 10 }, 5, 4, 24,
    24, 12, 8,
    {
        { 230, 230, 230, 255 }, /* IM_GUI_COLOR_TEXT */
        { 25,  25,  25,  255 }, /* IM_GUI_COLOR_BORDER */
        { 50,  50,  50,  255 }, /* IM_GUI_COLOR_WINDOWBG */
        { 25,  25,  25,  255 }, /* IM_GUI_COLOR_TITLEBG */
        { 240, 240, 240, 255 }, /* IM_GUI_COLOR_TITLETEXT */
        { 0,   0,   0,   0   }, /* IM_GUI_COLOR_PANELBG */
        { 75,  75,  75,  255 }, /* IM_GUI_COLOR_BUTTON */
        { 95,  95,  95,  255 }, /* IM_GUI_COLOR_BUTTONHOVER */
        { 115, 115, 115, 255 }, /* IM_GUI_COLOR_BUTTONFOCUS */
        { 30,  30,  30,  255 }, /* IM_GUI_COLOR_BASE */
        { 35,  35,  35,  255 }, /* IM_GUI_COLOR_BASEHOVER */
        { 40,  40,  40,  255 }, /* IM_GUI_COLOR_BASEFOCUS */
        { 43,  43,  43,  255 }, /* IM_GUI_COLOR_SCROLLBASE */
        { 30,  30,  30,  255 }  /* IM_GUI_COLOR_SCROLLTHUMB */
    }
};

Im_Vec2 im_gui_vec2(i32 x, i32 y)
{
    Im_Vec2 res;
    res.x = x; res.y = y;
    return res;
}

Im_Rect im_gui_rect(i32 x, i32 y, i32 w, i32 h)
{
    Im_Rect res;
    res.x = x; res.y = y; res.w = w; res.h = h;
    return res;
}

Im_Color im_gui_color(i32 r, i32 g, i32 b, i32 a)
{
    Im_Color res;
    res.r = (u8)r; res.g = (u8)g; res.b = (u8)b; res.a = (u8)a;
    return res;
}

static Im_Rect im_gui_expand_rect_(Im_Rect rect, i32 n)
{
    return im_gui_rect(rect.x - n, rect.y - n, rect.w + n * 2, rect.h + n * 2);
}

static Im_Rect im_gui_intersect_rects_(Im_Rect r1, Im_Rect r2)
{
    i32 x1 = im_gui_max(r1.x, r2.x);
    i32 y1 = im_gui_max(r1.y, r2.y);
    i32 x2 = im_gui_max(x1, im_gui_min(r1.x + r1.w, r2.x + r2.w));
    i32 y2 = im_gui_max(y1, im_gui_min(r1.y + r1.h, r2.y + r2.h));
    return im_gui_rect(x1, y1, x2 - x1, y2 - y1);
}

static i32 im_gui_rect_overlaps_vec2_(Im_Rect r, Im_Vec2 p)
{
    return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h;
}

static void im_gui_draw_frame_(Im_Gui *ctx, Im_Rect rect, i32 colorid)
{
    im_gui_draw_rect(ctx, rect, ctx->style->colors[colorid]);
    if (colorid == IM_GUI_COLOR_SCROLLBASE ||
        colorid == IM_GUI_COLOR_SCROLLTHUMB ||
        colorid == IM_GUI_COLOR_TITLEBG) { return; }
    if (ctx->style->colors[IM_GUI_COLOR_BORDER].a) {
        im_gui_draw_box(ctx, im_gui_expand_rect_(rect, 1), ctx->style->colors[IM_GUI_COLOR_BORDER]);
    }
}

void im_gui_init(Im_Gui *ctx)
{
    im_gui_memset_(ctx, 0, sizeof(*ctx));
    ctx->draw_frame = im_gui_draw_frame_;
    ctx->_style = im_gui_default_style_;
    ctx->style = &ctx->_style;
}

void im_gui_begin(Im_Gui *ctx)
{
    IM_GUI_ASSERT(ctx->text_width && ctx->text_height);
    ctx->command_list.idx = 0;
    ctx->root_list.idx = 0;
    ctx->scroll_target = NULL;
    ctx->hover_root = ctx->next_hover_root;
    ctx->next_hover_root = NULL;
    ctx->mouse_delta.x = ctx->mouse_pos.x - ctx->last_mouse_pos.x;
    ctx->mouse_delta.y = ctx->mouse_pos.y - ctx->last_mouse_pos.y;
    ctx->frame++;
}

static i32 im_gui_compare_zindex_(const void *a, const void *b)
{
    return (*(Im_Gui_Container **)a)->zindex - (*(Im_Gui_Container **)b)->zindex;
}

void im_gui_end(Im_Gui *ctx)
{
    i32 i, n;

    IM_GUI_ASSERT(ctx->container_stack.idx == 0);
    IM_GUI_ASSERT(ctx->clip_stack.idx      == 0);
    IM_GUI_ASSERT(ctx->id_stack.idx        == 0);
    IM_GUI_ASSERT(ctx->layout_stack.idx    == 0);

    if (ctx->scroll_target) {
        ctx->scroll_target->scroll.x += ctx->scroll_delta.x;
        ctx->scroll_target->scroll.y += ctx->scroll_delta.y;
    }

    if (!ctx->updated_focus) { ctx->focus = 0; }
    ctx->updated_focus = 0;

    if (ctx->mouse_pressed && ctx->next_hover_root &&
        ctx->next_hover_root->zindex < ctx->last_zindex &&
        ctx->next_hover_root->zindex >= 0
    ) {
        im_gui_bring_to_front(ctx, ctx->next_hover_root);
    }

    ctx->key_pressed = 0;
    ctx->input_text[0] = '\0';
    ctx->mouse_pressed = 0;
    ctx->scroll_delta = im_gui_vec2(0, 0);
    ctx->last_mouse_pos = ctx->mouse_pos;

    n = ctx->root_list.idx;
    qsort(ctx->root_list.items, (size_t)n, sizeof(Im_Gui_Container *), im_gui_compare_zindex_);

    for (i = 0; i < n; i++) {
        Im_Gui_Container *cnt = ctx->root_list.items[i];
        if (i == 0) {
            Im_Gui_Command *cmd = (Im_Gui_Command *)ctx->command_list.items;
            cmd->jump.dst = (char *)cnt->head + sizeof(Im_Gui_Jump_Command);
        } else {
            Im_Gui_Container *prev = ctx->root_list.items[i - 1];
            prev->tail->jump.dst = (char *)cnt->head + sizeof(Im_Gui_Jump_Command);
        }
        if (i == n - 1) {
            cnt->tail->jump.dst = ctx->command_list.items + ctx->command_list.idx;
        }
    }
}

void im_gui_set_focus(Im_Gui *ctx, Im_Gui_Id id)
{
    ctx->focus = id;
    ctx->updated_focus = 1;
}

#define IM_GUI_HASH_INITIAL_ 2166136261u

static void im_gui_hash_(Im_Gui_Id *hash, const void *data, i32 size)
{
    const u8 *p = (const u8 *)data;
    while (size--) {
        *hash = (*hash ^ *p++) * 16777619u;
    }
}

Im_Gui_Id im_gui_get_id(Im_Gui *ctx, const void *data, i32 size)
{
    i32 idx = ctx->id_stack.idx;
    Im_Gui_Id res = (idx > 0) ? ctx->id_stack.items[idx - 1] : IM_GUI_HASH_INITIAL_;
    im_gui_hash_(&res, data, size);
    ctx->last_id = res;
    return res;
}

void im_gui_push_id(Im_Gui *ctx, const void *data, i32 size)
{
    im_gui_push(ctx->id_stack, im_gui_get_id(ctx, data, size));
}

void im_gui_pop_id(Im_Gui *ctx)
{
    im_gui_pop(ctx->id_stack);
}

void im_gui_push_clip_rect(Im_Gui *ctx, Im_Rect rect)
{
    Im_Rect last = im_gui_get_clip_rect(ctx);
    im_gui_push(ctx->clip_stack, im_gui_intersect_rects_(rect, last));
}

void im_gui_pop_clip_rect(Im_Gui *ctx)
{
    im_gui_pop(ctx->clip_stack);
}

Im_Rect im_gui_get_clip_rect(Im_Gui *ctx)
{
    IM_GUI_ASSERT(ctx->clip_stack.idx > 0);
    return ctx->clip_stack.items[ctx->clip_stack.idx - 1];
}

i32 im_gui_check_clip(Im_Gui *ctx, Im_Rect r)
{
    Im_Rect cr = im_gui_get_clip_rect(ctx);
    /* "fully outside" and "fully inside" the same clip rect are mutually
     * exclusive for any pair of rects, so summing their (0/1) indicators
     * against their distinct return values is exact, not approximate. */
    i32 all = r.x > cr.x + cr.w || r.x + r.w < cr.x ||
              r.y > cr.y + cr.h || r.y + r.h < cr.y;
    i32 none = r.x >= cr.x && r.x + r.w <= cr.x + cr.w &&
               r.y >= cr.y && r.y + r.h <= cr.y + cr.h;
    return all * IM_GUI_CLIP_ALL + (1 - all) * (1 - none) * IM_GUI_CLIP_PART;
}

static void im_gui_push_layout_(Im_Gui *ctx, Im_Rect body, Im_Vec2 scroll)
{
    Im_Gui_Layout layout;
    i32 width = 0;
    im_gui_memset_(&layout, 0, sizeof(layout));
    layout.body = im_gui_rect(body.x - scroll.x, body.y - scroll.y, body.w, body.h);
    layout.max = im_gui_vec2(-0x1000000, -0x1000000);
    im_gui_push(ctx->layout_stack, layout);
    im_gui_layout_row(ctx, 1, &width, 0);
}

static Im_Gui_Layout *im_gui_get_layout_(Im_Gui *ctx)
{
    return &ctx->layout_stack.items[ctx->layout_stack.idx - 1];
}

static void im_gui_pop_container_(Im_Gui *ctx)
{
    Im_Gui_Container *cnt = im_gui_get_current_container(ctx);
    Im_Gui_Layout *layout = im_gui_get_layout_(ctx);
    cnt->content_size.x = layout->max.x - layout->body.x;
    cnt->content_size.y = layout->max.y - layout->body.y;
    im_gui_pop(ctx->container_stack);
    im_gui_pop(ctx->layout_stack);
    im_gui_pop_id(ctx);
}

Im_Gui_Container *im_gui_get_current_container(Im_Gui *ctx)
{
    IM_GUI_ASSERT(ctx->container_stack.idx > 0);
    return ctx->container_stack.items[ctx->container_stack.idx - 1];
}

static Im_Gui_Container *im_gui_get_container_(Im_Gui *ctx, Im_Gui_Id id, i32 opt)
{
    Im_Gui_Container *cnt;
    i32 idx = im_gui_pool_get(ctx, ctx->container_pool, IM_GUI_CONTAINERPOOL_SIZE, id);
    if (idx >= 0) {
        if (ctx->containers[idx].open || ~opt & IM_GUI_OPT_CLOSED) {
            im_gui_pool_update(ctx, ctx->container_pool, idx);
        }
        return &ctx->containers[idx];
    }
    if (opt & IM_GUI_OPT_CLOSED) { return NULL; }
    idx = im_gui_pool_init(ctx, ctx->container_pool, IM_GUI_CONTAINERPOOL_SIZE, id);
    cnt = &ctx->containers[idx];
    im_gui_memset_(cnt, 0, sizeof(*cnt));
    cnt->open = 1;
    im_gui_bring_to_front(ctx, cnt);
    return cnt;
}

Im_Gui_Container *im_gui_get_container(Im_Gui *ctx, const char *name)
{
    Im_Gui_Id id = im_gui_get_id(ctx, name, (i32)im_gui_strlen_(name));
    return im_gui_get_container_(ctx, id, 0);
}

void im_gui_bring_to_front(Im_Gui *ctx, Im_Gui_Container *cnt)
{
    cnt->zindex = ++ctx->last_zindex;
}

/* ---- pool ---- */

i32 im_gui_pool_init(Im_Gui *ctx, Im_Gui_Pool_Item *items, i32 len, Im_Gui_Id id)
{
    i32 i, n = -1, f = ctx->frame;
    for (i = 0; i < len; i++) {
        if (items[i].last_update < f) {
            f = items[i].last_update;
            n = i;
        }
    }
    IM_GUI_ASSERT(n > -1);
    items[n].id = id;
    im_gui_pool_update(ctx, items, n);
    return n;
}

i32 im_gui_pool_get(Im_Gui *ctx, Im_Gui_Pool_Item *items, i32 len, Im_Gui_Id id)
{
    i32 i;
    (void)ctx;
    for (i = 0; i < len; i++) {
        if (items[i].id == id) { return i; }
    }
    return -1;
}

void im_gui_pool_update(Im_Gui *ctx, Im_Gui_Pool_Item *items, i32 idx)
{
    items[idx].last_update = ctx->frame;
}

/* ---- input handlers ---- */

void im_gui_input_mousemove(Im_Gui *ctx, i32 x, i32 y)
{
    ctx->mouse_pos = im_gui_vec2(x, y);
}

void im_gui_input_mousedown(Im_Gui *ctx, i32 x, i32 y, i32 btn)
{
    im_gui_input_mousemove(ctx, x, y);
    ctx->mouse_down |= btn;
    ctx->mouse_pressed |= btn;
}

void im_gui_input_mouseup(Im_Gui *ctx, i32 x, i32 y, i32 btn)
{
    im_gui_input_mousemove(ctx, x, y);
    ctx->mouse_down &= ~btn;
}

void im_gui_input_scroll(Im_Gui *ctx, i32 x, i32 y)
{
    ctx->scroll_delta.x += x;
    ctx->scroll_delta.y += y;
}

void im_gui_input_keydown(Im_Gui *ctx, i32 key)
{
    ctx->key_pressed |= key;
    ctx->key_down |= key;
}

void im_gui_input_keyup(Im_Gui *ctx, i32 key)
{
    ctx->key_down &= ~key;
}

void im_gui_input_text(Im_Gui *ctx, const char *text)
{
    size_t len = im_gui_strlen_(ctx->input_text);
    size_t size = im_gui_strlen_(text) + 1;
    IM_GUI_ASSERT(len + size <= sizeof(ctx->input_text));
    im_gui_memcpy_(ctx->input_text + len, text, size);
}

/* ---- command list ---- */

Im_Gui_Command *im_gui_push_command(Im_Gui *ctx, i32 type, i32 size)
{
    Im_Gui_Command *cmd = (Im_Gui_Command *)(ctx->command_list.items + ctx->command_list.idx);
    IM_GUI_ASSERT(ctx->command_list.idx + size < IM_GUI_COMMANDLIST_SIZE);
    cmd->base.type = type;
    cmd->base.size = size;
    ctx->command_list.idx += size;
    return cmd;
}

i32 im_gui_next_command(Im_Gui *ctx, Im_Gui_Command **cmd)
{
    if (*cmd) {
        *cmd = (Im_Gui_Command *)(((char *)*cmd) + (*cmd)->base.size);
    } else {
        *cmd = (Im_Gui_Command *)ctx->command_list.items;
    }
    while ((char *)*cmd != ctx->command_list.items + ctx->command_list.idx) {
        if ((*cmd)->type != IM_GUI_COMMAND_JUMP) { return 1; }
        *cmd = (Im_Gui_Command *)(*cmd)->jump.dst;
    }
    return 0;
}

static Im_Gui_Command *im_gui_push_jump_(Im_Gui *ctx, Im_Gui_Command *dst)
{
    Im_Gui_Command *cmd;
    cmd = im_gui_push_command(ctx, IM_GUI_COMMAND_JUMP, sizeof(Im_Gui_Jump_Command));
    cmd->jump.dst = dst;
    return cmd;
}

void im_gui_set_clip(Im_Gui *ctx, Im_Rect rect)
{
    Im_Gui_Command *cmd;
    cmd = im_gui_push_command(ctx, IM_GUI_COMMAND_CLIP, sizeof(Im_Gui_Clip_Command));
    cmd->clip.rect = rect;
}

void im_gui_draw_rect(Im_Gui *ctx, Im_Rect rect, Im_Color color)
{
    Im_Gui_Command *cmd;
    rect = im_gui_intersect_rects_(rect, im_gui_get_clip_rect(ctx));
    if (rect.w > 0 && rect.h > 0) {
        cmd = im_gui_push_command(ctx, IM_GUI_COMMAND_RECT, sizeof(Im_Gui_Rect_Command));
        cmd->rect.rect = rect;
        cmd->rect.color = color;
    }
}

void im_gui_draw_box(Im_Gui *ctx, Im_Rect rect, Im_Color color)
{
    im_gui_draw_rect(ctx, im_gui_rect(rect.x + 1, rect.y, rect.w - 2, 1), color);
    im_gui_draw_rect(ctx, im_gui_rect(rect.x + 1, rect.y + rect.h - 1, rect.w - 2, 1), color);
    im_gui_draw_rect(ctx, im_gui_rect(rect.x, rect.y, 1, rect.h), color);
    im_gui_draw_rect(ctx, im_gui_rect(rect.x + rect.w - 1, rect.y, 1, rect.h), color);
}

void im_gui_draw_text(Im_Gui *ctx, Im_Gui_Font font, const char *str, i32 len,
                       Im_Vec2 pos, Im_Color color)
{
    Im_Gui_Command *cmd;
    Im_Rect rect;
    i32 clipped;

    if (len < 0) { len = (i32)im_gui_strlen_(str); }
    rect = im_gui_rect(pos.x, pos.y, ctx->text_width(font, str, len), ctx->text_height(font));
    clipped = im_gui_check_clip(ctx, rect);
    if (clipped == IM_GUI_CLIP_ALL) { return; }
    if (clipped == IM_GUI_CLIP_PART) { im_gui_set_clip(ctx, im_gui_get_clip_rect(ctx)); }

    cmd = im_gui_push_command(ctx, IM_GUI_COMMAND_TEXT, (i32)sizeof(Im_Gui_Text_Command) + len);
    im_gui_memcpy_(cmd->text.str, str, (size_t)len);
    cmd->text.str[len] = '\0';
    cmd->text.pos = pos;
    cmd->text.color = color;
    cmd->text.font = font;
    if (clipped) { im_gui_set_clip(ctx, im_gui_unclipped_rect_); }
}

void im_gui_draw_icon(Im_Gui *ctx, i32 id, Im_Rect rect, Im_Color color)
{
    Im_Gui_Command *cmd;
    i32 clipped = im_gui_check_clip(ctx, rect);
    if (clipped == IM_GUI_CLIP_ALL) { return; }
    if (clipped == IM_GUI_CLIP_PART) { im_gui_set_clip(ctx, im_gui_get_clip_rect(ctx)); }
    cmd = im_gui_push_command(ctx, IM_GUI_COMMAND_ICON, sizeof(Im_Gui_Icon_Command));
    cmd->icon.id = id;
    cmd->icon.rect = rect;
    cmd->icon.color = color;
    if (clipped) { im_gui_set_clip(ctx, im_gui_unclipped_rect_); }
}

/* ---- layout ---- */

enum { IM_GUI_RELATIVE_ = 1, IM_GUI_ABSOLUTE_ = 2 };

void im_gui_layout_begin_column(Im_Gui *ctx)
{
    im_gui_push_layout_(ctx, im_gui_layout_next(ctx), im_gui_vec2(0, 0));
}

void im_gui_layout_end_column(Im_Gui *ctx)
{
    Im_Gui_Layout *a, *b;
    b = im_gui_get_layout_(ctx);
    im_gui_pop(ctx->layout_stack);
    a = im_gui_get_layout_(ctx);
    a->position.x = im_gui_max(a->position.x, b->position.x + b->body.x - a->body.x);
    a->next_row = im_gui_max(a->next_row, b->next_row + b->body.y - a->body.y);
    a->max.x = im_gui_max(a->max.x, b->max.x);
    a->max.y = im_gui_max(a->max.y, b->max.y);
}

void im_gui_layout_row(Im_Gui *ctx, i32 items, const i32 *widths, i32 height)
{
    Im_Gui_Layout *layout = im_gui_get_layout_(ctx);
    if (widths) {
        IM_GUI_ASSERT(items <= IM_GUI_MAX_WIDTHS);
        im_gui_memcpy_(layout->widths, widths, (size_t)items * sizeof(widths[0]));
    }
    layout->items = items;
    layout->position = im_gui_vec2(layout->indent, layout->next_row);
    layout->size.y = height;
    layout->item_index = 0;
}

void im_gui_layout_width(Im_Gui *ctx, i32 width)
{
    im_gui_get_layout_(ctx)->size.x = width;
}

void im_gui_layout_height(Im_Gui *ctx, i32 height)
{
    im_gui_get_layout_(ctx)->size.y = height;
}

void im_gui_layout_set_next(Im_Gui *ctx, Im_Rect r, i32 relative)
{
    Im_Gui_Layout *layout = im_gui_get_layout_(ctx);
    layout->next = r;
    layout->next_type = relative ? IM_GUI_RELATIVE_ : IM_GUI_ABSOLUTE_;
}

Im_Rect im_gui_layout_next(Im_Gui *ctx)
{
    Im_Gui_Layout *layout = im_gui_get_layout_(ctx);
    Im_Gui_Style *style = ctx->style;
    Im_Rect res;

    if (layout->next_type) {
        i32 type = layout->next_type;
        layout->next_type = 0;
        res = layout->next;
        if (type == IM_GUI_ABSOLUTE_) { return (ctx->last_rect = res); }
    } else {
        if (layout->item_index == layout->items) {
            im_gui_layout_row(ctx, layout->items, NULL, layout->size.y);
        }

        res.x = layout->position.x;
        res.y = layout->position.y;

        res.w = layout->items > 0 ? layout->widths[layout->item_index] : layout->size.x;
        res.h = layout->size.y;
        if (res.w == 0) { res.w = style->size.x + style->padding * 2; }
        if (res.h == 0) { res.h = style->size.y + style->padding * 2; }
        if (res.w < 0) { res.w += layout->body.w - res.x + 1; }
        if (res.h < 0) { res.h += layout->body.h - res.y + 1; }

        layout->item_index++;
    }

    layout->position.x += res.w + style->spacing;
    layout->next_row = im_gui_max(layout->next_row, res.y + res.h + style->spacing);

    res.x += layout->body.x;
    res.y += layout->body.y;

    layout->max.x = im_gui_max(layout->max.x, res.x + res.w);
    layout->max.y = im_gui_max(layout->max.y, res.y + res.h);

    return (ctx->last_rect = res);
}

/* ---- controls ---- */

static i32 im_gui_in_hover_root_(Im_Gui *ctx)
{
    i32 i = ctx->container_stack.idx;
    while (i--) {
        if (ctx->container_stack.items[i] == ctx->hover_root) { return 1; }
        if (ctx->container_stack.items[i]->head) { break; }
    }
    return 0;
}

void im_gui_draw_control_frame(Im_Gui *ctx, Im_Gui_Id id, Im_Rect rect, i32 colorid, i32 opt)
{
    if (opt & IM_GUI_OPT_NOFRAME) { return; }
    colorid += (ctx->focus == id) ? 2 : (ctx->hover == id) ? 1 : 0;
    ctx->draw_frame(ctx, rect, colorid);
}

void im_gui_draw_control_text(Im_Gui *ctx, const char *str, Im_Rect rect, i32 colorid, i32 opt)
{
    Im_Vec2 pos;
    Im_Gui_Font font = ctx->style->font;
    i32 tw = ctx->text_width(font, str, -1);
    im_gui_push_clip_rect(ctx, rect);
    pos.y = rect.y + (rect.h - ctx->text_height(font)) / 2;
    switch (opt & (IM_GUI_OPT_ALIGNCENTER | IM_GUI_OPT_ALIGNRIGHT)) {
    case IM_GUI_OPT_ALIGNCENTER:
    case (IM_GUI_OPT_ALIGNCENTER | IM_GUI_OPT_ALIGNRIGHT): /* both set: center wins, same priority the old if-first order gave it */
        pos.x = rect.x + (rect.w - tw) / 2;
        break;
    case IM_GUI_OPT_ALIGNRIGHT:
        pos.x = rect.x + rect.w - tw - ctx->style->padding;
        break;
    default:
        pos.x = rect.x + ctx->style->padding;
        break;
    }
    im_gui_draw_text(ctx, font, str, -1, pos, ctx->style->colors[colorid]);
    im_gui_pop_clip_rect(ctx);
}

i32 im_gui_mouse_over(Im_Gui *ctx, Im_Rect rect)
{
    return im_gui_rect_overlaps_vec2_(rect, ctx->mouse_pos) &&
           im_gui_rect_overlaps_vec2_(im_gui_get_clip_rect(ctx), ctx->mouse_pos) &&
           im_gui_in_hover_root_(ctx);
}

void im_gui_update_control(Im_Gui *ctx, Im_Gui_Id id, Im_Rect rect, i32 opt)
{
    i32 mouseover = im_gui_mouse_over(ctx, rect);

    if (ctx->focus == id) { ctx->updated_focus = 1; }
    if (opt & IM_GUI_OPT_NOINTERACT) { return; }
    if (mouseover && !ctx->mouse_down) { ctx->hover = id; }

    if (ctx->focus == id) {
        if (ctx->mouse_pressed && !mouseover) { im_gui_set_focus(ctx, 0); }
        if (!ctx->mouse_down && ~opt & IM_GUI_OPT_HOLDFOCUS) { im_gui_set_focus(ctx, 0); }
    }

    if (ctx->hover == id) {
        if (ctx->mouse_pressed) {
            im_gui_set_focus(ctx, id);
        } else if (!mouseover) {
            ctx->hover = 0;
        }
    }
}

void im_gui_text(Im_Gui *ctx, const char *text)
{
    const char *start, *end, *p = text;
    i32 width = -1;
    Im_Gui_Font font = ctx->style->font;
    Im_Color color = ctx->style->colors[IM_GUI_COLOR_TEXT];
    im_gui_layout_begin_column(ctx);
    im_gui_layout_row(ctx, 1, &width, ctx->text_height(font));
    do {
        Im_Rect r = im_gui_layout_next(ctx);
        i32 w = 0;
        start = end = p;
        do {
            const char *word = p;
            while (*p && *p != ' ' && *p != '\n') { p++; }
            w += ctx->text_width(font, word, (i32)(p - word));
            if (w > r.w && end != start) { break; }
            w += ctx->text_width(font, p, 1);
            end = p++;
        } while (*end && *end != '\n');
        im_gui_draw_text(ctx, font, start, (i32)(end - start), im_gui_vec2(r.x, r.y), color);
        p = end + 1;
    } while (*end);
    im_gui_layout_end_column(ctx);
}

void im_gui_label(Im_Gui *ctx, const char *text)
{
    im_gui_draw_control_text(ctx, text, im_gui_layout_next(ctx), IM_GUI_COLOR_TEXT, 0);
}

i32 im_gui_button_ex(Im_Gui *ctx, const char *label, i32 icon, i32 opt)
{
    i32 res = 0;
    Im_Gui_Id id = label ? im_gui_get_id(ctx, label, (i32)im_gui_strlen_(label))
                         : im_gui_get_id(ctx, &icon, sizeof(icon));
    Im_Rect r = im_gui_layout_next(ctx);
    im_gui_update_control(ctx, id, r, opt);
    if (ctx->mouse_pressed == IM_GUI_MOUSE_LEFT && ctx->focus == id) {
        res |= IM_GUI_RES_SUBMIT;
    }
    im_gui_draw_control_frame(ctx, id, r, IM_GUI_COLOR_BUTTON, opt);
    if (label) { im_gui_draw_control_text(ctx, label, r, IM_GUI_COLOR_TEXT, opt); }
    if (icon) { im_gui_draw_icon(ctx, icon, r, ctx->style->colors[IM_GUI_COLOR_TEXT]); }
    return res;
}

i32 im_gui_checkbox(Im_Gui *ctx, const char *label, i32 *state)
{
    i32 res = 0;
    Im_Gui_Id id = im_gui_get_id(ctx, &state, sizeof(state));
    Im_Rect r = im_gui_layout_next(ctx);
    Im_Rect box = im_gui_rect(r.x, r.y, r.h, r.h);
    im_gui_update_control(ctx, id, r, 0);
    if (ctx->mouse_pressed == IM_GUI_MOUSE_LEFT && ctx->focus == id) {
        res |= IM_GUI_RES_CHANGE;
        *state = !*state;
    }
    im_gui_draw_control_frame(ctx, id, box, IM_GUI_COLOR_BASE, 0);
    if (*state) {
        im_gui_draw_icon(ctx, IM_GUI_ICON_CHECK, box, ctx->style->colors[IM_GUI_COLOR_TEXT]);
    }
    r = im_gui_rect(r.x + box.w, r.y, r.w - box.w, r.h);
    im_gui_draw_control_text(ctx, label, r, IM_GUI_COLOR_TEXT, 0);
    return res;
}

i32 im_gui_textbox_raw(Im_Gui *ctx, char *buf, i32 bufsz, Im_Gui_Id id, Im_Rect r, i32 opt)
{
    i32 res = 0;
    im_gui_update_control(ctx, id, r, opt | IM_GUI_OPT_HOLDFOCUS);

    if (ctx->focus == id) {
        i32 len = (i32)im_gui_strlen_(buf);
        i32 n = im_gui_min(bufsz - len - 1, (i32)im_gui_strlen_(ctx->input_text));
        if (n > 0) {
            im_gui_memcpy_(buf + len, ctx->input_text, (size_t)n);
            len += n;
            buf[len] = '\0';
            res |= IM_GUI_RES_CHANGE;
        }
        if (ctx->key_pressed & IM_GUI_KEY_BACKSPACE && len > 0) {
            while ((buf[--len] & 0xc0) == 0x80 && len > 0) { /* skip UTF-8 continuation bytes */ }
            buf[len] = '\0';
            res |= IM_GUI_RES_CHANGE;
        }
        if (ctx->key_pressed & IM_GUI_KEY_RETURN) {
            im_gui_set_focus(ctx, 0);
            res |= IM_GUI_RES_SUBMIT;
        }
    }

    im_gui_draw_control_frame(ctx, id, r, IM_GUI_COLOR_BASE, opt);
    if (ctx->focus == id) {
        Im_Color color = ctx->style->colors[IM_GUI_COLOR_TEXT];
        Im_Gui_Font font = ctx->style->font;
        i32 textw = ctx->text_width(font, buf, -1);
        i32 texth = ctx->text_height(font);
        i32 ofx = r.w - ctx->style->padding - textw - 1;
        i32 textx = r.x + im_gui_min(ofx, ctx->style->padding);
        i32 texty = r.y + (r.h - texth) / 2;
        im_gui_push_clip_rect(ctx, r);
        im_gui_draw_text(ctx, font, buf, -1, im_gui_vec2(textx, texty), color);
        im_gui_draw_rect(ctx, im_gui_rect(textx + textw, texty, 1, texth), color);
        im_gui_pop_clip_rect(ctx);
    } else {
        im_gui_draw_control_text(ctx, buf, r, IM_GUI_COLOR_TEXT, opt);
    }

    return res;
}

static i32 im_gui_number_textbox_(Im_Gui *ctx, f32 *value, Im_Rect r, Im_Gui_Id id)
{
    if (ctx->mouse_pressed == IM_GUI_MOUSE_LEFT && ctx->key_down & IM_GUI_KEY_SHIFT &&
        ctx->hover == id
    ) {
        ctx->number_edit = id;
        sprintf(ctx->number_edit_buf, IM_GUI_REAL_FMT, (double)*value);
    }
    if (ctx->number_edit == id) {
        i32 res = im_gui_textbox_raw(ctx, ctx->number_edit_buf, sizeof(ctx->number_edit_buf), id, r, 0);
        if (res & IM_GUI_RES_SUBMIT || ctx->focus != id) {
            *value = (f32)strtod(ctx->number_edit_buf, NULL);
            ctx->number_edit = 0;
        } else {
            return 1;
        }
    }
    return 0;
}

i32 im_gui_textbox_ex(Im_Gui *ctx, char *buf, i32 bufsz, i32 opt)
{
    Im_Gui_Id id = im_gui_get_id(ctx, &buf, sizeof(buf));
    Im_Rect r = im_gui_layout_next(ctx);
    return im_gui_textbox_raw(ctx, buf, bufsz, id, r, opt);
}

i32 im_gui_slider_ex(Im_Gui *ctx, f32 *value, f32 low, f32 high, f32 step, const char *fmt, i32 opt)
{
    char buf[IM_GUI_MAX_FMT + 1];
    Im_Rect thumb;
    i32 x, w, res = 0;
    f32 last = *value, v = last;
    Im_Gui_Id id = im_gui_get_id(ctx, &value, sizeof(value));
    Im_Rect base = im_gui_layout_next(ctx);

    if (im_gui_number_textbox_(ctx, &v, base, id)) { return res; }

    im_gui_update_control(ctx, id, base, opt);

    if (ctx->focus == id && (ctx->mouse_down | ctx->mouse_pressed) == IM_GUI_MOUSE_LEFT) {
        v = low + (f32)(ctx->mouse_pos.x - base.x) * (high - low) / (f32)base.w;
        if (step) { v = (f32)((i64)((v + step / 2) / step)) * step; }
    }
    *value = v = im_gui_clamp(v, low, high);
    if (last != v) { res |= IM_GUI_RES_CHANGE; }

    im_gui_draw_control_frame(ctx, id, base, IM_GUI_COLOR_BASE, opt);
    w = ctx->style->thumb_size;
    x = (i32)((v - low) * (f32)(base.w - w) / (high - low));
    thumb = im_gui_rect(base.x + x, base.y, w, base.h);
    im_gui_draw_control_frame(ctx, id, thumb, IM_GUI_COLOR_BUTTON, opt);
    sprintf(buf, fmt, (double)v);
    im_gui_draw_control_text(ctx, buf, base, IM_GUI_COLOR_TEXT, opt);

    return res;
}

i32 im_gui_number_ex(Im_Gui *ctx, f32 *value, f32 step, const char *fmt, i32 opt)
{
    char buf[IM_GUI_MAX_FMT + 1];
    i32 res = 0;
    Im_Gui_Id id = im_gui_get_id(ctx, &value, sizeof(value));
    Im_Rect base = im_gui_layout_next(ctx);
    f32 last = *value;

    if (im_gui_number_textbox_(ctx, value, base, id)) { return res; }

    im_gui_update_control(ctx, id, base, opt);

    if (ctx->focus == id && ctx->mouse_down == IM_GUI_MOUSE_LEFT) {
        *value += (f32)ctx->mouse_delta.x * step;
    }
    if (*value != last) { res |= IM_GUI_RES_CHANGE; }

    im_gui_draw_control_frame(ctx, id, base, IM_GUI_COLOR_BASE, opt);
    sprintf(buf, fmt, (double)*value);
    im_gui_draw_control_text(ctx, buf, base, IM_GUI_COLOR_TEXT, opt);

    return res;
}

static i32 im_gui_header_(Im_Gui *ctx, const char *label, i32 istreenode, i32 opt)
{
    Im_Rect r;
    i32 active, expanded;
    Im_Gui_Id id = im_gui_get_id(ctx, label, (i32)im_gui_strlen_(label));
    i32 idx = im_gui_pool_get(ctx, ctx->treenode_pool, IM_GUI_TREENODEPOOL_SIZE, id);
    i32 width = -1;
    im_gui_layout_row(ctx, 1, &width, 0);

    active = (idx >= 0);
    expanded = (opt & IM_GUI_OPT_EXPANDED) ? !active : active;
    r = im_gui_layout_next(ctx);
    im_gui_update_control(ctx, id, r, 0);

    active ^= (ctx->mouse_pressed == IM_GUI_MOUSE_LEFT && ctx->focus == id);

    if (idx >= 0) {
        if (active) { im_gui_pool_update(ctx, ctx->treenode_pool, idx); }
        else { im_gui_memset_(&ctx->treenode_pool[idx], 0, sizeof(Im_Gui_Pool_Item)); }
    } else if (active) {
        im_gui_pool_init(ctx, ctx->treenode_pool, IM_GUI_TREENODEPOOL_SIZE, id);
    }

    if (istreenode) {
        if (ctx->hover == id) { ctx->draw_frame(ctx, r, IM_GUI_COLOR_BUTTONHOVER); }
    } else {
        im_gui_draw_control_frame(ctx, id, r, IM_GUI_COLOR_BUTTON, 0);
    }
    im_gui_draw_icon(ctx, expanded ? IM_GUI_ICON_EXPANDED : IM_GUI_ICON_COLLAPSED,
                      im_gui_rect(r.x, r.y, r.h, r.h), ctx->style->colors[IM_GUI_COLOR_TEXT]);
    r.x += r.h - ctx->style->padding;
    r.w -= r.h - ctx->style->padding;
    im_gui_draw_control_text(ctx, label, r, IM_GUI_COLOR_TEXT, 0);

    return expanded ? IM_GUI_RES_ACTIVE : 0;
}

i32 im_gui_header_ex(Im_Gui *ctx, const char *label, i32 opt)
{
    return im_gui_header_(ctx, label, 0, opt);
}

i32 im_gui_begin_treenode_ex(Im_Gui *ctx, const char *label, i32 opt)
{
    i32 res = im_gui_header_(ctx, label, 1, opt);
    if (res & IM_GUI_RES_ACTIVE) {
        im_gui_get_layout_(ctx)->indent += ctx->style->indent;
        im_gui_push(ctx->id_stack, ctx->last_id);
    }
    return res;
}

void im_gui_end_treenode(Im_Gui *ctx)
{
    im_gui_get_layout_(ctx)->indent -= ctx->style->indent;
    im_gui_pop_id(ctx);
}

static void im_gui_scrollbar_(Im_Gui *ctx, Im_Gui_Container *cnt, Im_Rect *b, Im_Vec2 cs,
                               i32 is_y)
{
    i32 sz_content = is_y ? cs.y : cs.x;
    i32 body_extent = is_y ? b->h : b->w;
    i32 maxscroll = sz_content - body_extent;

    if (maxscroll > 0 && body_extent > 0) {
        Im_Rect base, thumb;
        Im_Gui_Id id = im_gui_get_id(ctx, is_y ? "!scrollbary" : "!scrollbarx", 11);
        i32 *scroll = is_y ? &cnt->scroll.y : &cnt->scroll.x;
        i32 mouse_d = is_y ? ctx->mouse_delta.y : ctx->mouse_delta.x;

        base = *b;
        if (is_y) {
            base.x = b->x + b->w;
            base.w = ctx->style->scrollbar_size;
        } else {
            base.y = b->y + b->h;
            base.h = ctx->style->scrollbar_size;
        }

        im_gui_update_control(ctx, id, base, 0);
        if (ctx->focus == id && ctx->mouse_down == IM_GUI_MOUSE_LEFT) {
            *scroll += mouse_d * sz_content / (is_y ? base.h : base.w);
        }
        *scroll = im_gui_clamp(*scroll, 0, maxscroll);

        ctx->draw_frame(ctx, base, IM_GUI_COLOR_SCROLLBASE);
        thumb = base;
        if (is_y) {
            thumb.h = im_gui_max(ctx->style->thumb_size, base.h * b->h / cs.y);
            thumb.y += *scroll * (base.h - thumb.h) / maxscroll;
        } else {
            thumb.w = im_gui_max(ctx->style->thumb_size, base.w * b->w / cs.x);
            thumb.x += *scroll * (base.w - thumb.w) / maxscroll;
        }
        ctx->draw_frame(ctx, thumb, IM_GUI_COLOR_SCROLLTHUMB);

        if (im_gui_mouse_over(ctx, *b)) { ctx->scroll_target = cnt; }
    } else {
        if (is_y) { cnt->scroll.y = 0; } else { cnt->scroll.x = 0; }
    }
}

static void im_gui_scrollbars_(Im_Gui *ctx, Im_Gui_Container *cnt, Im_Rect *body)
{
    i32 sz = ctx->style->scrollbar_size;
    Im_Vec2 cs = cnt->content_size;
    cs.x += ctx->style->padding * 2;
    cs.y += ctx->style->padding * 2;
    im_gui_push_clip_rect(ctx, *body);
    if (cs.y > cnt->body.h) { body->w -= sz; }
    if (cs.x > cnt->body.w) { body->h -= sz; }
    im_gui_scrollbar_(ctx, cnt, body, cs, 1);
    im_gui_scrollbar_(ctx, cnt, body, cs, 0);
    im_gui_pop_clip_rect(ctx);
}

static void im_gui_push_container_body_(Im_Gui *ctx, Im_Gui_Container *cnt, Im_Rect body, i32 opt)
{
    if (~opt & IM_GUI_OPT_NOSCROLL) { im_gui_scrollbars_(ctx, cnt, &body); }
    im_gui_push_layout_(ctx, im_gui_expand_rect_(body, -ctx->style->padding), cnt->scroll);
    cnt->body = body;
}

static void im_gui_begin_root_container_(Im_Gui *ctx, Im_Gui_Container *cnt)
{
    im_gui_push(ctx->container_stack, cnt);
    im_gui_push(ctx->root_list, cnt);
    cnt->head = im_gui_push_jump_(ctx, NULL);
    if (im_gui_rect_overlaps_vec2_(cnt->rect, ctx->mouse_pos) &&
        (!ctx->next_hover_root || cnt->zindex > ctx->next_hover_root->zindex)
    ) {
        ctx->next_hover_root = cnt;
    }
    im_gui_push(ctx->clip_stack, im_gui_unclipped_rect_);
}

static void im_gui_end_root_container_(Im_Gui *ctx)
{
    Im_Gui_Container *cnt = im_gui_get_current_container(ctx);
    cnt->tail = im_gui_push_jump_(ctx, NULL);
    cnt->head->jump.dst = ctx->command_list.items + ctx->command_list.idx;
    im_gui_pop_clip_rect(ctx);
    im_gui_pop_container_(ctx);
}

i32 im_gui_begin_window_ex(Im_Gui *ctx, const char *title, Im_Rect rect, i32 opt)
{
    Im_Rect body;
    Im_Gui_Id id = im_gui_get_id(ctx, title, (i32)im_gui_strlen_(title));
    Im_Gui_Container *cnt = im_gui_get_container_(ctx, id, opt);
    if (!cnt || !cnt->open) { return 0; }
    im_gui_push(ctx->id_stack, id);

    if (cnt->rect.w == 0) { cnt->rect = rect; }
    im_gui_begin_root_container_(ctx, cnt);
    rect = body = cnt->rect;

    if (~opt & IM_GUI_OPT_NOFRAME) {
        ctx->draw_frame(ctx, rect, IM_GUI_COLOR_WINDOWBG);
    }

    if (~opt & IM_GUI_OPT_NOTITLE) {
        Im_Rect tr = rect;
        tr.h = ctx->style->title_height;
        ctx->draw_frame(ctx, tr, IM_GUI_COLOR_TITLEBG);

        {
            Im_Gui_Id title_id = im_gui_get_id(ctx, "!title", 6);
            im_gui_update_control(ctx, title_id, tr, opt);
            im_gui_draw_control_text(ctx, title, tr, IM_GUI_COLOR_TITLETEXT, opt);
            if (title_id == ctx->focus && ctx->mouse_down == IM_GUI_MOUSE_LEFT) {
                cnt->rect.x += ctx->mouse_delta.x;
                cnt->rect.y += ctx->mouse_delta.y;
            }
            body.y += tr.h;
            body.h -= tr.h;
        }

        if (~opt & IM_GUI_OPT_NOCLOSE) {
            Im_Gui_Id close_id = im_gui_get_id(ctx, "!close", 6);
            Im_Rect r = im_gui_rect(tr.x + tr.w - tr.h, tr.y, tr.h, tr.h);
            tr.w -= r.w;
            im_gui_draw_icon(ctx, IM_GUI_ICON_CLOSE, r, ctx->style->colors[IM_GUI_COLOR_TITLETEXT]);
            im_gui_update_control(ctx, close_id, r, opt);
            if (ctx->mouse_pressed == IM_GUI_MOUSE_LEFT && close_id == ctx->focus) {
                cnt->open = 0;
            }
        }
    }

    im_gui_push_container_body_(ctx, cnt, body, opt);

    if (~opt & IM_GUI_OPT_NORESIZE) {
        i32 sz = ctx->style->title_height;
        Im_Gui_Id resize_id = im_gui_get_id(ctx, "!resize", 7);
        Im_Rect r = im_gui_rect(rect.x + rect.w - sz, rect.y + rect.h - sz, sz, sz);
        im_gui_update_control(ctx, resize_id, r, opt);
        if (resize_id == ctx->focus && ctx->mouse_down == IM_GUI_MOUSE_LEFT) {
            cnt->rect.w = im_gui_max(96, cnt->rect.w + ctx->mouse_delta.x);
            cnt->rect.h = im_gui_max(64, cnt->rect.h + ctx->mouse_delta.y);
        }
    }

    if (opt & IM_GUI_OPT_AUTOSIZE) {
        Im_Rect r = im_gui_get_layout_(ctx)->body;
        cnt->rect.w = cnt->content_size.x + (cnt->rect.w - r.w);
        cnt->rect.h = cnt->content_size.y + (cnt->rect.h - r.h);
    }

    if (opt & IM_GUI_OPT_POPUP && ctx->mouse_pressed && ctx->hover_root != cnt) {
        cnt->open = 0;
    }

    im_gui_push_clip_rect(ctx, cnt->body);
    return IM_GUI_RES_ACTIVE;
}

void im_gui_end_window(Im_Gui *ctx)
{
    im_gui_pop_clip_rect(ctx);
    im_gui_end_root_container_(ctx);
}

void im_gui_open_popup(Im_Gui *ctx, const char *name)
{
    Im_Gui_Container *cnt = im_gui_get_container(ctx, name);
    ctx->hover_root = ctx->next_hover_root = cnt;
    cnt->rect = im_gui_rect(ctx->mouse_pos.x, ctx->mouse_pos.y, 1, 1);
    cnt->open = 1;
    im_gui_bring_to_front(ctx, cnt);
}

i32 im_gui_begin_popup(Im_Gui *ctx, const char *name)
{
    i32 opt = IM_GUI_OPT_POPUP | IM_GUI_OPT_AUTOSIZE | IM_GUI_OPT_NORESIZE |
              IM_GUI_OPT_NOSCROLL | IM_GUI_OPT_NOTITLE | IM_GUI_OPT_CLOSED;
    return im_gui_begin_window_ex(ctx, name, im_gui_rect(0, 0, 0, 0), opt);
}

void im_gui_end_popup(Im_Gui *ctx)
{
    im_gui_end_window(ctx);
}

void im_gui_begin_panel_ex(Im_Gui *ctx, const char *name, i32 opt)
{
    Im_Gui_Container *cnt;
    im_gui_push_id(ctx, name, (i32)im_gui_strlen_(name));
    cnt = im_gui_get_container_(ctx, ctx->last_id, opt);
    cnt->rect = im_gui_layout_next(ctx);
    if (~opt & IM_GUI_OPT_NOFRAME) {
        ctx->draw_frame(ctx, cnt->rect, IM_GUI_COLOR_PANELBG);
    }
    im_gui_push(ctx->container_stack, cnt);
    im_gui_push_container_body_(ctx, cnt, cnt->rect, opt);
    im_gui_push_clip_rect(ctx, cnt->body);
}

void im_gui_end_panel(Im_Gui *ctx)
{
    im_gui_pop_clip_rect(ctx);
    im_gui_pop_container_(ctx);
}

#endif /* IM_GUI_IMPLEMENTATION_ */
#endif /* IM_GUI_IMPLEMENTATION */
