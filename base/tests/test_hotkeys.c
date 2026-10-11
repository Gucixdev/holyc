#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define HOTKEYS_IMPLEMENTATION
#include "arena_alloc.h"
#include "hotkeys.h"

static i32 fired_a_ = 0;
static i32 fired_ctrl_s_ = 0;

static void on_a_(void *userdata) { (void)userdata; fired_a_++; }
static void on_ctrl_s_(void *userdata) { (void)userdata; fired_ctrl_s_++; }

int main(void)
{
    Arena a = {0};
    Hotkeys hk;

    print_cstr("test_hotkeys\n");

    hotkeys_init(&a, &hk, 8);
    hotkeys_register(&a, &hk, 'A', 0, on_a_, NULL);
    hotkeys_register(&a, &hk, 'S', HOTKEY_MOD_CTRL, on_ctrl_s_, NULL);

    hotkeys_dispatch(&hk, 'A', 0);
    TEST_CHECK(fired_a_ == 1, "plain 'A' key fires its own callback");
    TEST_CHECK(fired_ctrl_s_ == 0, "plain 'A' key does not fire the unrelated ctrl+S callback");

    hotkeys_dispatch(&hk, 'A', HOTKEY_MOD_CTRL);
    TEST_CHECK(fired_a_ == 1, "ctrl+A does NOT match a binding registered for plain 'A' (modifiers must match exactly)");

    hotkeys_dispatch(&hk, 'S', HOTKEY_MOD_CTRL);
    TEST_CHECK(fired_ctrl_s_ == 1, "ctrl+S fires its own callback");

    hotkeys_dispatch(&hk, 'S', HOTKEY_MOD_CTRL | HOTKEY_MOD_SHIFT);
    TEST_CHECK(fired_ctrl_s_ == 1, "ctrl+shift+S does not match ctrl+S (extra modifier means no match)");

    hotkeys_dispatch(&hk, 'Z', 0);
    TEST_CHECK(fired_a_ == 1 && fired_ctrl_s_ == 1, "an unregistered key fires nothing");

    print_flush();
    return test_failures_ != 0;
}
