#ifndef HOLYC_FUZZY_MATCH_H_
#define HOLYC_FUZZY_MATCH_H_

/* fuzzy_match - real fuzzy-subsequence scoring, ported (not copied -
 * this is a from-scratch C implementation, read against fzf's own
 * src/algo/algo.go for the exact recurrence/bonus formulas, same
 * "read for design, write our own" approach this project's make/mmc/
 * imc/amc took for their own reference material) from fzf's own
 * FuzzyMatchV2 algorithm: a Smith-Waterman-style local-alignment DP,
 * not a naive first-subsequence-found match, so "abc" scores "abc.txt"
 * higher than "axbxcx" and a match right after a word-boundary (`/`,
 * `_`, `-`, whitespace, or a camelCase transition) scores higher than
 * the same characters matched mid-word.
 *
 * Real, honest scope cut vs fzf's own algo.go: this ports the GENERAL
 * O(text_len * pattern_len) path only - fzf's own file also has
 * hand-fused fast paths for 1- and 2-character patterns and an ASCII
 * prefilter phase, both pure performance optimizations (not different
 * results), skipped here for a simpler, still-correct implementation.
 * Verified for ranking-equivalence against the real `fzf --filter`
 * binary (see tests/test_fuzzy_match.c) - not just self-consistency. */

#include "core/base.h"
#include "core/arena_alloc.h"
#include <ctype.h>

#define FUZZY_SCORE_MATCH 16
#define FUZZY_SCORE_GAP_START (-3)
#define FUZZY_SCORE_GAP_EXTENSION (-1)
#define FUZZY_BONUS_BOUNDARY 8            /* scoreMatch / 2 */
#define FUZZY_BONUS_NON_WORD 8             /* scoreMatch / 2 */
#define FUZZY_BONUS_CAMEL_123 7             /* bonusBoundary + scoreGapExtension */
#define FUZZY_BONUS_CONSECUTIVE 4           /* -(scoreGapStart + scoreGapExtension) */
#define FUZZY_BONUS_FIRST_CHAR_MULTIPLIER 2
#define FUZZY_BONUS_BOUNDARY_WHITE 10       /* bonusBoundary + 2, "default" scheme */
#define FUZZY_BONUS_BOUNDARY_DELIMITER 9    /* bonusBoundary + 1, "default" scheme */

typedef enum {
    FUZZY_CLASS_WHITE = 0,
    FUZZY_CLASS_NONWORD,
    FUZZY_CLASS_DELIMITER,
    FUZZY_CLASS_LOWER,
    FUZZY_CLASS_UPPER,
    FUZZY_CLASS_LETTER,
    FUZZY_CLASS_NUMBER
} Fuzzy_Char_Class;

/* returns the match score (higher is better), or a negative value if
 * `pattern` isn't a subsequence of `text` at all (never matches).
 * Case-insensitive, ASCII-scoped (same "real, from-scratch, no
 * Unicode class table" cut this project's other from-scratch parsers
 * take - a non-ASCII byte is treated as FUZZY_CLASS_LETTER). */
i32 fuzzy_match(Arena *a, const char *text, size_t text_len, const char *pattern, size_t pattern_len);

/* same as fuzzy_match, with an explicit case_sensitive switch (0 =
 * identical to fuzzy_match's own always-insensitive behavior, 1 =
 * exact-case comparison) - added for real fzf-style smart-case
 * callers (case-sensitive only when the query itself has an
 * uppercase letter) without changing fuzzy_match's own existing
 * signature/behavior for every caller that doesn't need it. */
i32 fuzzy_match_cs(Arena *a, const char *text, size_t text_len, const char *pattern, size_t pattern_len, i32 case_sensitive);

#endif /* HOLYC_FUZZY_MATCH_H_ */

#ifdef FUZZY_MATCH_IMPLEMENTATION
#ifndef FUZZY_MATCH_IMPLEMENTATION_
#define FUZZY_MATCH_IMPLEMENTATION_

static Fuzzy_Char_Class fuzzy_class_of_(char c)
{
    if (c >= 'a' && c <= 'z') return FUZZY_CLASS_LOWER;
    if (c >= 'A' && c <= 'Z') return FUZZY_CLASS_UPPER;
    if (c >= '0' && c <= '9') return FUZZY_CLASS_NUMBER;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') return FUZZY_CLASS_WHITE;
    if (c == '/' || c == ':' || c == ';' || c == ',') return FUZZY_CLASS_DELIMITER;
    if ((unsigned char)c >= 0x80) return FUZZY_CLASS_LETTER; /* non-ASCII: treat as a plain letter, real scope cut */
    return FUZZY_CLASS_NONWORD;
}

static char fuzzy_lower_(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static i32 fuzzy_eq_(char a, char b, i32 case_sensitive)
{
    return case_sensitive ? (a == b) : (fuzzy_lower_(a) == fuzzy_lower_(b));
}

/* bonusFor(prevClass, class) - ported from algo.go's own bonusFor,
 * "default" scheme constants (bonusBoundaryWhite/Delimiter). */
static i32 fuzzy_bonus_for_(Fuzzy_Char_Class prev, Fuzzy_Char_Class cur)
{
    if (cur >= FUZZY_CLASS_NONWORD) {
        if (prev == FUZZY_CLASS_WHITE) return FUZZY_BONUS_BOUNDARY_WHITE;
        if (prev == FUZZY_CLASS_DELIMITER) return FUZZY_BONUS_BOUNDARY_DELIMITER;
        if (prev == FUZZY_CLASS_NONWORD) return FUZZY_BONUS_BOUNDARY;
    }
    if ((prev == FUZZY_CLASS_LOWER && cur == FUZZY_CLASS_UPPER) ||
        (prev != FUZZY_CLASS_NUMBER && cur == FUZZY_CLASS_NUMBER)) {
        return FUZZY_BONUS_CAMEL_123;
    }
    if (cur == FUZZY_CLASS_NONWORD || cur == FUZZY_CLASS_DELIMITER) return FUZZY_BONUS_NON_WORD;
    if (cur == FUZZY_CLASS_WHITE) return FUZZY_BONUS_BOUNDARY_WHITE;
    return 0;
}

i32 fuzzy_match_cs(Arena *a, const char *text, size_t text_len, const char *pattern, size_t pattern_len, i32 case_sensitive)
{
    i16 *bonus;
    i16 *H; /* [text_len * pattern_len], H[i*pattern_len + j] */
    i16 *C; /* consecutive-chain length, same shape as H */
    size_t i, j;
    Fuzzy_Char_Class prev_class;
    i32 best;

    if (pattern_len == 0) return 0;
    if (pattern_len > text_len) return -1;

    /* subsequence pre-check: pattern must occur in order at all, or
     * there is no match to score (mirrors algo.go's own "pidx != M"
     * early-out). */
    {
        size_t pi = 0;
        for (i = 0; i < text_len && pi < pattern_len; i++) {
            if (fuzzy_eq_(text[i], pattern[pi], case_sensitive)) pi++;
        }
        if (pi != pattern_len) return -1;
    }

    bonus = arena_alloc(a, text_len * sizeof(i16));
    prev_class = FUZZY_CLASS_WHITE; /* start-of-string counts as a boundary, same as algo.go's bonusAt(0) */
    for (i = 0; i < text_len; i++) {
        Fuzzy_Char_Class cls = fuzzy_class_of_(text[i]);
        bonus[i] = (i16)fuzzy_bonus_for_(prev_class, cls);
        prev_class = cls;
    }

    H = arena_alloc(a, text_len * pattern_len * sizeof(i16));
    C = arena_alloc(a, text_len * pattern_len * sizeof(i16));

    /* row 0: match pattern[0] anywhere in text, gap-decay elsewhere */
    {
        i32 prev_h = 0;
        i32 in_gap = 0;
        for (i = 0; i < text_len; i++) {
            i32 h;
            if (fuzzy_eq_(text[i], pattern[0], case_sensitive)) {
                h = FUZZY_SCORE_MATCH + bonus[i] * FUZZY_BONUS_FIRST_CHAR_MULTIPLIER;
                C[i * pattern_len + 0] = 1;
                in_gap = 0;
            } else {
                i32 gap_score = prev_h + (in_gap ? FUZZY_SCORE_GAP_EXTENSION : FUZZY_SCORE_GAP_START);
                h = gap_score > 0 ? gap_score : 0;
                C[i * pattern_len + 0] = 0;
                in_gap = 1;
            }
            H[i * pattern_len + 0] = (i16)h;
            prev_h = h;
        }
    }

    /* rows 1..pattern_len-1 */
    for (j = 1; j < pattern_len; j++) {
        i32 in_gap = 0;
        for (i = 0; i < text_len; i++) {
            i32 h_left_prev = (i > 0) ? H[(i - 1) * pattern_len + j] : 0;
            i32 s2 = h_left_prev + (in_gap ? FUZZY_SCORE_GAP_EXTENSION : FUZZY_SCORE_GAP_START);
            i32 s1 = -1000000;
            i32 consecutive = 0;

            if (i > 0 && fuzzy_eq_(text[i], pattern[j], case_sensitive)) {
                i32 h_diag = H[(i - 1) * pattern_len + (j - 1)];
                i32 c_diag = C[(i - 1) * pattern_len + (j - 1)];
                i32 b = bonus[i];
                s1 = h_diag + FUZZY_SCORE_MATCH;
                consecutive = c_diag + 1;
                if (consecutive > 1) {
                    size_t chain_start = i - (size_t)consecutive + 1;
                    i32 fb = bonus[chain_start];
                    if (b >= FUZZY_BONUS_BOUNDARY && b > fb) {
                        consecutive = 1;
                    } else {
                        i32 m = b;
                        if (FUZZY_BONUS_CONSECUTIVE > m) m = FUZZY_BONUS_CONSECUTIVE;
                        if (fb > m) m = fb;
                        b = m;
                    }
                }
                if (s1 + b < s2) {
                    s1 += bonus[i];
                    consecutive = 0;
                } else {
                    s1 += b;
                }
            }

            {
                i32 h = s1 > s2 ? s1 : s2;
                if (h < 0) h = 0;
                in_gap = s1 < s2;
                H[i * pattern_len + j] = (i16)h;
                C[i * pattern_len + j] = (i16)consecutive;
            }
        }
    }

    best = 0;
    for (i = 0; i < text_len; i++) {
        i32 h = H[i * pattern_len + (pattern_len - 1)];
        if (h > best) best = h;
    }
    return best;
}

i32 fuzzy_match(Arena *a, const char *text, size_t text_len, const char *pattern, size_t pattern_len)
{
    return fuzzy_match_cs(a, text, text_len, pattern, pattern_len, 0);
}

#endif /* FUZZY_MATCH_IMPLEMENTATION_ */
#endif /* FUZZY_MATCH_IMPLEMENTATION */
