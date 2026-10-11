#ifndef HOLYC_FSM_H_
#define HOLYC_FSM_H_

/* Minimal finite state machine - named states (plain i32 ids, caller
 * defines their own #define'd constants, see test_fsm.c), transitions
 * with an optional per-state enter/exit callback. Deliberately NOT
 * hierarchical/nested states (a real, separate scope - a sub-machine
 * per state, entered/exited alongside the parent transition - left out
 * on purpose, not silently dropped). */

#include "types/base.h"
#include "alloc/arena_alloc.h"

typedef void (*Fsm_Callback)(i32 state, void *ctx);

typedef struct {
    Fsm_Callback *on_enter; /* arena-owned, [state_count] */
    Fsm_Callback *on_exit;  /* arena-owned, [state_count] */
    i32 state_count;
    i32 current;
    void *ctx; /* passed through to every callback, caller-owned */
} Fsm;

void fsm_init(Arena *a, Fsm *fsm, i32 state_count, i32 initial_state, void *ctx);
void fsm_set_enter(Fsm *fsm, i32 state, Fsm_Callback fn);
void fsm_set_exit(Fsm *fsm, i32 state, Fsm_Callback fn);
i32 fsm_current(const Fsm *fsm);
void fsm_transition(Fsm *fsm, i32 new_state);

#endif /* HOLYC_FSM_H_ */

#ifdef FSM_IMPLEMENTATION
#ifndef FSM_IMPLEMENTATION_
#define FSM_IMPLEMENTATION_

void fsm_init(Arena *a, Fsm *fsm, i32 state_count, i32 initial_state, void *ctx)
{
    i32 i;

    fsm->on_enter = (Fsm_Callback *)arena_realloc(a, 0, 0, (size_t)state_count * sizeof(Fsm_Callback));
    fsm->on_exit = (Fsm_Callback *)arena_realloc(a, 0, 0, (size_t)state_count * sizeof(Fsm_Callback));
    for (i = 0; i < state_count; i++) {
        fsm->on_enter[i] = 0;
        fsm->on_exit[i] = 0;
    }
    fsm->state_count = state_count;
    fsm->current = initial_state;
    fsm->ctx = ctx;
}

void fsm_set_enter(Fsm *fsm, i32 state, Fsm_Callback fn)
{
    fsm->on_enter[state] = fn;
}

void fsm_set_exit(Fsm *fsm, i32 state, Fsm_Callback fn)
{
    fsm->on_exit[state] = fn;
}

i32 fsm_current(const Fsm *fsm)
{
    return fsm->current;
}

void fsm_transition(Fsm *fsm, i32 new_state)
{
    if (new_state == fsm->current) return;

    if (fsm->on_exit[fsm->current] != 0) fsm->on_exit[fsm->current](fsm->current, fsm->ctx);
    fsm->current = new_state;
    if (fsm->on_enter[fsm->current] != 0) fsm->on_enter[fsm->current](fsm->current, fsm->ctx);
}

#endif /* FSM_IMPLEMENTATION_ */
#endif /* FSM_IMPLEMENTATION */
