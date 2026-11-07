/* ecs_sched.c - see ecs_sched.h. */
#include "ecs_sched.h"

#include <string.h>

#include "ecs_mem.h"

typedef struct system {
	char *name;
	ecs_phase phase;
	ecs_system_fn fn;
	void *user;
	uint64_t reads, writes;
	bool enabled;
	int batch;               /* level within the phase after build, -1 if not scheduled */
} system_t;

typedef struct edge { int before, after; } edge;

struct ecs_sched {
	ecs_sched_config cfg;
	system_t *sys;
	int n_sys, cap_sys;
	edge *edges;
	int n_edges, cap_edges;

	/* plan: per phase, system ids sorted by (batch, registration) with batch boundaries */
	int *plan[ECS_PHASE_COUNT];
	int plan_n[ECS_PHASE_COUNT];
	int n_batches[ECS_PHASE_COUNT];
	bool dirty;

	ecs_events *events;
	ecs_batch_runner runner;
	void *runner_ctx;
	ecs_task *task_buf;
	int task_cap;

	float time_scale, hit_stop_left;
	bool paused;
	float acc;
	uint64_t fixed_ticks, frames;
};

ecs_sched_config ecs_sched_config_default(void)
{
	ecs_sched_config c = { 1.0f / 60.0f, 5 };
	return c;
}

ecs_sched *ecs_sched_new(const ecs_sched_config *cfg)
{
	ecs_sched *s = ecs_calloc(1, sizeof *s);
	if (!s) return NULL;
	s->cfg = cfg ? *cfg : ecs_sched_config_default();
	if (s->cfg.fixed_dt <= 0.0f) s->cfg.fixed_dt = 1.0f / 60.0f;
	if (s->cfg.max_steps < 1) s->cfg.max_steps = 1;
	s->time_scale = 1.0f;
	return s;
}

void ecs_sched_free(ecs_sched *s)
{
	if (!s) return;
	for (int i = 0; i < s->n_sys; i++) ecs_free(s->sys[i].name);
	for (int p = 0; p < ECS_PHASE_COUNT; p++) ecs_free(s->plan[p]);
	ecs_free(s->sys); ecs_free(s->edges); ecs_free(s->task_buf);
	ecs_free(s);
}

int ecs_sched_add(ecs_sched *s, const char *name, ecs_phase phase, ecs_system_fn fn, void *user, uint64_t reads, uint64_t writes)
{
	if (!s || !fn || (int)phase < 0 || phase >= ECS_PHASE_COUNT) return -1;
	if (s->n_sys == s->cap_sys) {
		int nc = s->cap_sys ? s->cap_sys * 2 : 16;
		system_t *p = ecs_realloc(s->sys, (size_t)nc * sizeof *p);
		if (!p) return -1;
		s->sys = p;
		s->cap_sys = nc;
	}
	size_t n = strlen(name ? name : "") + 1;
	char *nm = ecs_malloc(n);
	if (!nm) return -1;
	memcpy(nm, name ? name : "", n);
	s->sys[s->n_sys] = (system_t){ nm, phase, fn, user, reads, writes, true, -1 };
	s->dirty = true;
	return s->n_sys++;
}

bool ecs_sched_order(ecs_sched *s, int before, int after)
{
	if (!s || before < 0 || after < 0 || before >= s->n_sys || after >= s->n_sys || before == after) return false;
	if (s->sys[before].phase != s->sys[after].phase) return false;
	if (s->n_edges == s->cap_edges) {
		int nc = s->cap_edges ? s->cap_edges * 2 : 16;
		edge *e = ecs_realloc(s->edges, (size_t)nc * sizeof *e);
		if (!e) return false;
		s->edges = e;
		s->cap_edges = nc;
	}
	s->edges[s->n_edges++] = (edge){ before, after };
	s->dirty = true;
	return true;
}

void ecs_sched_set_enabled(ecs_sched *s, int id, bool en)
{
	if (s && id >= 0 && id < s->n_sys && s->sys[id].enabled != en) {
		s->sys[id].enabled = en;
		s->dirty = true;
	}
}

static bool conflicts(const system_t *a, const system_t *b)
{
	return (a->writes & (b->reads | b->writes)) || (b->writes & a->reads);
}

bool ecs_sched_build(ecs_sched *s)
{
	if (!s) return false;
	const int N = s->n_sys;
	int *level = ecs_calloc((size_t)(N ? N : 1), sizeof *level);
	int *indeg = ecs_calloc((size_t)(N ? N : 1), sizeof *indeg);
	int *order = ecs_malloc((size_t)(N ? N : 1) * sizeof *order);
	bool *done = ecs_calloc((size_t)(N ? N : 1), sizeof *done);
	unsigned char *adj = ecs_calloc((size_t)N * (size_t)(N ? N : 1) + 1, 1);   /* adj[i*N+j]: i must run before j */
	if (!level || !indeg || !order || !done || !adj) {
		ecs_free(level); ecs_free(indeg); ecs_free(order); ecs_free(done); ecs_free(adj);
		return false;
	}

	/* edges: explicit ones, plus a conflict edge from the earlier-registered system to the later one unless an explicit edge
	 * already orders that pair the other way. Both only within a phase and among enabled systems. */
	for (int e = 0; e < s->n_edges; e++) {
		int a = s->edges[e].before, b = s->edges[e].after;
		if (s->sys[a].enabled && s->sys[b].enabled) adj[a * N + b] = 1;
	}
	for (int i = 0; i < N; i++) {
		if (!s->sys[i].enabled) continue;
		for (int j = i + 1; j < N; j++) {
			if (!s->sys[j].enabled || s->sys[i].phase != s->sys[j].phase) continue;
			if (adj[i * N + j] || adj[j * N + i]) continue;
			if (conflicts(&s->sys[i], &s->sys[j])) adj[i * N + j] = 1;
		}
	}
	for (int i = 0; i < N; i++)
		for (int j = 0; j < N; j++)
			if (adj[i * N + j]) indeg[j]++;

	/* Kahn's algorithm, always taking the lowest registration index among ready systems (stable, deterministic). */
	int n_done = 0, n_enabled = 0;
	for (int i = 0; i < N; i++) if (s->sys[i].enabled) n_enabled++;
	while (n_done < n_enabled) {
		int pick = -1;
		for (int i = 0; i < N; i++)
			if (s->sys[i].enabled && !done[i] && indeg[i] == 0) { pick = i; break; }
		if (pick < 0) {                                      /* cycle */
			ecs_free(level); ecs_free(indeg); ecs_free(order); ecs_free(done); ecs_free(adj);
			return false;
		}
		done[pick] = true;
		order[n_done++] = pick;
		for (int j = 0; j < N; j++) {
			if (!adj[pick * N + j]) continue;
			indeg[j]--;
			if (level[j] < level[pick] + 1) level[j] = level[pick] + 1;      /* longest path = batch index */
		}
	}

	for (int p = 0; p < ECS_PHASE_COUNT; p++) {
		ecs_free(s->plan[p]);
		s->plan[p] = NULL;
		s->plan_n[p] = 0;
		s->n_batches[p] = 0;
	}
	for (int i = 0; i < N; i++) s->sys[i].batch = -1;
	for (int p = 0; p < ECS_PHASE_COUNT; p++) {
		int cnt = 0, maxl = -1;
		for (int k = 0; k < n_done; k++) if (s->sys[order[k]].phase == (ecs_phase)p) { cnt++; if (level[order[k]] > maxl) maxl = level[order[k]]; }
		if (!cnt) continue;
		int *pl = ecs_malloc((size_t)cnt * sizeof *pl);
		if (!pl) continue;
		int w = 0;
		for (int l = 0; l <= maxl; l++)                       /* by batch, then registration order within a batch */
			for (int i = 0; i < N; i++)
				if (s->sys[i].enabled && s->sys[i].phase == (ecs_phase)p && level[i] == l) { pl[w++] = i; s->sys[i].batch = l; }
		s->plan[p] = pl;
		s->plan_n[p] = w;
		s->n_batches[p] = maxl + 1;
	}
	s->dirty = false;
	ecs_free(level); ecs_free(indeg); ecs_free(order); ecs_free(done); ecs_free(adj);
	return true;
}

void ecs_sched_set_events(ecs_sched *s, ecs_events *ev) { if (s) s->events = ev; }
void ecs_sched_set_runner(ecs_sched *s, ecs_batch_runner fn, void *ctx) { if (s) { s->runner = fn; s->runner_ctx = ctx; } }

static void run_phase(ecs_sched *s, ecs_phase p, float dt)
{
	const int n = s->plan_n[p];
	int i = 0;
	while (i < n) {
		int batch = s->sys[s->plan[p][i]].batch, j = i;
		while (j < n && s->sys[s->plan[p][j]].batch == batch) j++;
		int count = j - i;
		if (s->runner && count > 1) {
			if (s->task_cap < count) {
				ecs_task *t = ecs_realloc(s->task_buf, (size_t)count * sizeof *t);
				if (!t) goto serial;
				s->task_buf = t;
				s->task_cap = count;
			}
			for (int k = 0; k < count; k++) {
				const system_t *sy = &s->sys[s->plan[p][i + k]];
				s->task_buf[k] = (ecs_task){ sy->fn, sy->user, dt };
			}
			s->runner(s->runner_ctx, s->task_buf, count);
		} else {
		serial:
			for (int k = i; k < j; k++) {
				const system_t *sy = &s->sys[s->plan[p][k]];
				sy->fn(sy->user, dt);
			}
		}
		i = j;
	}
}

int ecs_sched_update(ecs_sched *s, float frame_dt)
{
	if (!s) return 0;
	if (s->dirty && !ecs_sched_build(s)) return 0;
	if (frame_dt < 0.0f) frame_dt = 0.0f;
	s->frames++;

	/* real time first: hit-stop eats real seconds, then scaled time drives the simulation */
	float real = frame_dt;
	if (s->hit_stop_left > 0.0f) {
		float eat = real < s->hit_stop_left ? real : s->hit_stop_left;
		s->hit_stop_left -= eat;
		real -= eat;
	}
	float sim = (s->paused || s->hit_stop_left > 0.0f) ? 0.0f : real * s->time_scale;

	run_phase(s, ECS_PHASE_PRE_UPDATE, sim);

	int steps = 0;
	s->acc += sim;
	while (s->acc >= s->cfg.fixed_dt && steps < s->cfg.max_steps) {
		run_phase(s, ECS_PHASE_FIXED_UPDATE, s->cfg.fixed_dt);
		s->acc -= s->cfg.fixed_dt;
		s->fixed_ticks++;
		steps++;
		if (s->events) ecs_events_dispatch(s->events);
	}
	if (s->acc >= s->cfg.fixed_dt) s->acc = 0.0f;           /* capped: drop the backlog instead of spiralling */

	run_phase(s, ECS_PHASE_UPDATE, sim);
	run_phase(s, ECS_PHASE_POST_UPDATE, sim);
	if (s->events) ecs_events_dispatch(s->events);
	return steps;
}

void ecs_sched_set_time_scale(ecs_sched *s, float scale) { if (s) s->time_scale = scale < 0.0f ? 0.0f : scale; }
void ecs_sched_set_paused(ecs_sched *s, bool paused) { if (s) s->paused = paused; }
void ecs_sched_hit_stop(ecs_sched *s, float seconds) { if (s && seconds > s->hit_stop_left) s->hit_stop_left = seconds; }
uint64_t ecs_sched_fixed_ticks(const ecs_sched *s) { return s->fixed_ticks; }
uint64_t ecs_sched_frames(const ecs_sched *s) { return s->frames; }
float ecs_sched_alpha(const ecs_sched *s) { return s->acc / s->cfg.fixed_dt; }
float ecs_sched_fixed_dt(const ecs_sched *s) { return s->cfg.fixed_dt; }
int ecs_sched_batch_count(const ecs_sched *s, ecs_phase p) { return s->n_batches[p]; }
int ecs_sched_batch_of(const ecs_sched *s, int id) { return (id >= 0 && id < s->n_sys) ? s->sys[id].batch : -1; }
