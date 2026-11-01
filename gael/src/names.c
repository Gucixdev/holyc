/* names.c - the top level's names in one hash index.
 *
 * Functions, globals, structs and enums, and macros share one namespace, and the built-in names are taken. A slot
 * holds a name once, with its number in each table that has it (-1 when that table does not), so finding a function
 * is a probe or a few instead of a walk over every function: compiling grew with the square of the number of
 * functions before. Open addressing with linear probing, FNV-1a; slots are never deleted - a failed REPL unit, which
 * takes names back, clears the index and fills it again from the tables (compile.c). The index starts small and
 * doubles (refilled from the tables) when it is half full: a hash puts every name on a page of its own, so a big
 * index would cost a page fault for each name. */

#define NAME_SLOTS (1 << 18) /* the most slots: a power of two, at least twice the names there can be */

typedef struct {
	const char *name; /* lives as long as the table entry it names */
	u32 hash;
	u8 reserved;      /* a form, an operator, a builtin or a built-in type */
	i32 fn, global, type, macro;
} NameSlot;

static NameSlot *g_names; /* reserved address space for NAME_SLOTS (compile.c's tables_init) */
static i32 g_nnames;
static u32 g_name_cap = 1 << 10; /* the slots in use: a power of two */

static o names_rebuild(void); /* compile.c: the index again from the tables */
static i32 g_unit_gem; /* the gem the unit being compiled belongs to, 0 for the program; what it defines is that gem's */

static u32 name_hash(const char *s)
{
	u32 h = 2166136261u;
	while (*s) {
		h ^= (u8)*s++;
		h *= 16777619u;
	}
	return h;
}

/* name_at NAME HASH: the slot that holds NAME, or the empty one where it would go */
static NameSlot *name_at(const char *s, u32 h)
{
	u32 i = h & (g_name_cap - 1);
	while (g_names[i].name && (g_names[i].hash != h || strcmp(g_names[i].name, s))) i = (i + 1) & (g_name_cap - 1);
	return &g_names[i];
}

/* names_get NAME: its slot, 0 when nothing has that name */
static const NameSlot *names_get(const char *s)
{
	const NameSlot *sl = name_at(s, name_hash(s));
	return sl->name ? sl : 0;
}

/* names_put NAME: its slot, made when it is new; NAME must outlive it */
static NameSlot *names_put(const char *s)
{
	u32 h = name_hash(s);
	NameSlot *sl = name_at(s, h);
	if (!sl->name && g_nnames + 1 > (i32)(g_name_cap / 2)) { /* half full: twice as big, filled again */
		if (g_name_cap == NAME_SLOTS) die("too many names (NAME_SLOTS)");
		g_name_cap *= 2;
		names_rebuild();
		sl = name_at(s, h);
	}
	if (!sl->name) {
		sl->name = s;
		sl->hash = h;
		sl->reserved = 0;
		sl->fn = sl->global = sl->type = sl->macro = -1;
		g_nnames++;
	}
	return sl;
}

static o names_clear(void)
{
	if (g_nnames) memset(g_names, 0, g_name_cap * sizeof(NameSlot)); /* fresh reserved pages are zero already */
	g_nnames = 0;
}
