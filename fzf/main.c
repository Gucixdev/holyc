/* main.c - fzf in C: reads the candidates, runs either the non-interactive filter (-f) or the interface. */
#include "fzf.h"
#include "opts.h"
#include <fcntl.h>
#include <signal.h>

typedef struct { Item **v; size_t n, cap; } ItemVec;

static void vec_push(ItemVec *v, Item *it)
{
	if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 1024; v->v = xrealloc(v->v, v->cap * sizeof(Item *)); }
	it->index = (uint32_t)v->n;
	v->v[v->n++] = it;
}

/* reads every line of fd into items; returns false when there is nothing to read from (a terminal) */
static void read_all(const Opts *o, FILE *f, ItemVec *out)
{
	Buf line;
	buf_init(&line);
	int c;
	char sep = o->read0 ? 0 : '\n';
	char chunk[65536];
	size_t got;
	while ((got = fread(chunk, 1, sizeof chunk, f)) > 0) {
		for (size_t i = 0; i < got; i++) {
			c = (unsigned char)chunk[i];
			if (c == sep) { vec_push(out, item_new(o, line.p ? line.p : "", line.len)); buf_reset(&line); }
			else buf_addc(&line, (char)c);
		}
	}
	if (line.len) vec_push(out, item_new(o, line.p, line.len));
	buf_free(&line);
}

static void print_item(const Opts *o, const Item *it, FILE *out)
{
	if (o->ansi && it->nansi >= 0) {
		Buf b;
		buf_init(&b);
		ansi_extract(it->orig, it->olen, &b, NULL, NULL, NULL);
		fwrite(b.p ? b.p : "", 1, b.len, out);
		buf_free(&b);
	} else fwrite(it->orig, 1, it->olen, out);
	fputc(o->print0 ? 0 : '\n', out);
}

static int filter_mode(Opts *o, ItemVec *items)
{
	Pattern *p = pattern_new(o, o->filter);
	size_t nh;
	Hit *hits = search_items(o, p, items->v, items->n, &nh);
	if (o->print_query) { fputs(o->filter, stdout); fputc(o->print0 ? 0 : '\n', stdout); }
	for (size_t i = 0; i < nh; i++) print_item(o, items->v[hits[i].idx], stdout);
	free(hits);
	pattern_free(p);
	return nh ? 0 : 1;
}

int ui_main(Opts *o);                                             /* ui.c */

int main(int argc, char **argv)
{
	locale_init();
	signal(SIGPIPE, SIG_IGN);
	Opts *o = opts_new();
	opts_parse(o, argc, argv);
	opts_finish(o);
	ItemVec items = {0};
	if (o->has_filter) {
		if (isatty(0)) die("no input to filter (pipe candidates to fzf -f)");
		read_all(o, stdin, &items);
		return filter_mode(o, &items);
	}
	return ui_main(o);
}
