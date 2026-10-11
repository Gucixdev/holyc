#ifndef HOLYC_TEXT_OPS_H_
#define HOLYC_TEXT_OPS_H_

/* A small, minimal mix of Perl's text-manipulation reach with grep/
 * ripgrep/awk/cloc's own basic-operations simplicity: count lines/
 * words/occurrences, split text into an array of fields, find matching
 * lines. Built entirely on core/str.h's existing String_View toolkit
 * (sv_chop_by_delim etc) rather than hand-rolled pointer arithmetic -
 * this file's own job is just the higher-level "count/split/search"
 * operations str.h itself doesn't provide, not a competing string
 * primitive layer.
 *
 * Every "into an array" operation takes a caller-provided output array
 * + max capacity and returns the count actually filled, rather than
 * growing a buffer itself - matches this project's own established
 * fixed-capacity-output stance (signal.h's subscriber list, pkg.h's
 * dependency list) for the same reason: the caller already knows a
 * sane upper bound for its own use case, and a real, checkable "ran out
 * of room" signal (the returned count hitting max) beats a hidden
 * unbounded allocation. */

#include "core/base.h"

/* counts '\n' bytes + 1 for a nonempty text with no trailing newline
 * (cloc's own "last line without a trailing newline still counts"
 * convention) - 0 for an empty string. */
size_t text_ops_count_lines(String_View text);

/* counts maximal runs of non-whitespace bytes (space/tab/newline/CR
 * are whitespace) - the same definition `wc -w` uses. */
size_t text_ops_count_words(String_View text);

/* counts non-overlapping occurrences of `needle` in `haystack` (an
 * empty needle matches nowhere - not every position - avoiding the
 * degenerate "empty string appears len+1 times" result nobody wants). */
size_t text_ops_count_occurrences(String_View haystack, String_View needle);

/* splits `text` on every byte equal to `delim`, writing up to
 * `max_parts` fields into `out` (each a slice of `text`, not a copy -
 * valid exactly as long as `text`'s own backing memory is). Returns
 * the number of fields actually written; if `text` would split into
 * more than `max_parts` fields, the LAST written field absorbs
 * everything from its start through the end of `text` (the same
 * "final field keeps whatever's left" behavior csv.h's own row
 * splitting and most real split() implementations converge on with a
 * limit argument), rather than silently dropping the remainder. */
size_t text_ops_split(String_View text, char delim, String_View *out, size_t max_parts);

/* writes up to `max_lines` lines of `text` that CONTAIN `needle`
 * (grep's own basic substring-match mode, not a regex engine - a real,
 * documented scope cut, not a silently incomplete grep) into `out`,
 * each a String_View slice of `text` (no trailing newline included).
 * Returns the number of matching lines actually written - note this
 * can be fewer than the TOTAL number of matching lines in `text` if
 * `max_lines` is hit first, same "caller-provided cap, real count
 * returned" contract as text_ops_split. */
size_t text_ops_grep_lines(String_View text, String_View needle, String_View *out, size_t max_lines);

#endif /* HOLYC_TEXT_OPS_H_ */

#ifdef TEXT_OPS_IMPLEMENTATION
#ifndef TEXT_OPS_IMPLEMENTATION_
#define TEXT_OPS_IMPLEMENTATION_

static i32 text_ops_is_space_(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

size_t text_ops_count_lines(String_View text)
{
    size_t n, i;
    if (text.count == 0) return 0;
    n = 1;
    for (i = 0; i < text.count; i++) {
        if (text.data[i] == '\n') n++;
    }
    /* a trailing newline doesn't start a new (empty, never-written)
     * line - cloc's own convention, matches `wc -l` too */
    if (text.data[text.count - 1] == '\n') n--;
    return n;
}

size_t text_ops_count_words(String_View text)
{
    size_t n = 0, i = 0;
    i32 in_word = 0;
    for (i = 0; i < text.count; i++) {
        i32 is_sp = text_ops_is_space_(text.data[i]);
        if (!is_sp && !in_word) { n++; in_word = 1; }
        else if (is_sp) { in_word = 0; }
    }
    return n;
}

static i32 text_ops_sv_at_(String_View sv, size_t pos, String_View needle)
{
    size_t i;
    if (pos + needle.count > sv.count) return 0;
    for (i = 0; i < needle.count; i++) {
        if (sv.data[pos + i] != needle.data[i]) return 0;
    }
    return 1;
}

size_t text_ops_count_occurrences(String_View haystack, String_View needle)
{
    size_t n = 0, pos = 0;
    if (needle.count == 0) return 0;
    while (pos + needle.count <= haystack.count) {
        if (text_ops_sv_at_(haystack, pos, needle)) { n++; pos += needle.count; }
        else pos++;
    }
    return n;
}

size_t text_ops_split(String_View text, char delim, String_View *out, size_t max_parts)
{
    String_View rest = text;
    size_t n = 0;

    if (max_parts == 0) return 0;

    while (n + 1 < max_parts) {
        String_View chunk;
        if (!sv_try_chop_by_delim(&rest, delim, &chunk)) break;
        out[n++] = chunk;
    }
    /* whatever's left (0 or more full fields' worth, if max_parts was
     * hit) becomes the final field as-is, absorbing any remaining
     * delimiters rather than splitting them further */
    out[n++] = rest;
    return n;
}

size_t text_ops_grep_lines(String_View text, String_View needle, String_View *out, size_t max_lines)
{
    String_View rest = text;
    size_t n = 0;

    while (rest.count > 0 && n < max_lines) {
        String_View line = sv_chop_by_delim(&rest, '\n');
        if (line.count > 0 && line.data[line.count - 1] == '\r') line.count--;
        if (text_ops_count_occurrences(line, needle) > 0) out[n++] = line;
    }
    return n;
}

#endif /* TEXT_OPS_IMPLEMENTATION_ */
#endif /* TEXT_OPS_IMPLEMENTATION */
