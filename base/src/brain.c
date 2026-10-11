#ifndef HOLYC_BRAIN_C_
#define HOLYC_BRAIN_C_

/* Brain: a throttled "think" tick - holds a caller-supplied decision
 * function (could be a minimax/mcts/goap/htn/uai call from anywhere
 * else in ml/, or a caller's own logic; this file has no opinion
 * which) and calls it at most once every think_interval seconds,
 * accumulating dt in between - the standard real-game-AI optimization
 * of NOT re-deciding every single frame (an NPC doesn't need to
 * re-plan 60 times a second; thinking every 0.1-0.5s is common and
 * usually indistinguishable in practice). Same "generic via function
 * pointer" stance every other ml/ file in this directory takes. */

#include "base.h"

typedef void (*Brain_Think_Fn)(void *ctx, f32 dt_since_last_think);

typedef struct {
    Brain_Think_Fn think;
    void *ctx;
    f32 think_interval;
    f32 accumulated;
} Brain;

void brain_init(Brain *b, Brain_Think_Fn think, void *ctx, f32 think_interval);

/* call every real frame with the real frame dt - internally decides
 * whether enough time has accumulated to actually call think() this
 * call or not. Returns 1 if think() was actually called this update,
 * 0 if it was just accumulating. */
i32 brain_update(Brain *b, f32 dt);

#endif /* HOLYC_BRAIN_C_ */

#ifdef BRAIN_IMPLEMENTATION
#ifndef BRAIN_IMPLEMENTATION_
#define BRAIN_IMPLEMENTATION_

void brain_init(Brain *b, Brain_Think_Fn think, void *ctx, f32 think_interval)
{
    b->think = think;
    b->ctx = ctx;
    b->think_interval = think_interval;
    b->accumulated = 0.0f;
}

i32 brain_update(Brain *b, f32 dt)
{
    b->accumulated += dt;
    if (b->accumulated < b->think_interval) return 0;

    {
        f32 elapsed = b->accumulated;
        b->accumulated = 0.0f;
        b->think(b->ctx, elapsed);
    }
    return 1;
}

#endif /* BRAIN_IMPLEMENTATION_ */
#endif /* BRAIN_IMPLEMENTATION */
