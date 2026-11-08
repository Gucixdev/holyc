/* audio.c - the compositor's volume: one tiny backend, the `volume` command, the OSD launcher.
 * Two channels: spk (the default sink) and mic (the default source). Backends, chosen on every call by $DESKTOP_AUDIO_BACKEND:
 *   (unset)  wpctl   `wpctl get-volume / set-volume / set-mute @DEFAULT_AUDIO_SINK@ | @DEFAULT_AUDIO_SOURCE@` (PipeWire/WirePlumber)
 *   fake     a text file ($DESKTOP_AUDIO_FILE, default $XDG_RUNTIME_DIR/desktop-audio-fake): "spk 0.50 0" / "mic 0.30 0" = level, muted.
 *            Used by the tests so they never touch the real volume.
 * Commands (api.c routes `volume ARGS` here, key bindings use them too):
 *   volume up|down [PCT]        speaker louder / quieter by the step setting (or PCT)       volume micup|micdown [PCT]   same, microphone
 *   volume mute [on|off]        speaker mute (toggle without argument)                      volume micmute [on|off]
 *   volume set spk|mic PCT      absolute level                                              volume get   state line      volume osd   show the panel
 * Everything that changes something bumps `seq`, which the panel watches to restart its timeout. */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "audio.h"

int volume_osd_ms = 1500, volume_step = 5, volume_max = 100;
static long seq;

typedef struct { double level; int muted; } Chan;

static int
is_fake(void)
{
	const char *b = getenv("DESKTOP_AUDIO_BACKEND");
	return b && !strcmp(b, "fake");
}

static const char *
fake_path(void)
{
	static char p[512];
	const char *f = getenv("DESKTOP_AUDIO_FILE"), *rt = getenv("XDG_RUNTIME_DIR");
	if (f && *f) return f;
	snprintf(p, sizeof p, "%s/desktop-audio-fake", rt ? rt : "/tmp");
	return p;
}

static void
fake_load(Chan c[2])
{
	FILE *f = fopen(fake_path(), "r");
	char name[8];
	double l;
	int m;
	c[0] = (Chan){ 0.50, 0 };
	c[1] = (Chan){ 0.30, 0 };
	if (!f) return;
	while (fscanf(f, "%7s %lf %d", name, &l, &m) == 3) {
		int i = !strcmp(name, "mic");
		c[i] = (Chan){ l, m != 0 };
	}
	fclose(f);
}

static void
fake_save(const Chan c[2])
{
	char tmp[600];
	FILE *f;
	snprintf(tmp, sizeof tmp, "%s.tmp", fake_path());
	if (!(f = fopen(tmp, "w"))) return;
	fprintf(f, "spk %.4f %d\nmic %.4f %d\n", c[0].level, c[0].muted, c[1].level, c[1].muted);
	fclose(f);
	rename(tmp, fake_path());
}

static const char *
target(int mic)
{
	return mic ? "@DEFAULT_AUDIO_SOURCE@" : "@DEFAULT_AUDIO_SINK@";
}

/* run wpctl with arguments, output discarded; 0 = success */
static int
wpctl(const char *a, const char *b, const char *c, const char *d)
{
	pid_t p = fork();
	int st;
	if (p < 0) return -1;
	if (p == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
		execlp("wpctl", "wpctl", a, b, c, d, (char *)NULL);
		_exit(127);
	}
	if (waitpid(p, &st, 0) < 0) return 0; /* SIGCHLD reaped it elsewhere: assume it worked */
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int
wp_get(int mic, Chan *c)
{
	char cmd[128], line[128];
	FILE *f;
	double l;
	snprintf(cmd, sizeof cmd, "wpctl get-volume %s 2>/dev/null", target(mic));
	if (!(f = popen(cmd, "r"))) return -1;
	if (!fgets(line, sizeof line, f) || sscanf(line, "Volume: %lf", &l) != 1) { pclose(f); return -1; }
	pclose(f);
	c->level = l;
	c->muted = strstr(line, "[MUTED]") != NULL;
	return 0;
}

static void
load(Chan c[2])
{
	if (is_fake()) { fake_load(c); return; }
	for (int i = 0; i < 2; i++) if (wp_get(i, &c[i])) c[i] = (Chan){ 0, 0 };
}

static int
store(int mic, Chan *ch, int level_too, int mute_too)
{
	if (is_fake()) {
		Chan c[2];
		fake_load(c);
		if (level_too) c[mic].level = ch->level;
		if (mute_too) c[mic].muted = ch->muted;
		fake_save(c);
		return 0;
	}
	if (level_too) {
		char v[32];
		snprintf(v, sizeof v, "%.2f", ch->level);
		if (wpctl("set-volume", target(mic), v, NULL)) return -1;
	}
	if (mute_too && wpctl("set-mute", target(mic), ch->muted ? "1" : "0", NULL)) return -1;
	return 0;
}

void
audio_state(char *buf, size_t n)
{
	Chan c[2];
	load(c);
	snprintf(buf, n, "spk %d %d mic %d %d seq %ld osd %d step %d max %d", (int)lround(c[0].level * 100), c[0].muted,
		(int)lround(c[1].level * 100), c[1].muted, seq, volume_osd_ms, volume_step, volume_max);
}

void
audio_osd(void)
{
	pid_t p;
	if (volume_osd_ms <= 0) return;
	if ((p = fork()) == 0) {
		setsid();
		execl("/proc/self/exe", "desktop", "settings", "volume", "--osd", (char *)NULL);
		_exit(127);
	}
}

static int
chan_of(const char *s)
{
	return s && (!strcmp(s, "mic") || !strcmp(s, "micup") || !strcmp(s, "micdown") || !strcmp(s, "micmute")) ? 1 : 0;
}

int
audio_command(char *a, FILE *o)
{
	char *w[4] = {0}, *save = NULL;
	int n = 0, changed = 0, mic;
	Chan c[2];
	if (!a) a = "";
	for (char *t = strtok_r(a, " \t", &save); t && n < 4; t = strtok_r(NULL, " \t", &save)) w[n++] = t;
	if (n == 0 || !strcmp(w[0], "get")) {
		char s[160];
		audio_state(s, sizeof s);
		fprintf(o, "%s\n", s);
		return 0;
	}
	mic = chan_of(w[0]);
	load(c);
	if (!strcmp(w[0], "osd")) { audio_osd(); fputs("ok\n", o); return 0; }
	if (!strcmp(w[0], "up") || !strcmp(w[0], "down") || !strcmp(w[0], "micup") || !strcmp(w[0], "micdown")) {
		int step = n > 1 ? atoi(w[1]) : volume_step, pct = (int)lround(c[mic].level * 100);
		if (step <= 0) step = volume_step;
		pct += strstr(w[0], "up") ? step : -step;
		if (pct > volume_max) pct = volume_max;
		if (pct < 0) pct = 0;
		c[mic].level = pct / 100.0;
		if (store(mic, &c[mic], 1, 0)) goto fail;
		if (pct > 0 && c[mic].muted) { c[mic].muted = 0; store(mic, &c[mic], 0, 1); } /* raising the volume unmutes, like every desktop */
		changed = 1;
	} else if (!strcmp(w[0], "mute") || !strcmp(w[0], "micmute")) {
		int to = n > 1 ? (!strcmp(w[1], "on") || !strcmp(w[1], "1")) : !c[mic].muted;
		if (n > 1 && strcmp(w[1], "on") && strcmp(w[1], "off") && strcmp(w[1], "1") && strcmp(w[1], "0")) { fprintf(o, "error: volume %s [on|off]\n", w[0]); return 0; }
		c[mic].muted = to;
		if (store(mic, &c[mic], 0, 1)) goto fail;
		changed = 1;
	} else if (!strcmp(w[0], "set") && n == 3 && (!strcmp(w[1], "spk") || !strcmp(w[1], "mic"))) {
		double pct = atof(w[2]);
		mic = !strcmp(w[1], "mic");
		if (pct < 0) pct = 0;
		if (pct > volume_max) pct = volume_max;
		c[mic].level = lround(pct) / 100.0;
		if (store(mic, &c[mic], 1, 0)) goto fail;
		changed = 1;
	} else {
		fputs("error: volume up|down|micup|micdown [PCT] | mute|micmute [on|off] | set spk|mic PCT | get | osd\n", o);
		return 0;
	}
	seq++;
	fputs("ok\n", o);
	return changed;
fail:
	fputs("error: audio backend failed (is wpctl installed and PipeWire running?)\n", o);
	return 0;
}
