#ifndef HOLYC_ANSII_SEQUENCER_H_
#define HOLYC_ANSII_SEQUENCER_H_

/* Terminal renderer for a sequencer.h Sequencer - "sequencer dla video
 * audio ale ansii". Same split as ansii_node.h/node.h: all timeline
 * state (tracks, clips, playhead) lives in sequencer.h, this file only
 * turns it into cells on a tui.h Tui_Buffer.
 *
 * Layout: one row per track, in track-index order, starting at a
 * caller-given (origin_x, origin_y). A clip covering [start, start+dur)
 * seconds becomes a bracketed span "[label...]" from
 * origin_x + (i32)(start * px_per_sec) to origin_x + (i32)((start+dur) *
 * px_per_sec) - 1 on that track's row; the label is written inside the
 * brackets, truncated (not wrapped) if the span is too narrow, matching
 * ansii_node.h's own "first pass, no overflow handling" stance on
 * anything fancier. The playhead is drawn as a '|' column at
 * origin_x + (i32)(seq->playhead * px_per_sec), on every track row,
 * drawn AFTER clips so it stays visible even where it crosses one. */

#include "sequencer.h"
#include "tui.h"

/* px_per_sec: caller-chosen time-to-column scale (e.g. 10.0 means one
 * screen column per 100ms) - not stored in Sequencer itself, since that
 * struct is a pure editing data model with no notion of a viewport. */
void ansii_sequencer_render(const Sequencer *seq, Tui_Buffer *buf, i32 origin_x, i32 origin_y, f64 px_per_sec);

#endif /* HOLYC_ANSII_SEQUENCER_H_ */

#ifdef ANSII_SEQUENCER_IMPLEMENTATION
#ifndef ANSII_SEQUENCER_IMPLEMENTATION_
#define ANSII_SEQUENCER_IMPLEMENTATION_

static void ansii_sequencer_draw_clip_(Tui_Buffer *buf, i32 row_y, i32 x0, i32 x1, const char *label)
{
    i32 x;
    i32 inner_w;
    size_t label_len, i;

    if (x1 < x0) x1 = x0;
    tui_buffer_set_cell(buf, x0, row_y, '[', TUI_DEFAULT, TUI_DEFAULT);
    tui_buffer_set_cell(buf, x1, row_y, ']', TUI_DEFAULT, TUI_DEFAULT);
    for (x = x0 + 1; x < x1; x++) {
        tui_buffer_set_cell(buf, x, row_y, '-', TUI_DEFAULT, TUI_DEFAULT);
    }

    inner_w = x1 - x0 - 1;
    if (inner_w <= 0) return;

    label_len = arena_strlen(label);
    if (label_len > (size_t)inner_w) label_len = (size_t)inner_w;
    for (i = 0; i < label_len; i++) {
        tui_buffer_set_cell(buf, x0 + 1 + (i32)i, row_y, (u32)(u8)label[i], TUI_DEFAULT, TUI_DEFAULT);
    }
}

void ansii_sequencer_render(const Sequencer *seq, Tui_Buffer *buf, i32 origin_x, i32 origin_y, f64 px_per_sec)
{
    size_t t, c;
    i32 playhead_x;

    for (t = 0; t < seq->track_count; t++) {
        const Sequencer_Track *track = &seq->tracks[t];
        i32 row_y = origin_y + (i32)t;

        for (c = 0; c < track->count; c++) {
            const Sequencer_Clip *clip = &track->items[c];
            i32 x0 = origin_x + (i32)(clip->start_time * px_per_sec);
            i32 x1 = origin_x + (i32)((clip->start_time + clip->duration) * px_per_sec) - 1;
            ansii_sequencer_draw_clip_(buf, row_y, x0, x1, clip->label);
        }
    }

    /* playhead drawn last, on top of any clip it crosses, on every
     * track row - a single global marker, not a per-track one, since
     * one playhead applies to the whole sequencer at once. */
    playhead_x = origin_x + (i32)(seq->playhead * px_per_sec);
    for (t = 0; t < seq->track_count; t++) {
        tui_buffer_set_cell(buf, playhead_x, origin_y + (i32)t, '|', TUI_DEFAULT, TUI_DEFAULT);
    }
}

#endif /* ANSII_SEQUENCER_IMPLEMENTATION_ */
#endif /* ANSII_SEQUENCER_IMPLEMENTATION */
