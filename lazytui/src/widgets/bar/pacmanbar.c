/* pacmanbar.c - Pacmanbar: a pacman -Sy style download line, byte-exact with pacman's own callback.c
 * Needs: lazytui.h (the lib core). One widget, one file: copy it into any project that links the core. */
#include "internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the same arithmetic as pacman's callback.c (fill_progress and the info columns) */
static void humanize(double v, char *num, size_t nn, const char **unit, int decimals_rule)
{
	static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"};
	int u = 0;
	while (v > 2048.0 && u < 8) { v /= 1024.0; u++; }
	if (decimals_rule) snprintf(num, nn, "%.*f", v < 10 ? 2 : v < 100 ? 1 : 0, v);      /* rates: 4.15  10.6  798 */
	else snprintf(num, nn, "%.1f", v);                                                  /* sizes: 22.0 */
	*unit = units[u];
}

size_t lt_pacman_line(char *out, size_t cap, int cols, const char *name, double done, double total, double rate, int eta_s, lt_pacman_state *st)
{
	if (cap == 0) return 0;
	char *o = out;
	size_t n = 0;
	int infolen = cols * 6 / 10;
	if (infolen < 50) infolen = 50;
	int F = infolen - 30;                                         /* name column */
	char nm[512];
	size_t nl = strlen(name ? name : "");
	if ((int)nl > F - 1) { int keep = F - 4 > 0 ? F - 4 : 0; snprintf(nm, sizeof nm, "%.*s...", keep, name); }
	else snprintf(nm, sizeof nm, "%s", name ? name : "");
	done = lt_finite_or(done, 0); rate = lt_finite_or(rate, 0);
	char sn[32], rn[32], eta[24];
	const char *su, *ru;
	humanize(done, sn, sizeof sn, &su, 0);
	humanize(rate < 0 ? 0 : rate, rn, sizeof rn, &ru, 1);
	if (eta_s < 0) snprintf(eta, sizeof eta, "--:--");
	else if (eta_s >= 3600) snprintf(eta, sizeof eta, "%02d:%02d:%02d", eta_s / 3600, eta_s / 60 % 60, eta_s % 60);
	else snprintf(eta, sizeof eta, "%02d:%02d", eta_s / 60, eta_s % 60);
	char sz[48];
	snprintf(sz, sizeof sz, "%s %3s", sn, su);
	n += (size_t)snprintf(o + n, cap - n, " %-*s %10s %5s %3s/s %s", F, nm, sz, rn, ru, eta);
	double pf = total > 0 ? lt_finite_or(done, 0) * 100.0 / total : 0;
	int percent = !(pf > 0) ? 0 : pf > 100 ? 100 : (int)pf;
	int proglen = cols - infolen;
	int hashlen = proglen > 8 ? proglen - 8 : 0;
	int hash = percent * hashlen / 100;
	lt_pacman_state local = {0, 0};
	if (!st) st = &local;
	if (percent == 0) { st->lasthash = 0; st->mouth = 0; }
	if (hashlen > 0 && n + (size_t)hashlen + 4 < cap) {
		o[n++] = ' '; o[n++] = '[';
		for (int i = hashlen; i > 0; --i) {
			if (i > hashlen - hash) o[n++] = '-';
			else if (i == hashlen - hash) {
				if (st->lasthash != hash) { st->lasthash = hash; st->mouth = st->mouth == 1 ? 0 : 1; }
				o[n++] = st->mouth ? 'c' : 'C';
			} else o[n++] = i % 3 == 0 ? 'o' : ' ';
		}
		o[n++] = ']';
	}
	if (proglen >= 5) n += (size_t)snprintf(o + n, cap - n, " %3d%%", percent);
	o[n < cap ? n : cap - 1] = 0;
	return n;
}

void lt_pacmanbar(lt_canvas *c, lt_rect r, const lt_theme *t, const char *name, double done, double total, double rate, int eta_s, lt_pacman_state *st)
{
	r = lt_one_row(r);
	if (r.w < 1) return;
	char line[1024];
	lt_pacman_line(line, sizeof line, r.w, name, done, total, rate, eta_s, st);
	lt_rect all = lt_full(c);
	lt_text(c, all, r.x, r.y, line, lt_slot_style(t, LT_T_TEXT), r.w);
	/* the only colour: the chomping mouth (the characters themselves are pacman's) */
	char *open = strchr(line, '['), *close = open ? strchr(open, ']') : NULL;
	if (open && close) {
		lt_style pac = lt_slot_style(t, LT_T_WARN);
		pac.attr |= LT_BOLD;
		for (char *p = open + 1; p < close; p++)
			if ((*p == 'C' || *p == 'c') && (int)(p - line) < r.w) { lt_put(c, all, r.x + (int)(p - line), r.y, (uint32_t)*p, pac); break; }
	}
}
