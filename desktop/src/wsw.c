/* wsw - workspace switcher, an applet of `desktop` (desktop wsw ...). Under dwl the workspaces (tags) are BUILT IN: Super+Z/X
 * (shiftview) and Super+Shift+Z/X (tagshiftview) need no process at all; this applet is for scripts and for the Hyprland
 * fallback session, where it replaces the old bash+python script (no hyprctl, no python, no shell).
 *
 *   wsw next | prev          show the next/previous workspace (wraps at WSW_MAX, default 5)
 *   wsw N                    show workspace N (1..WSW_MAX)
 *   wsw move_next | move_prev   send the focused window to the next/previous workspace
 *   wsw move N               send the focused window to workspace N
 *
 * Hyprland (HYPRLAND_INSTANCE_SIGNATURE set): talks to its socket directly. Monitors are ordered left to right by x; the
 * leftmost shows workspace N, the second one N+MAX (so two monitors switch together, as the old bash+python script did),
 * and the focus ends where it started. Workspaces 1..MAX belong to the first monitor, MAX+1..2*MAX to the second.
 * dwl (no Hyprland, $XDG_RUNTIME_DIR/dwl.sock present): workspaces are tags; "next" is view of the next tag on the
 * selected output. Only that output is switched.
 *
 * `wsw --selftest` checks the JSON reading against a sample of Hyprland's output (no compositor needed).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAXMON 8

static int MAX = 5;

/* ---- JSON: just enough to read Hyprland's monitor list --------------------------------------------------------- */

/* value of KEY at the top level of the object/array that starts at S (S points at '{' or '['); NULL when absent */
static const char *jget(const char *s, const char *key)
{
	int depth = 0, instr = 0;
	size_t kl = strlen(key);
	for (const char *p = s; *p; p++) {
		if (instr) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '"') instr = 0;
			continue;
		}
		if (*p == '"') {
			if (depth == 1 && !strncmp(p + 1, key, kl) && p[1 + kl] == '"' && p[2 + kl] == ':') {
				const char *v = p + 3 + kl;
				while (*v == ' ') v++;
				return v;
			}
			instr = 1;
		} else if (*p == '{' || *p == '[') depth++;
		else if (*p == '}' || *p == ']') { if (--depth <= 0) break; }
	}
	return NULL;
}

/* the end (one past the closing bracket) of the value that starts at V */
static const char *jend(const char *v)
{
	int depth = 0, instr = 0;
	for (const char *p = v; *p; p++) {
		if (instr) {
			if (*p == '\\' && p[1]) p++;
			else if (*p == '"') instr = 0;
			continue;
		}
		if (*p == '"') instr = 1;
		else if (*p == '{' || *p == '[') depth++;
		else if (*p == '}' || *p == ']') { if (--depth <= 0) return p + 1; }
		else if (depth == 0 && (*p == ',' || *p == ' ')) return p;
	}
	return v + strlen(v);
}

struct mon { char name[64]; int x, focused, ws; };

/* parse the array of monitors, sorted by x; returns how many */
static int parse_monitors(const char *js, struct mon *m)
{
	int n = 0;
	const char *p = js;
	while (*p && *p != '[') p++;
	if (!*p) return 0;
	for (p++; *p && n < MAXMON;) {
		while (*p == ' ' || *p == ',' || *p == '\n') p++;
		if (*p != '{') break;
		const char *e = jend(p);
		char *obj = strndup(p, e - p);
		const char *v;
		struct mon *x = &m[n];
		memset(x, 0, sizeof *x);
		if ((v = jget(obj, "name")) && *v == '"') {
			size_t i = 0;
			for (v++; *v && *v != '"' && i < sizeof x->name - 1; v++) x->name[i++] = *v;
		}
		if ((v = jget(obj, "x"))) x->x = atoi(v);
		if ((v = jget(obj, "focused"))) x->focused = !strncmp(v, "true", 4);
		if ((v = jget(obj, "activeWorkspace")) && (v = jget(v, "id"))) x->ws = atoi(v);
		free(obj);
		if (x->name[0]) n++;
		p = e;
	}
	for (int i = 1; i < n; i++)  /* insertion sort by x */
		for (int j = i; j > 0 && m[j].x < m[j - 1].x; j--) { struct mon t = m[j]; m[j] = m[j - 1]; m[j - 1] = t; }
	return n;
}

/* ---- sockets ---------------------------------------------------------------------------------------------------- */

static int request(const char *path, const char *req, char *out, size_t outn)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	size_t got = 0;
	ssize_t r;
	if (fd < 0) return -1;
	snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
	if (write(fd, req, strlen(req)) < 0) { close(fd); return -1; }
	if (out) {
		while (got + 1 < outn && (r = read(fd, out + got, outn - 1 - got)) > 0) got += r;
		out[got] = 0;
	} else {
		char sink[256];
		while (read(fd, sink, sizeof sink) > 0) {}
	}
	close(fd);
	return 0;
}

static char hypr_path[512], dwl_path[512];

static int hypr(const char *req, char *out, size_t n) { return request(hypr_path, req, out, n); }

/* ---- Hyprland --------------------------------------------------------------------------------------------------- */

static int hypr_active_ws(void)
{
	static char buf[16384];
	const char *v;
	if (hypr("j/activewindow", buf, sizeof buf) < 0) return 1;
	if ((v = jget(buf, "workspace")) && (v = jget(v, "id")) && atoi(v) > 0) return atoi(v);
	return 1;
}

static int run_hypr(const char *cmd, const char *arg)
{
	static char buf[65536];
	struct mon m[MAXMON];
	char batch[1024];
	int n, vd, len;
	if (hypr("j/monitors", buf, sizeof buf) < 0) { fputs("wsw: cannot reach Hyprland\n", stderr); return 1; }
	n = parse_monitors(buf, m);
	if (n == 0) return 1;

	if (!strcmp(cmd, "move_next") || !strcmp(cmd, "move_prev") || !strcmp(cmd, "move")) {
		int ws = hypr_active_ws(), lo = ws <= MAX ? 1 : MAX + 1, hi = lo + MAX - 1, nws;
		if (!strcmp(cmd, "move")) nws = atoi(arg) + (lo - 1);
		else if (!strcmp(cmd, "move_next")) { nws = ws + 1; if (nws > hi) nws = lo; }
		else { nws = ws - 1; if (nws < lo) nws = hi; }
		snprintf(batch, sizeof batch, "dispatch movetoworkspace %d", nws);
		return hypr(batch, NULL, 0) < 0;
	}
	if (!strcmp(cmd, "next") || !strcmp(cmd, "prev")) {
		vd = m[0].ws > 0 ? m[0].ws : 1;
		if (!strcmp(cmd, "next")) { if (++vd > MAX) vd = 1; }
		else if (--vd < 1) vd = MAX;
	} else if ((vd = atoi(cmd)) < 1 || vd > MAX) {
		fprintf(stderr, "wsw: workspace must be 1..%d\n", MAX);
		return 2;
	}
	len = snprintf(batch, sizeof batch, "[[BATCH]]dispatch focusmonitor %s;dispatch workspace %d", m[0].name, vd);
	if (n > 1)
		len += snprintf(batch + len, sizeof batch - len, ";dispatch focusmonitor %s;dispatch workspace %d;dispatch focusmonitor %s",
		                m[1].name, vd + MAX, m[0].name);
	return hypr(batch, NULL, 0) < 0;
}

/* ---- dwl -------------------------------------------------------------------------------------------------------- */

static int run_dwl(const char *cmd, const char *arg)
{
	static char buf[65536];
	char req[64];
	int cur = 1, vd;
	const char *p;
	if (request(dwl_path, "state", buf, sizeof buf) < 0) { fputs("wsw: cannot reach dwl\n", stderr); return 1; }
	/* the selected output's visible tags: first set bit of "view" on the line with "selected":1 */
	for (p = buf; (p = strstr(p, "\"selected\":1")); p++) {
		const char *v = strstr(p, "\"view\":");
		if (v) {
			unsigned bits = (unsigned)strtoul(v + 7, NULL, 10);
			for (cur = 1; cur <= 31 && !(bits & (1u << (cur - 1))); cur++) {}
			if (cur > 31) cur = 1;
			break;
		}
	}
	if (!strcmp(cmd, "move_next") || !strcmp(cmd, "move_prev") || !strcmp(cmd, "move")) {
		if (!strcmp(cmd, "move")) vd = atoi(arg);
		else if (!strcmp(cmd, "move_next")) vd = cur % MAX + 1;
		else vd = cur == 1 ? MAX : cur - 1;
		snprintf(req, sizeof req, "tag %d\n", vd);
	} else {
		if (!strcmp(cmd, "next")) vd = cur % MAX + 1;
		else if (!strcmp(cmd, "prev")) vd = cur <= 1 ? MAX : cur - 1;
		else vd = atoi(cmd);
		snprintf(req, sizeof req, "view %d\n", vd);
	}
	if (vd < 1 || vd > MAX) { fprintf(stderr, "wsw: workspace must be 1..%d\n", MAX); return 2; }
	return request(dwl_path, req, NULL, 0) < 0;
}

/* ---- self test -------------------------------------------------------------------------------------------------- */

static int selftest(void)
{
	/* shape of `hyprctl monitors -j`: nested objects that repeat the key names "name" and "id" */
	const char *js = "[{\"id\": 1,\"name\": \"DP-2\",\"x\": 1920,\"y\": 0,\"activeWorkspace\": {\"id\": 7,\"name\": \"7\"},"
	                 "\"specialWorkspace\": {\"id\": 0,\"name\": \"\"},\"focused\": false},"
	                 "{\"id\": 0,\"name\": \"HDMI-A-1\",\"description\": \"a \\\"quoted\\\" name\",\"x\": 0,\"y\": 0,"
	                 "\"activeWorkspace\": {\"id\": 2,\"name\": \"2\"},\"focused\": true}]";
	struct mon m[MAXMON];
	int n = parse_monitors(js, m), bad = 0;
	bad |= n != 2;
	bad |= strcmp(m[0].name, "HDMI-A-1") || m[0].x != 0 || !m[0].focused || m[0].ws != 2;   /* sorted: left monitor first */
	bad |= strcmp(m[1].name, "DP-2") || m[1].x != 1920 || m[1].focused || m[1].ws != 7;
	bad |= parse_monitors("[]", m) != 0;
	puts(bad ? "selftest: FAIL" : "selftest: ok");
	return bad;
}

int main(int argc, char **argv)
{
	const char *rt = getenv("XDG_RUNTIME_DIR"), *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE"), *mx = getenv("WSW_MAX");
	if (argc >= 2 && !strcmp(argv[1], "--selftest")) return selftest();
	if (argc < 2 || (!strcmp(argv[1], "move") && argc < 3)) {
		fputs("usage: wsw next | prev | N | move_next | move_prev | move N\n", stderr);
		return 2;
	}
	if (mx && atoi(mx) > 0) MAX = atoi(mx);
	if (!rt) { fputs("wsw: XDG_RUNTIME_DIR not set\n", stderr); return 1; }
	if (sig && *sig) {
		snprintf(hypr_path, sizeof hypr_path, "%s/hypr/%s/.socket.sock", rt, sig);
		return run_hypr(argv[1], argc > 2 ? argv[2] : "");
	}
	snprintf(dwl_path, sizeof dwl_path, "%s/dwl.sock", rt);
	return run_dwl(argv[1], argc > 2 ? argv[2] : "");
}
