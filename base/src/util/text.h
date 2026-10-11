#ifndef HOLYC_TEXT_H_
#define HOLYC_TEXT_H_

/* Higher-level text operations built ON str.h's String_View, not a
 * replacement for it - str.h covers single-cut/scan primitives
 * (sv_chop_left, sv_chop_right, sv_trim, sv_index_of); this file covers the "split into
 * many pieces" and whole-buffer transforms a caller building a real
 * text tool (a line-oriented parser, a word-count utility, a simple
 * text editor's own operations) needs on top of those.
 *
 * From the file's own original prose spec ("split merge join trim swap
 * move reverse slice count / ucase lcase / word line block"): trim is
 * already str.h's own sv_trim, not duplicated here. Implemented below:
 * split (by any single delimiter byte), join (the inverse - concatenate
 * with a separator), reverse (in-place byte reversal), count (occurrences
 * of a byte), ucase/lcase (in-place ASCII case conversion), words/lines
 * (split at whitespace/newline granularity specifically, since "split
 * on one exact delimiter" and "split on any run of whitespace" are
 * genuinely different algorithms, not the same one called twice).
 * "swap"/"move"/"slice"/"block" from the original list are NOT
 * implemented here - the terse one-line spec doesn't pin down which of
 * several plausible meanings each one is (slice could be str.h's own
 * sv_chop_left/right already; swap/move/block have no single obvious
 * reading), a real, honest gap rather than a guessed-at implementation
 * that might not match what was actually wanted.
 *
 * Needs STR_IMPLEMENTATION defined too, alongside TEXT_IMPLEMENTATION
 * (text_join calls str.h's own mem_cpy) - same "no auto-forwarding
 * across IMPLEMENTATION guards" rule hashset.h documents for its own
 * HASHTABLE_IMPLEMENTATION dependency. */

#include "core/base.h"
#include "core/base.h"

#ifndef TEXT_MAX_PIECES
#define TEXT_MAX_PIECES 256
#endif

/* splits `sv` on every occurrence of `delim`, writing up to `max_out`
 * pieces into `out` (caller-owned, no allocation) and returning the
 * actual piece count - if the real piece count exceeds max_out, only
 * the first max_out pieces are written but the TRUE count is still
 * returned (same truncation-detection contract as path_join/path_
 * normalize: compare the return value against max_out to detect it). */
size_t text_split(String_View sv, char delim, String_View *out, size_t max_out);

/* splits on any run of one-or-more whitespace (space/tab/\r/\n),
 * collapsing runs and skipping leading/trailing whitespace, so "  a  b "
 * yields exactly {"a","b"}, not {"","a","","b",""}. Same truncation
 * contract as text_split. */
size_t text_words(String_View sv, String_View *out, size_t max_out);

/* splits on '\n', with a trailing '\r' (if present) trimmed off each
 * line - handles both bare LF and CRLF line endings uniformly. A
 * trailing newline does NOT produce an extra empty final line (matches
 * how most line-oriented tools treat "a\nb\n" as 2 lines, not 3). Same
 * truncation contract as text_split. */
size_t text_lines(String_View sv, String_View *out, size_t max_out);

/* concatenates `count` String_Views with `sep` inserted between each
 * (none before the first or after the last) into an arena-owned
 * buffer, returned as a new String_View. */
String_View text_join(Arena *a, const String_View *parts, size_t count, String_View sep);

void text_reverse(char *buf, size_t len); /* in-place byte reversal */

size_t text_count(String_View sv, char needle); /* occurrences of `needle` in sv */

/* in-place ASCII case conversion - bytes outside 'a'-'z'/'A'-'Z' are
 * left untouched (this is a plain byte-value operation, not a real
 * Unicode-aware case fold - a documented, deliberate scope cut, same
 * spirit as base63.h/base64.h's own "no locale dependency" stance
 * elsewhere in this project). */
void text_ucase(char *buf, size_t len);
void text_lcase(char *buf, size_t len);

#endif /* HOLYC_TEXT_H_ */

#ifdef TEXT_IMPLEMENTATION
#ifndef TEXT_IMPLEMENTATION_
#define TEXT_IMPLEMENTATION_

static i32 text_is_space_(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

size_t text_split(String_View sv, char delim, String_View *out, size_t max_out)
{
    size_t count = 0;
    size_t start = 0;
    size_t i;

    for (i = 0; i <= sv.count; i++) {
        if (i == sv.count || sv.data[i] == delim) {
            if (count < max_out) out[count] = sv_from_parts(sv.data + start, i - start);
            count++;
            start = i + 1;
        }
    }
    return count;
}

size_t text_words(String_View sv, String_View *out, size_t max_out)
{
    size_t count = 0;
    size_t i = 0;

    while (i < sv.count) {
        size_t start;
        while (i < sv.count && text_is_space_(sv.data[i])) i++;
        if (i >= sv.count) break;
        start = i;
        while (i < sv.count && !text_is_space_(sv.data[i])) i++;
        if (count < max_out) out[count] = sv_from_parts(sv.data + start, i - start);
        count++;
    }
    return count;
}

size_t text_lines(String_View sv, String_View *out, size_t max_out)
{
    size_t count = 0;
    size_t start = 0;
    size_t i;

    for (i = 0; i < sv.count; i++) {
        if (sv.data[i] == '\n') {
            size_t end = i;
            if (end > start && sv.data[end - 1] == '\r') end--;
            if (count < max_out) out[count] = sv_from_parts(sv.data + start, end - start);
            count++;
            start = i + 1;
        }
    }
    if (start < sv.count) {
        size_t end = sv.count;
        if (end > start && sv.data[end - 1] == '\r') end--;
        if (count < max_out) out[count] = sv_from_parts(sv.data + start, end - start);
        count++;
    }
    return count;
}

String_View text_join(Arena *a, const String_View *parts, size_t count, String_View sep)
{
    size_t total = 0;
    size_t i, w;
    char *buf;

    for (i = 0; i < count; i++) {
        total += parts[i].count;
        if (i + 1 < count) total += sep.count;
    }

    buf = (char *)arena_alloc(a, total);
    w = 0;
    for (i = 0; i < count; i++) {
        mem_cpy(buf + w, parts[i].data, parts[i].count);
        w += parts[i].count;
        if (i + 1 < count) {
            mem_cpy(buf + w, sep.data, sep.count);
            w += sep.count;
        }
    }

    return sv_from_parts(buf, total);
}

void text_reverse(char *buf, size_t len)
{
    size_t i, j;
    for (i = 0, j = len > 0 ? len - 1 : 0; i < j; i++, j--) {
        char t = buf[i];
        buf[i] = buf[j];
        buf[j] = t;
    }
}

size_t text_count(String_View sv, char needle)
{
    size_t n = 0, i;
    for (i = 0; i < sv.count; i++) if (sv.data[i] == needle) n++;
    return n;
}

void text_ucase(char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] = (char)(buf[i] - 'a' + 'A');
    }
}

void text_lcase(char *buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (buf[i] >= 'A' && buf[i] <= 'Z') buf[i] = (char)(buf[i] - 'A' + 'a');
    }
}

#endif /* TEXT_IMPLEMENTATION_ */
#endif /* TEXT_IMPLEMENTATION */
