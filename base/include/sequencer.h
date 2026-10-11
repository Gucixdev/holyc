#ifndef HOLYC_SEQUENCER_H_
#define HOLYC_SEQUENCER_H_

/* Generic video/audio editing timeline (sequencer dla video audio etc) -
 * deliberately just the EDITING data model, not media decode: this
 * project already has src/codec/mp3_dec.h and src/codec/wav_dec.h for
 * actual audio decode, and other src/codec decoders for images. A Clip
 * here references
 * external media by an opaque label/source-ref string only - this file
 * never opens a file or touches a codec, it only tracks WHEN each clip
 * plays and on which track, the same separation of concerns as
 * project/vecindex/ivf_coarse.h keeping routing separate from the fine
 * quantizer that actually scores vectors.
 *
 * Arena-allocated (arena_alloc.h's arena_da_append growable-array
 * convention) - no malloc/free in this file directly.
 *
 * Overlap policy: add_clip REJECTS a clip that overlaps an existing clip
 * on the same track (returns 0, does not insert) rather than silently
 * splitting/trimming/replacing anything - the simplest correct behavior
 * for a first version. A real editor's drag-to-overlap-trims-neighbor
 * behavior is a UI-level policy on top of this, not something this data
 * model should decide unilaterally. */

#include "arena_alloc.h"

typedef enum {
    SEQUENCER_TRACK_VIDEO = 0,
    SEQUENCER_TRACK_AUDIO = 1
} Sequencer_Track_Kind;

typedef struct {
    f64 start_time;   /* seconds */
    f64 duration;     /* seconds, must be > 0 */
    char label[64];   /* opaque source reference, NUL-terminated, truncated if longer */
} Sequencer_Clip;

typedef struct {
    Sequencer_Track_Kind kind;
    Sequencer_Clip *items;
    size_t count;
    size_t capacity;
} Sequencer_Track;

typedef struct {
    Arena *a;
    Sequencer_Track *tracks;
    size_t track_count;
    size_t track_capacity;
    f64 playhead;      /* seconds */
    f64 fps;            /* frames/sec context for callers that want frame-accurate snapping - not enforced here */
} Sequencer;

void sequencer_init(Sequencer *seq, Arena *a, f64 fps);

/* returns the new track's index. */
size_t sequencer_add_track(Sequencer *seq, Sequencer_Track_Kind kind);

/* returns 1 if the clip was added, 0 if it overlaps an existing clip on
 * that track (track_index out of range also returns 0) - see file header
 * for why overlap is a hard rejection, not a trim/split. */
i32 sequencer_add_clip(Sequencer *seq, size_t track_index, f64 start_time, f64 duration, const char *label);

/* removes the clip at clip_index on the given track (shifts later clips
 * down by one slot to keep the array dense) - returns 1 on success, 0 if
 * either index is out of range. */
i32 sequencer_remove_clip(Sequencer *seq, size_t track_index, size_t clip_index);

void sequencer_set_playhead(Sequencer *seq, f64 time);

/* writes up to out_capacity clip indices (into that track's own items[])
 * that are active (start_time <= time < start_time+duration) at `time`
 * into out_indices; returns how many were written. Clips never overlap
 * within a track (add_clip enforces this), so this is at most 1 for any
 * single track, but the caller-facing shape stays an array for symmetry
 * with how a caller might want to query across several tracks in a loop
 * without special-casing "0 or 1". */
size_t sequencer_query_active_clips(const Sequencer *seq, size_t track_index, f64 time,
                                     size_t *out_indices, size_t out_capacity);

#endif /* HOLYC_SEQUENCER_H_ */

#ifdef SEQUENCER_IMPLEMENTATION
#ifndef SEQUENCER_IMPLEMENTATION_
#define SEQUENCER_IMPLEMENTATION_

void sequencer_init(Sequencer *seq, Arena *a, f64 fps)
{
    seq->a = a;
    seq->tracks = 0;
    seq->track_count = 0;
    seq->track_capacity = 0;
    seq->playhead = 0.0;
    seq->fps = fps;
}

size_t sequencer_add_track(Sequencer *seq, Sequencer_Track_Kind kind)
{
    Sequencer_Track t;
    t.kind = kind;
    t.items = 0;
    t.count = 0;
    t.capacity = 0;

    /* arena_da_append (arena_alloc.h) needs its target's growable-array
     * fields named items/count/capacity exactly - Sequencer's are named
     * tracks/track_count/track_capacity instead (test_sequencer.c already
     * depends on those exact names), so this one call site grows the
     * array by hand instead of renaming the struct to fit the macro. */
    if (seq->track_count >= seq->track_capacity) {
        size_t new_capacity = seq->track_capacity == 0 ? ARENA_DA_INIT_CAP : seq->track_capacity * 2;
        seq->tracks = (Sequencer_Track *)arena_realloc(
            seq->a, seq->tracks,
            seq->track_capacity * sizeof(*seq->tracks),
            new_capacity * sizeof(*seq->tracks));
        seq->track_capacity = new_capacity;
    }
    seq->tracks[seq->track_count++] = t;

    return seq->track_count - 1;
}

static i32 sequencer_clips_overlap_(f64 a_start, f64 a_dur, f64 b_start, f64 b_dur)
{
    f64 a_end = a_start + a_dur;
    f64 b_end = b_start + b_dur;
    return (a_start < b_end) && (b_start < a_end);
}

i32 sequencer_add_clip(Sequencer *seq, size_t track_index, f64 start_time, f64 duration, const char *label)
{
    Sequencer_Track *track;
    Sequencer_Clip clip;
    size_t i, n;

    if (track_index >= seq->track_count) return 0;
    if (duration <= 0.0) return 0;

    track = &seq->tracks[track_index];
    for (i = 0; i < track->count; i++) {
        if (sequencer_clips_overlap_(start_time, duration, track->items[i].start_time, track->items[i].duration)) {
            return 0;
        }
    }

    clip.start_time = start_time;
    clip.duration = duration;
    n = arena_strlen(label);
    if (n >= sizeof(clip.label)) n = sizeof(clip.label) - 1;
    arena_memcpy(clip.label, label, n);
    clip.label[n] = 0;

    arena_da_append(seq->a, track, clip);
    return 1;
}

i32 sequencer_remove_clip(Sequencer *seq, size_t track_index, size_t clip_index)
{
    Sequencer_Track *track;
    size_t i;

    if (track_index >= seq->track_count) return 0;
    track = &seq->tracks[track_index];
    if (clip_index >= track->count) return 0;

    for (i = clip_index; i + 1 < track->count; i++) {
        track->items[i] = track->items[i + 1];
    }
    track->count--;
    return 1;
}

void sequencer_set_playhead(Sequencer *seq, f64 time)
{
    seq->playhead = time;
}

size_t sequencer_query_active_clips(const Sequencer *seq, size_t track_index, f64 time,
                                     size_t *out_indices, size_t out_capacity)
{
    const Sequencer_Track *track;
    size_t i, written;

    if (track_index >= seq->track_count) return 0;
    track = &seq->tracks[track_index];

    written = 0;
    for (i = 0; i < track->count && written < out_capacity; i++) {
        f64 start = track->items[i].start_time;
        f64 end = start + track->items[i].duration;
        if (time >= start && time < end) {
            out_indices[written++] = i;
        }
    }
    return written;
}

#endif /* SEQUENCER_IMPLEMENTATION_ */
#endif /* SEQUENCER_IMPLEMENTATION */
