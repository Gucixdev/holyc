#include "test_common.h"
#define ARENA_ALLOC_IMPLEMENTATION
#include "arena_alloc.h"
#define SEQUENCER_IMPLEMENTATION
#include "sequencer.h"
#define TUI_IMPLEMENTATION
#include "tui.h"
#define ANSII_SEQUENCER_IMPLEMENTATION
#include "ansii_sequencer.h"

int main(void)
{
    Arena a;
    Sequencer seq;
    Tui_Buffer buf;
    size_t video_track, audio_track;

    print_cstr("test_ansii_sequencer\n");

    a.begin = a.end = NULL;
    sequencer_init(&seq, &a, 30.0);
    video_track = sequencer_add_track(&seq, SEQUENCER_TRACK_VIDEO);
    audio_track = sequencer_add_track(&seq, SEQUENCER_TRACK_AUDIO);

    /* durations chosen generously (>= label length + 2 for brackets) so
     * the label actually fits inside its clip's span, not truncated to
     * nothing - see ansii_sequencer_render's inner_w math. */
    sequencer_add_clip(&seq, video_track, 0.0, 10.0, "intro");
    sequencer_add_clip(&seq, audio_track, 2.0, 10.0, "music");
    sequencer_set_playhead(&seq, 15.0);

    tui_buffer_init(&buf, 40, 4);
    tui_buffer_clear(&buf, TUI_DEFAULT, TUI_DEFAULT);
    /* px_per_sec = 1.0: seconds map straight to columns for easy math */
    ansii_sequencer_render(&seq, &buf, 0, 0, 1.0);

    TEST_CHECK(buf.cells[0].ch == '[', "render: video track clip opens with '[' at column 0 (start_time=0)");
    TEST_CHECK(buf.cells[9].ch == ']', "render: video track clip closes at column 9 (start+dur-1=0+10-1)");
    TEST_CHECK(buf.cells[1].ch == 'i', "render: video track clip label starts with 'i' (intro)");
    TEST_CHECK(buf.cells[1 * 40 + 2].ch == '[', "render: audio track clip starts at column 2 (start_time=2.0)");
    TEST_CHECK(buf.cells[1 * 40 + 11].ch == ']', "render: audio track clip closes at column 11 (start+dur-1=2+10-1)");
    TEST_CHECK(buf.cells[1 * 40 + 3].ch == 'm', "render: audio track clip label starts with 'm' (music)");

    TEST_CHECK(buf.cells[0 * 40 + 15].ch == '|', "render: playhead column (15) drawn on video track row");
    TEST_CHECK(buf.cells[1 * 40 + 15].ch == '|', "render: playhead column (15) drawn on audio track row too, past both clips");

    {
        Sequencer empty_seq;
        Tui_Buffer empty_buf;
        sequencer_init(&empty_seq, &a, 30.0);
        tui_buffer_init(&empty_buf, 5, 5);
        tui_buffer_clear(&empty_buf, TUI_DEFAULT, TUI_DEFAULT);
        ansii_sequencer_render(&empty_seq, &empty_buf, 0, 0, 1.0);
        TEST_CHECK(empty_buf.cells[0].ch == ' ', "render: no tracks draws nothing, no crash");
        tui_buffer_free(&empty_buf);
    }

    tui_buffer_free(&buf);
    print_flush();
    return test_failures_ != 0;
}
