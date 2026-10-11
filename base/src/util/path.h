#ifndef HOLYC_PATH_H_
#define HOLYC_PATH_H_

/* API shape borrowed from cwalk (Leonard Ikle, MIT) - basename/dirname/
 * extension/join/normalize/is_absolute cover the large majority of real
 * path-manipulation call sites. NOT a line-for-line port of cwalk's own
 * ~1500-line implementation, though: that size is almost entirely the
 * dual Windows/Unix path-style machinery (drive letters, backslash vs
 * slash, runtime style switching) which a Linux-only project has no use
 * for. This is a fresh, focused, Unix-only ('/' separator, no drive
 * letters) implementation matching cwalk's proven function names and
 * semantics where they apply, not a derivative of its source. Left out
 * entirely: get_absolute/get_relative/change_root/get_intersection/
 * the segment-iteration API/style switching - none of them make sense,
 * or add enough value over hand-rolled call-site code, without the
 * Windows-style half of cwalk's own design they were built alongside. */

#include "types/base.h"

/* own local strlen_ - own copy per this project's standing "no cross-
 * IMPLEMENTATION-guard dependency" rule, avoiding a <string.h>
 * dependency for one small byte loop. */
static size_t path_strlen_(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

/* points *basename at the first character of the last path component
 * (e.g. "c.txt" in "/a/b/c.txt") and sets *length - trailing separators
 * are ignored first ("/a/b/" behaves like "/a/b"). Both outputs are left
 * as (NULL, 0) if the path has no basename (empty, or root "/"). */
void path_get_basename(const char *path, const char **basename, size_t *length);

/* length of the directory portion at the START of `path` (path itself
 * doesn't change) - 0 if there is no directory portion. */
void path_get_dirname(const char *path, size_t *length);

/* *extension points at the '.' of the last path component's extension,
 * *length includes the dot. Returns 0 (and leaves both untouched) if the
 * last component has no extension, or is entirely made of dots (".",
 * "..", "...") - matching the common shell convention that a leading dot
 * makes a hidden file, not an extension. */
i32 path_get_extension(const char *path, const char **extension, size_t *length);
i32 path_has_extension(const char *path);

i32 path_is_absolute(const char *path);

/* joins path_a and path_b with exactly one '/' between them, collapsing
 * runs of separators at the join point. Writes at most buffer_size bytes
 * (always NUL-terminated if buffer_size > 0) and returns the length the
 * result WOULD have been if untruncated - same truncation contract as
 * snprintf, so callers can detect truncation by comparing the return
 * value against buffer_size. */
size_t path_join(const char *path_a, const char *path_b, char *buffer, size_t buffer_size);

/* resolves "." and ".." segments and collapses repeated separators;
 * leading ".." segments in a relative path are kept (there's nothing to
 * resolve them against). Same truncation contract as path_join. `path`
 * and `buffer` may be the same pointer. */
size_t path_normalize(const char *path, char *buffer, size_t buffer_size);

#endif /* HOLYC_PATH_H_ */

#ifdef PATH_IMPLEMENTATION
#ifndef PATH_IMPLEMENTATION_
#define PATH_IMPLEMENTATION_

void path_get_basename(const char *path, const char **basename, size_t *length)
{
    size_t len = path_strlen_(path);
    size_t end = len;
    size_t start;

    while (end > 0 && path[end - 1] == '/') end--;
    if (end == 0) { *basename = (const char *)0; *length = 0; return; }

    start = end;
    while (start > 0 && path[start - 1] != '/') start--;

    if (start == end) { *basename = (const char *)0; *length = 0; return; }

    *basename = path + start;
    *length = end - start;
}

void path_get_dirname(const char *path, size_t *length)
{
    size_t len = path_strlen_(path);
    size_t end = len;

    while (end > 0 && path[end - 1] == '/') end--;
    while (end > 0 && path[end - 1] != '/') end--;
    while (end > 0 && path[end - 1] == '/') end--;

    /* a leading root separator IS the dirname of e.g. "/file" - restore
     * exactly one char for that case rather than reporting an empty
     * dirname for an absolute path's top-level entries. branchless: the
     * condition is 0 or 1, so it doubles as the value to add. */
    end += (size_t)(end == 0 && len > 0 && path[0] == '/');

    *length = end;
}

i32 path_get_extension(const char *path, const char **extension, size_t *length)
{
    const char *basename;
    size_t base_len, i;

    path_get_basename(path, &basename, &base_len);
    if (!basename) return 0;

    /* scan from the end so "archive.tar.gz" yields ".gz", not
     * ".tar.gz" - matches cwalk's own "last dot" rule */
    for (i = base_len; i > 0; i--) {
        if (basename[i - 1] == '.') {
            if (i - 1 == 0) return 0; /* ".gitignore"-style: leading dot, no extension */
            *extension = basename + i - 1;
            *length = base_len - (i - 1);
            return 1;
        }
    }
    return 0;
}

i32 path_has_extension(const char *path)
{
    const char *ext;
    size_t len;
    return path_get_extension(path, &ext, &len);
}

i32 path_is_absolute(const char *path)
{
    return path[0] == '/';
}

size_t path_join(const char *path_a, const char *path_b, char *buffer, size_t buffer_size)
{
    size_t len_a = path_strlen_(path_a);
    size_t len_b = path_strlen_(path_b);
    size_t total;
    size_t i, w;

    while (len_a > 0 && path_a[len_a - 1] == '/') len_a--;
    while (len_b > 0 && path_b[0] == '/') { path_b++; len_b--; }

    total = len_a + (size_t)(len_a > 0 && len_b > 0) + len_b;

    w = 0;
    for (i = 0; i < len_a && w + 1 < buffer_size; i++) buffer[w++] = path_a[i];
    if (len_a > 0 && len_b > 0 && w + 1 < buffer_size) buffer[w++] = '/';
    for (i = 0; i < len_b && w + 1 < buffer_size; i++) buffer[w++] = path_b[i];
    if (buffer_size > 0) buffer[w] = '\0';

    return total;
}

size_t path_normalize(const char *path, char *buffer, size_t buffer_size)
{
    /* segments are collected as (offset, length) pairs into a small
     * fixed stack - a real filesystem path is never thousands of
     * segments deep, and this avoids a second allocator dependency for
     * what is, in practice, a bounded problem. */
    #define PATH_MAX_SEGMENTS_ 256
    size_t starts[PATH_MAX_SEGMENTS_];
    size_t lens[PATH_MAX_SEGMENTS_];
    i32 n_segments = 0;
    i32 is_abs = (path[0] == '/');
    size_t i = 0;
    size_t len = path_strlen_(path);
    size_t total, w;

    while (i < len) {
        size_t start;
        while (i < len && path[i] == '/') i++;
        start = i;
        while (i < len && path[i] != '/') i++;
        if (i == start) continue;

        if (i - start == 1 && path[start] == '.') {
            continue; /* "./" - drop */
        }
        if (i - start == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (n_segments > 0 && !(lens[n_segments - 1] == 2 &&
                                     starts[n_segments - 1] < len &&
                                     path[starts[n_segments - 1]] == '.')) {
                n_segments--; /* pop a real segment */
                continue;
            }
            if (is_abs) continue; /* ".." above root: nothing to do */
            /* relative path with nothing to resolve against - keep it */
        }

        if (n_segments < PATH_MAX_SEGMENTS_) {
            starts[n_segments] = start;
            lens[n_segments] = i - start;
            n_segments++;
        }
    }

    total = (size_t)is_abs;
    for (i = 0; i < (size_t)n_segments; i++) {
        total += lens[i] + (i > 0 ? 1 : 0);
    }
    if (total == 0) total = 1; /* normalize("", ...) and normalize("a/..", ...) both become "." */

    w = 0;
    if (is_abs && w + 1 < buffer_size) buffer[w++] = '/';
    for (i = 0; i < (size_t)n_segments; i++) {
        size_t j;
        if (i > 0 && w + 1 < buffer_size) buffer[w++] = '/';
        for (j = 0; j < lens[i] && w + 1 < buffer_size; j++) buffer[w++] = path[starts[i] + j];
    }
    if (n_segments == 0 && !is_abs && w + 1 < buffer_size) buffer[w++] = '.';
    if (buffer_size > 0) buffer[w] = '\0';

    #undef PATH_MAX_SEGMENTS_
    return total;
}

#endif /* PATH_IMPLEMENTATION_ */
#endif /* PATH_IMPLEMENTATION */
