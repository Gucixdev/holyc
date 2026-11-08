/* settings.c - runtime control: the dwl.cmd fifo and the ctl socket (state out, commands and session restore in).
 * Part of the dwl compositor, split out of dwl.c (dwl: https://codeberg.org/dwl/dwl, see src/LICENSES/). */
#include "dwl.h"


/* command channel: lines on $XDG_RUNTIME_DIR/dwl.cmd (fifo), one command per line, same language as the socket (api.c, `commands`).
 * State goes out on stdout (printstatus) and as events to subscribers (api.c). */
static struct wl_event_source *cmd_src;

static int cmd_fd = -1;

static char cmd_path[256];

static char cmd_buf[1024];

static size_t cmd_len;

static void
cmdrun(char *l)
{
	FILE *null = fopen("/dev/null", "w");
	api_exec(l, null ? null : stderr); /* the fifo has no reply channel; api.c defines the commands */
	if (null) fclose(null);
}

static int
cmdread(int fd, uint32_t mask, void *data)
{
	ssize_t r = read(fd, cmd_buf + cmd_len, sizeof(cmd_buf) - 1 - cmd_len);
	char *nl;
	if (r <= 0) /* writer closed: fifo is opened O_RDWR so this is only EAGAIN */
		return 0;
	cmd_len += r;
	cmd_buf[cmd_len] = 0;
	while ((nl = strchr(cmd_buf, '\n'))) {
		*nl = 0;
		cmdrun(cmd_buf);
		cmd_len -= (nl + 1 - cmd_buf);
		memmove(cmd_buf, nl + 1, cmd_len + 1);
	}
	if (cmd_len >= sizeof(cmd_buf) - 1)
		cmd_len = 0; /* overlong line: drop */
	return 0;
}

/* control socket: $XDG_RUNTIME_DIR/dwl.sock, one request line -> one reply, then close. Same commands as the fifo, plus
 *   state            JSON of every output and window (appid, title, pid, tags, floating, geometry, focus)
 *   save PATH        write that JSON to PATH, one window per line     -> "ok N"
 *   restore PATH     put the windows that are open now back where PATH has them (tags, floating, geometry, visible tags);
 *                    windows are matched by output + appid; ones that are not open are listed under "missing"
 * Reply is JSON for state/restore, "ok" for the fifo-style commands. This is also what the MCP applet talks to. */
static struct wl_event_source *ctl_src;

static int ctl_fd = -1;

static char ctl_path[256];

static void
jstr(FILE *f, const char *s)
{
	fputc('"', f);
	for (; s && *s; s++) {
		unsigned char ch = *s;
		if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
		else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
		else fputc(ch, f);
	}
	fputc('"', f);
}

static int
ctlpid(Client *c)
{
	int pid = 0;
#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->pid;
#endif
	wl_client_get_credentials(c->surface.xdg->client->client, &pid, NULL, NULL);
	return pid;
}

/* the command line that started a window, shell-quoted, read from /proc/PID/cmdline ("" if unreadable) */
static void
ctlcmd(FILE *f, int pid)
{
	char path[64], buf[2048], q[4200];
	size_t n, len = 0;
	FILE *p;
	snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
	q[0] = 0;
	if (pid > 0 && (p = fopen(path, "r"))) {
		n = fread(buf, 1, sizeof buf - 1, p);
		buf[n] = 0;
		fclose(p);
		for (size_t i = 0; i < n; i += strlen(buf + i) + 1) {
			const char *a = buf + i;
			if (len + strlen(a) * 4 + 4 >= sizeof q) break;
			if (len) q[len++] = ' ';
			q[len++] = '\'';
			for (; *a; a++) {
				if (*a == '\'') { memcpy(q + len, "'\\''", 4); len += 4; }
				else q[len++] = *a;
			}
			q[len++] = '\'';
		}
		q[len] = 0;
	}
	jstr(f, q);
}

static void
ctlstate(FILE *f)
{
	Monitor *m;
	Client *c;
	int first = 1;
	fputs("{\"clients\":[\n", f);
	wl_list_for_each(m, &mons, link)
		wl_list_for_each(c, &clients, link) {
			if (c->mon != m)
				continue;
			fprintf(f, "%s{\"output\":", first ? "" : ",\n");
			first = 0;
			jstr(f, m->wlr_output->name);
			fputs(",\"appid\":", f); jstr(f, client_get_appid(c));
			fputs(",\"title\":", f); jstr(f, client_get_title(c));
			fputs(",\"cmd\":", f); ctlcmd(f, ctlpid(c));
			fprintf(f, ",\"pid\":%d,\"tags\":%"PRIu32",\"floating\":%d,\"fullscreen\":%d,\"focused\":%d,"
				"\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}", ctlpid(c), c->tags, c->isfloating, c->isfullscreen,
				c == focustop(m), c->geom.x, c->geom.y, c->geom.width, c->geom.height);
		}
	fputs("\n],\"outputs\":[\n", f);
	first = 1;
	wl_list_for_each(m, &mons, link) {
		fprintf(f, "%s{\"name\":", first ? "" : ",\n");
		first = 0;
		jstr(f, m->wlr_output->name);
		fprintf(f, ",\"selected\":%d,\"view\":%"PRIu32",\"layout\":", m == selmon, m->tagset[m->seltags]);
		jstr(f, m->ltsymbol);
		fprintf(f, ",\"w\":%d,\"h\":%d}", m->m.width, m->m.height);
	}
	fputs("\n]}\n", f);
}

/* value of "key": in a saved line; string values are copied unescaped-minimal into out (outlen), numbers returned */
static long
jfind(const char *line, const char *key, char *out, size_t outlen)
{
	char pat[40];
	const char *p;
	snprintf(pat, sizeof pat, "\"%s\":", key);
	if (!(p = strstr(line, pat)))
		return -1;
	p += strlen(pat);
	if (*p == '"' && out) {
		size_t n = 0;
		for (p++; *p && *p != '"' && n + 1 < outlen; p++) {
			if (*p == '\\' && p[1]) p++;
			out[n++] = *p;
		}
		out[n] = 0;
		return 0;
	}
	return strtol(p, NULL, 10);
}

/* restore state: the saved windows, kept after the call so windows we spawn can be placed when they map */
#define NSAVED 128
static struct Saved {
	char out[64], app[128], cmd[512];
	uint32_t tags;
	int floating, done;
	struct wlr_box g;
} saved[NSAVED];

static int nsaved;

static time_t saved_at; /* spawned windows are placed for 20 s after the restore, then the list is forgotten */

/* the live window that stands for saved[i]: same output and appid, not claimed by an earlier saved entry */
static Client *
ctlmatch(int i, Client **claimed, int nclaimed)
{
	Client *c;
	wl_list_for_each(c, &clients, link) {
		const char *id = client_get_appid(c);
		int taken = 0;
		if (!c->mon || strcmp(c->mon->wlr_output->name, saved[i].out) || strcmp(id ? id : "", saved[i].app))
			continue;
		for (int k = 0; k < nclaimed; k++) taken |= claimed[k] == c;
		if (!taken)
			return c;
	}
	return NULL;
}

static void
ctlapply(int i, Client *c)
{
	c->tags = saved[i].tags & TAGMASK ? saved[i].tags & TAGMASK : c->tags;
	setfloating(c, saved[i].floating);
	if (c->isfloating && saved[i].g.width > 0 && saved[i].g.height > 0)
		resize(c, saved[i].g, 0);
	saved[i].done = 1;
}

/* tiling order: the saved order is the order of the clients list, so put the placed windows at its head in that order */
static void
ctlreorder(void)
{
	Client *claimed[NSAVED], *c;
	int n = 0;
	for (int i = 0; i < nsaved; i++)
		if (saved[i].done && (c = ctlmatch(i, claimed, n))) { claimed[n++] = c; }
	for (int k = n - 1; k >= 0; k--) {
		wl_list_remove(&claimed[k]->link);
		wl_list_insert(&clients, &claimed[k]->link);
	}
	Monitor *m;
	wl_list_for_each(m, &mons, link)
		arrange(m);
}

/* hook in mapnotify: a window we spawned for a restore has appeared, put it where the layout says */
void
ctlmapped(Client *c)
{
	const char *id = client_get_appid(c);
	Monitor *m;
	if (!nsaved || time(NULL) - saved_at > 20)
		return;
	for (int i = 0; i < nsaved; i++) {
		if (saved[i].done || !saved[i].cmd[0] || strcmp(id ? id : "", saved[i].app))
			continue;
		ctlapply(i, c);
		wl_list_for_each(m, &mons, link) /* move it to the saved output if that output exists */
			if (!strcmp(m->wlr_output->name, saved[i].out)) {
				if (c->mon != m) setmon(c, m, c->tags);
				break;
			}
		ctlreorder();
		return;
	}
}

static void
ctlrestore(char *arg, FILE *out)
{
	char line[4096], oname[64], app[128];
	char *flag = strstr(arg, " nospawn");
	int nospawn = flag != NULL, moved = 0, spawned = 0, nmiss = 0;
	Client *claimed[NSAVED];
	FILE *in;
	if (flag) *flag = 0;
	if (!(in = fopen(arg, "r"))) {
		fprintf(out, "{\"error\":\"cannot open %s\"}\n", arg);
		return;
	}
	nsaved = 0;
	saved_at = time(NULL);
	while (fgets(line, sizeof line, in)) {
		Monitor *m;
		long v;
		if (jfind(line, "output", oname, sizeof oname) < 0 && jfind(line, "name", oname, sizeof oname) < 0)
			continue;
		if (jfind(line, "appid", app, sizeof app) < 0) {
			if ((v = jfind(line, "view", NULL, 0)) > 0) /* an output line: the visible tags */
				wl_list_for_each(m, &mons, link)
					if (!strcmp(m->wlr_output->name, oname)) {
						m->tagset[m->seltags] = v & TAGMASK;
						arrange(m);
					}
			continue;
		}
		if (nsaved >= NSAVED)
			continue;
		struct Saved *s = &saved[nsaved++];
		memset(s, 0, sizeof *s);
		snprintf(s->out, sizeof s->out, "%s", oname);
		snprintf(s->app, sizeof s->app, "%s", app);
		jfind(line, "cmd", s->cmd, sizeof s->cmd);
		s->tags = (uint32_t)jfind(line, "tags", NULL, 0);
		s->floating = jfind(line, "floating", NULL, 0) > 0;
		s->g = (struct wlr_box){ jfind(line, "x", NULL, 0), jfind(line, "y", NULL, 0),
			jfind(line, "w", NULL, 0), jfind(line, "h", NULL, 0) };
	}
	fclose(in);
	fputs("{\"missing\":[", out);
	for (int i = 0, nc = 0; i < nsaved; i++) {
		Client *c = ctlmatch(i, claimed, nc);
		if (c) {
			claimed[nc++] = c;
			ctlapply(i, c);
			moved++;
		} else if (!nospawn && saved[i].cmd[0]) {
			const char *argv[] = { "/bin/sh", "-c", saved[i].cmd, NULL };
			Arg a = { .v = argv };
			spawn(&a);
			spawned++;
		} else {
			fprintf(out, "%s", nmiss++ ? "," : "");
			jstr(out, saved[i].app);
		}
	}
	ctlreorder();
	fprintf(out, "],\"restored\":%d,\"spawned\":%d}\n", moved, spawned);
	printstatus();
}

static int
ctlread(int fd, uint32_t mask, void *data)
{
	int cfd = accept(fd, NULL, NULL);
	char buf[1024];
	ssize_t r;
	char *rest, *nl, *mem = NULL;
	size_t memlen = 0;
	FILE *out;
	if (cfd < 0)
		return 0;
	fcntl(cfd, F_SETFD, FD_CLOEXEC);
	/* the client writes one line right after connect; give it a moment but never stall the compositor */
	struct timeval tv = { 0, 200000 };
	setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	r = read(cfd, buf, sizeof buf - 1);
	if (r <= 0) { close(cfd); return 0; }
	buf[r] = 0;
	if ((nl = strchr(buf, '\n'))) *nl = 0;
	rest = strchr(buf, ' ');
	if (rest) *rest++ = 0;
	out = open_memstream(&mem, &memlen);
	if (!strcmp(buf, "state")) ctlstate(out);
	else if (!strcmp(buf, "save") && rest) {
		FILE *f = fopen(rest, "w");
		if (f) { ctlstate(f); fclose(f); fprintf(out, "ok\n"); }
		else fprintf(out, "{\"error\":\"cannot write %s\"}\n", rest);
	} else if (!strcmp(buf, "restore") && rest) ctlrestore(rest, out);
	else if (!strcmp(buf, "subscribe")) {
		fclose(out);
		if (api_subscribe(cfd) == 0 && write(cfd, "ok subscribed\n", 14) >= 0) { free(mem); return 0; } /* stays open: api.c owns it */
		if (write(cfd, "error: too many subscribers\n", 28) < 0) {}
		free(mem); close(cfd);
		return 0;
	} else {
		if (rest) rest[-1] = ' ';
		api_exec(buf, out);
	}
	fclose(out);
	if (mem) { if (write(cfd, mem, memlen) < 0) {} free(mem); }
	close(cfd);
	return 0;
}

void
ctlinit(void)
{
	struct sockaddr_un a = { .sun_family = AF_UNIX };
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (!rt)
		return;
	snprintf(ctl_path, sizeof ctl_path, "%s/dwl.sock", rt);
	snprintf(a.sun_path, sizeof a.sun_path, "%s", ctl_path);
	unlink(ctl_path);
	if ((ctl_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return;
	if (bind(ctl_fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(ctl_fd, 8) < 0) {
		close(ctl_fd); ctl_fd = -1; ctl_path[0] = 0;
		return;
	}
	chmod(ctl_path, 0600);
	fcntl(ctl_fd, F_SETFD, FD_CLOEXEC);
	fd_set_nonblock(ctl_fd);
	ctl_src = wl_event_loop_add_fd(event_loop, ctl_fd, WL_EVENT_READABLE, ctlread, NULL);
}

void
cmdinit(void)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (!rt)
		return;
	snprintf(cmd_path, sizeof(cmd_path), "%s/dwl.cmd", rt);
	unlink(cmd_path);
	if (mkfifo(cmd_path, 0600) < 0)
		return;
	if ((cmd_fd = open(cmd_path, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0)
		return;
	cmd_src = wl_event_loop_add_fd(event_loop, cmd_fd, WL_EVENT_READABLE, cmdread, NULL);
}

/* remove the fifo and socket files when the event loop ends */
void
settingscleanup(void)
{
	if (cmd_path[0])
		unlink(cmd_path);
	if (ctl_path[0])
		unlink(ctl_path);
}
