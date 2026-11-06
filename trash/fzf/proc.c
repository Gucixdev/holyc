/* proc.c - everything that talks to other processes: the input feed and reload, the preview command, execute/become,
 * placeholders in command templates ({}, {+}, {q}, {n}, {1..2}), the history file. */
#include "uistate.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern void ui_items_clear(void);
extern void ui_after_feed(void);

/* ------------------------------------------------------------------ processes */
static const char *shell_of(void)
{
	if (U.o->with_shell && *U.o->with_shell) return U.o->with_shell;
	const char *s = getenv("SHELL");
	return (s && *s) ? s : "sh";
}

char *env_setup(void)
{
	char b[64];
	int rows, cols;
	term_size(&rows, &cols);
	snprintf(b, sizeof b, "%d", U.g.h ? U.g.h : rows); setenv("FZF_LINES", b, 1);
	snprintf(b, sizeof b, "%d", U.g.w ? U.g.w : cols); setenv("FZF_COLUMNS", b, 1);
	snprintf(b, sizeof b, "%d", U.pv_rect.h); setenv("FZF_PREVIEW_LINES", b, 1);
	snprintf(b, sizeof b, "%d", U.pv_rect.w); setenv("FZF_PREVIEW_COLUMNS", b, 1);
	char *q = ui_query_str();
	setenv("FZF_QUERY", q, 1);
	free(q);
	setenv("FZF_PROMPT", U.prompt, 1);
	snprintf(b, sizeof b, "%zu", U.nhits); setenv("FZF_MATCH_COUNT", b, 1);
	snprintf(b, sizeof b, "%zu", U.items.n); setenv("FZF_TOTAL_COUNT", b, 1);
	snprintf(b, sizeof b, "%zu", U.nsel); setenv("FZF_SELECT_COUNT", b, 1);
	snprintf(b, sizeof b, "%zu", U.nhits ? U.cur + 1 : 0); setenv("FZF_POS", b, 1);
	Item *it = ui_current();
	if (it) { snprintf(b, sizeof b, "%u", it->index); setenv("FZF_CURRENT_INDEX", b, 1); } else unsetenv("FZF_CURRENT_INDEX");
	setenv("FZF_INPUT_STATE", U.hide_input ? "hidden" : "enabled", 1);
	setenv("FZF_KEY", U.last_key ? U.last_key : "", 1);
	setenv("FZF_ACTION", U.last_key ? U.last_key : "", 1);
	setenv("FZF_PORT", U.listen_port ? U.listen_port : "", 1);
	setenv("FZF_LAYOUT", U.o->layout == LAYOUT_REVERSE ? "reverse" : U.o->layout == LAYOUT_REVERSE_LIST ? "reverse-list" : "default", 1);
	return NULL;
}

/* runs cmd through the shell with its stdout+stderr on a pipe we read; stdin is /dev/null */
bool proc_start(Proc *p, const char *cmd, int stdin_fd)
{
	int fds[2];
	if (pipe(fds) < 0) return false;
	env_setup();
	pid_t pid = fork();
	if (pid < 0) { close(fds[0]); close(fds[1]); return false; }
	if (pid == 0) {
		setpgid(0, 0);
		int in = stdin_fd >= 0 ? stdin_fd : open("/dev/null", O_RDONLY);
		dup2(in, 0);
		dup2(fds[1], 1);
		dup2(fds[1], 2);
		close(fds[0]); close(fds[1]);
		signal(SIGPIPE, SIG_DFL);
		execlp(shell_of(), shell_of(), "-c", cmd, (char *)NULL);
		_exit(127);
	}
	setpgid(pid, pid);
	close(fds[1]);
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	p->fd = fds[0]; p->pid = pid; p->live = true;
	return true;
}

void proc_kill(Proc *p)
{
	if (!p->live) return;
	kill(-p->pid, SIGKILL);
	kill(p->pid, SIGKILL);
	waitpid(p->pid, NULL, 0);
	if (p->fd >= 0) close(p->fd);
	p->live = false; p->fd = -1;
}

void proc_reap(Proc *p)
{
	if (!p->live) return;
	waitpid(p->pid, NULL, WNOHANG);
	if (p->fd >= 0) close(p->fd);
	p->live = false; p->fd = -1;
	waitpid(p->pid, NULL, 0);
}

int proc_run_capture(const char *cmd, Buf *out)
{
	Proc p = {-1, 0, false};
	if (!proc_start(&p, cmd, -1)) return -1;
	char tmp[8192];
	for (;;) {
		struct pollfd pf = {p.fd, POLLIN, 0};
		poll(&pf, 1, 5000);
		ssize_t n = read(p.fd, tmp, sizeof tmp);
		if (n > 0) buf_addn(out, tmp, (size_t)n);
		else if (n == 0 || (errno != EAGAIN && errno != EINTR)) break;
	}
	int st = 0;
	close(p.fd);
	waitpid(p.pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

void proc_execute(const char *cmd, bool silent)
{
	env_setup();
	if (silent) {
		pid_t pid = fork();
		if (pid == 0) {
			setsid();
			int dn = open("/dev/null", O_RDWR);
			dup2(dn, 0); dup2(dn, 1); dup2(dn, 2);
			execlp(shell_of(), shell_of(), "-c", cmd, (char *)NULL);
			_exit(127);
		}
		if (pid > 0) waitpid(pid, NULL, 0);
		return;
	}
	term_close();
	pid_t pid = fork();
	if (pid == 0) {
		int t = open("/dev/tty", O_RDWR);
		if (t >= 0) { dup2(t, 0); dup2(t, 1); dup2(t, 2); }
		execlp(shell_of(), shell_of(), "-c", cmd, (char *)NULL);
		_exit(127);
	}
	if (pid > 0) waitpid(pid, NULL, 0);
	term_open(U.fullscreen, U.o->mouse);
	ui_resize();
}

void proc_become(const char *cmd)
{
	env_setup();
	term_close();
	int t = open("/dev/tty", O_RDWR);
	if (t >= 0) { dup2(t, 0); dup2(t, 2); }
	execlp(shell_of(), shell_of(), "-c", cmd, (char *)NULL);
	fprintf(stderr, "fzf: become: cannot run %s\n", cmd);
	_exit(127);
}

/* ------------------------------------------------------------------ placeholders */
char *proc_quote(const char *s)
{
	Buf b;
	buf_init(&b);
	buf_addc(&b, '\'');
	for (; *s; s++) {
		if (*s == '\'') buf_adds(&b, "'\\''");
		else buf_addc(&b, *s);
	}
	buf_addc(&b, '\'');
	return buf_take(&b);
}

static char *item_line(const Item *it)
{
	if (U.o->ansi) {
		Buf b;
		buf_init(&b);
		ansi_extract(it->orig, it->olen, &b, NULL, NULL, NULL);
		return buf_take(&b);
	}
	return xstrndup(it->orig, it->olen);
}

static char *trim_blanks(char *s)
{
	size_t n = strlen(s), a = 0;
	while (a < n && (s[a] == ' ' || s[a] == '\t')) a++;
	while (n > a && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
	char *r = xstrndup(s + a, n - a);
	free(s);
	return r;
}

/* the fields of the item's original line selected by spec ("1", "2..", "-1", "1,3") */
static char *item_fields(const Item *it, const char *spec, bool keep_space)
{
	Range *rg = NULL;
	int n = ranges_parse(spec, &rg);
	if (n <= 0) { free(rg); return xstrdup(""); }
	Opts tmp = *U.o;
	tmp.with_nth = rg; tmp.nwith = n; tmp.with_tmpl = NULL; tmp.ansi = U.o->ansi;
	Item *t = item_new(&tmp, it->orig, it->olen);
	char *r = xstrndup(t->text, t->tlen);
	item_free(t);
	free(rg);
	return keep_space ? r : trim_blanks(r);
}

static void append_items(Buf *out, Item **v, size_t n, const char *fields, bool idx, bool raw, bool keep_space, bool tmpfile)
{
	Buf all;
	buf_init(&all);
	for (size_t i = 0; i < n; i++) {
		char *s;
		char nb[24];
		if (idx) { snprintf(nb, sizeof nb, "%u", v[i]->index); s = xstrdup(nb); }
		else if (fields && *fields) s = item_fields(v[i], fields, keep_space);
		else s = item_line(v[i]);
		if (i) buf_addc(&all, tmpfile ? '\n' : ' ');
		if (tmpfile) buf_adds(&all, s);
		else if (raw) buf_adds(&all, s);
		else { char *q = proc_quote(s); buf_adds(&all, q); free(q); }
		free(s);
	}
	if (tmpfile) {
		char path[] = "/tmp/fzf-preview-XXXXXX";
		int fd = mkstemp(path);
		if (fd >= 0) { if (write(fd, all.p ? all.p : "", all.len) < 0) {} close(fd); }
		char *q = proc_quote(path);
		buf_adds(out, q);
		free(q);
	} else buf_addn(out, all.p ? all.p : "", all.len);
	buf_free(&all);
}

char *expand_template(const char *t, bool for_preview)
{
	(void)for_preview;
	Buf out;
	buf_init(&out);
	Item *cur = ui_current();
	for (const char *p = t; *p; p++) {
		if (*p == '\\' && p[1] == '{') { buf_addc(&out, '{'); p++; continue; }
		if (*p != '{') { buf_addc(&out, *p); continue; }
		const char *e = strchr(p, '}');
		if (!e) { buf_addc(&out, *p); continue; }
		/* nested braces inside the field spec are not allowed, so the first '}' ends the placeholder */
		char *body = xstrndup(p + 1, (size_t)(e - p - 1));
		const char *s = body;
		bool plus = false, raw = false, keep = false, file = false, idx = false;
		for (; *s == '+' || *s == 'r' || *s == 's' || *s == 'f' || *s == 'n'; s++) {
			if (*s == '+') plus = true;
			else if (*s == 'r') raw = true;
			else if (*s == 's') keep = true;
			else if (*s == 'f') file = true;
			else if (*s == 'n') { idx = true; if (s[1]) { /* "{n}" vs "{nfoo}" */ } else { s++; break; } }
		}
		if (!strcmp(body, "q") || !strcmp(body, "fzf:query")) {
			char *q = ui_query_str();
			char *qq = proc_quote(q);
			buf_adds(&out, qq);
			free(q); free(qq);
		} else if (!strcmp(body, "fzf:action") || !strcmp(body, "fzf:prompt")) {
			char *qq = proc_quote(body[4] == 'a' ? (U.last_key ? U.last_key : "") : U.prompt);
			buf_adds(&out, qq); free(qq);
		} else if (!*s || (s[0] == 'n' && !s[1]) || (idx && !*s)) {
			Item *one[1] = {cur};
			if (plus) {
				if (U.nsel) append_items(&out, U.sel, U.nsel, "", idx, raw, keep, file);
				else if (cur) append_items(&out, one, 1, "", idx, raw, keep, file);
			} else if (cur) append_items(&out, one, 1, "", idx, raw, keep, file);
			else if (!idx) buf_adds(&out, "''");
		} else if (isdigit((unsigned char)*s) || *s == '-' || *s == '.') {
			Item *one[1] = {cur};
			if (plus) {
				if (U.nsel) append_items(&out, U.sel, U.nsel, s, false, raw, keep, file);
				else if (cur) append_items(&out, one, 1, s, false, raw, keep, file);
			} else if (cur) append_items(&out, one, 1, s, false, raw, keep, file);
			else buf_adds(&out, "''");
		} else {
			buf_addn(&out, p, (size_t)(e - p + 1));                    /* unknown: left as it was */
		}
		free(body);
		p = e;
	}
	return buf_take(&out);
}

/* ------------------------------------------------------------------ input feed and reload */
typedef struct { ItemVec *items; ItemVec *hdr; int left; } Sink;
static Sink feed_sink, reload_sink;
static ItemVec reload_items, reload_hdr;
static bool reload_swapped;

static void take_line(Sink *k, const char *s, size_t n)
{
	if (k->left > 0) { itemvec_push(k->hdr, item_new(U.o, s, n)); k->left--; return; }
	itemvec_push(k->items, item_new(U.o, s, n));
}

/* reads what is available from pr, cuts lines; *eof is set at the end of the stream; returns true if it got data */
static bool consume(Proc *pr, Buf *part, Sink *k, bool *eof)
{
	char tmp[65536];
	char sep = U.o->read0 ? 0 : '\n';
	bool any = false;
	for (int rounds = 0; rounds < 8; rounds++) {
		ssize_t n = read(pr->fd, tmp, sizeof tmp);
		if (n < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) return any; *eof = true; break; }
		if (n == 0) { *eof = true; break; }
		any = true;
		size_t start = 0;
		for (size_t i = 0; i < (size_t)n; i++) {
			if (tmp[i] != sep) continue;
			buf_addn(part, tmp + start, i - start);
			take_line(k, part->p ? part->p : "", part->len);
			buf_reset(part);
			start = i + 1;
		}
		buf_addn(part, tmp + start, (size_t)n - start);
	}
	if (*eof && part->len) { take_line(k, part->p, part->len); buf_reset(part); }
	return any;
}

void feed_setup(void)
{
	U.feed.fd = -1; U.feed.live = false;
	buf_init(&U.feed_part);
	feed_sink = (Sink){&U.items, &U.hdr, U.hdr_left};
	if (!isatty(0)) {
		U.feed.fd = 0; U.feed.pid = 0; U.feed.live = true;
		fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
		return;
	}
	const char *cmd = getenv("FZF_DEFAULT_COMMAND");
	if (!cmd || !*cmd)
		cmd = "command find -L . -mindepth 1 \\( -path '*/.*' -o -fstype 'sysfs' -o -fstype 'devfs' -o -fstype 'devtmpfs' -o -fstype 'proc' \\) -prune "
		      "-o -type f -print -o -type d -print -o -type l -print 2>/dev/null | cut -b3-";
	if (!proc_start(&U.feed, cmd, -1)) { U.feed_done = true; return; }
}

void feed_read(void)
{
	if (!U.feed.live) return;
	bool eof = false;
	bool any = consume(&U.feed, &U.feed_part, &feed_sink, &eof);
	if (any || eof) U.need_search = true;
	if (eof) {
		if (U.feed.pid) proc_reap(&U.feed); else U.feed.live = false;
		U.feed_done = true;
		ui_after_feed();
	}
}

void reload_start(const char *cmd, bool sync)
{
	proc_kill(&U.reload);
	buf_init(&U.reload_part);
	buf_reset(&U.reload_part);
	reload_items = (ItemVec){0}; reload_hdr = (ItemVec){0};
	reload_swapped = false;
	U.reload_sync = sync;
	if (!proc_start(&U.reload, cmd, -1)) return;
	reload_sink = (Sink){&reload_items, &reload_hdr, U.o->header_lines};
	U.feed_done = false;
}

static void reload_swap(void)
{
	ui_items_clear();
	U.items = reload_items; U.hdr = reload_hdr;
	reload_items = (ItemVec){0}; reload_hdr = (ItemVec){0};
	reload_sink.items = &U.items; reload_sink.hdr = &U.hdr;
	reload_swapped = true;
	U.need_search = true;
}

void reload_read(void)
{
	if (!U.reload.live) return;
	bool eof = false;
	bool any = consume(&U.reload, &U.reload_part, &reload_sink, &eof);
	if (any && !U.reload_sync && !reload_swapped) reload_swap();
	else if (any && reload_swapped) U.need_search = true;
	if (eof) {
		proc_reap(&U.reload);
		if (!reload_swapped) reload_swap();
		U.feed_done = true;
		U.need_search = true;
		ui_after_feed();
	}
}

/* ------------------------------------------------------------------ preview */
void pv_kill(void) { proc_kill(&U.pv); }

void pv_schedule(void)
{
	if (!U.pv_cmd || U.pv_hidden) return;
	U.pv_pending = true;
	U.pv_start_at = now_sec() + 0.04;
}

static int scroll_expr(void)
{
	if (!U.pw_active.scroll) return 0;
	char *e = expand_template(U.pw_active.scroll, false);
	int base = -1, height = U.pv_rect.h - U.pw_active.header_lines;
	if (height < 0) height = 0;
	/* strip quotes the expansion added around a number */
	Buf b;
	buf_init(&b);
	for (char *c = e; *c; c++) if (*c != '\'') buf_addc(&b, *c);
	char *s = buf_take(&b);
	free(e);
	for (char *c = s; *c;) {
		if (*c == '+' || *c == '-' || *c == '/') {
			bool neg = false;
			char *st = c;
			if (*c == '-' && c[1] == '/') { c++; }
			if (*c == '/') {
				int d = atoi(c + 1);
				if (d) base -= height / d;
				break;
			}
			if (*st == '-') neg = true;
			long v = strtol(c + 1, &c, 10);
			base += neg ? -(int)v : (int)v;
		} else c++;
	}
	free(s);
	return base < 0 ? 0 : base;
}

void pv_start(void)
{
	U.pv_pending = false;
	proc_kill(&U.pv);
	Item *it = ui_current();
	buf_reset(&U.pv_out);
	U.pv_nl = 0;
	U.pv_scroll = 0; U.pv_hscroll = 0;
	U.pv_done = false;
	if (!U.pv_cmd || U.pv_hidden || !it) { U.pv_done = true; return; }
	U.pv_item = it->index;
	char *cmd = expand_template(U.pv_cmd, true);
	if (!proc_start(&U.pv, cmd, -1)) U.pv_done = true;
	free(cmd);
	U.pv_scroll = scroll_expr();
}

static void pv_index(size_t from)
{
	if (U.pv_nl == 0) {
		if (U.pv_cap == 0) { U.pv_cap = 256; U.pv_lines = xmalloc(U.pv_cap * sizeof(size_t)); }
		U.pv_lines[U.pv_nl++] = 0;
	}
	for (size_t i = from; i < U.pv_out.len; i++) {
		if (U.pv_out.p[i] != '\n') continue;
		if (i + 1 >= U.pv_out.len) break;                  /* a trailing newline does not start a line */
		if (U.pv_nl == U.pv_cap) { U.pv_cap *= 2; U.pv_lines = xrealloc(U.pv_lines, U.pv_cap * sizeof(size_t)); }
		U.pv_lines[U.pv_nl++] = i + 1;
	}
}

void pv_poll(void)
{
	if (!U.pv.live) return;
	char tmp[65536];
	for (int r = 0; r < 16; r++) {
		ssize_t n = read(U.pv.fd, tmp, sizeof tmp);
		if (n > 0) {
			size_t before = U.pv_out.len;
			buf_addn(&U.pv_out, tmp, (size_t)n);
			/* a form feed... clear-screen escape resets the preview (fzf keeps the output after the last one) */
			pv_index(before ? before - 1 : 0);
			if (U.pw_active.follow) U.pv_scroll = 1 << 28;
			continue;
		}
		if (n < 0 && errno == EAGAIN) return;
		if (n < 0 && errno == EINTR) continue;
		proc_reap(&U.pv);
		U.pv_done = true;
		return;
	}
}

int pv_nfds(void) { return U.pv.live ? U.pv.fd : -1; }

void pv_scroll(int lines)
{
	int hl = U.pw_active.header_lines;
	int maxs = (int)U.pv_nl - hl - (U.pv_rect.h - hl);
	if (maxs < 0) maxs = 0;
	long v = (long)U.pv_scroll + lines;
	if (U.pw_active.follow && lines < 0) U.pw_active.follow = false;
	if (v < 0) v = 0;
	if (v > maxs) v = maxs;
	U.pv_scroll = (int)v;
}

void pv_page(int dir, bool half)
{
	int h = U.pv_rect.h - U.pw_active.header_lines;
	if (h < 1) h = 1;
	pv_scroll(dir * (half ? (h + 1) / 2 : h));
}

void pv_toggle(void)
{
	U.pv_hidden = !U.pv_hidden;
	if (U.pv_hidden) { proc_kill(&U.pv); return; }
	pv_schedule();
}

/* ------------------------------------------------------------------ history */
void history_load(void)
{
	if (!U.o->history_file) return;
	FILE *f = fopen(U.o->history_file, "r");
	if (!f) return;
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, f)) > 0) {
		if (line[n - 1] == '\n') line[--n] = 0;
		U.hist = xrealloc(U.hist, (size_t)(U.nhist + 1) * sizeof(char *));
		U.hist[U.nhist++] = xstrdup(line);
	}
	free(line);
	fclose(f);
	U.hist_pos = U.nhist;
}

void history_save(const char *q)
{
	if (!U.o->history_file || !*q) return;
	if (U.nhist && !strcmp(U.hist[U.nhist - 1], q)) return;
	U.hist = xrealloc(U.hist, (size_t)(U.nhist + 1) * sizeof(char *));
	U.hist[U.nhist++] = xstrdup(q);
	int from = U.nhist > U.o->history_size ? U.nhist - U.o->history_size : 0;
	FILE *f = fopen(U.o->history_file, "w");
	if (!f) return;
	chmod(U.o->history_file, 0600);
	for (int i = from; i < U.nhist; i++) fprintf(f, "%s\n", U.hist[i]);
	fclose(f);
}

/* ------------------------------------------------------------------ item keys (--track, --id-nth) */
char *item_key(const Item *it)
{
	if (U.o->nid > 0) {
		Opts tmp = *U.o;
		tmp.with_nth = U.o->id_nth; tmp.nwith = U.o->nid; tmp.with_tmpl = NULL;
		Item *t = item_new(&tmp, it->orig, it->olen);
		char *r = xstrndup(t->text, t->tlen);
		item_free(t);
		return r;
	}
	return xstrndup(it->text, it->tlen);
}

/* ------------------------------------------------------------------ --listen: HTTP on localhost, POST = actions, GET = state */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

void listen_start(void)
{
	if (!U.o->listen_addr) return;
	char *spec = xstrdup(U.o->listen_addr);
	char *colon = strrchr(spec, ':');
	const char *host = "127.0.0.1";
	int port = 0;
	if (colon) { *colon = 0; port = atoi(colon + 1); if (*spec && strcmp(spec, "localhost")) host = spec; }
	else port = atoi(spec);
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) die("cannot create the listen socket");
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	struct sockaddr_in a = {0};
	a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &a.sin_addr) != 1) die("invalid listen address: %s", U.o->listen_addr);
	if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 8) < 0) die("cannot listen on %s", U.o->listen_addr);
	socklen_t l = sizeof a;
	getsockname(s, (struct sockaddr *)&a, &l);
	char pb[16];
	snprintf(pb, sizeof pb, "%d", ntohs(a.sin_port));
	U.listen_port = xstrdup(pb);
	setenv("FZF_PORT", pb, 1);
	fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
	fcntl(s, F_SETFD, FD_CLOEXEC);
	U.listen_fd = s;
	free(spec);
}

static void json_str(Buf *b, const char *s, size_t n)
{
	buf_addc(b, '"');
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\\') { buf_addc(b, '\\'); buf_addc(b, (char)c); }
		else if (c == '\n') buf_adds(b, "\\n");
		else if (c == '\t') buf_adds(b, "\\t");
		else if (c < 32) buf_printf(b, "\\u%04x", c);
		else buf_addc(b, (char)c);
	}
	buf_addc(b, '"');
}

static void reply(int c, int code, const char *body, size_t n)
{
	char h[160];
	int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Length: %zu\r\nConnection: close\r\nContent-Type: application/json\r\n\r\n", code, code == 200 ? "OK" : "Bad Request", n);
	if (write(c, h, (size_t)hl) < 0) return;
	if (n && write(c, body, n) < 0) return;
}

static void serve(int c)
{
	Buf req;
	buf_init(&req);
	char tmp[4096];
	size_t need = 0;
	for (int i = 0; i < 50; i++) {
		struct pollfd pf = {c, POLLIN, 0};
		if (poll(&pf, 1, 200) <= 0) break;
		ssize_t n = read(c, tmp, sizeof tmp);
		if (n <= 0) break;
		buf_addn(&req, tmp, (size_t)n);
		char *he = req.p ? memmem(req.p, req.len, "\r\n\r\n", 4) : NULL;
		if (he) {
			size_t hl = (size_t)(he - req.p) + 4;
			const char *cl = strcasestr(req.p, "content-length:");
			size_t clen = cl && cl < he ? (size_t)atol(cl + 15) : 0;
			need = hl + clen;
			if (req.len >= need) break;
		}
	}
	if (!req.len) { buf_free(&req); return; }
	buf_addc(&req, 0);
	bool post = !strncmp(req.p, "POST", 4);
	char *body = strstr(req.p, "\r\n\r\n");
	body = body ? body + 4 : (char *)"";
	if (post) {
		while (*body == ' ' || *body == '\n' || *body == '\r') body++;
		size_t bl = strlen(body);
		while (bl && (body[bl - 1] == '\n' || body[bl - 1] == '\r' || body[bl - 1] == ' ')) body[--bl] = 0;
		ActLoc l = {NULL, 0};
		char *err = NULL;
		/* acts_from_string dies on a bad action: parse in a child-safe way by validating names first */
		if (!bl) err = "empty";
		if (!err) {
			acts_from_string(&l, body);
			free(U.last_key); U.last_key = xstrdup("");
			act_run_list(l.v, l.n);
			acts_free(&l);
			ui_dirty();
		}
		if (err) reply(c, 400, err, strlen(err)); else reply(c, 200, "", 0);
	} else {
		Buf j;
		buf_init(&j);
		char *q = ui_query_str();
		buf_printf(&j, "{\"reading\":%s,\"progress\":0,\"query\":", U.feed_done ? "false" : "true");
		json_str(&j, q, strlen(q));
		buf_printf(&j, ",\"position\":%zu,\"sort\":%s,\"totalCount\":%zu,\"matchCount\":%zu,\"current\":", U.nhits ? U.cur : 0, U.o->sort ? "true" : "false", U.items.n, U.nhits);
		Item *it = ui_current();
		if (it) { buf_printf(&j, "{\"index\":%u,\"text\":", it->index); json_str(&j, it->orig, it->olen); buf_addc(&j, '}'); } else buf_adds(&j, "null");
		buf_addc(&j, '}');
		reply(c, 200, j.p, j.len);
		free(q); buf_free(&j);
	}
	buf_free(&req);
}

void listen_poll(void)
{
	if (U.listen_fd <= 0) return;
	for (int i = 0; i < 8; i++) {
		int c = accept(U.listen_fd, NULL, NULL);
		if (c < 0) return;
		serve(c);
		close(c);
	}
}
