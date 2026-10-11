#ifndef HOLYC_REACT_H_
#define HOLYC_REACT_H_

/* Signal: the generic "react/observe" primitive from the dataflow
 * spec's own "pipe stream chunk observe signal react pattern" list -
 * a value that notifies a list of subscribed observers whenever
 * signal_emit() fires. Type-erased (void* event_data + void*
 * userdata), same shape as im_gui.h's own callback fields - C89 has no
 * generics, so this is the standard way around that. reactive.h (also
 * in this directory) is the higher-level GUI paradigm that a caller
 * could build ON TOP of this; this file itself knows nothing about
 * GUIs. Observer storage grows the same doubling-via-Arena way
 * array.h's dynamic mode does (own copy of that pattern, not a
 * dependency on array.h - this project's own "own local copy, not
 * cross-file dependency" rule). */

#include "base.h"
#include "arena_alloc.h"

typedef void (*Signal_Fn)(void *userdata, void *event_data);

typedef struct {
    Signal_Fn fn;
    void *userdata;
} Signal_Observer;

typedef struct {
    Signal_Observer *observers; /* arena-owned, grows via signal_subscribe */
    size_t count;
    size_t capacity;
} Signal;

void signal_init(Signal *s);
void signal_subscribe(Arena *a, Signal *s, Signal_Fn fn, void *userdata);

/* removes the first observer matching BOTH fn and userdata (the pair
 * identifies a subscription, since the same fn is often subscribed
 * multiple times with different userdata). Returns 0 if no match. */
i32 signal_unsubscribe(Signal *s, Signal_Fn fn, void *userdata);

void signal_emit(const Signal *s, void *event_data); /* calls every observer, in subscribe order */

#endif /* HOLYC_REACT_H_ */

#ifdef REACT_IMPLEMENTATION
#ifndef REACT_IMPLEMENTATION_
#define REACT_IMPLEMENTATION_

static void react_memcpy_(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
}

void signal_init(Signal *s)
{
    s->observers = 0;
    s->count = 0;
    s->capacity = 0;
}

void signal_subscribe(Arena *a, Signal *s, Signal_Fn fn, void *userdata)
{
    if (s->count >= s->capacity) {
        size_t new_capacity = s->capacity == 0 ? 8 : s->capacity * 2;
        Signal_Observer *new_items = (Signal_Observer *)arena_alloc(a, new_capacity * sizeof(Signal_Observer));
        if (s->count > 0) react_memcpy_(new_items, s->observers, s->count * sizeof(Signal_Observer));
        s->observers = new_items;
        s->capacity = new_capacity;
    }
    s->observers[s->count].fn = fn;
    s->observers[s->count].userdata = userdata;
    s->count++;
}

i32 signal_unsubscribe(Signal *s, Signal_Fn fn, void *userdata)
{
    size_t i;
    for (i = 0; i < s->count; i++) {
        if (s->observers[i].fn == fn && s->observers[i].userdata == userdata) {
            s->observers[i] = s->observers[s->count - 1];
            s->count--;
            return 1;
        }
    }
    return 0;
}

void signal_emit(const Signal *s, void *event_data)
{
    size_t i;
    for (i = 0; i < s->count; i++) {
        s->observers[i].fn(s->observers[i].userdata, event_data);
    }
}

#endif /* REACT_IMPLEMENTATION_ */
#endif /* REACT_IMPLEMENTATION */
