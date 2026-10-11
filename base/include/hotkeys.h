#ifndef HOLYC_HOTKEYS_H_
#define HOLYC_HOTKEYS_H_

/* Keybinding registration: map a (key code, modifier bitflags) pair to a
 * caller-supplied callback, then feed in raw key-down events and have
 * the matching callback fire. key codes are caller-defined (ASCII, a
 * platform scancode, whatever the caller's input source produces) -
 * this file doesn't interpret them, just compares for equality.
 * Modifier flags use `src/core/bit.h`'s BIT()/FLAG_* macros - arena-
 * allocated growable registration table (`arena_da_append`, see
 * `src/mem/arena_alloc.h`). A match requires the modifiers to be
 * EXACTLY equal, not a superset/subset - ctrl+shift+S does not also
 * match a binding registered for plain ctrl+S, a real, deliberate
 * design choice (the alternative - "at least these modifiers" matching
 * - creates ambiguous overlapping bindings, e.g. ctrl+S and
 * ctrl+shift+S both registered would both fire on ctrl+shift+S). */

#include "base.h"
#include "arena_alloc.h"

#define HOTKEY_MOD_CTRL  BIT(0)
#define HOTKEY_MOD_SHIFT BIT(1)
#define HOTKEY_MOD_ALT   BIT(2)
#define HOTKEY_MOD_SUPER BIT(3)

typedef void (*Hotkey_Fn)(void *userdata);

typedef struct {
    i32 key;
    u64 mods;
    Hotkey_Fn fn;
    void *userdata;
} Hotkey_Binding;

typedef struct {
    Hotkey_Binding *items;
    size_t count;
    size_t capacity;
} Hotkeys;

void hotkeys_init(Arena *a, Hotkeys *hk, size_t initial_capacity);
void hotkeys_register(Arena *a, Hotkeys *hk, i32 key, u64 mods, Hotkey_Fn fn, void *userdata);

/* fires the first matching binding's callback, if any (key AND mods
 * must both match exactly) - returns 1 if something fired, 0 if no
 * binding matched. */
i32 hotkeys_dispatch(const Hotkeys *hk, i32 key, u64 mods);

#endif /* HOLYC_HOTKEYS_H_ */

#ifdef HOTKEYS_IMPLEMENTATION
#ifndef HOTKEYS_IMPLEMENTATION_
#define HOTKEYS_IMPLEMENTATION_

void hotkeys_init(Arena *a, Hotkeys *hk, size_t initial_capacity)
{
    hk->items = NULL;
    hk->count = 0;
    hk->capacity = 0;
    if (initial_capacity > 0) {
        hk->items = (Hotkey_Binding *)arena_alloc(a, initial_capacity * sizeof(Hotkey_Binding));
        hk->capacity = initial_capacity;
    }
}

void hotkeys_register(Arena *a, Hotkeys *hk, i32 key, u64 mods, Hotkey_Fn fn, void *userdata)
{
    Hotkey_Binding b;
    b.key = key;
    b.mods = mods;
    b.fn = fn;
    b.userdata = userdata;
    arena_da_append(a, hk, b);
}

i32 hotkeys_dispatch(const Hotkeys *hk, i32 key, u64 mods)
{
    size_t i;
    for (i = 0; i < hk->count; i++) {
        if (hk->items[i].key == key && hk->items[i].mods == mods) {
            hk->items[i].fn(hk->items[i].userdata);
            return 1;
        }
    }
    return 0;
}

#endif /* HOTKEYS_IMPLEMENTATION_ */
#endif /* HOTKEYS_IMPLEMENTATION */
