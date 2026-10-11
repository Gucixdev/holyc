/* test_canvas.c - the cell grid: put/text/fill, wide characters, clipping, colour quantisation down to 256/16/8 colours and to plain
 * ASCII, the diff renderer (only changed cells, minimal cursor moves), snapshots, and utf8 encode/decode round-tripping. */
#include "lazytui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } } while (0)
static lt_caps UNI = {true, true, false, true, 1 << 24};
static lt_caps NOUNI = {false, false, false, false, 8};

int main(void)
{
	lt_canvas *c = lt_canvas_new(10, 4, UNI);
	CHECK(lt_canvas_w(c) == 10 && lt_canvas_h(c) == 4, "the size is what was asked for");
	lt_canvas_clear(c, LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
	lt_rect all = {0, 0, 10, 4};
	lt_put(c, all, 2, 1, 'x', LT_STYLE(LT_RGB(255, 0, 0), LT_DEFAULT, 0));
	CHECK(lt_canvas_char(c, 2, 1) == 'x', "a put cell reads back");
	CHECK(lt_canvas_char(c, 0, 0) == ' ', "an untouched cell is blank");
	CHECK(lt_canvas_char(c, 99, 99) == ' ', "out of bounds reads as blank, not a crash");
	int used = lt_text(c, all, 0, 0, "hello", LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0), 10);
	CHECK(used == 5 && lt_canvas_char(c, 0, 0) == 'h' && lt_canvas_char(c, 4, 0) == 'o', "text lands character by character");
	int clipped = lt_text(c, all, 0, 3, "toolong", LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0), 3);
	CHECK(clipped == 3 && lt_canvas_char(c, 3, 3) == ' ', "text is clipped to maxw, not just the canvas edge");
	lt_fill(c, (lt_rect){0, 2, 10, 1}, '#', LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
	CHECK(lt_canvas_char(c, 0, 2) == '#' && lt_canvas_char(c, 9, 2) == '#', "fill covers the whole rectangle");

	/* a clip rectangle stops drawing outside it even when the caller's own rect is bigger */
	lt_canvas_set_clip(c, (lt_rect){0, 3, 4, 1});
	lt_fill(c, (lt_rect){0, 3, 10, 1}, '!', LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
	CHECK(lt_canvas_char(c, 3, 3) == '!' && lt_canvas_char(c, 5, 3) != '!', "the clip rectangle wins over a wider draw");
	lt_canvas_reset_clip(c);
	lt_fill(c, (lt_rect){5, 3, 5, 1}, '!', LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
	CHECK(lt_canvas_char(c, 6, 3) == '!', "resetting the clip allows drawing there again");

	/* wide characters: a CJK glyph occupies two cells, the second reads as 0 */
	lt_canvas_clear(c, LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
	lt_put(c, all, 0, 0, 0x4E2D /* 中 */, LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
	CHECK(lt_canvas_char(c, 0, 0) == 0x4E2D && lt_canvas_char(c, 1, 0) == 0, "a wide glyph's right half is the sentinel 0");
	lt_put(c, all, 1, 0, 'y', LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));                /* overwriting the right half */
	CHECK(lt_canvas_char(c, 0, 0) == ' ' && lt_canvas_char(c, 1, 0) == 'y', "overwriting a wide glyph's half clears the other half");

	/* no-unicode canvas: unicode drawing characters fall back to ASCII automatically */
	lt_canvas *nc = lt_canvas_new(6, 1, NOUNI);
	lt_rect nall = {0, 0, 6, 1};
	lt_put(nc, nall, 0, 0, 0x2588, LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));           /* full block */
	CHECK(lt_canvas_char(nc, 0, 0) != 0x2588, "a block glyph is not drawn verbatim without unicode");
	lt_canvas_free(nc);

	/* colour round-trip and mixing */
	int r, g, b;
	CHECK(lt_color_rgb(LT_RGB(10, 20, 30), &r, &g, &b) && r == 10 && g == 20 && b == 30, "truecolour round-trips exactly");
	CHECK(!lt_color_rgb(LT_DEFAULT, &r, &g, &b), "the terminal default has no rgb");
	lt_color mid = lt_color_mix(LT_RGB(0, 0, 0), LT_RGB(200, 200, 200), 0.5);
	lt_color_rgb(mid, &r, &g, &b);
	CHECK(r > 90 && r < 110, "a 50%% mix lands near the middle");

	/* snapshot: take a rectangle, mutate the canvas, blit it back unchanged */
	lt_canvas_clear(c, LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
	lt_text(c, all, 0, 0, "snap", LT_STYLE(LT_RGB(1, 2, 3), LT_DEFAULT, 0), 10);
	lt_snap *snap = lt_snap_take(c, (lt_rect){0, 0, 4, 1});
	lt_fill(c, (lt_rect){0, 0, 4, 1}, ' ', LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
	CHECK(lt_canvas_char(c, 0, 0) == ' ', "the area was cleared");
	lt_snap_blit(snap, c, 0, 0);
	CHECK(lt_canvas_char(c, 0, 0) == 's' && lt_canvas_char(c, 3, 0) == 'p', "the snapshot restores exactly what was captured");
	lt_snap_free(snap);

	/* diff renderer: an unchanged canvas diffs to (almost) nothing; a changed cell appears in the escape sequence and the buffer variant agrees */
	lt_canvas *prev = lt_canvas_new(10, 4, UNI);
	lt_canvas_copy(prev, c);
	size_t n1;
	char *d1 = lt_canvas_diff(c, prev, 0, &n1);
	CHECK(n1 < 16, "diffing identical canvases produces (almost) no output");
	lt_put(c, all, 0, 0, 'Z', LT_STYLE(LT_RGB(9, 9, 9), LT_DEFAULT, 0));
	size_t n2;
	char *d2 = lt_canvas_diff(c, prev, 0, &n2);
	CHECK(n2 > n1, "a changed cell produces real output");
	CHECK(strchr(d2, 'Z') != NULL, "the changed character appears in the escape stream");
	char *buf = NULL; size_t cap = 0;
	size_t n3 = lt_canvas_diff_buf(c, prev, 0, &buf, &cap);
	CHECK(n3 == n2 && !memcmp(buf, d2, n2), "the reusable-buffer variant matches the allocating one");
	free(buf); free(d1); free(d2);
	lt_canvas_free(prev);

	/* a full dump (used by tests elsewhere) strips trailing blanks per row and joins with newlines */
	lt_canvas_clear(c, LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0));
	lt_text(c, all, 0, 0, "hi", LT_STYLE(LT_DEFAULT, LT_DEFAULT, 0), 10);
	char *dump = lt_canvas_dump(c);
	CHECK(!strncmp(dump, "hi\n", 3), "the first row is the text with no trailing padding");
	free(dump);

	/* resize keeps the canvas usable and clears it */
	lt_canvas_resize(c, 20, 8);
	CHECK(lt_canvas_w(c) == 20 && lt_canvas_h(c) == 8, "resize takes effect");
	CHECK(lt_canvas_char(c, 15, 5) == ' ', "the grown area is blank");
	lt_canvas_free(c);

	/* utf8 round trip */
	char buf8[8];
	size_t enc = lt_utf8_encode(0x1F600, buf8);                 /* an emoji, 4 bytes */
	buf8[enc] = 0;
	uint32_t back;
	size_t dec = lt_utf8_decode(buf8, &back);
	CHECK(dec == enc && back == 0x1F600, "a 4-byte codepoint round-trips");
	CHECK(lt_utf8_width("a") == 1 && lt_utf8_width("中") == 2, "ascii is narrow, CJK is wide");
	uint32_t bad;
	CHECK(lt_utf8_decode("\xff", &bad) >= 1 && bad == 0xFFFD, "an invalid byte decodes to the replacement character, not a crash");

	printf(fails ? "canvas: %d failed\n" : "canvas: ok\n", fails);
	return fails != 0;
}
