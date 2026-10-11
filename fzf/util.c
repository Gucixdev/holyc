/* util.c - allocation, growable buffers, UTF-8 and terminal cell widths. */
#include "fzf.h"
#include <locale.h>

void die(const char *fmt, ...)
{
	va_list ap;
	fputs("fzf: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}
void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) die("out of memory"); return p; }
void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) die("out of memory"); return p; }
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }

void buf_init(Buf *b) { b->p = NULL; b->len = b->cap = 0; }
void buf_free(Buf *b) { free(b->p); buf_init(b); }
void buf_reset(Buf *b) { b->len = 0; if (b->p) b->p[0] = 0; }
static void buf_grow(Buf *b, size_t extra)
{
	if (b->len + extra + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 256;
		while (nc < b->len + extra + 1) nc *= 2;
		b->p = xrealloc(b->p, nc);
		b->cap = nc;
	}
}
void buf_addn(Buf *b, const char *s, size_t n) { buf_grow(b, n); memcpy(b->p + b->len, s, n); b->len += n; b->p[b->len] = 0; }
void buf_adds(Buf *b, const char *s) { buf_addn(b, s, strlen(s)); }
void buf_addc(Buf *b, char c) { buf_addn(b, &c, 1); }
void buf_printf(Buf *b, const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n > 0) { buf_grow(b, (size_t)n); vsnprintf(b->p + b->len, (size_t)n + 1, fmt, ap2); b->len += (size_t)n; }
	va_end(ap2);
}
char *buf_take(Buf *b)
{
	char *p = b->p ? b->p : xstrdup("");
	b->p = NULL; b->len = b->cap = 0;
	return p;
}

/* ---- UTF-8 ---- */
int u8dec(const char *s, size_t n, rune *out)
{
	const unsigned char *u = (const unsigned char *)s;
	if (n == 0) { *out = 0; return 0; }
	if (u[0] < 0x80) { *out = u[0]; return 1; }
	int len = (u[0] >= 0xF0 && u[0] < 0xF8) ? 4 : (u[0] >= 0xE0) ? 3 : (u[0] >= 0xC2 && u[0] < 0xE0) ? 2 : 0;
	if (u[0] >= 0xF8 || len == 0 || (size_t)len > n) { *out = 0xFFFD; return 1; }
	rune r = len == 2 ? (u[0] & 0x1F) : len == 3 ? (u[0] & 0x0F) : (u[0] & 0x07);
	for (int i = 1; i < len; i++) {
		if ((u[i] & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
		r = (r << 6) | (u[i] & 0x3F);
	}
	*out = r;
	return len;
}
size_t u8enc(rune r, char *o)
{
	if (r < 0x80) { o[0] = (char)r; return 1; }
	if (r < 0x800) { o[0] = (char)(0xC0 | (r >> 6)); o[1] = (char)(0x80 | (r & 0x3F)); return 2; }
	if (r < 0x10000) { o[0] = (char)(0xE0 | (r >> 12)); o[1] = (char)(0x80 | ((r >> 6) & 0x3F)); o[2] = (char)(0x80 | (r & 0x3F)); return 3; }
	o[0] = (char)(0xF0 | (r >> 18)); o[1] = (char)(0x80 | ((r >> 12) & 0x3F)); o[2] = (char)(0x80 | ((r >> 6) & 0x3F)); o[3] = (char)(0x80 | (r & 0x3F));
	return 4;
}
size_t utf8_to_runes(const char *s, size_t n, rune *out)
{
	size_t i = 0, k = 0;
	while (i < n) { rune r; i += (size_t)u8dec(s + i, n - i, &r); out[k++] = r; }
	return k;
}
size_t utf8_len(const char *s, size_t n)
{
	size_t i = 0, k = 0;
	while (i < n) { rune r; i += (size_t)u8dec(s + i, n - i, &r); k++; }
	return k;
}

/* Cell width: combining marks and controls 0, East Asian wide / emoji 2, everything else 1. */
int rune_width(rune r)
{
	if (r < 0x20 || (r >= 0x7F && r < 0xA0)) return 0;
	if (r < 0x300) return 1;
	static const struct { rune lo, hi; } zero[] = {
		{0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x0610, 0x061A},
		{0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0E31, 0x0E31},
		{0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064},
		{0x20D0, 0x20FF}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF}};
	for (size_t i = 0; i < sizeof zero / sizeof zero[0]; i++)
		if (r >= zero[i].lo && r <= zero[i].hi) return 0;
	static const struct { rune lo, hi; } wide[] = {
		{0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3}, {0x25FD, 0x25FE},
		{0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
		{0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA}, {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA},
		{0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755},
		{0x2757, 0x2757}, {0x2795, 0x2797}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55},
		{0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
		{0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF},
		{0x1F900, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD}};
	for (size_t i = 0; i < sizeof wide / sizeof wide[0]; i++)
		if (r >= wide[i].lo && r <= wide[i].hi) return 2;
	return 1;
}
int str_width(const char *s, size_t n)
{
	int w = 0;
	size_t i = 0;
	while (i < n) { rune r; i += (size_t)u8dec(s + i, n - i, &r); w += rune_width(r); }
	return w;
}

void locale_init(void)
{
	if (!setlocale(LC_CTYPE, "")) setlocale(LC_CTYPE, "C.UTF-8");
	else if (MB_CUR_MAX == 1) setlocale(LC_CTYPE, "C.UTF-8");   /* the C locale cannot classify non-ASCII: use a UTF-8 one */
}
rune rune_lower(rune r) { return r < 0x80 ? ((r >= 'A' && r <= 'Z') ? r + 32 : r) : (rune)towlower((wint_t)r); }
rune rune_upper(rune r) { return r < 0x80 ? ((r >= 'a' && r <= 'z') ? r - 32 : r) : (rune)towupper((wint_t)r); }
bool rune_is_space(rune r)
{
	return r == ' ' || (r >= 9 && r <= 13) || r == 0x85 || r == 0xA0 || (r >= 0x2000 && r <= 0x200A) || r == 0x1680 || r == 0x2028 ||
	       r == 0x2029 || r == 0x202F || r == 0x205F || r == 0x3000;
}
