#include "test_common.h"
#define IM_GUI_IMPLEMENTATION
#include "immediate.h"
#include <string.h>

/* fixed-width stand-in font metrics - real usage would measure glyphs via
 * whatever text-rendering backend is wired up, none exists in this project
 * yet (see im_gui.h's own header comment) */
static i32 stub_text_width_(Im_Gui_Font font, const char *str, i32 len)
{
    (void)font;
    if (len < 0) return (i32)strlen(str) * 8;
    return len * 8;
}

static i32 stub_text_height_(Im_Gui_Font font)
{
    (void)font;
    return 16;
}

int main(void)
{
    Im_Gui ctx;
    i32 checkbox_state = 0;
    i32 button_res, checkbox_res, win_res;
    i32 widths[2];

    print_cstr("test_im_gui\n");

    im_gui_init(&ctx);
    ctx.text_width = stub_text_width_;
    ctx.text_height = stub_text_height_;

    TEST_CHECK(ctx.style == &ctx._style, "init: style points at the context's own default style");
    TEST_CHECK(ctx.frame == 0, "init: frame counter starts at 0");

    im_gui_input_mousemove(&ctx, 50, 50);
    im_gui_begin(&ctx);
    TEST_CHECK(ctx.frame == 1, "begin: frame counter increments");

    win_res = im_gui_begin_window(&ctx, "Test Window", im_gui_rect(0, 0, 200, 200));
    TEST_CHECK(win_res & IM_GUI_RES_ACTIVE, "begin_window: a freshly opened window reports active");
    TEST_CHECK(ctx.container_stack.idx == 1, "begin_window: pushes exactly one container");

    widths[0] = 80; widths[1] = 80;
    im_gui_layout_row(&ctx, 2, widths, 0);

    button_res = im_gui_button(&ctx, "OK");
    TEST_CHECK((button_res & IM_GUI_RES_SUBMIT) == 0, "button: no click input this frame means no submit");

    checkbox_res = im_gui_checkbox(&ctx, "Enable", &checkbox_state);
    TEST_CHECK((checkbox_res & IM_GUI_RES_CHANGE) == 0, "checkbox: no click input this frame means no change");
    TEST_CHECK(checkbox_state == 0, "checkbox: state untouched without a click");

    im_gui_label(&ctx, "a label");

    im_gui_end_window(&ctx);
    TEST_CHECK(ctx.container_stack.idx == 0, "end_window: container stack balanced after close");
    TEST_CHECK(ctx.clip_stack.idx == 0, "end_window: clip stack balanced after close");

    im_gui_end(&ctx);
    TEST_CHECK(ctx.command_list.idx > 0, "end: frame produced a non-empty command list (frame drew something)");

    /* a second full frame must also balance cleanly - catches any stack
     * leak that only shows up on frame N+1 (e.g. retained pool state) */
    im_gui_input_mousedown(&ctx, 10, 10, IM_GUI_MOUSE_LEFT);
    im_gui_begin(&ctx);
    im_gui_begin_window(&ctx, "Test Window", im_gui_rect(0, 0, 200, 200));
    im_gui_layout_row(&ctx, 2, widths, 0);
    im_gui_button(&ctx, "OK");
    im_gui_checkbox(&ctx, "Enable", &checkbox_state);
    im_gui_end_window(&ctx);
    im_gui_end(&ctx);
    TEST_CHECK(ctx.container_stack.idx == 0, "second frame: container stack still balanced");
    TEST_CHECK(ctx.frame == 2, "second frame: frame counter incremented again");

    /* command list walk: id_stack/clip_stack/etc are asserted balanced
     * inside im_gui_end() already (aborts on failure) - reaching here at
     * all is itself a real correctness signal. Walk the command list too,
     * exercising im_gui_next_command()'s jump-following. */
    {
        Im_Gui_Command *cmd = NULL;
        i32 n_commands = 0;
        while (im_gui_next_command(&ctx, &cmd)) {
            TEST_CHECK(cmd->type > 0 && cmd->type < IM_GUI_COMMAND_MAX, "command walk: every command has a valid type");
            n_commands++;
        }
        TEST_CHECK(n_commands > 0, "command walk: at least one non-jump command was recorded");
    }

    print_flush();
    return test_failures_ != 0;
}
