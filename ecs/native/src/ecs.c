/* ecs.c - see ecs.h for the design. Entities are a packed (generation << 24 | index) handle; the low 24 bits index a slot table shared
 * by every archetype (~16M live slots before that would overflow; freed indices are recycled via a free-list so a long-running world
 * doesn't just grow forever); the high 8 bits are a generation counter for that slot, bumped on every destroy, so a handle to a
 * destroyed-and-recycled entity reads as dead rather than silently pointing at whatever now lives in its old slot. Only caveat inherent
 * to any fixed-width generation counter: after exactly 256 destroys of the same slot, a very old stale handle could in principle collide
 * with the current generation - the same trade-off flecs and EnTT make, not worth widening the handle to dodge in a first version.
 *
 * Every entity lives in exactly one archetype (a table for its exact component signature); add/remove moves it to a neighbouring
 * archetype, copying its other components' data along a row at a time. Archetypes are linked by cached add/remove edges per component
 * id, so the same repeated transition from the same shape is O(1) after the first time it's ever taken; a cold transition falls back to
 * a linear scan of every existing archetype for one with a matching signature (component ids kept sorted so two archetypes with the
 * same set always compare equal via a straight memcmp). */
#include "ecs.h"
#include "ecs_mem.h"
#include <stdlib.h>
#include <string.h>

#define ECS_INDEX_BITS 24
#define ECS_INDEX_MASK ((1u << ECS_INDEX_BITS) - 1)
#define ECS_GEN_MASK 0xFFu

static uint32_t idx_of(ecs_entity e) { return e & ECS_INDEX_MASK; }
static uint32_t gen_of(ecs_entity e) { return e >> ECS_INDEX_BITS; }
static ecs_entity make_entity(uint32_t idx, uint32_t gen) { return ((gen & ECS_GEN_MASK) << ECS_INDEX_BITS) | (idx & ECS_INDEX_MASK); }
static char *dupstr(const char *s) { size_t n = strlen(s) + 1; char *p = ecs_malloc(n); if (p) memcpy(p, s, n); return p; }

typedef struct { char *name; size_t size; } comp_type;               /* size 0 = a tag */

typedef struct archetype {
	int *ids; int nids;                /* this archetype's shape: sorted component ids */
	void **col;                         /* nids pointers, one packed array per id (NULL for a tag - there's nothing to store) */
	ecs_entity *ent;                    /* n long: which entity owns row i */
	int n, cap;                         /* row count / capacity, shared by ent[] and every column */
	struct archetype **add_edge, **remove_edge;   /* both sized to world->cap_types, indexed by component id */
} archetype;

typedef struct { archetype *arch; int row; } entity_rec;

struct ecs_world {
	uint32_t *gen; bool *alive; int n_slots, cap_slots;
	uint32_t *free_list; int n_free, cap_free;
	int n_alive;
	entity_rec *rec;                    /* parallel to gen/alive, sized cap_slots */
	comp_type *types; int n_types, cap_types;
	archetype **arch; int n_arch, cap_arch;
	archetype *empty_arch;              /* every fresh entity starts here: the archetype with zero components */
};

static bool valid_comp(const ecs_world *w, int comp) { return w && comp >= 0 && comp < w->n_types; }

/* ------------------------------------------------------------------ archetypes */
static int col_index(const archetype *a, int comp)
{
	for (int i = 0; i < a->nids; i++) if (a->ids[i] == comp) return i;
	return -1;
}
static bool sig_equal(const archetype *a, const int *ids, int n) { return a->nids == n && (n == 0 || memcmp(a->ids, ids, (size_t)n * sizeof *ids) == 0); }
static archetype *find_archetype(ecs_world *w, const int *ids, int n)
{
	for (int i = 0; i < w->n_arch; i++) if (sig_equal(w->arch[i], ids, n)) return w->arch[i];
	return NULL;
}
static archetype *archetype_create(ecs_world *w, const int *ids, int n)
{
	archetype *a = ecs_calloc(1, sizeof *a);
	a->nids = n;
	if (n > 0) { a->ids = ecs_malloc((size_t)n * sizeof *a->ids); memcpy(a->ids, ids, (size_t)n * sizeof *a->ids); a->col = ecs_calloc((size_t)n, sizeof *a->col); }
	a->add_edge = ecs_calloc((size_t)(w->cap_types ? w->cap_types : 1), sizeof *a->add_edge);
	a->remove_edge = ecs_calloc((size_t)(w->cap_types ? w->cap_types : 1), sizeof *a->remove_edge);
	if (w->n_arch == w->cap_arch) { w->cap_arch = w->cap_arch ? w->cap_arch * 2 : 8; w->arch = ecs_realloc(w->arch, (size_t)w->cap_arch * sizeof *w->arch); }
	w->arch[w->n_arch++] = a;
	return a;
}
static void archetype_free(archetype *a)
{
	for (int i = 0; i < a->nids; i++) ecs_free(a->col[i]);
	ecs_free(a->ids); ecs_free(a->col); ecs_free(a->ent); ecs_free(a->add_edge); ecs_free(a->remove_edge);
	ecs_free(a);
}
static void archetype_grow(const ecs_world *w, archetype *a)
{
	if (a->n < a->cap) return;
	a->cap = a->cap ? a->cap * 2 : 8;
	a->ent = ecs_realloc(a->ent, (size_t)a->cap * sizeof *a->ent);
	for (int i = 0; i < a->nids; i++) {
		size_t sz = w->types[a->ids[i]].size;
		if (sz > 0) a->col[i] = ecs_realloc(a->col[i], (size_t)a->cap * sz);
	}
}
static int archetype_add_row(const ecs_world *w, archetype *a, ecs_entity e)
{
	archetype_grow(w, a);
	int row = a->n++;
	a->ent[row] = e;
	return row;
}
/* swap-remove: whoever was in the last row takes the freed row's place, so every column and ent[] stay contiguous with no gaps; the
 * moved entity's own world record is fixed up to point at its new row. */
static void archetype_remove_row(ecs_world *w, archetype *a, int row)
{
	int last = a->n - 1;
	if (row != last) {
		a->ent[row] = a->ent[last];
		for (int i = 0; i < a->nids; i++) {
			size_t sz = w->types[a->ids[i]].size;
			if (sz > 0) memcpy((char *)a->col[i] + (size_t)row * sz, (char *)a->col[i] + (size_t)last * sz, sz);
		}
		w->rec[idx_of(a->ent[row])].row = row;
	}
	a->n--;
}
/* the archetype reached by adding/removing one component, via a cached edge if this exact transition has happened before from this
 * exact shape, else a linear search among every existing archetype (creating a new one if truly none matches) - cached both ways
 * before returning, so the reverse transition is also O(1) from here on. */
static archetype *archetype_with_added(ecs_world *w, archetype *from, int comp)
{
	if (from->add_edge[comp]) return from->add_edge[comp];
	int n = from->nids + 1;
	int *ids = ecs_malloc((size_t)n * sizeof *ids);
	int j = 0; bool inserted = false;
	for (int i = 0; i < from->nids; i++) {
		if (!inserted && comp < from->ids[i]) ids[j++] = comp, inserted = true;
		ids[j++] = from->ids[i];
	}
	if (!inserted) ids[j++] = comp;
	archetype *to = find_archetype(w, ids, n);
	if (!to) to = archetype_create(w, ids, n);
	ecs_free(ids);
	from->add_edge[comp] = to;
	to->remove_edge[comp] = from;
	return to;
}
static archetype *archetype_with_removed(ecs_world *w, archetype *from, int comp)
{
	if (from->remove_edge[comp]) return from->remove_edge[comp];
	int n = from->nids - 1;
	int *ids = n > 0 ? ecs_malloc((size_t)n * sizeof *ids) : NULL;
	int j = 0;
	for (int i = 0; i < from->nids; i++) if (from->ids[i] != comp) ids[j++] = from->ids[i];
	archetype *to = find_archetype(w, ids, n);
	if (!to) to = archetype_create(w, ids, n);
	ecs_free(ids);
	from->remove_edge[comp] = to;
	to->add_edge[comp] = from;
	return to;
}
static bool archetype_matches(const archetype *a, const int *ids, int n)
{
	for (int i = 0; i < n; i++) if (col_index(a, ids[i]) < 0) return false;
	return true;
}

/* ------------------------------------------------------------------ world / entities */
ecs_world *ecs_world_new(void)
{
	ecs_world *w = ecs_calloc(1, sizeof *w);
	if (!w) return NULL;
	w->empty_arch = archetype_create(w, NULL, 0);
	return w;
}
void ecs_world_free(ecs_world *w)
{
	if (!w) return;
	for (int i = 0; i < w->n_arch; i++) archetype_free(w->arch[i]);
	ecs_free(w->arch);
	for (int i = 0; i < w->n_types; i++) ecs_free(w->types[i].name);
	ecs_free(w->types);
	ecs_free(w->gen); ecs_free(w->alive); ecs_free(w->rec); ecs_free(w->free_list);
	ecs_free(w);
}

bool ecs_alive(const ecs_world *w, ecs_entity e)
{
	if (!w || e == ECS_NULL) return false;
	uint32_t idx = idx_of(e);
	return idx < (uint32_t)w->n_slots && w->alive[idx] && gen_of(e) == (w->gen[idx] & ECS_GEN_MASK);
}
int ecs_entity_count(const ecs_world *w) { return w ? w->n_alive : 0; }

static void world_ensure_slot(ecs_world *w)
{
	if (w->n_slots < w->cap_slots) return;
	w->cap_slots = w->cap_slots ? w->cap_slots * 2 : 16;
	w->gen = ecs_realloc(w->gen, (size_t)w->cap_slots * sizeof *w->gen);
	w->alive = ecs_realloc(w->alive, (size_t)w->cap_slots * sizeof *w->alive);
	w->rec = ecs_realloc(w->rec, (size_t)w->cap_slots * sizeof *w->rec);
}
ecs_entity ecs_create(ecs_world *w)
{
	if (!w) return ECS_NULL;
	uint32_t idx;
	if (w->n_free > 0) idx = w->free_list[--w->n_free];
	else { world_ensure_slot(w); idx = (uint32_t)w->n_slots++; w->gen[idx] = 0; }
	w->alive[idx] = true;
	w->n_alive++;
	ecs_entity e = make_entity(idx, w->gen[idx]);
	w->rec[idx].arch = w->empty_arch;
	w->rec[idx].row = archetype_add_row(w, w->empty_arch, e);
	return e;
}
void ecs_destroy(ecs_world *w, ecs_entity e)
{
	if (!ecs_alive(w, e)) return;
	uint32_t idx = idx_of(e);
	entity_rec *r = &w->rec[idx];
	archetype_remove_row(w, r->arch, r->row);
	r->arch = w->empty_arch; r->row = -1;
	w->alive[idx] = false;
	w->gen[idx]++;
	if (w->n_free == w->cap_free) { w->cap_free = w->cap_free ? w->cap_free * 2 : 16; w->free_list = ecs_realloc(w->free_list, (size_t)w->cap_free * sizeof *w->free_list); }
	w->free_list[w->n_free++] = idx;
	w->n_alive--;
}

/* ------------------------------------------------------------------ component types */
int ecs_component_id(const ecs_world *w, const char *name)
{
	if (!w || !name) return -1;
	for (int i = 0; i < w->n_types; i++) if (!strcmp(w->types[i].name, name)) return i;
	return -1;
}
int ecs_register(ecs_world *w, const char *name, size_t size)
{
	if (!w || !name || ecs_component_id(w, name) >= 0) return -1;
	if (w->n_types == w->cap_types) {
		int old_cap = w->cap_types;
		w->cap_types = w->cap_types ? w->cap_types * 2 : 8;
		w->types = ecs_realloc(w->types, (size_t)w->cap_types * sizeof *w->types);
		for (int i = 0; i < w->n_arch; i++) {                 /* every archetype's edge tables are indexed by component id, so they all grow too */
			archetype *a = w->arch[i];
			a->add_edge = ecs_realloc(a->add_edge, (size_t)w->cap_types * sizeof *a->add_edge);
			a->remove_edge = ecs_realloc(a->remove_edge, (size_t)w->cap_types * sizeof *a->remove_edge);
			for (int k = old_cap; k < w->cap_types; k++) { a->add_edge[k] = NULL; a->remove_edge[k] = NULL; }
		}
	}
	comp_type *t = &w->types[w->n_types];
	t->name = dupstr(name);
	t->size = size;
	return w->n_types++;
}
int ecs_register_tag(ecs_world *w, const char *name) { return ecs_register(w, name, 0); }
const char *ecs_component_name(const ecs_world *w, int comp) { return valid_comp(w, comp) ? w->types[comp].name : NULL; }
size_t ecs_component_size(const ecs_world *w, int comp) { return valid_comp(w, comp) ? w->types[comp].size : 0; }
int ecs_type_count(const ecs_world *w) { return w ? w->n_types : 0; }

/* ------------------------------------------------------------------ components on an entity */
void *ecs_add(ecs_world *w, ecs_entity e, int comp)
{
	if (!ecs_alive(w, e) || !valid_comp(w, comp)) return NULL;
	entity_rec *r = &w->rec[idx_of(e)];
	archetype *from = r->arch;
	int ci = col_index(from, comp);
	if (ci >= 0) { size_t sz = w->types[comp].size; return sz ? (char *)from->col[ci] + (size_t)r->row * sz : (void *)(uintptr_t)1; }

	archetype *to = archetype_with_added(w, from, comp);
	int newrow = archetype_add_row(w, to, e);
	for (int i = 0; i < from->nids; i++) {                    /* carry every component the entity already had along to its new row */
		size_t sz = w->types[from->ids[i]].size;
		if (sz == 0) continue;
		int tci = col_index(to, from->ids[i]);
		memcpy((char *)to->col[tci] + (size_t)newrow * sz, (char *)from->col[i] + (size_t)r->row * sz, sz);
	}
	int newci = col_index(to, comp);
	size_t newsz = w->types[comp].size;
	void *result = newsz ? (char *)to->col[newci] + (size_t)newrow * newsz : (void *)(uintptr_t)1;
	if (newsz) memset((char *)to->col[newci] + (size_t)newrow * newsz, 0, newsz);
	archetype_remove_row(w, from, r->row);
	r->arch = to; r->row = newrow;
	return result;
}
void ecs_remove(ecs_world *w, ecs_entity e, int comp)
{
	if (!ecs_alive(w, e) || !valid_comp(w, comp)) return;
	entity_rec *r = &w->rec[idx_of(e)];
	archetype *from = r->arch;
	if (col_index(from, comp) < 0) return;
	archetype *to = archetype_with_removed(w, from, comp);
	int newrow = archetype_add_row(w, to, e);
	for (int i = 0; i < to->nids; i++) {                      /* carry over everything except the one being dropped */
		size_t sz = w->types[to->ids[i]].size;
		if (sz == 0) continue;
		int fci = col_index(from, to->ids[i]);
		memcpy((char *)to->col[i] + (size_t)newrow * sz, (char *)from->col[fci] + (size_t)r->row * sz, sz);
	}
	archetype_remove_row(w, from, r->row);
	r->arch = to; r->row = newrow;
}
void *ecs_get(ecs_world *w, ecs_entity e, int comp)
{
	if (!ecs_alive(w, e) || !valid_comp(w, comp)) return NULL;
	entity_rec *r = &w->rec[idx_of(e)];
	int ci = col_index(r->arch, comp);
	if (ci < 0) return NULL;
	size_t sz = w->types[comp].size;
	return sz ? (char *)r->arch->col[ci] + (size_t)r->row * sz : (void *)(uintptr_t)1;
}
bool ecs_has(const ecs_world *w, ecs_entity e, int comp)
{
	if (!ecs_alive(w, e) || !valid_comp(w, comp)) return false;
	return col_index(w->rec[idx_of(e)].arch, comp) >= 0;
}
int ecs_type_count_entities(const ecs_world *w, int comp)
{
	if (!valid_comp(w, comp)) return 0;
	int total = 0;
	for (int i = 0; i < w->n_arch; i++) if (col_index(w->arch[i], comp) >= 0) total += w->arch[i]->n;
	return total;
}

/* ------------------------------------------------------------------ views */
struct ecs_view {
	ecs_world *w;
	int *ids; int n;
	archetype **matched; int n_matched;
	int arch_i, row;         /* ecs_view_next's cursor: which matched archetype, which row within it */
	int table_i;             /* ecs_view_next_table's cursor: -1 before the first call */
};
ecs_view *ecs_view_new(ecs_world *w, const int *comps, int n)
{
	if (!w || n < 0 || (n > 0 && !comps)) return NULL;
	ecs_view *v = ecs_calloc(1, sizeof *v);
	v->w = w; v->n = n; v->table_i = -1;
	if (n > 0) { v->ids = ecs_malloc((size_t)n * sizeof *v->ids); memcpy(v->ids, comps, (size_t)n * sizeof *v->ids); }
	v->matched = ecs_malloc((size_t)(w->n_arch ? w->n_arch : 1) * sizeof *v->matched);
	for (int i = 0; i < w->n_arch; i++) if (archetype_matches(w->arch[i], v->ids, v->n)) v->matched[v->n_matched++] = w->arch[i];
	return v;
}
void ecs_view_free(ecs_view *v) { if (v) { ecs_free(v->ids); ecs_free(v->matched); ecs_free(v); } }
int ecs_view_archetype_count(const ecs_view *v) { return v ? v->n_matched : 0; }

bool ecs_view_next(ecs_view *v, ecs_entity *out)
{
	if (!v) return false;
	while (v->arch_i < v->n_matched) {
		archetype *a = v->matched[v->arch_i];
		if (v->row < a->n) { *out = a->ent[v->row]; v->row++; return true; }
		v->arch_i++; v->row = 0;
	}
	return false;
}

bool ecs_view_next_table(ecs_view *v)
{
	if (!v) return false;
	v->table_i++;
	return v->table_i < v->n_matched;
}
static archetype *current_table(const ecs_view *v) { return (v && v->table_i >= 0 && v->table_i < v->n_matched) ? v->matched[v->table_i] : NULL; }
int ecs_view_count(const ecs_view *v) { archetype *a = current_table(v); return a ? a->n : 0; }
const ecs_entity *ecs_view_entities(const ecs_view *v) { archetype *a = current_table(v); return a ? a->ent : NULL; }
void *ecs_view_column(const ecs_view *v, int comp)
{
	archetype *a = current_table(v);
	if (!a || !valid_comp(v->w, comp)) return NULL;
	int ci = col_index(a, comp);
	return (ci >= 0 && v->w->types[comp].size > 0) ? a->col[ci] : NULL;
}
