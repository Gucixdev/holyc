#ifndef HOLYC_SUFFIX_H_
#define HOLYC_SUFFIX_H_

/* Suffix array over a fixed text buffer: sorted[i] = start index of
 * the i-th suffix in lexicographic order. O(n^2 log n) comparison
 * sort (qsort with strcmp-style suffix comparison) - not the linear-
 * time DC3/SA-IS construction; fine for the text sizes this project
 * actually needs a suffix index for. suffix_search does substring
 * lookup via binary search over the sorted suffixes, O(m log n) for a
 * pattern of length m. */

#include "base.h"
#include "arena_alloc.h"
#include "str.h" /* mem_cmp. SUFFIX_IMPLEMENTATION callers must also define
                   * STR_IMPLEMENTATION in the same TU. */
#include <stdlib.h> /* qsort */

typedef struct {
    const char *text;
    size_t text_len;
    u32 *sorted; /* arena-owned, text_len entries: suffix start indices, sorted */
} Suffix_Array;

void suffix_build(Arena *a, Suffix_Array *sa, const char *text, size_t text_len);

/* returns 1 if `pattern` occurs anywhere in the indexed text, 0
 * otherwise. If found and out_pos is non-NULL, *out_pos is set to one
 * matching start offset (not necessarily the first). */
i32 suffix_search(const Suffix_Array *sa, const char *pattern, size_t pattern_len, size_t *out_pos);

#endif /* HOLYC_SUFFIX_H_ */

#ifdef SUFFIX_IMPLEMENTATION
#ifndef SUFFIX_IMPLEMENTATION_
#define SUFFIX_IMPLEMENTATION_

static const char *suffix_cmp_text_;
static size_t suffix_cmp_text_len_;

static i32 suffix_cmp_(const void *pa, const void *pb)
{
    u32 a = *(const u32 *)pa;
    u32 b = *(const u32 *)pb;
    size_t la = suffix_cmp_text_len_ - a;
    size_t lb = suffix_cmp_text_len_ - b;
    size_t min_len = la < lb ? la : lb;
    size_t i;
    for (i = 0; i < min_len; i++) {
        u8 ca = (u8)suffix_cmp_text_[a + i];
        u8 cb = (u8)suffix_cmp_text_[b + i];
        if (ca != cb) return (i32)ca - (i32)cb;
    }
    return (i32)la - (i32)lb;
}

void suffix_build(Arena *a, Suffix_Array *sa, const char *text, size_t text_len)
{
    size_t i;
    sa->text = text;
    sa->text_len = text_len;
    sa->sorted = (u32 *)arena_alloc(a, sizeof(u32) * (text_len > 0 ? text_len : 1));
    for (i = 0; i < text_len; i++) sa->sorted[i] = (u32)i;

    suffix_cmp_text_ = text;
    suffix_cmp_text_len_ = text_len;
    qsort(sa->sorted, text_len, sizeof(u32), suffix_cmp_);
}

static i32 suffix_starts_with_(const Suffix_Array *sa, u32 suffix_start, const char *pattern, size_t pattern_len)
{
    size_t avail = sa->text_len - suffix_start;
    size_t cmp_len = avail < pattern_len ? avail : pattern_len;
    i32 c = mem_cmp(sa->text + suffix_start, pattern, cmp_len);
    if (c != 0) return c;
    return avail < pattern_len ? -1 : 0; /* suffix shorter than pattern but matched so far -> pattern sorts after */
}

i32 suffix_search(const Suffix_Array *sa, const char *pattern, size_t pattern_len, size_t *out_pos)
{
    size_t lo = 0, hi = sa->text_len;

    if (pattern_len == 0 || sa->text_len == 0) return 0;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        i32 c = suffix_starts_with_(sa, sa->sorted[mid], pattern, pattern_len);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }

    if (lo < sa->text_len && suffix_starts_with_(sa, sa->sorted[lo], pattern, pattern_len) == 0) {
        if (out_pos) *out_pos = sa->sorted[lo];
        return 1;
    }
    return 0;
}

#endif /* SUFFIX_IMPLEMENTATION_ */
#endif /* SUFFIX_IMPLEMENTATION */
