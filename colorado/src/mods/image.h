/* image.h - palette extraction: PPM (P6) / PAM (P7) decoding without any library, anything else through `magick` or `convert` when present,
 * k-means in CIE Lab, mapping of the clusters to theme roles. stb-style single header.
 *
 *   co_img_load(path, &img, err, n)            0 ok; images other than P6/P7 need ImageMagick (spawned without a shell, path never parsed as a coder)
 *   co_extract(&img, name, force_dark(-1/0/1), &theme)   k=8 clusters, bg/fg/accent picked by lightness, chroma and population, rest derived
 */
#ifndef COLORADO_IMAGE_H
#define COLORADO_IMAGE_H
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include "gen.h"

typedef struct { int w, h; unsigned char *rgb; } co_img;

static int co_pnm_int(const unsigned char **p, const unsigned char *e) {
	for (;;) { while (*p < e && isspace(**p)) (*p)++; if (*p < e && **p == '#') { while (*p < e && **p != '\n') (*p)++; } else break; }
	if (*p >= e || !isdigit(**p)) return -1;
	long v = 0; while (*p < e && isdigit(**p)) { v = v * 10 + (**p - '0'); if (v > 1 << 28) return -1; (*p)++; }
	return (int)v;
}

static int co_img_pnm(const unsigned char *b, size_t n, co_img *im) {
	const unsigned char *p = b + 2, *e = b + n; int depth = 3, maxv, w, h;
	if (n < 8) return -1;
	if (b[1] == '6') { w = co_pnm_int(&p, e); h = co_pnm_int(&p, e); maxv = co_pnm_int(&p, e); }
	else if (b[1] == '7') { /* PAM header: WIDTH/HEIGHT/DEPTH/MAXVAL/TUPLTYPE lines until ENDHDR */
		w = h = maxv = -1; char key[16];
		for (;;) {
			while (p < e && isspace(*p)) p++;
			if (e - p >= 6 && !memcmp(p, "ENDHDR", 6)) { p += 6; break; }
			if (p >= e) return -1;
			size_t k = 0; while (p < e && !isspace(*p) && k < sizeof key - 1) key[k++] = (char)*p++; key[k] = 0;
			if (!strcmp(key, "WIDTH")) w = co_pnm_int(&p, e); else if (!strcmp(key, "HEIGHT")) h = co_pnm_int(&p, e);
			else if (!strcmp(key, "DEPTH")) depth = co_pnm_int(&p, e); else if (!strcmp(key, "MAXVAL")) maxv = co_pnm_int(&p, e);
			else while (p < e && *p != '\n') p++;
		}
	} else return -1;
	if (w <= 0 || h <= 0 || maxv <= 0 || maxv > 65535 || depth < 3 || depth > 4 || (long long)w * h > 100000000LL) return -1;
	p++; /* single whitespace after header */
	int bps = maxv > 255 ? 2 : 1;
	if ((size_t)(e - p) < (size_t)w * h * depth * bps) return -1;
	im->w = w; im->h = h; im->rgb = malloc((size_t)w * h * 3);
	for (long i = 0; i < (long)w * h; i++) for (int c = 0; c < 3; c++) {
		const unsigned char *q = p + ((size_t)i * depth + c) * bps;
		unsigned v = bps == 2 ? (unsigned)(q[0] << 8 | q[1]) : q[0];
		im->rgb[i * 3 + c] = (unsigned char)(maxv == 255 ? v : v * 255 / maxv);
	}
	return 0;
}

static unsigned char *co_slurp_fd(int fd, size_t *n) {
	size_t cap = 1 << 16, len = 0; unsigned char *b = malloc(cap);
	for (;;) { if (len == cap) { cap *= 2; b = realloc(b, cap); } ssize_t r = read(fd, b + len, cap - len); if (r < 0 && errno == EINTR) continue; if (r <= 0) break; len += (size_t)r; }
	*n = len; return b;
}

static int co_img_via_tool(const char *tool, const char *path, co_img *im) {
	int pf[2]; if (pipe(pf)) return -1;
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		char rel[PATH_MAX + 4]; snprintf(rel, sizeof rel, "%s%s", path[0] == '/' ? "" : "./", path); /* never a coder prefix, never an option */
		dup2(pf[1], 1); close(pf[0]); close(pf[1]);
		int nul = open("/dev/null", O_WRONLY); if (nul >= 0) dup2(nul, 2);
		if (!strcmp(tool, "magick")) execlp("magick", "magick", rel, "-resize", "192x192>", "-depth", "8", "ppm:-", (char *)NULL);
		else execlp("convert", "convert", rel, "-resize", "192x192>", "-depth", "8", "ppm:-", (char *)NULL);
		_exit(127);
	}
	close(pf[1]);
	size_t n; unsigned char *b = co_slurp_fd(pf[0], &n); close(pf[0]);
	int st; waitpid(pid, &st, 0);
	int rc = (WIFEXITED(st) && WEXITSTATUS(st) == 0 && n > 8 && b[0] == 'P') ? co_img_pnm(b, n, im) : -1;
	free(b); return rc;
}

static int co_img_load(const char *path, co_img *im, char *err, size_t en) {
	size_t n; unsigned char *b = (unsigned char *)co_read_file(path, &n);
	if (!b) { snprintf(err, en, "cannot read %s", path); return -1; }
	int rc = -1;
	if (n > 2 && b[0] == 'P' && (b[1] == '6' || b[1] == '7')) { rc = co_img_pnm(b, n, im); if (rc) snprintf(err, en, "%s: malformed PPM/PAM", path); free(b); return rc; }
	free(b);
	if (!co_img_via_tool("magick", path, im) || !co_img_via_tool("convert", path, im)) return 0;
	snprintf(err, en, "%s: not a PPM/PAM and ImageMagick (magick/convert) could not decode it - convert it to PPM first", path);
	return -1;
}

typedef struct { double lab[3]; double n; } co_cl;

static uint32_t co_rng(uint32_t *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }

/* k-means (k-means++ init, fixed seed so the result is reproducible) over at most 20000 sampled pixels */
static int co_kmeans(const co_img *im, co_cl *cl, int k) {
	long total = (long)im->w * im->h, step = total > 20000 ? total / 20000 : 1, m = 0;
	double (*px)[3] = malloc(sizeof(double[3]) * (size_t)(total / step + 1));
	for (long i = 0; i < total; i += step) co_to_lab(co_rgb_of(im->rgb[i * 3], im->rgb[i * 3 + 1], im->rgb[i * 3 + 2]), px[m++]);
	if (m == 0) { free(px); return 0; }
	uint32_t seed = 2463534242u; double *d = malloc(sizeof(double) * (size_t)m); int *as = calloc((size_t)m, sizeof(int));
	memcpy(cl[0].lab, px[co_rng(&seed) % (uint32_t)m], sizeof cl[0].lab);
	int kk = 1;
	for (; kk < k; kk++) {
		double sum = 0;
		for (long i = 0; i < m; i++) { double best = 1e18; for (int c = 0; c < kk; c++) { double x = co_lab_dist2(px[i], cl[c].lab); if (x < best) best = x; } d[i] = best; sum += best; }
		if (sum < 1e-6) break;
		double r = (co_rng(&seed) / 4294967296.0) * sum; long pick = 0;
		for (; pick < m - 1; pick++) { r -= d[pick]; if (r <= 0) break; }
		memcpy(cl[kk].lab, px[pick], sizeof cl[kk].lab);
	}
	k = kk;
	for (int it = 0; it < 25; it++) {
		double acc[16][3] = { { 0 } }; double cnt[16] = { 0 }; int moved = 0;
		for (long i = 0; i < m; i++) {
			int bc = 0; double best = 1e18;
			for (int c = 0; c < k; c++) { double x = co_lab_dist2(px[i], cl[c].lab); if (x < best) { best = x; bc = c; } }
			if (as[i] != bc) { as[i] = bc; moved++; }
			for (int j = 0; j < 3; j++) acc[bc][j] += px[i][j];
			cnt[bc]++;
		}
		for (int c = 0; c < k; c++) if (cnt[c] > 0) for (int j = 0; j < 3; j++) cl[c].lab[j] = acc[c][j] / cnt[c];
		for (int c = 0; c < k; c++) cl[c].n = cnt[c] / (double)m;
		if (!moved && it) break;
	}
	free(px); free(d); free(as);
	return k;
}

static void co_extract(const co_img *im, const char *name, int force_dark, co_theme *t) {
	co_cl cl[16]; memset(cl, 0, sizeof cl);
	int k = co_kmeans(im, cl, 8);
	double mean_l = 0; for (int i = 0; i < k; i++) mean_l += cl[i].lab[0] * cl[i].n;
	int dark = force_dark >= 0 ? force_dark : mean_l < 50;
	int bgi = -1, acci = -1, fgi = -1; double bs = 0;
	for (int i = 0; i < k; i++) if (cl[i].n >= 0.03 && (bgi < 0 || (dark ? cl[i].lab[0] < cl[bgi].lab[0] : cl[i].lab[0] > cl[bgi].lab[0]))) bgi = i;
	if (bgi < 0) bgi = 0;
	for (int i = 0; i < k; i++) {
		double ch = hypot(cl[i].lab[1], cl[i].lab[2]);
		if (ch < 12 || cl[i].lab[0] < 25 || cl[i].lab[0] > 88) continue;
		double sc = ch * sqrt(cl[i].n + 0.02);
		if (sc > bs) { bs = sc; acci = i; }
	}
	double bl[3] = { cl[bgi].lab[0], cl[bgi].lab[1] * 0.5, cl[bgi].lab[2] * 0.5 };
	bl[0] = dark ? fmin(bl[0], 16) : fmax(bl[0], 95);
	co_rgb bg = co_from_lab(bl), accent = acci >= 0 ? co_from_lab(cl[acci].lab) : co_from_hsl(210, 0.6, 0.5), fg;
	int have_fg = 0; double far = 0;
	for (int i = 0; i < k; i++) {
		if (hypot(cl[i].lab[1], cl[i].lab[2]) > 25) continue;
		double dl = fabs(cl[i].lab[0] - bl[0]);
		if (dl > far && dl > 45) { far = dl; fgi = i; }
	}
	if (fgi >= 0) { fg = co_from_lab(cl[fgi].lab); have_fg = 1; } else fg = bg;
	co_theme_derive(t, name, accent, &bg, have_fg ? &fg : NULL, dark);
}
#endif
