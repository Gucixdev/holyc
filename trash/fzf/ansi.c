/* ansi.c - takes the colour escapes out of a line (--ansi) and remembers which byte range had which colour. */
#include "fzf.h"
#include "opts.h"

typedef struct { int fg, bg, attr; } St;
#define DEF (-1)
/* attribute bits, same numbering as term.c uses */
#define A_BOLD 1
#define A_DIM 2
#define A_ITAL 4
#define A_UL 8
#define A_BLINK 16
#define A_REV 32
#define A_STRIKE 64

static void sgr(St *s, const int *v, int n)
{
	if (n == 0) { s->fg = s->bg = DEF; s->attr = 0; return; }
	for (int i = 0; i < n; i++) {
		int c = v[i];
		if (c == 0) { s->fg = s->bg = DEF; s->attr = 0; }
		else if (c == 1) s->attr |= A_BOLD;
		else if (c == 2) s->attr |= A_DIM;
		else if (c == 3) s->attr |= A_ITAL;
		else if (c == 4) s->attr |= A_UL;
		else if (c == 5) s->attr |= A_BLINK;
		else if (c == 7) s->attr |= A_REV;
		else if (c == 9) s->attr |= A_STRIKE;
		else if (c == 22) s->attr &= ~(A_BOLD | A_DIM);
		else if (c == 23) s->attr &= ~A_ITAL;
		else if (c == 24) s->attr &= ~A_UL;
		else if (c == 25) s->attr &= ~A_BLINK;
		else if (c == 27) s->attr &= ~A_REV;
		else if (c == 29) s->attr &= ~A_STRIKE;
		else if (c >= 30 && c <= 37) s->fg = c - 30;
		else if (c >= 90 && c <= 97) s->fg = c - 90 + 8;
		else if (c >= 40 && c <= 47) s->bg = c - 40;
		else if (c >= 100 && c <= 107) s->bg = c - 100 + 8;
		else if (c == 39) s->fg = DEF;
		else if (c == 49) s->bg = DEF;
		else if (c == 38 || c == 48) {
			int *tgt = c == 38 ? &s->fg : &s->bg;
			if (i + 2 < n && v[i + 1] == 5) { *tgt = v[i + 2] & 255; i += 2; }
			else if (i + 4 < n && v[i + 1] == 2) { *tgt = 0x1000000 | ((v[i + 2] & 255) << 16) | ((v[i + 3] & 255) << 8) | (v[i + 4] & 255); i += 4; }
		}
	}
}

/* Copies line to out without escape sequences. When spans is not NULL, coloured ranges (byte offsets into the
 * stripped text) are appended to it. Returns true if the line held any escape. */
bool ansi_extract(const char *s, size_t n, Buf *out, AnsiSpan **spans, uint32_t *nspans, uint32_t *cap)
{
	St st = {DEF, DEF, 0}, open = {DEF, DEF, 0};
	size_t i = 0, span_start = 0;
	bool found = false, in_span = false;
	while (i < n) {
		if (s[i] == 033 && i + 1 < n) {
			found = true;
			if (s[i + 1] == '[') {
				size_t j = i + 2;
				int v[32], nv = 0, cur = 0;
				bool have = false;
				while (j < n && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';' || s[j] == ':')) {
					if (s[j] == ';' || s[j] == ':') { if (nv < 32) v[nv++] = have ? cur : 0; cur = 0; have = false; }
					else { cur = cur * 10 + (s[j] - '0'); have = true; }
					j++;
				}
				if (have || nv) { if (nv < 32) v[nv++] = have ? cur : 0; }
				while (j < n && s[j] >= 0x20 && s[j] <= 0x2F) j++;
				if (j < n && s[j] == 'm') {
					St old = st;
					sgr(&st, v, nv);
					if (spans && (old.fg != st.fg || old.bg != st.bg || old.attr != st.attr)) {
						if (in_span && out->len > span_start) {
							if (*nspans >= *cap) { *cap = *cap ? *cap * 2 : 4; *spans = xrealloc(*spans, *cap * sizeof(AnsiSpan)); }
							(*spans)[(*nspans)++] = (AnsiSpan){(uint32_t)span_start, (uint32_t)(out->len - span_start), (uint32_t)open.fg, (uint32_t)open.bg, (uint16_t)open.attr};
						}
						open = st; span_start = out->len;
						in_span = st.fg != DEF || st.bg != DEF || st.attr;
					}
				}
				i = j < n ? j + 1 : n;
			} else if (s[i + 1] == ']') {             /* OSC ... BEL or ST */
				size_t j = i + 2;
				while (j < n && s[j] != 7 && !(s[j] == 033 && j + 1 < n && s[j + 1] == '\\')) j++;
				i = j < n ? (s[j] == 7 ? j + 1 : j + 2) : n;
			} else i += 2;
			continue;
		}
		buf_addc(out, s[i++]);
	}
	if (spans && in_span && out->len > span_start) {
		if (*nspans >= *cap) { *cap = *cap ? *cap * 2 : 4; *spans = xrealloc(*spans, *cap * sizeof(AnsiSpan)); }
		(*spans)[(*nspans)++] = (AnsiSpan){(uint32_t)span_start, (uint32_t)(out->len - span_start), (uint32_t)open.fg, (uint32_t)open.bg, (uint16_t)open.attr};
	}
	return found;
}
