#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#define SEQUENCER_IMPLEMENTATION
#include "arena_alloc.h"
#include "sequencer.h"

int main(void)
{
    Arena a = {0};
    Sequencer seq;
    size_t video_track, audio_track;
    size_t idx[4];
    size_t n;
    i32 ok;

    print_cstr("test_sequencer\n");

    sequencer_init(&seq, &a, 30.0);
    TEST_CHECK(seq.track_count == 0, "init: no tracks yet");
    TEST_CHECK(seq.playhead == 0.0, "init: playhead starts at 0");

    video_track = sequencer_add_track(&seq, SEQUENCER_TRACK_VIDEO);
    audio_track = sequencer_add_track(&seq, SEQUENCER_TRACK_AUDIO);
    TEST_CHECK(seq.track_count == 2, "add_track: two tracks now exist");
    TEST_CHECK(seq.tracks[video_track].kind == SEQUENCER_TRACK_VIDEO, "add_track: video track kind stored correctly");
    TEST_CHECK(seq.tracks[audio_track].kind == SEQUENCER_TRACK_AUDIO, "add_track: audio track kind stored correctly");

    ok = sequencer_add_clip(&seq, video_track, 0.0, 5.0, "intro.mp4");
    TEST_CHECK(ok == 1, "add_clip: first clip on an empty track succeeds");

    ok = sequencer_add_clip(&seq, video_track, 10.0, 5.0, "outro.mp4");
    TEST_CHECK(ok == 1, "add_clip: second non-overlapping clip succeeds");

    ok = sequencer_add_clip(&seq, video_track, 3.0, 4.0, "overlap.mp4");
    TEST_CHECK(ok == 0, "add_clip: overlapping clip is rejected");
    TEST_CHECK(seq.tracks[video_track].count == 2, "add_clip: rejected overlap did not get inserted");

    ok = sequencer_add_clip(&seq, video_track, 5.0, 5.0, "touching.mp4");
    TEST_CHECK(ok == 1, "add_clip: a clip exactly touching (not overlapping) an edge is accepted");

    TEST_CHECK(sequencer_add_clip(&seq, video_track, 100.0, 0.0, "zero_dur.mp4") == 0,
               "add_clip: zero duration is rejected");

    ok = sequencer_add_clip(&seq, 99, 0.0, 1.0, "nope.mp4");
    TEST_CHECK(ok == 0, "add_clip: out-of-range track index is rejected");

    n = sequencer_query_active_clips(&seq, video_track, 2.0, idx, 4);
    TEST_CHECK(n == 1 && idx[0] == 0, "query_active_clips: time inside the first clip finds it");

    n = sequencer_query_active_clips(&seq, video_track, 12.0, idx, 4);
    TEST_CHECK(n == 1 && idx[0] == 1, "query_active_clips: time inside the outro clip (index 1) finds it");

    /* NOT a gap: touching.mp4 [5,10) was added above specifically because
     * it touches both neighbors with no space between - 7.5 falls inside
     * it (index 2), so this queries the middle clip, not empty space. */
    n = sequencer_query_active_clips(&seq, video_track, 7.5, idx, 4);
    TEST_CHECK(n == 1 && idx[0] == 2, "query_active_clips: time inside the touching (middle) clip finds it");

    sequencer_set_playhead(&seq, 42.5);
    TEST_CHECK(seq.playhead == 42.5, "set_playhead: stored value round-trips");

    ok = sequencer_remove_clip(&seq, video_track, 0);
    TEST_CHECK(ok == 1, "remove_clip: removing the first clip succeeds");
    TEST_CHECK(seq.tracks[video_track].count == 2, "remove_clip: track count dropped by one");
    n = sequencer_query_active_clips(&seq, video_track, 2.0, idx, 4);
    TEST_CHECK(n == 0, "remove_clip: the removed clip's time range is now empty");
    n = sequencer_query_active_clips(&seq, video_track, 12.0, idx, 4);
    TEST_CHECK(n == 1, "remove_clip: a later clip survived the removal and shifted down correctly");

    ok = sequencer_remove_clip(&seq, video_track, 99);
    TEST_CHECK(ok == 0, "remove_clip: out-of-range clip index is rejected");

    TEST_CHECK(seq.tracks[audio_track].count == 0, "audio track untouched by all the video-track operations above");

    arena_free(&a);
    print_flush();
    return test_failures_ != 0;
}
