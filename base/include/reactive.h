#ifndef HOLYC_REACT_GUI_H_
#define HOLYC_REACT_GUI_H_

/* "reactive gui bez grama reacta" - reactive GUI, without a gram of
 * React. Explicitly NOT a virtual-DOM: no shadow tree, no tree diffing,
 * no reconciliation pass walking two trees to find what changed. That
 * would fight this project's branchless/no-hidden-cost philosophy
 * - a vdom's whole cost model is "diff first, THEN
 * decide what to redraw", which is itself a nontrivial O(tree size) pass
 * every single frame, on top of whatever the actual redraw costs.
 *
 * What this actually is: a small per-widget DIRTY-CHECKING layer on top
 * of src/uiux/im_gui.h. Each call site that wants to skip re-submitting
 * an unchanged widget hashes whatever values that widget's appearance
 * actually depends on (via react_gui_hash, base_type.h's fnv1a_hash
 * underneath) and asks react_gui_should_update(r, id, hash) whether
 * that hash changed since last frame for that id. If it returns 0, the
 * caller skips its im_gui_label/im_gui_button/etc call for this widget
 * this frame - im_gui.h's own retained id/pool/focus state (see
 * Im_Gui_Container/Im_Gui_Pool_Item) keeps the widget "existing" across
 * skipped frames the same way it already handles a widget not being
 * submitted on frames where an if/switch simply routes around it, so
 * skipping a submission here isn't new machinery, it's this file
 * deciding WHEN to skip rather than the caller hand-rolling that
 * decision inline everywhere.
 *
 * This is genuinely smaller in scope than what "reactive" usually means
 * in a web-UI context: no computed/derived state graph, no automatic
 * dependency tracking (the caller must name what to hash - nothing here
 * infers it), no scheduling/batching. It answers exactly one question,
 * "did the thing this widget depends on change since last frame", as
 * cheaply as one array lookup + one u32 compare - not a smaller version
 * of React's model, a different and much smaller one that happens to
 * solve the same everyday problem (skip redundant redraw work). */

#include "base.h"

#ifndef REACT_GUI_MAX_NODES
#define REACT_GUI_MAX_NODES 256
#endif

typedef struct {
    u32 id;
    u32 hash;
    i32 seen; /* 0 until this id's first should_update call - first call always reports "changed" */
} React_Node;

typedef struct {
    React_Node nodes[REACT_GUI_MAX_NODES];
    i32 node_count;
} React_Gui;

void react_gui_init(React_Gui *r);

/* convenience wrapper over base_type.h's fnv1a_hash (seed 0) - hash
 * together whatever raw values (packed into a local struct/array at the
 * call site) a widget's appearance actually depends on. */
u32 react_gui_hash(const void *data, size_t len);

/* id identifies WHICH reactive node this is across frames (e.g. from
 * im_gui_get_id on the same label a caller would pass to im_gui.h
 * anyway) - NOT the hash of its current state. Returns 1 (changed, the
 * caller should submit this widget to im_gui.h this frame) the first
 * time an id is seen, or whenever dep_hash differs from what was
 * recorded last time; returns 0 (unchanged, safe to skip) otherwise.
 * Always records dep_hash for next frame's comparison, whichever it
 * returns. Returns 1 (fail open, never silently drop a real widget) if
 * REACT_GUI_MAX_NODES is exhausted and id has never been seen before. */
i32 react_gui_should_update(React_Gui *r, u32 id, u32 dep_hash);

#endif /* HOLYC_REACT_GUI_H_ */

#ifdef REACT_GUI_IMPLEMENTATION
#ifndef REACT_GUI_IMPLEMENTATION_
#define REACT_GUI_IMPLEMENTATION_

void react_gui_init(React_Gui *r)
{
    i32 i;
    r->node_count = 0;
    for (i = 0; i < REACT_GUI_MAX_NODES; i++) {
        r->nodes[i].id = 0;
        r->nodes[i].hash = 0;
        r->nodes[i].seen = 0;
    }
}

u32 react_gui_hash(const void *data, size_t len)
{
    return fnv1a_hash(data, len, 0);
}

i32 react_gui_should_update(React_Gui *r, u32 id, u32 dep_hash)
{
    i32 i;
    React_Node *slot = 0;

    for (i = 0; i < r->node_count; i++) {
        if (r->nodes[i].id == id) {
            slot = &r->nodes[i];
            break;
        }
    }

    if (!slot) {
        if (r->node_count >= REACT_GUI_MAX_NODES) return 1; /* fail open, see file header */
        slot = &r->nodes[r->node_count++];
        slot->id = id;
        slot->seen = 0;
    }

    if (!slot->seen) {
        slot->seen = 1;
        slot->hash = dep_hash;
        return 1;
    }

    if (slot->hash != dep_hash) {
        slot->hash = dep_hash;
        return 1;
    }

    return 0;
}

#endif /* REACT_GUI_IMPLEMENTATION_ */
#endif /* REACT_GUI_IMPLEMENTATION */
