/* algo.c - the matching algorithms. A port of the scoring scheme of fzf's FuzzyMatchV2 (a Smith-Waterman style
 * alignment with word-boundary / camelCase / consecutive bonuses and gap penalties), the greedy V1 fallback and the
 * exact / prefix / suffix / equal matchers, written from the documented algorithm so that ranking is the same. */
#include "fzf.h"

enum { C_WHITE, C_NONWORD, C_DELIM, C_LOWER, C_UPPER, C_LETTER, C_NUMBER };

#define SCORE_MATCH 16
#define SCORE_GAP_START (-3)
#define SCORE_GAP_EXT (-1)
#define BONUS_BOUNDARY (SCORE_MATCH / 2)
#define BONUS_NONWORD (SCORE_MATCH / 2)
#define BONUS_CAMEL123 (BONUS_BOUNDARY + SCORE_GAP_EXT)
#define BONUS_CONSECUTIVE (-(SCORE_GAP_START + SCORE_GAP_EXT))
#define BONUS_FIRST_MULT 2

static int16_t bonus_white = BONUS_BOUNDARY + 2, bonus_delim = BONUS_BOUNDARY + 1;
static int initial_class = C_WHITE;
static uint8_t ascii_class[128];
static int16_t bonus_matrix[7][7];
static const MRes NOMATCH = {-1, -1, 0};

static int class_of_ascii_init(int c, const char *delims)
{
	if (c >= 'a' && c <= 'z') return C_LOWER;
	if (c >= 'A' && c <= 'Z') return C_UPPER;
	if (c >= '0' && c <= '9') return C_NUMBER;
	if (strchr(" \t\n\v\f\r", c) && c) return C_WHITE;
	if (c && strchr(delims, c)) return C_DELIM;
	return C_NONWORD;
}

static int16_t bonus_for(int prev, int cls)
{
	if (cls >= C_NONWORD) {
		if (prev == C_WHITE) return bonus_white;
		if (prev == C_DELIM) return bonus_delim;
		if (prev == C_NONWORD) return BONUS_BOUNDARY;
	}
	if ((prev == C_LOWER && cls == C_UPPER) || (prev != C_NUMBER && cls == C_NUMBER)) return BONUS_CAMEL123;
	if (cls == C_NONWORD || cls == C_DELIM) return BONUS_NONWORD;
	if (cls == C_WHITE) return bonus_white;
	return 0;
}

bool algo_init(const char *scheme)
{
	const char *delims = "/,:;|";
	initial_class = C_WHITE;
	if (!strcmp(scheme, "default")) { bonus_white = BONUS_BOUNDARY + 2; bonus_delim = BONUS_BOUNDARY + 1; }
	else if (!strcmp(scheme, "path")) { bonus_white = BONUS_BOUNDARY; bonus_delim = BONUS_BOUNDARY + 1; delims = "/"; initial_class = C_DELIM; }
	else if (!strcmp(scheme, "history")) { bonus_white = BONUS_BOUNDARY; bonus_delim = BONUS_BOUNDARY; }
	else return false;
	for (int i = 0; i < 128; i++) ascii_class[i] = (uint8_t)class_of_ascii_init(i, delims);
	for (int i = 0; i <= C_NUMBER; i++)
		for (int j = 0; j <= C_NUMBER; j++) bonus_matrix[i][j] = bonus_for(i, j);
	return true;
}

static int class_of(rune r)
{
	if (r < 128) return ascii_class[r];
	if (iswlower((wint_t)r)) return C_LOWER;
	if (iswupper((wint_t)r)) return C_UPPER;
	if (iswdigit((wint_t)r)) return C_NUMBER;
	if (iswalpha((wint_t)r)) return C_LETTER;
	if (rune_is_space(r)) return C_WHITE;
	return C_NONWORD;
}

/* ---- accent folding: lowercase Latin letters (with accents and strokes) to ASCII, applied after case folding ---- */
#include "normtab.h"
rune normalize_rune(rune r)
{
	if (r < 0xC0 || r > 0x1EFF) return r;
	size_t lo = 0, hi = sizeof norm_tab / sizeof norm_tab[0];
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		if (norm_tab[mid].from == r) return norm_tab[mid].to;
		if (norm_tab[mid].from < r) lo = mid + 1; else hi = mid;
	}
	return r;
}
void normalize_runes(rune *r, int n) { for (int i = 0; i < n; i++) r[i] = normalize_rune(r[i]); }

static inline rune fold(rune c, bool cs, bool norm)
{
	if (!cs) { if (c >= 'A' && c <= 'Z') c += 32; else if (c > 127) c = rune_lower(c); }
	if (norm && c > 127) c = normalize_rune(c);
	return c;
}

/* ---- scratch memory (one per thread) ---- */
struct Scratch { int16_t *i16; size_t c16; int32_t *i32; size_t c32; rune *rb; size_t crb; int *ib[3]; size_t cib[3]; };
Scratch *scratch_new(void) { return calloc(1, sizeof(Scratch)); }
void scratch_free(Scratch *s) { if (s) { free(s->i16); free(s->i32); free(s->rb); for (int i = 0; i < 3; i++) free(s->ib[i]); free(s); } }
int *scratch_ibuf(Scratch *s, int w, size_t n) { if (n > s->cib[w]) { s->cib[w] = n * 2 + 16; s->ib[w] = xrealloc(s->ib[w], s->cib[w] * sizeof(int)); } return s->ib[w]; }
rune *scratch_rbuf(Scratch *s, size_t n) { if (n > s->crb) { s->crb = n * 2 + 16; s->rb = xrealloc(s->rb, s->crb * sizeof(rune)); } return s->rb; }
static int16_t *a16(Scratch *s, size_t n, size_t off)
{
	if (off + n > s->c16) { s->c16 = (off + n) * 2; s->i16 = xrealloc(s->i16, s->c16 * sizeof(int16_t)); }
	return s->i16 + off;
}
static int32_t *a32(Scratch *s, size_t n, size_t off)
{
	if (off + n > s->c32) { s->c32 = (off + n) * 2; s->i32 = xrealloc(s->i32, s->c32 * sizeof(int32_t)); }
	return s->i32 + off;
}

/* window of the text that can hold a match: from one before the first occurrence of pat[0] to the last occurrence of the
 * last pattern character. Returns false when a pattern character is missing (no match at all). */
static bool fuzzy_index(Runes t, const rune *pat, int m, bool cs, bool norm, int *lo, int *hi)
{
	int first = 0, idx = 0, last = 0;
	rune pc = 0;
	for (int p = 0; p < m; p++) {
		pc = pat[p];
		while (idx < t.n && fold(t.r[idx], cs, norm) != pc) idx++;
		if (idx >= t.n) return false;
		if (p == 0 && idx > 0) first = idx - 1;
		last = idx++;
	}
	int end = last + 1;
	for (int i = t.n - 1; i > last; i--)
		if (fold(t.r[i], cs, norm) == pc) { end = i + 1; break; }
	*lo = first; *hi = end;
	return true;
}

static int16_t bonus_at(Runes t, int i)
{
	if (i == 0) return bonus_white;
	return bonus_matrix[class_of(t.r[i - 1])][class_of(t.r[i])];
}

static int calc_score(bool cs, bool norm, Runes t, const rune *pat, int m, int sidx, int eidx, int *pos, int *npos)
{
	int pidx = 0, score = 0, consecutive = 0, first_bonus = 0;
	bool in_gap = false;
	int prev = initial_class;
	int np = 0;
	if (sidx > 0) prev = class_of(t.r[sidx - 1]);
	for (int i = sidx; i < eidx; i++) {
		rune c = t.r[i];
		int cls = class_of(c);
		c = fold(c, cs, norm);
		if (pidx < m && c == pat[pidx]) {
			if (pos) pos[np++] = i;
			score += SCORE_MATCH;
			int bonus = bonus_matrix[prev][cls];
			if (consecutive == 0) first_bonus = bonus;
			else {
				if (bonus >= BONUS_BOUNDARY && bonus > first_bonus) first_bonus = bonus;
				if (first_bonus > bonus) bonus = first_bonus;
				if (BONUS_CONSECUTIVE > bonus) bonus = BONUS_CONSECUTIVE;
			}
			score += pidx == 0 ? bonus * BONUS_FIRST_MULT : bonus;
			in_gap = false;
			consecutive++;
			pidx++;
		} else {
			score += in_gap ? SCORE_GAP_EXT : SCORE_GAP_START;
			in_gap = true;
			consecutive = 0;
			first_bonus = 0;
		}
		prev = cls;
	}
	if (npos) *npos = np;
	return score;
}

MRes fuzzy_v1(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc)
{
	(void)sc;
	if (npos) *npos = 0;
	if (m == 0) return (MRes){0, 0, 0};
	int lo, hi;
	if (!fuzzy_index(t, pat, m, cs, norm, &lo, &hi)) return NOMATCH;
	int pidx = 0, sidx = -1, eidx = -1, n = t.n;
	for (int i = 0; i < n; i++) {
		rune c = fold(t.r[fwd ? i : n - i - 1], cs, norm);
		rune pc = pat[fwd ? pidx : m - pidx - 1];
		if (c == pc) {
			if (sidx < 0) sidx = i;
			if (++pidx == m) { eidx = i + 1; break; }
		}
	}
	if (sidx >= 0 && eidx >= 0) {
		pidx--;
		for (int i = eidx - 1; i >= sidx; i--) {
			rune c = fold(t.r[fwd ? i : n - i - 1], cs, norm);
			rune pc = pat[fwd ? pidx : m - pidx - 1];
			if (c == pc && --pidx < 0) { sidx = i; break; }
		}
		if (!fwd) { int s2 = n - eidx, e2 = n - sidx; sidx = s2; eidx = e2; }
		int score = calc_score(cs, norm, t, pat, m, sidx, eidx, pos, npos);
		return (MRes){sidx, eidx, score};
	}
	return NOMATCH;
}

MRes fuzzy_v2(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc)
{
	if (npos) *npos = 0;
	if (m == 0) return (MRes){0, 0, 0};
	int N = t.n;
	if (m > N) return NOMATCH;
	if ((int64_t)N * m > (1 << 21) || m > 1000) return fuzzy_v1(cs, norm, fwd, t, pat, m, pos, npos, sc);

	int minIdx, maxIdx;
	if (!fuzzy_index(t, pat, m, cs, norm, &minIdx, &maxIdx)) return NOMATCH;
	N = maxIdx - minIdx;

	size_t o16 = 0, o32 = 0;
	int16_t *H0 = a16(sc, (size_t)N, o16); o16 += (size_t)N;
	int16_t *C0 = a16(sc, (size_t)N, o16); o16 += (size_t)N;
	int16_t *B = a16(sc, (size_t)N, o16); o16 += (size_t)N;
	int32_t *F = a32(sc, (size_t)m, o32); o32 += (size_t)m;
	int32_t *T = a32(sc, (size_t)N, o32); o32 += (size_t)N;
	/* a16/a32 may have moved the block while growing: re-derive every pointer from the base */
	H0 = sc->i16; C0 = sc->i16 + N; B = sc->i16 + 2 * N; F = sc->i32; T = sc->i32 + m;
	for (int i = 0; i < N; i++) T[i] = (int32_t)t.r[minIdx + i];

	int maxScore = 0, maxScorePos = 0, pidx = 0, lastIdx = 0;
	rune pchar0 = pat[0], pchar = pat[0];
	int prevH0 = 0, prevClass = initial_class;
	bool inGap = false;
	for (int off = 0; off < N; off++) {
		rune c = (rune)T[off];
		int cls = class_of(c);
		c = fold(c, cs, norm);
		T[off] = (int32_t)c;
		int bonus = bonus_matrix[prevClass][cls];
		B[off] = (int16_t)bonus;
		prevClass = cls;
		if (c == pchar) {
			if (pidx < m) { F[pidx] = off; pidx++; pchar = pat[pidx < m - 1 ? pidx : m - 1]; }
			lastIdx = off;
		}
		if (c == pchar0) {
			int score = SCORE_MATCH + bonus * BONUS_FIRST_MULT;
			H0[off] = (int16_t)score;
			C0[off] = 1;
			if (m == 1 && ((fwd && score > maxScore) || (!fwd && score >= maxScore))) {
				maxScore = score; maxScorePos = off;
				if (fwd && bonus >= BONUS_BOUNDARY) break;
			}
			inGap = false;
		} else {
			int v = prevH0 + (inGap ? SCORE_GAP_EXT : SCORE_GAP_START);
			H0[off] = (int16_t)(v > 0 ? v : 0);
			C0[off] = 0;
			inGap = true;
		}
		prevH0 = H0[off];
	}
	if (pidx != m) return NOMATCH;
	if (m == 1) {
		if (pos) { pos[0] = minIdx + maxScorePos; if (npos) *npos = 1; }
		return (MRes){minIdx + maxScorePos, minIdx + maxScorePos + 1, maxScore};
	}

	int f0 = F[0], width = lastIdx - f0 + 1;
	int16_t *H = a16(sc, (size_t)width * (size_t)m, o16); o16 += (size_t)width * (size_t)m;
	int16_t *C = a16(sc, (size_t)width * (size_t)m, o16);
	H0 = sc->i16; C0 = sc->i16 + N; B = sc->i16 + 2 * N; F = sc->i32; T = sc->i32 + m;
	H = sc->i16 + 3 * N; C = H + (size_t)width * (size_t)m;
	memcpy(H, H0 + f0, (size_t)width * sizeof(int16_t));
	memcpy(C, C0 + f0, (size_t)width * sizeof(int16_t));
	memset(H + width, 0, (size_t)width * (size_t)(m - 1) * sizeof(int16_t));
	memset(C + width, 0, (size_t)width * (size_t)(m - 1) * sizeof(int16_t));

	for (int pi = 1; pi < m; pi++) {
		int f = F[pi];
		rune pc = pat[pi];
		int row = pi * width;
		bool gap = false;
		H[row + f - f0 - 1] = 0;                              /* Hleft[0] = 0 */
		for (int col = f; col <= lastIdx; col++) {
			int hleft = H[row + col - f0 - 1], hdiag = H[row - width + col - f0 - 1], cdiag = C[row - width + col - f0 - 1];
			int s1 = 0, s2 = hleft + (gap ? SCORE_GAP_EXT : SCORE_GAP_START), consecutive = 0;
			if (pc == (rune)T[col]) {
				s1 = hdiag + SCORE_MATCH;
				int b = B[col];
				consecutive = cdiag + 1;
				if (consecutive > 1) {
					int fb = B[col - consecutive + 1];
					if (b >= BONUS_BOUNDARY && b > fb) consecutive = 1;
					else { if (BONUS_CONSECUTIVE > b) b = BONUS_CONSECUTIVE; if (fb > b) b = fb; }
				}
				if (s1 + b < s2) { s1 += B[col]; consecutive = 0; }
				else s1 += b;
			}
			C[row + col - f0] = (int16_t)consecutive;
			gap = s1 < s2;
			int score = s1 > s2 ? s1 : s2;
			if (score < 0) score = 0;
			if (pi == m - 1 && ((fwd && score > maxScore) || (!fwd && score >= maxScore))) { maxScore = score; maxScorePos = col; }
			H[row + col - f0] = (int16_t)score;
		}
	}

	int j = f0;
	if (pos) {
		int i = m - 1, np = 0;
		j = maxScorePos;
		bool preferMatch = true;
		for (;;) {
			int I = i * width, j0 = j - f0, s = H[I + j0], s1 = 0, s2 = 0;
			if (i > 0 && j >= F[i]) s1 = H[I - width + j0 - 1];
			if (j > F[i]) s2 = H[I + j0 - 1];
			int row = i;
			if (s > s1 && (s > s2 || (s == s2 && preferMatch))) {
				pos[np++] = j + minIdx;
				if (i == 0) break;
				i--;
			}
			preferMatch = C[I + j0] > 1 || (row + 1 < m && j < lastIdx && j + 1 >= F[row + 1] && C[I + width + j0 + 1] > 0);
			j--;
		}
		if (npos) *npos = np;
	}
	return (MRes){minIdx + j, minIdx + maxScorePos + 1, maxScore};
}

static MRes exact_impl(bool cs, bool norm, bool fwd, bool boundary, Runes t, const rune *pat, int m, int *pos, int *npos)
{
	if (npos) *npos = 0;
	if (m == 0) return (MRes){0, 0, 0};
	int n = t.n;
	if (n < m) return NOMATCH;
	int lo, hi;
	if (!fuzzy_index(t, pat, m, cs, norm, &lo, &hi)) return NOMATCH;

	int pidx = 0, bestPos = -1, bonus = 0, bbonus = 0, bestBonus = -1;
	for (int index = 0; index < n; index++) {
		int idx_ = fwd ? index : n - index - 1;
		rune c = fold(t.r[idx_], cs, norm);
		int pidx_ = fwd ? pidx : m - pidx - 1;
		bool ok = pat[pidx_] == c;
		if (ok) {
			if (pidx_ == 0) bonus = bonus_at(t, idx_);
			if (boundary) {
				if (fwd && pidx_ == 0) bbonus = bonus;
				else if (!fwd && pidx_ == m - 1) bbonus = idx_ < n - 1 ? bonus_at(t, idx_ + 1) : bonus_white;
				ok = bbonus >= BONUS_BOUNDARY;
				if (ok && pidx_ == 0) ok = idx_ == 0 || class_of(t.r[idx_ - 1]) <= C_DELIM;
				if (ok && pidx_ == m - 1) ok = idx_ == n - 1 || class_of(t.r[idx_ + 1]) <= C_DELIM;
			}
		}
		if (ok) {
			pidx++;
			if (pidx == m) {
				if (bonus > bestBonus) { bestPos = index; bestBonus = bonus; }
				if (bonus >= BONUS_BOUNDARY) break;
				index -= pidx - 1;
				pidx = 0; bonus = 0;
			}
		} else {
			index -= pidx;
			pidx = 0; bonus = 0;
		}
	}
	if (bestPos < 0) return NOMATCH;
	int sidx, eidx;
	if (fwd) { sidx = bestPos - m + 1; eidx = bestPos + 1; }
	else { sidx = n - (bestPos + 1); eidx = n - (bestPos - m + 1); }
	int score;
	if (boundary) {
		score = bonus;
		int deduct = bonus - BONUS_BOUNDARY + 1;
		if (sidx > 0 && t.r[sidx - 1] == '_') { score -= deduct + 1; deduct = 1; }
		if (eidx < n && t.r[eidx] == '_') score -= deduct;
		score += SCORE_MATCH * m + bonus_white * (m + 1);
	} else score = calc_score(cs, norm, t, pat, m, sidx, eidx, NULL, NULL);
	(void)pos;
	return (MRes){sidx, eidx, score};
}
MRes exact_naive(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc) { (void)sc; return exact_impl(cs, norm, fwd, false, t, pat, m, pos, npos); }
MRes exact_boundary(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc) { (void)sc; return exact_impl(cs, norm, fwd, true, t, pat, m, pos, npos); }

int leading_spaces(Runes t) { int i = 0; while (i < t.n && rune_is_space(t.r[i])) i++; return i; }
int trailing_spaces(Runes t) { int i = 0; while (i < t.n && rune_is_space(t.r[t.n - 1 - i])) i++; return i; }

MRes prefix_match(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc)
{
	(void)fwd; (void)pos; (void)sc;
	if (npos) *npos = 0;
	if (m == 0) return (MRes){0, 0, 0};
	int trimmed = rune_is_space(pat[0]) ? 0 : leading_spaces(t);
	if (t.n - trimmed < m) return NOMATCH;
	for (int i = 0; i < m; i++)
		if (fold(t.r[trimmed + i], cs, norm) != pat[i]) return NOMATCH;
	return (MRes){trimmed, trimmed + m, calc_score(cs, norm, t, pat, m, trimmed, trimmed + m, NULL, NULL)};
}
MRes suffix_match(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc)
{
	(void)fwd; (void)pos; (void)sc;
	if (npos) *npos = 0;
	int trimmed = t.n;
	if (m == 0 || !rune_is_space(pat[m - 1])) trimmed -= trailing_spaces(t);
	if (m == 0) return (MRes){trimmed, trimmed, 0};
	int diff = trimmed - m;
	if (diff < 0) return NOMATCH;
	for (int i = 0; i < m; i++)
		if (fold(t.r[i + diff], cs, norm) != pat[i]) return NOMATCH;
	return (MRes){trimmed - m, trimmed, calc_score(cs, norm, t, pat, m, trimmed - m, trimmed, NULL, NULL)};
}
MRes equal_match(bool cs, bool norm, bool fwd, Runes t, const rune *pat, int m, int *pos, int *npos, Scratch *sc)
{
	(void)fwd; (void)pos; (void)sc;
	if (npos) *npos = 0;
	if (m == 0) return NOMATCH;
	int lead = rune_is_space(pat[0]) ? 0 : leading_spaces(t);
	int trail = rune_is_space(pat[m - 1]) ? 0 : trailing_spaces(t);
	if (t.n - lead - trail != m) return NOMATCH;
	for (int i = 0; i < m; i++)
		if (fold(t.r[lead + i], cs, norm) != pat[i]) return NOMATCH;
	return (MRes){lead, lead + m, (SCORE_MATCH + bonus_white) * m + (BONUS_FIRST_MULT - 1) * bonus_white};
}
