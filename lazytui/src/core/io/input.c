/* input.c - the text behind a searchbar: UTF-8 with a cursor counted in characters and fzf's editing keys. */
#include "lazytui.h"
#include <stdlib.h>
#include <string.h>

void lt_input_init(lt_input *in) { memset(in, 0, sizeof *in); in->text = calloc(1, 1); in->cap = 1; }
void lt_input_free(lt_input *in) { free(in->text); free(in->yank); memset(in, 0, sizeof *in); }

static void grow(lt_input *in, size_t extra)
{
	if (in->len + extra + 1 > in->cap) { in->cap = (in->len + extra + 1) * 2; in->text = realloc(in->text, in->cap); }
}
static size_t nchars(const lt_input *in) { size_t n = 0; for (const char *s = in->text; *s; n++) { uint32_t r; s += lt_utf8_decode(s, &r); } return n; }
static size_t off_of(const lt_input *in, int chars)      /* byte offset of the chars-th character */
{
	const char *s = in->text;
	for (int i = 0; i < chars && *s; i++) { uint32_t r; s += lt_utf8_decode(s, &r); }
	return (size_t)(s - in->text);
}
static uint32_t char_at(const lt_input *in, int i) { uint32_t r = 0; lt_utf8_decode(in->text + off_of(in, i), &r); return r; }
static bool is_word(uint32_t r) { return r > 127 || (r >= '0' && r <= '9') || (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || r == '_'; }
static bool is_space(uint32_t r) { return r == ' ' || r == '\t'; }

static void insert(lt_input *in, const char *s, size_t n)
{
	grow(in, n);
	size_t at = off_of(in, in->cursor);
	memmove(in->text + at + n, in->text + at, in->len - at + 1);
	memcpy(in->text + at, s, n);
	in->len += n;
	for (size_t i = 0; i < n;) { uint32_t r; i += lt_utf8_decode(s + i, &r); in->cursor++; }
}
static void cut(lt_input *in, int from, int to, bool kill)   /* characters [from, to) */
{
	size_t a = off_of(in, from), b = off_of(in, to);
	if (b <= a) return;
	if (kill) { free(in->yank); in->yank = malloc(b - a + 1); memcpy(in->yank, in->text + a, b - a); in->yank[b - a] = 0; }
	memmove(in->text + a, in->text + b, in->len - b + 1);
	in->len -= b - a;
	if (in->cursor > to) in->cursor -= to - from; else if (in->cursor > from) in->cursor = from;
}
static int word_back(const lt_input *in, int i, bool ws)
{
	while (i > 0 && (ws ? is_space(char_at(in, i - 1)) : !is_word(char_at(in, i - 1)))) i--;
	while (i > 0 && (ws ? !is_space(char_at(in, i - 1)) : is_word(char_at(in, i - 1)))) i--;
	return i;
}
static int word_fwd(const lt_input *in, int i)
{
	int n = (int)nchars(in);
	while (i < n && !is_word(char_at(in, i))) i++;
	while (i < n && is_word(char_at(in, i))) i++;
	return i;
}

void lt_input_set(lt_input *in, const char *text)
{
	size_t n = strlen(text);
	grow(in, n);
	memcpy(in->text, text, n + 1);
	in->len = n;
	in->cursor = (int)nchars(in);
}

bool lt_input_paste(lt_input *in, const char *s)
{
	size_t n = strlen(s);
	char *c = malloc(n + 1);
	size_t k = 0;
	for (size_t i = 0; i < n; i++) c[k++] = (s[i] == '\n' || s[i] == '\r' || s[i] == '\t') ? ' ' : s[i];
	if (k) insert(in, c, k);
	free(c);
	return k > 0;
}

bool lt_input_key(lt_input *in, int key)
{
	int n = (int)nchars(in), c0 = in->cursor;
	size_t l0 = in->len;
	int base = key & 0xFFFFFF, mods = key & ~0xFFFFFF;
	if (mods & LT_MOD_ALT) {
		if (base == 'b') in->cursor = word_back(in, in->cursor, false);
		else if (base == 'f') in->cursor = word_fwd(in, in->cursor);
		else if (base == 'd') cut(in, in->cursor, word_fwd(in, in->cursor), true);
		else if (base == LT_K_BSPACE) cut(in, word_back(in, in->cursor, false), in->cursor, true);
		return in->cursor != c0 || in->len != l0;
	}
	if (mods & LT_MOD_CTRL) {
		switch (base) {
		case 'a': in->cursor = 0; break;
		case 'e': in->cursor = n; break;
		case 'b': if (in->cursor > 0) in->cursor--; break;
		case 'f': if (in->cursor < n) in->cursor++; break;
		case 'h': if (in->cursor > 0) cut(in, in->cursor - 1, in->cursor, false); break;
		case 'd': if (in->cursor < n) cut(in, in->cursor, in->cursor + 1, false); break;
		case 'k': cut(in, in->cursor, n, true); break;
		case 'u': cut(in, 0, in->cursor, true); break;
		case 'w': cut(in, word_back(in, in->cursor, true), in->cursor, true); break;
		case 'y': if (in->yank) insert(in, in->yank, strlen(in->yank)); break;
		default: return false;
		}
		return in->cursor != c0 || in->len != l0;
	}
	switch (base) {
	case LT_K_LEFT: if (in->cursor > 0) in->cursor--; break;
	case LT_K_RIGHT: if (in->cursor < n) in->cursor++; break;
	case LT_K_HOME: in->cursor = 0; break;
	case LT_K_END: in->cursor = n; break;
	case LT_K_BSPACE: if (in->cursor > 0) cut(in, in->cursor - 1, in->cursor, false); break;
	case LT_K_DEL: if (in->cursor < n) cut(in, in->cursor, in->cursor + 1, false); break;
	default:
		if (base >= 32 && base < 0x110000 && base != 127) { char u[5]; size_t l = lt_utf8_encode((uint32_t)base, u); insert(in, u, l); }
		else return false;
	}
	return in->cursor != c0 || in->len != l0;
}

/* ---- searchbar events ---- */
static bool consumes(int key)
{
	int base = key & 0xFFFFFF, mods = key & ~0xFFFFFF;
	if (mods & LT_MOD_ALT) return base == 'b' || base == 'f' || base == 'd' || base == LT_K_BSPACE;
	if (mods & LT_MOD_CTRL) return strchr("aebfhdkuwy", base) != NULL && base < 128;
	switch (base) {
	case LT_K_LEFT: case LT_K_RIGHT: case LT_K_HOME: case LT_K_END: case LT_K_BSPACE: case LT_K_DEL: return true;
	}
	return base >= 32 && base < 0x110000 && base != 127 && base < LT_K_ENTER;
}

bool lt_searchbar_event(lt_input *in, const char *prompt, lt_rect r, const lt_event *ev, lt_wres *res)
{
	lt_wres dummy;
	if (!res) res = &dummy;
	*res = LT_W_IGNORED;
	if (ev->kind == LT_EV_PASTE) { bool ch = ev->paste && lt_input_paste(in, ev->paste); *res = ch ? LT_W_CHANGED : LT_W_HANDLED; return ch; }
	if (ev->kind == LT_EV_MOUSE) {
		if ((ev->key & 0xFFFFFF) != LT_K_CLICK || ev->x < r.x || ev->x >= r.x + r.w || ev->y < r.y || ev->y >= r.y + r.h) return false;
		int col = ev->x - r.x - lt_utf8_width(prompt ? prompt : "> "), acc = 0, k = 0;
		for (const char *s = in->text; *s;) {
			uint32_t q;
			size_t l = lt_utf8_decode(s, &q);
			char tmp[5]; memcpy(tmp, s, l); tmp[l] = 0;
			int w = lt_utf8_width(tmp);
			if (acc + w > col) break;
			acc += w; k++; s += l;
		}
		in->cursor = col < 0 ? 0 : k;
		*res = LT_W_HANDLED;
		return false;
	}
	if (ev->kind != LT_EV_KEY || !consumes(ev->key)) return false;
	bool ch = lt_input_key(in, ev->key);
	*res = ch ? LT_W_CHANGED : LT_W_HANDLED;
	return ch;
}
