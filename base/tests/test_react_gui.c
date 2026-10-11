#include "test_common.h"
#define BASE_TYPE_IMPLEMENTATION
#include "base.h"
#define REACT_GUI_IMPLEMENTATION
#include "reactive.h"

int main(void)
{
    React_Gui r;
    u32 label_hash_a, label_hash_b;
    i32 n;

    print_cstr("test_react_gui\n");

    react_gui_init(&r);

    label_hash_a = react_gui_hash("hello", 5);
    label_hash_b = react_gui_hash("world", 5);
    TEST_CHECK(label_hash_a != label_hash_b, "react_gui_hash: different data hashes differently");
    TEST_CHECK(react_gui_hash("hello", 5) == label_hash_a, "react_gui_hash: same data hashes the same, deterministic");

    n = react_gui_should_update(&r, 1, label_hash_a);
    TEST_CHECK(n == 1, "should_update: first time seeing id=1 always reports changed");

    n = react_gui_should_update(&r, 1, label_hash_a);
    TEST_CHECK(n == 0, "should_update: same id, same hash next frame reports unchanged");

    n = react_gui_should_update(&r, 1, label_hash_b);
    TEST_CHECK(n == 1, "should_update: same id, different hash reports changed");

    n = react_gui_should_update(&r, 1, label_hash_b);
    TEST_CHECK(n == 0, "should_update: settled on the new hash, next frame unchanged again");

    n = react_gui_should_update(&r, 2, label_hash_a);
    TEST_CHECK(n == 1, "should_update: a DIFFERENT id starts fresh (first-seen), independent of id=1's state");

    n = react_gui_should_update(&r, 1, label_hash_b);
    TEST_CHECK(n == 0, "should_update: id=1 still unaffected by id=2's tracking");

    {
        i32 i, all_first_seen_changed = 1;
        for (i = 0; i < REACT_GUI_MAX_NODES - 2; i++) {
            if (react_gui_should_update(&r, (u32)(1000 + i), 42) != 1) {
                all_first_seen_changed = 0;
            }
        }
        TEST_CHECK(all_first_seen_changed, "should_update: filling the node table, every new id still reports first-seen changed");
    }

    {
        i32 fail_open;
        i32 i;
        for (i = 0; i < 8; i++) {
            react_gui_should_update(&r, (u32)(5000 + i), 1);
        }
        fail_open = react_gui_should_update(&r, 99999, 1);
        TEST_CHECK(fail_open == 1, "should_update: table exhausted, a brand-new id fails open (reports changed, never silently dropped)");
    }

    print_flush();
    return test_failures_ != 0;
}
