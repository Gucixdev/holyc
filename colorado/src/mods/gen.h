/* gen.h - generator core: what a generator produces (co_file), and how a file's content merges into what is already on disk.
 * stb-style single header. Every generator (gen_*.h) only appends co_file records + notes to a co_set; nothing here touches the disk
 * except co_read_file (read only).
 *
 * kinds:  K_WHOLE  colorado owns the file, replace it
 *         K_INI    merge `key=value` lines into [section] of an existing ini, other keys/sections stay untouched
 *         K_BLOCK  replace/append the region between "<open> colorado:begin<close>" and "<open> colorado:end<close>" markers
 */
#ifndef COLORADO_GEN_H
#define COLORADO_GEN_H
#include <stdio.h>
#include "theme.h"

enum { W_CONFIG, W_DATA, W_HOME };
/* absolute XDG config dir; set by main before generating (qt5ct wants absolute paths in its ini) */
static char co_cfg_root[512] = "~/.config";
enum { K_WHOLE, K_INI, K_BLOCK };

typedef struct {
	int where, kind;
	char rel[160];
	char section[48];       /* K_INI */
	char bopen[8], bclose[8]; /* K_BLOCK comment syntax */
	const char *gen;
	co_sb body;
} co_file;

typedef struct { co_file *f; int n, cap; co_sb notes; } co_set;

typedef struct {
	const char *name, *what;
	const char *status;     /* "verified: ..." or "unverified: ..." */
	int verified;           /* part of the default apply set */
	void (*fn)(const co_theme *, co_set *);
} co_gen;

static co_file *co_set_add(co_set *s, const char *gen, int where, const char *rel, int kind) {
	if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 16; s->f = realloc(s->f, (size_t)s->cap * sizeof *s->f); if (!s->f) { perror("colorado"); exit(1); } }
	co_file *f = &s->f[s->n++];
	memset(f, 0, sizeof *f);
	f->where = where; f->kind = kind; f->gen = gen;
	snprintf(f->rel, sizeof f->rel, "%s", rel);
	return f;
}
static co_file *co_set_block(co_set *s, const char *gen, int where, const char *rel, const char *open, const char *close) {
	co_file *f = co_set_add(s, gen, where, rel, K_BLOCK);
	snprintf(f->bopen, sizeof f->bopen, "%s", open); snprintf(f->bclose, sizeof f->bclose, "%s", close);
	return f;
}
static co_file *co_set_ini(co_set *s, const char *gen, int where, const char *rel, const char *section) {
	co_file *f = co_set_add(s, gen, where, rel, K_INI);
	snprintf(f->section, sizeof f->section, "%s", section);
	return f;
}
static void co_set_free(co_set *s) { for (int i = 0; i < s->n; i++) co_sb_free(&s->f[i].body); free(s->f); co_sb_free(&s->notes); memset(s, 0, sizeof *s); }

/* ring of hex strings so a generator can write co_h(t->c[CR_BG]) inside a printf */
static const char *co_h(co_rgb c) { static char ring[64][8]; static int i; i = (i + 1) & 63; co_hex(c, ring[i]); return ring[i]; }
static const char *co_h6(co_rgb c) { return co_h(c) + 1; }
__attribute__((unused)) static const char *co_ha(co_rgb c, int a) { static char ring[64][12]; static int i; i = (i + 1) & 63; snprintf(ring[i], 12, "#%02x%02x%02x%02x", c.r, c.g, c.b, a); return ring[i]; }
static const char *co_ah(co_rgb c, int a) { static char ring[64][12]; static int i; i = (i + 1) & 63; snprintf(ring[i], 12, "#%02x%02x%02x%02x", a, c.r, c.g, c.b); return ring[i]; }

/* ---- merging ---- */
static char *co_read_file(const char *path, size_t *len) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	size_t cap = 4096, n = 0; char *b = malloc(cap);
	for (;;) { size_t r = fread(b + n, 1, cap - n - 1, f); n += r; if (r == 0) break; if (n + 1 >= cap) { cap *= 2; b = realloc(b, cap); } }
	fclose(f); b[n] = 0; if (len) *len = n; return b;
}

static int co_line_is_section(const char *l, size_t n, const char *sec) {
	size_t sl = strlen(sec);
	return n == sl + 2 && l[0] == '[' && !strncmp(l + 1, sec, sl) && l[n - 1] == ']';
}

/* append the not-yet-written keys at the end of the section's content, i.e. before the blank lines that separate it from the next one */
static void co_ini_flush(co_sb *out, int nk, const char **lines, const size_t *llen, char *used) {
	size_t keep = out->n;
	while (keep >= 2 && out->p[keep - 1] == '\n' && out->p[keep - 2] == '\n') keep--;
	char *tail = strdup(out->p + keep); out->n = keep; out->p[keep] = 0;
	if (out->n && out->p[out->n - 1] != '\n') co_sb_add(out, "\n");
	for (int i = 0; i < nk; i++) if (!used[i]) { co_sb_addn(out, lines[i], llen[i]); co_sb_add(out, "\n"); used[i] = 1; }
	co_sb_add(out, tail); free(tail);
}

static void co_merge_ini(const char *old, const co_file *f, co_sb *out) {
	/* body keys: "k=v" lines */
	size_t nb = 0; const char *b = f->body.p ? f->body.p : "";
	int nkeys = 0; for (const char *p = b; *p; p++) if (*p == '\n') nkeys++;
	char *used = calloc((size_t)nkeys + 1, 1);
	const char *keys[256]; size_t klen[256], vend[256]; const char *lines[256]; size_t llen[256]; int nk = 0;
	for (const char *p = b; *p && nk < 256;) {
		const char *nl = strchr(p, '\n'); size_t len = nl ? (size_t)(nl - p) : strlen(p);
		const char *eq = memchr(p, '=', len);
		if (eq) { keys[nk] = p; klen[nk] = (size_t)(eq - p); lines[nk] = p; llen[nk] = len; vend[nk] = len; nk++; }
		p += len + (nl ? 1 : 0);
	}
	(void)nb; (void)vend;
	int insec = 0, seen = 0; const char *p = old;
	while (*p) {
		const char *nl = strchr(p, '\n'); size_t len = nl ? (size_t)(nl - p) : strlen(p);
		size_t tl = len; while (tl && (p[tl - 1] == '\r' || p[tl - 1] == ' ')) tl--;
		int is_hdr = len && p[0] == '[';
		if (is_hdr && insec) { /* leaving our section: flush unused keys */
			co_ini_flush(out, nk, lines, llen, used);
			insec = 0;
		}
		if (is_hdr && co_line_is_section(p, tl, f->section)) { insec = 1; seen = 1; }
		else if (insec) {
			const char *eq = memchr(p, '=', len); int hit = -1;
			if (eq && p[0] != '#' && p[0] != ';') {
				size_t kl = (size_t)(eq - p); while (kl && p[kl - 1] == ' ') kl--;
				for (int i = 0; i < nk; i++) if (klen[i] == kl && !strncmp(keys[i], p, kl)) { hit = i; break; }
			}
			if (hit >= 0) { co_sb_addn(out, lines[hit], llen[hit]); co_sb_add(out, "\n"); used[hit] = 1; p += len + (nl ? 1 : 0); continue; }
		}
		co_sb_addn(out, p, len); if (nl) co_sb_add(out, "\n");
		p += len + (nl ? 1 : 0);
	}
	if (insec) co_ini_flush(out, nk, lines, llen, used);
	else if (!seen) {
		if (out->n && out->p[out->n - 1] != '\n') co_sb_add(out, "\n");
		if (out->n) co_sb_add(out, "\n");
		co_sb_f(out, "[%s]\n", f->section); co_sb_add(out, b);
	}
	free(used);
}

static void co_merge_block(const char *old, const co_file *f, co_sb *out) {
	char beg[64], end[64]; snprintf(beg, sizeof beg, "%s colorado:begin%s", f->bopen, f->bclose); snprintf(end, sizeof end, "%s colorado:end%s", f->bopen, f->bclose);
	const char *b = strstr(old, beg), *e = b ? strstr(b, end) : NULL;
	co_sb blk = { 0 };
	co_sb_f(&blk, "%s\n%s%s%s\n", beg, f->body.p ? f->body.p : "", "", end);
	if (b && e) {
		e += strlen(end); if (*e == '\n') e++;
		co_sb_addn(out, old, (size_t)(b - old)); co_sb_addn(out, blk.p, blk.n); co_sb_add(out, e);
	} else {
		co_sb_add(out, old);
		if (*old && old[strlen(old) - 1] != '\n') co_sb_add(out, "\n");
		if (*old) co_sb_add(out, "\n");
		co_sb_addn(out, blk.p, blk.n);
	}
	co_sb_free(&blk);
}

/* new content of file f given the current content `old` ("" when absent). Caller frees out. */
static void co_merge(const char *old, const co_file *f, co_sb *out) {
	out->n = 0; if (out->p) out->p[0] = 0;
	switch (f->kind) {
	case K_INI: co_merge_ini(old, f, out); break;
	case K_BLOCK: co_merge_block(old, f, out); break;
	default: co_sb_add(out, f->body.p ? f->body.p : ""); break;
	}
}
#endif
