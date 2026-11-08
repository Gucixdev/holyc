/* unit_test.c - colour maths against known WCAG values, theme parse/emit round trip, derivation, ini/block merge, image extraction.
 * Pure functions only; touches no files except reading ../themes. */
#define _GNU_SOURCE
#include "apply.h"
#include "gens.h"
#include "image.h"

static int pass, fail;
#define CHECK(c, ...) do { if (c) pass++; else { fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); puts(""); } } while (0)
static co_rgb H(const char *s) { co_rgb c; if (co_hex_parse(s, &c)) { printf("bad hex in test %s\n", s); exit(2); } return c; }
static int near(double a, double b, double e) { return fabs(a - b) <= e; }

static void t_wcag(void) {
	CHECK(near(co_contrast(H("#000"), H("#fff")), 21.0, 0.001), "black/white");
	CHECK(near(co_contrast(H("#fff"), H("#fff")), 1.0, 0.001), "same");
	CHECK(near(co_contrast(H("#777777"), H("#ffffff")), 4.48, 0.01), "777 on white %.3f", co_contrast(H("#777"), H("#fff")));
	CHECK(near(co_contrast(H("#767676"), H("#ffffff")), 4.54, 0.01), "767676 on white");
	CHECK(near(co_contrast(H("#595959"), H("#ffffff")), 7.0, 0.02), "595959 on white");
	CHECK(near(co_contrast(H("#808080"), H("#ffffff")), 3.95, 0.01), "808080 on white");
	CHECK(near(co_contrast(H("#ff0000"), H("#ffffff")), 4.0, 0.01), "red on white %.3f", co_contrast(H("#f00"), H("#fff")));
	CHECK(near(co_contrast(H("#0000ff"), H("#ffffff")), 8.59, 0.01), "blue on white");
	CHECK(near(co_contrast(H("#ffffff"), H("#767676")), co_contrast(H("#767676"), H("#ffffff")), 1e-12), "symmetric");
	CHECK(near(co_lum(H("#ffffff")), 1.0, 1e-9) && near(co_lum(H("#000000")), 0.0, 1e-9), "luminance extremes");
	CHECK(near(co_lum(H("#00ff00")), 0.7152, 1e-4), "green luminance");
}
static void t_hex(void) {
	co_rgb c; char b[8];
	CHECK(!co_hex_parse("#1E1e2E", &c) && c.r == 0x1e && c.g == 0x1e && c.b == 0x2e, "mixed case");
	CHECK(!co_hex_parse("abc", &c) && c.r == 0xaa && c.g == 0xbb && c.b == 0xcc, "short form");
	CHECK(co_hex_parse("#12345", &c) == -1, "5 digits");
	CHECK(co_hex_parse("#1234567", &c) == -1, "7 digits");
	CHECK(co_hex_parse("#gg0000", &c) == -1, "non hex");
	CHECK(co_hex_parse("", &c) == -1, "empty");
	co_hex(H("#0a0B0c"), b); CHECK(!strcmp(b, "#0a0b0c"), "hex out %s", b);
}
static void t_spaces(void) {
	int bad = 0;
	for (int r = 0; r < 256; r += 15) for (int g = 0; g < 256; g += 17) for (int b = 0; b < 256; b += 15) {
		co_rgb c = co_rgb_of(r, g, b), d; double h, s, l, lab[3];
		co_to_hsl(c, &h, &s, &l); d = co_from_hsl(h, s, l); if (abs(d.r - r) > 1 || abs(d.g - g) > 1 || abs(d.b - b) > 1) bad++;
		co_to_lab(c, lab); d = co_from_lab(lab); if (abs(d.r - r) > 1 || abs(d.g - g) > 1 || abs(d.b - b) > 1) bad++;
	}
	CHECK(!bad, "hsl/lab round trip drifted on %d samples", bad);
	double lab[3]; co_to_lab(H("#ffffff"), lab); CHECK(near(lab[0], 100, 0.1) && near(lab[1], 0, 0.1), "white lab");
	co_to_lab(H("#000000"), lab); CHECK(near(lab[0], 0, 0.1), "black lab");
	co_to_lab(H("#ff0000"), lab); CHECK(near(lab[0], 53.24, 0.1) && near(lab[1], 80.09, 0.2) && near(lab[2], 67.20, 0.2), "red lab %.2f %.2f %.2f", lab[0], lab[1], lab[2]);
}

static void t_theme(void) {
	co_theme t; char err[200];
	co_sb good = { 0 }; co_theme d; co_theme_derive(&d, "x", H("#c0392b"), NULL, NULL, 1); co_theme_emit(&d, &good);
	CHECK(!co_theme_parse(good.p, &t, err, sizeof err), "parse own emit: %s", err);
	co_sb again = { 0 }; co_theme_emit(&t, &again); CHECK(!strcmp(good.p, again.p), "emit(parse(emit)) differs");
	CHECK(!memcmp(&t.c, &d.c, sizeof t.c) && t.radius == d.radius && t.dark == d.dark, "values survive round trip");
	/* error cases */
	const char *cases[][2] = { { "unknown key", "name = a\nfoo = 1\n" }, { "no equals", "name a\n" }, { "bad colour", "name = a\nbg = red\n" }, { "missing role", "name = a\nbg = #000000\n" },
		{ "bad variant", "name = a\nvariant = grey\n" }, { "bad int", "name = a\nradius = big\n" }, { "quote in font", "name = a\nfont = \"x\n" } };
	for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) CHECK(co_theme_parse(cases[i][1], &t, err, sizeof err) == -1 && err[0], "should reject: %s", cases[i][0]);
	CHECK(co_theme_parse(good.p, &t, err, sizeof err) == 0, "again ok");
	char *p = strdup(good.p); char *q = strstr(p, "bg = "); q[5] = 'z';
	CHECK(co_theme_parse(p, &t, err, sizeof err) == -1 && strstr(err, "line"), "error carries a line number: %s", err); free(p);
	co_sb_free(&good); co_sb_free(&again);
	/* every shipped theme parses and round-trips */
	const char *names[] = { "dark", "light", "high-contrast", "gruvbox-dark", "nord", "catppuccin-mocha" };
	for (unsigned i = 0; i < sizeof names / sizeof *names; i++) {
		char path[300]; snprintf(path, sizeof path, "../themes/%s.theme", names[i]); char *txt = co_read_file(path, NULL);
		CHECK(txt && !co_theme_parse(txt, &t, err, sizeof err), "%s: %s", names[i], txt ? err : "missing"); if (!txt) continue;
		co_sb o = { 0 }; co_theme_emit(&t, &o); CHECK(!strcmp(o.p, txt), "%s is not in canonical emit form", names[i]); co_sb_free(&o);
		co_sb c = { 0 }; int f = co_theme_check(&t, &c); co_sb_free(&c);
		if (!strcmp(names[i], "dark") || !strcmp(names[i], "light") || !strcmp(names[i], "high-contrast")) CHECK(f == 0, "%s must pass WCAG, %d failures", names[i], f);
		free(txt);
	}
}

static void t_derive(void) {
	const char *acc[] = { "#4f9cff", "#e74c3c", "#2ecc71", "#f1c40f", "#9b59b6", "#ffffff", "#000000", "#808080", "#00ff88" };
	for (unsigned i = 0; i < sizeof acc / sizeof *acc; i++) for (int dark = 0; dark < 2; dark++) {
		co_theme t; co_theme_derive(&t, "d", H(acc[i]), NULL, NULL, dark);
		double worst = 99; for (size_t k = 0; k < sizeof co_pairs / sizeof *co_pairs; k++) { if (co_pairs[k].fg == CR_ACCENT_FG || co_pairs[k].fg == CR_SEL_FG) continue; double r = co_contrast(t.c[co_pairs[k].fg], t.c[co_pairs[k].bg]) - co_pairs[k].min; if (r < worst) worst = r; }
		CHECK(worst > -0.05, "derive(%s,%s) violates a pair by %.2f", acc[i], dark ? "dark" : "light", worst);
		CHECK(co_contrast(t.c[CR_ACCENT_FG], t.c[CR_ACCENT]) >= 4.5 - 1e-9 || !strcmp(acc[i], "#808080"), "accent_fg on %s", acc[i]);
		CHECK((co_lum(t.c[CR_BG]) < 0.4) == dark, "bg polarity for %s", acc[i]);
	}
	co_theme t; co_rgb bg = H("#fdf6e3"); co_theme_derive(&t, "s", H("#268bd2"), &bg, NULL, 1);
	CHECK(t.dark == 0 && co_eq(t.c[CR_BG], bg), "explicit light bg wins over the dark flag");
}

static void t_merge(void) {
	co_set s = { 0 }; co_file *f = co_set_ini(&s, "t", W_CONFIG, "a.ini", "Settings"); co_sb_add(&f->body, "a=1\nb=2\n");
	co_sb o = { 0 };
	co_merge("", f, &o); CHECK(!strcmp(o.p, "[Settings]\na=1\nb=2\n"), "ini into empty: %s", o.p);
	co_sb o2 = { 0 }; co_merge("[X]\nz=9\n\n[Settings]\na=0\nkeep=yes\n\n[Y]\nq=1\n", f, &o2);
	CHECK(!strcmp(o2.p, "[X]\nz=9\n\n[Settings]\na=1\nkeep=yes\nb=2\n\n[Y]\nq=1\n"), "ini merge keeps others:\n%s", o2.p);
	co_sb o3 = { 0 }; co_merge(o2.p, f, &o3); CHECK(!strcmp(o2.p, o3.p), "ini merge idempotent");
	co_sb o4 = { 0 }; co_merge("[Other]\nk=v", f, &o4); CHECK(!strcmp(o4.p, "[Other]\nk=v\n\n[Settings]\na=1\nb=2\n"), "ini appends section: %s", o4.p);
	co_file *g = co_set_block(&s, "t", W_CONFIG, "b.css", "/*", " */"); co_sb_add(&g->body, "x{}\n");
	co_sb b1 = { 0 }; co_merge("mine{}\n", g, &b1); CHECK(!strcmp(b1.p, "mine{}\n\n/* colorado:begin */\nx{}\n/* colorado:end */\n"), "block append: %s", b1.p);
	co_sb b2 = { 0 }; co_merge(b1.p, g, &b2); CHECK(!strcmp(b1.p, b2.p), "block idempotent");
	co_sb_free(&g->body); co_sb_add(&g->body, "y{}\n"); co_sb b3 = { 0 }; co_merge("pre\n/* colorado:begin */\nold\n/* colorado:end */\npost\n", g, &b3);
	CHECK(!strcmp(b3.p, "pre\n/* colorado:begin */\ny{}\n/* colorado:end */\npost\n"), "block replace in place: %s", b3.p);
	co_sb b4 = { 0 }; co_merge("", g, &b4); CHECK(b4.p[0] == '/', "block into empty has no leading blank line");
	co_sb_free(&o); co_sb_free(&o2); co_sb_free(&o3); co_sb_free(&o4); co_sb_free(&b1); co_sb_free(&b2); co_sb_free(&b3); co_sb_free(&b4); co_set_free(&s);
}

static void t_generators(void) {
	co_theme t; char err[200]; char *txt = co_read_file("expected/theme.theme", NULL);
	CHECK(txt && !co_theme_parse(txt, &t, err, sizeof err), "golden theme: %s", err); free(txt);
	for (int i = 0; i < CO_NGENS; i++) {
		co_set s = { 0 }; co_gens[i].fn(&t, &s);
		CHECK(s.n > 0 || s.notes.n > 0, "%s produced nothing", co_gens[i].name);
		for (int k = 0; k < s.n; k++) {
			CHECK(s.f[k].body.n > 0 && s.f[k].rel[0] && s.f[k].rel[0] != '/' && !strstr(s.f[k].rel, ".."), "%s: file %s has a bad path or empty body", co_gens[i].name, s.f[k].rel);
			CHECK(s.f[k].body.p[s.f[k].body.n - 1] == '\n', "%s: %s must end with a newline", co_gens[i].name, s.f[k].rel);
		}
		co_set_free(&s);
	}
	/* qt palette: 21 entries x 3 states, #aarrggbb, in QPalette order */
	co_sb q = { 0 }; co_qt_scheme(&t, &q);
	int lines = 0, bad = 0; for (char *l = strtok(q.p, "\n"); l; l = strtok(NULL, "\n")) { lines++; if (lines == 1) continue; int commas = 0; for (char *p = l; *p; p++) if (*p == ',') commas++; if (commas != 20) bad++; }
	CHECK(lines == 4 && !bad, "qt scheme shape: %d lines, %d bad", lines, bad); co_sb_free(&q);
	co_rgb v[QP_N]; int a[QP_N]; co_qt_palette(&t, 0, v, a);
	CHECK(co_eq(v[0], t.c[CR_FG]) && co_eq(v[10], t.c[CR_BG]) && co_eq(v[12], t.c[CR_SEL_BG]) && co_eq(v[14], t.c[CR_LINK]) && a[20] == 0x80, "qt role order (WindowText, Window, Highlight, Link, Placeholder alpha)");
}

static void t_extract(void) {
	co_img im; im.w = 64; im.h = 48; im.rgb = malloc(64 * 48 * 3);
	for (int y = 0; y < 48; y++) for (int x = 0; x < 64; x++) {
		co_rgb c = co_rgb_of(20, 24, 40);                       /* dark navy field */
		if (x >= 40 && y >= 10 && y < 20) c = co_rgb_of(220, 40, 40); /* red accent patch */
		if (y >= 36 && x < 30) c = co_rgb_of(230, 230, 230);    /* light text patch */
		memcpy(im.rgb + ((size_t)y * 64 + x) * 3, &c, 3);
	}
	co_theme t; co_extract(&im, "img", -1, &t);
	double h, s, l; co_to_hsl(t.c[CR_ACCENT], &h, &s, &l);
	CHECK(t.dark, "dark image -> dark theme"); CHECK(co_lum(t.c[CR_BG]) < 0.05, "bg is dark");
	CHECK((h < 15 || h > 345) && s > 0.5, "accent is the red patch (h=%.0f s=%.2f)", h, s);
	co_theme t2; co_extract(&im, "img", 0, &t2); CHECK(!t2.dark && co_lum(t2.c[CR_BG]) > 0.7, "forced light");
	co_theme t3; co_extract(&im, "img", -1, &t3); CHECK(!memcmp(&t.c, &t3.c, sizeof t.c), "extraction is deterministic");
	/* PPM / PAM decoding */
	unsigned char ppm[] = "P6\n# c\n2 1\n255\n\xff\x00\x00\x00\x00\xff"; co_img p; CHECK(!co_img_pnm(ppm, sizeof ppm - 1, &p) && p.w == 2 && p.rgb[0] == 255 && p.rgb[5] == 255, "P6"); free(p.rgb);
	unsigned char pam[] = "P7\nWIDTH 1\nHEIGHT 1\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n\x10\x20\x30\xff"; co_img q;
	CHECK(!co_img_pnm(pam, sizeof pam - 1, &q) && q.rgb[0] == 0x10 && q.rgb[2] == 0x30, "P7 RGB_ALPHA"); free(q.rgb);
	unsigned char trunc[] = "P6\n4 4\n255\nabc"; CHECK(co_img_pnm(trunc, sizeof trunc - 1, &q) == -1, "truncated ppm rejected");
	unsigned char huge[] = "P6\n99999 99999\n255\nabc"; CHECK(co_img_pnm(huge, sizeof huge - 1, &q) == -1, "huge dimensions rejected");
	free(im.rgb);
}

int main(void) {
	t_wcag(); t_hex(); t_spaces(); t_theme(); t_derive(); t_merge(); t_generators(); t_extract();
	printf("unit: %d passed, %d failed\n", pass, fail);
	return fail ? 1 : 0;
}
