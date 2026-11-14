/* autodemo - scripted, non-interactive demo recorder -> animated WebP / mp4 / webm.
 * See README.md. Build: make. TUI backend = private tmux server + software renderer. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <webp/encode.h>
#include <webp/mux.h>

/* ------------------------------------------------------------ script ---- */
enum { C_RUN, C_TYPE, C_KEY, C_WAIT, C_CAPTION, C_SLIDE, C_TRANS, C_CMOVE, C_CCLICK,
       C_CHIDE, C_CSHOW, C_WINDOW, C_THEME, C_SIZE, C_LOOP, C_FPS, C_FONT, C_TITLE,
       C_ENV, C_CD, C_SETUP, C_NOP };
typedef struct { int op, line, n; char *a[6]; } Cmd;
typedef struct { Cmd *v; int n, cap; } Script;

static const struct { const char *name; int op, min, max; } SPEC[] = {
  {"run",C_RUN,1,1},{"type",C_TYPE,1,2},{"key",C_KEY,1,5},{"wait",C_WAIT,1,1},
  {"caption",C_CAPTION,1,3},{"slide",C_SLIDE,1,3},{"transition",C_TRANS,1,2},
  {"window",C_WINDOW,1,1},{"theme",C_THEME,1,1},{"size",C_SIZE,1,1},{"loop",C_LOOP,0,0},
  {"fps",C_FPS,1,1},{"font",C_FONT,1,1},{"title",C_TITLE,1,1},{"env",C_ENV,1,1},
  {"cd",C_CD,1,1},{"setup",C_SETUP,1,1},{0}};

static int dur_ms(const char *s, int *out) {
  char *e; double v = strtod(s, &e);
  if (e == s || v < 0) return -1;
  if (!*e || !strcmp(e, "ms")) *out = (int)v;
  else if (!strcmp(e, "s")) *out = (int)(v * 1000);
  else return -1;
  return 0;
}

/* split one line into tokens (quotes, \n \" \\ \t escapes, # comments) */
static int tokenize(char *s, char **tok, int max, char *err) {
  int n = 0;
  while (*s) {
    while (isspace((unsigned char)*s)) s++;
    if (!*s || *s == '#') break;
    if (n >= max) { strcpy(err, "too many arguments"); return -1; }
    char *w = s;
    tok[n++] = w;
    if (*s == '"') {
      s++;
      for (;;) {
        if (!*s) { strcpy(err, "unterminated string"); return -1; }
        if (*s == '"') { s++; break; }
        if (*s == '\\' && s[1]) {
          s++; *w++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'e' ? 27 : *s; s++;
        } else *w++ = *s++;
      }
      *w = 0;
    } else {
      while (*s && !isspace((unsigned char)*s)) s++;
      if (*s) *s++ = 0;
    }
  }
  return n;
}

static int valid_key(const char *k) {
  static const char *names[] = {"Enter","Escape","Tab","BTab","Space","BSpace","Up","Down","Left","Right",
    "Home","End","PageUp","PageDown","PgUp","PgDn","DC","IC","F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12",0};
  char b[64]; strncpy(b, k, 63); b[63] = 0;
  char *star = strchr(b, '*'); if (star) { *star = 0; if (atoi(star + 1) < 1) return 0; }
  for (int i = 0; names[i]; i++) if (!strcmp(b, names[i])) return 1;
  if (strlen(b) == 1) return 1;
  if ((b[0] == 'C' || b[0] == 'M' || b[0] == 'S') && b[1] == '-' && b[2]) return 1;
  return 0;
}

/* returns 0 ok; on error prints file:line and returns -1 (continues to report all) */
static int parse_script(const char *path, Script *sc) {
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "autodemo: %s: %s\n", path, strerror(errno)); return -1; }
  char buf[4096]; int line = 0, bad = 0, seen_run = 0;
  while (fgets(buf, sizeof buf, f)) {
    line++;
    char *tok[12], err[128] = "";
    int n;
    { /* run/setup/cd/env/title take the rest of the line verbatim unless it is one quoted string */
      char *w = buf; while (isspace((unsigned char)*w)) w++;
      size_t wl = strcspn(w, " \t\n");
      if ((wl == 3 && !strncmp(w, "run", 3)) || (wl == 5 && !strncmp(w, "setup", 5)) || (wl == 2 && !strncmp(w, "cd", 2)) || (wl == 3 && !strncmp(w, "env", 3))) {
        char *r = w + wl; while (*r == ' ' || *r == '\t') r++;
        size_t rl = strlen(r); while (rl && isspace((unsigned char)r[rl - 1])) r[--rl] = 0;
        if (*r && *r != '#' && *r != '"') { w[wl] = 0; tok[0] = w; tok[1] = r; n = 2; goto tokenized; }
      }
    }
    n = tokenize(buf, tok, 12, err);
    tokenized:
    if (n < 0) { fprintf(stderr, "%s:%d: %s\n", path, line, err); bad = 1; continue; }
    if (!n) continue;
    Cmd c = {.line = line}; int op = -1, argi = 1;
    if (!strcmp(tok[0], "cursor")) {
      const char *sub = n > 1 ? tok[1] : "";
      op = !strcmp(sub, "move") ? C_CMOVE : !strcmp(sub, "click") ? C_CCLICK :
           !strcmp(sub, "hide") ? C_CHIDE : !strcmp(sub, "show") ? C_CSHOW : -1;
      argi = 2;
      if (op < 0) { fprintf(stderr, "%s:%d: cursor needs move|click|hide|show\n", path, line); bad = 1; continue; }
      int need = op == C_CMOVE ? 2 : 0, nn = n - 2;
      if (nn < need || nn > need + 1) { fprintf(stderr, "%s:%d: cursor %s: wrong argument count\n", path, line, sub); bad = 1; continue; }
    } else {
      int k;
      for (k = 0; SPEC[k].name; k++) if (!strcmp(tok[0], SPEC[k].name)) break;
      if (!SPEC[k].name) { fprintf(stderr, "%s:%d: unknown command '%s'\n", path, line, tok[0]); bad = 1; continue; }
      op = SPEC[k].op;
      if (n - 1 < SPEC[k].min || n - 1 > SPEC[k].max) { fprintf(stderr, "%s:%d: %s: wrong argument count (%d..%d)\n", path, line, tok[0], SPEC[k].min, SPEC[k].max); bad = 1; continue; }
    }
    c.op = op; c.n = n - argi;
    for (int i = 0; i < c.n; i++) c.a[i] = strdup(tok[argi + i]);
    int ms, ok = 1;
    switch (op) {
    case C_RUN: seen_run = 1; break;
    case C_TYPE: case C_KEY: case C_CCLICK:
      if (!seen_run) { fprintf(stderr, "%s:%d: input before any 'run'\n", path, line); ok = 0; }
      if (op == C_TYPE && c.n > 1 && atoi(c.a[1]) < 1) { fprintf(stderr, "%s:%d: type speed must be ms per char >= 1\n", path, line); ok = 0; }
      if (op == C_KEY) for (int i = 0; i < c.n; i++) if (!valid_key(c.a[i])) { fprintf(stderr, "%s:%d: bad key '%s'\n", path, line, c.a[i]); ok = 0; }
      break;
    case C_WAIT: if (dur_ms(c.a[0], &ms)) { fprintf(stderr, "%s:%d: bad duration '%s'\n", path, line, c.a[0]); ok = 0; } break;
    case C_SLIDE: if (c.n == 3 && dur_ms(c.a[2], &ms)) { fprintf(stderr, "%s:%d: bad duration '%s'\n", path, line, c.a[2]); ok = 0; } break;
    case C_TRANS:
      if (strcmp(c.a[0], "fade") && strcmp(c.a[0], "slide") && strcmp(c.a[0], "none")) { fprintf(stderr, "%s:%d: transition fade|slide|none\n", path, line); ok = 0; }
      if (c.n == 2 && dur_ms(c.a[1], &ms)) { fprintf(stderr, "%s:%d: bad duration\n", path, line); ok = 0; }
      break;
    case C_CAPTION:
      if (c.n >= 2 && strcmp(c.a[1], "top") && strcmp(c.a[1], "bottom")) { fprintf(stderr, "%s:%d: caption position top|bottom\n", path, line); ok = 0; }
      break;
    case C_SIZE: { int w, h; if (sscanf(c.a[0], "%dx%d", &w, &h) != 2 || w < 10 || h < 3 || w > 400 || h > 200) { fprintf(stderr, "%s:%d: size WxH (cells)\n", path, line); ok = 0; } break; }
    case C_WINDOW:
      if (strcmp(c.a[0], "tui") && strcmp(c.a[0], "gui") && strcmp(c.a[0], "web")) { fprintf(stderr, "%s:%d: window tui|gui|web\n", path, line); ok = 0; }
      break;
    case C_THEME:
      if (strcmp(c.a[0], "dark") && strcmp(c.a[0], "light") && strcmp(c.a[0], "dracula")) { fprintf(stderr, "%s:%d: theme dark|light|dracula\n", path, line); ok = 0; }
      break;
    case C_FPS: if (atoi(c.a[0]) < 1 || atoi(c.a[0]) > 30) { fprintf(stderr, "%s:%d: fps 1..30\n", path, line); ok = 0; } break;
    case C_FONT: if (atoi(c.a[0]) < 8 || atoi(c.a[0]) > 40) { fprintf(stderr, "%s:%d: font px 8..40\n", path, line); ok = 0; } break;
    case C_ENV: if (!strchr(c.a[0], '=')) { fprintf(stderr, "%s:%d: env KEY=VALUE\n", path, line); ok = 0; } break;
    case C_CMOVE: if (c.n == 3 && dur_ms(c.a[2], &ms)) { fprintf(stderr, "%s:%d: bad duration\n", path, line); ok = 0; } break;
    }
    if (!ok) { bad = 1; continue; }
    if (sc->n == sc->cap) { sc->cap = sc->cap ? sc->cap * 2 : 64; sc->v = realloc(sc->v, sc->cap * sizeof(Cmd)); }
    sc->v[sc->n++] = c;
  }
  fclose(f);
  return bad ? -1 : 0;
}

/* ------------------------------------------------------------ images ---- */
typedef struct { int w, h; uint8_t *p; } Img; /* RGBA */
static Img img_new(int w, int h) { Img i = {w, h, calloc((size_t)w * h, 4)}; return i; }
static void img_fill(Img *im, int x, int y, int w, int h, uint32_t c, int a) {
  for (int j = y < 0 ? 0 : y; j < y + h && j < im->h; j++)
    for (int i = x < 0 ? 0 : x; i < x + w && i < im->w; i++) {
      uint8_t *p = im->p + ((size_t)j * im->w + i) * 4;
      p[0] = (p[0] * (255 - a) + (c >> 16 & 255) * a) / 255;
      p[1] = (p[1] * (255 - a) + (c >> 8 & 255) * a) / 255;
      p[2] = (p[2] * (255 - a) + (c & 255) * a) / 255; p[3] = 255;
    }
}
static void img_px(Img *im, int x, int y, uint32_t c, int a) { if (a > 0) img_fill(im, x, y, 1, 1, c, a > 255 ? 255 : a); }
static void img_disc(Img *im, double cx, double cy, double r, uint32_t c, int a, double ring) {
  for (int j = (int)(cy - r - 1); j <= cy + r + 1; j++) for (int i = (int)(cx - r - 1); i <= cx + r + 1; i++) {
    double d = hypot(i - cx, j - cy); if (d > r) continue;
    if (ring > 0 && d < r - ring) continue;
    img_px(im, i, j, c, a);
  }
}
static void img_rrect(Img *im, int x, int y, int w, int h, int r, uint32_t c, int a) {
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
    int dx = i < r ? r - i : i >= w - r ? i - (w - r - 1) : 0, dy = j < r ? r - j : j >= h - r ? j - (h - r - 1) : 0;
    if (dx * dx + dy * dy > r * r) continue;
    img_px(im, x + i, y + j, c, a);
  }
}

/* ------------------------------------------------------------- fonts ---- */
static FT_Library ft;
static FT_Face fmono, fmonob, fsans, fsansb;
static int mono_px, cw, ch, base;

static FT_Face load_face(const char *const *paths) {
  FT_Face f;
  for (; *paths; paths++) if (!FT_New_Face(ft, *paths, 0, &f)) return f;
  return NULL;
}
static int fonts_init(int px) {
  static const char *m[] = {"/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/liberation/LiberationMono-Regular.ttf", 0};
  static const char *mb[] = {"/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/liberation/LiberationMono-Bold.ttf", 0};
  static const char *s[] = {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf", 0};
  static const char *sb[] = {"/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/liberation/LiberationSans-Bold.ttf", 0};
  if (FT_Init_FreeType(&ft)) return -1;
  const char *ov = getenv("AUTODEMO_FONT");
  const char *ovl[] = {ov, 0};
  fmono = ov ? load_face(ovl) : NULL;
  if (!fmono) fmono = load_face(m);
  fmonob = load_face(mb); fsans = load_face(s); fsansb = load_face(sb);
  if (!fmono) return -1;
  if (!fmonob) fmonob = fmono;
  if (!fsans) fsans = fmono;
  if (!fsansb) fsansb = fsans;
  mono_px = px;
  FT_Set_Pixel_Sizes(fmono, 0, px); FT_Set_Pixel_Sizes(fmonob, 0, px);
  if (FT_Load_Char(fmono, 'M', FT_LOAD_DEFAULT)) return -1;
  cw = (int)((fmono->glyph->advance.x + 32) >> 6);
  int asc = (int)((fmono->size->metrics.ascender + 63) >> 6), desc = (int)(-fmono->size->metrics.descender + 63) >> 6;
  ch = asc + desc; base = asc;
  return 0;
}
static uint32_t utf8_next(const unsigned char **sp) {
  const unsigned char *s = *sp; uint32_t c = *s++;
  int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
  if (extra) c &= 0x3f >> extra;
  while (extra-- && (*s & 0xc0) == 0x80) c = (c << 6) | (*s++ & 0x3f);
  *sp = s; return c;
}
static void put_utf8(FILE *f, uint32_t c) {
  if (c < 0x80) fputc(c, f);
  else if (c < 0x800) { fputc(0xc0 | c >> 6, f); fputc(0x80 | (c & 63), f); }
  else if (c < 0x10000) { fputc(0xe0 | c >> 12, f); fputc(0x80 | (c >> 6 & 63), f); fputc(0x80 | (c & 63), f); }
  else { fputc(0xf0 | c >> 18, f); fputc(0x80 | (c >> 12 & 63), f); fputc(0x80 | (c >> 6 & 63), f); fputc(0x80 | (c & 63), f); }
}
/* draw one glyph with origin (x, baseline); returns advance */
static int draw_glyph(Img *im, FT_Face f, uint32_t cp, int x, int bl, uint32_t col, int alpha, int embolden) {
  if (FT_Load_Char(f, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT)) return 0;
  FT_GlyphSlot g = f->glyph; FT_Bitmap *b = &g->bitmap;
  for (unsigned r = 0; r < b->rows; r++) for (unsigned c = 0; c < b->width; c++) {
    int a = b->buffer[r * b->pitch + c] * alpha / 255;
    img_px(im, x + g->bitmap_left + (int)c, bl - g->bitmap_top + (int)r, col, a);
    if (embolden) img_px(im, x + g->bitmap_left + (int)c + 1, bl - g->bitmap_top + (int)r, col, a);
  }
  return (int)((g->advance.x + 32) >> 6);
}
static int text_width(FT_Face f, int px, const char *s) {
  int w = 0; FT_Set_Pixel_Sizes(f, 0, px);
  for (const unsigned char *p = (const unsigned char *)s; *p;) { uint32_t c = utf8_next(&p); if (!FT_Load_Char(f, c, FT_LOAD_DEFAULT)) w += (int)((f->glyph->advance.x + 32) >> 6); }
  return w;
}
static int draw_text(Img *im, FT_Face f, int px, int x, int bl, const char *s, uint32_t col, int alpha) {
  FT_Set_Pixel_Sizes(f, 0, px); int x0 = x;
  for (const unsigned char *p = (const unsigned char *)s; *p;) x += draw_glyph(im, f, utf8_next(&p), x, bl, col, alpha, 0);
  return x - x0;
}

/* ------------------------------------------------------------ themes ---- */
typedef struct { const char *name; uint32_t pal[16], fg, bg, outer, bar; } Theme;
static const Theme THEMES[] = {
  {"dark", {0x1d1f21,0xcc6666,0xb5bd68,0xf0c674,0x81a2be,0xb294bb,0x8abeb7,0xc5c8c6,0x969896,0xde935f,0xb5bd68,0xf0c674,0x81a2be,0xb294bb,0x8abeb7,0xffffff}, 0xc5c8c6, 0x1d1f21, 0x2b2f3a, 0x30343f},
  {"light",{0x000000,0xc82829,0x718c00,0xeab700,0x4271ae,0x8959a8,0x3e999f,0xd6d6d6,0x4d4d4c,0xf5871f,0x718c00,0xeab700,0x4271ae,0x8959a8,0x3e999f,0xffffff}, 0x4d4d4c, 0xfafafa, 0xd8dce5, 0xe6e8ee},
  {"dracula",{0x21222c,0xff5555,0x50fa7b,0xf1fa8c,0xbd93f9,0xff79c6,0x8be9fd,0xf8f8f2,0x6272a4,0xff6e6e,0x69ff94,0xffffa5,0xd6acff,0xff92df,0xa4ffff,0xffffff}, 0xf8f8f2, 0x282a36, 0x1b1c26, 0x343746},
};

/* ------------------------------------------------------------- grid ---- */
typedef struct { uint32_t cp, fg, bg; uint8_t at; } Cell; /* at: 1 bold 2 dim 4 underline 8 reverse */
typedef struct { int cols, rows, cx, cy, cvis; Cell *c; } Grid;

static uint32_t pal256(const Theme *t, int n) {
  if (n < 16) return t->pal[n];
  if (n < 232) { n -= 16; int r = n / 36, g = n / 6 % 6, b = n % 6; static const int l[] = {0,95,135,175,215,255}; return l[r] << 16 | l[g] << 8 | l[b]; }
  int v = 8 + (n - 232) * 10; return v << 16 | v << 8 | v;
}
static void grid_parse(Grid *g, const char *s, const Theme *t) {
  for (int i = 0; i < g->cols * g->rows; i++) g->c[i] = (Cell){' ', t->fg, t->bg, 0};
  int row = 0, col = 0, fgi = -1; uint32_t fg = t->fg, bg = t->bg; uint8_t at = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p && row < g->rows) {
    if (*p == '\n') { row++; col = 0; fg = t->fg; bg = t->bg; at = 0; fgi = -1; p++; continue; }
    if (*p == 27 && p[1] == '[') {
      p += 2; int v[16], nv = 0, cur = 0, have = 0;
      while (*p && !(*p >= 0x40 && *p <= 0x7e)) {
        if (*p == ';' || *p == ':') { if (nv < 16) v[nv++] = have ? cur : 0; cur = 0; have = 0; }
        else if (isdigit(*p)) { cur = cur * 10 + *p - '0'; have = 1; }
        p++;
      }
      if (nv < 16) v[nv++] = have ? cur : 0;
      if (*p == 'm') {
        for (int i = 0; i < nv; i++) {
          int k = v[i];
          if (k == 0) { fg = t->fg; bg = t->bg; at = 0; fgi = -1; }
          else if (k == 1) at |= 1; else if (k == 2) at |= 2; else if (k == 4) at |= 4; else if (k == 7) at |= 8;
          else if (k == 22) at &= ~3; else if (k == 24) at &= ~4; else if (k == 27) at &= ~8;
          else if (k >= 30 && k <= 37) { fg = t->pal[k - 30]; fgi = k - 30; }
          else if (k == 39) { fg = t->fg; fgi = -1; } else if (k == 49) bg = t->bg;
          else if (k >= 40 && k <= 47) bg = t->pal[k - 40];
          else if (k >= 90 && k <= 97) { fg = t->pal[k - 90 + 8]; fgi = -1; }
          else if (k >= 100 && k <= 107) bg = t->pal[k - 100 + 8];
          else if ((k == 38 || k == 48) && i + 1 < nv) {
            uint32_t c = 0; int ok = 0;
            if (v[i + 1] == 5 && i + 2 < nv) { c = pal256(t, v[i + 2] & 255); i += 2; ok = 1; }
            else if (v[i + 1] == 2 && i + 4 < nv) { c = (v[i + 2] & 255) << 16 | (v[i + 3] & 255) << 8 | (v[i + 4] & 255); i += 4; ok = 1; }
            if (ok) { if (k == 38) { fg = c; fgi = -1; } else bg = c; }
          }
        }
      }
      if (*p) p++;
      continue;
    }
    if (*p == 27) { p++; continue; }
    uint32_t cp = utf8_next(&p);
    if (col < g->cols) {
      uint32_t f = fg; if ((at & 1) && fgi >= 0) f = t->pal[fgi + 8];
      g->c[row * g->cols + col] = (Cell){cp, f, bg, at}; col++;
    }
  }
}

/* --------------------------------------------------------- tmux I/O ---- */
static char tm_sock[64], tm_conf[160], tm_home[128];
static int verbose;

/* run argv, optional capture of stdout (malloc'd); returns exit status */
static int run_argv(char *const *argv, char **out) {
  int pfd[2] = {-1, -1};
  if (out && pipe(pfd)) return -1;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (!pid) {
    int dn = open("/dev/null", 2);
    dup2(dn, 0); dup2(out ? pfd[1] : dn, 1); if (!verbose) dup2(dn, 2);
    if (out) { close(pfd[0]); close(pfd[1]); }
    execvp(argv[0], argv); _exit(127);
  }
  if (out) {
    close(pfd[1]); size_t cap = 65536, n = 0; char *b = malloc(cap); ssize_t r;
    while ((r = read(pfd[0], b + n, cap - n - 1)) > 0) { n += r; if (n + 1 >= cap) b = realloc(b, cap *= 2); }
    b[n] = 0; close(pfd[0]); *out = b;
  }
  int st; waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static int tmux(char *out_[], char **out) { /* NULL-terminated argv after the common prefix */
  char *argv[64]; int n = 0;
  argv[n++] = "tmux"; argv[n++] = "-L"; argv[n++] = tm_sock; argv[n++] = "-f"; argv[n++] = tm_conf;
  for (int i = 0; out_[i] && n < 62; i++) argv[n++] = out_[i];
  argv[n] = 0;
  return run_argv(argv, out);
}
#define TMUX(...) tmux((char *[]){__VA_ARGS__, NULL}, NULL)

/* ---------------------------------------------------------- recorder ---- */
typedef struct {
  /* config */
  int cols, rows, fps, fontpx, loop, theme, backend; char title[128];
  char env[16][256]; int nenv; char cwd[512];
  /* canvas */
  int W, H, M, TB, PAD, tx0, ty0;
  /* state */
  Grid gr, prev; int have_session, dirty;
  Img layer, last, frame; int have_last;
  char caption[256]; int cap_top;
  int ptr_on; double px, py; int ripple;
  char trans[8]; int trans_ms;
  long T; double sleep_scale; int preview;
  /* sinks */
  WebPAnimEncoder *enc; WebPAnimEncoderOptions eopt; WebPConfig wcfg; int lossy;
  FILE *ff; int ff_written, ff_fps; char outfmt; /* w webp, v video */
  struct { long ms; char *file; int done; } snap[8]; int nsnap;
  long nframes; char slide_title[256];
} G;

static void msleep(G *g, int ms) {
  if (ms <= 0) return;
  ms = (int)(ms * g->sleep_scale);
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&ts, NULL);
}
static void write_ppm(const Img *im, const char *path) {
  FILE *f = fopen(path, "wb"); if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
  for (int i = 0; i < im->w * im->h; i++) fwrite(im->p + i * 4, 1, 3, f);
  fclose(f);
}
static int vid_flush(G *g, long upto) {
  long target = upto * g->ff_fps / 1000; size_t sz = (size_t)g->last.w * g->last.h * 4;
  while (g->ff_written < target) { if (fwrite(g->last.p, 1, sz, g->ff) != sz) return -1; g->ff_written++; }
  return 0;
}
/* present image for dur ms: dedup identical frames, only changed ones reach the encoder */
static void put(G *g, const Img *im, int dur) {
  size_t sz = (size_t)im->w * im->h * 4;
  if (!g->have_last || memcmp(g->last.p, im->p, sz)) {
    for (int i = 0; i < g->nsnap; i++) if (!g->snap[i].done && g->have_last && g->snap[i].ms < g->T) { write_ppm(&g->last, g->snap[i].file); g->snap[i].done = 1; }
    if (g->ff && g->have_last) vid_flush(g, g->T);
    if (g->enc) {
      WebPPicture pic; WebPPictureInit(&pic); pic.width = im->w; pic.height = im->h; pic.use_argb = 1;
      WebPPictureImportRGBA(&pic, im->p, im->w * 4);
      if (!WebPAnimEncoderAdd(g->enc, &pic, (int)g->T, &g->wcfg)) fprintf(stderr, "autodemo: webp add failed: %s\n", WebPAnimEncoderGetError(g->enc));
      WebPPictureFree(&pic);
    }
    memcpy(g->last.p, im->p, sz); g->have_last = 1; g->nframes++;
  }
  g->T += dur;
}

/* ---- rendering ---- */
static void layout(G *g) {
  const Theme *t = &THEMES[g->theme];
  g->M = 14; g->TB = 30; g->PAD = 8;
  g->W = g->cols * cw + 2 * g->PAD + 2 * g->M; g->H = g->rows * ch + 2 * g->PAD + g->TB + 2 * g->M;
  g->W += g->W & 1; g->H += g->H & 1;
  g->tx0 = g->M + g->PAD; g->ty0 = g->M + g->TB + g->PAD;
  (void)t;
}
static void render_layer(G *g) {
  const Theme *t = &THEMES[g->theme]; Img *L = &g->layer;
  img_fill(L, 0, 0, L->w, L->h, t->outer, 255);
  int ww = L->w - 2 * g->M, wh = L->h - 2 * g->M;
  img_fill(L, g->M + 4, g->M + 8, ww, wh, 0x000000, 60); /* shadow */
  img_rrect(L, g->M, g->M, ww, wh, 8, t->bg, 255);
  img_rrect(L, g->M, g->M, ww, g->TB, 8, t->bar, 255);
  img_fill(L, g->M, g->M + g->TB - 8, ww, 8, t->bar, 255);
  img_disc(L, g->M + 16, g->M + g->TB / 2, 6, 0xff5f57, 255, 0);
  img_disc(L, g->M + 36, g->M + g->TB / 2, 6, 0xfebc2e, 255, 0);
  img_disc(L, g->M + 56, g->M + g->TB / 2, 6, 0x28c840, 255, 0);
  int tw = text_width(fsans, 13, g->title);
  draw_text(L, fsans, 13, g->M + (ww - tw) / 2, g->M + g->TB / 2 + 5, g->title, t->fg, 200);
  if (!g->have_session) return;
  Grid *gr = &g->gr;
  for (int r = 0; r < gr->rows; r++) for (int c = 0; c < gr->cols; c++) {
    Cell k = gr->c[r * gr->cols + c]; uint8_t at = k.at;
    if (gr->cvis && r == gr->cy && c == gr->cx) at ^= 8;
    uint32_t fg = k.fg, bg = k.bg;
    if (at & 8) { uint32_t x = fg; fg = bg; bg = x; }
    int x = g->tx0 + c * cw, y = g->ty0 + r * ch;
    if (bg != t->bg) img_fill(L, x, y, cw, ch, bg, 255);
    if (k.cp > ' ') {
      int a = (at & 2) ? 140 : 255;
      draw_glyph(L, (at & 1) ? fmonob : fmono, k.cp, x, y + base, fg, a, 0);
    }
    if (at & 4) img_fill(L, x, y + base + 2, cw, 1, fg, 255);
  }
}
static void compose(G *g) {
  Img *F = &g->frame; memcpy(F->p, g->layer.p, (size_t)F->w * F->h * 4);
  if (g->ptr_on) {
    static const char *arrow[] = {"X","XX","XWX","XWWX","XWWWX","XWWWWX","XWWWWWX","XWWWWWWX","XWWWWWWWX","XWWWWWWWWX","XWWWWWXXXXX","XWWXWWX","XWX XWWX","XX  XWWX","X    XWWX","     XWWX","      XX",0};
    int ox = (int)g->px, oy = (int)g->py;
    if (g->ripple) img_disc(F, ox, oy, 6 + g->ripple * 7, 0x58a6ff, 200 - g->ripple * 40, 2.5);
    for (int j = 0; arrow[j]; j++) for (int i = 0; arrow[j][i]; i++) {
      char c = arrow[j][i]; if (c == ' ') continue;
      img_fill(F, ox + i * 2, oy + j * 2, 2, 2, c == 'X' ? 0x000000 : 0xffffff, 255);
    }
  }
  if (g->caption[0]) {
    int px = 20, tw = text_width(fsansb, px, g->caption), bw = tw + 36, bh = px + 22;
    if (bw > F->w - 20) bw = F->w - 20;
    int bx = (F->w - bw) / 2, by = g->cap_top ? g->M + g->TB + 14 : F->h - g->M - bh - 14;
    img_rrect(F, bx, by, bw, bh, 10, 0x000000, 205);
    draw_text(F, fsansb, px, bx + (bw - tw) / 2, by + bh / 2 + px / 3, g->caption, 0xffffff, 255);
  }
}
static void present_dirty(G *g, int dur) {
  if (g->dirty || !g->have_last) { compose(g); g->dirty = 0; put(g, &g->frame, dur); }
  else g->T += dur;
}

/* ---- capture ---- */
static int capture(G *g) {
  char *out = NULL;
  int rc = tmux((char *[]){"capture-pane", "-p", "-e", "-t", "d", ";", "display-message", "-p", "-t", "d", "@@CUR@@ #{cursor_x} #{cursor_y} #{cursor_flag}", NULL}, &out);
  if (rc || !out) { free(out); return -1; }
  char *m = strstr(out, "@@CUR@@"); int cx = 0, cy = 0, cf = 0;
  if (m) { sscanf(m + 7, "%d %d %d", &cx, &cy, &cf); *m = 0; }
  grid_parse(&g->gr, out, &THEMES[g->theme]); free(out);
  g->gr.cx = cx; g->gr.cy = cy; g->gr.cvis = cf;
  size_t sz = (size_t)g->gr.cols * g->gr.rows * sizeof(Cell);
  if (memcmp(g->gr.c, g->prev.c, sz) || g->gr.cx != g->prev.cx || g->gr.cy != g->prev.cy || g->gr.cvis != g->prev.cvis) {
    memcpy(g->prev.c, g->gr.c, sz); g->prev.cx = g->gr.cx; g->prev.cy = g->gr.cy; g->prev.cvis = g->gr.cvis;
    render_layer(g); g->dirty = 1;
  }
  return 0;
}
/* one timeline step: let the app settle, sample the screen, show it for ms */
static void tick(G *g, int ms) {
  int settle = ms < 25 ? ms : 25;
  msleep(g, settle);
  if (g->have_session) capture(g);
  present_dirty(g, ms);
  msleep(g, ms - settle);
}
static void wait_ms(G *g, int ms) {
  int step = 1000 / g->fps;
  while (ms > 0) { int s = ms < step ? ms : step; tick(g, s); ms -= s; }
}

/* ---- transitions ---- */
static void transition(G *g, const Img *A, const Img *B) {
  if (!strcmp(g->trans, "none") || g->trans_ms <= 0) { put(g, B, 0); return; }
  int n = g->trans_ms / 40; if (n < 1) n = 1;
  Img M = img_new(A->w, A->h); size_t sz = (size_t)A->w * A->h * 4;
  for (int i = 1; i <= n; i++) {
    double u = (double)i / n; u = u * u * (3 - 2 * u);
    if (!strcmp(g->trans, "fade")) {
      int a = (int)(u * 256);
      for (size_t k = 0; k < sz; k++) M.p[k] = (A->p[k] * (256 - a) + B->p[k] * a) >> 8;
    } else {
      int off = (int)((1 - u) * A->w);
      for (int y = 0; y < A->h; y++) {
        uint8_t *row = M.p + (size_t)y * A->w * 4; const uint8_t *ar = A->p + (size_t)y * A->w * 4, *br = B->p + (size_t)y * A->w * 4;
        /* A slides out to the left, B enters from the right */
        int keep = off; /* columns of A still visible */
        memcpy(row, ar + (size_t)(A->w - keep) * 4, (size_t)keep * 4);
        memcpy(row + (size_t)keep * 4, br, (size_t)(A->w - keep) * 4);
      }
    }
    put(g, &M, 40);
  }
  free(M.p);
}
static void render_slide(G *g, Img *S, const char *title, const char *sub) {
  const Theme *t = &THEMES[g->theme];
  for (int y = 0; y < S->h; y++) { /* subtle vertical gradient */
    uint32_t c = t->bg; int k = y * 24 / S->h;
    int r = (c >> 16 & 255) + k / 2, gg = (c >> 8 & 255) + k / 2, b = (c & 255) + k; if (r > 255) r = 255; if (gg > 255) gg = 255; if (b > 255) b = 255;
    img_fill(S, 0, y, S->w, 1, r << 16 | gg << 8 | b, 255);
  }
  int tp = S->w / 14; if (tp > 56) tp = 56; if (tp < 20) tp = 20;
  while (tp > 16 && text_width(fsansb, tp, title) > S->w - 60) tp -= 2;
  int tw = text_width(fsansb, tp, title);
  int cy = S->h / 2 - (sub && *sub ? tp / 2 : 0);
  draw_text(S, fsansb, tp, (S->w - tw) / 2, cy + tp / 3, title, t->fg, 255);
  img_fill(S, S->w / 2 - 30, cy + tp, 60, 3, t->pal[4], 255);
  if (sub && *sub) {
    int sp = tp / 2 + 2, sw = text_width(fsans, sp, sub);
    draw_text(S, fsans, sp, (S->w - sw) / 2, cy + tp + sp + 20, sub, t->fg, 190);
  }
}

/* ---- input ---- */
static unsigned rng_state = 12345;
static int jitter(int base) { rng_state = rng_state * 1103515245u + 12345u; return base * (70 + (rng_state >> 16) % 61) / 100; }

static int send_text(G *g, const char *s, int speed) {
  for (const unsigned char *p = (const unsigned char *)s; *p;) {
    const unsigned char *q = p; utf8_next(&q);
    char ch_[8]; size_t n = q - p; memcpy(ch_, p, n); ch_[n] = 0; p = q;
    int rc;
    if (ch_[0] == '\n') rc = TMUX("send-keys", "-t", "d", "Enter");
    else rc = TMUX("send-keys", "-t", "d", "-l", "--", ch_);
    if (rc) { fprintf(stderr, "autodemo: tmux send-keys failed\n"); return -1; }
    tick(g, ch_[0] == '\n' ? speed * 3 : jitter(speed));
  }
  return 0;
}
static int send_key(G *g, const char *spec) {
  char b[64]; strncpy(b, spec, 63); b[63] = 0; int rep = 1;
  char *star = strchr(b, '*'); if (star) { *star = 0; rep = atoi(star + 1); }
  for (int i = 0; i < rep; i++) {
    if (TMUX("send-keys", "-t", "d", b)) { fprintf(stderr, "autodemo: tmux send-keys '%s' failed\n", b); return -1; }
    tick(g, rep > 1 ? 90 : 120);
  }
  return 0;
}
static void pointer_move(G *g, double cx, double cy, int ms) {
  double tx = g->tx0 + cx * cw + cw / 2, ty = g->ty0 + cy * ch + ch / 3;
  if (!g->ptr_on) { g->ptr_on = 1; g->px = tx + 60; g->py = ty + 40; }
  double sx = g->px, sy = g->py; int n = ms / 40; if (n < 1) n = 1;
  for (int i = 1; i <= n; i++) {
    double u = (double)i / n; u = u * u * (3 - 2 * u);
    g->px = sx + (tx - sx) * u; g->py = sy + (ty - sy) * u; g->dirty = 1; tick(g, ms / n);
  }
}
static void pointer_click(G *g) {
  int col = (int)((g->px - g->tx0) / cw), row = (int)((g->py - g->ty0) / ch);
  if (col >= 0 && row >= 0 && col < g->cols && row < g->rows) { /* SGR mouse press+release for apps that enabled mouse reporting */
    char seq[64]; snprintf(seq, sizeof seq, "\033[<0;%d;%dM\033[<0;%d;%dm", col + 1, row + 1, col + 1, row + 1);
    TMUX("send-keys", "-t", "d", "-l", "--", seq);
  }
  for (int i = 1; i <= 4; i++) { g->ripple = i; g->dirty = 1; tick(g, 50); }
  g->ripple = 0; g->dirty = 1; tick(g, 120);
}

/* ---- session ---- */
static int start_session(G *g, const char *cmd) {
  char *v[3];
  if (g->have_session) TMUX("kill-server");
  char geom_x[16], geom_y[16]; snprintf(geom_x, 16, "%d", g->cols); snprintf(geom_y, 16, "%d", g->rows);
  char full[4096]; int n = snprintf(full, sizeof full, "exec env");
  for (int i = 0; i < g->nenv; i++) n += snprintf(full + n, sizeof full - n, " '%s'", g->env[i]);
  snprintf(full + n, sizeof full - n, " sh -c '%s'", cmd);
  (void)v;
  char *argv[24]; int a = 0;
  argv[a++] = "new-session"; argv[a++] = "-d"; argv[a++] = "-s"; argv[a++] = "d"; argv[a++] = "-x"; argv[a++] = geom_x; argv[a++] = "-y"; argv[a++] = geom_y;
  if (g->cwd[0]) { argv[a++] = "-c"; argv[a++] = g->cwd; }
  argv[a++] = full; argv[a] = NULL;
  /* single-quote escaping of cmd is the script author's job; reject quotes cleanly */
  if (strchr(cmd, '\'')) {
    /* escape ' as '\'' */
    char esc[2048]; int e = 0; for (const char *p = cmd; *p && e < 2040; p++) { if (*p == '\'') { memcpy(esc + e, "'\\''", 4); e += 4; } else esc[e++] = *p; }
    esc[e] = 0; snprintf(full + n, sizeof full - n, " sh -c '%s'", esc);
  }
  if (tmux(argv, NULL)) { fprintf(stderr, "autodemo: cannot start tmux session\n"); return -1; }
  g->have_session = 1;
  for (int i = 0; i < 40; i++) { msleep(g, 50); if (!capture(g)) break; }
  return 0;
}

/* ---- backends (interface for phase 2) ----
 * A backend turns "input events" into either a cell Grid (tui) or a pixel Img
 * (gui/web). Only tui is implemented; gui = headless wlroots compositor + wtype + grim,
 * web = headless Firefox screenshots. They are selected with `window gui|web`. */
static int backend_available(const char *name) {
  if (!strcmp(name, "tui")) return 1;
  fprintf(stderr, "autodemo: window %s backend is not implemented yet (phase 2, see README)\n", name);
  return 0;
}

/* ------------------------------------------------------------- main ---- */
static void dump_ascii(G *g, const Cmd *c) {
  printf("--- t=%.1fs line %d: %s %s\n", g->T / 1000.0, c->line, c->op == C_RUN ? "run" : c->op == C_SLIDE ? "slide" : "wait", c->a[0] ? c->a[0] : "");
  if (!g->have_session) return;
  for (int r = 0; r < g->gr.rows; r++) {
    int last = g->gr.cols - 1; while (last >= 0 && g->gr.c[r * g->gr.cols + last].cp <= ' ') last--;
    for (int k = 0; k <= last; k++) put_utf8(stdout, g->gr.c[r * g->gr.cols + k].cp ? g->gr.c[r * g->gr.cols + k].cp : ' ');
    putchar('\n');
  }
}

static int execute(G *g, Script *sc) {
  for (int i = 0; i < sc->n; i++) {
    Cmd *c = &sc->v[i]; int ms;
    switch (c->op) {
    case C_RUN: if (start_session(g, c->a[0])) return -1; g->dirty = 1; if (!g->title[0]) snprintf(g->title, sizeof g->title, "%.100s", c->a[0]); render_layer(g); g->dirty = 1; tick(g, 200); if (g->preview) dump_ascii(g, c); break;
    case C_SETUP: {
      char *argv[] = {"sh", "-c", c->a[0], NULL};
      if (run_argv(argv, NULL)) { fprintf(stderr, "line %d: setup command failed\n", c->line); return -1; }
      break; }
    case C_TYPE: if (send_text(g, c->a[0], c->n > 1 ? atoi(c->a[1]) : 55)) return -1; break;
    case C_KEY: for (int k = 0; k < c->n; k++) if (send_key(g, c->a[k])) return -1; break;
    case C_WAIT: dur_ms(c->a[0], &ms); wait_ms(g, ms); if (g->preview) dump_ascii(g, c); break;
    case C_CAPTION:
      snprintf(g->caption, sizeof g->caption, "%s", !strcmp(c->a[0], "off") ? "" : c->a[0]);
      g->cap_top = c->n > 1 && !strcmp(c->a[1], "top"); g->dirty = 1; tick(g, 150); break;
    case C_SLIDE: {
      ms = 2000; if (c->n == 3) dur_ms(c->a[2], &ms);
      if (g->have_session) capture(g);
      compose(g); Img A = img_new(g->W, g->H); memcpy(A.p, g->frame.p, (size_t)g->W * g->H * 4);
      Img S = img_new(g->W, g->H); render_slide(g, &S, c->a[0], c->n > 1 ? c->a[1] : "");
      transition(g, &A, &S); put(g, &S, ms);
      if (g->have_session) { capture(g); render_layer(g); }
      compose(g); transition(g, &S, &g->frame); g->dirty = 1;
      if (g->preview) dump_ascii(g, c);
      free(A.p); free(S.p); break; }
    case C_TRANS: snprintf(g->trans, sizeof g->trans, "%s", c->a[0]); g->trans_ms = 300; if (c->n == 2) dur_ms(c->a[1], &g->trans_ms); break;
    case C_CMOVE: ms = 500; if (c->n == 3) dur_ms(c->a[2], &ms); pointer_move(g, atof(c->a[0]), atof(c->a[1]), ms); break;
    case C_CCLICK: if (!g->ptr_on) { fprintf(stderr, "line %d: click before cursor move\n", c->line); return -1; } pointer_click(g); break;
    case C_CHIDE: g->ptr_on = 0; g->dirty = 1; tick(g, 50); break;
    case C_CSHOW: g->ptr_on = 1; g->dirty = 1; tick(g, 50); break;
    default: break; /* header directives were applied earlier */
    }
  }
  return 0;
}

static void apply_header(G *g, Script *sc) {
  g->cols = 100; g->rows = 30; g->fps = 10; g->fontpx = 15; g->theme = 0; g->backend = 0;
  strcpy(g->trans, "fade"); g->trans_ms = 300;
  for (int i = 0; i < sc->n; i++) {
    Cmd *c = &sc->v[i];
    switch (c->op) {
    case C_SIZE: sscanf(c->a[0], "%dx%d", &g->cols, &g->rows); break;
    case C_THEME: for (int k = 0; k < 3; k++) if (!strcmp(c->a[0], THEMES[k].name)) g->theme = k; break;
    case C_FPS: g->fps = atoi(c->a[0]); break;
    case C_FONT: g->fontpx = atoi(c->a[0]); break;
    case C_LOOP: g->loop = 1; break;
    case C_TITLE: snprintf(g->title, sizeof g->title, "%s", c->a[0]); break;
    case C_WINDOW: g->backend = !strcmp(c->a[0], "tui") ? 0 : 1; break;
    case C_ENV: if (g->nenv < 16) snprintf(g->env[g->nenv++], 256, "%s", c->a[0]); break;
    case C_CD: snprintf(g->cwd, sizeof g->cwd, "%s", c->a[0]); break;
    }
  }
}

static long estimate_ms(Script *sc) {
  long t = 0; int ms;
  for (int i = 0; i < sc->n; i++) {
    Cmd *c = &sc->v[i];
    switch (c->op) {
    case C_WAIT: dur_ms(c->a[0], &ms); t += ms; break;
    case C_SLIDE: ms = 2000; if (c->n == 3) dur_ms(c->a[2], &ms); t += ms + 600; break;
    case C_TYPE: t += (long)strlen(c->a[0]) * (c->n > 1 ? atoi(c->a[1]) : 55); break;
    case C_KEY: t += 120 * c->n; break;
    case C_CMOVE: ms = 500; if (c->n == 3) dur_ms(c->a[2], &ms); t += ms; break;
    case C_CCLICK: t += 320; break;
    case C_RUN: t += 200; break;
    }
  }
  return t;
}

static int usage(void) {
  fprintf(stderr, "usage: autodemo render X.demo -o OUT.(webp|mp4|webm) [-q N] [-v] [--snap MS FILE.ppm]\n"
                  "       autodemo preview X.demo      (run and dump key screens as ASCII)\n"
                  "       autodemo check X.demo        (validate the script)\n"
                  "  -q N   webp: 0 = lossless (default), 1..100 = lossy quality\n");
  return 2;
}

int main(int argc, char **argv) {
  if (argc < 3) return usage();
  const char *mode = argv[1], *in = argv[2], *out = NULL; int q = 0;
  G *g = calloc(1, sizeof *g);
  for (int i = 3; i < argc; i++) {
    if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "-q") && i + 1 < argc) q = atoi(argv[++i]);
    else if (!strcmp(argv[i], "-v")) verbose = 1;
    else if (!strcmp(argv[i], "--snap") && i + 2 < argc && g->nsnap < 8) { g->snap[g->nsnap].ms = atol(argv[++i]); g->snap[g->nsnap++].file = argv[++i]; }
    else return usage();
  }
  Script sc = {0};
  if (parse_script(in, &sc)) return 1;
  apply_header(g, &sc);
  if (!strcmp(mode, "check")) {
    printf("%s: ok, %d commands, %dx%d cells, ~%.1fs\n", in, sc.n, g->cols, g->rows, estimate_ms(&sc) / 1000.0);
    return 0;
  }
  int render = !strcmp(mode, "render"), preview = !strcmp(mode, "preview");
  if (!render && !preview) return usage();
  if (render && !out) return usage();
  for (int i = 0; i < sc.n; i++) if (sc.v[i].op == C_WINDOW && !backend_available(sc.v[i].a[0])) return 1;
  if (fonts_init(g->fontpx)) { fprintf(stderr, "autodemo: no usable monospace TTF found (set AUTODEMO_FONT=/path.ttf)\n"); return 1; }
  layout(g);
  g->preview = preview; g->sleep_scale = preview ? 0.3 : 1.0;
  g->gr = (Grid){g->cols, g->rows, 0, 0, 0, calloc(g->cols * g->rows, sizeof(Cell))};
  g->prev = g->gr; g->prev.c = calloc(g->cols * g->rows, sizeof(Cell));
  g->layer = img_new(g->W, g->H); g->last = img_new(g->W, g->H); g->frame = img_new(g->W, g->H);
  render_layer(g); g->dirty = 1;

  /* private tmux server + private HOME: never touches the real user's config/sessions */
  snprintf(tm_sock, sizeof tm_sock, "autodemo-%d", getpid());
  snprintf(tm_home, sizeof tm_home, "/tmp/autodemo-home-XXXXXX");
  if (!mkdtemp(tm_home)) { perror("mkdtemp"); return 1; }
  snprintf(tm_conf, sizeof tm_conf, "%s/tmux.conf", tm_home);
  FILE *cf = fopen(tm_conf, "w");
  fprintf(cf, "set -g status off\nset -g remain-on-exit on\nset -g escape-time 0\nset -g default-terminal xterm-256color\nset -g history-limit 2000\n");
  fclose(cf);
  char xdg[160];
  setenv("HOME", tm_home, 1); unsetenv("TMUX"); setenv("LANG", "C.UTF-8", 1); setenv("LC_ALL", "C.UTF-8", 1);
  snprintf(xdg, sizeof xdg, "%s/.config", tm_home); setenv("XDG_CONFIG_HOME", xdg, 1);
  snprintf(xdg, sizeof xdg, "%s/.local/share", tm_home); setenv("XDG_DATA_HOME", xdg, 1);
  snprintf(xdg, sizeof xdg, "%s/.local/state", tm_home); setenv("XDG_STATE_HOME", xdg, 1);
  snprintf(xdg, sizeof xdg, "%s/.cache", tm_home); setenv("XDG_CACHE_HOME", xdg, 1);
  setenv("PS1", "$ ", 1); setenv("COLORTERM", "truecolor", 1);
  if (g->nenv < 16 && !getenv("AUTODEMO_KEEP_ENV")) {}

  const char *ext = out ? strrchr(out, '.') : NULL;
  if (render && ext && (!strcmp(ext, ".mp4") || !strcmp(ext, ".webm"))) {
    char cmd[1024]; g->ff_fps = 30;
    snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s %dx%d -r 30 -i - -an %s -pix_fmt yuv420p '%s'", g->W, g->H,
             !strcmp(ext, ".mp4") ? "-c:v libx264 -crf 24 -preset slow -movflags +faststart" : "-c:v libvpx-vp9 -crf 34 -b:v 0", out);
    g->ff = popen(cmd, "w"); if (!g->ff) { fprintf(stderr, "autodemo: ffmpeg not available\n"); return 1; }
    g->outfmt = 'v';
  } else if (render) {
    WebPAnimEncoderOptionsInit(&g->eopt); g->eopt.anim_params.loop_count = g->loop ? 0 : 1; g->eopt.minimize_size = 1; g->eopt.allow_mixed = 1;
    WebPConfigInit(&g->wcfg); g->wcfg.lossless = q == 0; g->wcfg.quality = q ? q : 90; g->wcfg.method = 4;
    if (q == 0) g->wcfg.exact = 0;
    g->enc = WebPAnimEncoderNew(g->W, g->H, &g->eopt); g->outfmt = 'w';
  }

  int rc = execute(g, &sc);
  if (g->have_session) TMUX("kill-server");
  if (rc) return 1;
  /* hold the last frame briefly so loops do not snap */
  present_dirty(g, 600);
  for (int i = 0; i < g->nsnap; i++) if (!g->snap[i].done && g->have_last) write_ppm(&g->last, g->snap[i].file);
  if (g->ff) { vid_flush(g, g->T); if (pclose(g->ff)) { fprintf(stderr, "autodemo: ffmpeg failed\n"); return 1; } }
  if (g->enc) {
    WebPAnimEncoderAdd(g->enc, NULL, (int)g->T, NULL);
    WebPData data; WebPDataInit(&data);
    if (!WebPAnimEncoderAssemble(g->enc, &data)) { fprintf(stderr, "autodemo: webp assemble failed\n"); return 1; }
    FILE *f = fopen(out, "wb"); if (!f || fwrite(data.bytes, 1, data.size, f) != data.size) { perror(out); return 1; }
    fclose(f); WebPDataClear(&data); WebPAnimEncoderDelete(g->enc);
  }
  if (render) {
    FILE *f = fopen(out, "rb"); long sz = 0; if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
    printf("%s: %dx%d, %.1fs, %ld unique frames, %.1f KiB\n", out, g->W, g->H, g->T / 1000.0, g->nframes, sz / 1024.0);
  }
  return 0;
}
