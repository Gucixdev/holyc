/* ui.c - the event loop that ties the parts together. Everything that can happen later - timers (lt_ui_after / lt_ui_every), the tick hooks,
 * the spring animations - is an entry in one priority queue keyed by its due time, so the loop sleeps in poll() exactly until the
 * earliest of them or the next key press: an idle UI costs nothing and nothing wakes it needlessly. Pending input is drained and
 * coalesced (a burst of mouse drags is one move) before a single frame is drawn, into a canvas that is diffed against the previous
 * one and written as one burst; the diff buffer and the frame arena are reused, so a steady-state frame allocates nothing. */
#include "lazytui.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct { lt_event_fn fn; void *ctx; } ev_hook;
typedef struct { lt_tick_fn fn; void *ctx; } tick_hook;
typedef struct { int id; double every; lt_timer_fn fn; void *ctx; bool live; } utimer;

#define ID_TICK (-1)
#define ID_ANIM (-2)
#define MAXPEND 64

struct lt_ui {
	lt_tty *t;
	lt_canvas *cur, *prev;
	lt_anim anim;
	lt_layout *layout;
	lt_theme theme;
	lt_draw_fn draw; void *draw_ctx;
	ev_hook *ev; int nev;
	tick_hook *tk; int ntk;
	bool quit, redraw, prev_valid;
	lt_pq *due;                       /* (time, timer id): the loop's whole schedule */
	utimer *tm; int ntm, captm, next_id;
	double tick_period, next_tick, last_tick;
	double next_anim, last_anim;
	lt_arena *frame_arena;
	char *out; size_t outcap;         /* the diff buffer, reused */
	double frame_ms; unsigned long frames;
	bool tick_redraw;
	double frame_sum, frame_max;
};

lt_ui *lt_ui_new(lt_tty *t)
{
	lt_ui *u = calloc(1, sizeof *u);
	u->t = t;
	int rows, cols;
	lt_tty_size(t, &rows, &cols);
	u->cur = lt_canvas_new(cols, rows, lt_tty_caps(t));
	u->prev = lt_canvas_new(cols, rows, lt_tty_caps(t));
	lt_anim_init(&u->anim);
	u->layout = lt_layout_new();
	u->theme = lt_theme_default();
	u->redraw = true;
	u->due = lt_pq_new();
	u->frame_arena = lt_arena_new(256 * 1024);
	u->tick_period = 1.0 / 30.0;
	u->tick_redraw = true;
	u->next_id = 1;
	return u;
}
void lt_ui_free(lt_ui *u)
{
	if (!u) return;
	lt_canvas_free(u->cur); lt_canvas_free(u->prev); lt_layout_free(u->layout);
	lt_pq_free(u->due); lt_arena_free(u->frame_arena);
	free(u->tm); free(u->out);
	free(u->ev); free(u->tk); free(u);
}
lt_anim *lt_ui_anim(lt_ui *u) { return &u->anim; }
lt_layout *lt_ui_layout(lt_ui *u) { return u->layout; }
const lt_theme *lt_ui_theme(const lt_ui *u) { return &u->theme; }
void lt_ui_set_theme(lt_ui *u, lt_theme t) { u->theme = t; u->redraw = true; }
lt_tty *lt_ui_term(lt_ui *u) { return u->t; }
void lt_ui_on_draw(lt_ui *u, lt_draw_fn fn, void *ctx) { u->draw = fn; u->draw_ctx = ctx; }
void lt_ui_on_event(lt_ui *u, lt_event_fn fn, void *ctx) { u->ev = realloc(u->ev, (size_t)(u->nev + 1) * sizeof *u->ev); u->ev[u->nev++] = (ev_hook){fn, ctx}; }
void lt_ui_on_tick(lt_ui *u, lt_tick_fn fn, void *ctx) { u->tk = realloc(u->tk, (size_t)(u->ntk + 1) * sizeof *u->tk); u->tk[u->ntk++] = (tick_hook){fn, ctx}; }
void lt_ui_redraw(lt_ui *u) { u->redraw = true; }
void lt_ui_quit(lt_ui *u) { u->quit = true; }

/* ---- timers ---- */
static int add_timer(lt_ui *u, double s, double every, lt_timer_fn fn, void *ctx)
{
	if (!fn || !(s >= 0)) return 0;
	if (u->ntm == u->captm) { u->captm = u->captm ? u->captm * 2 : 8; u->tm = realloc(u->tm, (size_t)u->captm * sizeof *u->tm); }
	int id = u->next_id++;
	u->tm[u->ntm++] = (utimer){id, every, fn, ctx, true};
	lt_pq_push(u->due, lt_now() + s, id, NULL);
	return id;
}
int lt_ui_after(lt_ui *u, double seconds, lt_timer_fn fn, void *ctx) { return add_timer(u, seconds, 0, fn, ctx); }
int lt_ui_every(lt_ui *u, double seconds, lt_timer_fn fn, void *ctx) { return seconds > 0 ? add_timer(u, seconds, seconds, fn, ctx) : 0; }
void lt_ui_cancel(lt_ui *u, int id)
{
	for (int i = 0; i < u->ntm; i++) if (u->tm[i].id == id && u->tm[i].live) { u->tm[i].live = false; lt_pq_remove(u->due, id); }
}
void lt_ui_set_tick_redraw(lt_ui *u, bool on) { u->tick_redraw = on; }
void lt_ui_set_fps(lt_ui *u, double fps) { if (fps >= 1 && fps <= 240) u->tick_period = 1.0 / fps; }
lt_arena *lt_ui_arena(lt_ui *u) { return u->frame_arena; }
double lt_ui_frame_ms(const lt_ui *u) { return u->frame_ms; }
unsigned long lt_ui_frames(const lt_ui *u) { return u->frames; }
double lt_ui_frame_avg_ms(const lt_ui *u) { return u->frames ? u->frame_sum / (double)u->frames : 0; }
double lt_ui_frame_max_ms(const lt_ui *u) { return u->frame_max; }

static void frame(lt_ui *u)
{
	double t0 = lt_now();
	int w = lt_canvas_w(u->cur), h = lt_canvas_h(u->cur);
	lt_arena_reset(u->frame_arena);
	lt_layout_solve(u->layout, w, h);
	lt_canvas_clear(u->cur, u->theme.s[LT_T_TEXT]);
	if (u->draw) u->draw(u, u->cur, u->draw_ctx);
	size_t n = lt_canvas_diff_buf(u->cur, u->prev_valid ? u->prev : NULL, lt_tty_origin(u->t), &u->out, &u->outcap);
	lt_tty_write(u->t, u->out, n);
	lt_canvas_copy(u->prev, u->cur);
	u->prev_valid = true;
	u->redraw = false;
	u->frame_ms = (lt_now() - t0) * 1000.0;
	u->frame_sum += u->frame_ms;
	if (u->frame_ms > u->frame_max) u->frame_max = u->frame_ms;
	u->frames++;
}

static void handle(lt_ui *u, lt_event *ev)
{
	if (ev->kind == LT_EV_RESIZE) {
		int rows, cols;
		lt_tty_size(u->t, &rows, &cols);
		lt_canvas_resize(u->cur, cols, rows);
		lt_canvas_resize(u->prev, cols, rows);
		u->prev_valid = false;
		lt_tty_write(u->t, "\033[2J", 4);
		u->redraw = true;
	} else {
		if (ev->kind == LT_EV_MODS) u->redraw = true;
		bool handled = false;
		for (int i = 0; i < u->nev && !handled; i++) handled = u->ev[i].fn(u, ev, u->ev[i].ctx);
		if (!handled && ev->kind == LT_EV_KEY && ev->key == (LT_MOD_CTRL | 'c')) u->quit = true;
		u->redraw = true;
	}
	free(ev->paste);
	ev->paste = NULL;
}

static bool is_drag(const lt_event *e) { return e->kind == LT_EV_MOUSE && (e->key & 0xFFFFFF) == LT_K_DRAG; }

/* everything already waiting on the terminal, with runs of mouse drags collapsed to the last one (dragging is absolute: only where the
 * pointer is now matters), so a fast drag does not queue up frames behind it */
static int drain(lt_ui *u, lt_event *first, lt_event *out)
{
	int n = 0;
	out[n++] = *first;
	lt_event e;
	while (n < MAXPEND && lt_tty_wait(u->t, 0, &e)) out[n++] = e;
	int k = 0;
	for (int i = 0; i < n; i++) {
		if (is_drag(&out[i]) && i + 1 < n && is_drag(&out[i + 1])) continue;
		out[k++] = out[i];
	}
	return k;
}

static bool animating(const lt_ui *u) { for (int i = 0; i < u->anim.n; i++) if (!lt_spring_at_rest(&u->anim.sp[i])) return true; return false; }

void lt_ui_run(lt_ui *u)
{
	double now = lt_now();
	u->last_tick = now; u->next_tick = now + u->tick_period;
	u->last_anim = now; u->next_anim = now;
	while (!u->quit) {
		if (u->redraw) frame(u);
		now = lt_now();
		bool anim = animating(u);
		double wake = INFINITY, k;
		if (lt_pq_peek(u->due, &k, NULL, NULL)) wake = k;
		if (u->ntk && u->next_tick < wake) wake = u->next_tick;
		if (anim) { if (u->next_anim < now) u->next_anim = now; if (u->next_anim < wake) wake = u->next_anim; }
		int timeout = isinf(wake) ? -1 : (int)ceil((wake - now) * 1000.0);
		if (timeout < 0 && !isinf(wake)) timeout = 0;
		lt_event ev;
		if (lt_tty_wait(u->t, timeout, &ev)) {
			lt_event batch[MAXPEND];
			int n = drain(u, &ev, batch);
			for (int i = 0; i < n && !u->quit; i++) handle(u, &batch[i]);
			for (int i = 0; i < n; i++) if (batch[i].paste) free(batch[i].paste);        /* dropped (coalesced) events own nothing, but be safe */
		}
		now = lt_now();
		double due;
		int id;
		while (lt_pq_peek(u->due, &due, &id, NULL) && due <= now) {                /* timers */
			lt_pq_pop(u->due, NULL, NULL, NULL);
			for (int i = 0; i < u->ntm; i++) {
				utimer *tm = &u->tm[i];
				if (tm->id != id || !tm->live) continue;
				lt_timer_fn fn = tm->fn; void *ctx = tm->ctx;
				if (tm->every > 0) { double nx = due + tm->every; if (nx <= now) nx = now + tm->every; lt_pq_push(u->due, nx, id, NULL); }
				else tm->live = false;
				fn(u, ctx);                                                          /* may add or cancel timers: tm is not touched after this */
				u->redraw = true;
				break;
			}
		}
		if (anim && now >= u->next_anim) {
			double dt = now - u->last_anim;
			if (dt > 0.25) dt = 0.25;
			u->last_anim = now; u->next_anim = now + 1.0 / 60.0;
			lt_anim_tick(&u->anim, dt);
			u->redraw = true;
		}
		if (u->ntk && now >= u->next_tick) {
			double dt = now - u->last_tick;
			if (dt > 0.25) dt = 0.25;
			u->last_tick = now; u->next_tick = now + u->tick_period;
			for (int i = 0; i < u->ntk; i++) u->tk[i].fn(u, dt, u->tk[i].ctx);
			if (u->tick_redraw) u->redraw = true;
		}
		if (u->ntm > 32) {                                                              /* compact the dead timers now and then */
			int k2 = 0;
			for (int i = 0; i < u->ntm; i++) if (u->tm[i].live) u->tm[k2++] = u->tm[i];
			u->ntm = k2;
		}
	}
}
