/* vm.c - the bytecode machine: one operand stack of untyped cells and a stack of call frames.
 *
 * The compiler already proved every type, so a cell carries no tag: ADD_I reads two ints, ADD_F two floats. A struct
 * or an option is a run of cells. Integer arithmetic wraps (done in u64, no undefined behaviour); dividing by zero
 * and running out of stack are runtime errors with the line they happened on. A frame needs its slots plus the
 * deepest operand stack the compiler counted for it, and the VM checks that all of it fits before it enters the
 * frame, so no instruction inside can write past the stack.
 *
 * Memory: strings made at run time, vecs and maps are objects on the heap with a reference count. The compiler emits
 * RETAIN when it copies a value out of a variable, DROP when it throws one away and RELEASE when a variable's value
 * dies; an instruction consumes the values it takes, once it is done with them, unless it only borrows a variable's.
 * An object whose count reaches 0 goes on a chain through its own count cell and is freed at once; the objects a
 * freed one held join the chain and the same loop frees them, so freeing never recurses. The heap is a buddy
 * allocator: blocks of 2^k cells, split to fit and merged with their free neighbour (buddy) when freed, so memory a
 * growing vec gave back can make one big block again. The program's own strings live in the text arena, marked by
 * bit 63, and are never counted. */

typedef union {
	i64 i;
	f64 f;
} Cell;

#define VM_STACK (1 << 16)
#define VM_FRAMES (1 << 12)

/* a call frame is one cell: where to return and the caller's frame start */
#define FRAME(ret_pc, fp) ((i64)(ret_pc) << 32 | (i64)(u32)(fp))
#define FRAME_PC(c) ((i32)((c).i >> 32))
#define FRAME_FP(c) ((i32)(u32)(c).i)

static Cell *g_global_cells; /* reserved address space, like the heap */
static Cell g_vstack[VM_STACK];
static Cell g_vframes[VM_FRAMES];

/* The heap: one array of 2^g_heap_order cells, reserved as address space when the VM starts (the system gives it
 * pages only where something is written), like holyc's flat arena. Blocks of 2^k cells, each starting at a multiple
 * of its size; a block's buddy is the other half of the block twice its size. Free blocks of each size are a doubly
 * linked list through their first two cells, -(next + 2) and -(prev + 2), so a freed object's count cell reads as
 * freed (below 1). Cells 0 to 3 are a block of their own that is never freed: an empty string, so a cell of 0 (a
 * zeroed slot) reads as "" and as no object. Handles are indexes, not pointers, so the heap can be copied whole */
#define MAX_HEAP_ORDER 34     /* 128 GiB, and as much again for arenas */
#define DEFAULT_HEAP_ORDER 27 /* 1 GiB; GAEL_HEAP sets another size */
static Cell *g_heap;
static i32 g_heap_order;
static i64 g_free_head[MAX_HEAP_ORDER + 1]; /* the first free block of each size, -1 when none */
static u8 *g_free_order;                    /* k + 1 when a free block of 2^k cells starts at this cell * 4, else 0 */
static i64 g_live;                          /* heap cells handed out and not freed yet, for the leak check */
static int g_check;                         /* GAEL_CHECK=1: freed blocks get a pattern, leaks are reported */

static o free_push(i64 at, i32 k)
{
	i64 head = g_free_head[k];
	g_heap[at].i = -(head + 2);
	g_heap[at + 1].i = -1; /* no previous one */
	if (head >= 0) g_heap[head + 1].i = -(at + 2);
	g_free_head[k] = at;
	g_free_order[at >> 2] = (u8)(k + 1);
}

static o free_unlink(i64 at, i32 k)
{
	i64 next = -g_heap[at].i - 2, prev = -g_heap[at + 1].i - 2;
	if (prev >= 0) g_heap[prev].i = -(next + 2);
	else g_free_head[k] = next;
	if (next >= 0) g_heap[next + 1].i = -(prev + 2);
	g_free_order[at >> 2] = 0;
}

static i32 order_of(i64 n)
{
	i32 k = 2;
	while (k < 62 && ((i64)1 << k) < n) k++;
	return k;
}

/* heap_alloc N: a block of at least N cells, the smallest free one split down to size; 0 for N = 0, -1 when the heap
 * has no free block that big */
static i64 heap_alloc(i64 n)
{
	i32 k, j;
	i64 at;
	if (n <= 0) return 0;
	k = order_of(n);
	for (j = k; j <= g_heap_order && g_free_head[j] < 0; j++) {}
	if (j > g_heap_order) return -1;
	at = g_free_head[j];
	free_unlink(at, j);
	while (j > k) { /* the upper half of each split stays free */
		j--;
		free_push(at + ((i64)1 << j), j);
	}
	g_live += (i64)1 << k;
	return at;
}

/* heap_free AT N: give back the block heap_alloc N returned, merged with its buddy while that one is free too */
static o heap_free(i64 at, i64 n)
{
	i32 k;
	if (at <= 0 || n <= 0) return;
	k = order_of(n);
	g_live -= (i64)1 << k;
	if (g_check) memset(g_heap + at, 0xa5, ((size_t)1 << k) * sizeof(Cell)); /* a stale read shows, a stale count dies */
	while (k < g_heap_order) {
		i64 buddy = at ^ ((i64)1 << k);
		if (g_free_order[buddy >> 2] != k + 1) break;
		free_unlink(buddy, k);
		at &= ~((i64)1 << k);
		k++;
	}
	free_push(at, k);
}

/* heap_grow AT N: make the block heap_alloc N returned twice as big where it is, when it is the lower half of its
 * pair and the upper half (its buddy) is free; 1 when it did. Afterwards it is the block heap_alloc 2N would return */
static int heap_grow(i64 at, i64 n)
{
	i32 k;
	i64 buddy;
	if (at <= 0 || n <= 0) return 0;
	k = order_of(n);
	buddy = at + ((i64)1 << k);
	if (k >= g_heap_order || (at & ((i64)1 << k)) || g_free_order[buddy >> 2] != k + 1) return 0;
	free_unlink(buddy, k);
	g_live += (i64)1 << k;
	return 1;
}

/* Arenas. After the heap's cells comes the arena area, as big: a stack of bump regions, one for each (arena ...)
 * that is running, the innermost on top. While one runs, a new object goes into it; an object's later parts (a vec's
 * bigger block, a map's new entries) go where the object is, or to the heap when its arena is not the innermost any
 * more. Freeing a block in an arena does nothing: an arena's cells come back all at once when it ends. Each arena
 * counts its objects still alive, and when it ends that count must be 0 */
#define MAX_ARENAS 64
typedef struct {
	i64 start;   /* its first cell */
	i64 live[8]; /* its objects not freed yet, by kind */
	i32 line;    /* of the (arena ...) form */
} ArenaLevel;
static ArenaLevel g_arenas[MAX_ARENAS];
static i32 g_narenas;
static i32 g_arena_floor; /* the arenas below it belong to whoever resumed the running coroutine, which makes nothing
                           * in them */
static i64 g_arena_lo, g_arena_top; /* the area's first cell; the innermost arena's next free one */

static i64 arena_take(i64 n)
{
	i64 at = g_arena_top;
	if (n <= 0) return 0;
	if (n > ((i64)2 << g_heap_order) - at) return -1;
	g_arena_top += n;
	return at;
}

/* in_inner H: H was made in the innermost arena */
static int in_inner(i64 h) { return g_narenas > g_arena_floor && h >= g_arenas[g_narenas - 1].start && h < g_arena_top; }

/* obj_alloc N: the cells of a new object: from the innermost arena when one runs, else from the heap */
static i64 obj_alloc(i64 n) { return g_narenas > g_arena_floor ? arena_take(n) : heap_alloc(n); }

/* part_alloc OBJ N: a new block for a part of OBJ: where OBJ is when that is the innermost arena, else on the heap */
static i64 part_alloc(i64 obj, i64 n) { return in_inner(obj) ? arena_take(n) : heap_alloc(n); }

static o part_free(i64 at, i64 n)
{
	if (at < g_arena_lo) heap_free(at, n);
}

/* part_grow AT N: make a block of N cells twice as big where it is: on the heap when its buddy is free, in the
 * innermost arena when it is the last block there; 1 when it did */
static int part_grow(i64 at, i64 n)
{
	if (at < g_arena_lo) return heap_grow(at, n);
	if (!in_inner(at) || n <= 0 || at + n != g_arena_top || n > ((i64)2 << g_heap_order) - g_arena_top) return 0;
	g_arena_top += n;
	return 1;
}

/* part_ungrow AT N: take back what part_grow AT N added */
static o part_ungrow(i64 at, i64 n)
{
	i64 half = (i64)1 << order_of(n);
	if (at < g_arena_lo) heap_free(at + half, half);
	else if (at + 2 * n == g_arena_top) g_arena_top = at + n;
}


/* heap_order_of SIZE: GAEL_HEAP's value, bytes with K, M or G after them, as the order of the largest number of cells
 * (a power of 2) that fits; 0 when it is not a size or is under 32K */
static i32 heap_order_of(const char *s)
{
	char *end;
	u64 bytes;
	i32 k = 12, shift;
	if (*s < '0' || *s > '9') return 0;
	bytes = strtoul(s, &end, 10);
	if (!strchr("KkMmGg", *end) || (*end && end[1])) return 0;
	shift = !*end ? 0 : *end == 'K' || *end == 'k' ? 10 : *end == 'M' || *end == 'm' ? 20 : 30;
	bytes = shift && bytes >> (64 - shift) ? ~(u64)0 : bytes << shift; /* too big: as big as can be */
	while (k < MAX_HEAP_ORDER && ((u64)8 << (k + 1)) <= bytes) k++;
	return ((u64)8 << k) <= bytes ? k : 0;
}

/* vm_init CHECK: reserve the heap, the arena area after it and, as big as the heap, the text arena (a system that
 * refuses gets them halved), make the heap one free block, and take cells 0 to 3 for the empty string */
static o vm_init(int check)
{
	const char *size = getenv("GAEL_HEAP");
	i32 k = size ? heap_order_of(size) : DEFAULT_HEAP_ORDER;
	o *text;
	if (!k) die("GAEL_HEAP: give a size of 32K or more, like 64M or 2G");
	for (;; k--) {
		size_t bytes = (size_t)8 << k;
		g_heap = (Cell *)reserve(2 * bytes); /* the heap, then the arena area */
		g_free_order = (u8 *)reserve(bytes >> 5); /* a byte for every 4 cells of the heap */
		text = reserve(bytes);
		if (g_heap && g_free_order && text) break;
		if (g_heap) munmap((o *)g_heap, 2 * bytes);
		if (g_free_order) munmap(g_free_order, bytes >> 5);
		if (text) munmap(text, bytes);
		if (k == 12) die("the system gave no address space for the heap");
	}
	g_text.base = (u8 *)text;
	g_text.cap = (size_t)8 << k;
	g_global_cells = (Cell *)table(MAX_GLOBAL_CELLS, sizeof(Cell));
	g_heap_order = k;
	g_arena_lo = g_arena_top = (i64)1 << k;
	g_check = check;
	for (k = 0; k <= MAX_HEAP_ORDER; k++) g_free_head[k] = -1;
	free_push(0, g_heap_order);
	(void)heap_alloc(4);
	memset(g_heap, 0, 4 * sizeof(Cell));
	g_live = 0;
}

/* every object starts with its count and its kind; the kind cell holds the type above the low 8 bits (the vec or
 * map type) or, for a string, its length */
enum { OB_RC, OB_KIND };
enum { KIND_STR = 1, KIND_VEC, KIND_MAP, KIND_POOL, KIND_CORO, KIND_CLOSURE };

/* A function value is one cell: 0 none yet, minus (a function's number + 1), or a closure - an object that keeps the
 * values of the locals a (fn ...) used from the code around it: [count][kind and the type of those values][the
 * function's number][the values]. A fn made by (fn ...) takes its closure, or 0, in slot 0, before its arguments. */
enum { CL_FN = 2, CL_DATA };
#define UNSET_FN "this function value was never set: a global read before its def ran"

/* Coroutines. (coroutine (f x)) runs f in a coroutine: stackful, with a value stack and frames of its own, heap blocks
 * that grow when a call needs more. The VM switches between the main program and a coroutine by saving the registers
 * of one (into g_main, or into the coroutine) and loading the other's. A coroutine that pauses (sleep, yield) goes back
 * to whoever ran it - the code that started it the first time, the main program's scheduler after that - and from its
 * first pause on it is in g_coros, which holds a count on it until it ends. Nothing waits for a coroutine: (poll co)
 * looks whether it has ended. A coroutine object: */
enum { CO_STATE = 2, CO_STACK, CO_CAP, CO_FRAMES, CO_FCAP, CO_SP, CO_FP, CO_NF, CO_PC, CO_UNTIL, CO_RESUMER,
       CO_STEP, CO_FLOOR, CO_LINE, CO_LISTED, CO_RESULT };
enum { CS_READY, CS_SLEEPING, CS_RUNNING, CS_DONE, CS_DEAD };
enum { MW_STEP = 1, MW_RUN, MW_SLEEP };
#define MAX_COROS (1 << 16)
#define CORO_STACK0 256 /* the cells a coroutine starts with, doubled when a call needs more */
#define CORO_FRAMES0 16

static i64 g_coros[MAX_COROS]; /* the coroutines that pause, in the order they first did */
static i32 g_ncoros;
static i64 g_cur;              /* the coroutine running; 0: the main program */
static f64 g_coro_time;        /* seconds: step-coroutines adds its dt, the main program's sleep the real time */
static i64 g_step;             /* passes of the scheduler: a coroutine that yields pauses until the next one */
static struct { i32 sp, fp, nf, pc; } g_main;                     /* its registers while a coroutine runs */
static struct { i32 kind, at, last; f64 until, wall; } g_mwait; /* what the main program runs coroutines for (MW_);
                                                                * kind 0: nothing */

static Ty obj_type(i64 h) { return (Ty)((u64)g_heap[h + OB_KIND].i >> 8); }

/* a string value: bit 63 set - the program's own text, offset and length in the text arena; else the heap index of
 * a string object [count][length][bytes and a 0] */
#define STR_LEN(v) ((v) < 0 ? (u32)((u64)(v) & 0xffffffffu) : (u32)((u64)g_heap[(v) + OB_KIND].i >> 8))
#define STR_P(v) ((v) < 0 ? (char *)g_text.base + (((u64)(v) >> 32) & 0x7fffffffu) : (char *)(g_heap + (v) + 2))
#define STR_OFF(v) ((size_t)(((u64)(v) >> 32) & 0x7fffffffu))
static i64 str_cells(u64 len) { return 2 + (i64)((len + 8) / 8); }

static i64 g_pending;    /* objects whose count reached 0, chained through their count cells */
static int g_draining;   /* the chain is being freed: a release inside only adds to it */
static i64 g_objects[8]; /* the strings, vecs, maps and pools alive, by kind */

#define USED_FREED "internal error: an object was used after it was freed"

static o drain(void);

/* obj_born H KIND TYPE: H's first cells become a new object's count (1) and kind */
static o obj_born(i64 h, i64 kind, u64 type)
{
	g_heap[h + OB_RC].i = 1;
	g_heap[h + OB_KIND].i = kind | (i64)(type << 8);
	g_objects[kind]++;
	if (h >= g_arena_lo) g_arenas[g_narenas - 1].live[kind]++;
}

/* obj_died H KIND: the object at H was freed; one less alive in its arena */
static o obj_died(i64 h, i64 kind)
{
	i32 k = g_narenas - 1;
	g_objects[kind]--;
	if (h < g_arena_lo) return;
	while (k >= 0 && h < g_arenas[k].start) k--;
	if (k >= 0) g_arenas[k].live[kind]--; /* else an arena an error left behind (REPL) */
}

static o obj_retain(i64 h)
{
	if (h <= 0) return; /* no object, or the program's own string */
	if (g_heap[h + OB_RC].i <= 0) die(USED_FREED);
	g_heap[h + OB_RC].i++;
}

static o obj_release(i64 h)
{
	if (h <= 0) return;
	if (g_heap[h + OB_RC].i <= 0) die(USED_FREED);
	if (--g_heap[h + OB_RC].i) return;
	g_heap[h + OB_RC].i = -(g_pending + 1);
	g_pending = h;
	if (!g_draining) drain();
}

/* refs V STRIDE TYPE RETAIN: every object inside a value of TYPE, whose cell C is at V + C * STRIDE, gets one owner
 * more (RETAIN) or one less. An object inside a none is not there */
/* ONE_HANDLE(T): a value of type T is one cell, a handle (a string, a vec, a map, a pool, a coroutine, a function): its
 * leaves are the shared leaf 0 */
#define ONE_HANDLE(t) (g_tys[t].leaf_first == 0 && g_tys[t].nleaf == 1)

static o refs(const Cell *v, i64 stride, Ty t, int retain)
{
	i32 k, j, n = g_tys[t].nleaf, first = g_tys[t].leaf_first;
	for (k = 0; k < n; k++) {
		const Leaf *lf = &g_leaves[first + k];
		for (j = 0; j < lf->nguards && v[g_guards[lf->guard_first + j] * stride].i; j++) {}
		if (j < lf->nguards) continue;
		if (retain) obj_retain(v[lf->off * stride].i);
		else obj_release(v[lf->off * stride].i);
	}
}

/* Columns. A vec's and a pool's elements are columns (SoA): cell C of every element in one run, CAP long, so one
 * field of every element is one run of memory - with one-cell elements a plain array. A column of bools (the cells a
 * type's bits mark) is a run of bits instead, 64 to a cell: (vec bool) and the flags of many structs take a bit each. */
static i64 col_words(i64 cap) { return (cap + 63) >> 6; }

static i64 popcount(u64 m)
{
	i64 n = 0;
	for (; m; m &= m - 1) n++;
	return n;
}

/* col_off TYPE C CAP: where column C of CAP elements of TYPE starts, in cells from the data */
static i64 col_off(Ty et, i64 c, i64 cap)
{
	u64 m = g_tys[et].bits;
	i64 nb;
	if (!m) return c * cap;
	nb = popcount(m & (((u64)1 << c) - 1)); /* a type with bools has at most 64 cells, so c < 64 */
	return (c - nb) * cap + nb * col_words(cap);
}

/* cols_size TYPE CAP: the cells the columns of CAP elements of TYPE take */
static i64 cols_size(Ty et, i64 cap)
{
	i64 nb = popcount(g_tys[et].bits);
	return (cells(et) - nb) * cap + nb * col_words(cap);
}

/* cols_load DATA TYPE CAP I TO: element I of the columns at DATA into the cells at TO */
static o cols_load(i64 data, Ty et, i64 cap, i64 i, Cell *to)
{
	const Cell *d = g_heap + data;
	u64 m = g_tys[et].bits;
	i64 c, n = cells(et), off = 0, w;
	if (!m) {
		for (c = 0; c < n; c++) to[c] = d[c * cap + i];
		return;
	}
	for (c = 0, w = col_words(cap); c < n; c++) {
		if (m >> c & 1) { to[c].i = (i64)((u64)d[off + (i >> 6)].i >> (i & 63) & 1); off += w; }
		else { to[c] = d[off + i]; off += cap; }
	}
}

/* cols_store DATA TYPE CAP I FROM: the cells at FROM become element I */
static o cols_store(i64 data, Ty et, i64 cap, i64 i, const Cell *from)
{
	Cell *d = g_heap + data;
	u64 m = g_tys[et].bits;
	i64 c, n = cells(et), off = 0, w;
	if (!m) {
		for (c = 0; c < n; c++) d[c * cap + i] = from[c];
		return;
	}
	for (c = 0, w = col_words(cap); c < n; c++) {
		if (m >> c & 1) {
			u64 word = (u64)d[off + (i >> 6)].i, bit = (u64)1 << (i & 63);
			d[off + (i >> 6)].i = (i64)(from[c].i ? word | bit : word & ~bit);
			off += w;
		} else {
			d[off + i] = from[c];
			off += cap;
		}
	}
}

/* cols_refs DATA TYPE CAP I RETAIN: the objects element I holds get one owner more (RETAIN) or lose one; a bool is
 * never one, nor an option's tag, so their cells are whole */
static o cols_refs(i64 data, Ty et, i64 cap, i64 i, int retain)
{
	const Cell *d = g_heap + data;
	i32 k, j, n = g_tys[et].nleaf, first = g_tys[et].leaf_first;
	for (k = 0; k < n; k++) {
		const Leaf *lf = &g_leaves[first + k];
		i64 at;
		for (j = 0; j < lf->nguards && d[col_off(et, g_guards[lf->guard_first + j], cap) + i].i; j++) {}
		if (j < lf->nguards) continue;
		at = d[col_off(et, lf->off, cap) + i].i;
		if (retain) obj_retain(at);
		else obj_release(at);
	}
}

/* A vec: [count][kind][len][cap][data]; its elements are columns */
enum { VH_LEN = 2, VH_CAP, VH_DATA, VH_CELLS };

static Ty vec_elem(i64 v) { return g_tys[obj_type(v)].elem; }
static Cell *vec_cell0(const Cell *hd, i64 i) { return g_heap + hd[VH_DATA].i + i; } /* of a vec of one-cell, non-bool elements */

static o vec_load(const Cell *hd, Ty et, i64 i, Cell *to) { cols_load(hd[VH_DATA].i, et, hd[VH_CAP].i, i, to); }
static o vec_store(const Cell *hd, Ty et, i64 i, const Cell *from) { cols_store(hd[VH_DATA].i, et, hd[VH_CAP].i, i, from); }

/* A map: [count][kind][entries][used][cap][ent][slots][idx][info][shift]; its entries in the order they went in, each
 * an alive flag, the key and the value's cells; an index of SLOTS cells (a power of two), each 0 or an entry's number +
 * 1 in the low 32 bits and the low 32 bits of the key's hash in the high ones, so a probe looks at an entry only when
 * those agree. A key's probe starts at the top bits of its hash (SHIFT is 64 - log2 SLOTS) and goes on linearly. A removed
 * entry stays in place, dead, until the map grows and packs the live ones. INFO says what the map's type would: the
 * cells of an entry (low 16 bits), whether the keys are strings (bit 16) and whether a value holds objects (bit 17) */
enum { MH_COUNT = 2, MH_USED, MH_CAP, MH_ENT, MH_SLOTS, MH_IDX, MH_INFO, MH_SHIFT, MH_CELLS };
#define MAP_ESIZE(hd) ((hd)[MH_INFO].i & 0xffff)
#define MAP_STRKEYS(hd) ((hd)[MH_INFO].i >> 16 & 1)
#define MAP_VREFS(hd) ((hd)[MH_INFO].i >> 17 & 1)

static i64 map_vsize(i64 m) { return MAP_ESIZE(g_heap + m) - 2; }
static int map_strkeys(i64 m) { return (int)MAP_STRKEYS(g_heap + m); }

/* A pool: [count][kind][cap][elements][free][data][gens]; a fixed number of slots, the elements as columns like a
 * vec's. gens holds each slot's generation, odd while the slot holds an
 * element, then the free list: the next free slot after slot I at gens + cap + I, -1 at the end. A handle is the slot
 * in its low 32 bits and the generation it had in the high 32; the element is still there when they match */
enum { PH_CAP = 2, PH_COUNT, PH_FREE, PH_DATA, PH_GENS, PH_CELLS };


/* pool_slot POOL HANDLE: the slot whose element HANDLE names, or -1 when the handle is stale */
static i64 pool_slot(const Cell *hd, i64 h)
{
	u64 i = (u64)h & 0xffffffffu, gen = (u64)h >> 32;
	if (i >= (u64)hd[PH_CAP].i || !(gen & 1) || (u64)g_heap[hd[PH_GENS].i + (i64)i].i != gen) return -1;
	return (i64)i;
}

static i64 handle_of(const Cell *hd, i64 i) { return (i64)((u64)g_heap[hd[PH_GENS].i + i].i << 32 | (u64)i); }

/* obj_free H: give back an object whose count reached 0; what it held loses an owner */
static o obj_free(i64 h)
{
	Cell *hd = g_heap + h;
	obj_died(h, hd[OB_KIND].i & 7);
	switch (hd[OB_KIND].i & 0xff) {
	case KIND_STR:
		part_free(h, str_cells((u64)hd[OB_KIND].i >> 8));
		break;
	case KIND_VEC: {
		Ty et = vec_elem(h);
		i64 i;
		for (i = 0; g_tys[et].nleaf && i < hd[VH_LEN].i; i++) cols_refs(hd[VH_DATA].i, et, hd[VH_CAP].i, i, 0);
		part_free(hd[VH_DATA].i, cols_size(et, hd[VH_CAP].i));
		part_free(h, VH_CELLS);
		break;
	}
	case KIND_POOL: {
		Ty et = g_tys[obj_type(h)].elem;
		i64 i, cap = hd[PH_CAP].i;
		const Cell *gens = g_heap + hd[PH_GENS].i;
		for (i = 0; g_tys[et].nleaf && i < cap; i++) {
			if (gens[i].i & 1) cols_refs(hd[PH_DATA].i, et, cap, i, 0);
		}
		part_free(hd[PH_DATA].i, cols_size(et, cap));
		part_free(hd[PH_GENS].i, 2 * cap);
		part_free(h, PH_CELLS);
		break;
	}
	case KIND_MAP: {
		Ty vt = g_tys[obj_type(h)].elem;
		i64 e, esize = 2 + cells(vt);
		int strkeys = map_strkeys(h);
		for (e = 0; e < hd[MH_USED].i; e++) {
			const Cell *ent = g_heap + hd[MH_ENT].i + e * esize;
			if (!ent[0].i) continue;
			if (strkeys) obj_release(ent[1].i);
			refs(ent + 2, 1, vt, 0);
		}
		part_free(hd[MH_ENT].i, hd[MH_CAP].i * esize);
		part_free(hd[MH_IDX].i, hd[MH_SLOTS].i);
		part_free(h, MH_CELLS);
		break;
	}
	case KIND_CLOSURE:
		refs(hd + CL_DATA, 1, obj_type(h), 0);
		heap_free(h, CL_DATA + cells(obj_type(h)));
		break;
	case KIND_CORO: {
		Ty rt = g_tys[obj_type(h)].elem;
		if (hd[CO_STATE].i == CS_DONE) refs(hd + CO_RESULT, 1, rt, 0);
		if (hd[CO_STACK].i) { /* a coroutine a runtime error stopped (REPL): what its stack holds cannot be known */
			heap_free(hd[CO_STACK].i, hd[CO_CAP].i);
			heap_free(hd[CO_FRAMES].i, hd[CO_FCAP].i);
		}
		heap_free(h, CO_RESULT + cells(rt));
		break;
	}
	default:
		die(USED_FREED);
	}
}

static o drain(void)
{
	g_draining = 1;
	while (g_pending) {
		i64 h = g_pending;
		g_pending = -g_heap[h + OB_RC].i - 1;
		obj_free(h);
	}
	g_draining = 0;
}

/* vm_error IN MSG: the runtime error at instruction IN, "file:line: runtime error: msg" - the file IN was compiled
 * from, a gem's or a used file's as well as the program's */
static int vm_error(const Ins *in, const char *msg)
{
	fflush(stdout);
	fprintf(stderr, "%s:%ld: runtime error: %s\n", code_file((i32)(in - g_code)), (long)in->line, msg);
	return 1;
}

#define HEAP_FULL "out of memory (the heap is full)"
#define NOT_MADE "this vec, map or pool was never made: a global read before its def ran"
#define STALE "a stale handle: its element was removed from the pool"
#define MADE(h) if ((h) <= 0) return vm_error(in, NOT_MADE)
#define TEXT_FULL "out of memory for strings (the text arena is full)"

/* Strings made at run time are first written to the free end of the text arena, then copied into a string object
 * (text_take); the arena goes back to where it was */
static int text_put(const char *p, size_t n)
{
	if (g_text.cap - g_text.used < n + 1) return 0; /* + 1: room for a 0 after it */
	memcpy(g_text.base + g_text.used, p, n);
	g_text.used += n;
	return 1;
}

static int text_room(void) { return g_text.used < g_text.cap; }

/* str_new P N: a string object with these bytes (0, the empty string kept at the start of the heap, when there are
 * none); -1 when the heap is full */
static i64 str_new(const char *p, size_t n)
{
	i64 h;
	if (!n) return 0;
	if ((h = obj_alloc(str_cells(n))) < 0) return -1;
	obj_born(h, KIND_STR, n);
	memcpy(g_heap + h + 2, p, n);
	((char *)(g_heap + h + 2))[n] = 0;
	return h;
}

/* text_take START: what was written since START as a new string; -1 when the heap is full */
static i64 text_take(size_t start)
{
	i64 h = str_new((const char *)g_text.base + start, g_text.used - start);
	g_text.used = start;
	return h;
}

/* text_const START: what was written since START, kept in the arena for good as a string of the program's own */
static i64 text_const(size_t start)
{
	size_t len = g_text.used - start;
	g_text.base[g_text.used++] = 0;
	return str_cell(start, len);
}

/* c_str STR: the string with a 0 after it, for the C library, written to the free end of the text arena; valid until
 * the next string is made, 0 when there is no room */
static const char *c_str(i64 v)
{
	char *d = (char *)g_text.base + g_text.used;
	u32 n = STR_LEN(v);
	if (g_text.cap - g_text.used < (size_t)n + 1) return 0;
	memcpy(d, STR_P(v), n);
	d[n] = 0;
	return d;
}

/* str_find S PART FROM: where PART first starts in S at or after FROM, or -1 */
static i64 str_find(i64 hay, i64 part, i64 from)
{
	const char *h = STR_P(hay), *p = STR_P(part);
	i64 n = STR_LEN(hay), m = STR_LEN(part), i;
	for (i = from; i + m <= n; i++) {
		if (!memcmp(h + i, p, (size_t)m)) return i;
	}
	return -1;
}

/* str_piece S FROM LEN: bytes of S as a string: a view when S is the program's own text, a copy otherwise (an object
 * of its own, so it outlives S); -1 when the heap is full */
static i64 str_piece(i64 v, i64 from, i64 len)
{
	if (v < 0) return str_cell(STR_OFF(v) + (size_t)from, (size_t)len);
	return str_new(STR_P(v) + from, (size_t)len);
}

static int str_eq(i64 a, i64 b) { return STR_LEN(a) == STR_LEN(b) && !memcmp(STR_P(a), STR_P(b), STR_LEN(a)); }

/* Printing goes to g_sink, or, when it is 0, onto the free end of the text arena: the string being built */
static FILE *g_sink;
static int g_sink_full; /* the text arena had no room for the whole string */

static o out(const char *p, size_t n)
{
	if (g_sink) fwrite(p, 1, n, g_sink);
	else if (!g_sink_full && !text_put(p, n)) g_sink_full = 1;
}

static o out_s(const char *s) { out(s, strlen(s)); }

/* to_f32 V: V rounded to the nearest f32, kept as an f64; past the largest f32, infinity */
static f64 to_f32(f64 v)
{
	if (v > 3.4028234663852886e38) return HUGE_VAL;
	if (v < -3.4028234663852886e38) return -HUGE_VAL;
	return v == v ? (f64)(f32)v : v;
}

static o print_float(f64 v)
{
	char buf[64];
	sprintf(buf, "%.15g", v);
	out_s(buf);
	if (!strpbrk(buf, ".eni")) out_s(".0"); /* 2.0 stays visibly a float; inf and nan stay as they are */
}

/* print_f32 V: an f32 with the fewest digits that read back as the same f32: 0.1, not 0.100000001490116 */
static o print_f32(f64 v)
{
	char buf[64];
	int p;
	for (p = 6; p < 9; p++) {
		sprintf(buf, "%.*g", p, v);
		if (to_f32(strtod(buf, 0)) == v) break;
	}
	sprintf(buf, "%.*g", p, v);
	out_s(buf);
	if (!strpbrk(buf, ".eni")) out_s(".0");
}

/* print_quoted STR: a string inside a struct or an option, written the way the source would write it */
static o print_quoted(i64 v)
{
	const char *p = STR_P(v);
	u32 i, n = STR_LEN(v);
	out_s("\"");
	for (i = 0; i < n; i++) {
		if (p[i] == '"' || p[i] == '\\') { out("\\", 1); out(p + i, 1); }
		else if (p[i] == '\n') out_s("\\n");
		else if (p[i] == '\t') out_s("\\t");
		else out(p + i, 1);
	}
	out_s("\"");
}

#define PRINT_DEPTH 64
#define PRINT_SCRATCH (1 << 16)
static Cell g_print_scratch[PRINT_SCRATCH]; /* a vec's element, gathered from its columns to be printed */

/* print_value TYPE CELLS: one value, as the REPL shows it: (Vec2 :x 1.0 :y 2.0), (some 3), none, north, (vec 1 2) */
static o print_value(Ty type, const Cell *c)
{
	struct {
		Ty t;
		i32 i;
		const Cell *c;
		i32 mark; /* the scratch in use before this value was gathered */
	} st[PRINT_DEPTH];
	i32 n = 1, top = 0;
	st[0].t = type;
	st[0].i = -1;
	st[0].c = c;
	st[0].mark = 0;
	while (n > 0) {
		const TyInfo *ti = &g_tys[st[n - 1].t];
		const Cell *v = st[n - 1].c;
		i32 *i = &st[n - 1].i;
		if (ti->kind == K_VEC) {
			const Cell *hd = g_heap + v->i;
			if (*i < 0) {
				out_s("(vec");
				*i = 0;
			}
			if (*i >= hd[VH_LEN].i) { out_s(")"); top = st[--n].mark; continue; }
			out_s(" ");
			if (n == PRINT_DEPTH || top + cells(ti->elem) > PRINT_SCRATCH) { out_s("..."); (*i)++; continue; }
			st[n].t = ti->elem;
			st[n].i = -1;
			st[n].mark = top;
			vec_load(hd, ti->elem, *i, g_print_scratch + top);
			st[n].c = g_print_scratch + top;
			top += cells(ti->elem);
			(*i)++;
			n++;
			continue;
		}
		if (ti->kind == K_MAP) { /* (map (k v) ...): *i counts three steps per entry - "(", key, value */
			const Cell *hd = g_heap + v->i, *ent;
			i64 esize = 2 + cells(ti->elem), e;
			if (*i < 0) {
				out_s("(map");
				*i = 0;
			}
			e = *i / 3;
			while (*i % 3 == 0 && e < hd[MH_USED].i && !g_heap[hd[MH_ENT].i + e * esize].i) *i = (i32)(++e * 3);
			if (e >= hd[MH_USED].i) { out_s(")"); top = st[--n].mark; continue; }
			ent = g_heap + hd[MH_ENT].i + e * esize;
			if (*i % 3 == 2) { /* the value was printed */
				out_s(")");
				(*i)++;
				continue;
			}
			out_s(*i % 3 ? " " : " (");
			(*i)++;
			if (n == PRINT_DEPTH) { out_s("..."); continue; }
			st[n].t = *i % 3 == 1 ? ti->key : ti->elem;
			st[n].i = -1;
			st[n].mark = top;
			st[n].c = *i % 3 == 1 ? ent + 1 : ent + 2;
			n++;
			continue;
		}
		if (ti->kind == K_STRUCT || ti->kind == K_OPTION) {
			const Field *fd = 0;
			if (*i < 0) {
				if (ti->kind == K_OPTION && !v->i) { out_s("none"); top = st[--n].mark; continue; }
				out_s("(");
				out_s(ti->kind == K_OPTION ? "some" : ti->name);
				*i = 0;
			}
			if (ti->kind == K_STRUCT && *i < ti->n) fd = &g_fields[ti->first + *i];
			if (ti->kind == K_OPTION && *i < 1) out_s(" ");
			else if (!fd) { out_s(")"); top = st[--n].mark; continue; }
			else { out_s(" :"); out_s(fd->name); out_s(" "); }
			(*i)++;
			if (n == PRINT_DEPTH) { out_s("..."); continue; }
			st[n].t = fd ? fd->type : ti->elem;
			st[n].i = -1;
			st[n].mark = top;
			st[n].c = fd ? v + fd->offset : v + 1;
			n++;
			continue;
		}
		if (ti->kind == K_ENUM) {
			out_s(g_variants[ti->first + v->i]);
		} else if (ti->kind == K_FN) {
			out_s("(fn ");
			out_s(v->i < 0 ? g_fns[-v->i - 1].name : v->i > 0 ? g_fns[g_heap[v->i + CL_FN].i].name : "none");
			out_s(")");
		} else if (ti->kind == K_CORO) {
			i64 state = g_heap[v->i + CO_STATE].i;
			out_s(state == CS_DONE ? "(coroutine done)" : state == CS_DEAD ? "(coroutine stopped)" : "(coroutine paused)");
		} else if (ti->kind == K_POOL || ti->kind == K_HANDLE) {
			char buf[64];
			if (ti->kind == K_POOL) sprintf(buf, "(pool %ld/%ld)", (long)g_heap[v->i + PH_COUNT].i, (long)g_heap[v->i + PH_CAP].i);
			else sprintf(buf, "(handle %lu %lu)", (unsigned long)((u64)v->i & 0xffffffffu), (unsigned long)((u64)v->i >> 32));
			out_s(buf);
		} else if (is_int(st[n - 1].t)) {
			char buf[32];
			if (st[n - 1].t == T_U64) sprintf(buf, "%lu", (unsigned long)(u64)v->i);
			else sprintf(buf, "%ld", (long)v->i);
			out_s(buf);
		} else if (st[n - 1].t == T_F32) {
			print_f32(v->f);
		} else if (st[n - 1].t == T_FLOAT) {
			print_float(v->f);
		} else if (st[n - 1].t == T_BOOL) {
			out_s(v->i ? "true" : "false");
		} else if (st[n - 1].t == T_PATH) {
			out_s("$");
			out(STR_P(v->i), STR_LEN(v->i));
		} else if (st[n - 1].t == T_STR && n > 1) {
			print_quoted(v->i);
		} else if (st[n - 1].t == T_STR) {
			out(STR_P(v->i), STR_LEN(v->i));
		} else {
			out_s("()");
		}
		top = st[--n].mark;
	}
}

/* print_fmt FORMAT ARGS: a print, println or format; the compiler checked every % code against its value */
static o print_fmt(const Fmt *fm, const Cell *args)
{
	const Str *fmt = &g_strs[fm->str];
	u32 i;
	i32 k = 0;
	for (i = 0; i < fmt->len; i++) {
		if (fmt->p[i] != '%') { out(fmt->p + i, 1); continue; }
		if (fmt->p[++i] == '%') { out_s("%"); continue; }
		if (fmt->p[i] == 'x') { /* hex, as many bits as the type has */
			char buf[24];
			i32 bits = int_bits(g_fmt_tys[fm->first + k]);
			u64 v = (u64)args->i;
			if (bits < 64) v &= ((u64)1 << bits) - 1;
			sprintf(buf, "%lx", (unsigned long)v);
			out_s(buf);
		} else
			print_value(g_fmt_tys[fm->first + k], args);
		args += cells(g_fmt_tys[fm->first + k]);
		k++;
	}
}

/* release_args FORMAT ARGS: the printed values are consumed */
static o release_args(const Fmt *fm, const Cell *args)
{
	i32 k;
	for (k = 0; k < fm->n; k++) {
		refs(args, 1, g_fmt_tys[fm->first + k], 0);
		args += cells(g_fmt_tys[fm->first + k]);
	}
}

/* trimmed STR START LEN: the string without spaces, tabs and newlines around it */
static o trimmed(i64 v, const char **p, u32 *n)
{
	*p = STR_P(v);
	*n = STR_LEN(v);
	while (*n && strchr(" \t\r\n", (*p)[0])) (*p)++, (*n)--;
	while (*n && strchr(" \t\r\n", (*p)[*n - 1])) (*n)--;
}

/* parse_int STR OUT: 1 when the whole string (spaces around it aside) is an int that fits in 64 bits */
static int parse_int(i64 v, i64 *out_v)
{
	const char *p;
	u32 n, i = 0;
	u64 acc = 0, limit;
	int neg;
	trimmed(v, &p, &n);
	neg = n && p[0] == '-';
	if (n && (p[0] == '-' || p[0] == '+')) i = 1;
	if (i == n) return 0;
	limit = neg ? (u64)1 << 63 : ((u64)1 << 63) - 1;
	for (; i < n; i++) {
		u64 d = (u64)(p[i] - '0');
		if (p[i] < '0' || p[i] > '9' || acc > (limit - d) / 10) return 0;
		acc = acc * 10 + d;
	}
	*out_v = neg ? (i64)(0 - acc) : (i64)acc;
	return 1;
}

/* parse_float STR OUT: 1 when the whole string (spaces around it aside) is a number strtod reads */
static int parse_float(i64 v, f64 *out_v)
{
	char buf[128], *end;
	const char *p;
	u32 n;
	trimmed(v, &p, &n);
	if (!n || n >= sizeof(buf)) return 0;
	memcpy(buf, p, n);
	buf[n] = 0;
	*out_v = strtod(buf, &end);
	return *end == 0;
}

static const char *g_fail; /* why the last helper below failed */

/* run_cmd CMD RES: run a command (0-terminated) and return its standard output as a new string, the newlines at its
 * end left out; 0 when there was no room (the rest of the output is still read, so the command can finish). CMD may
 * sit at the free end of the text arena: popen has passed it to the child before the output lands on it */
static int run_cmd(const char *c, i64 *res)
{
	size_t start = g_text.used;
	int full = !c;
	FILE *pipe = 0;
	fflush(stdout);
	if (c) pipe = popen(c, "r");
	if (pipe) {
		for (;;) {
			char buf[4096];
			size_t got = fread(buf, 1, sizeof(buf), pipe);
			if (!got) break;
			if (!full && !text_put(buf, got)) full = 1;
		}
		pclose(pipe);
	}
	g_fail = TEXT_FULL;
	if (full) {
		g_text.used = start;
		return 0;
	}
	while (g_text.used > start && g_text.base[g_text.used - 1] == '\n') g_text.used--;
	g_fail = HEAP_FULL;
	return (*res = text_take(start)) >= 0;
}

/* run_pipe CMDS N INPUT RES: the commands joined into one pipeline, each in its own subshell; with INPUT the first
 * string goes to the first command's stdin through a temporary file */
static int run_pipe(const Cell *cmds, i64 n, int input, i64 *res)
{
	size_t start = g_text.used;
	char tmp[600];
	int ok = 1, fd;
	i64 k;
	if (input) {
		const char *dir = getenv("TMPDIR");
		if (!dir || strlen(dir) > 500) dir = "/tmp";
		sprintf(tmp, "%s/gael-stdin.XXXXXX", dir);
		g_fail = "<<<: no temporary file for the text";
		if ((fd = mkstemp(tmp)) < 0) return 0;
		ok = write(fd, STR_P(cmds[0].i), STR_LEN(cmds[0].i)) == (long)STR_LEN(cmds[0].i);
		close(fd);
	}
	for (k = input; ok && k < n; k++) { /* "(a) < 'tmp' | (b)", built at the free end of the text arena */
		if (k > input) ok = text_put(" | ", 3);
		ok = ok && text_put("(", 1) && text_put(STR_P(cmds[k].i), STR_LEN(cmds[k].i)) && text_put(")", 1);
		if (ok && input && k == input) ok = text_put(" < '", 4) && text_put(tmp, strlen(tmp)) && text_put("'", 1);
	}
	if (ok) ok = text_room();
	if (ok) {
		g_text.base[g_text.used] = 0;
		g_text.used = start; /* the command was scratch; the output goes where it was */
		ok = run_cmd((const char *)g_text.base + start, res);
	} else {
		g_text.used = start;
		g_fail = TEXT_FULL;
	}
	if (input) unlink(tmp);
	return ok;
}

/* read_into FILE RES: everything left in FILE as a new string */
static int read_into(FILE *in, i64 *res)
{
	size_t start = g_text.used;
	int ok = text_room();
	for (;;) {
		char buf[4096];
		size_t got = fread(buf, 1, sizeof(buf), in);
		if (!got) break;
		if (ok && !text_put(buf, got)) ok = 0;
	}
	g_fail = TEXT_FULL;
	if (!ok) {
		g_text.used = start;
		return 0;
	}
	g_fail = HEAP_FULL;
	return (*res = text_take(start)) >= 0;
}

/* vec_new TYPE N FROM: a vec of type TYPE holding the N elements at FROM (moved in), or N zeros without FROM; -1 when
 * the heap is full */
static i64 vec_new(Ty vt, i64 n, const Cell *from)
{
	Ty et = g_tys[vt].elem;
	i64 size = cells(et), cap = n < 4 ? 4 : n, h = obj_alloc(VH_CELLS), data = obj_alloc(cols_size(et, cap)), i;
	if (h < 0 || data < 0) return -1;
	obj_born(h, KIND_VEC, vt);
	g_heap[h + VH_LEN].i = n;
	g_heap[h + VH_CAP].i = cap;
	g_heap[h + VH_DATA].i = data;
	if (data) memset(g_heap + data, 0, (size_t)cols_size(et, cap) * sizeof(Cell));
	for (i = 0; from && i < n; i++) vec_store(g_heap + h, et, i, from + i * size); /* the stack has them in a row */
	return h;
}

/* vec_push V FROM: move one element in at the end; 0 when the heap is full */
static int vec_push(i64 v, const Cell *from)
{
	Cell *hd = g_heap + v;
	Ty et = vec_elem(v);
	if (hd[VH_LEN].i == hd[VH_CAP].i) { /* twice the room; each column moves to its place in it */
		i64 old = hd[VH_CAP].i, cap = old * 2, data = hd[VH_DATA].i, c, n = cells(et), len = hd[VH_LEN].i;
		if (!g_tys[et].bits && part_grow(data, old * n)) { /* in place: the last column first, each moves up */
			for (c = n - 1; c > 0; c--) memmove(g_heap + data + c * cap, g_heap + data + c * old, (size_t)len * sizeof(Cell));
		} else { /* a bit column does not double with the room, so such a vec always moves */
			if ((data = part_alloc(v, cols_size(et, cap))) < 0) return 0;
			for (c = 0; c < n; c++) {
				i64 run = c < 64 && (g_tys[et].bits >> c & 1) ? col_words(len) : len;
				memcpy(g_heap + data + col_off(et, c, cap), g_heap + hd[VH_DATA].i + col_off(et, c, old), (size_t)run * sizeof(Cell));
			}
			part_free(hd[VH_DATA].i, cols_size(et, old));
		}
		hd[VH_DATA].i = data;
		hd[VH_CAP].i = cap;
	}
	vec_store(hd, et, hd[VH_LEN].i, from);
	hd[VH_LEN].i++;
	return 1;
}

/* hash_key KEY IS_STR: a string's FNV-1a, or an int's (a bool's, an enum value's) Fibonacci hash: times 2^64 / phi,
 * whose top bits a map's index starts from */
static u64 hash_key(i64 key, int is_str)
{
	u64 h;
	if (is_str) {
		const char *p = STR_P(key);
		u32 i, n = STR_LEN(key);
		h = 14695981039346656037UL;
		for (i = 0; i < n; i++) h = (h ^ (u8)p[i]) * 1099511628211UL;
		return h * 11400714819323198485UL; /* FNV's low bits are its best; this brings them to the top */
	}
	return (u64)key * 11400714819323198485UL;
}

/* map_new TYPE: an empty map of this map type; -1 when the heap is full */
static i64 map_new(Ty mt)
{
	i64 esize = 2 + cells(g_tys[mt].elem), h = obj_alloc(MH_CELLS), ent = obj_alloc(8 * esize), idx = obj_alloc(16);
	if (h < 0 || ent < 0 || idx < 0) return -1;
	obj_born(h, KIND_MAP, mt);
	g_heap[h + MH_COUNT].i = 0;
	g_heap[h + MH_USED].i = 0;
	g_heap[h + MH_CAP].i = 8;
	g_heap[h + MH_ENT].i = ent;
	g_heap[h + MH_SLOTS].i = 16;
	g_heap[h + MH_IDX].i = idx;
	g_heap[h + MH_INFO].i = esize | (i64)(g_tys[mt].key == T_STR || g_tys[mt].key == T_PATH) << 16 | (i64)(g_tys[g_tys[mt].elem].nleaf > 0) << 17;
	g_heap[h + MH_SHIFT].i = 60;
	memset(g_heap + idx, 0, 16 * sizeof(Cell));
	return h;
}

/* map_find M KEY SLOT HASH: the entry number of KEY, or -1; *SLOT gets the index slot where it is or would go, and
 * *HASH the key's hash */
static i64 map_find(i64 m, i64 key, i64 *slot, u64 *hash)
{
	const Cell *hd = g_heap + m, *idx = g_heap + hd[MH_IDX].i, *ents = g_heap + hd[MH_ENT].i;
	i64 strkeys = MAP_STRKEYS(hd), mask = hd[MH_SLOTS].i - 1, esize = MAP_ESIZE(hd), at;
	u64 h = hash_key(key, (int)strkeys);
	u32 tag = (u32)h; /* for ints below 2^32 a different key is a different tag: the product is odd, so one to one */
	*hash = h;
	for (at = (i64)(h >> hd[MH_SHIFT].i);; at = (at + 1) & mask) {
		u64 v = (u64)idx[at].i;
		const Cell *ent;
		if (!v) {
			*slot = at;
			return -1;
		}
		if ((u32)(v >> 32) != tag) continue;
		ent = ents + ((i64)(u32)v - 1) * esize;
		if (ent[0].i && (strkeys ? str_eq(ent[1].i, key) : ent[1].i == key)) {
			*slot = at;
			return (i64)(u32)v - 1;
		}
	}
}

/* map_grow M: room for twice the entries, the dead ones packed out, the index made again (it stays at most half
 * full, so a probe always ends); 0 when the heap is full */
static int map_grow(i64 m)
{
	Cell *hd = g_heap + m;
	i64 esize = MAP_ESIZE(hd), cap = hd[MH_CAP].i * 2, slots = hd[MH_SLOTS].i * 2, ent, idx, e, k = 0, mask = slots - 1;
	int idx_here = part_grow(hd[MH_IDX].i, hd[MH_SLOTS].i), ent_here; /* each grows in place when it can */
	idx = idx_here ? hd[MH_IDX].i : part_alloc(m, slots);
	if (idx < 0) return 0;
	ent_here = part_grow(hd[MH_ENT].i, hd[MH_CAP].i * esize);
	ent = ent_here ? hd[MH_ENT].i : part_alloc(m, cap * esize);
	if (ent < 0) { /* the map stays as it was */
		if (idx_here) part_ungrow(idx, hd[MH_SLOTS].i);
		else part_free(idx, slots);
		return 0;
	}
	if (hd[MH_COUNT].i == hd[MH_USED].i) { /* none dead: the entries as they are */
		k = hd[MH_USED].i;
		if (!ent_here) memcpy(g_heap + ent, g_heap + hd[MH_ENT].i, (size_t)(k * esize) * sizeof(Cell));
	} else {
		for (e = 0; e < hd[MH_USED].i; e++) { /* the live ones, packed; in place they only move down */
			const Cell *from = g_heap + hd[MH_ENT].i + e * esize;
			if (from[0].i) memmove(g_heap + ent + k++ * esize, from, (size_t)esize * sizeof(Cell));
		}
	}
	memset(g_heap + idx, 0, (size_t)slots * sizeof(Cell));
	if (!ent_here) part_free(hd[MH_ENT].i, hd[MH_CAP].i * esize);
	if (!idx_here) part_free(hd[MH_IDX].i, hd[MH_SLOTS].i);
	hd[MH_ENT].i = ent;
	hd[MH_IDX].i = idx;
	hd[MH_CAP].i = cap;
	hd[MH_SLOTS].i = slots;
	hd[MH_SHIFT].i--;
	hd[MH_USED].i = k;
	for (e = 0; e < k; e++) { /* the keys are all different: each goes to the first free slot of its probe */
		u64 h = hash_key(g_heap[ent + e * esize + 1].i, (int)MAP_STRKEYS(hd));
		i64 at = (i64)(h >> hd[MH_SHIFT].i);
		while (g_heap[idx + at].i) at = (at + 1) & mask;
		g_heap[idx + at].i = (i64)((u64)(u32)h << 32 | (u64)(e + 1));
	}
	return 1;
}

/* map_insert M KEY VALUE: move the entry in, or give an existing key the new value (the old value and the key just
 * passed lose an owner); 0 when the heap is full */
static int map_insert(i64 m, i64 key, const Cell *val)
{
	Cell *hd = g_heap + m, *ent;
	i64 esize = MAP_ESIZE(hd), vsize = esize - 2, slot, e = 0, k;
	u64 h;
	e = map_find(m, key, &slot, &h);
	if (e >= 0) {
		ent = g_heap + hd[MH_ENT].i + e * esize;
		if (MAP_VREFS(hd)) refs(ent + 2, 1, g_tys[obj_type(m)].elem, 0);
		for (k = 0; k < vsize; k++) ent[2 + k] = val[k];
		if (MAP_STRKEYS(hd)) obj_release(key);
		return 1;
	}
	if (hd[MH_USED].i == hd[MH_CAP].i) {
		if (!map_grow(m)) return 0;
		(void)map_find(m, key, &slot, &h);
	}
	e = hd[MH_USED].i++;
	ent = g_heap + hd[MH_ENT].i + e * esize;
	ent[0].i = 1;
	ent[1].i = key;
	for (k = 0; k < vsize; k++) ent[2 + k] = val[k];
	g_heap[hd[MH_IDX].i + slot].i = (i64)((u64)(u32)h << 32 | (u64)(e + 1));
	hd[MH_COUNT].i++;
	return 1;
}

/* pool_new TYPE CAP: a pool of this type with CAP free slots; -1 when the heap is full */
static i64 pool_new(Ty pt, i64 cap)
{
	i64 h = obj_alloc(PH_CELLS), data = obj_alloc(cols_size(g_tys[pt].elem, cap)), gens = obj_alloc(2 * cap), i;
	if (h < 0 || data < 0 || gens < 0) return -1;
	obj_born(h, KIND_POOL, pt);
	g_heap[h + PH_CAP].i = cap;
	g_heap[h + PH_COUNT].i = 0;
	g_heap[h + PH_FREE].i = 0;
	g_heap[h + PH_DATA].i = data;
	g_heap[h + PH_GENS].i = gens;
	for (i = 0; i < cap; i++) {
		g_heap[gens + i].i = 0;
		g_heap[gens + cap + i].i = i + 1 < cap ? i + 1 : -1;
	}
	return h;
}

/* keep: (keep x) copies onto the heap the objects of the innermost arena that x holds, and the objects of that arena
 * they hold in turn, so x outlives the arena; objects from elsewhere are shared. While it runs, a copied object's
 * count cell holds -(its copy), so an object held twice is copied once and a cycle ends; the counts come back at the
 * end. Its lists sit at the free end of the text arena */
static i64 *g_kept;      /* pairs: an object copied, its count */
static Cell **g_kwork;   /* cells of the copies that still hold an object of the arena */
static i64 g_nkept, g_nkwork, g_kcap;

/* cols_keep DATA TYPE CAP I: keep_leaves for element I of columns */
static int cols_keep(i64 data, Ty et, i64 cap, i64 i)
{
	Cell *d = g_heap + data;
	i32 k, j, n = g_tys[et].nleaf, first = g_tys[et].leaf_first;
	for (k = 0; k < n; k++) {
		const Leaf *lf = &g_leaves[first + k];
		Cell *c = d + col_off(et, lf->off, cap) + i;
		for (j = 0; j < lf->nguards && d[col_off(et, g_guards[lf->guard_first + j], cap) + i].i; j++) {}
		if (j < lf->nguards) continue;
		if (!in_inner(c->i)) obj_retain(c->i); /* shared by the copy */
		else if (g_nkwork == g_kcap) return 0;
		else g_kwork[g_nkwork++] = c;
	}
	return 1;
}

static int keep_leaves(Cell *v, i64 stride, Ty t)
{
	i32 k, j, n = g_tys[t].nleaf, first = g_tys[t].leaf_first;
	for (k = 0; k < n; k++) {
		const Leaf *lf = &g_leaves[first + k];
		Cell *c = v + lf->off * stride;
		for (j = 0; j < lf->nguards && v[g_guards[lf->guard_first + j] * stride].i; j++) {}
		if (j < lf->nguards) continue;
		if (!in_inner(c->i)) obj_retain(c->i); /* shared by the copy */
		else if (g_nkwork == g_kcap) return 0;
		else g_kwork[g_nkwork++] = c;
	}
	return 1;
}

/* keep_copy H: the heap copy of arena object H, with one owner more; -1 when the heap or the lists are full */
static i64 keep_copy(i64 h)
{
	Cell *hd = g_heap + h;
	i64 kind = hd[OB_KIND].i & 0xff, n = -1, a = 0, b = 0, i, e;
	Ty t = obj_type(h);
	if (hd[OB_RC].i < 0) { /* copied already */
		n = -hd[OB_RC].i;
		g_heap[n + OB_RC].i++;
		return n;
	}
	if (g_nkept == g_kcap) return -1;
	if (kind == KIND_STR) {
		i64 size = str_cells((u64)hd[OB_KIND].i >> 8);
		if ((n = heap_alloc(size)) < 0) return -1;
		memcpy(g_heap + n, hd, (size_t)size * sizeof(Cell));
	} else if (kind == KIND_VEC) {
		i64 size = cols_size(g_tys[t].elem, hd[VH_CAP].i);
		if ((n = heap_alloc(VH_CELLS)) < 0 || (a = heap_alloc(size)) < 0) return -1;
		memcpy(g_heap + n, hd, VH_CELLS * sizeof(Cell));
		if (a) memcpy(g_heap + a, g_heap + hd[VH_DATA].i, (size_t)size * sizeof(Cell));
		g_heap[n + VH_DATA].i = a;
		for (i = 0; g_tys[g_tys[t].elem].nleaf && i < hd[VH_LEN].i; i++)
			if (!cols_keep(a, g_tys[t].elem, hd[VH_CAP].i, i)) return -1;
	} else if (kind == KIND_MAP) {
		i64 esize = 2 + cells(g_tys[t].elem);
		if ((n = heap_alloc(MH_CELLS)) < 0 || (a = heap_alloc(hd[MH_CAP].i * esize)) < 0 || (b = heap_alloc(hd[MH_SLOTS].i)) < 0) return -1;
		memcpy(g_heap + n, hd, MH_CELLS * sizeof(Cell));
		memcpy(g_heap + a, g_heap + hd[MH_ENT].i, (size_t)(hd[MH_CAP].i * esize) * sizeof(Cell));
		memcpy(g_heap + b, g_heap + hd[MH_IDX].i, (size_t)hd[MH_SLOTS].i * sizeof(Cell));
		g_heap[n + MH_ENT].i = a;
		g_heap[n + MH_IDX].i = b;
		for (e = 0; e < hd[MH_USED].i; e++) {
			Cell *ent = g_heap + a + e * esize;
			if (!ent[0].i) continue;
			if (g_tys[t].key == T_STR && !keep_leaves(ent + 1, 1, T_STR)) return -1;
			if (!keep_leaves(ent + 2, 1, g_tys[t].elem)) return -1;
		}
	} else if (kind == KIND_POOL) {
		i64 cap = hd[PH_CAP].i, size = cols_size(g_tys[t].elem, cap);
		if ((n = heap_alloc(PH_CELLS)) < 0 || (a = heap_alloc(size)) < 0 || (b = heap_alloc(2 * cap)) < 0) return -1;
		memcpy(g_heap + n, hd, PH_CELLS * sizeof(Cell));
		if (a) memcpy(g_heap + a, g_heap + hd[PH_DATA].i, (size_t)size * sizeof(Cell));
		memcpy(g_heap + b, g_heap + hd[PH_GENS].i, (size_t)(2 * cap) * sizeof(Cell));
		g_heap[n + PH_DATA].i = a;
		g_heap[n + PH_GENS].i = b;
		for (i = 0; g_tys[g_tys[t].elem].nleaf && i < cap; i++)
			if ((g_heap[b + i].i & 1) && !cols_keep(a, g_tys[t].elem, cap, i)) return -1;
	}
	g_heap[n + OB_RC].i = 1;
	g_objects[kind]++;
	g_kept[2 * g_nkept] = h;
	g_kept[2 * g_nkept + 1] = hd[OB_RC].i;
	g_nkept++;
	hd[OB_RC].i = -n;
	return n;
}

/* keep VALUE TYPE: the value on the stack stops holding objects of the innermost arena; 0 when memory ran out */
static int keep(Cell *val, Ty t)
{
	size_t off = (g_text.used + 7) & ~(size_t)7;
	i64 *olds, nolds = 0, k, room = (i64)((g_text.cap - off) / sizeof(i64));
	const TyInfo *ti = &g_tys[t];
	if (g_narenas <= g_arena_floor || !ti->nleaf) return 1;
	olds = (i64 *)(o *)(g_text.base + off);    /* the value's own objects of the arena, released at the end */
	g_kcap = (room - ti->nleaf) / 3;
	g_kept = olds + ti->nleaf;
	g_kwork = (Cell **)(o *)(g_kept + 2 * g_kcap);
	g_nkept = g_nkwork = 0;
	if (g_kcap < 1) return 0;
	for (k = 0; k < ti->nleaf; k++) {
		const Leaf *lf = &g_leaves[ti->leaf_first + k];
		i64 j, h = val[lf->off].i, n;
		for (j = 0; j < lf->nguards && val[g_guards[lf->guard_first + j]].i; j++) {}
		if (j < lf->nguards || !in_inner(h)) continue;
		if ((n = keep_copy(h)) < 0) return 0;
		olds[nolds++] = h;
		val[lf->off].i = n;
	}
	while (g_nkwork > 0) {
		Cell *c = g_kwork[--g_nkwork];
		i64 n = keep_copy(c->i);
		if (n < 0) return 0;
		c->i = n;
	}
	for (k = 0; k < g_nkept; k++) g_heap[g_kept[2 * k]].i = g_kept[2 * k + 1];
	for (k = 0; k < nolds; k++) obj_release(olds[k]);
	return 1;
}

/* arena_error IN ARENA: the runtime error for an arena that ends while objects made in it are still held */
static int arena_error(const Ins *in, const ArenaLevel *ar)
{
	static const char *const kinds[] = { "?", "string", "vec", "map", "pool" };
	char msg[400];
	size_t n = 0;
	i64 k, kinds_left = 0, total = 0;
	for (k = 1; k < 5; k++) {
		kinds_left += ar->live[k] != 0;
		total += ar->live[k];
	}
	for (k = 1; k < 5; k++) { /* "1 string, 2 vecs and 1 map" */
		if (!ar->live[k]) continue;
		kinds_left--;
		sprintf(msg + n, "%ld %s%s%s", (long)ar->live[k], kinds[k], ar->live[k] == 1 ? "" : "s",
		        kinds_left > 1 ? ", " : kinds_left == 1 ? " and " : "");
		n = strlen(msg);
	}
	sprintf(msg + n, " made in this arena %s still held when it ends (kept outside it, or in a cycle); (keep x) copies a value out",
	        total == 1 ? "is" : "are");
	return vm_error(in, msg);
}

/* index_error IN I LEN: the runtime error for an index outside a vec */
static int index_error(const Ins *in, i64 i, i64 len)
{
	char msg[96];
	sprintf(msg, "index %ld is outside a vec of %ld", (long)i, (long)len);
	return vm_error(in, msg);
}

/* range_error IN FROM TO LEN: substr's bytes FROM up to TO do not fit a string of LEN, or TO is before FROM */
static int range_error(const Ins *in, i64 from, i64 to, i64 len)
{
	char msg[160];
	sprintf(msg, "substr %ld %ld does not fit a string of %ld bytes", (long)from, (long)to, (long)len);
	return vm_error(in, msg);
}

/* the command line after the file name, kept in the text arena as the program's own strings */
#define MAX_ARGS 4096
static i64 g_arg_cells[MAX_ARGS];
static i64 g_nargs;

static o args_init(int argc, char **argv)
{
	int i;
	for (i = 0; i < argc && i < MAX_ARGS; i++) {
		size_t start = g_text.used;
		if (!text_put(argv[i], strlen(argv[i]))) die("arguments too long for the text arena");
		g_arg_cells[g_nargs++] = text_const(start);
	}
}

/* vm_run PC: run from PC until HALT; 0 when it finished, 1 after a runtime error */
static i64 coro_state(i64 t) { return g_heap[t + CO_STATE].i; }

/* coro_ready T: the paused coroutine T can go on now */
static int coro_ready(i64 t)
{
	const Cell *co = g_heap + t;
	switch (co[CO_STATE].i) {
	case CS_READY: return co[CO_STEP].i != g_step;
	case CS_SLEEPING: return co[CO_UNTIL].f <= g_coro_time;
	default: return 0;
	}
}

/* coro_unlist I: the coroutine at I in g_coros has ended: out of the list, which lets go of its count */
static o coro_unlist(i32 i)
{
	i64 t = g_coros[i];
	memmove(g_coros + i, g_coros + i + 1, (size_t)(g_ncoros - i - 1) * sizeof(i64));
	g_ncoros--;
	g_heap[t + CO_LISTED].i = 0;
	obj_release(t);
}

static f64 wall_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (f64)ts.tv_sec + (f64)ts.tv_nsec / 1e9;
}

static o wall_sleep(f64 secs)
{
	struct timespec ts;
	if (secs <= 0) return;
	ts.tv_sec = (time_t)secs;
	ts.tv_nsec = (long)((secs - (f64)ts.tv_sec) * 1e9);
	while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

/* main_wait: the main program runs the coroutines for what g_mwait says - one step, until none is left, or while it
 * sleeps - in passes over g_coros. 1: g_coros[g_mwait.last] goes on now (the caller switches to it; when it pauses or
 * ends, the main program is back at the same instruction, which calls this again); 0: done. Between passes the
 * coroutine time follows the real time, and when every coroutine sleeps the process sleeps until the first one wakes */
static int main_wait(void)
{
	for (;;) {
		f64 next = -1, now;
		i32 i;
		if (g_mwait.last >= 0) {
			if (coro_state(g_coros[g_mwait.last]) >= CS_DONE) {
				coro_unlist(g_mwait.last);
				if (g_mwait.at > g_mwait.last) g_mwait.at--;
			}
			g_mwait.last = -1;
		}
		if (g_mwait.kind == MW_RUN && !g_ncoros) return 0;
		if (g_mwait.kind == MW_SLEEP && g_coro_time >= g_mwait.until) return 0;
		while (g_mwait.at < g_ncoros && !coro_ready(g_coros[g_mwait.at])) {
			if (coro_state(g_coros[g_mwait.at]) == CS_DEAD) coro_unlist(g_mwait.at);
			else g_mwait.at++;
		}
		if (g_mwait.at < g_ncoros) {
			g_mwait.last = g_mwait.at++;
			return 1;
		}
		if (g_mwait.kind == MW_STEP) return 0; /* one pass */
		g_step++;
		g_mwait.at = 0;
		now = wall_time();
		g_coro_time += now - g_mwait.wall;
		g_mwait.wall = now;
		for (i = 0; i < g_ncoros; i++) {
			const Cell *co = g_heap + g_coros[i];
			if (coro_ready(g_coros[i])) { next = g_coro_time; break; }
			if (co[CO_STATE].i == CS_SLEEPING && (next < 0 || co[CO_UNTIL].f < next)) next = co[CO_UNTIL].f;
		}
		if (g_mwait.kind == MW_SLEEP && (next < 0 || g_mwait.until < next)) next = g_mwait.until;
		if (next < 0) return 0; /* none left to run */
		if (next > g_coro_time) {
			wall_sleep(next - g_coro_time);
			now = wall_time();
			g_coro_time += now - g_mwait.wall;
			g_mwait.wall = now;
			if (g_coro_time < next) g_coro_time = next; /* it slept at least that long */
		}
	}
}

/* main_wait_start KIND: the main program starts to wait */
static o main_wait_start(i32 kind)
{
	g_mwait.kind = kind;
	g_mwait.at = 0;
	g_mwait.last = -1;
	g_mwait.wall = wall_time();
	g_step++;
}

/* coro_new FN TYPE LINE: a coroutine that will run FN from its start, with its stack and frames; its parameters are
 * for the caller to copy in. -1 when the heap is full */
static i64 coro_new(const Fn *fn, Ty type, i32 line)
{
	i64 need = fn->nslots + fn->maxdepth, cap = CORO_STACK0, size = CO_RESULT + cells(g_tys[type].elem), t, st, frs, k;
	Cell *co;
	while (cap < need) cap *= 2;
	if ((t = heap_alloc(size)) < 0) return -1;
	if ((st = heap_alloc(cap)) < 0) { heap_free(t, size); return -1; }
	if ((frs = heap_alloc(CORO_FRAMES0)) < 0) { heap_free(st, cap); heap_free(t, size); return -1; }
	obj_born(t, KIND_CORO, (u64)type);
	co = g_heap + t;
	co[CO_STATE].i = CS_RUNNING;
	co[CO_STACK].i = st;
	co[CO_CAP].i = cap;
	co[CO_FRAMES].i = frs;
	co[CO_FCAP].i = CORO_FRAMES0;
	co[CO_SP].i = fn->nslots;
	co[CO_FP].i = 0;
	co[CO_NF].i = 1;
	co[CO_PC].i = fn->entry;
	co[CO_UNTIL].f = 0;
	co[CO_RESUMER].i = 0;
	co[CO_STEP].i = g_step;
	co[CO_FLOOR].i = 0;
	co[CO_LINE].i = line;
	co[CO_LISTED].i = 0;
	for (k = fn->pcells; k < fn->nslots; k++) g_heap[st + k].i = 0;
	g_heap[frs].i = FRAME(g_coro_end, 0); /* the function's return lands on CORO_END */
	return t;
}

/* coro_grow CELLS FRAMES: the running coroutine's stack made big enough for CELLS cells and FRAMES frames (doubling, up
 * to the main program's limits); 0 when it cannot be */
static int coro_grow(i64 cells_needed, i64 frames_needed)
{
	Cell *co = g_heap + g_cur;
	i64 cap = co[CO_CAP].i, fcap = co[CO_FCAP].i, at;
	if (cells_needed > VM_STACK || frames_needed > VM_FRAMES) return 0;
	if (cells_needed > cap) {
		while (cap < cells_needed) cap *= 2;
		if ((at = heap_alloc(cap)) < 0) return 0;
		memcpy(g_heap + at, g_heap + co[CO_STACK].i, (size_t)co[CO_CAP].i * sizeof(Cell));
		heap_free(co[CO_STACK].i, co[CO_CAP].i);
		co[CO_STACK].i = at;
		co[CO_CAP].i = cap;
	}
	if (frames_needed > fcap) {
		while (fcap < frames_needed) fcap *= 2;
		if ((at = heap_alloc(fcap)) < 0) return 0;
		memcpy(g_heap + at, g_heap + co[CO_FRAMES].i, (size_t)co[CO_FCAP].i * sizeof(Cell));
		heap_free(co[CO_FRAMES].i, co[CO_FCAP].i);
		co[CO_FRAMES].i = at;
		co[CO_FCAP].i = fcap;
	}
	return 1;
}

/* The registers of the running context out to where they are kept (g_main, or the coroutine), and another context's in */
#define SAVE_REGS(at_pc) do { \
	if (g_cur) { \
		Cell *tk_ = g_heap + g_cur; \
		tk_[CO_SP].i = sp; tk_[CO_FP].i = fp; tk_[CO_NF].i = nf; tk_[CO_PC].i = (at_pc); \
	} else { \
		g_main.sp = sp; g_main.fp = fp; g_main.nf = nf; g_main.pc = (at_pc); \
	} } while (0)
#define LOAD_REGS(t) do { \
	g_cur = (t); \
	if (g_cur) { \
		const Cell *tk_ = g_heap + g_cur; \
		s = g_heap + tk_[CO_STACK].i; fr = g_heap + tk_[CO_FRAMES].i; cap = tk_[CO_CAP].i; fcap = tk_[CO_FCAP].i; \
		sp = (i32)tk_[CO_SP].i; fp = (i32)tk_[CO_FP].i; nf = (i32)tk_[CO_NF].i; pc = (i32)tk_[CO_PC].i; \
	} else { \
		s = g_vstack; fr = g_vframes; cap = VM_STACK; fcap = VM_FRAMES; \
		sp = g_main.sp; fp = g_main.fp; nf = g_main.nf; pc = g_main.pc; \
	} } while (0)
/* the running coroutine's stack and frames, after coro_grow moved them */
#define LOAD_STACK() do { \
	const Cell *tk_ = g_heap + g_cur; \
	s = g_heap + tk_[CO_STACK].i; fr = g_heap + tk_[CO_FRAMES].i; cap = tk_[CO_CAP].i; fcap = tk_[CO_FCAP].i; \
	} while (0)
/* the running coroutine pauses (its state is set): back to whoever ran it; on its first pause it joins g_coros */
#define SUSPEND(at_pc) do { \
	i64 t_ = g_cur; \
	if (g_narenas != g_arena_floor) return vm_error(in, "a coroutine cannot pause inside an arena: the arena would end out of turn"); \
	SAVE_REGS(at_pc); \
	if (!g_heap[t_ + CO_LISTED].i) { \
		if (g_ncoros == MAX_COROS) return vm_error(in, "too many coroutines paused (MAX_COROS)"); \
		g_coros[g_ncoros++] = t_; \
		g_heap[t_ + CO_LISTED].i = 1; \
		obj_retain(t_); \
	} \
	g_arena_floor = (i32)g_heap[t_ + CO_FLOOR].i; \
	LOAD_REGS(g_heap[t_ + CO_RESUMER].i); } while (0)
/* the main program lets coroutine T go on; it comes back to this instruction */
#define RESUME(t) do { \
	i64 t_ = (t); \
	SAVE_REGS(pc - 1); \
	g_heap[t_ + CO_RESUMER].i = 0; \
	g_heap[t_ + CO_STATE].i = CS_RUNNING; \
	g_heap[t_ + CO_STEP].i = g_step; \
	g_heap[t_ + CO_FLOOR].i = g_arena_floor; \
	g_arena_floor = g_narenas; \
	LOAD_REGS(t_); } while (0)

/* Dispatch. With gcc and clang each instruction's code jumps straight to the next one's through a table of label
 * addresses (computed goto, as Lua does), so each instruction has a jump of its own for the branch predictor; with
 * another compiler, or GAEL_SWITCH defined, it is a plain switch in a loop. The cases are the same in both: CASE names
 * one, NEXT ends it. */
#if defined(__GNUC__) && !defined(GAEL_SWITCH)
#define VM_GOTO 1
#define CASE(op) case op: L_##op:
#define NEXT __extension__ ({ in = &g_code[pc++]; goto *labels[in->op]; })
#else
#define CASE(op) case op:
#define NEXT continue
#endif

/* the cases of the superinstructions: VM_BIN for the arithmetic ones, VM_JMP for the compare-and-jumps (ops.h) */
#define K_i(x) g_ints[x]
#define K_f(x) g_floats[x]
#define E_ADD_I(x, y) (i64)((u64)(x) + (u64)(y))
#define E_SUB_I(x, y) (i64)((u64)(x) - (u64)(y))
#define E_MUL_I(x, y) (i64)((u64)(x) * (u64)(y))
#define E_ADD_F(x, y) ((x) + (y))
#define E_SUB_F(x, y) ((x) - (y))
#define E_MUL_F(x, y) ((x) * (y))
#define E_DIV_F(x, y) ((x) / (y))
#define VM_BIN(op, F) \
	CASE(OP_##op##_LL) s[sp].F = E_##op(s[fp + in->a].F, s[fp + in->b].F); sp++; pc += 2; NEXT; \
	CASE(OP_##op##_LK) s[sp].F = E_##op(s[fp + in->a].F, K_##F(in->b)); sp++; pc += 2; NEXT; \
	CASE(OP_##op##_SL) s[sp - 1].F = E_##op(s[sp - 1].F, s[fp + in->a].F); pc++; NEXT; \
	CASE(OP_##op##_SK) s[sp - 1].F = E_##op(s[sp - 1].F, K_##F(in->a)); pc++; NEXT;
#define VM_JMP(op, F, R) \
	CASE(OP_J##op##_LL) if ((s[fp + in->a].F R s[fp + in->b].F) == in[1].b) pc = in[1].a; else pc += 3; NEXT; \
	CASE(OP_J##op##_LK) if ((s[fp + in->a].F R K_##F(in->b)) == in[1].b) pc = in[1].a; else pc += 3; NEXT; \
	CASE(OP_J##op##_SK) sp--; if ((s[sp].F R K_##F(in->a)) == in[1].b) pc = in[1].a; else pc += 2; NEXT; \
	CASE(OP_J##op##_SS) sp -= 2; if ((s[sp].F R s[sp + 1].F) == in->b) pc = in->a; else pc++; NEXT;

static int gpu_run(i32 k, const Cell *frame); /* gpu.c: 1 when kernel K ran on the GPU, 0 to run it here, -1 an error */
static int gfx_op(i32 op, const Cell *a, Cell *out, const char *path); /* gfx.c */
static char g_gpu_error[512];

static int vm_run(i32 pc)
{
#ifdef VM_GOTO
#define VM_LABEL(name, k) &&L_OP_##name,
	__extension__ static void *const labels[] = { GAEL_OPS(VM_LABEL) };
#undef VM_LABEL
#endif
	i32 sp = 0, fp = 0, nf = 0;
	i64 cap = VM_STACK, fcap = VM_FRAMES;
	Cell *s = g_vstack, *fr = g_vframes;
	const Ins *in = 0;
	g_narenas = 0; /* arenas a runtime error left open (REPL) stay used but take nothing new */
	g_arena_floor = 0;
	g_mwait.kind = 0;
	if (g_cur) { /* a runtime error stopped a coroutine (REPL): it and the coroutines that were running it are dead */
		i64 t = g_cur;
		while (t) {
			g_heap[t + CO_STATE].i = CS_DEAD;
			t = g_heap[t + CO_RESUMER].i;
		}
		g_cur = 0;
	}
	for (;;) {
		in = &g_code[pc++];
		switch ((Op)in->op) {
		CASE(OP_HALT) return 0;
		CASE(OP_ENTER)
			if (fp + in->a + in->b > cap) return vm_error(in, "stack overflow");
			while (sp < fp + in->a) s[sp++].i = 0;
			NEXT;
		CASE(OP_PUSH_I) s[sp++].i = g_ints[in->a]; NEXT;
		CASE(OP_PUSH_F) s[sp++].f = g_floats[in->a]; NEXT;
		CASE(OP_PUSH_S) s[sp++].i = g_strs[in->a].cell; NEXT;
		CASE(OP_PUSH_K) s[sp++].i = in->a; NEXT;
		CASE(OP_ZERO) memset(s + sp, 0, (size_t)in->a * sizeof(Cell)); sp += in->a; NEXT;
		CASE(OP_POP) sp--; NEXT;
		CASE(OP_POPN) sp -= in->a; NEXT;
		CASE(OP_LOAD_L) s[sp++] = s[fp + in->a]; NEXT;
		CASE(OP_STORE_L) s[fp + in->a] = s[--sp]; NEXT;
		CASE(OP_LOAD_G) s[sp++] = g_global_cells[in->a]; NEXT;
		CASE(OP_STORE_G) g_global_cells[in->a] = s[--sp]; NEXT;
		CASE(OP_LOADN_L) { /* a few cells: a loop, not a call of memcpy */
			const Cell *from = s + fp + in->a;
			i32 k;
			for (k = 0; k < in->b; k++) s[sp + k] = from[k];
			sp += in->b;
			NEXT;
		}
		CASE(OP_STOREN_L) {
			Cell *to = s + fp + in->a;
			i32 k;
			sp -= in->b;
			for (k = 0; k < in->b; k++) to[k] = s[sp + k];
			NEXT;
		}
		CASE(OP_LOADN_G) memcpy(s + sp, g_global_cells + in->a, (size_t)in->b * sizeof(Cell)); sp += in->b; NEXT;
		CASE(OP_STOREN_G) sp -= in->b; memcpy(g_global_cells + in->a, s + sp, (size_t)in->b * sizeof(Cell)); NEXT;
		CASE(OP_RETAIN)
			if (ONE_HANDLE(in->a)) obj_retain(s[sp - 1].i);
			else refs(s + sp - cells((Ty)in->a), 1, (Ty)in->a, 1);
			NEXT;
		CASE(OP_DROP)
			sp -= cells((Ty)in->a);
			if (ONE_HANDLE(in->a)) obj_release(s[sp].i);
			else refs(s + sp, 1, (Ty)in->a, 0);
			NEXT;
		CASE(OP_RELEASE_L)
			if (ONE_HANDLE(in->b)) obj_release(s[fp + in->a].i);
			else refs(s + fp + in->a, 1, (Ty)in->b, 0);
			NEXT;
		CASE(OP_RELEASE_G)
			if (ONE_HANDLE(in->b)) obj_release(g_global_cells[in->a].i);
			else refs(g_global_cells + in->a, 1, (Ty)in->b, 0);
			NEXT;

		CASE(OP_ADD_I) sp--; s[sp - 1].i = (i64)((u64)s[sp - 1].i + (u64)s[sp].i); NEXT;
		CASE(OP_SUB_I) sp--; s[sp - 1].i = (i64)((u64)s[sp - 1].i - (u64)s[sp].i); NEXT;
		CASE(OP_MUL_I) sp--; s[sp - 1].i = (i64)((u64)s[sp - 1].i * (u64)s[sp].i); NEXT;
		CASE(OP_DIV_I)
		CASE(OP_MOD_I)
			sp--;
			if (s[sp].i == 0) return vm_error(in, "division by zero");
			if (s[sp].i == -1) { /* INT_MIN / -1 overflows; -x and 0 are what wrapping gives */
				s[sp - 1].i = in->op == OP_DIV_I ? (i64)(0 - (u64)s[sp - 1].i) : 0;
				NEXT;
			}
			s[sp - 1].i = in->op == OP_DIV_I ? s[sp - 1].i / s[sp].i : s[sp - 1].i % s[sp].i;
			NEXT;
		CASE(OP_NEG_I) s[sp - 1].i = (i64)(0 - (u64)s[sp - 1].i); NEXT;
		CASE(OP_DIV_U)
		CASE(OP_MOD_U)
			sp--;
			if (s[sp].i == 0) return vm_error(in, "division by zero");
			s[sp - 1].i = (i64)(in->op == OP_DIV_U ? (u64)s[sp - 1].i / (u64)s[sp].i : (u64)s[sp - 1].i % (u64)s[sp].i);
			NEXT;
		CASE(OP_BAND) sp--; s[sp - 1].i = (i64)((u64)s[sp - 1].i & (u64)s[sp].i); NEXT;
		CASE(OP_BOR) sp--; s[sp - 1].i = (i64)((u64)s[sp - 1].i | (u64)s[sp].i); NEXT;
		CASE(OP_BXOR) sp--; s[sp - 1].i = (i64)((u64)s[sp - 1].i ^ (u64)s[sp].i); NEXT;
		CASE(OP_BNOT) s[sp - 1].i = (i64)~(u64)s[sp - 1].i; NEXT;
		CASE(OP_SHL)
		CASE(OP_SHR) { /* a: the int type */
			Ty t = (Ty)in->a;
			i64 n = s[sp - 1].i, bits = int_bits(t);
			u64 v = (u64)s[sp - 2].i;
			if (n < 0 || n >= bits) {
				char msg[96];
				sprintf(msg, "a shift by %ld does not fit a %ld-bit int", (long)n, (long)bits);
				return vm_error(in, msg);
			}
			sp--;
			if (in->op == OP_SHR && !is_unsigned(t) && s[sp - 1].i < 0) v = ~(~v >> n); /* the sign comes in from the left */
			else v = in->op == OP_SHR ? v >> n : v << n;
			s[sp - 1].i = (i64)v;
			if (bits < 64) { /* back into the type, as WRAP does */
				u64 mask = ((u64)1 << bits) - 1, sign = (mask >> 1) + 1;
				v &= mask;
				s[sp - 1].i = is_unsigned(t) || !(v & sign) ? (i64)v : -(i64)(mask - v) - 1;
			}
			NEXT;
		}
		CASE(OP_WRAP) { /* keep the low bits; a signed type gets its sign back */
			u64 v = (u64)s[sp - 1].i, mask = ((u64)1 << int_bits((Ty)in->a)) - 1, sign = (mask >> 1) + 1;
			v &= mask;
			s[sp - 1].i = is_unsigned((Ty)in->a) || !(v & sign) ? (i64)v : -(i64)(mask - v) - 1;
			NEXT;
		}
		CASE(OP_F32) s[sp - 1].f = to_f32(s[sp - 1].f); NEXT;
		CASE(OP_ADD_F) sp--; s[sp - 1].f += s[sp].f; NEXT;
		CASE(OP_SUB_F) sp--; s[sp - 1].f -= s[sp].f; NEXT;
		CASE(OP_MUL_F) sp--; s[sp - 1].f *= s[sp].f; NEXT;
		CASE(OP_DIV_F) sp--; s[sp - 1].f /= s[sp].f; NEXT;
		CASE(OP_NEG_F) s[sp - 1].f = -s[sp - 1].f; NEXT;
		CASE(OP_MATH) {
			f64 x = s[sp - 1].f;
			switch (in->a) {
			case 0: x = sqrt(x); break;
			case 1: x = sin(x); break;
			case 2: x = cos(x); break;
			case 3: x = tan(x); break;
			case 4: x = asin(x); break;
			case 5: x = acos(x); break;
			case 6: x = atan(x); break;
			case 7: x = exp(x); break;
			case 8: x = log(x); break;
			case 9: x = floor(x); break;
			default: x = ceil(x); break;
			}
			s[sp - 1].f = x;
			NEXT;
		}
		CASE(OP_MATH2) sp--; s[sp - 1].f = in->a ? pow(s[sp - 1].f, s[sp].f) : atan2(s[sp - 1].f, s[sp].f); NEXT;

		CASE(OP_LT_I) sp--; s[sp - 1].i = s[sp - 1].i < s[sp].i; NEXT;
		CASE(OP_LE_I) sp--; s[sp - 1].i = s[sp - 1].i <= s[sp].i; NEXT;
		CASE(OP_GT_I) sp--; s[sp - 1].i = s[sp - 1].i > s[sp].i; NEXT;
		CASE(OP_GE_I) sp--; s[sp - 1].i = s[sp - 1].i >= s[sp].i; NEXT;
		CASE(OP_EQ_I) sp--; s[sp - 1].i = s[sp - 1].i == s[sp].i; NEXT;
		CASE(OP_NE_I) sp--; s[sp - 1].i = s[sp - 1].i != s[sp].i; NEXT;
		CASE(OP_LT_F) sp--; s[sp - 1].i = s[sp - 1].f < s[sp].f; NEXT;
		CASE(OP_LE_F) sp--; s[sp - 1].i = s[sp - 1].f <= s[sp].f; NEXT;
		CASE(OP_GT_F) sp--; s[sp - 1].i = s[sp - 1].f > s[sp].f; NEXT;
		CASE(OP_GE_F) sp--; s[sp - 1].i = s[sp - 1].f >= s[sp].f; NEXT;
		CASE(OP_LT_U) sp--; s[sp - 1].i = (u64)s[sp - 1].i < (u64)s[sp].i; NEXT;
		CASE(OP_LE_U) sp--; s[sp - 1].i = (u64)s[sp - 1].i <= (u64)s[sp].i; NEXT;
		CASE(OP_GT_U) sp--; s[sp - 1].i = (u64)s[sp - 1].i > (u64)s[sp].i; NEXT;
		CASE(OP_GE_U) sp--; s[sp - 1].i = (u64)s[sp - 1].i >= (u64)s[sp].i; NEXT;
		CASE(OP_EQ_F) sp--; s[sp - 1].i = s[sp - 1].f == s[sp].f; NEXT;
		CASE(OP_NE_F) sp--; s[sp - 1].i = s[sp - 1].f != s[sp].f; NEXT;
		CASE(OP_EQ_S)
		CASE(OP_NE_S) { /* a: bit 0 the first string is borrowed, bit 1 the second */
			i64 a = s[sp - 2].i, b = s[sp - 1].i;
			int eq = str_eq(a, b);
			if (!(in->a & 1)) obj_release(a);
			if (!(in->a & 2)) obj_release(b);
			sp--;
			s[sp - 1].i = in->op == OP_EQ_S ? eq : !eq;
			NEXT;
		}
		CASE(OP_NOT) s[sp - 1].i = !s[sp - 1].i; NEXT;

		CASE(OP_JMP) pc = in->a; NEXT;
		CASE(OP_JF) if (!s[--sp].i) pc = in->a; NEXT;
		CASE(OP_JT) if (s[--sp].i) pc = in->a; NEXT;

		CASE(OP_I2F) s[sp - 1].f = (f64)s[sp - 1].i; NEXT;
		CASE(OP_U2F) s[sp - 1].f = (f64)(u64)s[sp - 1].i; NEXT;
		CASE(OP_F2I) { /* a: the int type */
			f64 v = s[sp - 1].f, lo, hi; /* it fits when lo < v < hi; both are false for nan */
			Ty t = (Ty)in->a;
			i32 bits = int_bits(t);
			if (is_unsigned(t)) lo = -1.0, hi = bits == 64 ? 18446744073709551616.0 : (f64)((u64)1 << bits);
			else lo = bits == 64 ? -9223372036854777856.0 : -(f64)((u64)1 << (bits - 1)) - 1.0, hi = (f64)((u64)1 << (bits - 1));
			if (!(v > lo && v < hi)) {
				char msg[160];
				sprintf(msg, "%s of a float that does not fit in it (%s)", tn(t), int_range(t));
				return vm_error(in, msg);
			}
			s[sp - 1].i = t == T_U64 && v >= 9223372036854775808.0 ? (i64)(u64)v : (i64)v;
			NEXT;
		}
		CASE(OP_LEN_S) {
			i64 v = s[sp - 1].i;
			s[sp - 1].i = (i64)STR_LEN(v);
			if (!in->a) obj_release(v);
			NEXT;
		}

		CASE(OP_CALL)
		CASE(OP_CALL_V) {
			const Fn *fn;
			i32 nfp;
			i64 fv = 0;
			if (in->op == OP_CALL_V) {
				fv = s[--sp].i;
				if (!fv) return vm_error(in, UNSET_FN);
				fn = &g_fns[fv < 0 ? -fv - 1 : g_heap[fv + CL_FN].i];
			} else {
				fn = &g_fns[in->a];
			}
			nfp = sp + fn->env - fn->pcells;
			if (nf == fcap || nfp + fn->nslots + fn->maxdepth > cap) {
				if (!g_cur || !coro_grow(nfp + fn->nslots + fn->maxdepth, nf + 1))
					return vm_error(in, "stack overflow (recursion too deep?)");
				LOAD_STACK(); /* the coroutine's stack moved */
			}
			if (fn->env) { /* its closure (or 0) goes under the arguments, into slot 0; the call owns it now */
				memmove(s + nfp + 1, s + nfp, (size_t)(sp - nfp) * sizeof(Cell));
				s[nfp].i = fv > 0 ? fv : 0;
				sp++;
			}
			fr[nf].i = FRAME(pc, fp);
			nf++;
			fp = nfp;
			while (sp < fp + fn->nslots) s[sp++].i = 0;
			pc = fn->entry;
			NEXT;
		}
		CASE(OP_TAILCALL)
		CASE(OP_TAILCALL_V) { /* the arguments move down to this frame's start, and the callee runs in it */
			const Fn *fn;
			i64 fv = 0;
			if (in->op == OP_TAILCALL_V) {
				fv = s[--sp].i;
				if (!fv) return vm_error(in, UNSET_FN);
				fn = &g_fns[fv < 0 ? -fv - 1 : g_heap[fv + CL_FN].i];
			} else {
				fn = &g_fns[in->a];
			}
			if (fp + fn->nslots + fn->maxdepth > cap) {
				if (!g_cur || !coro_grow(fp + fn->nslots + fn->maxdepth, nf)) return vm_error(in, "stack overflow");
				LOAD_STACK();
			}
			if (fn->env) {
				i32 at = sp + 1 - fn->pcells;
				memmove(s + at + 1, s + at, (size_t)(sp - at) * sizeof(Cell));
				s[at].i = fv > 0 ? fv : 0;
				sp++;
			}
			memmove(s + fp, s + sp - fn->pcells, (size_t)fn->pcells * sizeof(Cell));
			sp = fp + fn->pcells;
			while (sp < fp + fn->nslots) s[sp++].i = 0;
			pc = fn->entry;
			NEXT;
		}
		CASE(OP_RET_V) {
			Cell r = s[sp - 1];
			sp = fp;
			nf--;
			fp = FRAME_FP(fr[nf]);
			pc = FRAME_PC(fr[nf]);
			s[sp++] = r;
			NEXT;
		}
		CASE(OP_RET_N)
			memmove(s + fp, s + sp - in->a, (size_t)in->a * sizeof(Cell));
			sp = fp + in->a;
			nf--;
			fp = FRAME_FP(fr[nf]);
			pc = FRAME_PC(fr[nf]);
			NEXT;
		CASE(OP_RET_U)
			sp = fp;
			nf--;
			fp = FRAME_FP(fr[nf]);
			pc = FRAME_PC(fr[nf]);
			NEXT;

		CASE(OP_PRINT)
		CASE(OP_PRINTLN)
			sp -= g_fmts[in->a].cells;
			if (in->b) fflush(stdout); /* stdout's text first when both go to one terminal */
			g_sink = in->b ? stderr : stdout;
			print_fmt(&g_fmts[in->a], s + sp);
			if (in->op == OP_PRINTLN) putc('\n', g_sink);
			release_args(&g_fmts[in->a], s + sp);
			NEXT;
		CASE(OP_FORMAT) {
			size_t start = g_text.used;
			i64 h;
			sp -= g_fmts[in->a].cells;
			g_sink = 0;
			g_sink_full = !text_room();
			print_fmt(&g_fmts[in->a], s + sp);
			if (g_sink_full) return vm_error(in, TEXT_FULL);
			release_args(&g_fmts[in->a], s + sp);
			if ((h = text_take(start)) < 0) return vm_error(in, HEAP_FULL);
			s[sp++].i = h;
			NEXT;
		}
		CASE(OP_SH) {
			i64 cmd = s[sp - 1].i, res;
			if (!run_cmd(c_str(cmd), &res)) return vm_error(in, g_fail);
			obj_release(cmd);
			s[sp - 1].i = res;
			NEXT;
		}
		CASE(OP_SHPIPE) {
			i64 res, k;
			sp -= in->a;
			if (!run_pipe(s + sp, in->a, in->b, &res)) return vm_error(in, g_fail);
			for (k = 0; k < in->a; k++) obj_release(s[sp + k].i);
			s[sp++].i = res;
			NEXT;
		}
		CASE(OP_PID) s[sp++].i = (i64)getpid(); NEXT;
		CASE(OP_ENV) {
			i64 name = s[sp - 1].i, h;
			const char *c = c_str(name), *val;
			if (!c) return vm_error(in, TEXT_FULL);
			val = getenv(c);
			if (!val) val = "";
			if ((h = str_new(val, strlen(val))) < 0) return vm_error(in, HEAP_FULL);
			obj_release(name);
			s[sp - 1].i = h;
			NEXT;
		}
		CASE(OP_SYSTEM)
		CASE(OP_RUN) {
			i64 cmd = s[sp - 1].i;
			const char *c = c_str(cmd);
			int st;
			if (!c) return vm_error(in, TEXT_FULL);
			fflush(stdout);
			st = system(c);
			obj_release(cmd);
			st = st == -1 ? -1 : (st >> 8) & 0xff; /* the exit status as the shell reports it (Linux) */
			s[sp - 1].i = in->op == OP_RUN ? st == 0 : st;
			NEXT;
		}
		CASE(OP_PARSE_I) {
			i64 v = 0, str = s[sp - 1].i;
			int ok = parse_int(str, &v);
			obj_release(str);
			s[sp - 1].i = ok;
			s[sp++].i = v;
			NEXT;
		}
		CASE(OP_PARSE_F) {
			i64 str = s[sp - 1].i;
			f64 v = 0;
			int ok = parse_float(str, &v);
			obj_release(str);
			s[sp - 1].i = ok;
			s[sp++].f = ok ? v : 0;
			NEXT;
		}
		CASE(OP_SUBSTR) {
			i64 v = s[sp - 3].i, from = s[sp - 2].i, to = s[sp - 1].i, h;
			if (from < 0 || from > to || to > STR_LEN(v)) return range_error(in, from, to, STR_LEN(v));
			if ((h = str_piece(v, from, to - from)) == -1) return vm_error(in, HEAP_FULL);
			obj_release(v);
			sp -= 2;
			s[sp - 1].i = h;
			NEXT;
		}
		CASE(OP_BYTE) {
			i64 v = s[sp - 2].i, i = s[sp - 1].i;
			if (i < 0 || i >= STR_LEN(v)) {
				char msg[96];
				sprintf(msg, "byte %ld is outside a string of %ld bytes", (long)i, (long)STR_LEN(v));
				return vm_error(in, msg);
			}
			sp--;
			s[sp - 1].i = (u8)STR_P(v)[i];
			if (!in->a) obj_release(v);
			NEXT;
		}
		CASE(OP_FIND) { /* the two strings become the option's two cells */
			i64 a = s[sp - 2].i, b = s[sp - 1].i, at = str_find(a, b, 0);
			obj_release(a);
			obj_release(b);
			s[sp - 2].i = at >= 0;
			s[sp - 1].i = at >= 0 ? at : 0;
			NEXT;
		}
		CASE(OP_SPLIT) {
			i64 v = s[sp - 2].i, sep = s[sp - 1].i, n = 1, at, start = 0, h, piece;
			Cell *data;
			if (!STR_LEN(sep)) return vm_error(in, "split: the separator is empty");
			for (at = str_find(v, sep, 0); at >= 0; at = str_find(v, sep, at + STR_LEN(sep))) n++;
			if ((h = vec_new(g_vec_str, n, 0)) < 0) return vm_error(in, HEAP_FULL);
			data = g_heap + g_heap[h + VH_DATA].i;
			for (n = 0;; start = at + STR_LEN(sep)) {
				at = str_find(v, sep, start);
				if ((piece = str_piece(v, start, (at >= 0 ? at : STR_LEN(v)) - start)) == -1) return vm_error(in, HEAP_FULL);
				data[n++].i = piece;
				if (at < 0) break;
			}
			obj_release(v);
			obj_release(sep);
			sp--;
			s[sp - 1].i = h;
			NEXT;
		}
		CASE(OP_JOIN) {
			i64 v = s[sp - 2].i, sep = s[sp - 1].i, k, h;
			const Cell *hd = g_heap + v;
			size_t start = g_text.used;
			int ok = text_room();
			MADE(v);
			for (k = 0; ok && k < hd[VH_LEN].i; k++) {
				i64 e = vec_cell0(hd, k)->i;
				if (k) ok = text_put(STR_P(sep), STR_LEN(sep));
				if (ok) ok = text_put(STR_P(e), STR_LEN(e));
			}
			if (!ok) {
				g_text.used = start;
				return vm_error(in, TEXT_FULL);
			}
			if ((h = text_take(start)) < 0) return vm_error(in, HEAP_FULL);
			obj_release(v);
			obj_release(sep);
			sp--;
			s[sp - 1].i = h;
			NEXT;
		}
		CASE(OP_CHAR)
			if (s[sp - 1].i < 0 || s[sp - 1].i > 255) {
				char msg[64];
				sprintf(msg, "char takes 0 to 255, this is %ld", (long)s[sp - 1].i);
				return vm_error(in, msg);
			}
			s[sp - 1].i = str_cell((size_t)(2 * s[sp - 1].i), 1);
			NEXT;
		CASE(OP_TRIM) {
			i64 v = s[sp - 1].i, h;
			const char *p;
			u32 n;
			trimmed(v, &p, &n);
			if ((h = str_piece(v, p - STR_P(v), n)) == -1) return vm_error(in, HEAP_FULL);
			obj_release(v);
			s[sp - 1].i = h;
			NEXT;
		}
		CASE(OP_READ_FILE) { /* the path's cell becomes the option's two: tag, string */
			i64 pathv = s[sp - 1].i, v = 0;
			const char *path = c_str(pathv);
			FILE *file = path ? fopen(path, "rb") : 0;
			if (!path) return vm_error(in, TEXT_FULL);
			if (file) {
				int ok = read_into(file, &v);
				fclose(file);
				if (!ok) return vm_error(in, g_fail);
			}
			obj_release(pathv);
			s[sp - 1].i = file != 0;
			s[sp++].i = v;
			NEXT;
		}
		CASE(OP_WRITE_FILE) { /* a: append, b: the text is below the path */
			i64 text = s[sp - (in->b ? 2 : 1)].i, pathv = s[sp - (in->b ? 1 : 2)].i;
			const char *path = c_str(pathv);
			FILE *file = path ? fopen(path, in->a ? "ab" : "wb") : 0;
			int ok = file && fwrite(STR_P(text), 1, STR_LEN(text), file) == STR_LEN(text);
			if (file && fclose(file)) ok = 0;
			if (!path) return vm_error(in, TEXT_FULL);
			obj_release(text);
			obj_release(pathv);
			sp--;
			s[sp - 1].i = ok;
			NEXT;
		}
		CASE(OP_READ_LINE) {
			size_t start = g_text.used;
			int c = EOF, any = 0;
			i64 h;
			fflush(stdout); /* a prompt printed before it shows up first */
			while ((c = getchar()) != EOF) {
				char ch = (char)c;
				any = 1;
				if (ch == '\n') break;
				if (!text_put(&ch, 1)) {
					g_text.used = start;
					return vm_error(in, TEXT_FULL);
				}
			}
			if (g_text.used > start && g_text.base[g_text.used - 1] == '\r') g_text.used--;
			if (!any) {
				g_text.used = start;
				s[sp++].i = 0;
				s[sp++].i = 0;
				NEXT;
			}
			if ((h = text_take(start)) < 0) return vm_error(in, HEAP_FULL);
			s[sp++].i = 1;
			s[sp++].i = h;
			NEXT;
		}
		CASE(OP_GETENV) {
			i64 name = s[sp - 1].i, h = 0;
			const char *c = c_str(name), *val;
			if (!c) return vm_error(in, TEXT_FULL);
			val = getenv(c);
			if (val && (h = str_new(val, strlen(val))) < 0) return vm_error(in, HEAP_FULL);
			obj_release(name);
			s[sp - 1].i = val != 0;
			s[sp++].i = h;
			NEXT;
		}
		CASE(OP_ARGS) {
			i64 h = vec_new(g_vec_str, g_nargs, 0), k;
			if (h < 0) return vm_error(in, HEAP_FULL);
			for (k = 0; k < g_nargs; k++) vec_cell0(g_heap + h, k)->i = g_arg_cells[k];
			s[sp++].i = h;
			NEXT;
		}
		CASE(OP_EXIT)
			fflush(stdout);
			exit((int)(s[sp - 1].i & 0xff));
		CASE(OP_INC_L) s[fp + in->a].i = (i64)((u64)s[fp + in->a].i + 1); NEXT;
		CASE(OP_VEC_NEW) { /* a: elements on the stack, b: the vec type */
			i64 h;
			sp -= in->a * cells(g_tys[in->b].elem);
			if ((h = vec_new((Ty)in->b, in->a, s + sp)) < 0) return vm_error(in, HEAP_FULL);
			s[sp++].i = h;
			NEXT;
		}
		CASE(OP_VEC_LEN) {
			i64 v = s[sp - 1].i;
			MADE(v);
			s[sp - 1].i = g_heap[v + VH_LEN].i;
			if (!in->a) obj_release(v);
			NEXT;
		}
		CASE(OP_VEC_PUSH)
			sp -= in->a + 1;
			MADE(s[sp].i);
			if (!vec_push(s[sp].i, s + sp + 1)) return vm_error(in, HEAP_FULL);
			if (!in->b) obj_release(s[sp].i);
			NEXT;
		CASE(OP_VEC_GET) { /* a copy of the element: the vec keeps its own */
			i64 v = s[sp - 2].i, i = s[sp - 1].i;
			const Cell *hd = g_heap + v;
			Ty et;
			MADE(v);
			if (i < 0 || i >= hd[VH_LEN].i) return index_error(in, i, hd[VH_LEN].i);
			sp -= 2;
			et = vec_elem(v);
			if (!g_tys[et].bits) { /* no bit columns: cell C at C * cap */
				const Cell *d = g_heap + hd[VH_DATA].i + i;
				i64 c, cap = hd[VH_CAP].i;
				for (c = 0; c < in->a; c++) s[sp + c] = d[c * cap];
			} else if (in->a == 1) { /* (vec bool): one bit */
				s[sp].i = (i64)((u64)g_heap[hd[VH_DATA].i + (i >> 6)].i >> (i & 63) & 1);
			} else {
				vec_load(hd, et, i, s + sp);
			}
			if (g_tys[et].nleaf) refs(s + sp, 1, et, 1);
			sp += in->a;
			if (!in->b) obj_release(v);
			NEXT;
		}
		CASE(OP_VEC_PUT) {
			const Cell *hd;
			i64 i, v;
			Ty et;
			sp -= in->a + 2;
			v = s[sp].i;
			hd = g_heap + v;
			i = s[sp + 1].i;
			MADE(v);
			if (i < 0 || i >= hd[VH_LEN].i) return index_error(in, i, hd[VH_LEN].i);
			et = vec_elem(v);
			if (g_tys[et].nleaf) cols_refs(hd[VH_DATA].i, et, hd[VH_CAP].i, i, 0); /* the element it replaces */
			if (!g_tys[et].bits) {
				Cell *d = g_heap + hd[VH_DATA].i + i;
				i64 c, cap = hd[VH_CAP].i;
				for (c = 0; c < in->a; c++) d[c * cap] = s[sp + 2 + c];
			} else if (in->a == 1) { /* (vec bool): one bit */
				Cell *w = g_heap + hd[VH_DATA].i + (i >> 6);
				u64 bit = (u64)1 << (i & 63);
				w->i = (i64)(s[sp + 2].i ? (u64)w->i | bit : (u64)w->i & ~bit);
			} else {
				vec_store(hd, et, i, s + sp + 2);
			}
			if (!in->b) obj_release(v);
			NEXT;
		}
		CASE(OP_VEC_POP) { /* the last element moves out */
			i64 v = s[sp - 1].i;
			Cell *hd = g_heap + v;
			MADE(v);
			if (!hd[VH_LEN].i) { /* none */
				s[sp - 1].i = 0;
				memset(s + sp, 0, (size_t)in->a * sizeof(Cell));
			} else {
				hd[VH_LEN].i--;
				s[sp - 1].i = 1;
				vec_load(hd, vec_elem(v), hd[VH_LEN].i, s + sp);
			}
			sp += in->a;
			if (!in->b) obj_release(v);
			NEXT;
		}
		CASE(OP_MAP_NEW) { /* a: entries on the stack, b: the map type */
			i64 vsize = cells(g_tys[in->b].elem), k, m;
			sp -= in->a * (1 + vsize);
			if ((m = map_new((Ty)in->b)) < 0) return vm_error(in, HEAP_FULL);
			for (k = 0; k < in->a; k++) {
				const Cell *e = s + sp + k * (1 + vsize);
				if (!map_insert(m, e[0].i, e + 1)) return vm_error(in, HEAP_FULL);
			}
			s[sp++].i = m;
			NEXT;
		}
		CASE(OP_MAP_INSERT)
			sp -= in->a + 2;
			MADE(s[sp].i);
			if (!map_insert(s[sp].i, s[sp + 1].i, s + sp + 2)) return vm_error(in, HEAP_FULL);
			if (!in->b) obj_release(s[sp].i);
			NEXT;
		CASE(OP_MAP_LOOKUP)
		CASE(OP_MAP_REMOVE) { /* the map and the key become the option: tag, value */
			i64 m = s[sp - 2].i, key = s[sp - 1].i, slot, e, k;
			int strkeys;
			u64 h;
			MADE(m);
			strkeys = map_strkeys(m);
			e = map_find(m, key, &slot, &h);
			sp -= 2;
			s[sp].i = e >= 0;
			if (e >= 0) {
				Cell *ent = g_heap + g_heap[m + MH_ENT].i + e * (2 + in->a);
				for (k = 0; k < in->a; k++) s[sp + 1 + k] = ent[2 + k];
				if (in->op == OP_MAP_LOOKUP) {
					if (MAP_VREFS(g_heap + m)) refs(s + sp + 1, 1, g_tys[obj_type(m)].elem, 1); /* a copy: the map keeps its own */
				} else { /* the value moves out, the entry dies with its key */
					ent[0].i = 0;
					g_heap[m + MH_COUNT].i--;
					if (strkeys) obj_release(ent[1].i);
				}
			} else {
				for (k = 0; k < in->a; k++) s[sp + 1 + k].i = 0;
			}
			if (strkeys && !(in->b & 2)) obj_release(key);
			if (!(in->b & 1)) obj_release(m);
			sp += 1 + in->a;
			NEXT;
		}
		CASE(OP_MAP_HAS) {
			i64 m = s[sp - 2].i, key = s[sp - 1].i, slot, found;
			u64 h;
			MADE(m);
			found = map_find(m, key, &slot, &h) >= 0;
			if (map_strkeys(m) && !(in->a & 2)) obj_release(key);
			if (!(in->a & 1)) obj_release(m);
			sp--;
			s[sp - 1].i = found;
			NEXT;
		}
		CASE(OP_MAP_LEN) {
			i64 m = s[sp - 1].i;
			MADE(m);
			s[sp - 1].i = g_heap[m + MH_COUNT].i;
			if (!in->a) obj_release(m);
			NEXT;
		}
		CASE(OP_MAP_KEYS)
		CASE(OP_MAP_VALUES) { /* a: the vec type made; its elements are copies */
			i64 m = s[sp - 1].i, e, k = 0, v, esize = 2 + map_vsize(m);
			MADE(m);
			if ((v = vec_new((Ty)in->a, g_heap[m + MH_COUNT].i, 0)) < 0) return vm_error(in, HEAP_FULL);
			for (e = 0; e < g_heap[m + MH_USED].i; e++) {
				const Cell *ent = g_heap + g_heap[m + MH_ENT].i + e * esize;
				if (!ent[0].i) continue;
				refs(in->op == OP_MAP_KEYS ? ent + 1 : ent + 2, 1, g_tys[in->a].elem, 1);
				vec_store(g_heap + v, g_tys[in->a].elem, k++, in->op == OP_MAP_KEYS ? ent + 1 : ent + 2);
			}
			if (!in->b) obj_release(m);
			s[sp - 1].i = v;
			NEXT;
		}
		CASE(OP_POOL_NEW) { /* b: the pool type */
			i64 cap = s[sp - 1].i, h;
			if (cap < 1 || cap > 0x7fffffff) {
				char msg[96];
				sprintf(msg, "a pool holds 1 to 2147483647 elements, not %ld", (long)cap);
				return vm_error(in, msg);
			}
			if ((h = pool_new((Ty)in->b, cap)) < 0) return vm_error(in, HEAP_FULL);
			s[sp - 1].i = h;
			NEXT;
		}
		CASE(OP_POOL_SPAWN) { /* the element moves into the first free slot */
			Cell *hd, *gens;
			i64 p, i, cap, h;
			sp -= in->a + 1;
			p = s[sp].i;
			MADE(p);
			hd = g_heap + p;
			cap = hd[PH_CAP].i;
			if ((i = hd[PH_FREE].i) < 0) {
				char msg[96];
				sprintf(msg, "the pool is full (%ld elements)", (long)cap);
				return vm_error(in, msg);
			}
			gens = g_heap + hd[PH_GENS].i;
			hd[PH_FREE].i = gens[cap + i].i;
			gens[i].i = (gens[i].i + 1) & 0xffffffff; /* odd: it holds an element */
			hd[PH_COUNT].i++;
			cols_store(hd[PH_DATA].i, g_tys[obj_type(p)].elem, cap, i, s + sp + 1);
			h = handle_of(hd, i);
			if (!in->b) obj_release(p);
			s[sp++].i = h;
			NEXT;
		}
		CASE(OP_POOL_GET) { /* a copy of the element: the pool keeps its own */
			i64 p = s[sp - 2].i, i;
			const Cell *hd = g_heap + p;
			MADE(p);
			if ((i = pool_slot(hd, s[sp - 1].i)) < 0) return vm_error(in, STALE);
			sp -= 2;
			cols_load(hd[PH_DATA].i, g_tys[obj_type(p)].elem, hd[PH_CAP].i, i, s + sp);
			refs(s + sp, 1, g_tys[obj_type(p)].elem, 1);
			sp += in->a;
			if (!in->b) obj_release(p);
			NEXT;
		}
		CASE(OP_POOL_PUT) {
			const Cell *hd;
			i64 p, i;
			sp -= in->a + 2;
			p = s[sp].i;
			MADE(p);
			hd = g_heap + p;
			if ((i = pool_slot(hd, s[sp + 1].i)) < 0) return vm_error(in, STALE);
			cols_refs(hd[PH_DATA].i, g_tys[obj_type(p)].elem, hd[PH_CAP].i, i, 0); /* the element it replaces */
			cols_store(hd[PH_DATA].i, g_tys[obj_type(p)].elem, hd[PH_CAP].i, i, s + sp + 2);
			if (!in->b) obj_release(p);
			NEXT;
		}
		CASE(OP_POOL_LOOKUP)
		CASE(OP_POOL_REMOVE) { /* the pool and the handle become the option: tag, element */
			i64 p = s[sp - 2].i, i, c;
			Cell *hd = g_heap + p;
			MADE(p);
			i = pool_slot(hd, s[sp - 1].i);
			sp -= 2;
			s[sp].i = i >= 0;
			if (i >= 0) cols_load(hd[PH_DATA].i, g_tys[obj_type(p)].elem, hd[PH_CAP].i, i, s + sp + 1);
			else for (c = 0; c < in->a; c++) s[sp + 1 + c].i = 0;
			if (i >= 0 && in->op == OP_POOL_LOOKUP) {
				refs(s + sp + 1, 1, g_tys[obj_type(p)].elem, 1); /* a copy: the pool keeps its own */
			} else if (i >= 0) { /* the element moves out, the slot goes back on the free list */
				Cell *gens = g_heap + hd[PH_GENS].i;
				gens[i].i = (gens[i].i + 1) & 0xffffffff; /* even: free */
				gens[hd[PH_CAP].i + i].i = hd[PH_FREE].i;
				hd[PH_FREE].i = i;
				hd[PH_COUNT].i--;
			}
			sp += 1 + in->a;
			if (!(in->b & 1)) obj_release(p);
			NEXT;
		}
		CASE(OP_POOL_HAS) {
			i64 p = s[sp - 2].i, found;
			MADE(p);
			found = pool_slot(g_heap + p, s[sp - 1].i) >= 0;
			if (!(in->a & 1)) obj_release(p);
			sp--;
			s[sp - 1].i = found;
			NEXT;
		}
		CASE(OP_POOL_LEN) {
			i64 p = s[sp - 1].i;
			MADE(p);
			s[sp - 1].i = g_heap[p + PH_COUNT].i;
			if (!in->a) obj_release(p);
			NEXT;
		}
		CASE(OP_POOL_NEXT) { /* for (h p): the next slot that holds an element */
			i64 p = s[sp - 2].i, i = s[sp - 1].i, cap;
			const Cell *hd = g_heap + p, *gens;
			MADE(p);
			cap = hd[PH_CAP].i;
			gens = g_heap + hd[PH_GENS].i;
			while (i < cap && !(gens[i].i & 1)) i++;
			s[sp - 2].i = i < cap ? i : -1;
			s[sp - 1].i = i < cap ? handle_of(hd, i) : 0;
			if (!in->a) obj_release(p);
			NEXT;
		}
		CASE(OP_ARENA_BEGIN)
			if (g_narenas == MAX_ARENAS) return vm_error(in, "arenas nested deeper than 64");
			memset(&g_arenas[g_narenas], 0, sizeof(ArenaLevel));
			g_arenas[g_narenas].start = g_arena_top;
			g_arenas[g_narenas].line = in->line;
			g_narenas++;
			NEXT;
		CASE(OP_ARENA_END) {
			const ArenaLevel *ar = &g_arenas[g_narenas - 1];
			i64 k;
			for (k = 0; k < 8; k++) {
				if (ar->live[k]) return arena_error(in, ar);
			}
			if (g_check) memset(g_heap + ar->start, 0xa5, (size_t)(g_arena_top - ar->start) * sizeof(Cell));
			g_arena_top = ar->start;
			g_narenas--;
			NEXT;
		}
		CASE(OP_KEEP)
			if (!keep(s + sp - cells((Ty)in->a), (Ty)in->a)) return vm_error(in, "keep: out of memory (the heap or the text arena is full)");
			NEXT;
		CASE(OP_CLOSURE) { /* a: the fn, b: the type of the values it keeps, which are on the stack and move in */
			i32 n = cells((Ty)in->b);
			i64 h = heap_alloc(CL_DATA + n);
			if (h < 0) return vm_error(in, HEAP_FULL);
			obj_born(h, KIND_CLOSURE, (u64)in->b);
			g_heap[h + CL_FN].i = in->a;
			sp -= n;
			memcpy(g_heap + h + CL_DATA, s + sp, (size_t)n * sizeof(Cell));
			s[sp++].i = h;
			NEXT;
		}
		CASE(OP_ENV_LOAD)
			memcpy(s + sp, g_heap + s[fp].i + CL_DATA + in->a, (size_t)in->b * sizeof(Cell));
			sp += in->b;
			NEXT;
		CASE(OP_ENV_STORE)
			sp -= in->b;
			memcpy(g_heap + s[fp].i + CL_DATA + in->a, s + sp, (size_t)in->b * sizeof(Cell));
			NEXT;
		CASE(OP_ENV_RELEASE) refs(g_heap + s[fp].i + CL_DATA + in->a, 1, (Ty)in->b, 0); NEXT;
		CASE(OP_CORO_NEW) { /* the new coroutine runs at once, until it first pauses or ends */
			const Fn *fn = &g_fns[in->a];
			i64 t = coro_new(fn, (Ty)in->b, in->line);
			if (t < 0) return vm_error(in, HEAP_FULL);
			memcpy(g_heap + g_heap[t + CO_STACK].i, s + sp - fn->pcells, (size_t)fn->pcells * sizeof(Cell));
			sp -= fn->pcells;
			s[sp++].i = t; /* the starter's handle: the coroutine's first count */
			g_heap[t + CO_RESUMER].i = g_cur;
			g_heap[t + CO_FLOOR].i = g_arena_floor;
			SAVE_REGS(pc);
			g_arena_floor = g_narenas;
			LOAD_REGS(t);
			NEXT;
		}
		CASE(OP_CORO_END) { /* the coroutine's function returned: its result goes into the coroutine, its stack goes */
			i64 t = g_cur;
			Cell *co = g_heap + t;
			i32 n = cells(g_tys[obj_type(t)].elem);
			memcpy(co + CO_RESULT, s + sp - n, (size_t)n * sizeof(Cell));
			heap_free(co[CO_STACK].i, co[CO_CAP].i);
			heap_free(co[CO_FRAMES].i, co[CO_FCAP].i);
			co[CO_STACK].i = co[CO_FRAMES].i = 0;
			co[CO_STATE].i = CS_DONE;
			g_arena_floor = (i32)co[CO_FLOOR].i;
			LOAD_REGS(co[CO_RESUMER].i);
			NEXT;
		}
		CASE(OP_CORO_POLL) { /* (some result) once it has ended, none before: nothing waits */
			i64 t = s[sp - 1].i;
			Ty rt = g_tys[(Ty)in->a].elem;
			i32 n = cells(rt);
			if (t <= 0) return vm_error(in, "this coroutine was never started: a global read before its def ran");
			if (coro_state(t) == CS_DEAD) return vm_error(in, "this coroutine was stopped by a runtime error");
			sp--;
			if (coro_state(t) >= CS_DONE) {
				s[sp].i = 1;
				memcpy(s + sp + 1, g_heap + t + CO_RESULT, (size_t)n * sizeof(Cell));
				refs(s + sp + 1, 1, rt, 1); /* a copy: the coroutine keeps its own */
			} else {
				memset(s + sp, 0, (size_t)(n + 1) * sizeof(Cell));
			}
			sp += 1 + n;
			obj_release(t);
			NEXT;
		}
		CASE(OP_SLEEP)
			if (g_cur) {
				g_heap[g_cur + CO_STATE].i = CS_SLEEPING;
				g_heap[g_cur + CO_UNTIL].f = g_coro_time + s[--sp].f;
				SUSPEND(pc);
				NEXT;
			}
			if (!g_mwait.kind) {
				main_wait_start(MW_SLEEP);
				g_mwait.until = g_coro_time + s[--sp].f;
			}
			{
				int r = main_wait();
				if (r > 0) { RESUME(g_coros[g_mwait.last]); NEXT; }
				g_mwait.kind = 0;
			}
			NEXT;
		CASE(OP_YIELD)
			if (!g_cur) return vm_error(in, "yield is for a coroutine; the main program runs its coroutines with step-coroutines");
			g_heap[g_cur + CO_STATE].i = CS_READY;
			SUSPEND(pc);
			NEXT;
		CASE(OP_CORO_STEP)
		CASE(OP_CORO_RUN) {
			int r;
			if (g_cur) return vm_error(in, "the main program runs the coroutines, not a coroutine: a coroutine pauses with sleep or yield");
			if (!g_mwait.kind) {
				main_wait_start(in->op == OP_CORO_STEP ? MW_STEP : MW_RUN);
				if (in->op == OP_CORO_STEP) g_coro_time += s[--sp].f;
			}
			if ((r = main_wait()) > 0) { RESUME(g_coros[g_mwait.last]); NEXT; }
			g_mwait.kind = 0;
			if (in->op == OP_CORO_STEP) s[sp++].i = g_ncoros;
			NEXT;
		}
		CASE(OP_CORO_DONE) {
			i64 t = s[sp - 1].i;
			s[sp - 1].i = t > 0 && coro_state(t) >= CS_DONE;
			obj_release(t);
			NEXT;
		}
		CASE(OP_CORO_TIME) s[sp++].f = g_coro_time; NEXT;
		CASE(OP_FAIL) return vm_error(in, g_strs[in->a].p);
		CASE(OP_GFX) { /* gfx.c: b arguments; the string among them (save-png's path, open-window's title, key-down?'s
		                * key) is let go of here */
			Cell out;
			i32 at = in->a == 7 || in->a == 9 ? (i32)sp - 1 : in->a == 8 ? (i32)sp - 3 : -1;
			const char *str = at >= 0 ? c_str(s[at].i) : 0;
			int r = gfx_op(in->a, s + sp - in->b, &out, str);
			if (at >= 0) obj_release(s[at].i);
			sp -= in->b;
			if (r < 0) return vm_error(in, g_gpu_error);
			if (r > 0) s[sp++] = out;
			NEXT;
		}
		CASE(OP_GPU) { /* gpu.c; a runtime error there stops the program, as on the VM */
			int r = gpu_run(in->a, s + fp);
			if (r < 0) return vm_error(in, g_gpu_error);
			s[sp++].i = r;
			NEXT;
		}
		CASE(OP_SHOW)
			sp -= cells((Ty)in->a);
			g_sink = stdout;
			fputs("=> ", stdout);
			print_value((Ty)in->a, s + sp);
			printf(" : %s\n", tn((Ty)in->a));
			refs(s + sp, 1, (Ty)in->a, 0);
			NEXT;

		/* the superinstructions (ops.h): a run of instructions in one step, then past the NOPs it left */
		VM_BIN(ADD_I, i) VM_BIN(SUB_I, i) VM_BIN(MUL_I, i)
		VM_BIN(ADD_F, f) VM_BIN(SUB_F, f) VM_BIN(MUL_F, f) VM_BIN(DIV_F, f)
		CASE(OP_MOD_I_LK) s[sp].i = s[fp + in->a].i % g_ints[in->b]; sp++; pc += 2; NEXT;
		CASE(OP_MOD_I_SK) s[sp - 1].i %= g_ints[in->a]; pc++; NEXT;
		VM_JMP(LT_I, i, <) VM_JMP(LE_I, i, <=) VM_JMP(GT_I, i, >) VM_JMP(GE_I, i, >=) VM_JMP(EQ_I, i, ==) VM_JMP(NE_I, i, !=)
		VM_JMP(LT_F, f, <) VM_JMP(LE_F, f, <=) VM_JMP(GT_F, f, >) VM_JMP(GE_F, f, >=) VM_JMP(EQ_F, f, ==) VM_JMP(NE_F, f, !=)
		CASE(OP_LOAD2_L) s[sp] = s[fp + in->a]; s[sp + 1] = s[fp + in->b]; sp += 2; pc++; NEXT;
		CASE(OP_STORE_LOAD_L) s[fp + in->a] = s[--sp]; s[sp++] = s[fp + in->b]; pc++; NEXT;
		CASE(OP_INC_JMP) s[fp + in->a].i = (i64)((u64)s[fp + in->a].i + 1); pc = in->b; NEXT;
		CASE(OP_NOP) NEXT;
		default:
			return vm_error(in, "internal: unknown instruction");
		}
	}
}

/* leak_check: free what the globals hold; 0 when that leaves nothing on the heap */
static i64 leak_check(void)
{
	i32 i;
	for (i = 0; i < g_nglobals; i++) refs(g_global_cells + g_globals[i].cell, 1, g_globals[i].type, 0);
	drain();
	return g_live;
}

#define GAEL_OP_NAME_(name, n) #name,
static const char *const g_op_names[] = { GAEL_OPS(GAEL_OP_NAME_) "?" };
#define GAEL_OP_ARGS_(name, n) n,
static const int g_op_args[] = { GAEL_OPS(GAEL_OP_ARGS_) 0 };

/* fused_form OP: the form of a superinstruction (ops.h): 'L' LL, 'K' LK, 'l' SL, 'k' SK, 'S' SS, 0 for another
 * instruction; *FL gets 1 when its values are floats, *JUMP 1 for a compare-and-jump */
static int fused_form(u8 op, int *fl, int *jump)
{
	*fl = *jump = 0;
	if (op >= OP_ADD_I_LL && op <= OP_DIV_F_SK) {
		*fl = op >= OP_ADD_F_LL;
		return "LKlk"[(op - OP_ADD_I_LL) % 4];
	}
	if (op == OP_MOD_I_LK || op == OP_MOD_I_SK) return op == OP_MOD_I_LK ? 'K' : 'k';
	if (op >= OP_JLT_I_LL && op <= OP_JNE_F_SS) {
		*fl = op >= OP_JLT_F_LL;
		*jump = 1;
		return "LKkS"[(op - OP_JLT_I_LL) % 4];
	}
	return 0;
}

/* disassemble FROM TO: the code of one unit, one instruction per line; the NOPs a superinstruction left are not shown */
static o disassemble(i32 from, i32 to)
{
	i32 pc;
	for (pc = from; pc < to; pc++) {
		const Ins *in = &g_code[pc];
		int fl, jump, form = fused_form(in->op, &fl, &jump);
		if (in->op == OP_NOP) continue;
		printf("%5ld  %-8s", (long)pc, g_op_names[in->op]);
		if (g_op_args[in->op] >= 1) printf(" %ld", (long)in->a);
		if (g_op_args[in->op] >= 2) printf(" %ld", (long)in->b);
		if (form == 'K' || form == 'k') { /* the constant */
			i32 k = form == 'K' ? in->b : in->a;
			if (fl) { g_sink = stdout; fputs("\t(", stdout); print_float(g_floats[k]); putchar(')'); }
			else printf("\t(%ld)", (long)g_ints[k]);
		}
		if (jump) { /* where it jumps, and when */
			const Ins *t = form == 'S' ? in : in + 1;
			printf("\t-> %ld when %s", (long)t->a, t->b ? "true" : "false");
		}
		if (in->op == OP_PUSH_I) printf("\t(%ld)", (long)g_ints[in->a]);
		if (in->op == OP_PUSH_F) { g_sink = stdout; fputs("\t(", stdout); print_float(g_floats[in->a]); putchar(')'); }
		if (in->op == OP_PUSH_S || in->op == OP_FAIL) printf("\t(\"%.40s\")", g_strs[in->a].p);
		if (in->op == OP_PRINT || in->op == OP_PRINTLN || in->op == OP_FORMAT) printf("\t(\"%.40s\")", g_strs[g_fmts[in->a].str].p);
		if (in->op == OP_CALL || in->op == OP_TAILCALL || in->op == OP_CORO_NEW || in->op == OP_CLOSURE) printf("\t(%.40s)", g_fns[in->a].name);
		if (in->op == OP_SHOW || in->op == OP_RETAIN || in->op == OP_DROP || in->op == OP_MAP_KEYS || in->op == OP_MAP_VALUES ||
		    in->op == OP_KEEP || in->op == OP_WRAP || in->op == OP_F2I || in->op == OP_CALL_V || in->op == OP_TAILCALL_V ||
		    in->op == OP_CORO_POLL)
			printf("\t(%.40s)", tn((Ty)in->a));
		if (in->op == OP_RELEASE_L || in->op == OP_RELEASE_G || in->op == OP_VEC_NEW || in->op == OP_MAP_NEW || in->op == OP_POOL_NEW)
			printf("\t(%.40s)", tn((Ty)in->b));
		if (in->line) printf("\t; line %ld", (long)in->line);
		putchar('\n');
	}
}
