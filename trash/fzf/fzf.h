/* fzf.h - shared declarations of the C fzf (see README.md). Plain C99 + POSIX, libc only. */
#ifndef FZF_H
#define FZF_H
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

typedef uint32_t rune;

/* ---------------------------------------------------------------- util.c */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

typedef struct { char *p; size_t len, cap; } Buf;
void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_reset(Buf *b);
void buf_addn(Buf *b, const char *s, size_t n);
void buf_adds(Buf *b, const char *s);
void buf_addc(Buf *b, char c);
void buf_printf(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *buf_take(Buf *b);            /* NUL-terminated, caller frees, buffer reset */

int u8dec(const char *s, size_t n, rune *out);       /* bytes consumed (>=1); invalid -> U+FFFD, 1 byte */
size_t u8enc(rune r, char *out);
int rune_width(rune r);                              /* 0, 1 or 2 cells */
int str_width(const char *s, size_t n);
size_t utf8_to_runes(const char *s, size_t n, rune *out);   /* out needs n slots; returns count */
size_t utf8_len(const char *s, size_t n);                    /* number of runes */
rune rune_lower(rune r);
rune rune_upper(rune r);
bool rune_is_space(rune r);
void locale_init(void);

/* ---------------------------------------------------------------- algo.c */
typedef struct { int start, end, score; } MRes;      /* start < 0 = no match */
typedef struct { const rune *r; int n; } Runes;
typedef struct Scratch Scratch;
Scratch *scratch_new(void);
void scratch_free(Scratch *s);
bool algo_init(const char *scheme);                  /* "default", "path", "history" */
rune normalize_rune(rune r);
void normalize_runes(rune *r, int n);
typedef MRes (*AlgoFn)(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes fuzzy_v2(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes fuzzy_v1(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes exact_naive(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes exact_boundary(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes prefix_match(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes suffix_match(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
MRes equal_match(bool cs, bool norm, bool fwd, Runes text, const rune *pat, int m, int *pos, int *npos, Scratch *sc);
int leading_spaces(Runes t);
int trailing_spaces(Runes t);

/* ---------------------------------------------------------------- items and search (pattern.c) */
typedef struct { int begin, end; } Range;            /* 1-based, negative from the end, 0 = open end (..) */
typedef struct { char *str; size_t len; regex_t re; bool has_re; } Delim;   /* str NULL and no regex = the awk default */
bool range_parse(const char *s, Range *r);
int ranges_parse(const char *spec, Range **out);     /* comma separated; -1 on error */
void delim_set(Delim *d, const char *s);

typedef struct { uint32_t off, len; uint32_t fg, bg; uint16_t attr; } AnsiSpan;   /* colour of a byte range of Item.text */

typedef struct Item {
	char *orig;  uint32_t olen;      /* the line as it was read (printed as the result) */
	char *text;  uint32_t tlen;      /* what is shown and matched: --with-nth applied, ANSI codes removed */
	uint32_t index;                  /* input order */
	uint32_t nrunes;                 /* runes in text */
	uint16_t trim;                   /* length without surrounding blanks, in runes */
	AnsiSpan *ansi; uint32_t nansi;  /* colours found in the line (--ansi) */
	char *id;                        /* --id-nth key, NULL = the text */
	uint8_t flags;
} Item;
#define ITEM_SEL 1

typedef enum { CR_SCORE, CR_CHUNK, CR_LENGTH, CR_BEGIN, CR_END, CR_PATHNAME } Crit;

typedef struct Opts Opts;                            /* defined in opts.h */
typedef struct Pattern Pattern;
typedef struct { uint16_t pts[4]; int *pos; int npos; int minb, mine, maxe; } MatchOut;

Pattern *pattern_new(const Opts *o, const char *query);
void pattern_free(Pattern *p);
bool pattern_empty(const Pattern *p);
bool pattern_sortable(const Pattern *p);
bool pattern_match(const Pattern *p, Item *it, Scratch *sc, bool want_pos, MatchOut *out);

typedef struct { uint32_t idx; uint64_t key; } Hit;
/* runs the pattern over items[0..n), returns the hits sorted by rank (or in input order when the pattern is empty
 * or sorting is off). *nhits is the count; the caller frees the array. */
Hit *search_items(const Opts *o, Pattern *p, Item **items, size_t n, size_t *nhits);

bool ansi_extract(const char *s, size_t n, Buf *out, AnsiSpan **spans, uint32_t *nspans, uint32_t *cap);
rune *scratch_rbuf(Scratch *s, size_t n);
int *scratch_ibuf(Scratch *s, int which, size_t n);   /* three reusable int buffers per scratch */
Item *item_new(const Opts *o, const char *line, size_t len);
void item_free(Item *it);

#endif
