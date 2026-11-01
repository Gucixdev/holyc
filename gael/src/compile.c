/* compile.c - type checking and bytecode generation in one pass over the tree, without recursion.
 *
 * A work stack of frames says which node is being compiled and how far it got (phase). A finished expression leaves
 * exactly one type on the type stack, and code that leaves its value on the VM stack (no value for unit). An error
 * is reported with its line and column and the expression gets the type ERR, which agrees with everything: one
 * mistake gives one message, compilation goes on, and every error of a unit comes out in one run.
 *
 * A value takes as many VM cells as its type says: one for a number, a bool, a string or an enum variant, the sum of
 * its fields for a struct, and one more than the value inside for an option (the tag comes first). Structs are
 * values, copied like ints. A string made at run time, a vec and a map are objects on the heap that count their
 * owners: reading a variable copies its value (RETAIN), and a value that is dropped or a variable whose scope ends
 * gives its objects up (DROP, RELEASE). A parent tells a child which type it expects (want), so `none` knows which
 * option it is; T_UNIT as the expected type means the value is dropped. The compiler also counts how deep each
 * frame's operand stack gets, so the VM can check that a whole frame fits before entering it. */

typedef u16 Ty;
enum { T_ERR, T_UNIT, T_INT, T_FLOAT, T_BOOL, T_STR, T_NONE, T_ANY, T_I8, T_I16, T_I32, T_U8, T_U16, T_U32, T_U64, T_F32, T_PATH,
	T_USER };
typedef enum { K_PRIM, K_STRUCT, K_ENUM, K_OPTION, K_VEC, K_MAP, K_POOL, K_HANDLE, K_FN, K_CORO } TyKind;

typedef struct {
	const char *name;
	u8 kind;
	Ty elem;      /* option, vec, pool and handle: the type inside; map: the value type; fn: the return type */
	Ty key;       /* map: the key type */
	i32 cells;    /* VM cells of one value */
	i32 first, n; /* struct: its fields g_fields[first...]; enum: its variants g_variants[first...]; fn: g_tparams */
	i32 line;     /* where it was declared; 0 for the built-in and the option types */
	const char *file;
	u8 broken;    /* its declaration had an error (already reported): uses of it stay quiet */
	i32 leaf_first, nleaf; /* the objects inside a value: g_leaves[leaf_first...] */
	i32 gem;      /* the gem that declared it, 0 for the program and the built-in types */
	u64 bits;     /* bit C: cell C of a value is a bool, which a vec or a pool keeps as one bit (0 past 64 cells) */
} TyInfo;

typedef struct {
	const char *name;
	Ty type;
	i32 offset; /* in cells, from the start of the struct */
	i32 def;    /* (hp : int 100): the function that gives its default value (Type@field), -1 when it has none */
} Field;

/* The tables that grow with the program are reserved address space (table in base.h): the limits are generous, and
 * pages come only where something is written */
#define MAX_TYS (1 << 16)
#define MAX_FIELDS (1 << 18)
#define MAX_VARIANTS (1 << 18)
static const TyInfo g_prim_tys[T_USER] = {
	{ "<error>", K_PRIM, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "unit", K_PRIM, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "int", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "float", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "bool", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1 },
	{ "string", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0 },
	{ "<none>", K_PRIM, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "any value", K_PRIM, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "i8", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "i16", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "i32", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "u8", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "u16", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "u32", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "u64", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "f32", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ "NodePath", K_PRIM, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 } /* $Player/Sprite, the program's own text */
};
static TyInfo *g_tys; /* the built-in ones first */
static i32 g_ntys = T_USER;
static Field *g_fields;
static i32 g_nfields;
static Ty *g_tparams; /* the parameter types of function types */
static i32 g_ntparams;
static const char **g_variants;
static i32 g_nvariants;

/* A leaf is a cell inside a value that holds an object (a string, a vec, a map). It holds one only when every guard
 * of it - the tag cell of each option around it - is not 0. The VM walks a type's leaves to count owners and free */
typedef struct {
	i32 off, guard_first, nguards;
} Leaf;

#define MAX_LEAVES (1 << 20)
static Leaf *g_leaves; /* leaf 0: the value's only cell (a string, a vec, a map); shared by those types */
static i32 g_nleaves = 1;
static i32 *g_guards;
static i32 g_nguards;
static Ty g_vec_str; /* (vec string), the type split and args make */

typedef struct {
	u8 op;
	i32 a, b;
	i32 line;
} Ins;

typedef struct {
	const char *p; /* in the text arena, followed by a 0 */
	u32 len;
	i64 cell;      /* the string as a VM value */
} Str;

#define MAX_PARAMS 8
typedef struct {
	const char *name;
	u8 nparams;
	Ty ret;
	Ty params[MAX_PARAMS];
	i32 pcells; /* cells the arguments take on the stack */
	i32 entry, nslots, maxdepth, line;
	const char *file;
	i32 decl_unit, decl_node; /* the defn that declared it, so only that one compiles a body into it */
	i32 gem;                  /* the gem it belongs to, 0 for the program */
	u8 env;                   /* made by (fn ...): it takes its closure (or 0) in slot 0, before the arguments */
} Fn;

typedef struct {
	const char *name;
	Ty type;
	i32 cell; /* the first of its cells in the global area */
	i32 line;
	const char *file;
	u8 exported;        /* @export: an editor shows it and lets you change it */
	const char *value;  /* the source of its value when that is a literal, for gael -x */
	i32 gem;            /* the gem it belongs to, 0 for the program */
} Global;

/* Gems. A gem's main file starts with its manifest, (gem name :version "0.1" :requires (a b) :provides (c)
 * :excludes (d)); the program turns gems on with (enable name...), which loads each one after what it requires (the
 * loader in main.c). A gem's code uses only its own names and those of the gems it requires - by name, or through
 * something they provide - checked at each use when compiling. Two gems on together may not provide the same thing,
 * and neither may exclude the other. The program can use every gem that is on; (disable name) turns one off, and its
 * names cannot be used again until (enable name). */
#define MAX_GEMS 1024
#define MAX_GEM_LINKS 16
enum { GEM_KNOWN = 1, GEM_LOADING, GEM_ON, GEM_OFF };
typedef struct {
	const char *name, *file, *version;
	const char *requires[MAX_GEM_LINKS], *provides[MAX_GEM_LINKS], *excludes[MAX_GEM_LINKS];
	i32 nrequires, nprovides, nexcludes;
	u8 state;
	u8 tested; /* gael test on its main file: its test code is compiled and its tests run; else neither */
} Gem;
/* The REPL: the functions from files that a unit gave new bodies, for the line repldump.gael writes before the unit.
 * The files stay as they are; the new bodies live in the running program and in the dump */
#define MAX_REPLACED 16
static struct {
	const char *name, *file;
	i32 line;
} g_replaced[MAX_REPLACED];
static i32 g_nreplaced;

/* The file each stretch of code was compiled from, for a runtime error's line: the units in the order compiled, a new
 * entry where the file changes */
#define MAX_CODE_FILES 4096
static struct {
	i32 start;
	const char *file;
} g_code_files[MAX_CODE_FILES];
static i32 g_ncode_files;

static const char *code_file(i32 pc)
{
	i32 i;
	for (i = g_ncode_files - 1; i >= 0; i--) {
		if (g_code_files[i].start <= pc) return g_code_files[i].file;
	}
	return g_unit.file;
}

/* Marks for the tools: (@warn "...") and (@err "...") produce no code; gael -x lists them, an editor shows them at
 * their line. Tests: each (@test "name" body...) is compiled as a function, run only by gael test */
#define MAX_MARKS 4096
static struct {
	const char *file, *text;
	i32 line;
	u8 err;
} g_marks[MAX_MARKS];
static i32 g_nmarks;
#define MAX_TESTS 4096
static struct {
	const char *name, *file;
	i32 line;
} g_tests[MAX_TESTS];
static i32 g_ntests;
static int g_test_mode; /* gael test: the top level's own forms are compiled but jumped over; defs run, then tests */
static int g_show_expanded; /* gael -m: the unit's forms are printed once its macros have expanded */


static Gem g_gems[MAX_GEMS]; /* zero until used, so it costs the binary nothing; gem 0 is set in compile_init */
static i32 g_ngems = 1;
static int g_unit_main; /* the unit is a gem's main file: it starts with the manifest */

static int gem_use(i32 n, i32 owner);

enum { AT_LOCAL, AT_GLOBAL, AT_ENV }; /* a slot of the frame, a global, a value the running fn's closure keeps */

typedef struct {
	const char *name;
	Ty type;
	i32 slot; /* the first of its slots in the frame */
	u32 stamp; /* the stamp of the name that bound it: a symbol sees only locals with its own stamp (macro.c) */
	u8 alias; /* a name for part of a value something else owns (the x of (some x)): never released by itself */
	u8 kind;  /* LK_LET, LK_FOR, LK_SOME, LK_PARAM: what the warning about an unused one suggests */
	u8 used;  /* read in its scope; a set alone does not count. A parameter counts as used */
	u8 set;   /* given a new value by set */
	i32 node; /* the name that bound it, -1 for none */
	i32 errs; /* the unit's errors when it was bound: one reported since may have hidden a use */
} Local;
enum { LK_LET, LK_FOR, LK_SOME, LK_PARAM };

/* a print's format: the string and the type of each value, so the VM can print values of any size */
typedef struct {
	i32 str, first, n, cells;
} Fmt;

/* what lives as long as the process: code, constants, strings, functions, globals */
#define MAX_CODE (1 << 24)
#define MAX_CONSTS (1 << 20)
#define MAX_FNS (1 << 16)
#define MAX_GLOBALS (1 << 16)
#define MAX_GLOBAL_CELLS (1 << 22)
#define MAX_FMTS (1 << 18)
#define PERM_BYTES ((size_t)256 << 20)
static Ins *g_code;
static i32 g_ncode;
static i64 *g_ints;
static i32 g_nints;
static f64 *g_floats;
static i32 g_nfloats;
static Str *g_strs;
static i32 g_nstrs;
static Fn *g_fns;
static i32 *g_snap_entry, *g_snap_slots, *g_snap_depth; /* the REPL: what each function was before a unit */
static i32 g_nfns;
static Global *g_globals;
static i32 g_nglobals, g_nglobal_cells;
static Fmt *g_fmts;
static i32 g_nfmts;
static Ty *g_fmt_tys;
static i32 g_nfmt_tys;
static Arena g_perm = { 0, 0, 0, "permanent" };

/* The program's own strings, each followed by a 0, and scratch space for strings being built at run time. A string
 * of the program is one cell: bit 63 set, its offset in this arena in bits 32 to 62 and its length in the low 32. It
 * lives as long as the process; a string made at run time is an object on the VM's heap (vm.c). vm_init reserves the
 * arena, as big as the heap */
static Arena g_text = { 0, 0, 0, "text" };

static i64 str_cell(size_t off, size_t len) { return (i64)((u64)1 << 63 | (u64)off << 32 | (u64)len); }

/* text_init: the 256 one-byte strings that (char n) returns, at the start of the text arena */
static o text_init(void)
{
	i32 c;
	for (c = 0; c < 256; c++) {
		g_text.base[2 * c] = (u8)c;
		g_text.base[2 * c + 1] = 0;
	}
	g_text.used = 512;
}

/* what one compilation needs */
#define MAX_LOCALS 1024
#define MAX_WORK 4096
static Local g_locals[MAX_LOCALS];
static i32 g_nlocals, g_local_floor; /* names below the floor belong to an outer frame and are not visible */
static i32 g_slots, g_maxslots;
static i32 g_depth, g_maxdepth;      /* the operand stack of the frame being compiled, in cells */
static Ty g_types[MAX_WORK];
static i32 g_ntypes;
static i32 g_patches[MAX_WORK];
static i32 g_npatches;
static u8 *g_seen;      /* match: the variants an arm already covers */

typedef enum {
	F_START, F_CALL, F_CORO, F_RETURN, F_IF, F_WHEN, F_WHILE, F_DO, F_LET, F_SET, F_AND, F_OR, F_NOT, F_ARITH, F_CMP, F_PRINT,
	F_CONV, F_BUILTIN, F_NEW, F_SOME, F_MATCH, F_VEC, F_VECOP, F_FOR, F_DOT, F_MAP, F_MAPOP, F_SHPIPE, F_POOL, F_ARENA,
	F_KEEP, F_CALLV, F_LAMBDA, F_XOR, F_BITS, F_COND, F_ASSERT
} Form;

typedef struct {
	i32 node;    /* the form being compiled; -1 for a body (a run of sibling forms) */
	u8 form, phase;
	u8 op;       /* the operator, which comparison, which builtin; set: a global; new: named fields; match: covered */
	u8 keep;     /* bodies: 1 keeps the value of the last form */
	u8 tail;     /* its value is the function's result, nothing runs after it: a call here is a tail call */
	Ty rt;       /* running type, a binding's declared type, the matched type */
	Ty want;     /* the type the parent expects; T_NONE when it does not know, T_UNIT when it drops the value */
	Ty res;      /* match: the type of the arms */
	i32 cur;     /* the child being compiled */
	i32 k;       /* argument number */
	i32 a, b, c; /* code positions to patch, a function, type, slot or format, a binding's name */
	i32 mark;    /* scope or patch-stack height to go back to */
	i32 mark2;   /* slot count to go back to */
	i32 depth;   /* operand stack depth to go back to at a branch */
} Frame;

static Frame g_work[MAX_WORK];
static i32 g_nwork;

/* Conditions (F_COND): the jumps a condition makes when it is false, and when it is true, waiting for their target */
static i32 g_fjumps[MAX_WORK], g_nfjumps, g_tjumps[MAX_WORK], g_ntjumps;
enum { CC_IF, CC_WHEN, CC_WHILE, CC_AND, CC_OR, CC_NOT, CC_NAND, CC_NOR, CC_ASSERT }; /* whose words a condition's error uses */
static i32 g_unit_id; /* counts compiled units; node indices restart in each */

#define NAME_FMT "%.64s"

static o err_node(i32 n, const char *fmt, ...)
{
	char msg[900];
	va_list ap;
	va_start(ap, fmt);
	vsprintf(msg, fmt, ap);
	va_end(ap);
	error_at(g_nodes[n].line, g_nodes[n].col, "%s", msg);
}

static o warn_node(i32 n, const char *fmt, ...)
{
	char msg[900];
	va_list ap;
	va_start(ap, fmt);
	vsprintf(msg, fmt, ap);
	va_end(ap);
	warning_at(g_nodes[n].line, g_nodes[n].col, "%s", msg);
}

/* where FILE LINE: "line 3" in the file being compiled, "lib.gael:3" in another one */
static const char *where(const char *file, i32 line)
{
	static char buf[300];
	if (!file || !strcmp(file, g_unit.file)) sprintf(buf, "line %ld", (long)line);
	else sprintf(buf, "%.250s:%ld", file, (long)line);
	return buf;
}

static const char *tn(Ty t) { return t < g_ntys ? g_tys[t].name : "<error>"; }
static i32 cells(Ty t) { return t < g_ntys ? g_tys[t].cells : 0; }
static int is_struct(Ty t) { return t < g_ntys && g_tys[t].kind == K_STRUCT; }
static int is_enum(Ty t) { return t < g_ntys && g_tys[t].kind == K_ENUM; }
static int is_option(Ty t) { return t < g_ntys && g_tys[t].kind == K_OPTION; }
static int is_vec(Ty t) { return t < g_ntys && g_tys[t].kind == K_VEC; }
static int is_map(Ty t) { return t < g_ntys && g_tys[t].kind == K_MAP; }
static int is_pool(Ty t) { return t < g_ntys && g_tys[t].kind == K_POOL; }
static int is_handle(Ty t) { return t < g_ntys && g_tys[t].kind == K_HANDLE; }
static int is_fn(Ty t) { return t < g_ntys && g_tys[t].kind == K_FN; }
static int is_coro(Ty t) { return t < g_ntys && g_tys[t].kind == K_CORO; }
static Ty elem_of(Ty t) { return t < g_ntys && g_tys[t].kind >= K_OPTION ? g_tys[t].elem : (Ty)T_ERR; }
static Ty key_of(Ty t) { return is_map(t) ? g_tys[t].key : (Ty)T_ERR; }
static int has_refs(Ty t) { return t < g_ntys && g_tys[t].nleaf > 0; }
/* the sized numbers: int is i64 and float is f64; an int narrower than 64 bits sits in its cell sign- or
 * zero-extended, an f32 as the f64 it rounds to */
static int is_int(Ty t) { return t == T_INT || (t >= T_I8 && t <= T_U64); }
static int is_float(Ty t) { return t == T_FLOAT || t == T_F32; }
static int numeric(Ty t) { return is_int(t) || is_float(t); }
static int is_unsigned(Ty t) { return t >= T_U8 && t <= T_U64; }
static i32 int_bits(Ty t) { return t == T_I8 || t == T_U8 ? 8 : t == T_I16 || t == T_U16 ? 16 : t == T_I32 || t == T_U32 ? 32 : 64; }
static int comparable(Ty t) { return numeric(t) || t == T_BOOL || t == T_STR || t == T_PATH || is_enum(t) || is_handle(t); }
static int key_type(Ty t) { return is_int(t) || t == T_BOOL || t == T_STR || t == T_PATH || is_enum(t) || is_handle(t); }
static int matchable(Ty t) { return is_int(t) || t == T_BOOL || t == T_STR || is_enum(t) || is_option(t); }

static o depth_add(i32 d)
{
	g_depth += d;
	if (g_depth > g_maxdepth) g_maxdepth = g_depth;
}

/* effect OP A B: how many cells the instruction adds to the operand stack (negative: takes away) */
static i32 builtin_effect(Op op);

static i32 effect(Op op, i32 a, i32 b)
{
	switch (op) {
	case OP_PUSH_I: case OP_PUSH_F: case OP_PUSH_S: case OP_PUSH_K: case OP_LOAD_L: case OP_LOAD_G: return 1;
	case OP_ZERO: return a;
	case OP_LOADN_L: case OP_LOADN_G: return b;
	case OP_POPN: return -a;
	case OP_DROP: return -cells((Ty)a);
	case OP_RETAIN: case OP_RELEASE_L: case OP_RELEASE_G: return 0;
	case OP_STOREN_L: case OP_STOREN_G: return -b;
	case OP_POP: case OP_STORE_L: case OP_STORE_G: case OP_JF: case OP_JT:
	case OP_ADD_I: case OP_SUB_I: case OP_MUL_I: case OP_DIV_I: case OP_MOD_I:
	case OP_ADD_F: case OP_SUB_F: case OP_MUL_F: case OP_DIV_F:
	case OP_LT_I: case OP_LE_I: case OP_GT_I: case OP_GE_I: case OP_EQ_I: case OP_NE_I:
	case OP_LT_F: case OP_LE_F: case OP_GT_F: case OP_GE_F: case OP_EQ_F: case OP_NE_F:
	case OP_EQ_S: case OP_NE_S: case OP_DIV_U: case OP_MOD_U: case OP_LT_U: case OP_LE_U: case OP_GT_U: case OP_GE_U: return -1;
	case OP_BAND: case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR: return -1;
	case OP_CALL: case OP_TAILCALL: return cells(g_fns[a].ret) - g_fns[a].pcells;
	case OP_CALL_V: case OP_TAILCALL_V: {
		i32 k, d = cells(g_tys[a].elem) - 1;
		for (k = 0; k < g_tys[a].n; k++) d -= cells(g_tparams[g_tys[a].first + k]);
		return d;
	}
	case OP_PRINT: case OP_PRINTLN: return -g_fmts[a].cells;
	case OP_FORMAT: return 1 - g_fmts[a].cells;
	case OP_PARSE_I: case OP_PARSE_F: return 1;
	case OP_VEC_NEW: return 1 - a * cells(elem_of((Ty)b));
	case OP_VEC_PUSH: return -(1 + a);
	case OP_VEC_GET: return a - 2;
	case OP_VEC_PUT: return -(2 + a);
	case OP_VEC_POP: return a;
	case OP_MAP_NEW: return 1 - a * (1 + cells(elem_of((Ty)b)));
	case OP_MAP_INSERT: return -(2 + a);
	case OP_MAP_LOOKUP: case OP_MAP_REMOVE: return a - 1;
	case OP_MAP_HAS: case OP_POOL_HAS: return -1;
	case OP_POOL_SPAWN: return -a;
	case OP_POOL_GET: return a - 2;
	case OP_POOL_PUT: return -(2 + a);
	case OP_POOL_LOOKUP: case OP_POOL_REMOVE: return a - 1;
	case OP_POOL_NEW: case OP_POOL_LEN: case OP_POOL_NEXT: return 0;
	case OP_ARENA_BEGIN: case OP_ARENA_END: case OP_KEEP: return 0;
	case OP_SHPIPE: return 1 - a;
	case OP_CORO_NEW: return 1 - g_fns[a].pcells;
	case OP_CLOSURE: return 1 - cells((Ty)b);
	case OP_ENV_LOAD: return b;
	case OP_ENV_STORE: return -b;
	case OP_ENV_RELEASE: return 0;
	case OP_CORO_POLL: return cells(elem_of((Ty)a));
	case OP_SLEEP: return -1;
	case OP_YIELD: case OP_CORO_STEP: case OP_CORO_RUN: case OP_CORO_DONE: case OP_CORO_END: return 0;
	case OP_CORO_TIME: return 1;
	case OP_SHOW: return -cells((Ty)a);
	default: return builtin_effect(op);
	}
}

static i32 emit(Op op, i32 a, i32 b, i32 line)
{
	if (g_ncode == MAX_CODE) die("program too large (MAX_CODE instructions)");
	g_code[g_ncode].op = (u8)op;
	g_code[g_ncode].a = a;
	g_code[g_ncode].b = b;
	g_code[g_ncode].line = line;
	depth_add(effect(op, a, b));
	return g_ncode++;
}

static o patch(i32 at, i32 target) { g_code[at].a = target; }

/* emit_load GLOBAL AT TYPE LINE: push the value of TYPE kept at slot (or global cell) AT */
/* WHERE is AT_LOCAL (AT a slot of the frame), AT_GLOBAL (a cell of the globals) or AT_ENV (an offset in the running
 * fn's closure, whose handle is in slot 0) */
static o emit_load(int where, i32 at, Ty t, i32 line)
{
	i32 n = cells(t);
	if (where == AT_ENV) { if (n) emit(OP_ENV_LOAD, at, n, line); }
	else if (n == 1) emit(where ? OP_LOAD_G : OP_LOAD_L, at, 0, line);
	else if (n > 1) emit(where ? OP_LOADN_G : OP_LOADN_L, at, n, line);
}

static o emit_store(int where, i32 at, Ty t, i32 line)
{
	i32 n = cells(t);
	if (where == AT_ENV) { if (n) emit(OP_ENV_STORE, at, n, line); }
	else if (n == 1) emit(where ? OP_STORE_G : OP_STORE_L, at, 0, line);
	else if (n > 1) emit(where ? OP_STOREN_G : OP_STOREN_L, at, n, line);
}

/* emit_copy WHERE AT TYPE LINE: push a copy of the value kept at AT; the objects in it get one owner more */
static o emit_copy(int where, i32 at, Ty t, i32 line)
{
	emit_load(where, at, t, line);
	if (has_refs(t)) emit(OP_RETAIN, t, 0, line);
}

/* emit_release WHERE AT TYPE LINE: the value kept at AT dies; the objects in it lose an owner */
static o emit_release(int where, i32 at, Ty t, i32 line)
{
	if (!has_refs(t)) return;
	emit(where == AT_ENV ? OP_ENV_RELEASE : where ? OP_RELEASE_G : OP_RELEASE_L, at, t, line);
}

/* borrow_last ARG REST: when ARG, just compiled, is a variable read (a LOAD and a RETAIN) and the operands REST after
 * it cannot give that variable a new value before the instruction runs, the RETAIN goes: the instruction borrows the
 * variable's value instead of consuming a copy. Only a set can change a local, so REST must not set one of that name;
 * any call may set a global, so after a global REST must be atoms. 1 when it did */
static i32 next_of(i32 n);
static i32 first_of(i32 n);

static int borrow_last(i32 arg, i32 rest)
{
	static i32 stack[MAX_WORK];
	const char *name, *dot;
	size_t len;
	i32 n = 0;
	int local;
	if (arg < 0 || g_nodes[arg].kind != N_SYM || g_ncode < 2 || g_code[g_ncode - 1].op != OP_RETAIN) return 0;
	local = g_code[g_ncode - 2].op == OP_LOAD_L || g_code[g_ncode - 2].op == OP_LOADN_L;
	name = g_nodes[arg].text;
	dot = strchr(name, '.');
	len = dot ? (size_t)(dot - name) : strlen(name);
	for (; rest >= 0; rest = next_of(rest)) {
		if (n == MAX_WORK) return 0;
		stack[n++] = rest;
	}
	while (n > 0) { /* every list in REST, nested ones too */
		i32 k = stack[--n], head = first_of(k), x;
		const char *target;
		if (g_nodes[k].kind != N_LIST) continue;
		if (!local) return 0;
		if (head >= 0 && g_nodes[head].kind == N_SYM && find_macro(g_nodes[head].text) >= 0) return 0; /* may hide a set */
		if (head >= 0 && g_nodes[head].kind == N_SYM && !strcmp(g_nodes[head].text, "set") && (x = next_of(head)) >= 0 &&
		    g_nodes[x].kind == N_SYM) {
			target = g_nodes[x].text;
			if (!strncmp(target, name, len) && (target[len] == 0 || target[len] == '.')) return 0;
		}
		for (x = head; x >= 0; x = next_of(x)) {
			if (n == MAX_WORK) return 0;
			stack[n++] = x;
		}
	}
	g_ncode--;
	return 1;
}

/* emit_pop TYPE LINE: throw away the value on top */
static o emit_pop(Ty t, i32 line)
{
	i32 n = cells(t);
	if (has_refs(t)) emit(OP_DROP, t, 0, line);
	else if (n == 1) emit(OP_POP, 0, 0, line);
	else if (n > 1) emit(OP_POPN, n, 0, line);
}

static o push_type(Ty t)
{
	if (g_ntypes == MAX_WORK) die("expression nested too deep (type stack)");
	g_types[g_ntypes++] = t;
}

static Ty pop_type(void) { return g_ntypes ? g_types[--g_ntypes] : (Ty)T_ERR; }

static i32 next_of(i32 n) { return n < 0 ? -1 : g_nodes[n].next; }
static i32 first_of(i32 n) { return n < 0 ? -1 : g_nodes[n].first; }

static i32 count_from(i32 n)
{
	i32 c = 0;
	for (; n >= 0; n = g_nodes[n].next) c++;
	return c;
}

static int is_sym(i32 n) { return n >= 0 && g_nodes[n].kind == N_SYM; }
static int sym_is(i32 n, const char *s) { return is_sym(n) && strcmp(g_nodes[n].text, s) == 0; }
static int head_is(i32 n, const char *s) { return n >= 0 && g_nodes[n].kind == N_LIST && sym_is(g_nodes[n].first, s); }

static const char *const g_reserved[] = {
	"if", "when", "while", "do", "let", "set", "and", "or", "not", "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "=",
	"!=", "print", "println", "int", "float", "bool", "string", "unit", "len", "sh", "defn", "def", "true", "false", ":",
	"struct", "enum", "match", "some", "none", "option", "_", "format", "eprint", "eprintln", "vec", "push", "get", "put",
	"pop", "for", "use", ".", "map", "insert", "lookup", "has", "remove", "keys", "values", "chain", "pipe", "pipe-in",
	"nand", "nor", "xor", "xnor", "bit-and", "bit-or", "bit-xor", "bit-not", "shl", "shr",
	"pool", "spawn", "handle", "arena", "keep", "fn", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
	"defmacro", "...", "gem", "enable", "disable", "return", "coroutine", "poll", "sleep", "yield", "step-coroutines", "run-coroutines", "done?", "coroutine-time",
	"string-of", "name-of", "assert", "compile-error", "gpu-each"
};


/* min, max, abs and clamp: built in, but not reserved (soft_form) */
static const char *const g_soft_names[] = { "min", "max", "abs", "clamp" };
#define SOFT_MARK 1000 /* the ival of the < and the if a soft form wrote: SOFT_MARK + its index in g_soft_names */
static i32 g_soft_k; /* the soft form being rewritten */

/* the forms on coroutines, in the order of F_CORO's op */
static const char *const g_coro_names[] = { "poll", "sleep", "yield", "step-coroutines", "run-coroutines", "done?", "coroutine-time" };
static const char *const g_coro_usage[] = { "(poll co)", "(sleep 0.5)", "(yield)", "(step-coroutines dt)", "(run-coroutines)", "(done? co)", "(coroutine-time)" };

static int is_reserved(const char *s)
{
	const NameSlot *sl = names_get(s);
	return sl && sl->reserved;
}

/* find_named_type NAME: a built-in type, a struct or an enum by name; T_NONE when there is none */
static Ty find_named_type(const char *s)
{
	const NameSlot *sl = names_get(s);
	if (!strcmp(s, "i64")) return T_INT;
	if (!strcmp(s, "f64")) return T_FLOAT;
	return sl && sl->type >= 0 ? (Ty)sl->type : (Ty)T_NONE;
}

/* add_leaf LEAF SHIFT GUARD: a leaf of a type inside the one being made, SHIFT cells further on; with GUARD it sits in
 * an option whose tag is the cell before SHIFT */
static o add_leaf(const Leaf *from, i32 shift, int guard)
{
	Leaf *lf;
	i32 j;
	if (g_nleaves == MAX_LEAVES || g_nguards + from->nguards + 1 > MAX_LEAVES) die("too many strings, vecs and maps inside types");
	lf = &g_leaves[g_nleaves++];
	lf->off = from->off + shift;
	lf->guard_first = g_nguards;
	lf->nguards = from->nguards + guard;
	if (guard) g_guards[g_nguards++] = shift - 1;
	for (j = 0; j < from->nguards; j++) g_guards[g_nguards++] = g_guards[from->guard_first + j] + shift;
}

/* set_leaves TYPE: where the objects inside a value of TYPE are, from the leaves of the types it holds; and which of
 * its cells are bools */
static o set_leaves(Ty t)
{
	TyInfo *ti = &g_tys[t];
	i32 first = g_nleaves, i, k;
	ti->bits = 0;
	if (ti->kind == K_VEC || ti->kind == K_MAP || ti->kind == K_POOL || ti->kind == K_CORO || ti->kind == K_FN) {
		ti->leaf_first = 0;
		ti->nleaf = 1;
		return;
	}
	if (ti->cells <= 64 && ti->kind == K_OPTION) ti->bits = g_tys[ti->elem].bits << 1;
	for (i = 0; ti->cells <= 64 && ti->kind == K_STRUCT && i < ti->n; i++)
		ti->bits |= g_tys[g_fields[ti->first + i].type].bits << g_fields[ti->first + i].offset;
	if (ti->kind == K_OPTION) {
		const TyInfo *e = &g_tys[ti->elem];
		for (k = 0; k < e->nleaf; k++) add_leaf(&g_leaves[e->leaf_first + k], 1, 1);
	}
	for (i = 0; ti->kind == K_STRUCT && i < ti->n; i++) {
		const Field *fd = &g_fields[ti->first + i];
		const TyInfo *e = &g_tys[fd->type];
		for (k = 0; k < e->nleaf; k++) add_leaf(&g_leaves[e->leaf_first + k], fd->offset, 0);
	}
	ti->leaf_first = first;
	ti->nleaf = g_nleaves - first;
}

/* intern_wrap KIND TYPE: the type (option TYPE), (vec TYPE), (pool TYPE) or (handle TYPE), made once. An option holds
 * its value in place (a tag cell first); a vec and a pool are one cell, a handle to their elements on the heap; a
 * handle is one cell, a slot of a pool and the generation it had when the element went in */
static Ty intern_wrap(u8 kind, Ty elem)
{
	char name[160];
	TyInfo *t;
	i32 i;
	for (i = T_USER; i < g_ntys; i++) {
		if (g_tys[i].kind == kind && g_tys[i].elem == elem) return (Ty)i;
	}
	if (g_ntys == MAX_TYS) die("too many types");
	sprintf(name, "(%s %.140s)", kind == K_VEC ? "vec" : kind == K_POOL ? "pool" : kind == K_HANDLE ? "handle" : kind == K_CORO ? "coroutine" : "option",
	        tn(elem));
	t = &g_tys[g_ntys];
	memset(t, 0, sizeof(*t));
	t->name = arena_strndup(&g_perm, name, strlen(name));
	t->kind = kind;
	t->elem = elem;
	t->cells = kind == K_OPTION ? 1 + cells(elem) : 1;
	set_leaves((Ty)g_ntys);
	return (Ty)g_ntys++;
}

static Ty intern_option(Ty elem) { return intern_wrap(K_OPTION, elem); }

/* intern_map KEY VALUE: the type (map KEY VALUE), made once; one cell, a handle to the map on the heap */
static Ty intern_map(Ty key, Ty val)
{
	char name[160];
	TyInfo *t;
	i32 i;
	for (i = T_USER; i < g_ntys; i++) {
		if (g_tys[i].kind == K_MAP && g_tys[i].key == key && g_tys[i].elem == val) return (Ty)i;
	}
	if (g_ntys == MAX_TYS) die("too many types");
	sprintf(name, "(map %.70s %.70s)", tn(key), tn(val));
	t = &g_tys[g_ntys];
	memset(t, 0, sizeof(*t));
	t->name = arena_strndup(&g_perm, name, strlen(name));
	t->kind = K_MAP;
	t->key = key;
	t->elem = val;
	t->cells = 1;
	set_leaves((Ty)g_ntys);
	return (Ty)g_ntys++;
}

/* intern_fn PARAMS N RET: the type (fn (params...) ret), made once. A function value is one cell, the function's
 * number + 1 (0: none); a call through it checks the arguments against these types like a direct call */
static Ty intern_fn(const Ty *params, i32 n, Ty ret)
{
	char name[300];
	size_t used;
	TyInfo *t;
	i32 i, j;
	for (i = T_USER; i < g_ntys; i++) {
		const TyInfo *ti = &g_tys[i];
		if (ti->kind != K_FN || ti->n != n || ti->elem != ret) continue;
		for (j = 0; j < n && g_tparams[ti->first + j] == params[j]; j++) {}
		if (j == n) return (Ty)i;
	}
	if (g_ntys == MAX_TYS) die("too many types");
	if (g_ntparams + n > MAX_FIELDS) die("too many function types");
	strcpy(name, "(fn (");
	for (i = 0; i < n; i++) {
		used = strlen(name);
		sprintf(name + used, "%s%.24s", i ? " " : "", tn(params[i]));
	}
	used = strlen(name);
	sprintf(name + used, ") %.40s)", tn(ret));
	t = &g_tys[g_ntys];
	memset(t, 0, sizeof(*t));
	t->name = arena_strndup(&g_perm, name, strlen(name));
	t->kind = K_FN;
	t->elem = ret;
	t->first = g_ntparams;
	t->n = n;
	t->cells = 1;
	memcpy(g_tparams + g_ntparams, params, (size_t)n * sizeof(Ty));
	g_ntparams += n;
	set_leaves((Ty)g_ntys);
	return (Ty)g_ntys++;
}

/* compile_init: what every program starts with */
static o names_rebuild(void);

static i32 g_coro_end; /* the instruction a coroutine's function returns to: CORO_END */

static o tables_init(void)
{
	g_nodes = (Node *)table(MAX_NODES, sizeof(Node));
	g_unit_arena.base = (u8 *)table(UNIT_BYTES, 1);
	g_unit_arena.cap = UNIT_BYTES;
	g_tys = (TyInfo *)table(MAX_TYS, sizeof(TyInfo));
	memcpy(g_tys, g_prim_tys, sizeof(g_prim_tys));
	g_fields = (Field *)table(MAX_FIELDS, sizeof(Field));
	g_variants = (const char **)table(MAX_VARIANTS, sizeof(const char *));
	g_tparams = (Ty *)table(MAX_FIELDS, sizeof(Ty));
	g_leaves = (Leaf *)table(MAX_LEAVES, sizeof(Leaf));
	g_guards = (i32 *)table(MAX_LEAVES, sizeof(i32));
	g_code = (Ins *)table(MAX_CODE, sizeof(Ins));
	g_ints = (i64 *)table(MAX_CONSTS, sizeof(i64));
	g_floats = (f64 *)table(MAX_CONSTS, sizeof(f64));
	g_strs = (Str *)table(MAX_CONSTS, sizeof(Str));
	g_fmt_tys = (Ty *)table(MAX_CONSTS, sizeof(Ty));
	g_fns = (Fn *)table(MAX_FNS, sizeof(Fn));
	g_snap_entry = (i32 *)table(MAX_FNS, sizeof(i32));
	g_snap_slots = (i32 *)table(MAX_FNS, sizeof(i32));
	g_snap_depth = (i32 *)table(MAX_FNS, sizeof(i32));
	g_globals = (Global *)table(MAX_GLOBALS, sizeof(Global));
	g_fmts = (Fmt *)table(MAX_FMTS, sizeof(Fmt));
	g_seen = (u8 *)table(MAX_VARIANTS, 1);
	g_perm.base = (u8 *)table(PERM_BYTES, 1);
	g_perm.cap = PERM_BYTES;
	g_names = (NameSlot *)table(NAME_SLOTS, sizeof(NameSlot));
	g_mnodes = (Node *)table(MAX_MNODES, sizeof(Node));
	g_rules = (Rule *)table(MAX_RULES, sizeof(Rule));
	g_macros = (Macro *)table(MAX_MACROS, sizeof(Macro));
	g_mtext.base = (u8 *)table(MTEXT_BYTES, 1);
	g_mtext.cap = MTEXT_BYTES;
}

static o compile_init(void)
{
	g_gems[0].name = "the program";
	g_gems[0].state = GEM_ON;
	tables_init();
	text_init();
	g_coro_end = emit(OP_CORO_END, 0, 0, 0);
	g_vec_str = intern_wrap(K_VEC, T_STR);
	names_rebuild();
}

/* Builtins with a fixed signature. Argument and result types are letters: i int, f float, b bool, s string, u unit,
 * I (option int), F (option float), O (option string), S (vec string), x a string or a vec (len). params names the
 * arguments for messages. */
typedef struct {
	const char *name, *args, *params;
	char ret;
	Op op;
	i32 a, b; /* the instruction's operands */
} Builtin;

static const Builtin g_builtins[] = {
	{ "len", "x", "s", 'i', OP_LEN_S, 0, 0 },
	{ "sh", "s", "command", 's', OP_SH, 0, 0 },
	{ "run", "s", "command", 'b', OP_RUN, 0, 0 },
	{ "pid", "", "", 'i', OP_PID, 0, 0 },
	{ "env", "s", "name", 's', OP_ENV, 0, 0 },
	{ "system", "s", "command", 'i', OP_SYSTEM, 0, 0 },
	{ "parse-int", "s", "s", 'I', OP_PARSE_I, 0, 0 },
	{ "parse-float", "s", "s", 'F', OP_PARSE_F, 0, 0 },
	{ "substr", "sii", "s from to", 's', OP_SUBSTR, 0, 0 },
	{ "byte", "si", "s i", 'i', OP_BYTE, 0, 0 },
	{ "find", "ss", "s part", 'I', OP_FIND, 0, 0 },
	{ "split", "ss", "s separator", 'S', OP_SPLIT, 0, 0 },
	{ "join", "Ss", "v separator", 's', OP_JOIN, 0, 0 },
	{ "char", "i", "n", 's', OP_CHAR, 0, 0 },
	{ "trim", "s", "s", 's', OP_TRIM, 0, 0 },
	{ "read-file", "s", "path", 'O', OP_READ_FILE, 0, 0 },
	{ "write-file", "ss", "path text", 'b', OP_WRITE_FILE, 0, 0 },
	{ "append-file", "ss", "path text", 'b', OP_WRITE_FILE, 1, 0 },
	{ "read-line", "", "", 'O', OP_READ_LINE, 0, 0 },
	{ "getenv", "s", "name", 'O', OP_GETENV, 0, 0 },
	{ "args", "", "", 'S', OP_ARGS, 0, 0 },
	{ "exit", "i", "status", 'u', OP_EXIT, 0, 0 },
	{ "sqrt", "f", "x", 'f', OP_MATH, 0, 0 },
	{ "sin", "f", "x", 'f', OP_MATH, 1, 0 },
	{ "cos", "f", "x", 'f', OP_MATH, 2, 0 },
	{ "tan", "f", "x", 'f', OP_MATH, 3, 0 },
	{ "asin", "f", "x", 'f', OP_MATH, 4, 0 },
	{ "acos", "f", "x", 'f', OP_MATH, 5, 0 },
	{ "atan", "f", "x", 'f', OP_MATH, 6, 0 },
	{ "exp", "f", "x", 'f', OP_MATH, 7, 0 },
	{ "ln", "f", "x", 'f', OP_MATH, 8, 0 },
	{ "floor", "f", "x", 'f', OP_MATH, 9, 0 },
	{ "ceil", "f", "x", 'f', OP_MATH, 10, 0 },
	{ "atan2", "ff", "y x", 'f', OP_MATH2, 0, 0 },
	{ "pow", "ff", "x y", 'f', OP_MATH2, 1, 0 },
	{ "new-canvas", "ii", "width height", 'i', OP_GFX, 0, 2 },
	{ "draw-clear", "ii", "canvas color", 'u', OP_GFX, 1, 2 },
	{ "draw-rect", "iffffi", "canvas x y width height color", 'u', OP_GFX, 2, 6 },
	{ "draw-circle", "ifffi", "canvas x y radius color", 'u', OP_GFX, 3, 5 },
	{ "draw-line", "ifffffi", "canvas x1 y1 x2 y2 width color", 'u', OP_GFX, 4, 7 },
	{ "show-frame", "i", "canvas", 'b', OP_GFX, 5, 1 },
	{ "canvas-pixel", "iii", "canvas x y", 'i', OP_GFX, 6, 3 },
	{ "save-png", "is", "canvas path", 'b', OP_GFX, 7, 2 },
	{ "open-window", "sii", "title width height", 'i', OP_GFX, 8, 3 },
	{ "key-down?", "is", "window key", 'b', OP_GFX, 9, 2 },
	{ "mouse-x", "i", "window", 'f', OP_GFX, 10, 1 },
	{ "mouse-y", "i", "window", 'f', OP_GFX, 11, 1 },
	{ "mouse-down?", "ii", "window button", 'b', OP_GFX, 12, 2 }
};

static Ty code_type(char c)
{
	switch (c) {
	case 'i': return T_INT;
	case 'f': return T_FLOAT;
	case 'b': return T_BOOL;
	case 's': return T_STR;
	case 'I': return intern_option(T_INT);
	case 'F': return intern_option(T_FLOAT);
	case 'S': return intern_wrap(K_VEC, T_STR);
	case 'O': return intern_option(T_STR);
	case 'u': return T_UNIT;
	default: return T_NONE; /* x: the form checks it */
	}
}

static i32 builtin_effect(Op op)
{
	size_t i;
	const char *a;
	i32 d;
	for (i = 0; i < GAEL_COUNT(g_builtins); i++) {
		if (g_builtins[i].op != op) continue;
		d = cells(code_type(g_builtins[i].ret));
		for (a = g_builtins[i].args; *a; a++) d -= *a == 'x' ? 1 : cells(code_type(*a));
		return d;
	}
	return 0;
}

/* param_name BUILTIN K BUF: the name of argument K, for a message */
static const char *param_name(const Builtin *bi, i32 k, char *buf)
{
	const char *p = bi->params, *end;
	for (; k > 0 && (p = strchr(p, ' ')) != 0; k--) p++;
	if (!p) return "?";
	end = strchr(p, ' ');
	sprintf(buf, "%.*s", (int)(end ? end - p : (long)strlen(p)), p);
	return buf;
}

/* a_type TYPE: "a string", "an int" - for a message about the one argument of a builtin */
static const char *a_type(Ty t)
{
	static char buf[200];
	const char *name = tn(t);
	sprintf(buf, "%s %.180s", strchr("aeio", name[0]) || (name[0] == 'f' && name[1] >= '0' && name[1] <= '9') ? "an" : "a", name);
	return buf;
}

/* parse_type NODE: the type a node names - int, float, bool, string, unit, a struct, an enum, (option type),
 * (vec type) or (map key value) - or an error and ERR. Types nest, so it keeps its own stack */
static Ty parse_type(i32 root)
{
	struct {
		i32 node;
		u8 phase;
	} st[MAX_DEPTH + 1];
	Ty res[(MAX_DEPTH + 2) * (MAX_PARAMS + 1)]; /* finished types: a map's key or a fn's parameters wait here */
	i32 ns = 1, nr = 0;
	st[0].node = root;
	st[0].phase = 0;
	while (ns > 0) {
		i32 n = st[ns - 1].node, a1, a2;
		Ty t;
		if (n < 0) return T_ERR;
		if (g_nodes[n].kind != N_LIST) {
			if (!is_sym(n)) {
				err_node(n, "expected a type here: a number type, bool, string, unit, a struct, an enum, (option type), (vec type), (map key value), (pool type), (handle type), (coroutine type) or (fn (types) type)");
				return T_ERR;
			}
			t = find_named_type(g_nodes[n].text);
			if (t == T_NONE) {
				err_node(n, "unknown type " NAME_FMT, g_nodes[n].text);
				return T_ERR;
			}
			if (t >= T_USER && !gem_use(n, g_tys[t].gem)) return T_ERR; /* the built-in types are everyone's */
			res[nr++] = t;
			ns--;
			continue;
		}
		a1 = next_of(first_of(n));
		a2 = next_of(a1);
		if (head_is(n, "option") || head_is(n, "vec") || head_is(n, "pool") || head_is(n, "handle") || head_is(n, "coroutine")) {
			const char *what = g_nodes[first_of(n)].text;
			u8 kind = head_is(n, "vec") ? K_VEC : head_is(n, "pool") ? K_POOL : head_is(n, "handle") ? K_HANDLE :
			          head_is(n, "coroutine") ? K_CORO : K_OPTION;
			if (a1 < 0 || a2 >= 0) {
				err_node(n, "%s %s type is (%s type), like (%s int)", kind == K_OPTION ? "an" : "a", what, what, what);
				return T_ERR;
			}
			if (st[ns - 1].phase == 0) {
				st[ns - 1].phase = 1;
				st[ns].node = a1;
				st[ns++].phase = 0;
				continue;
			}
			t = res[--nr];
			if (t == T_UNIT && kind != K_CORO) {
				err_node(a1, kind == K_OPTION ? "(option unit) holds nothing; use bool" : "(%s unit) holds nothing; count with an int", what);
				return T_ERR;
			}
			res[nr++] = intern_wrap(kind, t);
			ns--;
			continue;
		}
		if (head_is(n, "fn")) { /* (fn (param types...) return type) */
			i32 np = a1 >= 0 && g_nodes[a1].kind == N_LIST ? count_from(first_of(a1)) : -1, x, k;
			Ty params[MAX_PARAMS];
			if (np < 0 || a2 < 0 || next_of(a2) >= 0) {
				err_node(n, "a function type is (fn (parameter types) return type), like (fn (int int) int)");
				return T_ERR;
			}
			if (np > MAX_PARAMS) {
				err_node(a1, "at most %d parameters", MAX_PARAMS);
				return T_ERR;
			}
			if (st[ns - 1].phase <= np) { /* the parameter types in turn, then the return type */
				for (x = first_of(a1), k = 0; k < st[ns - 1].phase && x >= 0; k++) x = next_of(x);
				st[ns].node = st[ns - 1].phase == np ? a2 : x;
				st[ns].phase = 0;
				st[ns - 1].phase++;
				ns++;
				continue;
			}
			t = res[--nr];
			nr -= np;
			for (k = 0; k < np; k++) {
				params[k] = res[nr + k];
				if (params[k] == T_UNIT) {
					err_node(a1, "a parameter cannot be unit");
					return T_ERR;
				}
			}
			res[nr++] = intern_fn(params, np, t);
			ns--;
			continue;
		}
		if (head_is(n, "map")) {
			Ty k;
			if (a1 < 0 || a2 < 0 || next_of(a2) >= 0) {
				err_node(n, "a map type is (map key value), like (map string int)");
				return T_ERR;
			}
			if (st[ns - 1].phase < 2) {
				st[ns].node = st[ns - 1].phase ? a2 : a1;
				st[ns].phase = 0;
				st[ns - 1].phase++;
				ns++;
				continue;
			}
			t = res[--nr];
			k = res[--nr];
			if (!key_type(k)) {
				err_node(a1, "a map's key is an int, a bool, a string or an enum value, not %s", tn(k));
				return T_ERR;
			}
			if (t == T_UNIT) {
				err_node(a2, "a map of unit holds nothing; use (map %s bool)", tn(k));
				return T_ERR;
			}
			res[nr++] = intern_map(k, t);
			ns--;
			continue;
		}
		err_node(n, "expected a type here: a number type, bool, string, unit, a struct, an enum, (option type), (vec type), (map key value), (pool type), (handle type), (coroutine type) or (fn (types) type)");
		return T_ERR;
	}
	return res[0];
}

static i32 add_int(i64 v)
{
	if (g_nints == MAX_CONSTS) die("too many int constants");
	g_ints[g_nints] = v;
	return g_nints++;
}

static i32 add_float(f64 v)
{
	if (g_nfloats == MAX_CONSTS) die("too many float constants");
	g_floats[g_nfloats] = v;
	return g_nfloats++;
}

static i32 add_str(const char *p, u32 len)
{
	char *d = (char *)g_text.base + g_text.used;
	if (g_nstrs == MAX_CONSTS) die("too many strings");
	if (g_text.cap - g_text.used < (size_t)len + 1) die("out of memory in the text arena");
	memcpy(d, p, len);
	d[len] = 0;
	g_strs[g_nstrs].p = d;
	g_strs[g_nstrs].len = len;
	g_strs[g_nstrs].cell = str_cell(g_text.used, len);
	g_text.used += (size_t)len + 1;
	return g_nstrs++;
}

/* find_local NAME STAMP: the innermost local bound under NAME by a symbol with this STAMP */
static i32 find_local(const char *name, u32 stamp)
{
	i32 i;
	for (i = g_nlocals - 1; i >= g_local_floor; i--) {
		if (g_locals[i].stamp == stamp && !strcmp(g_locals[i].name, name)) return i;
	}
	return -1;
}

static i32 find_global(const char *name)
{
	const NameSlot *sl = names_get(name);
	return sl ? sl->global : -1;
}

static i32 find_fn(const char *name)
{
	const NameSlot *sl = names_get(name);
	return sl ? sl->fn : -1;
}

/* names_rebuild: the name index from the tables - at the start, and after a failed REPL unit took names back. The
 * functions the compiler makes (a fn's, named fn@line:col, and a field's default, Type@field) are not looked up by name.
 * Later entries win, so the REPL's newest macro does. */
static o names_rebuild(void)
{
	size_t k;
	i32 i;
	names_clear();
	for (k = 0; k < GAEL_COUNT(g_reserved); k++) names_put(g_reserved[k])->reserved = 1;
	for (k = 0; k < GAEL_COUNT(g_builtins); k++) names_put(g_builtins[k].name)->reserved = 1;
	for (i = T_UNIT; i < g_ntys; i++) {
		if (i == T_NONE || i == T_ANY || g_tys[i].kind >= K_OPTION) continue;
		names_put(g_tys[i].name)->type = i;
	}
	for (i = 0; i < g_nfns; i++) {
		if (!strchr(g_fns[i].name, '@')) names_put(g_fns[i].name)->fn = i;
	}
	for (i = 0; i < g_nglobals; i++) names_put(g_globals[i].name)->global = i;
	for (i = 0; i < g_nmacros; i++) names_put(g_macros[i].name)->macro = i;
}

static const Field *find_field(Ty st, const char *name, size_t len)
{
	const TyInfo *ti = &g_tys[st];
	i32 i;
	for (i = 0; i < ti->n; i++) {
		const Field *fd = &g_fields[ti->first + i];
		if (strlen(fd->name) == len && !memcmp(fd->name, name, len)) return fd;
	}
	return 0;
}

static i32 find_variant(Ty et, const char *name, size_t len)
{
	const TyInfo *ti = &g_tys[et];
	i32 i;
	for (i = 0; i < ti->n; i++) {
		const char *v = g_variants[ti->first + i];
		if (strlen(v) == len && !memcmp(v, name, len)) return i;
	}
	return -1;
}

/* names_of TYPE BUF SIZE: "x, y" - a struct's fields or an enum's variants, for a message */
static const char *names_of(Ty t, char *buf, size_t size)
{
	const TyInfo *ti = &g_tys[t];
	size_t used = 0;
	i32 i;
	strcpy(buf, ti->n ? "" : "none");
	for (i = 0; i < ti->n; i++) {
		const char *s = ti->kind == K_STRUCT ? g_fields[ti->first + i].name : g_variants[ti->first + i];
		size_t len = strlen(s);
		if (used + len + 8 > size) {
			strcpy(buf + used, i ? ", ..." : "...");
			break;
		}
		if (i) {
			strcpy(buf + used, ", ");
			used += 2;
		}
		memcpy(buf + used, s, len + 1);
		used += len;
	}
	return buf;
}

static i32 new_slots(i32 n)
{
	i32 s = g_slots;
	g_slots += n;
	if (g_slots > g_maxslots) g_maxslots = g_slots;
	return s;
}

/* add_local NODE KIND NAME STAMP TYPE SLOT: a local bound by the name NODE (-1: none) */
static o add_local(i32 node, u8 kind, const char *name, u32 stamp, Ty type, i32 slot)
{
	Local *lc;
	if (g_nlocals == MAX_LOCALS) die("too many local names in scope (MAX_LOCALS)");
	lc = &g_locals[g_nlocals++];
	lc->name = name;
	lc->type = type;
	lc->slot = slot;
	lc->stamp = stamp;
	lc->alias = 0;
	lc->kind = kind;
	lc->used = kind == LK_PARAM;
	lc->set = 0;
	lc->node = node;
	lc->errs = g_unit.nerrors;
}

/* locals_end MARK: the scope of the locals from MARK on ends. One that was never read is a warning, unless its name
 * starts with _, a macro made it, or an error since it was bound may have hidden its use */
static o locals_end(i32 mark)
{
	i32 i;
	for (i = mark; i < g_nlocals; i++) {
		const Local *lc = &g_locals[i];
		const char *s = lc->name;
		if (lc->used || lc->node < 0 || lc->stamp || !s[0] || s[0] == '_' || lc->errs != g_unit.nerrors) continue;
		if (lc->kind == LK_FOR)
			warn_node(lc->node, NAME_FMT " is never used; write _ in its place when only the repeating matters, like (for (_ 0 3) ...)", s);
		else if (lc->kind == LK_SOME)
			warn_node(lc->node, NAME_FMT " is never used; write (some _) when only the some matters", s);
		else if (lc->set)
			warn_node(lc->node, NAME_FMT " is given values but never read; remove it, or call it _" NAME_FMT " if it must be there", s, s);
		else
			warn_node(lc->node, NAME_FMT " is never used; remove it, or call it _" NAME_FMT " if it must be there", s, s);
	}
	g_nlocals = mark;
}

/* the function being compiled: its first work frame (-1 at the top level) and its return type */
static i32 g_fn_base = -1;
static Ty g_fn_ret;
static i32 g_fn_tbase; /* g_types' height when the function's body began: values above it are the function's own */
static i32 g_fn_index = -1; /* the named function whose body it is, -1 at the top level; a fn inside it is part of it */

/* tail_ok FRAME RET: a call in FRAME returning RET can be a tail call */
static int tail_ok(const Frame *f, Ty ret) { return f->tail && g_fn_base >= 0 && ret == g_fn_ret && ret != T_ERR; }

/* emit_tail_releases LINE: before a tail call or a return - what the function's scopes would release on the way out:
 * its parameters and locals, the values the matches around hold, what the loops around go over, a function value
 * waiting in a slot for its arguments. The frame on top (the call, or the return) holds nothing of its own */
static o emit_tail_releases(i32 line)
{
	i32 i;
	for (i = g_nlocals - 1; i >= g_local_floor; i--) {
		if (!g_locals[i].alias) emit_release(AT_LOCAL, g_locals[i].slot, g_locals[i].type, line);
	}
	for (i = g_nwork - 2; i >= g_fn_base; i--) {
		const Frame *w = &g_work[i];
		if (w->form == F_MATCH && w->phase >= 3) emit_release(AT_LOCAL, w->a, w->rt, line);
		else if (w->form == F_FOR && w->op != 1 && w->phase >= 3) emit_release(AT_LOCAL, w->a, w->res, line);
		else if (w->form == F_CALLV && w->op == 3 && w->phase <= 1) emit_release(AT_LOCAL, w->c, w->rt, line);
	}
}

/* emit_ret TYPE LINE: return the value of TYPE on top (unit: nothing) */
static o emit_ret(Ty rt, i32 line)
{
	i32 nc = cells(rt);
	if (nc == 0) emit(OP_RET_U, 0, 0, line);
	else if (nc == 1) emit(OP_RET_V, 0, 0, line);
	else emit(OP_RET_N, nc, 0, line);
}

/* return_blocked: why a return cannot be compiled here, 0 when it can: it releases what the function's scopes hold,
 * so nothing may be half done between it and the function's body - values waiting on the stack for the rest of an
 * expression, a struct being filled by name, an arena that would not end */
static const char *return_blocked(void)
{
	i32 i;
	for (i = g_nwork - 2; i >= g_fn_base; i--) {
		if (g_work[i].form == F_ARENA) return "return cannot leave an arena; let the arena end, then return";
		if (g_work[i].form == F_NEW && g_work[i].op && g_work[i].phase >= 1) return "return cannot be among the fields of a struct being made";
	}
	if (g_depth > 0) /* the operand stack of the function holds values for the rest of an expression */
		return "return cannot be in the middle of an expression with values already worked out; put it where a statement goes, like (when c (return x))";
	return 0;
}

static Frame *push_frame(i32 node, Form form)
{
	Frame *f;
	if (g_nwork == MAX_WORK) die("expression nested too deep (work stack)");
	f = &g_work[g_nwork++];
	memset(f, 0, sizeof(*f));
	f->node = node;
	f->form = (u8)form;
	f->cur = -1;
	f->rt = f->want = f->res = T_NONE;
	return f;
}

/* push_child NODE WANT: compile NODE next; WANT is the type the parent expects */
static o push_child(i32 node, Ty want) { push_frame(node, F_START)->want = want; }

/* push_cond NODE WHEN LIST CTX: compile NODE as a condition - no value: a jump when it is false (WHEN 0) or true (WHEN
 * 1), put on g_fjumps (LIST 0) or g_tjumps (LIST 1) for the code around to aim, and on to what follows otherwise. and,
 * or, not, nand and nor become jumps alone, as the short circuit goes; anything else is its value and one jump. The
 * jumps a condition needs inside itself it aims itself, so a list holds only what the code around waits for. CTX words
 * a type error. */
static o push_cond(i32 node, int when, int list, int ctx)
{
	Frame *f = push_frame(node, F_COND);
	f->op = (u8)when;
	f->b = list;
	f->c = ctx;
}

/* aim_jumps LIST COUNT MARK TARGET: the jumps on LIST from MARK on go to TARGET, and leave the list */
static o aim_jumps(const i32 *list, i32 *n, i32 mark, i32 target)
{
	i32 i;
	for (i = mark; i < *n; i++) patch(list[i], target);
	*n = mark;
}

/* cond_operand AND_OR: the next operand of an and or an or that is a condition. Each one of an and jumps when it is
 * false, of an or when it is true; where the whole would jump too, its jump goes on the whole's list, otherwise
 * (an and jumping when true: a false operand only ends it) on the other list, aimed after the whole when it ends. The
 * last operand jumps as the whole does */
static o cond_operand(Frame *f)
{
	int when = next_of(f->cur) < 0 ? f->op : f->a == 'o';
	push_cond(f->cur, when, when == f->op ? f->b : !f->b, f->c);
}

static o add_jump(i32 *list, i32 *n, i32 at)
{
	if (*n == MAX_WORK) die("conditions nested too deep (MAX_WORK)");
	list[(*n)++] = at;
}

/* push_body FIRST KEEP WANT: compile the forms FIRST, FIRST.next, ... as one expression; with KEEP the last one's
 * value is the result (and gets WANT), otherwise every value is dropped and the result is unit */
static o push_body(i32 first, int keep, Ty want)
{
	Frame *f = push_frame(-1, F_DO);
	f->cur = first;
	f->keep = (u8)keep;
	f->want = want;
}

static o finish(Ty t)
{
	i32 n = g_work[g_nwork - 1].node;
	if (n >= 0 && t < 256) { /* the form's type, kept on it and on what it is a copy of */
		g_nodes[n].lty = (u8)t;
		if (g_nodes[n].src >= 0) g_nodes[g_nodes[n].src].lty = (u8)t;
	}
	push_type(t);
	g_nwork--;
}

/* pure_form NODE: the form only works out a value - a name, a literal, arithmetic, a comparison, a read - so when its
 * value is thrown away it did nothing. A call of a function may do something, so it is never pure (as in Go) */
static int pure_form(i32 n)
{
	static const char *const pure[] = {
		"+", "-", "*", "/", "%", "<", "<=", ">", ">=", "=", "!=", "bit-and", "bit-or", "bit-xor", "bit-not", "shl", "shr", "int", "float", "i8", "i16", "i32", "i64", "u8", "u16",
		"u32", "u64", "f32", "f64", "len", "get", "lookup", "has", "some", "format", "keys", "values", "vec", "map", ".",
		"fn", "done?", "coroutine-time", "parse-int", "parse-float", "substr", "byte", "find", "split", "join", "char",
		"trim", "getenv", "env", "args", "sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "exp", "ln", "floor",
		"ceil", "atan2", "pow"
	};
	static const char *const logic[] = { "and", "or", "not", "nand", "nor", "xor", "xnor" };
	static i32 st[4 * MAX_DEPTH];
	i32 sp = 0, head, x;
	size_t k;
	const char *s;
	st[sp++] = n;
	while (sp > 0) { /* every form here, and every operand of a logic one, must be pure */
		n = st[--sp];
		if (n < 0 || g_nodes[n].stamp) return 0; /* a macro wrote it: not the program's mistake */
		if (g_nodes[n].kind != N_LIST) continue;
		head = first_of(n);
		if (!is_sym(head)) return 0;
		s = g_nodes[head].text;
		for (k = 0; k < GAEL_COUNT(logic) && strcmp(s, logic[k]); k++) {}
		if (k < GAEL_COUNT(logic)) { /* (and (run "a") (run "b")) runs b when a succeeds, as bash's && does */
			for (x = next_of(head); x >= 0; x = next_of(x)) {
				if (sp == (i32)GAEL_COUNT(st)) return 0; /* too many to look at: say nothing */
				st[sp++] = x;
			}
			continue;
		}
		for (k = 0; k < GAEL_COUNT(pure) && strcmp(s, pure[k]); k++) {}
		if (k < GAEL_COUNT(pure)) continue;
		for (k = 0; k < GAEL_COUNT(g_soft_names) && strcmp(s, g_soft_names[k]); k++) {}
		if (k < GAEL_COUNT(g_soft_names) && find_fn(s) < 0 && find_macro(s) < 0 && find_global(s) < 0 &&
		    find_local(s, g_nodes[head].stamp) < 0)
			continue; /* min and the like, unless the program has its own of that name */
		return 0;
	}
	return 1;
}

/* thrown_away NODE: the warning for a pure form whose value nothing takes */
static o thrown_away(i32 n)
{
	char text[56];
	size_t at = 0;
	nprint(g_nodes, n, text, sizeof(text), &at);
	if (at == sizeof(text) - 1) strcpy(text + sizeof(text) - 5, "...");
	warn_node(n, "the value of %s is thrown away; use it, or remove it", text);
}

/* ends_flow NODE: nothing after this form in a body runs - it returns, or it ends the program */
static int ends_flow(i32 n) { return head_is(n, "return") || head_is(n, "exit"); }

static i32 line_of(Frame *f) { return f->node >= 0 ? g_nodes[f->node].line : (f->cur >= 0 ? g_nodes[f->cur].line : 0); }

static Op arith_op(u8 op, Ty t)
{
	int fl = is_float(t);
	switch (op) {
	case '+': return fl ? OP_ADD_F : OP_ADD_I;
	case '-': return fl ? OP_SUB_F : OP_SUB_I;
	case '*': return fl ? OP_MUL_F : OP_MUL_I;
	case '/': return fl ? OP_DIV_F : t == T_U64 ? OP_DIV_U : OP_DIV_I;
	default: return t == T_U64 ? OP_MOD_U : OP_MOD_I;
	}
}

/* emit_fit TYPE LINE: the value on top, made by 64-bit int or f64 arithmetic, back to the size of TYPE: an int
 * narrower than 64 bits wraps around, an f32 rounds */
static o emit_fit(Ty t, i32 line)
{
	if (is_int(t) && int_bits(t) < 64) emit(OP_WRAP, t, 0, line);
	else if (t == T_F32) emit(OP_F32, 0, 0, line);
}

/* emit_convert FROM TO LINE: (TO x) for an x of type FROM: ints wrap around to the new size, a float to an int stops
 * at run time when it does not fit */
static o emit_convert(Ty from, Ty to, i32 line)
{
	if (from == to) return;
	if (is_int(from) && is_int(to)) {
		emit_fit(to, line); /* to 64 bits: the same bits, int and u64 alike */
	} else if (is_int(from)) {
		emit(from == T_U64 ? OP_U2F : OP_I2F, 0, 0, line);
		emit_fit(to, line);
	} else if (is_int(to)) {
		emit(OP_F2I, to, 0, line);
	} else {
		emit_fit(to, line);
	}
}

/* int_range TYPE: "-128 to 127", for a message */
static const char *int_range(Ty t)
{
	static char buf[64];
	i32 bits = int_bits(t);
	if (t == T_U64) return "0 to 18446744073709551615";
	if (t == T_INT) return "-9223372036854775808 to 9223372036854775807";
	if (is_unsigned(t)) sprintf(buf, "0 to %lu", (unsigned long)(((u64)1 << bits) - 1));
	else sprintf(buf, "%ld to %ld", -(long)((u64)1 << (bits - 1)), (long)(((u64)1 << (bits - 1)) - 1));
	return buf;
}

/* int_fits TYPE NODE: the int literal NODE fits in TYPE */
static int int_fits(Ty t, const Node *nd)
{
	i32 bits = int_bits(t);
	if (nd->big) return t == T_U64;
	if (t == T_INT) return 1;
	if (t == T_U64) return nd->ival >= 0;
	if (is_unsigned(t)) return nd->ival >= 0 && (u64)nd->ival < (u64)1 << bits;
	return nd->ival >= -(i64)((u64)1 << (bits - 1)) && nd->ival < (i64)((u64)1 << (bits - 1));
}

/* quiet_type NODE: the type a number operand will have, when it can be told without compiling it - a variable or a
 * field of one, a conversion, a call - or T_NONE. A literal next to it takes that type */
static Ty quiet_type(i32 n)
{
	char base[256];
	const char *s, *dot, *p;
	Ty t = T_NONE;
	i32 i, head;
	if (n < 0) return T_NONE;
	if (g_nodes[n].kind == N_LIST && is_sym(head = g_nodes[n].first)) {
		t = find_named_type(g_nodes[head].text);
		if (numeric(t)) return t;
		if ((i = find_fn(g_nodes[head].text)) >= 0 && numeric(g_fns[i].ret)) return g_fns[i].ret;
		return T_NONE;
	}
	if (g_nodes[n].kind != N_SYM) return T_NONE;
	s = g_nodes[n].text;
	dot = strchr(s, '.');
	if ((size_t)(dot ? dot - s : (long)strlen(s)) >= sizeof(base)) return T_NONE;
	sprintf(base, "%.*s", (int)(dot ? dot - s : (long)strlen(s)), s);
	if ((i = find_local(base, g_nodes[n].stamp)) >= 0) t = g_locals[i].type;
	else if ((i = find_global(base)) >= 0) t = g_globals[i].type;
	for (p = dot; p && is_struct(t); p = strchr(p + 1, '.')) {
		const char *end = strchr(p + 1, '.');
		const Field *fd = find_field(t, p + 1, end ? (size_t)(end - p - 1) : strlen(p + 1));
		t = fd ? fd->type : (Ty)T_NONE;
	}
	return numeric(t) ? t : (Ty)T_NONE;
}

/* number_hint FIRST: the type of the first operand from FIRST on whose type quiet_type can tell, or T_NONE */
static Ty number_hint(i32 n)
{
	Ty t;
	for (; n >= 0; n = g_nodes[n].next) {
		if ((t = quiet_type(n)) != T_NONE) return t;
	}
	return T_NONE;
}

static const char *const g_cmp_names[] = { "<", "<=", ">", ">=", "=", "!=" };

static Op cmp_op(u8 which, Ty t)
{
	static const Op ints[] = { OP_LT_I, OP_LE_I, OP_GT_I, OP_GE_I, OP_EQ_I, OP_NE_I };
	static const Op floats[] = { OP_LT_F, OP_LE_F, OP_GT_F, OP_GE_F, OP_EQ_F, OP_NE_F };
	static const Op u64s[] = { OP_LT_U, OP_LE_U, OP_GT_U, OP_GE_U, OP_EQ_I, OP_NE_I };
	if (t == T_STR || t == T_PATH) return which == 4 ? OP_EQ_S : OP_NE_S;
	if (t == T_U64) return u64s[which]; /* narrower unsigned ints sit zero-extended, so they compare as ints */
	return is_float(t) ? floats[which] : ints[which]; /* bools and enum variants compare as ints */
}

/* fmt_type TEXT LEN K: the type the K-th % code of a format wants (T_ANY for %v); T_NONE when it has fewer codes.
 * With K = -1: returns T_ERR when the format has an unknown code, T_NONE otherwise; *count gets the number of codes */
static Ty fmt_type(const char *s, u32 n, i32 want, i32 *count)
{
	u32 i;
	i32 k = 0;
	for (i = 0; i < n; i++) {
		Ty t;
		if (s[i] != '%') continue;
		if (i + 1 == n) return want < 0 ? (Ty)T_ERR : (Ty)T_NONE;
		switch (s[++i]) {
		case '%': continue;
		case 'd': t = T_INT; break;
		case 'x': t = T_INT; break; /* an int in hex: 0xff8800ff prints ff8800ff */
		case 'f': t = T_FLOAT; break;
		case 's': t = T_STR; break;
		case 'b': t = T_BOOL; break;
		case 'v': t = T_ANY; break;
		default: return want < 0 ? (Ty)T_ERR : (Ty)T_NONE;
		}
		if (k == want) return t;
		k++;
	}
	if (count) *count = k;
	return T_NONE;
}

/* print, println and format, and the two that write to stderr */
static const char *const g_print_names[] = { "print", "println", "format", "eprint", "eprintln" };
static const Op g_print_ops[] = { OP_PRINT, OP_PRINTLN, OP_FORMAT, OP_PRINT, OP_PRINTLN };

/* the builtins on a map, and on a pool (lookup, has, remove) */
enum { MO_INSERT, MO_LOOKUP, MO_HAS, MO_REMOVE, MO_KEYS, MO_VALUES };
static const char *const g_mapop_names[] = { "insert", "lookup", "has", "remove", "keys", "values" };
static const char *const g_mapop_usage[] = { "(insert m key value)", "(lookup m key)", "(has m key)", "(remove m key)", "(keys m)", "(values m)" };
static const i32 g_mapop_args[] = { 3, 2, 2, 2, 1, 1 };
static const Op g_mapop_ops[] = { OP_MAP_INSERT, OP_MAP_LOOKUP, OP_MAP_HAS, OP_MAP_REMOVE, OP_MAP_KEYS, OP_MAP_VALUES };

/* the builtins on a vec, and on a pool (get, put, spawn) */
enum { VO_PUSH, VO_GET, VO_PUT, VO_POP, VO_SPAWN };
static const char *const g_vecop_names[] = { "push", "get", "put", "pop", "spawn" };
static const char *const g_vecop_usage[] = { "(push v x)", "(get v i)", "(put v i x)", "(pop v)", "(spawn p x)" };
static const i32 g_vecop_args[] = { 2, 2, 3, 1, 2 };

typedef struct {
	int where;
	i32 at;
	Ty type;
} Place;

#define MAX_LAMBDAS 64
#define MAX_CAPS 64
/* a local of the code around a fn that the fn uses: its closure keeps a copy, made when the fn is */
typedef struct {
	const char *name;
	u32 stamp;
	Ty type;
	i32 off;     /* cells into the closure's values */
	i32 src;     /* where the code around gets it: a slot of its frame, or (from_env) an offset in its own closure */
	u8 from_env;
} Capture;
static struct {
	i32 slots, maxslots, depth, maxdepth, nlocals, floor, jmp, fi, fn_base, fn_tbase;
	Ty fn_ret;
	i32 ncaps, envcells;
	Capture caps[MAX_CAPS];
} g_lambdas[MAX_LAMBDAS];
static i32 g_nlambdas;

/* cap_add LEVEL NAME STAMP TYPE FROM_ENV SRC: lambda LEVEL keeps one more value; its capture's index */
static i32 cap_add(i32 lv, const char *name, u32 stamp, Ty type, int from_env, i32 src)
{
	Capture *cp;
	if (g_lambdas[lv].ncaps == MAX_CAPS) die("a fn uses more than 64 locals of the code around it (MAX_CAPS)");
	cp = &g_lambdas[lv].caps[g_lambdas[lv].ncaps];
	cp->name = arena_strndup(&g_unit_arena, name, strlen(name)); /* NAME may be a caller's buffer */
	cp->stamp = stamp;
	cp->type = type;
	cp->from_env = (u8)from_env;
	cp->src = src;
	cp->off = g_lambdas[lv].envcells;
	g_lambdas[lv].envcells += cells(type);
	return g_lambdas[lv].ncaps++;
}

/* capture NAME STAMP: NAME is no local of the fn being compiled; when it is one of the code around it (around the fns
 * it sits in), the innermost fn keeps a copy in its closure, and so does each fn in between, to hand it in. The
 * capture's index in the innermost fn; -1 when no code around has such a local */
static i32 capture(const char *name, u32 stamp)
{
	i32 lv, c = -1, i;
	for (lv = g_nlambdas - 1; lv >= 0; lv--) {
		for (c = 0; c < g_lambdas[lv].ncaps && (g_lambdas[lv].caps[c].stamp != stamp || strcmp(g_lambdas[lv].caps[c].name, name)); c++) {}
		if (c < g_lambdas[lv].ncaps) break; /* fn lv keeps it already */
		for (i = g_lambdas[lv].nlocals - 1; i >= g_lambdas[lv].floor; i--) { /* the locals of the code around fn lv */
			if (g_locals[i].stamp == stamp && !strcmp(g_locals[i].name, name)) break;
		}
		if (i >= g_lambdas[lv].floor) {
			c = cap_add(lv, name, stamp, g_locals[i].type, 0, g_locals[i].slot);
			g_locals[i].used = 1; /* the closure copies it */
			break;
		}
	}
	if (lv < 0) return -1;
	for (lv++; lv < g_nlambdas; lv++) c = cap_add(lv, name, stamp, g_lambdas[lv - 1].caps[c].type, 1, g_lambdas[lv - 1].caps[c].off);
	return c;
}

static int g_place_set; /* resolve_place is finding the target of a set: not a read of the variable */

/* resolve_place NODE PLACE: where a variable or a field path like v.pos.x lives, and its type. 1 when found, 0 after
 * an error it reported, -1 when the first part is not a variable. Finding a local counts as reading it, but for set */
static int resolve_place(i32 n, Place *pl)
{
	const char *s = g_nodes[n].text, *dot = strchr(s, '.');
	size_t blen = dot ? (size_t)(dot - s) : strlen(s);
	char base[256];
	i32 i;
	if (blen >= sizeof(base)) return -1;
	memcpy(base, s, blen);
	base[blen] = 0;
	if ((i = find_local(base, g_nodes[n].stamp)) >= 0) {
		pl->where = AT_LOCAL;
		pl->at = g_locals[i].slot;
		pl->type = g_locals[i].type;
		if (g_place_set) g_locals[i].set = 1;
		else g_locals[i].used = 1;
	} else if ((i = capture(base, g_nodes[n].stamp)) >= 0) { /* a local of the code around: the closure keeps it */
		const Capture *cp = &g_lambdas[g_nlambdas - 1].caps[i];
		pl->where = AT_ENV;
		pl->at = cp->off;
		pl->type = cp->type;
	} else if ((i = find_global(base)) >= 0) {
		if (!gem_use(n, g_globals[i].gem)) return 0;
		pl->where = AT_GLOBAL;
		pl->at = g_globals[i].cell;
		pl->type = g_globals[i].type;
	} else {
		return -1;
	}
	while (dot) {
		const char *name = dot + 1, *end = strchr(name, '.');
		size_t len = end ? (size_t)(end - name) : strlen(name);
		const Field *fd;
		char names[256];
		if (pl->type == T_ERR) return 0; /* its declaration was wrong and said so */
		if (!len) {
			err_node(n, NAME_FMT ": a field name is missing after a dot", s);
			return 0;
		}
		if (!is_struct(pl->type)) {
			err_node(n, "%.*s is %s, it has no fields", (int)((size_t)(dot - s) < 64 ? (size_t)(dot - s) : 64), s, tn(pl->type));
			return 0;
		}
		fd = find_field(pl->type, name, len);
		if (!fd && g_tys[pl->type].broken) return 0;
		if (!fd) {
			err_node(n, "%s has no field %.*s; its fields are %s", tn(pl->type), (int)(len < 64 ? len : 64), name,
			         names_of(pl->type, names, sizeof(names)));
			return 0;
		}
		pl->at += fd->offset;
		pl->type = fd->type;
		dot = end;
	}
	return 1;
}

static o compile_atom(i32 n, Ty want)
{
	Node *nd = &g_nodes[n];
	const char *s = nd->text, *dot;
	Place pl;
	Ty t;
	int r;
	switch (nd->kind) {
	case N_INT: /* an int literal takes the int type expected here, when it fits */
		t = is_int(want) ? want : (Ty)T_INT;
		if (!int_fits(t, nd)) {
			if (nd->big && t != T_U64) err_node(n, NAME_FMT " does not fit in %s; it fits a u64", s, tn(t));
			else err_node(n, NAME_FMT " does not fit in %s (%s)", s, tn(t), int_range(t));
			push_type(T_ERR);
			return;
		}
		emit(OP_PUSH_I, add_int(nd->ival), 0, nd->line);
		nd->lty = (u8)t;
		push_type(t);
		return;
	case N_FLOAT: /* likewise for f32, rounded to it */
		if (want == T_F32) {
			if (nd->fval > 3.4028234663852886e38 || nd->fval < -3.4028234663852886e38) {
				err_node(n, NAME_FMT " does not fit in f32 (its largest is about 3.4e38)", s);
				push_type(T_ERR);
				return;
			}
			emit(OP_PUSH_F, add_float((f64)(f32)nd->fval), 0, nd->line);
			nd->lty = T_F32;
			push_type(T_F32);
			return;
		}
		emit(OP_PUSH_F, add_float(nd->fval), 0, nd->line);
		nd->lty = T_FLOAT;
		push_type(T_FLOAT);
		return;
	case N_STR: emit(OP_PUSH_S, add_str(nd->text, nd->len), 0, nd->line); push_type(T_STR); return;
	case N_KEY: err_node(n, "a keyword like " NAME_FMT " names a field: (Vec2 :x 1.0 :y 2.0)", s); push_type(T_ERR); return;
	default: break;
	}
	if (!strcmp(s, "true") || !strcmp(s, "false")) {
		emit(OP_PUSH_K, s[0] == 't', 0, nd->line);
		push_type(T_BOOL);
		return;
	}
	if (!strcmp(s, "none")) {
		if (is_option(want)) {
			emit(OP_PUSH_K, 0, 0, nd->line);
			if (cells(g_tys[want].elem)) emit(OP_ZERO, cells(g_tys[want].elem), 0, nd->line);
			push_type(want);
			return;
		}
		if (want == T_NONE || want == T_UNIT)
			err_node(n, "none needs to know its option type here; give it one, like (let ((x : (option int) none)) ...)");
		else if (want != T_ERR)
			err_node(n, "none is an option, but %s is expected here", tn(want));
		push_type(T_ERR);
		return;
	}
	if (!strcmp(s, "_")) {
		err_node(n, "_ stands for no name - a match pattern, or the variable of a for that only repeats - so it cannot be read");
		push_type(T_ERR);
		return;
	}
	if (s[0] == '@') {
		err_node(n, NAME_FMT " is an annotation: it goes before a top-level def, like @export (def speed : float 10.0)", s);
		push_type(T_ERR);
		return;
	}
	if (s[0] == '$') { /* $Player/Sprite: a node path, as in GDScript */
		if (!s[1] || s[1] == '$') {
			err_node(n, NAME_FMT " is not a node path, which is $Name or $Name/Child; the process id is (pid)", s);
			push_type(T_ERR);
			return;
		}
		emit(OP_PUSH_S, add_str(s + 1, nd->len - 1), 0, nd->line);
		push_type(T_PATH);
		return;
	}
	pl.where = AT_LOCAL;
	pl.at = 0;
	pl.type = T_ERR;
	r = resolve_place(n, &pl);
	if (r > 0) {
		if (pl.type < 256) nd->lty = (u8)pl.type;
		emit_copy(pl.where, pl.at, pl.type, nd->line);
		push_type(pl.type);
		return;
	}
	if (r == 0) {
		push_type(T_ERR);
		return;
	}
	dot = strchr(s, '.');
	if (dot && (size_t)(dot - s) < 256 && find_fn(s) < 0) { /* Dir.north; Vec2.len is a function, below */
		char base[256], names[256];
		i32 v;
		memcpy(base, s, (size_t)(dot - s));
		base[dot - s] = 0;
		t = find_named_type(base);
		if (is_enum(t) && (v = find_variant(t, dot + 1, strlen(dot + 1))) >= 0) {
			if (!gem_use(n, g_tys[t].gem)) { push_type(T_ERR); return; }
			emit(OP_PUSH_K, v, 0, nd->line);
			push_type(t);
			return;
		}
		if (is_enum(t) && !g_tys[t].broken)
			err_node(n, "%s has no variant " NAME_FMT "; its variants are %s", tn(t), dot + 1, names_of(t, names, sizeof(names)));
		else if (t != T_NONE && !is_enum(t))
			err_node(n, "%s is a type, not a variable", tn(t));
		if (t != T_NONE) {
			push_type(T_ERR);
			return;
		}
	}
	t = find_named_type(s);
	if ((r = find_fn(s)) >= 0) { /* one namespace: a function's name is a value too */
		if (!gem_use(n, g_fns[r].gem)) { push_type(T_ERR); return; }
		emit(OP_PUSH_K, -(r + 1), 0, nd->line); /* a function as a value: minus (its number + 1); a closure is a handle */
		push_type(intern_fn(g_fns[r].params, g_fns[r].nparams, g_fns[r].ret));
		return;
	}
	for (r = g_local_floor - 1; r >= 0 && (g_locals[r].stamp != nd->stamp || strcmp(g_locals[r].name, s)); r--) {}
	if (r >= 0) err_node(n, NAME_FMT " is a local of the code around this fn; a fn cannot use those yet, pass it in", s);
	else if (is_struct(t)) err_node(n, NAME_FMT " is a type; make one with (" NAME_FMT " ...)", s, s);
	else if (is_enum(t) && g_tys[t].n) err_node(n, NAME_FMT " is a type; a value of it is written " NAME_FMT ".%s", s, s, g_variants[g_tys[t].first]);
	else if (t != T_NONE) err_node(n, NAME_FMT " is a type, not a value", s);
	else err_node(n, "unknown name " NAME_FMT, s);
	push_type(T_ERR);
}

/* thread_steps CHAIN: (chain x (f a) g) becomes (g (f x a)) - each step gets the value so far as its first argument, the
 * way a bash pipe hands its output on. The nodes are relinked in place; returns the call that comes out, or -1 */
static i32 thread_steps(i32 n)
{
	i32 cur = next_of(first_of(n)), step = next_of(cur), next;
	if (cur < 0 || step < 0) {
		err_node(n, "chain takes a value and at least one step: (chain x (f a) g)");
		return -1;
	}
	for (; step >= 0; step = next) {
		next = next_of(step);
		if (is_sym(step)) { /* g: (g x) */
			i32 call = new_node(N_LIST, g_nodes[step].line, g_nodes[step].col);
			g_nodes[call].first = step;
			g_nodes[step].next = cur;
			g_nodes[cur].next = -1;
			cur = call;
		} else if (g_nodes[step].kind == N_LIST && is_sym(first_of(step))) { /* (f a): (f x a) */
			i32 head = first_of(step);
			g_nodes[cur].next = g_nodes[head].next;
			g_nodes[head].next = cur;
			cur = step;
		} else {
			err_node(step, "a step of chain is a function name or a call without its first value, like (split \"\\n\")");
			return -1;
		}
	}
	g_nodes[cur].next = -1;
	return cur;
}

/* hook TYPE NAME: the function TYPE.NAME of a struct or an enum - what a form like + or len calls for its values; -1
 * when the type has none */
static i32 hook(Ty t, const char *name)
{
	char buf[200];
	if (!is_struct(t) && !is_enum(t)) return -1;
	sprintf(buf, "%.120s.%.60s", g_tys[t].name, name);
	return find_fn(buf);
}

/* arith_hook FRAME TYPE: the function that + - * / % call when the value on their left is of TYPE, a struct or an
 * enum - its add, sub, mul, div or rem, taking that value and the next one - or that (- x) calls, its negate; -1
 * after an error it reported */
static i32 arith_hook(Frame *f, Ty t)
{
	static const char ops[] = "+-*/%";
	static const char *const names[] = { "add", "sub", "mul", "div", "rem" };
	int unary = f->k == 1;
	const char *name = unary ? "negate" : names[strchr(ops, f->op) - ops];
	i32 fi = hook(t, name), at = first_of(f->node);
	const Fn *fn = fi >= 0 ? &g_fns[fi] : 0;
	if (!fn) {
		err_node(at, "%c on %s calls %s.%s, which is not there: (defn %s.%s ((a : %s)%s) : %s ...)", f->op, tn(t), tn(t), name, tn(t), name,
		         tn(t), unary ? "" : " (b : ...)", tn(t));
		return -1;
	}
	if (fn->nparams != (unary ? 1 : 2) || fn->params[0] != t || fn->ret == T_UNIT) {
		err_node(at, "%c on %s calls %s, which must take %s and return a value", f->op, tn(t), fn->name,
		         unary ? "one value, of that type," : "two values, the first of that type,");
		return -1;
	}
	return gem_use(at, fn->gem) ? fi : -1;
}

/* cmp_hook FRAME TYPE LINE: both values of a comparison are of TYPE, a struct or an enum, and on the stack: = and !=
 * call its equal, else its compare; < <= > >= call its compare, whose int is below 0 when the first comes first, 0
 * when they are equal and above 0 otherwise. 0 when it has neither and = compares an enum's values themselves */
/* min, max, abs and clamp are built in but not reserved: a variable, a field, a macro or a function of that name comes
 * first, so (let ((max 0)) ...) and (struct Rect (min : Vec2) (max : Vec2)) stay fine. Each use is rewritten into an
 * if over < (soft_form, below), so they work on whatever < orders: numbers of one type, or a type with a compare hook */

static i32 soft_of(const Frame *f) /* the soft form that wrote this < or if, -1 when the program did */
{
	i32 h = first_of(f->node);
	return h >= 0 && g_nodes[h].kind == N_SYM && g_nodes[h].ival >= SOFT_MARK && g_nodes[h].ival < SOFT_MARK + (i64)GAEL_COUNT(g_soft_names)
	       ? (i32)(g_nodes[h].ival - SOFT_MARK) : -1;
}

static const char *op_name(const Frame *f, const char *name) /* the name a message gives the comparison */
{
	return soft_of(f) >= 0 ? g_soft_names[soft_of(f)] : name;
}

static int cmp_hook(Frame *f, Ty t, i32 line)
{
	i32 eq = f->op >= 4 ? hook(t, "equal") : -1, cmp = eq < 0 ? hook(t, "compare") : -1, at = first_of(f->node), fi = eq >= 0 ? eq : cmp;
	const char *what = op_name(f, g_cmp_names[f->op]);
	if (fi < 0) {
		if (is_enum(t) && f->op >= 4) return 0;
		if (f->op >= 4)
			err_node(at, "%s on %s calls %s.equal, or %s.compare, and neither is there: (defn %s.equal ((a : %s) (b : %s)) : bool ...)", what, tn(t),
			         tn(t), tn(t), tn(t), tn(t), tn(t));
		else
			err_node(at, "%s on %s calls %s.compare, which is not there: (defn %s.compare ((a : %s) (b : %s)) : int ...), below 0 when a comes first",
			         what, tn(t), tn(t), tn(t), tn(t), tn(t));
		return 1;
	}
	if (g_fns[fi].nparams != 2 || g_fns[fi].params[0] != t || g_fns[fi].params[1] != t || g_fns[fi].ret != (eq >= 0 ? T_BOOL : T_INT)) {
		err_node(at, "%s on %s calls %s, which must take two %s and return %s", what, tn(t), g_fns[fi].name, tn(t), eq >= 0 ? "bool" : "int");
		return 1;
	}
	if (!gem_use(at, g_fns[fi].gem)) return 1;
	emit(OP_CALL, fi, 0, line);
	if (eq >= 0) {
		if (f->op == 5) emit(OP_NOT, 0, 0, line);
		return 1;
	}
	emit(OP_PUSH_I, add_int(0), 0, line);
	emit(cmp_op(f->op, T_INT), 0, 0, line);
	return 1;
}

/* own_fn TYPE: the code being compiled is one of TYPE's own functions (Type.name, or a field's default), where
 * (Type ...) makes the struct itself instead of calling Type.init */
static int own_fn(Ty t)
{
	const char *fname, *tname = g_tys[t].name;
	size_t len = strlen(tname);
	if (g_fn_index < 0) return 0;
	fname = g_fns[g_fn_index].name;
	return !strncmp(fname, tname, len) && (fname[len] == '.' || fname[len] == '@');
}

/* method_word NAME: a builtin or a function a method call can stand for: (v.push x) is (push v x) */
static int method_word(const char *m)
{
	size_t k;
	for (k = 0; k < GAEL_COUNT(g_builtins); k++) if (!strcmp(m, g_builtins[k].name)) return 1;
	for (k = 0; k < GAEL_COUNT(g_vecop_names); k++) if (!strcmp(m, g_vecop_names[k])) return 1;
	for (k = 0; k < GAEL_COUNT(g_mapop_names); k++) if (!strcmp(m, g_mapop_names[k])) return 1;
	return !strcmp(m, "poll") || !strcmp(m, "done?") || find_fn(m) >= 0;
}

#define VALUE_NAME "the value" /* the local that a method on an expression's value is called on: ((get ps 0).hurt 3) */

/* method_call FRAME HEAD: (v.m args...) where v (or a field path like v.pos) is a variable and m none of its fields.
 * It stands for (T.m v args...) when the type T of v has a function m, else for (m v args...) with a builtin or a
 * function m, and the frame starts over on that call. 1 when it rewrote the call or reported an error, 0 when HEAD is
 * something else (Vec2.len, a field holding a function) */
static int method_call(Frame *f, i32 head)
{
	const char *s = g_nodes[head].text, *dot = strrchr(s, '.'), *m;
	char buf[300], fields[256];
	i32 recv, call, mh, fi = -1, line = g_nodes[head].line, col = g_nodes[head].col;
	Place pl;
	int r;
	Ty t;
	if (!dot || dot == s || !dot[1] || (size_t)(dot - s) >= 256) return 0;
	m = dot + 1;
	recv = new_node(N_SYM, line, col); /* v, or v.pos: the first argument */
	g_nodes[recv].text = arena_strndup(&g_unit_arena, s, (size_t)(dot - s));
	g_nodes[recv].len = (u32)(dot - s);
	g_nodes[recv].stamp = g_nodes[head].stamp;
	pl.where = AT_LOCAL;
	pl.at = 0;
	pl.type = T_ERR;
	r = resolve_place(recv, &pl);
	if (r < 0) return 0;
	t = pl.type;
	if (r > 0 && is_struct(t) && find_field(t, m, strlen(m))) return 0; /* a field: called as a function value, or wrong */
	if (r == 0 || t == T_ERR) { finish(T_ERR); return 1; }
	if ((fi = hook(t, m)) < 0 && !method_word(m)) {
		if (is_struct(t))
			err_node(head, "%.64s is %s; it has no field %.64s and %s has no function %.64s; its fields are %s", g_nodes[recv].text, tn(t), m,
			         tn(t), m, names_of(t, fields, sizeof(fields)));
		else if (is_enum(t))
			err_node(head, "%.64s is %s, which has no function %.64s", g_nodes[recv].text, tn(t), m);
		else
			err_node(head, "%.64s is %s; (%.64s.%.64s) stands for (%.64s %.64s), and there is no function %.64s", g_nodes[recv].text, tn(t),
			         g_nodes[recv].text, m, m, g_nodes[recv].text, m);
		finish(T_ERR);
		return 1;
	}
	if (fi >= 0 && f->want == T_UNIT && g_fns[fi].ret == t && g_fns[fi].nparams > 0 && g_fns[fi].params[0] == t) {
		if (!strncmp(s, VALUE_NAME ".", sizeof(VALUE_NAME))) /* ((get ps 0).heal 1.0): no variable to set */
			err_node(head, "%s gives a new %s and this drops it; keep what it gives in a variable, or pass it on", g_fns[fi].name, tn(t));
		else
			err_node(head, "%s gives a new %s and this drops it; it cannot change %.64s, which it got a copy of, so keep what it gives: (set %.64s (%.64s ...))",
			         g_fns[fi].name, tn(t), g_nodes[recv].text, g_nodes[recv].text, s);
		finish(T_ERR);
		return 1;
	}
	mh = new_node(N_SYM, line, col);
	if (fi >= 0) {
		sprintf(buf, "%.140s", g_fns[fi].name);
		g_nodes[mh].stamp = g_nodes[head].stamp;
	} else {
		sprintf(buf, "%.140s", m);
		g_nodes[mh].stamp = ++g_stamps; /* no local sees it: m is the builtin or the function, not a variable */
	}
	g_nodes[mh].text = arena_strndup(&g_unit_arena, buf, strlen(buf));
	g_nodes[mh].len = (u32)strlen(buf);
	call = new_node(N_LIST, g_nodes[f->node].line, g_nodes[f->node].col);
	g_nodes[call].first = mh;
	g_nodes[mh].next = recv;
	g_nodes[recv].next = next_of(head);
	f->node = call;
	f->cur = -1;
	f->k = 0;
	return 1;
}

/* method_on_value CALL E PATH: ((. e path) args...), a method or a function field of an expression's value, as
 * (let ((the value e)) (the value.path args...)) - the local's stamp is its own, so no name of the program sees it and
 * it sees none of theirs; method_call then does the rest. The tree of CALL is left as it is, for gael -m */
static i32 method_on_value(i32 call, i32 e, i32 path)
{
	u32 stamp = ++g_stamps;
	i32 line = g_nodes[call].line, col = g_nodes[call].col;
	i32 let = new_node(N_LIST, line, col), lh = new_node(N_SYM, line, col), binds = new_node(N_LIST, line, col);
	i32 bind = new_node(N_LIST, line, col), name = new_node(N_SYM, line, col), value = new_node(N_LIST, line, col);
	i32 body = new_node(N_LIST, line, col), mh = new_node(N_SYM, g_nodes[path].line, g_nodes[path].col);
	char buf[300];
	g_nodes[value] = g_nodes[e]; /* e itself, without the path after it */
	g_nodes[value].src = g_nodes[e].src >= 0 ? g_nodes[e].src : e;
	g_nodes[value].next = -1;
	g_nodes[lh].text = "let";
	g_nodes[lh].len = 3;
	g_nodes[name].text = VALUE_NAME;
	g_nodes[name].len = (u32)strlen(VALUE_NAME);
	g_nodes[name].stamp = stamp;
	sprintf(buf, VALUE_NAME ".%.250s", g_nodes[path].text);
	g_nodes[mh].text = arena_strndup(&g_unit_arena, buf, strlen(buf));
	g_nodes[mh].len = (u32)strlen(buf);
	g_nodes[mh].stamp = stamp;
	g_nodes[let].first = lh;
	g_nodes[lh].next = binds;
	g_nodes[binds].first = bind;
	g_nodes[bind].first = name;
	g_nodes[name].next = value;
	g_nodes[binds].next = body;
	g_nodes[body].first = mh;
	g_nodes[mh].next = next_of(first_of(call)); /* the arguments */
	return let;
}

typedef struct {
	i32 node;  /* the value as written */
	u32 stamp; /* the stamp of the local that holds it, 0 when it is used as written (a name or a literal) */
} Opnd;

static i32 node_copy(i32 n) /* N on its own: the same node with its siblings cut off; a list keeps its children */
{
	i32 c = new_node((NodeKind)g_nodes[n].kind, g_nodes[n].line, g_nodes[n].col);
	g_nodes[c] = g_nodes[n];
	g_nodes[c].next = -1;
	g_nodes[c].src = g_nodes[n].src >= 0 ? g_nodes[n].src : n;
	return c;
}

static i32 mk_sym(const char *text, i32 at, u32 stamp) /* a symbol where node AT is */
{
	i32 s = new_node(N_SYM, g_nodes[at].line, g_nodes[at].col);
	g_nodes[s].text = text;
	g_nodes[s].len = (u32)strlen(text);
	g_nodes[s].stamp = stamp;
	if (!strcmp(text, "<") || !strcmp(text, "if") || !strcmp(text, "-")) g_nodes[s].ival = SOFT_MARK + g_soft_k; /* messages name the form */
	return s;
}

static i32 mk_list(i32 at, i32 a, i32 b, i32 c, i32 d) /* (a b c d) where node AT is; -1 ends it early */
{
	i32 l = new_node(N_LIST, g_nodes[at].line, g_nodes[at].col), k[4], i, last = -1;
	k[0] = a;
	k[1] = b;
	k[2] = c;
	k[3] = d;
	for (i = 0; i < 4 && k[i] >= 0; i++) {
		if (last < 0) g_nodes[l].first = k[i];
		else g_nodes[last].next = k[i];
		last = k[i];
	}
	return l;
}

static Opnd opnd(i32 n) /* a name or a literal is read where it is needed, anything else once, into a local */
{
	Opnd o;
	o.node = n;
	o.stamp = g_nodes[n].kind == N_LIST || g_nodes[n].kind == N_STR ? ++g_stamps : 0;
	return o;
}

static i32 opnd_use(Opnd o) { return o.stamp ? mk_sym(VALUE_NAME, o.node, o.stamp) : node_copy(o.node); }
static int opnd_lit(Opnd o) { return g_nodes[o.node].kind == N_INT || g_nodes[o.node].kind == N_FLOAT; }

/* pick AT MAX A B: (if (< b a) b a) for min, (if (< a b) b a) for max, a on a tie - with a literal in the else, where
 * it takes the type of the other side, as it would in (< x 255) - inside a let for the operands that need a local */
static i32 pick(i32 at, int max, Opnd a, Opnd b)
{
	i32 cond, iff, binds = -1, last = -1;
	Opnd o[2];
	int k;
	if (opnd_lit(a) && !opnd_lit(b)) { /* the literal goes last, so that the if takes the other one's type */
		Opnd t = a;
		a = b;
		b = t;
	}
	if (opnd_lit(b) && !opnd_lit(a)) /* min: (if (< a 9) a 9), max: (if (< 9 a) a 9) */
		cond = max ? mk_list(at, mk_sym("<", at, 0), opnd_use(b), opnd_use(a), -1) : mk_list(at, mk_sym("<", at, 0), opnd_use(a), opnd_use(b), -1);
	else /* min: (if (< b a) b a), max: (if (< a b) b a) */
		cond = max ? mk_list(at, mk_sym("<", at, 0), opnd_use(a), opnd_use(b), -1) : mk_list(at, mk_sym("<", at, 0), opnd_use(b), opnd_use(a), -1);
	if (opnd_lit(b) && !opnd_lit(a)) iff = mk_list(at, mk_sym("if", at, 0), cond, opnd_use(a), opnd_use(b));
	else iff = mk_list(at, mk_sym("if", at, 0), cond, opnd_use(b), opnd_use(a));
	o[0] = a;
	o[1] = b;
	for (k = 0; k < 2; k++) { /* (let ((the value a) (the value b)) iff), with one local each that needs it */
		i32 bind;
		if (!o[k].stamp) continue;
		bind = mk_list(at, mk_sym(VALUE_NAME, at, o[k].stamp), node_copy(o[k].node), -1, -1);
		if (last < 0) binds = mk_list(at, bind, -1, -1, -1);
		else g_nodes[last].next = bind;
		last = bind;
	}
	return binds < 0 ? iff : mk_list(at, mk_sym("let", at, 0), binds, iff, -1);
}

/* soft_form CALL NAME: (min a b ...), (max a b ...), (abs x), (clamp x lo hi) as the if they stand for; -1 when NAME is
 * none of them, -2 after an error it reported */
static i32 soft_form(i32 call, const char *s)
{
	i32 arg = next_of(first_of(call)), n = count_from(arg), x;
	size_t k;
	Opnd v;
	for (k = 0; k < GAEL_COUNT(g_soft_names) && strcmp(s, g_soft_names[k]); k++) {}
	if (k == GAEL_COUNT(g_soft_names)) return -1;
	g_soft_k = (i32)k;
	if (k < 2) { /* min, max: two or more values, folded from the left */
		if (n < 2) { err_node(call, "%s takes two or more values: (%s a b)", s, s); return -2; }
		v = opnd(arg);
		for (arg = next_of(arg); arg >= 0; arg = next_of(arg)) {
			x = pick(call, (int)k, v, opnd(arg));
			v = opnd(x);
		}
		return v.node;
	}
	if (k == 2) { /* abs: (if (< x (- x)) (- x) x) */
		i32 neg, iff;
		if (n != 1) { err_node(call, "abs takes one value: (abs x)"); return -2; }
		v = opnd(arg);
		neg = mk_list(call, mk_sym("-", call, 0), opnd_use(v), -1, -1);
		iff = mk_list(call, mk_sym("if", call, 0), mk_list(call, mk_sym("<", call, 0), opnd_use(v), neg, -1),
		              mk_list(call, mk_sym("-", call, 0), opnd_use(v), -1, -1), opnd_use(v));
		if (!v.stamp) return iff;
		return mk_list(call, mk_sym("let", call, 0), mk_list(call, mk_list(call, mk_sym(VALUE_NAME, call, v.stamp), node_copy(arg), -1, -1), -1, -1, -1),
		               iff, -1);
	}
	if (n != 3) { err_node(call, "clamp takes a value and its bounds: (clamp x lo hi)"); return -2; } /* (min (max x lo) hi) */
	x = pick(call, 1, opnd(arg), opnd(next_of(arg)));
	return pick(call, 0, opnd(x), opnd(next_of(next_of(arg))));
}

static int gpu_each(Frame *f); /* kernel.c */
static o kernel_text(i32 k);

/* start_list FRAME: decide which form a list is, check its shape, and start it */
/* mark_note NODE: (@warn "text") or (@err "text") - recorded for the tools, no code */
static o mark_note(i32 n)
{
	i32 head = first_of(n), text = next_of(head);
	if (text < 0 || g_nodes[text].kind != N_STR || next_of(text) >= 0) {
		err_node(n, NAME_FMT " takes one string: (" NAME_FMT " \"what to look at\")", g_nodes[head].text, g_nodes[head].text);
		return;
	}
	if (g_nmarks == MAX_MARKS) return;
	g_marks[g_nmarks].file = g_unit.file;
	g_marks[g_nmarks].line = g_nodes[n].line;
	g_marks[g_nmarks].err = g_nodes[head].text[1] == 'e';
	g_marks[g_nmarks].text = arena_strndup(&g_perm, g_nodes[text].text, g_nodes[text].len);
	g_nmarks++;
}

/* test_fn NODE: (@test "name" body...) at the top level becomes (defn @test:K () : unit body...), a function that
 * gael test calls and nothing else does; 0 after an error it reported */
static int test_fn(i32 n)
{
	i32 head = first_of(n), name = next_of(head), line = g_nodes[n].line, col = g_nodes[n].col, fname, params, colon, ty;
	char buf[40];
	if (name < 0 || g_nodes[name].kind != N_STR) {
		err_node(n, "@test takes a name and a body: (@test \"adds\" (assert (= (+ 1 2) 3)))");
		g_nodes[head].text = "do"; /* reported once: what is left of it is (do) */
		g_nodes[head].len = 2;
		g_nodes[head].next = -1;
		return 0;
	}
	if (g_ntests == MAX_TESTS) die("too many tests (MAX_TESTS)");
	sprintf(buf, "@test:%ld", (long)g_ntests);
	fname = new_node(N_SYM, line, col);
	g_nodes[fname].text = arena_strndup(&g_unit_arena, buf, strlen(buf));
	g_nodes[fname].len = (u32)strlen(buf);
	params = new_node(N_LIST, line, col);
	colon = new_node(N_SYM, line, col);
	g_nodes[colon].text = ":";
	g_nodes[colon].len = 1;
	ty = new_node(N_SYM, line, col);
	g_nodes[ty].text = "unit";
	g_nodes[ty].len = 4;
	g_nodes[ty].next = next_of(name); /* the body */
	g_nodes[colon].next = ty;
	g_nodes[params].next = colon;
	g_nodes[fname].next = params;
	g_nodes[head].next = fname;
	g_nodes[head].text = "defn";
	g_nodes[head].len = 4;
	g_tests[g_ntests].name = arena_strndup(&g_perm, g_nodes[name].text, g_nodes[name].len);
	g_tests[g_ntests].file = g_unit.file;
	g_tests[g_ntests].line = line;
	g_ntests++;
	return 1;
}

static o start_list(Frame *f)
{
	i32 head = first_of(f->node), fi;
	const char *s;
	size_t k;
	Ty t;
	if (head < 0) { err_node(f->node, "empty list: a list starts with a form, an operator or a function"); finish(T_ERR); return; }
	if (g_nodes[head].kind == N_LIST) { /* ((get handlers 0) x): the function comes from an expression */
		i32 dot = first_of(head), e = dot >= 0 ? next_of(dot) : -1, path = e >= 0 ? next_of(e) : -1;
		if (sym_is(dot, ".") && is_sym(path) && next_of(path) < 0) {
			f->node = method_on_value(f->node, e, path); /* ((Vec3 1.0 0.0 0.0).cross b), ((get ps 0).hurt 3) */
			f->cur = -1;
			f->k = 0;
			return;
		}
		f->form = F_CALLV;
		f->op = 3;
		f->cur = next_of(head);
		f->k = count_from(f->cur);
		f->mark2 = g_slots;
		f->phase = 5;
		push_child(head, T_NONE);
		return;
	}
	if (!is_sym(head)) { err_node(head, "a list must start with a name: a form, an operator or a function"); finish(T_ERR); return; }
	s = g_nodes[head].text;
	f->cur = next_of(head);
	f->k = count_from(f->cur);
	if (s[0] == '@') { /* marks for the tools, and code only while working on the program */
		if (!strcmp(s, "@warn") || !strcmp(s, "@err")) { mark_note(f->node); finish(T_UNIT); return; }
		if (!strcmp(s, "@debug")) { f->form = F_DO; return; } /* its forms, their values dropped */
		if (!strcmp(s, "@test")) err_node(f->node, "@test goes at the top level: (@test \"name\" body...)");
		else err_node(head, "unknown annotation " NAME_FMT "; gael knows @export (def ...), (@test \"name\" body...), @test (form for the tests), (@debug ...), (@warn \"...\") and (@err \"...\")", s);
		finish(T_ERR);
		return;
	}
	if (!strcmp(s, "gpu-each")) { /* kernel.c: the GPU's kernel, and the loop for the VM in its place */
		if (!gpu_each(f)) finish(T_ERR);
		return;
	}
	if (!strcmp(s, "gpu kernel")) { /* (gpu kernel K), which only gpu-each writes: true when the GPU ran K */
		emit(OP_GPU, (i32)g_nodes[f->cur].ival, 0, g_nodes[f->node].line);
		finish(T_BOOL);
		return;
	}
	if (!strcmp(s, "gpu wgsl")) { /* (gpu wgsl K), after K's loop: its WGSL, now that every literal has its type */
		kernel_text((i32)g_nodes[f->cur].ival);
		finish(T_UNIT);
		return;
	}
	if (!strcmp(s, "if")) f->form = F_IF;
	else if (!strcmp(s, ".")) f->form = F_DOT;
	else if (!strcmp(s, "when")) f->form = F_WHEN;
	else if (!strcmp(s, "while")) f->form = F_WHILE;
	else if (!strcmp(s, "do")) { f->form = F_DO; f->keep = 1; }
	else if (!strcmp(s, "let")) f->form = F_LET;
	else if (!strcmp(s, "set")) f->form = F_SET;
	else if (!strcmp(s, "and") || !strcmp(s, "nand")) { f->form = F_AND; f->op = (u8)(s[0] == 'n'); }
	else if (!strcmp(s, "or") || !strcmp(s, "nor")) { f->form = F_OR; f->op = (u8)(s[0] == 'n'); }
	else if (!strcmp(s, "xor") || !strcmp(s, "xnor")) { f->form = F_XOR; f->op = (u8)(s[1] == 'n'); }
	else if (!strncmp(s, "bit-", 4) || !strcmp(s, "shl") || !strcmp(s, "shr")) {
		static const char *const bits[] = { "bit-and", "bit-or", "bit-xor", "bit-not", "shl", "shr" };
		for (k = 0; k < GAEL_COUNT(bits) && strcmp(s, bits[k]); k++) {}
		if (k < GAEL_COUNT(bits)) { f->form = F_BITS; f->op = (u8)k; }
	}
	else if (!strcmp(s, "pipe") || !strcmp(s, "pipe-in")) { f->form = F_SHPIPE; f->op = (u8)(s[4] == '-'); }
	else if (!strcmp(s, "chain")) {
		i32 n = thread_steps(f->node);
		if (n < 0) { finish(T_ERR); return; }
		f->node = n; /* compiled as the call it stands for */
		f->cur = -1;
		f->k = 0;
		return;
	}
	else if (!strcmp(s, "assert")) f->form = F_ASSERT;
	else if (!strcmp(s, "compile-error")) { /* a macro says its use is wrong: a compile error with its words, where it is used */
		if (f->k != 1 || g_nodes[f->cur].kind != N_STR) err_node(f->node, "compile-error takes one string: (compile-error \"what is wrong\")");
		else err_node(f->node, "%.800s", g_nodes[f->cur].text);
		finish(T_ERR);
		return;
	}
	else if (!strcmp(s, "coroutine")) { /* (coroutine (f args...)): the call, compiled to run in a new coroutine */
		i32 call = f->cur, h = call >= 0 && g_nodes[call].kind == N_LIST ? first_of(call) : -1;
		if (f->k != 1 || !is_sym(h) || (fi = find_fn(g_nodes[h].text)) < 0 || find_local(g_nodes[h].text, g_nodes[h].stamp) >= 0) {
			err_node(call >= 0 ? call : f->node, "coroutine takes a call of a function by its name: (coroutine (patrol goblin))");
			finish(T_ERR);
			return;
		}
		if (!gem_use(h, g_fns[fi].gem)) { finish(T_ERR); return; }
		f->node = call;
		f->form = F_CALL;
		f->a = fi;
		f->op = 1;
		f->tail = 0;
		f->cur = next_of(h);
		f->k = count_from(f->cur);
		return;
	}
	else if (!strcmp(s, "string-of") || !strcmp(s, "name-of")) { /* written by hand: the string or the name, here */
		if (!text_forms(f->node)) { finish(T_ERR); return; }
		f->cur = -1;
		f->k = 0;
		return;
	}
	else if (!strcmp(s, "return")) f->form = F_RETURN;
	else if (!strcmp(s, "not")) f->form = F_NOT;
	else if (s[1] == 0 && strchr("+-*/%", s[0])) { f->form = F_ARITH; f->op = (u8)s[0]; }

	else if (numeric(t = find_named_type(s))) { f->form = F_CONV; f->a = t; } /* (int x), (u8 x), (f32 x)... */
	else if (!strcmp(s, "some")) f->form = F_SOME;
	else if (!strcmp(s, "vec")) f->form = F_VEC;
	else if (!strcmp(s, "map")) f->form = F_MAP;
	else if (!strcmp(s, "pool")) f->form = F_POOL;
	else if (!strcmp(s, "arena")) f->form = F_ARENA;
	else if (!strcmp(s, "keep")) f->form = F_KEEP;
	else if (!strcmp(s, "fn")) f->form = F_LAMBDA;
	else if (!strcmp(s, "for")) f->form = F_FOR;
	else if (!strcmp(s, "match")) f->form = F_MATCH;
	else if (!strcmp(s, "defn") || !strcmp(s, "def") || !strcmp(s, "struct") || !strcmp(s, "enum") || !strcmp(s, "use") ||
	         !strcmp(s, "defmacro")) {
		err_node(head, NAME_FMT " is only allowed at the top level of a file", s);
		finish(T_ERR);
		return;
	} else if (!strcmp(s, "none")) {
		err_node(head, "none takes nothing; write it without parentheses");
		finish(T_ERR);
		return;
	} else {
		for (k = 0; k < GAEL_COUNT(g_cmp_names); k++) {
			if (!strcmp(s, g_cmp_names[k])) { f->form = F_CMP; f->op = (u8)k; break; }
		}
		for (k = 0; k < GAEL_COUNT(g_builtins); k++) {
			if (!strcmp(s, g_builtins[k].name)) { f->form = F_BUILTIN; f->op = (u8)k; break; }
		}
		for (k = 0; k < GAEL_COUNT(g_print_names); k++) {
			if (!strcmp(s, g_print_names[k])) { f->form = F_PRINT; f->op = (u8)k; break; }
		}
		for (k = 0; k < GAEL_COUNT(g_vecop_names); k++) {
			if (!strcmp(s, g_vecop_names[k])) { f->form = F_VECOP; f->op = (u8)k; break; }
		}
		for (k = 0; k < GAEL_COUNT(g_coro_names); k++) {
			if (!strcmp(s, g_coro_names[k])) { f->form = F_CORO; f->op = (u8)k; break; }
		}
		for (k = 0; k < GAEL_COUNT(g_mapop_names); k++) {
			if (!strcmp(s, g_mapop_names[k])) { f->form = F_MAPOP; f->op = (u8)k; break; }
		}
		if (f->form != F_START) return;
		if (strchr(s, '.') && method_call(f, head)) return;
		{ /* one namespace: a variable (or a field) named here comes before a function of that name */
			Place pl;
			int r;
			pl.where = AT_LOCAL;
			pl.at = 0;
			pl.type = T_ERR;
			r = resolve_place(head, &pl);
			if (r == 0) { finish(T_ERR); return; }
			if (r > 0 && !is_fn(pl.type)) {
				if (pl.type == T_ERR) ;
				else if (!strncmp(s, VALUE_NAME ".", sizeof(VALUE_NAME))) /* ((get v 0).x) */
					err_node(head, "%.64s is a field (%s), not a function: (expr).%.64s without the parens around it reads it",
					         s + sizeof(VALUE_NAME), tn(pl.type), s + sizeof(VALUE_NAME));
				else err_node(head, NAME_FMT " is %s, not a function", s, tn(pl.type));
				finish(T_ERR);
				return;
			}
			if (r > 0 && pl.where == AT_ENV) { /* from the closure: loaded into a slot before the arguments, like an expression's */
				f->form = F_CALLV;
				f->op = 3;
				f->cur = next_of(head);
				f->k = count_from(f->cur);
				f->mark2 = g_slots;
				f->phase = 5;
				push_child(head, T_NONE);
				return;
			}
			if (r > 0) {
				f->form = F_CALLV;
				f->op = (u8)(pl.where == AT_GLOBAL ? 2 : 1);
				f->c = pl.at;
				f->rt = pl.type;
				return;
			}
		}
		if ((fi = find_macro(s)) >= 0) { /* a macro: what it expands to is compiled in its place */
			i32 x = gem_use(head, g_macros[fi].gem) ? macro_expand(fi, f->node) : -1;
			if (x < 0) { finish(T_ERR); return; }
			if (g_show_expanded) { /* gael -m: the expansion takes the use's place in the tree, to be printed */
				i32 nx = g_nodes[f->node].next;
				g_nodes[f->node] = g_nodes[x];
				g_nodes[f->node].next = nx;
				x = f->node;
			}
			f->node = x;
			f->cur = -1;
			f->k = 0;
			return;
		}
		if (is_struct(t = find_named_type(s))) {
			if (!gem_use(head, g_tys[t].gem)) { finish(T_ERR); return; }
			fi = hook(t, "init");
			if (fi >= 0 && !(f->cur >= 0 && g_nodes[f->cur].kind == N_KEY) && !own_fn(t)) { /* (Type args...): its init makes it */
				if (g_fns[fi].ret != t) {
					err_node(head, "%s.init must return %s: (%s ...) calls it to make one", tn(t), tn(t), tn(t));
					finish(T_ERR);
					return;
				}
				f->form = F_CALL;
				f->a = fi;
				return;
			}
			f->form = F_NEW;
			f->a = t;
			return;
		}
		if ((fi = find_fn(s)) < 0 && (t == T_NONE) && (fi = soft_form(f->node, s)) != -1) { /* min, max, abs, clamp */
			if (fi == -2) { finish(T_ERR); return; }
			f->node = fi;
			f->cur = -1;
			f->k = 0;
			return;
		}
		if (fi < 0) {
			if (t != T_NONE) err_node(head, NAME_FMT " is a type, not a function", s);
			else if (find_local(s, g_nodes[head].stamp) >= 0 || find_global(s) >= 0) err_node(head, NAME_FMT " is a variable, not a function", s);
			else if (!strcmp(s, "await")) err_node(head, "gael never waits: (poll co) gives (option T) at once, none while the coroutine runs");
			else if (!strcmp(s, "async")) err_node(head, "gael has no async: (coroutine (f x)) starts f as a coroutine, and nothing waits for it");
			else err_node(head, "unknown function " NAME_FMT, s);
			finish(T_ERR);
			return;
		}
		if (!gem_use(head, g_fns[fi].gem)) { finish(T_ERR); return; }
		f->form = F_CALL;
		f->a = fi;
	}
}

/* emit_default FIELD LINE: push the default value of FIELD. A default made of constants only (100, "none", Dir.north)
 * is copied in from its function's code once that is compiled; any other is a call of it, so each struct gets its own
 * (vec) */
static o emit_default(const Field *fd, i32 line)
{
	const Fn *fn = &g_fns[fd->def];
	i32 pc, end;
	if (fn->entry >= 0) {
		for (end = fn->entry; end < g_ncode; end++) {
			u8 op = g_code[end].op;
			if (op != OP_PUSH_I && op != OP_PUSH_F && op != OP_PUSH_S && op != OP_PUSH_K && op != OP_ZERO) break;
		}
		if (end < g_ncode && (g_code[end].op == OP_RET_V || g_code[end].op == OP_RET_N)) {
			for (pc = fn->entry; pc < end; pc++) emit((Op)g_code[pc].op, g_code[pc].a, g_code[pc].b, line);
			return;
		}
	}
	emit(OP_CALL, fd->def, 0, line);
}

/* new_defaults FRAME LINE: a struct being made (F_NEW) gets the defaults of the fields its use left out - after the
 * values given in order, or into the scratch struct of named ones, which is then pushed whole */
static o new_defaults(Frame *f, i32 line)
{
	const TyInfo *ty = &g_tys[f->a];
	i32 i, p;
	for (i = f->op ? 0 : f->k; i < ty->n; i++) {
		const Field *fd = &g_fields[ty->first + i];
		if (f->op) {
			for (p = f->c; p >= 0 && strcmp(g_nodes[p].text + 1, fd->name); p = next_of(next_of(p))) {}
			if (p >= 0) continue;
		}
		emit_default(fd, line);
		if (f->op) emit_store(AT_LOCAL, f->b + fd->offset, fd->type, line);
	}
	if (f->op) {
		emit_load(AT_LOCAL, f->b, (Ty)f->a, line);
		g_slots = f->mark2;
	}
}

/* check_named NEW FIRST TYPE: (Vec2 :y 2.0 :x 1.0) - every key a field of TYPE, given once, and no field without a
 * default left out */
static int check_named(i32 node, i32 first, Ty st)
{
	const TyInfo *ty = &g_tys[st];
	char names[256];
	i32 p, q, count = 0, i;
	int ok = 1;
	for (p = first; p >= 0; p = next_of(next_of(p))) {
		if (g_nodes[p].kind != N_KEY) {
			err_node(p, "%s: expected a field name here, like :%s", ty->name, ty->n ? g_fields[ty->first].name : "x");
			return 0;
		}
		if (next_of(p) < 0) {
			err_node(p, "%s: field %s has no value", ty->name, g_nodes[p].text + 1);
			return 0;
		}
		for (q = first; q != p && strcmp(g_nodes[q].text, g_nodes[p].text); q = next_of(next_of(q))) {}
		if (q != p) {
			err_node(p, "%s: field %s is given twice", ty->name, g_nodes[p].text + 1);
			ok = 0;
		} else if (!find_field(st, g_nodes[p].text + 1, g_nodes[p].len - 1)) {
			err_node(p, "%s has no field %s; its fields are %s", ty->name, g_nodes[p].text + 1, names_of(st, names, sizeof(names)));
			ok = 0;
		} else {
			count++;
		}
	}
	if (ok && count < ty->n) {
		size_t used = 0;
		i32 missing = 0;
		names[0] = 0;
		for (i = 0; i < ty->n; i++) {
			const char *fname = g_fields[ty->first + i].name;
			if (g_fields[ty->first + i].def >= 0) continue; /* left out, it takes its default */
			for (p = first; p >= 0 && strcmp(g_nodes[p].text + 1, fname); p = next_of(next_of(p))) {}
			if (p >= 0) continue;
			missing++;
			if (used + strlen(fname) + 8 > sizeof(names)) { strcpy(names + used, ", ..."); break; }
			sprintf(names + used, "%s%s", used ? ", " : "", fname);
			used = strlen(names);
		}
		if (missing) {
			err_node(node, "%s: missing field%s %s", ty->name, missing == 1 ? "" : "s", names);
			ok = 0;
		}
	}
	return ok;
}

static const Field *field_of_key(Ty st, i32 key) { return find_field(st, g_nodes[key].text + 1, g_nodes[key].len - 1); }

/* pattern_variant ENUM NODE: the variant a match pattern names, bare (north) or with its type (Dir.north); -1 if none */
static i32 pattern_variant(Ty t, i32 p)
{
	const char *s, *dot;
	if (!is_sym(p)) return -1;
	s = g_nodes[p].text;
	dot = strchr(s, '.');
	if (dot) {
		if ((size_t)(dot - s) != strlen(g_tys[t].name) || strncmp(s, g_tys[t].name, (size_t)(dot - s))) return -1;
		s = dot + 1;
	}
	return find_variant(t, s, strlen(s));
}

/* check_arms MATCH FIRST TYPE: every pattern fits TYPE, none repeats or comes after a pattern that already covers
 * it, and together they cover every value of TYPE; reports what is wrong. Returns 1 when the patterns list every
 * value one by one, so the last arm needs no test */
static int check_arms(i32 node, i32 first, Ty t)
{
	const TyInfo *ti = &g_tys[t];
	char names[256];
	i32 arm, q, covered = 0, i;
	int mask = 0, full = 0, wild = 0, bad = 0;
	if (is_enum(t)) memset(g_seen, 0, (size_t)ti->n);
	for (arm = first; arm >= 0; arm = next_of(arm)) {
		i32 p = first_of(arm);
		int bit = 0, dup = 0;
		if (g_nodes[arm].kind != N_LIST || p < 0) {
			err_node(arm, "an arm is (pattern body...)");
			bad = 1;
			continue;
		}
		if (wild || full) {
			err_node(arm, wild ? "this arm never runs: the _ arm above matches everything"
			                   : "this arm never runs: the arms above already cover every value");
			break;
		}
		if (sym_is(p, "_")) { wild = 1; continue; }
		if (t == T_BOOL) {
			if (!sym_is(p, "true") && !sym_is(p, "false")) { err_node(p, "match on bool: a pattern is true, false or _"); bad = 1; continue; }
			bit = sym_is(p, "true") ? 1 : 2;
		} else if (is_int(t) || t == T_STR) {
			u8 kind = is_int(t) ? N_INT : N_STR;
			if (g_nodes[p].kind != kind) {
				if (is_int(t)) err_node(p, "match on %s: a pattern is an int like 0 or 42, or _", ti->name);
				else err_node(p, "match on string: a pattern is a string like \"yes\", or _");
				bad = 1;
				continue;
			}
			if (kind == N_INT && !int_fits(t, &g_nodes[p])) {
				err_node(p, "match on %s: %s does not fit in it (%s)", ti->name, g_nodes[p].text, int_range(t));
				bad = 1;
				continue;
			}
			for (q = first; q != arm; q = next_of(q)) {
				i32 qp = first_of(q);
				if (qp < 0 || g_nodes[qp].kind != kind) continue;
				if (kind == N_INT ? g_nodes[qp].ival == g_nodes[p].ival
				                  : g_nodes[qp].len == g_nodes[p].len && !memcmp(g_nodes[qp].text, g_nodes[p].text, g_nodes[p].len)) dup = 1;
			}
		} else if (is_enum(t)) {
			i = pattern_variant(t, p);
			if (i < 0) {
				err_node(p, "match on %s: a pattern is one of %s, or _", ti->name, names_of(t, names, sizeof(names)));
				bad = 1;
				continue;
			}
			if (g_seen[i]) dup = 1;
			else { g_seen[i] = 1; full = ++covered == ti->n; }
		} else { /* option */
			i32 name = next_of(first_of(p));
			if (sym_is(p, "none")) {
				bit = 2;
			} else if (head_is(p, "some") && is_sym(name) && next_of(name) < 0) {
				const char *s = g_nodes[name].text;
				if (strcmp(s, "_") && (is_reserved(s) || strchr(s, '.'))) err_node(name, NAME_FMT " cannot be a variable name", s);
				bit = 1;
			} else {
				err_node(p, "match on %s: a pattern is (some name), none or _", ti->name);
				bad = 1;
				continue;
			}
		}
		if (bit) {
			if (mask & bit) dup = 1;
			mask |= bit;
			full = mask == 3;
		}
		if (dup) err_node(arm, "this arm never runs: an arm above already matches the same value");
	}
	if (full || wild || bad) return full;
	if (t == T_BOOL) {
		err_node(node, "match on bool does not cover %s; add an arm for it or a _ arm", mask & 1 ? "false" : "true");
	} else if (is_enum(t)) {
		size_t used = 0;
		names[0] = 0;
		for (i = 0; i < ti->n; i++) {
			if (g_seen[i]) continue;
			if (used + strlen(g_variants[ti->first + i]) + 8 > sizeof(names)) { strcpy(names + used, ", ..."); break; }
			sprintf(names + used, "%s%s", used ? ", " : "", g_variants[ti->first + i]);
			used = strlen(names);
		}
		err_node(node, "match on %s does not cover %s; add arms for %s or a _ arm", ti->name, names,
		         ti->n - covered == 1 ? "it" : "them");
	} else if (is_option(t)) {
		err_node(node, "match on %s does not cover %s; add an arm for it or a _ arm", ti->name, mask & 1 ? "none" : "(some x)");
	} else {
		err_node(node, "match on %s needs a _ arm for the values the arms do not list", ti->name);
	}
	return 0;
}

/* fresh NODE: NODE makes a new string, vec, map or pool, or a struct or (some ...) with one in it; inside an arena
 * that object is made in the arena */
static int fresh(i32 n)
{
	static const char *const makers[] = { "format", "sh", "pipe", "pipe-in", "vec", "map", "pool", "split", "join",
		"read-file", "read-line", "getenv", "env", "keys", "values", "args" };
	i32 stack[64], ns = 0, head, x;
	size_t k;
	stack[ns++] = n;
	while (ns > 0) {
		n = stack[--ns];
		if (n < 0) continue;
		if (g_nodes[n].kind != N_LIST || !is_sym(head = first_of(n))) continue;
		for (k = 0; k < GAEL_COUNT(makers); k++) {
			if (!strcmp(g_nodes[head].text, makers[k])) return 1;
		}
		if (sym_is(head, "some") || is_struct(find_named_type(g_nodes[head].text))) {
			for (x = next_of(head); x >= 0 && ns < 64; x = next_of(x)) stack[ns++] = x;
		}
	}
	return 0;
}

/* arena_mark: the number of local names there were when the innermost arena being compiled began; -1 outside one */
static i32 arena_mark(void)
{
	i32 i;
	for (i = g_nwork - 1; i >= 0; i--) {
		if (g_work[i].form == F_LAMBDA) return -1; /* a fn's body runs when it is called, not inside this arena */
		if (g_work[i].form == F_ARENA) return g_work[i].mark;
	}
	return -1;
}

/* escapes WHAT PLACE VALUE: inside an arena, VALUE makes an object there and PLACE is a variable from outside it, so
 * the object would outlive the arena: report it */
static o escapes(const char *what, i32 place, i32 value)
{
	char base[256];
	const char *s, *dot;
	i32 mark = arena_mark(), i;
	if (mark < 0 || !is_sym(place) || !fresh(value)) return;
	s = g_nodes[place].text;
	dot = strchr(s, '.');
	if ((size_t)(dot ? dot - s : (long)strlen(s)) >= sizeof(base)) return;
	sprintf(base, "%.*s", (int)(dot ? dot - s : (long)strlen(s)), s);
	i = find_local(base, g_nodes[place].stamp);
	if (i >= mark || (i < 0 && find_global(base) < 0)) return;
	err_node(value, "%s: " NAME_FMT " lives outside this arena, and this value is made in it, so it would outlive it; (keep x) copies it out",
	         what, base);
}

/* start_arm MATCH: test the current arm's pattern against the matched value, bind the x of (some x), compile the body */
static o start_arm(Frame *f)
{
	i32 arm = f->cur, p = first_of(arm), line = arm >= 0 ? g_nodes[arm].line : 0, last = next_of(arm) < 0;
	Ty t = f->rt, want;
	int shaped = arm >= 0 && g_nodes[arm].kind == N_LIST && p >= 0;
	g_nlocals = f->mark;
	f->b = -1;
	if (shaped && !sym_is(p, "_") && !(last && f->op)) { /* no test when the arm takes whatever is left */
		emit(OP_LOAD_L, f->a, 0, line); /* the value, or an option's tag */
		if (is_option(t)) {
			f->b = emit(sym_is(p, "none") ? OP_JT : OP_JF, -1, 0, line);
		} else if (t == T_BOOL) {
			f->b = emit(sym_is(p, "true") ? OP_JF : OP_JT, -1, 0, line);
		} else {
			if (is_int(t)) emit(OP_PUSH_I, add_int(g_nodes[p].ival), 0, line);
			else if (t == T_STR) emit(OP_PUSH_S, add_str(g_nodes[p].text, g_nodes[p].len), 0, line);
			else emit(OP_PUSH_K, pattern_variant(t, p), 0, line);
			emit(t == T_STR ? OP_EQ_S : OP_EQ_I, t == T_STR, 0, line); /* the matched string is borrowed */
			f->b = emit(OP_JF, -1, 0, line);
		}
	}
	if (shaped && is_option(t) && head_is(p, "some")) {
		i32 name = next_of(first_of(p));
		if (is_sym(name) && strcmp(g_nodes[name].text, "_")) {
			add_local(name, LK_SOME, g_nodes[name].text, g_nodes[name].stamp, g_tys[t].elem, f->a + 1);
			g_locals[g_nlocals - 1].alias = 1; /* the match holds the value */
		}
	}
	want = f->want != T_NONE ? f->want : (f->res == T_ERR ? (Ty)T_NONE : f->res);
	f->phase = 3;
	push_body(shaped ? next_of(p) : -1, 1, want);
	g_work[g_nwork - 1].tail = f->tail;
}

/* A fn's body is compiled in the middle of the code around it, with a jump over it; what the compiler was counting
 * for the code around it waits here */

/* lambda_begin FN: (fn ((x : int)) : int body...) - register it and start its body; 0 after an error it reported */
static int lambda_begin(Frame *f)
{
	i32 params = f->cur, ret = -1, body, p, q, k, fi, line = g_nodes[f->node].line;
	Fn fn;
	char name[64];
	if (params < 0 || g_nodes[params].kind != N_LIST) {
		err_node(params < 0 ? f->node : params, "fn needs a parameter list, () when it takes none: (fn ((x : int)) : int (* x x))");
		return 0;
	}
	body = next_of(params);
	if (sym_is(body, ":")) {
		ret = next_of(body);
		body = next_of(ret);
	}
	memset(&fn, 0, sizeof(fn));
	for (p = first_of(params); p >= 0; p = next_of(p)) {
		i32 pn = first_of(p), colon = next_of(pn), pt = next_of(colon);
		if (g_nodes[p].kind != N_LIST || !is_sym(pn) || !sym_is(colon, ":") || pt < 0 || next_of(pt) >= 0) {
			err_node(p, "a parameter is (name : type)");
			return 0;
		}
		if (is_reserved(g_nodes[pn].text) || strchr(g_nodes[pn].text, '.'))
			err_node(pn, NAME_FMT " cannot be a parameter name", g_nodes[pn].text);
		for (q = first_of(params); q != p; q = next_of(q)) {
			if (!strcmp(g_nodes[first_of(q)].text, g_nodes[pn].text)) err_node(pn, "parameter " NAME_FMT " twice", g_nodes[pn].text);
		}
		if (fn.nparams == MAX_PARAMS) {
			err_node(p, "at most %d parameters", MAX_PARAMS);
			return 0;
		}
		fn.params[fn.nparams] = parse_type(pt);
		if (fn.params[fn.nparams] == T_UNIT) err_node(pt, "a parameter cannot be unit");
		fn.pcells += cells(fn.params[fn.nparams]);
		fn.nparams++;
	}
	fn.ret = ret >= 0 ? parse_type(ret) : (Ty)T_UNIT;
	fn.env = 1; /* its closure comes first, in slot 0 */
	fn.pcells++;
	sprintf(name, "fn@%ld:%ld", (long)line, (long)g_nodes[f->node].col);
	fn.name = arena_strndup(&g_perm, name, strlen(name));
	fn.line = line;
	fn.file = g_unit.file;
	fn.decl_unit = g_unit_id;
	fn.decl_node = f->node;
	if (g_nfns == MAX_FNS) die("too many functions");
	if (g_nlambdas == MAX_LAMBDAS) {
		err_node(f->node, "fns nested deeper than %d", MAX_LAMBDAS);
		return 0;
	}
	fi = g_nfns++;
	g_fns[fi] = fn;
	g_lambdas[g_nlambdas].fi = fi;
	g_lambdas[g_nlambdas].jmp = emit(OP_JMP, -1, 0, line);
	g_lambdas[g_nlambdas].slots = g_slots;
	g_lambdas[g_nlambdas].maxslots = g_maxslots;
	g_lambdas[g_nlambdas].depth = g_depth;
	g_lambdas[g_nlambdas].maxdepth = g_maxdepth;
	g_lambdas[g_nlambdas].nlocals = g_nlocals;
	g_lambdas[g_nlambdas].floor = g_local_floor;
	g_lambdas[g_nlambdas].fn_base = g_fn_base;
	g_lambdas[g_nlambdas].fn_ret = g_fn_ret;
	g_lambdas[g_nlambdas].fn_tbase = g_fn_tbase;
	g_lambdas[g_nlambdas].ncaps = 0;
	g_lambdas[g_nlambdas].envcells = 0;
	g_nlambdas++;
	g_fns[fi].entry = g_ncode;
	g_slots = g_maxslots = 0;
	g_depth = g_maxdepth = 0;
	g_local_floor = g_nlocals; /* the locals around it belong to another frame: what it uses of them, its closure keeps */
	add_local(-1, LK_PARAM, "", 0, intern_fn(g_fns[fi].params, g_fns[fi].nparams, g_fns[fi].ret), new_slots(1)); /* the closure */
	for (p = first_of(params), k = 0; p >= 0; p = next_of(p), k++)
		add_local(first_of(p), LK_PARAM, g_nodes[first_of(p)].text, g_nodes[first_of(p)].stamp, g_fns[fi].params[k],
		          new_slots(cells(g_fns[fi].params[k])));
	f->phase = 1;
	push_body(body, 1, g_fns[fi].ret);
	g_work[g_nwork - 1].tail = 1;
	g_fn_base = g_nwork - 1;
	g_fn_ret = g_fns[fi].ret;
	g_fn_tbase = g_ntypes;
	return 1;
}

/* env_type LEVEL: the values fn LEVEL keeps, as a struct type of its own (it says how to free them) */
static Ty env_type(i32 lv)
{
	TyInfo *t;
	i32 k;
	if (g_ntys == MAX_TYS) die("too many types");
	if (g_nfields + g_lambdas[lv].ncaps > MAX_FIELDS) die("too many fields");
	t = &g_tys[g_ntys];
	memset(t, 0, sizeof(*t));
	t->name = "(what a fn keeps)";
	t->kind = K_STRUCT;
	t->first = g_nfields;
	t->n = g_lambdas[lv].ncaps;
	t->cells = g_lambdas[lv].envcells;
	for (k = 0; k < g_lambdas[lv].ncaps; k++) {
		const Capture *cp = &g_lambdas[lv].caps[k];
		g_fields[g_nfields].name = arena_strndup(&g_perm, cp->name, strlen(cp->name));
		g_fields[g_nfields].type = cp->type;
		g_fields[g_nfields].offset = cp->off;
		g_fields[g_nfields].def = -1;
		g_nfields++;
	}
	set_leaves((Ty)g_ntys);
	return (Ty)g_ntys++;
}

/* lambda_end FN TYPE LINE: its body is done, with TYPE: return, go back to the code around it, push the fn */
static o lambda_end(Ty t, i32 line)
{
	i32 fi = g_lambdas[g_nlambdas - 1].fi, k, nc;
	Fn *fn = &g_fns[fi];
	for (k = 0; k <= fn->nparams; k++) emit_release(AT_LOCAL, g_locals[g_local_floor + k].slot, g_locals[g_local_floor + k].type, line);
	if (fn->ret == T_UNIT) emit_pop(t, line);
	else if (t != T_ERR && fn->ret != T_ERR && t != fn->ret) err_node(g_fns[fi].decl_node, "this fn must return %s, its body ends with %s", tn(fn->ret), tn(t));
	(void)nc;
	emit_ret(fn->ret, line);
	fn->nslots = g_maxslots;
	fn->maxdepth = g_maxdepth;
	g_nlambdas--;
	g_slots = g_lambdas[g_nlambdas].slots;
	g_maxslots = g_lambdas[g_nlambdas].maxslots;
	g_depth = g_lambdas[g_nlambdas].depth;
	g_maxdepth = g_lambdas[g_nlambdas].maxdepth;
	g_nlocals = g_lambdas[g_nlambdas].nlocals;
	g_local_floor = g_lambdas[g_nlambdas].floor;
	g_fn_base = g_lambdas[g_nlambdas].fn_base;
	g_fn_ret = g_lambdas[g_nlambdas].fn_ret;
	g_fn_tbase = g_lambdas[g_nlambdas].fn_tbase;
	patch(g_lambdas[g_nlambdas].jmp, g_ncode);
	if (g_lambdas[g_nlambdas].ncaps) { /* a closure: the values it keeps, copied now, in the code around it */
		Ty env = env_type(g_nlambdas);
		for (k = 0; k < g_lambdas[g_nlambdas].ncaps; k++) {
			const Capture *cp = &g_lambdas[g_nlambdas].caps[k];
			emit_copy(cp->from_env ? AT_ENV : AT_LOCAL, cp->src, cp->type, line);
		}
		emit(OP_CLOSURE, fi, env, line);
	} else {
		emit(OP_PUSH_K, -(fi + 1), 0, line);
	}
	finish(intern_fn(fn->params, fn->nparams, fn->ret));
}

/* run_work BASE: compile until the work stack is back at BASE; leaves one type per finished root */
static o run_work(i32 base)
{
	while (g_nwork > base) {
		Frame *f = &g_work[g_nwork - 1];
		i32 line = line_of(f), i;
		Ty t, t2;

		if (f->form == F_START) {
			if (f->node < 0) { finish(T_ERR); continue; } /* a missing operand; its form reported it */
			if (g_nodes[f->node].kind != N_LIST) {
				compile_atom(f->node, f->want);
				g_nwork--;
				continue;
			}
			start_list(f);
			continue;
		}

		switch (f->form) {
		case F_DO: /* f->k: the unit's errors when the form now compiling began */
			if (f->phase == 0) {
				if (f->cur < 0) { finish(T_UNIT); break; }
				f->phase = 1;
				f->k = g_unit.nerrors;
				push_child(f->cur, next_of(f->cur) < 0 && f->keep ? f->want : (Ty)T_UNIT);
				if (next_of(f->cur) < 0) g_work[g_nwork - 1].tail = f->tail;
				break;
			}
			t = pop_type();
			if (next_of(f->cur) < 0 && f->keep) { finish(t); break; }
			if (t != T_UNIT && t != T_ERR && f->k == g_unit.nerrors && pure_form(f->cur)) thrown_away(f->cur);
			emit_pop(t, g_nodes[f->cur].line);
			if (next_of(f->cur) < 0) { finish(T_UNIT); break; }
			if (ends_flow(f->cur) && !g_nodes[f->cur].stamp)
				warn_node(next_of(f->cur), "this never runs: the %s on line %ld comes first; remove it, or move it above",
				          head_is(f->cur, "exit") ? "exit" : "return", (long)g_nodes[f->cur].line);
			f->cur = next_of(f->cur);
			f->k = g_unit.nerrors;
			push_child(f->cur, next_of(f->cur) < 0 && f->keep ? f->want : (Ty)T_UNIT);
			if (next_of(f->cur) < 0) g_work[g_nwork - 1].tail = f->tail; /* without keep too: a unit call can end it */
			break;

		case F_IF:
			switch (f->phase) {
			case 0:
				if (f->k < 2 || f->k > 3) {
					err_node(f->node, "if takes a condition, a then branch and an optional else branch");
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				f->mark2 = g_nfjumps;
				push_cond(f->cur, 0, 0, CC_IF);
				break;
			case 1: /* the condition's false jumps wait on g_fjumps from mark2 */
				f->depth = g_depth;
				f->cur = next_of(f->cur);
				f->phase = 2;
				push_child(f->cur, next_of(f->cur) < 0 ? (Ty)T_UNIT : f->want); /* without else the value is dropped */
				g_work[g_nwork - 1].tail = f->tail;
				break;
			case 2:
				if (next_of(f->cur) < 0) {
					emit_pop(pop_type(), line);
					aim_jumps(g_fjumps, &g_nfjumps, f->mark2, g_ncode);
					finish(T_UNIT);
					break;
				}
				if (f->want == T_UNIT) { /* the value is dropped anyway, so the branches need not agree */
					emit_pop(pop_type(), line);
					push_type(T_UNIT);
				}
				f->b = emit(OP_JMP, -1, 0, line);
				aim_jumps(g_fjumps, &g_nfjumps, f->mark2, g_ncode);
				g_depth = f->depth;
				f->cur = next_of(f->cur);
				f->phase = 3;
				t = g_types[g_ntypes - 1]; /* the then branch tells an else branch like none what it is */
				push_child(f->cur, f->want != T_NONE ? f->want : (t == T_ERR ? (Ty)T_NONE : t));
				g_work[g_nwork - 1].tail = f->tail;
				break;
			default:
				t2 = pop_type();
				if (f->want == T_UNIT) {
					emit_pop(t2, line);
					t2 = T_UNIT;
				}
				t = pop_type();
				patch(f->b, g_ncode);
				if (t != t2 && t != T_ERR && t2 != T_ERR) { /* reported here, so the parent does not report it again */
					if (soft_of(f) < 0) /* in min and the like, the < said it */
						err_node(f->cur, "if: the then branch is %s and the else branch is %s; they must match", tn(t), tn(t2));
					t = T_ERR;
					t2 = T_ERR;
				}
				finish(t == T_ERR ? t2 : t);
				break;
			}
			break;

		case F_WHEN:
		case F_WHILE:
			switch (f->phase) {
			case 0:
				if (f->k < 1) {
					err_node(f->node, "%s takes a condition and a body", f->form == F_WHEN ? "when" : "while");
					finish(T_ERR);
					break;
				}
				f->a = g_ncode; /* while loops back here */
				f->phase = 1;
				f->mark2 = g_nfjumps;
				push_cond(f->cur, 0, 0, f->form == F_WHEN ? CC_WHEN : CC_WHILE);
				break;
			case 1:
				f->phase = 2;
				push_body(next_of(f->cur), 0, T_UNIT);
				if (f->form == F_WHEN) g_work[g_nwork - 1].tail = f->tail;
				break;
			default:
				(void)pop_type();
				if (f->form == F_WHILE) emit(OP_JMP, f->a, 0, line);
				aim_jumps(g_fjumps, &g_nfjumps, f->mark2, g_ncode);
				finish(T_UNIT);
				break;
			}
			break;

		case F_COND: { /* a condition (push_cond): f->op 0 jumps when false, 1 when true; f->b the list it goes on */
			i32 head = g_nodes[f->node].kind == N_LIST ? first_of(f->node) : -1, at;
			const char *hs = is_sym(head) ? g_nodes[head].text : "";
			if (f->phase == 0) {
				i32 k = head >= 0 ? count_from(next_of(head)) : 0;
				if (!strcmp(hs, "not") && k == 1) { /* the condition inside, with the jump's sense turned */
					f->node = next_of(head);
					f->op ^= 1;
					f->c = CC_NOT;
					break;
				}
				if (k >= 1 && (!strcmp(hs, "and") || !strcmp(hs, "or") || !strcmp(hs, "nand") || !strcmp(hs, "nor"))) {
					f->a = hs[0] == 'n' ? hs[1] : hs[0]; /* 'a' or 'o'; nand is not and, nor is not or */
					f->op ^= (u8)(hs[0] == 'n');
					f->c = hs[0] == 'a' ? CC_AND : hs[0] == 'o' ? CC_OR : hs[1] == 'a' ? CC_NAND : CC_NOR;
					f->mark = g_ntjumps;
					f->mark2 = g_nfjumps;
					f->cur = next_of(head);
					f->phase = 1;
					cond_operand(f);
					break;
				}
				f->phase = 2;
				push_child(f->node, T_NONE);
				break;
			}
			if (f->phase == 1) { /* one operand of an and or an or is done */
				f->cur = next_of(f->cur);
				if (f->cur >= 0) {
					cond_operand(f);
					break;
				}
				if ((f->a == 'a') == f->op) { /* the jumps its operands made to after it, on the other list, land here */
					if (f->b) aim_jumps(g_fjumps, &g_nfjumps, f->mark2, g_ncode);
					else aim_jumps(g_tjumps, &g_ntjumps, f->mark, g_ncode);
				}
				g_nwork--;
				break;
			}
			t = pop_type(); /* a value: one jump */
			if (t != T_BOOL && t != T_ERR) {
				static const char *const forms[] = { "if", "when", "while", "and", "or", "not", "nand", "nor", "assert" };
				if (f->c <= CC_WHILE || f->c == CC_ASSERT) err_node(f->node, "%s: the condition is %s, it must be bool", forms[f->c], tn(t));
				else if (f->c == CC_NOT) err_node(f->node, "not takes a bool, this is %s", tn(t));
				else err_node(f->node, "%s takes bool values, this one is %s%s", forms[f->c], tn(t),
				              t == T_STR ? "; (run \"cmd\") is true when a command succeeds" : "");
			}
			at = emit(f->op ? OP_JT : OP_JF, -1, 0, line);
			if (f->b) add_jump(g_tjumps, &g_ntjumps, at);
			else add_jump(g_fjumps, &g_nfjumps, at);
			g_nwork--;
			break;
		}

		case F_LET:
			switch (f->phase) {
			case 0:
				if (f->cur < 0 || g_nodes[f->cur].kind != N_LIST) {
					err_node(f->node, "let needs a list of bindings: (let ((x 1) (y : float 2.0)) body)");
					finish(T_ERR);
					break;
				}
				f->mark = g_nlocals;
				f->mark2 = g_slots;
				f->b = f->cur;              /* the bindings list; the body follows it */
				f->cur = first_of(f->cur);  /* the first binding */
				f->phase = 1;
				break;
			case 1: {
				i32 name, x, init;
				if (f->cur < 0) {
					f->phase = 3;
					push_body(next_of(f->b), 1, f->want);
					g_work[g_nwork - 1].tail = f->tail;
					break;
				}
				name = first_of(f->cur);
				if (g_nodes[f->cur].kind != N_LIST || !is_sym(name)) {
					err_node(f->cur, "a binding is (name value) or (name : type value)");
					f->cur = next_of(f->cur);
					break;
				}
				if (strcmp(g_nodes[name].text, "_") && (is_reserved(g_nodes[name].text) || strchr(g_nodes[name].text, '.')))
					err_node(name, NAME_FMT " cannot be a variable name", g_nodes[name].text); /* _: no name */
				x = next_of(name);
				f->rt = T_NONE;
				if (sym_is(x, ":")) {
					f->rt = parse_type(next_of(x));
					init = next_of(next_of(x));
				} else {
					init = x;
				}
				if (init < 0) {
					err_node(name, NAME_FMT " has no value", g_nodes[name].text);
					f->cur = next_of(f->cur);
					break;
				}
				if (next_of(init) >= 0) err_node(next_of(init), "a binding has one value");
				f->a = name;
				f->phase = 2;
				push_child(init, f->rt);
				break;
			}
			case 2: {
				const char *name = g_nodes[f->a].text;
				Ty type;
				t = pop_type();
				type = f->rt != T_NONE ? f->rt : t;
				if (f->rt != T_NONE && f->rt != T_ERR && t != T_ERR && t != f->rt)
					err_node(f->a, NAME_FMT " is declared %s, its value is %s", name, tn(f->rt), tn(t));
				if (type == T_UNIT) err_node(f->a, NAME_FMT " would hold no value (unit)", name);
				i = new_slots(cells(type));
				emit_store(0, i, type, g_nodes[f->a].line);
				add_local(f->a, LK_LET, name, g_nodes[f->a].stamp, type, i);
				f->cur = next_of(f->cur);
				f->phase = 1;
				break;
			}
			default:
				t = pop_type();
				for (i = g_nlocals - 1; i >= f->mark; i--) emit_release(0, g_locals[i].slot, g_locals[i].type, line);
				locals_end(f->mark);
				g_slots = f->mark2;
				finish(t);
				break;
			}
			break;

		case F_SET:
			if (f->phase == 0) {
				Place pl;
				if (f->k != 2 || !is_sym(f->cur)) {
					err_node(f->node, "set takes a name and a value: (set x 5)");
					finish(T_ERR);
					break;
				}
				pl.where = AT_LOCAL;
				pl.at = 0;
				pl.type = T_ERR;
				g_place_set = 1;
				f->c = resolve_place(f->cur, &pl);
				g_place_set = 0;
				if (f->c < 0) err_node(f->cur, "set: unknown name " NAME_FMT, g_nodes[f->cur].text);
				f->rt = f->c > 0 ? pl.type : (Ty)T_ERR;
				f->op = (u8)pl.where;
				f->a = pl.at;
				f->phase = 1;
				push_child(next_of(f->cur), f->rt);
				break;
			}
			t = pop_type();
			if (f->c > 0) {
				if (f->rt != T_ERR && t != T_ERR && t != f->rt)
					err_node(next_of(f->cur), "set: " NAME_FMT " is %s, the value is %s", g_nodes[f->cur].text, tn(f->rt), tn(t));
				if (has_refs(f->rt)) escapes("set", f->cur, next_of(f->cur));
				emit_release(f->op, f->a, f->rt, line);
				emit_store(f->op, f->a, f->rt, line);
			}
			finish(T_UNIT);
			break;

		case F_AND:
		case F_OR:
			if (f->phase == 0) {
				if (f->k < 1) {
					err_node(f->node, "%s takes one or more values", g_nodes[first_of(f->node)].text);
					finish(T_ERR);
					break;
				}
				f->mark = g_npatches;
				f->depth = g_depth;
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			t = pop_type();
			if (t != T_BOOL && t != T_ERR)
				err_node(f->cur, "%s takes bool values, this one is %s%s", g_nodes[first_of(f->node)].text, tn(t),
				         t == T_STR ? "; (run \"cmd\") is true when a command succeeds" : "");
			if (next_of(f->cur) >= 0) {
				if (g_npatches == MAX_WORK) die("too many and/or operands");
				g_patches[g_npatches++] = emit(f->form == F_AND ? OP_JF : OP_JT, -1, 0, line);
				f->cur = next_of(f->cur);
				push_child(f->cur, T_NONE);
				break;
			}
			if (f->mark < g_npatches) {
				i32 end = emit(OP_JMP, -1, 0, line);
				for (i = f->mark; i < g_npatches; i++) patch(g_patches[i], g_ncode);
				emit(OP_PUSH_K, f->form == F_OR, 0, line);
				patch(end, g_ncode);
				g_npatches = f->mark;
			}
			g_depth = f->depth + 1;
			if (f->op) emit(OP_NOT, 0, 0, line); /* nand, nor */
			finish(T_BOOL);
			break;

		case F_XOR: /* (xor a b...): true when an odd number of them are true; xnor: the opposite. Every one runs */
			if (f->phase == 0) {
				if (f->k < 2) {
					err_node(f->node, "%s takes two or more values", f->op ? "xnor" : "xor");
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			t = pop_type();
			if (t != T_BOOL && t != T_ERR) err_node(f->cur, "%s takes bool values, this one is %s", f->op ? "xnor" : "xor", tn(t));
			if (f->phase == 2) emit(OP_NE_I, 0, 0, line);
			f->phase = 2;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) {
				push_child(f->cur, T_NONE);
				break;
			}
			if (f->op) emit(OP_NOT, 0, 0, line);
			finish(T_BOOL);
			break;

		case F_BITS: { /* (bit-and a b...) bit-or bit-xor, (bit-not a), (shl a n) (shr a n): on ints of one type */
			static const char *const names[] = { "bit-and", "bit-or", "bit-xor", "bit-not", "shl", "shr" };
			static const Op ops[] = { OP_BAND, OP_BOR, OP_BXOR };
			const char *what = names[f->op];
			if (f->phase == 0) {
				if (f->op == 3 ? f->k != 1 : f->op > 3 ? f->k != 2 : f->k < 2) {
					err_node(f->node, f->op == 3 ? "bit-not takes one int" : f->op > 3 ? "%s takes an int and a count: (%s x 3)"
					                                                      : "%s takes two or more ints", what, what);
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				f->res = numeric(f->want) ? f->want : number_hint(f->cur);
				push_child(f->cur, f->res);
				break;
			}
			t = pop_type();
			if (f->phase == 3) { /* the count of a shift */
				if (t != T_INT && t != T_ERR) err_node(f->cur, "%s: the count is %s, it must be int", what, tn(t));
				if (f->rt != T_ERR) emit(f->op == 4 ? OP_SHL : OP_SHR, f->rt, 0, line);
				finish(f->rt);
				break;
			}
			if (t != T_ERR && !is_int(t)) {
				err_node(f->cur, "%s works on ints, not %s", what, tn(t));
				t = T_ERR;
			}
			if (f->phase == 1) {
				f->rt = t;
				if (f->op == 3) { /* bit-not */
					if (t != T_ERR) emit(OP_BNOT, 0, 0, line);
					if (t != T_ERR) emit_fit(t, line);
					finish(t);
					break;
				}
				if (f->op > 3) {
					f->phase = 3;
					f->cur = next_of(f->cur);
					push_child(f->cur, T_INT);
					break;
				}
			} else if (f->rt != T_ERR && t != T_ERR && t != f->rt) {
				err_node(f->cur, "%s: %s and %s do not mix; convert one, like (%s x) or (%s x)", what, tn(f->rt), tn(t), tn(f->rt), tn(t));
				f->rt = T_ERR;
			} else if (f->rt != T_ERR && t != T_ERR) {
				emit(ops[f->op], 0, 0, line); /* zero- or sign-extended stays so: no wrap needed */
			} else {
				f->rt = T_ERR;
			}
			f->phase = 2;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) push_child(f->cur, f->rt != T_ERR ? f->rt : f->res); else finish(f->rt);
			break;
		}

		case F_NOT:
			if (f->phase == 0) {
				if (f->k != 1) { err_node(f->node, "not takes one bool"); finish(T_ERR); break; }
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			t = pop_type();
			if (t != T_BOOL && t != T_ERR) err_node(f->cur, "not takes a bool, this is %s", tn(t));
			emit(OP_NOT, 0, 0, line);
			finish(T_BOOL);
			break;

		case F_ARITH:
			if (f->phase == 0) {
				if (f->k == 0 || (f->k == 1 && f->op != '-')) {
					err_node(f->node, "%c takes two or more numbers", f->op);
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				f->res = numeric(f->want) ? f->want : number_hint(f->cur); /* what a literal operand becomes */
				push_child(f->cur, f->res);
				break;
			}
			t = pop_type();
			if (f->c) { /* the right value of a hook, whose function takes the two */
				const Fn *fn = &g_fns[f->c - 1];
				if (t != T_ERR && t != fn->params[1]) {
					err_node(f->cur, "%c: %s takes %s as its second value, this is %s", f->op, fn->name, tn(fn->params[1]), tn(t));
					f->rt = T_ERR;
				} else {
					emit(OP_CALL, f->c - 1, 0, line);
					f->rt = t == T_ERR ? (Ty)T_ERR : fn->ret;
				}
				f->c = 0;
			} else if (f->phase == 1 && (is_struct(t) || is_enum(t))) { /* a struct or an enum first: its type's hook */
				f->rt = t;
				if (f->k == 1) { /* (- v): its negate */
					i32 fi = arith_hook(f, t);
					if (fi >= 0) emit(OP_CALL, fi, 0, line);
					finish(fi >= 0 ? g_fns[fi].ret : (Ty)T_ERR);
					break;
				}
			} else {
				if (t != T_ERR && !numeric(t)) { /* after an error in this form, quiet: one mistake, one message */
					char op[2];
					op[0] = (char)f->op;
					op[1] = 0;
					if (f->phase == 1)
						err_node(f->cur, "%s works on numbers, not %s", op_name(f, op), tn(t));
					else if (f->rt != T_ERR && (is_struct(t) || is_enum(t)))
						err_node(f->cur, "%c: %s and %s do not mix; with %s first, %c would call its function", f->op, tn(f->rt), tn(t), tn(t), f->op);
					else if (f->rt != T_ERR)
						err_node(f->cur, "%s works on numbers, not %s", op_name(f, op), tn(t));
					t = T_ERR;
				} else if (is_float(t) && f->op == '%') {
					if (f->phase == 1 || f->rt != T_ERR) err_node(f->cur, "%% works on ints only; this is %s", a_type(t));
					t = T_ERR;
				}
				if (f->phase == 1) {
					f->rt = t;
					if (f->k == 1) {
						if (t != T_ERR) emit(is_float(t) ? OP_NEG_F : OP_NEG_I, 0, 0, line);
						if (t != T_ERR) emit_fit(t, line);
						finish(t);
						break;
					}
				} else if (f->rt == T_ERR || t == T_ERR) {
					f->rt = T_ERR;
				} else if (t != f->rt) {
					err_node(f->cur, "%c: %s and %s do not mix; convert one, like (%s x) or (%s x)", f->op, tn(f->rt), tn(t), tn(f->rt), tn(t));
					f->rt = T_ERR;
				} else {
					emit(arith_op(f->op, t), 0, 0, line);
					emit_fit(t, line);
				}
			}
			f->phase = 2;
			f->cur = next_of(f->cur);
			if (f->cur < 0) { finish(f->rt); break; }
			if (is_struct(f->rt) || is_enum(f->rt)) { /* the value so far is a struct or an enum: its hook takes the next one */
				i32 fi = arith_hook(f, f->rt);
				f->c = fi + 1;
				if (fi < 0) f->rt = T_ERR;
			}
			push_child(f->cur, f->c ? g_fns[f->c - 1].params[1] : f->rt != T_ERR ? f->rt : f->res);
			break;

		case F_CMP:
			if (f->phase == 0) {
				if (f->k != 2) { err_node(f->node, "%s compares two values", g_cmp_names[f->op]); finish(T_ERR); break; }
				f->phase = 1;
				push_child(f->cur, quiet_type(next_of(f->cur))); /* (< 0 x) with x a u8: the 0 is a u8 */
				break;
			}
			if (f->phase == 1) {
				f->phase = 2;
				t = g_types[g_ntypes - 1];
				if (f->op >= 4 && (t == T_STR || t == T_PATH)) f->b = borrow_last(f->cur, next_of(f->cur)); /* = and != only read their strings */
				f->cur = next_of(f->cur);
				push_child(f->cur, t == T_ERR ? (Ty)T_NONE : t);
				break;
			}
			t2 = g_types[g_ntypes - 1];
			if (f->op >= 4 && (t2 == T_STR || t2 == T_PATH) && borrow_last(f->cur, -1)) f->b |= 2;
			t2 = pop_type();
			t = pop_type();
			if (t != T_ERR && t2 != T_ERR) {
				if (t != t2 && soft_of(f) >= 0)
					err_node(f->cur, "%s takes values of one type, these are %s and %s; convert one, like (float x) or (int x)",
					         op_name(f, "<"), tn(t2), tn(t));
				else if (t != t2)
					err_node(f->cur, "%s compares %s with %s; both sides need the same type", g_cmp_names[f->op], tn(t), tn(t2));
				else if ((is_struct(t) || is_enum(t)) && cmp_hook(f, t, line))
					; /* its type's equal or compare */
				else if (f->op < 4 && !numeric(t))
					err_node(f->cur, "%s orders numbers, not %s", op_name(f, g_cmp_names[f->op]), tn(t));
				else if (t == T_UNIT)
					err_node(f->cur, "%s: unit has no value to compare", g_cmp_names[f->op]);
				else if (!comparable(t))
					err_node(f->cur, "%s compares int, float, bool, string and enum values, not %s", g_cmp_names[f->op], tn(t));
				else
					emit(cmp_op(f->op, t), f->b, 0, line);
			}
			finish(T_BOOL);
			break;

		case F_PRINT: {
			const char *what = g_print_names[f->op];
			i32 fmt = next_of(first_of(f->node));
			Fmt *fr;
			if (f->phase == 0) {
				i32 codes = 0;
				if (fmt < 0 || g_nodes[fmt].kind != N_STR) {
					err_node(fmt < 0 ? f->node : fmt, "%s: the first argument must be a string literal (its %% codes are checked while compiling)", what);
					finish(T_ERR);
					break;
				}
				if (fmt_type(g_nodes[fmt].text, g_nodes[fmt].len, -1, &codes) == T_ERR) {
					err_node(fmt, "%s: unknown %% code; use %%d (int), %%x (int in hex), %%f (float), %%s (string), %%b (bool), %%v (any value) or %%%%", what);
					finish(T_ERR);
					break;
				}
				if (codes != f->k - 1) {
					err_node(fmt, "%s: the format has %ld %% code%s but %ld value%s follow%s", what, (long)codes,
					         codes == 1 ? "" : "s", (long)(f->k - 1), f->k == 2 ? "" : "s", f->k == 2 ? "s" : "");
					finish(T_ERR);
					break;
				}
				if (g_nfmts == MAX_FMTS) die("too many print formats");
				if (g_nfmt_tys + codes > MAX_CONSTS) die("too many printed values");
				f->a = g_nfmts++;
				fr = &g_fmts[f->a];
				fr->str = add_str(g_nodes[fmt].text, g_nodes[fmt].len);
				fr->first = g_nfmt_tys;
				fr->n = codes;
				fr->cells = 0;
				g_nfmt_tys += codes; /* reserved now: a print inside an argument adds its own after these */
				f->cur = next_of(fmt);
				f->k = 0;
				if (f->cur < 0) {
					emit(g_print_ops[f->op], f->a, f->op >= 3, line);
					finish(f->op == 2 ? (Ty)T_STR : (Ty)T_UNIT);
					break;
				}
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			fr = &g_fmts[f->a];
			t = pop_type();
			t2 = fmt_type(g_nodes[fmt].text, g_nodes[fmt].len, f->k, 0);
			if ((t2 == T_ANY || t2 == T_STR) && (i = hook(t, "text")) >= 0) { /* a struct or an enum written by its text */
				const Fn *fn = &g_fns[i];
				if (fn->nparams != 1 || fn->params[0] != t || fn->ret != T_STR)
					err_node(f->cur, "%s: %%%c of %s calls %s, which must take one %s and return string", what, t2 == T_ANY ? 'v' : 's', tn(t), fn->name, tn(t));
				else if (gem_use(f->cur, fn->gem))
					emit(OP_CALL, i, 0, line);
				t = T_STR;
			}
			if (t2 == T_STR && (is_struct(t) || is_enum(t))) {
				err_node(f->cur, "%s: %% code %ld wants a string, this value is %s; %%v writes any value, and %s.text would be what %%s writes: (defn %s.text ((x : %s)) : string ...)",
				         what, (long)(f->k + 1), tn(t), tn(t), tn(t), tn(t));
				t = T_ERR;
			}
			if (t2 == T_ANY) {
				if (t == T_UNIT) err_node(f->cur, "%s: %%v needs a value; this is unit", what);
			} else if (t != T_ERR && t != t2 && !(t2 == T_INT && is_int(t)) && !(t2 == T_FLOAT && is_float(t))) {
				err_node(f->cur, "%s: %% code %ld wants %s, this value is %s", what, (long)(f->k + 1), t2 == T_INT ? "an int" : t2 == T_FLOAT ? "a float" : tn(t2), tn(t));
			}
			g_fmt_tys[fr->first + f->k] = t;
			fr->cells += cells(t);
			f->k++;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) { push_child(f->cur, T_NONE); break; }
			emit(g_print_ops[f->op], f->a, f->op >= 3, line);
			finish(f->op == 2 ? (Ty)T_STR : (Ty)T_UNIT);
			break;
		}

		case F_BUILTIN: {
			const Builtin *bi = &g_builtins[f->op];
			i32 nargs = (i32)strlen(bi->args);
			char pname[64];
			if (f->phase == 0) {
				if (f->k != nargs) {
					if (nargs == 0) err_node(f->node, "%s takes no values: (%s)", bi->name, bi->name);
					else if (nargs == 1) err_node(f->node, "%s takes one value: (%s %s)", bi->name, bi->name, bi->params);
					else err_node(f->node, "%s takes %ld values: (%s %s)", bi->name, (long)nargs, bi->name, bi->params);
					finish(T_ERR);
					break;
				}
				if (!nargs) {
					emit(bi->op, bi->a, bi->b, line);
					finish(code_type(bi->ret));
					break;
				}
				f->k = 0;
				f->c = 0;
				f->phase = 1;
				/* sqrt and the like work on f32 too: an f32 in, an f32 out, as in (set p.x (sqrt d)) */
				push_child(f->cur, (bi->op == OP_MATH || bi->op == OP_MATH2) && f->want == T_F32 ? (Ty)T_F32 : code_type(bi->args[0]));
				break;
			}
			t = pop_type();
			if (!f->k && (bi->op == OP_MATH || bi->op == OP_MATH2) && t == T_F32) f->c = 4; /* the f32 one */
			if (bi->args[f->k] == 'x' && (is_struct(t) || is_enum(t))) { /* len of a struct or an enum: its len */
				i = hook(t, "len");
				if (i < 0)
					err_node(f->cur, "len on %s calls %s.len, which is not there: (defn %s.len ((x : %s)) : int ...)", tn(t), tn(t), tn(t), tn(t));
				else if (g_fns[i].nparams != 1 || g_fns[i].params[0] != t || g_fns[i].ret != T_INT)
					err_node(f->cur, "len on %s calls %s, which must take one %s and return int", tn(t), g_fns[i].name, tn(t));
				else if (gem_use(f->cur, g_fns[i].gem))
					emit(OP_CALL, i, 0, line);
				finish(T_INT);
				break;
			}
			if (!f->k && (bi->op == OP_LEN_S || bi->op == OP_BYTE)) f->b = borrow_last(f->cur, next_of(f->cur));
			t2 = f->c == 4 ? (Ty)T_F32 : code_type(bi->args[f->k]);
			if (bi->args[f->k] == 'x') {
				if (is_vec(t)) f->c = 1; /* len of a vec */
				else if (is_map(t)) f->c = 2;
				else if (is_pool(t)) f->c = 3;
				else if (t != T_STR && t != T_ERR) err_node(f->cur, "len takes a string, a vec, a map or a pool, not %s", tn(t));
			} else if (t != T_ERR && t != t2 && !(bi->op == OP_GFX && t2 == T_FLOAT && t == T_F32)) { /* drawing takes f32s too */
				if (nargs == 1) err_node(f->cur, "%s takes %s, not %s", bi->name, a_type(t2), tn(t));
				else err_node(f->cur, "%s: %s is %s, it must be %s", bi->name, param_name(bi, f->k, pname), tn(t), tn(t2));
			}
			f->k++;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) { push_child(f->cur, f->c == 4 ? (Ty)T_F32 : code_type(bi->args[f->k])); break; }
			if (f->c == 4) { /* worked out as a float, then rounded to the f32 it gives */
				emit(bi->op, bi->a, bi->b, line);
				emit(OP_F32, 0, 0, line);
				finish(T_F32);
				break;
			}
			emit(f->c == 1 ? OP_VEC_LEN : f->c == 2 ? OP_MAP_LEN : f->c == 3 ? OP_POOL_LEN : bi->op,
			     bi->op == OP_LEN_S || bi->op == OP_BYTE ? f->b : bi->a, f->c ? 0 : bi->b, line);
			finish(code_type(bi->ret));
			break;
		}

		case F_CONV: /* (TYPE x): x as another number type */
			if (f->phase == 0) {
				if (f->k != 1) {
					err_node(f->node, "%s takes one value: (%s x)", tn((Ty)f->a), tn((Ty)f->a));
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				push_child(f->cur, g_nodes[f->cur].kind == N_INT && g_nodes[f->cur].big ? (Ty)f->a : (Ty)T_NONE); /* (u64 0xff...ff) */
				break;
			}
			t = pop_type();
			if (t != T_ERR && !numeric(t)) err_node(f->cur, "%s converts a number, not %s", tn((Ty)f->a), tn(t));
			else if (t != T_ERR) emit_convert(t, (Ty)f->a, line);
			finish((Ty)f->a);
			break;

		case F_CALL: {
			Fn *fn = &g_fns[f->a];
			if (f->phase == 0) {
				if (f->k != fn->nparams) {
					err_node(f->node, NAME_FMT " takes %ld argument%s, got %ld", fn->name, (long)fn->nparams,
					         fn->nparams == 1 ? "" : "s", (long)f->k);
					finish(T_ERR);
					break;
				}
				f->k = 0;
				if (f->cur < 0 && f->op) { /* (coroutine (f)): a coroutine runs it */
					Ty tt = intern_wrap(K_CORO, fn->ret);
					emit(OP_CORO_NEW, f->a, tt, line);
					finish(tt);
					break;
				}
				if (f->cur < 0) {
					if (tail_ok(f, fn->ret)) emit_tail_releases(line);
					emit(tail_ok(f, fn->ret) ? OP_TAILCALL : OP_CALL, f->a, 0, line);
					finish(fn->ret);
					break;
				}
				f->phase = 1;
				push_child(f->cur, fn->params[0]);
				break;
			}
			t = pop_type();
			if (t != T_ERR && t != fn->params[f->k])
				err_node(f->cur, NAME_FMT ": argument %ld is %s, it must be %s", fn->name, (long)(f->k + 1), tn(t), tn(fn->params[f->k]));
			f->k++;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) { push_child(f->cur, fn->params[f->k]); break; }
			if (f->op) { /* (coroutine (f x)): a coroutine runs it */
				Ty tt = intern_wrap(K_CORO, fn->ret);
				emit(OP_CORO_NEW, f->a, tt, line);
				finish(tt);
				break;
			}
			if (tail_ok(f, fn->ret)) emit_tail_releases(line); /* the arguments are on the stack: the locals can go */
			emit(tail_ok(f, fn->ret) ? OP_TAILCALL : OP_CALL, f->a, 0, line);
			finish(fn->ret);
			break;
		}

		case F_RETURN: /* (return value), or (return) where the function returns unit: out of it now */
			if (f->phase == 0) {
				const char *why = g_fn_base < 0 ? "return is for the body of a function, a defn or a fn" : return_blocked();
				if (!why && f->k > 1) why = "return takes one value, or none in a function that returns unit";
				if (!why && g_fn_ret == T_UNIT && f->k) why = "this function returns unit, so its return takes no value: (return)";
				if (!why && g_fn_ret != T_UNIT && g_fn_ret != T_ERR && !f->k) why = "this function returns a value: (return x)";
				if (why) {
					err_node(f->node, "%s", why);
					finish(T_ERR);
					break;
				}
				f->depth = g_depth;
				if (f->k) {
					f->phase = 1;
					push_child(f->cur, g_fn_ret);
					break;
				}
				t = T_UNIT;
			} else {
				t = pop_type();
				if (t != T_ERR && g_fn_ret != T_ERR && t != g_fn_ret)
					err_node(f->cur, "return: this function returns %s, the value is %s", tn(g_fn_ret), tn(t));
			}
			emit_tail_releases(line);
			emit_ret(g_fn_ret, line);
			g_depth = f->depth;
			finish(T_ERR); /* it gives no value: it fits wherever it stands (what follows it never runs) */
			break;

		case F_ASSERT: /* (assert c) or (assert c "why"): when c is false, a runtime error with c's text, or with why */
			if (f->phase == 0) {
				i32 msg = next_of(f->cur);
				if (f->k < 1 || f->k > 2 || (msg >= 0 && g_nodes[msg].kind != N_STR)) {
					err_node(f->node, "assert takes a condition and a message if you like: (assert (> hp 0) \"hp went below zero\")");
					finish(T_ERR);
					break;
				}
				f->mark = g_ntjumps;
				f->phase = 1;
				push_cond(f->cur, 1, 1, CC_ASSERT); /* true: on, over the failure */
				break;
			}
			{
				char text[320];
				i32 msg = next_of(f->cur);
				size_t at;
				if (msg >= 0) {
					sprintf(text, "%.300s", g_nodes[msg].text);
				} else {
					strcpy(text, "assertion failed: ");
					at = strlen(text);
					nprint(g_nodes, f->cur, text, sizeof(text), &at);
				}
				emit(OP_FAIL, add_str(text, strlen(text)), 0, line);
			}
			aim_jumps(g_tjumps, &g_ntjumps, f->mark, g_ncode);
			finish(T_UNIT);
			break;

		case F_CORO: { /* poll, sleep, yield, step-coroutines, run-coroutines, done?, coroutine-time - in g_coro_names' order */
			static const Op ops[] = { OP_CORO_POLL, OP_SLEEP, OP_YIELD, OP_CORO_STEP, OP_CORO_RUN, OP_CORO_DONE, OP_CORO_TIME };
			static const u8 nargs[] = { 1, 1, 0, 1, 0, 1, 0 };
			int secs = f->op == 1 || f->op == 3, coro = f->op == 0 || f->op == 5;
			t = T_NONE;
			if (f->phase == 0) {
				if (f->k != nargs[f->op]) {
					err_node(f->node, "%s takes %s: %s", g_coro_names[f->op], nargs[f->op] ? "one value" : "nothing", g_coro_usage[f->op]);
					finish(T_ERR);
					break;
				}
				if (nargs[f->op]) {
					f->phase = 1;
					push_child(f->cur, secs ? (Ty)T_FLOAT : (Ty)T_NONE);
					break;
				}
			} else {
				t = pop_type();
				if (secs && t != T_FLOAT && t != T_ERR) {
					err_node(f->cur, "%s takes seconds as a float, like %s; this is %s", g_coro_names[f->op], g_coro_usage[f->op], tn(t));
					t = T_ERR;
				}
				if (coro && !is_coro(t) && t != T_ERR) {
					err_node(f->cur, "%s takes a coroutine, which (coroutine (f ...)) makes; this is %s", g_coro_names[f->op], tn(t));
					t = T_ERR;
				}
				if (f->op == 0 && t != T_ERR && elem_of(t) == T_UNIT) {
					err_node(f->node, "poll gives what a coroutine's function returns, and this one returns nothing; (done? co) says whether it ended");
					t = T_ERR;
				}
				if (t == T_ERR) { finish(T_ERR); break; }
			}
			emit(ops[f->op], f->op == 0 ? (i32)t : 0, 0, line);
			finish(f->op == 0 ? intern_option(elem_of(t)) : f->op == 3 ? (Ty)T_INT : f->op == 5 ? (Ty)T_BOOL : f->op == 6 ? (Ty)T_FLOAT : (Ty)T_UNIT);
			break;
		}

		case F_NEW: { /* (Vec2 1.0 2.0) in field order, or (Vec2 :y 2.0 :x 1.0) by name; the fields left out take their defaults */
			const TyInfo *ty = &g_tys[f->a];
			const Field *fd;
			i32 value;
			if (f->phase == 0) {
				char names[256];
				if (ty->broken) { finish(T_ERR); break; }
				f->op = (u8)(f->cur >= 0 && g_nodes[f->cur].kind == N_KEY);
				for (i = f->k; !f->op && i < ty->n && g_fields[ty->first + i].def >= 0; i++) {}
				for (value = 0; value < ty->n && g_fields[ty->first + value].def < 0; value++) {} /* a field with a default? */
				if (!f->op && (f->k > ty->n || (i < ty->n && value == ty->n))) {
					err_node(f->node, "%s has %ld field%s (%s), got %ld value%s", ty->name, (long)ty->n, ty->n == 1 ? "" : "s",
					         names_of((Ty)f->a, names, sizeof(names)), (long)f->k, f->k == 1 ? "" : "s");
					finish(T_ERR);
					break;
				}
				if (!f->op && i < ty->n) {
					err_node(f->node, "%s has %ld field%s (%s), got %ld value%s; %s has no default, so it must be given", ty->name, (long)ty->n,
					         ty->n == 1 ? "" : "s", names_of((Ty)f->a, names, sizeof(names)), (long)f->k, f->k == 1 ? "" : "s",
					         g_fields[ty->first + i].name);
					finish(T_ERR);
					break;
				}
				if (f->op && !check_named(f->node, f->cur, (Ty)f->a)) { finish(T_ERR); break; }
				f->c = f->cur; /* named: the first key, to find the fields left out at the end */
				f->k = 0;
				if (f->op) { /* named values go to a scratch struct in the frame, then the whole struct is pushed */
					f->mark2 = g_slots;
					f->b = new_slots(ty->cells);
				}
				if (f->cur < 0) { /* no values: every field takes its default */
					new_defaults(f, line);
					finish((Ty)f->a);
					break;
				}
				f->phase = 1;
				fd = f->op ? field_of_key((Ty)f->a, f->cur) : &g_fields[ty->first];
				push_child(f->op ? next_of(f->cur) : f->cur, fd->type);
				break;
			}
			t = pop_type();
			fd = f->op ? field_of_key((Ty)f->a, f->cur) : &g_fields[ty->first + f->k];
			value = f->op ? next_of(f->cur) : f->cur;
			if (t != T_ERR && t != fd->type)
				err_node(value, "%s: field %s is %s, the value is %s", ty->name, fd->name, tn(fd->type), tn(t));
			if (f->op) emit_store(0, f->b + fd->offset, fd->type, line);
			f->k++;
			f->cur = next_of(value);
			if (f->cur >= 0) {
				fd = f->op ? field_of_key((Ty)f->a, f->cur) : &g_fields[ty->first + f->k];
				push_child(f->op ? next_of(f->cur) : f->cur, fd->type);
				break;
			}
			new_defaults(f, line);
			finish((Ty)f->a);
			break;
		}

		case F_SOME:
			if (f->phase == 0) {
				if (f->k != 1) { err_node(f->node, "some takes one value: (some 5)"); finish(T_ERR); break; }
				emit(OP_PUSH_K, 1, 0, line); /* the tag, then the value */
				f->phase = 1;
				push_child(f->cur, is_option(f->want) ? g_tys[f->want].elem : (Ty)T_NONE);
				break;
			}
			t = pop_type();
			if (t == T_UNIT) err_node(f->cur, "some needs a value; this is unit");
			finish(t == T_ERR || t == T_UNIT ? (Ty)T_ERR : intern_option(t));
			break;

		case F_MATCH:
			switch (f->phase) {
			case 0:
				if (f->k < 2) {
					err_node(f->node, "match takes a value and its arms: (match x (pattern body...) ...)");
					finish(T_ERR);
					break;
				}
				f->mark2 = g_slots;
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			case 1:
				t = pop_type();
				if (t == T_ERR || g_tys[t].broken) { finish(T_ERR); break; }
				if (!matchable(t)) {
					err_node(f->cur, "match works on bool, int, string, enum and option values, not %s", tn(t));
					finish(T_ERR);
					break;
				}
				f->rt = t;
				f->op = (u8)check_arms(f->node, next_of(f->cur), t);
				f->a = new_slots(cells(t)); /* the matched value stays here while the arms look at it */
				emit_store(0, f->a, t, line);
				f->depth = g_depth;
				f->c = g_npatches;
				f->mark = g_nlocals;
				f->k = 0; /* becomes 1 when two arms disagree */
				f->cur = next_of(f->cur);
				start_arm(f);
				break;
			default: /* an arm's body is done */
				locals_end(f->mark); /* the x of its (some x) */
				t = pop_type();
				if (f->want == T_UNIT) {
					emit_pop(t, line);
					t = T_UNIT;
				}
				if (f->res == T_NONE || f->res == T_ERR) {
					f->res = t;
				} else if (t != f->res && t != T_ERR) {
					err_node(f->cur, "match: this arm is %s, the arms above are %s; every arm must give the same type", tn(t), tn(f->res));
					f->k = 1;
				}
				if (next_of(f->cur) >= 0) {
					if (g_npatches == MAX_WORK) die("too many match arms");
					g_patches[g_npatches++] = emit(OP_JMP, -1, 0, line);
					if (f->b >= 0) patch(f->b, g_ncode);
					g_depth = f->depth;
					f->cur = next_of(f->cur);
					start_arm(f);
					break;
				}
				if (f->b >= 0) patch(f->b, g_ncode);
				for (i = f->c; i < g_npatches; i++) patch(g_patches[i], g_ncode);
				emit_release(0, f->a, f->rt, line);
				g_npatches = f->c;
				g_nlocals = f->mark;
				g_slots = f->mark2;
				t = f->res == T_NONE ? (Ty)T_UNIT : f->res;
				g_depth = f->depth + cells(t);
				finish(f->k ? (Ty)T_ERR : t);
				break;
			}
			break;

		case F_VEC: /* (vec a b c), or (vec) when the expected type says what it holds */
			if (f->phase == 0) {
				f->rt = is_vec(f->want) ? g_tys[f->want].elem : (Ty)T_NONE; /* the element type, once known */
				if (f->cur < 0) {
					if (f->rt == T_NONE) {
						if (f->want != T_ERR)
							err_node(f->node, "an empty vec needs to know what it holds; give it a type, like (let ((v : (vec int) (vec))) ...)");
						finish(T_ERR);
						break;
					}
					emit(OP_VEC_NEW, 0, f->want, line);
					finish(f->want);
					break;
				}
				f->k = 0;
				f->phase = 1;
				push_child(f->cur, f->rt);
				break;
			}
			t = pop_type();
			if (t == T_UNIT) {
				err_node(f->cur, "a vec cannot hold unit");
				t = T_ERR;
			}
			if (f->rt == T_NONE) f->rt = t; /* the first element decides */
			else if (t != T_ERR && f->rt != T_ERR && t != f->rt)
				err_node(f->cur, "vec: element %ld is %s, the vec holds %s", (long)(f->k + 1), tn(t), tn(f->rt));
			f->k++;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) { push_child(f->cur, f->rt == T_ERR ? (Ty)T_NONE : f->rt); break; }
			t = f->rt == T_ERR ? (Ty)T_ERR : intern_wrap(K_VEC, f->rt);
			emit(OP_VEC_NEW, f->k, t, line);
			finish(t);
			break;

		case F_VECOP: { /* (push v x) (get v i) (put v i x) (pop v); (get p h) (put p h x) (spawn p x) on a pool */
			const char *what = g_vecop_names[f->op];
			Ty elem = elem_of(f->rt);
			int pool = is_pool(f->rt);
			if (f->phase == 0) {
				if (f->k != g_vecop_args[f->op]) {
					err_node(f->node, "%s takes %ld value%s: %s", what, (long)g_vecop_args[f->op], g_vecop_args[f->op] == 1 ? "" : "s",
					         g_vecop_usage[f->op]);
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			t = pop_type();
			if (f->phase == 1 && (f->op == VO_GET || f->op == VO_PUT) && (is_struct(t) || is_enum(t))) { /* its type's get or put */
				i = hook(t, what);
				if (i < 0)
					err_node(f->cur, "%s on %s calls %s.%s, which is not there: (defn %s.%s ((x : %s) (i : ...)%s)%s ...)", what, tn(t), tn(t), what,
					         tn(t), what, tn(t), f->op == VO_PUT ? " (value : ...)" : "", f->op == VO_PUT ? "" : " : ...");
				else if (g_fns[i].nparams != g_vecop_args[f->op] || g_fns[i].params[0] != t || (f->op == VO_PUT) != (g_fns[i].ret == T_UNIT))
					err_node(f->cur, "%s on %s calls %s, which must take %s and %s", what, tn(t), g_fns[i].name,
					         f->op == VO_PUT ? "three values, the first of that type," : "two values, the first of that type,",
					         f->op == VO_PUT ? "return unit" : "return a value");
				else if (gem_use(f->cur, g_fns[i].gem)) { /* the rest is a call of it, with this value as its first argument */
					push_type(t);
					f->form = F_CALL;
					f->a = i;
					f->op = 0;
					f->k = 0;
					break;
				}
				finish(T_ERR);
				break;
			}
			if (f->phase == 1) { /* the vec or the pool */
				f->b = borrow_last(f->cur, next_of(f->cur));
				pool = is_pool(t);
				if (t != T_ERR && pool && (f->op == VO_PUSH || f->op == VO_POP)) {
					err_node(f->cur, f->op == VO_PUSH ? "push works on a vec; a pool takes (spawn p x)"
					                                   : "pop works on a vec; take an element out of a pool with (remove p h)");
					t = T_ERR;
				} else if (t != T_ERR && !pool && f->op == VO_SPAWN) {
					err_node(f->cur, "spawn: the first value must be a pool, this is %s", tn(t));
					t = T_ERR;
				} else if (t != T_ERR && !pool && !is_vec(t)) {
					err_node(f->cur, "%s: the first value must be a vec%s, this is %s", what, f->op == VO_GET || f->op == VO_PUT ? " or a pool" : "", tn(t));
					t = T_ERR;
				}
				f->rt = t;
				elem = elem_of(t);
				if (f->op == VO_POP) {
					emit(OP_VEC_POP, cells(elem), f->b, line);
					finish(t == T_ERR ? (Ty)T_ERR : intern_option(elem));
					break;
				}
				f->phase = 2;
				f->cur = next_of(f->cur);
				if (f->op == VO_PUSH || f->op == VO_SPAWN) push_child(f->cur, t == T_ERR ? (Ty)T_NONE : elem);
				else push_child(f->cur, pool ? intern_wrap(K_HANDLE, elem) : (Ty)T_NONE);
				break;
			}
			if (f->phase == 2 && f->op != VO_PUSH && f->op != VO_SPAWN) { /* the index, or a pool's handle */
				Ty want = pool ? intern_wrap(K_HANDLE, elem) : (Ty)T_INT;
				if (t != want && t != T_ERR && f->rt != T_ERR) {
					if (pool) err_node(f->cur, "%s: a %s takes a %s, this is %s", what, tn(f->rt), tn(want), tn(t));
					else err_node(f->cur, "%s: the index is %s, it must be int", what, tn(t));
				}
				if (f->op == VO_GET) {
					emit(pool ? OP_POOL_GET : OP_VEC_GET, cells(elem), f->b, line);
					finish(elem);
					break;
				}
				f->phase = 3;
				f->cur = next_of(f->cur);
				push_child(f->cur, f->rt == T_ERR ? (Ty)T_NONE : elem);
				break;
			}
			/* the element of a push, a put or a spawn */
			if (t != T_ERR && f->rt != T_ERR && t != elem)
				err_node(f->cur, "%s: the %s holds %s, this value is %s", what, pool ? "pool" : "vec", tn(elem), tn(t));
			if (has_refs(elem)) escapes(what, next_of(first_of(f->node)), f->cur);
			if (f->op == VO_SPAWN) {
				emit(OP_POOL_SPAWN, cells(elem), f->b, line);
				finish(f->rt == T_ERR ? (Ty)T_ERR : intern_wrap(K_HANDLE, elem));
				break;
			}
			emit(pool ? OP_POOL_PUT : f->op == VO_PUSH ? OP_VEC_PUSH : OP_VEC_PUT, cells(elem), f->b, line);
			finish(T_UNIT);
			break;
		}

		case F_FOR: /* (for (x v) body...) over a vec, a map's keys, a pool's handles or a struct with len and get;
		             * (for (i from to) body...) over from, from + 1, ..., to - 1 */
			switch (f->phase) {
			case 0: {
				i32 b = f->cur, name = first_of(b), from = next_of(name);
				if (b < 0 || g_nodes[b].kind != N_LIST || !is_sym(name) || from < 0 || count_from(from) > 2) {
					err_node(b < 0 ? f->node : b, "for takes (x v) or (i from to), then a body: (for (i 0 10) (println \"%%d\" i))");
					finish(T_ERR);
					break;
				}
				if (strcmp(g_nodes[name].text, "_") && (is_reserved(g_nodes[name].text) || strchr(g_nodes[name].text, '.')))
					err_node(name, NAME_FMT " cannot be a variable name", g_nodes[name].text); /* _: no name, it only repeats */
				f->op = (u8)(next_of(from) >= 0); /* 1: a range */
				f->mark = g_nlocals;
				f->mark2 = g_slots;
				f->phase = 1;
				push_child(from, T_NONE);
				break;
			}
			case 1: {
				i32 b = f->cur, name = first_of(b);
				t = pop_type();
				if (f->op) { /* i, then the end, each in a slot */
					if (t != T_INT && t != T_ERR) err_node(next_of(name), "for: a range starts at an int, this is %s", tn(t));
					f->a = new_slots(2);
					emit(OP_STORE_L, f->a, 0, line);
					f->phase = 2;
					push_child(next_of(next_of(name)), T_INT);
					break;
				}
				if (is_pool(t)) { /* over the handles of a pool's elements, in slot order */
					f->rt = intern_wrap(K_HANDLE, elem_of(t));
					f->res = t;
					f->a = new_slots(3); /* the pool, the slot, the handle */
					emit(OP_STORE_L, f->a, 0, line);
					emit(OP_PUSH_I, add_int(0), 0, line);
					emit(OP_STORE_L, f->a + 1, 0, line);
					f->c = g_ncode;
					emit(OP_LOAD_L, f->a, 0, line); /* the hidden slot holds the pool; this read borrows it */
					emit(OP_LOAD_L, f->a + 1, 0, line);
					emit(OP_POOL_NEXT, 1, 0, line);
					emit(OP_STORE_L, f->a + 2, 0, line);
					emit(OP_STORE_L, f->a + 1, 0, line);
					emit(OP_LOAD_L, f->a + 1, 0, line);
					emit(OP_PUSH_I, add_int(0), 0, line);
					emit(OP_GE_I, 0, 0, line);
					f->b = emit(OP_JF, -1, 0, line);
					add_local(name, LK_FOR, g_nodes[name].text, g_nodes[name].stamp, f->rt, f->a + 2);
					f->phase = 3;
					push_body(next_of(b), 0, T_UNIT);
					break;
				}
				if (is_struct(t) || is_enum(t)) { /* over a struct or an enum: its len, and its get with an int */
					i32 fl = hook(t, "len"), fg = hook(t, "get"), idx;
					if (fl < 0 || fg < 0 || g_fns[fl].nparams != 1 || g_fns[fl].params[0] != t || g_fns[fl].ret != T_INT || g_fns[fg].nparams != 2 ||
					    g_fns[fg].params[0] != t || g_fns[fg].params[1] != T_INT || g_fns[fg].ret == T_UNIT) {
						err_node(next_of(name), "for over %s goes through %s.len and %s.get: (defn %s.len ((x : %s)) : int ...) and (defn %s.get ((x : %s) (i : int)) : ... ...)",
						         tn(t), tn(t), tn(t), tn(t), tn(t), tn(t), tn(t));
						t = T_ERR;
					} else if (gem_use(next_of(name), g_fns[fl].gem) && gem_use(next_of(name), g_fns[fg].gem)) {
						f->op = 2;
						f->res = t;
						f->rt = g_fns[fg].ret;
						f->a = new_slots(cells(t) + 1 + cells(f->rt)); /* the value, the index, the element */
						idx = f->a + cells(t);
						emit_store(0, f->a, t, line);
						emit(OP_PUSH_I, add_int(0), 0, line);
						emit(OP_STORE_L, idx, 0, line);
						f->c = g_ncode;
						emit(OP_LOAD_L, idx, 0, line);
						emit_copy(0, f->a, t, line); /* each call gets a copy, as any call does */
						emit(OP_CALL, fl, 0, line);
						emit(OP_LT_I, 0, 0, line);
						f->b = emit(OP_JF, -1, 0, line);
						emit_copy(0, f->a, t, line);
						emit(OP_LOAD_L, idx, 0, line);
						emit(OP_CALL, fg, 0, line);
						emit_store(0, idx + 1, f->rt, line);
						add_local(name, LK_FOR, g_nodes[name].text, g_nodes[name].stamp, f->rt, idx + 1);
						f->phase = 3;
						push_body(next_of(b), 0, T_UNIT);
						break;
					} else {
						t = T_ERR;
					}
				}
				if (is_map(t)) { /* over a map's keys, in the order they went in */
					t = intern_wrap(K_VEC, key_of(t));
					emit(OP_MAP_KEYS, t, 0, line);
				}
				if (t != T_ERR && !is_vec(t)) {
					err_node(next_of(name), "for: (x v) goes over a vec, the keys of a map or the handles of a pool, this is %s; a range is (i from to)", tn(t));
					t = T_ERR;
				}
				f->rt = elem_of(t);
				f->res = t;
				f->a = new_slots(2 + cells(f->rt)); /* the vec, the index, the element */
				emit(OP_STORE_L, f->a, 0, line);
				emit(OP_PUSH_I, add_int(0), 0, line);
				emit(OP_STORE_L, f->a + 1, 0, line);
				f->c = g_ncode;
				emit(OP_LOAD_L, f->a + 1, 0, line);
				emit(OP_LOAD_L, f->a, 0, line); /* the hidden slot holds the vec; these reads borrow it */
				emit(OP_VEC_LEN, 1, 0, line);
				emit(OP_LT_I, 0, 0, line);
				f->b = emit(OP_JF, -1, 0, line);
				emit(OP_LOAD_L, f->a, 0, line);
				emit(OP_LOAD_L, f->a + 1, 0, line);
				emit(OP_VEC_GET, cells(f->rt), 1, line);
				emit_store(0, f->a + 2, f->rt, line);
				add_local(name, LK_FOR, g_nodes[name].text, g_nodes[name].stamp, f->rt, f->a + 2);
				f->phase = 3;
				push_body(next_of(b), 0, T_UNIT);
				break;
			}
			case 2: {
				i32 name = first_of(f->cur);
				t = pop_type();
				if (t != T_INT && t != T_ERR) err_node(next_of(next_of(name)), "for: a range ends at an int, this is %s", tn(t));
				emit(OP_STORE_L, f->a + 1, 0, line);
				f->c = g_ncode;
				emit(OP_LOAD_L, f->a, 0, line);
				emit(OP_LOAD_L, f->a + 1, 0, line);
				emit(OP_LT_I, 0, 0, line);
				f->b = emit(OP_JF, -1, 0, line);
				add_local(name, LK_FOR, g_nodes[name].text, g_nodes[name].stamp, T_INT, f->a);
				f->phase = 3;
				push_body(next_of(f->cur), 0, T_UNIT);
				break;
			}
			default:
				i = f->op == 1 ? f->a : f->op == 2 ? f->a + cells(f->res) : f->a + 1; /* the index */
				(void)pop_type();
				if (f->op != 1) emit_release(0, i + 1, f->rt, line); /* this round's element */
				emit(OP_INC_L, i, 0, line);
				emit(OP_JMP, f->c, 0, line);
				patch(f->b, g_ncode);
				if (f->op != 1) emit_release(0, f->a, f->res, line); /* the vec, the pool, the struct */
				locals_end(f->mark);
				g_slots = f->mark2;
				finish(T_UNIT);
				break;
			}
			break;

		case F_SHPIPE: /* (pipe cmd...) a pipeline, its output; (pipe-in text cmd...) the same with text on its stdin */
			if (f->phase == 0) {
				if (f->k < 1 + f->op) {
					err_node(f->node, f->op ? "pipe-in takes the text and at least one command: (pipe-in text \"sort\")"
					                        : "pipe takes at least one command: (pipe \"ls\" \"wc -l\")");
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				f->a = f->k;
				f->k = 0;
				push_child(f->cur, T_STR);
				break;
			}
			t = pop_type();
			if (t != T_STR && t != T_ERR)
				err_node(f->cur, "%s: %s is %s, it must be a string", f->op ? "pipe-in" : "pipe", f->op && !f->k ? "the text" : "a command", tn(t));
			f->k++;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) { push_child(f->cur, T_STR); break; }
			emit(OP_SHPIPE, f->a, f->op, line);
			finish(T_STR);
			break;

		case F_MAP: /* (map (k v) ...), or (map) when the expected type says which */
			if (f->phase == 0) {
				i32 p;
				f->rt = is_map(f->want) ? key_of(f->want) : (Ty)T_NONE;
				f->res = is_map(f->want) ? elem_of(f->want) : (Ty)T_NONE;
				for (p = f->cur; p >= 0; p = next_of(p)) {
					if (g_nodes[p].kind != N_LIST || count_from(first_of(p)) != 2) {
						err_node(p, "a map entry is (key value), like (map (\"a\" 1) (\"b\" 2))");
						finish(T_ERR);
						break;
					}
				}
				if (p >= 0) break;
				if (f->cur < 0) {
					if (f->rt == T_NONE) {
						if (f->want != T_ERR)
							err_node(f->node, "an empty map needs to know what it holds; give it a type, like (let ((m : (map string int) (map))) ...)");
						finish(T_ERR);
						break;
					}
					emit(OP_MAP_NEW, 0, f->want, line);
					finish(f->want);
					break;
				}
				f->k = 0;
				f->phase = 1;
				push_child(first_of(f->cur), f->rt);
				break;
			}
			t = pop_type();
			if (f->phase == 1) { /* a key */
				if (f->rt == T_NONE) {
					if (t != T_ERR && !key_type(t)) err_node(first_of(f->cur), "a map's key is an int, a bool, a string or an enum value, not %s", tn(t));
					f->rt = key_type(t) ? t : (Ty)T_ERR;
				} else if (t != T_ERR && f->rt != T_ERR && t != f->rt) {
					err_node(first_of(f->cur), "map: key %ld is %s, the keys are %s", (long)(f->k + 1), tn(t), tn(f->rt));
				}
				f->phase = 2;
				push_child(next_of(first_of(f->cur)), f->res == T_ERR ? (Ty)T_NONE : f->res);
				break;
			}
			if (t == T_UNIT) { /* a value */
				err_node(next_of(first_of(f->cur)), "a map cannot hold unit");
				t = T_ERR;
			}
			if (f->res == T_NONE) f->res = t;
			else if (t != T_ERR && f->res != T_ERR && t != f->res)
				err_node(next_of(first_of(f->cur)), "map: value %ld is %s, the values are %s", (long)(f->k + 1), tn(t), tn(f->res));
			f->k++;
			f->cur = next_of(f->cur);
			if (f->cur >= 0) {
				f->phase = 1;
				push_child(first_of(f->cur), f->rt == T_ERR ? (Ty)T_NONE : f->rt);
				break;
			}
			t = f->rt == T_ERR || f->res == T_ERR ? (Ty)T_ERR : intern_map(f->rt, f->res);
			emit(OP_MAP_NEW, f->k, t, line);
			finish(t);
			break;

		case F_MAPOP: { /* (insert m k v) (lookup m k) (has m k) (remove m k) (keys m) (values m); on a pool lookup,
		                 * has and remove take a handle */
			const char *what = g_mapop_names[f->op];
			int pool = is_pool(f->rt);
			Ty val = elem_of(f->rt), key = pool ? intern_wrap(K_HANDLE, val) : key_of(f->rt);
			if (f->phase == 0) {
				if (f->k != g_mapop_args[f->op]) {
					err_node(f->node, "%s takes %ld value%s: %s", what, (long)g_mapop_args[f->op], g_mapop_args[f->op] == 1 ? "" : "s",
					         g_mapop_usage[f->op]);
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			t = pop_type();
			if (f->phase == 1) { /* the map or the pool */
				f->b = borrow_last(f->cur, next_of(f->cur));
				pool = is_pool(t);
				if (t != T_ERR && pool && (f->op == MO_INSERT || f->op == MO_KEYS || f->op == MO_VALUES)) {
					err_node(f->cur, f->op == MO_INSERT ? "insert works on a map; a pool takes (spawn p x)"
					                                     : "%s works on a map; go over a pool's handles with (for (h p) ...)", what);
					t = T_ERR;
				} else if (t != T_ERR && !pool && !is_map(t)) {
					err_node(f->cur, "%s: the first value must be a map%s, this is %s", what,
					         f->op == MO_LOOKUP || f->op == MO_HAS || f->op == MO_REMOVE ? " or a pool" : "", tn(t));
					t = T_ERR;
				}
				f->rt = t;
				val = elem_of(t);
				key = pool ? intern_wrap(K_HANDLE, val) : key_of(t);
				if (f->op == MO_KEYS || f->op == MO_VALUES) {
					t = t == T_ERR ? (Ty)T_ERR : intern_wrap(K_VEC, f->op == MO_KEYS ? key : val);
					emit(g_mapop_ops[f->op], t, f->b, line);
					finish(t);
					break;
				}
				f->phase = 2;
				f->cur = next_of(f->cur);
				push_child(f->cur, t == T_ERR ? (Ty)T_NONE : key);
				break;
			}
			if (f->phase == 2) { /* the key, or a pool's handle */
				if (t != T_ERR && f->rt != T_ERR && t != key) {
					if (pool) err_node(f->cur, "%s: a %s takes a %s, this is %s", what, tn(f->rt), tn(key), tn(t));
					else err_node(f->cur, "%s: the keys are %s, this one is %s", what, tn(key), tn(t));
				}
				if (f->op != MO_INSERT) { /* the key is only looked up: it can be borrowed too */
					if (borrow_last(f->cur, -1)) f->b |= 2;
					if (f->op == MO_HAS) emit(pool ? OP_POOL_HAS : OP_MAP_HAS, f->b, 0, line);
					else if (pool) emit(f->op == MO_LOOKUP ? OP_POOL_LOOKUP : OP_POOL_REMOVE, cells(val), f->b, line);
					else emit(g_mapop_ops[f->op], cells(val), f->b, line);
					finish(f->rt == T_ERR ? (Ty)T_ERR : f->op == MO_HAS ? (Ty)T_BOOL : intern_option(val));
					break;
				}
				f->phase = 3;
				f->cur = next_of(f->cur);
				push_child(f->cur, f->rt == T_ERR ? (Ty)T_NONE : val);
				break;
			}
			if (t != T_ERR && f->rt != T_ERR && t != val) err_node(f->cur, "insert: the values are %s, this one is %s", tn(val), tn(t));
			if (has_refs(key)) escapes("insert", next_of(first_of(f->node)), next_of(next_of(first_of(f->node))));
			if (has_refs(val)) escapes("insert", next_of(first_of(f->node)), f->cur);
			emit(OP_MAP_INSERT, cells(val), f->b, line);
			finish(T_UNIT);
			break;
		}

		case F_POOL: /* (pool n): a pool of n slots; the expected type says what it holds */
			if (f->phase == 0) {
				if (f->k != 1) {
					err_node(f->node, "pool takes the number of slots: (pool 1000)");
					finish(T_ERR);
					break;
				}
				if (!is_pool(f->want) && f->want != T_ERR)
					err_node(f->node, "a pool needs to know what it holds; give it a type, like (def ps : (pool Particle) (pool 1000))");
				f->phase = 1;
				push_child(f->cur, T_INT);
				break;
			}
			t = pop_type();
			if (t != T_INT && t != T_ERR) err_node(f->cur, "pool: the number of slots is %s, it must be int", tn(t));
			emit(OP_POOL_NEW, 0, is_pool(f->want) ? f->want : (Ty)T_ERR, line);
			finish(is_pool(f->want) ? f->want : (Ty)T_ERR);
			break;

		case F_ARENA: /* (arena body...): what the body makes lives in an arena that ends with it */
			if (f->phase == 0) {
				emit(OP_ARENA_BEGIN, 0, 0, line);
				f->mark = g_nlocals; /* names from here on live inside it */
				f->phase = 1;
				push_body(f->cur, 1, f->want);
				break;
			}
			t = pop_type();
			for (i = f->cur; next_of(i) >= 0; i = next_of(i)) {}
			if (has_refs(t) && fresh(i))
				err_node(i, "this value is made in the arena, which ends here; (keep x) copies it out");
			emit(OP_ARENA_END, 0, 0, line);
			finish(t);
			break;

		case F_KEEP: /* (keep x): x stops holding what the innermost arena made, so it outlives it */
			if (f->phase == 0) {
				if (f->k != 1) {
					err_node(f->node, "keep takes one value: (keep x)");
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				push_child(f->cur, f->want);
				break;
			}
			t = pop_type();
			if (has_refs(t)) emit(OP_KEEP, t, 0, line);
			finish(t);
			break;

		case F_LAMBDA: /* (fn ((x : int)) : int body...): a function without a name, as a value */
			if (f->phase == 0) {
				if (!lambda_begin(f)) finish(T_ERR);
				break;
			}
			lambda_end(pop_type(), line);
			break;

		case F_CALLV: { /* (f args...) where f is a variable, a field or an expression of a function type */
			const TyInfo *ft;
			const char *what = f->op == 3 ? "this function" : g_nodes[first_of(f->node)].text;
			if (f->phase == 5) { /* an expression made the function: it waits in a slot while the arguments run */
				t = pop_type();
				if (!is_fn(t)) {
					if (t != T_ERR) err_node(first_of(f->node), "this is %s, not a function, so it cannot be called", tn(t));
					finish(T_ERR);
					break;
				}
				f->rt = t;
				f->c = new_slots(1);
				emit(OP_STORE_L, f->c, 0, line);
				f->phase = 0;
			}
			ft = &g_tys[f->rt];
			if (f->phase == 0) {
				if (f->k != ft->n) {
					err_node(f->node, "%.64s takes %ld argument%s, got %ld", what, (long)ft->n, ft->n == 1 ? "" : "s", (long)f->k);
					if (f->op == 3) g_slots = f->mark2;
					finish(T_ERR);
					break;
				}
				f->k = 0;
				f->phase = 1;
				if (f->cur >= 0) {
					push_child(f->cur, g_tparams[ft->first]);
					break;
				}
			} else {
				t = pop_type();
				if (t != T_ERR && t != g_tparams[ft->first + f->k])
					err_node(f->cur, "%.64s: argument %ld is %s, it must be %s", what, (long)(f->k + 1), tn(t), tn(g_tparams[ft->first + f->k]));
				f->k++;
				f->cur = next_of(f->cur);
				if (f->cur >= 0) {
					push_child(f->cur, g_tparams[ft->first + f->k]);
					break;
				}
			}
			/* the function value goes on top, owned: the call takes it (a closure becomes the callee's slot 0) */
			if (f->op == 3) emit(OP_LOAD_L, f->c, 0, line); /* the slot's value moves into the call */
			else emit_copy(f->op == 2 ? AT_GLOBAL : AT_LOCAL, f->c, f->rt, line);
			if (tail_ok(f, ft->elem)) emit_tail_releases(line); /* after the copy: the variable may be one of them */
			emit(tail_ok(f, ft->elem) ? OP_TAILCALL_V : OP_CALL_V, f->rt, 0, line);
			if (f->op == 3) g_slots = f->mark2;
			finish(ft->elem);
			break;
		}

		case F_DOT: { /* (. expr pos.x): a field of the value of any expression; (f x).pos.x reads as this */
			i32 path = next_of(f->cur), off = 0, slot;
			const char *p, *end;
			Ty ft;
			if (f->phase == 0) {
				if (f->k != 2 || !is_sym(path)) {
					err_node(f->node, "a field of an expression is (expr).name, or (. expr name)");
					finish(T_ERR);
					break;
				}
				f->phase = 1;
				push_child(f->cur, T_NONE);
				break;
			}
			ft = t = pop_type();
			for (p = g_nodes[path].text; ft != T_ERR; p = end + 1) {
				const Field *fd;
				char names[256];
				size_t len;
				end = strchr(p, '.');
				len = end ? (size_t)(end - p) : strlen(p);
				if (!is_struct(ft)) {
					if (p == g_nodes[path].text) err_node(path, "the value is %s, it has no fields", tn(ft));
					else err_node(path, "%.*s is %s, it has no fields", (int)(p - 1 - g_nodes[path].text), g_nodes[path].text, tn(ft));
					ft = T_ERR;
					break;
				}
				fd = find_field(ft, p, len);
				if (!fd) {
					char seg[64];
					sprintf(seg, "%.*s", (int)(len < 63 ? len : 63), p);
					if (g_tys[ft].broken) ;
					else if (!end && hook(ft, seg) >= 0) /* (v).normal: a method, called in parens */
						err_node(path, "%s has no field %s; %s.%s is a function, called in parens: ((expr).%s ...)", tn(ft), seg, tn(ft), seg, seg);
					else
						err_node(path, "%s has no field %.*s; its fields are %s", tn(ft), (int)(len < 64 ? len : 64), p,
						         names_of(ft, names, sizeof(names)));
					ft = T_ERR;
					break;
				}
				off += fd->offset;
				ft = fd->type;
				if (!end) break;
			}
			if (ft != T_ERR) { /* the whole value to scratch slots, a copy of the field back, the rest dies */
				slot = new_slots(cells(t));
				emit_store(0, slot, t, line);
				emit_copy(0, slot + off, ft, line);
				emit_release(0, slot, t, line);
				g_slots = slot;
			}
			finish(ft);
			break;
		}

		default:
			err_node(f->node, "internal: unknown form");
			finish(T_ERR);
			break;
		}
	}
}

static o compile_expr(i32 n, Ty want)
{
	i32 base = g_nwork;
	push_child(n, want);
	run_work(base);
}

/* new_name_ok NAME FORM WHAT: a new struct or enum gets a plain name that nothing else uses */
static int new_name_ok(i32 name, i32 form, const char *what)
{
	const char *s;
	if (!is_sym(name)) {
		err_node(name < 0 ? form : name, "%s needs a name: %s", what,
		         what[0] == 's' ? "(struct Vec2 (x : float) (y : float))" : "(enum Dir north south east west)");
		return 0;
	}
	s = g_nodes[name].text;
	if (strchr(s, '.')) { err_node(name, "a type name cannot contain a dot"); return 0; }
	if (is_reserved(s)) { err_node(name, NAME_FMT " is a built-in name", s); return 0; }
	if (find_fn(s) >= 0) { err_node(name, NAME_FMT " is already a function", s); return 0; }
	if (find_global(s) >= 0) { err_node(name, NAME_FMT " is already a global", s); return 0; }
	if (find_macro(s) >= 0) { err_node(name, NAME_FMT " is already a macro", s); return 0; }
	return 1;
}

/* new_type NAME KIND: register a struct or an enum; the caller fills in its fields or variants */
static Ty new_type(i32 name, u8 kind)
{
	TyInfo *t;
	if (g_ntys == MAX_TYS) die("too many types");
	t = &g_tys[g_ntys];
	memset(t, 0, sizeof(*t));
	t->name = arena_strndup(&g_perm, g_nodes[name].text, g_nodes[name].len);
	t->kind = kind;
	t->line = g_nodes[name].line;
	t->file = g_unit.file;
	t->gem = g_unit_gem;
	names_put(t->name)->type = g_ntys;
	return (Ty)g_ntys++;
}

/* same_type NAME OLD KIND FIRST N REPL: NAME is a type already; only the REPL may read its declaration again, and
 * only unchanged. The fields or variants just read are dropped either way */
static o same_type(i32 name, Ty old, u8 kind, i32 first, i32 n, int repl)
{
	TyInfo *ot = &g_tys[old];
	int same = repl && ot->kind == kind && ot->n == n;
	i32 i;
	for (i = 0; same && i < n; i++) {
		if (kind == K_STRUCT)
			same = !strcmp(g_fields[ot->first + i].name, g_fields[first + i].name) && g_fields[ot->first + i].type == g_fields[first + i].type;
		else
			same = !strcmp(g_variants[ot->first + i], g_variants[first + i]);
	}
	if (!same) err_node(name, NAME_FMT " is already a type (%s)", g_nodes[name].text, where(ot->file, ot->line));
	if (kind == K_STRUCT) g_nfields = first; else g_nvariants = first;
}

/* declared_below FROM NAME: a struct or enum called NAME comes after FROM at the top level */
static int declared_below(i32 from, const char *name)
{
	i32 n;
	for (n = next_of(from); n >= 0; n = next_of(n)) {
		if ((head_is(n, "struct") || head_is(n, "enum")) && sym_is(next_of(first_of(n)), name)) return 1;
	}
	return 0;
}

/* default_fn TYPE FIELD NODE: the function that gives FIELD of the struct TYPE its default value, the expression
 * NODE; it is compiled where the struct is (compile_defaults) and is not a name a program can use */
static i32 default_fn(Ty st, const Field *fd, i32 node)
{
	char name[300];
	Fn *fn;
	if (g_nfns == MAX_FNS) die("too many functions");
	fn = &g_fns[g_nfns];
	memset(fn, 0, sizeof(*fn));
	sprintf(name, "%.140s@%.140s", g_tys[st].name, fd->name);
	fn->name = arena_strndup(&g_perm, name, strlen(name));
	fn->ret = fd->type;
	fn->line = g_nodes[node].line;
	fn->file = g_unit.file;
	fn->entry = -1;
	fn->decl_unit = g_unit_id;
	fn->decl_node = node;
	fn->gem = g_unit_gem;
	return g_nfns++;
}

/* declare_struct STRUCT REPL: (struct Name (field : type) ...), a field with its default value (hp : int 100); a
 * field's type must be declared above, or be the struct itself inside a vec (a tree) */
static o declare_struct(i32 n, int repl)
{
	i32 name = next_of(first_of(n)), p, q, first = g_nfields, ncells = 0, nf = 0, bad = 0, i;
	Ty old, self;
	if (!new_name_ok(name, n, "struct")) return;
	old = find_named_type(g_nodes[name].text);
	self = old != T_NONE ? old : new_type(name, K_STRUCT); /* known by name now, for a (vec Self) field */
	for (p = next_of(name); p >= 0; p = next_of(p)) {
		i32 fname = first_of(p), colon = next_of(fname), ft = next_of(colon), base = ft;
		int in_vec = 0;
		Ty t;
		if (g_nodes[p].kind != N_LIST || !is_sym(fname) || !sym_is(colon, ":") || ft < 0 || next_of(next_of(ft)) >= 0) {
			err_node(p, "a field is (name : type), or (name : type default)");
			bad++;
			continue;
		}
		if (is_reserved(g_nodes[fname].text) || strchr(g_nodes[fname].text, '.')) {
			err_node(fname, NAME_FMT " cannot be a field name", g_nodes[fname].text);
			bad++;
			continue;
		}
		for (q = next_of(name); q != p && !(g_nodes[q].kind == N_LIST && sym_is(first_of(q), g_nodes[fname].text)); q = next_of(q)) {}
		if (q != p) {
			err_node(fname, "field " NAME_FMT " twice", g_nodes[fname].text);
			bad++;
			continue;
		}
		while (head_is(base, "option") || head_is(base, "vec") || head_is(base, "pool") || head_is(base, "handle")) {
			if (!head_is(base, "option")) in_vec = 1; /* behind a handle the size stays one cell */
			base = next_of(first_of(base));
		}
		if (sym_is(base, g_nodes[name].text) && !in_vec) {
			err_node(ft, NAME_FMT " cannot hold a " NAME_FMT ", not even in an option: its size would have no end; (vec "
			         NAME_FMT ") can", g_nodes[name].text, g_nodes[name].text, g_nodes[name].text);
			bad++;
			continue;
		}
		if (is_sym(base) && find_named_type(g_nodes[base].text) == T_NONE && declared_below(n, g_nodes[base].text)) {
			err_node(base, NAME_FMT " is declared below; declare a type above the structs that use it", g_nodes[base].text);
			bad++;
			continue;
		}
		t = parse_type(ft);
		if (t == T_UNIT) err_node(ft, "a field cannot be unit");
		if (t == T_UNIT || t == T_ERR) { bad++; continue; }
		if (g_nfields == MAX_FIELDS) die("too many struct fields");
		g_fields[g_nfields].name = arena_strndup(&g_perm, g_nodes[fname].text, g_nodes[fname].len);
		g_fields[g_nfields].type = t;
		g_fields[g_nfields].offset = ncells;
		g_fields[g_nfields].def = next_of(ft); /* the default's node for now; its function below */
		g_nfields++;
		ncells += cells(t);
		nf++;
	}
	if (old != T_NONE) {
		same_type(name, old, K_STRUCT, first, nf, repl);
		return;
	}
	g_tys[self].first = first;
	g_tys[self].n = nf;
	g_tys[self].cells = ncells;
	g_tys[self].broken = (u8)(bad > 0);
	for (i = first; i < first + nf; i++) {
		if (g_fields[i].def >= 0) g_fields[i].def = default_fn(self, &g_fields[i], g_fields[i].def);
	}
	set_leaves(self);
	for (i = self + 1; i < g_ntys; i++) { /* an (option Self) in a field's (vec ...) was made while Self had no size */
		if (g_tys[i].kind != K_OPTION) continue;
		g_tys[i].cells = 1 + cells(g_tys[i].elem);
		set_leaves((Ty)i);
	}
}

/* declare_enum ENUM REPL: (enum Name variant ...) */
static o declare_enum(i32 n, int repl)
{
	i32 name = next_of(first_of(n)), p, i, first = g_nvariants, nv = 0, bad = 0;
	Ty old, self;
	if (!new_name_ok(name, n, "enum")) return;
	if (next_of(name) < 0) {
		err_node(name, "enum " NAME_FMT " needs at least one variant: (enum Dir north south)", g_nodes[name].text);
		return;
	}
	for (p = next_of(name); p >= 0; p = next_of(p)) {
		if (!is_sym(p) || is_reserved(g_nodes[p].text) || strchr(g_nodes[p].text, '.')) {
			err_node(p, "a variant is a plain name, like north");
			bad++;
			continue;
		}
		for (i = first; i < g_nvariants && strcmp(g_variants[i], g_nodes[p].text); i++) {}
		if (i < g_nvariants) {
			err_node(p, "variant " NAME_FMT " twice", g_nodes[p].text);
			bad++;
			continue;
		}
		if (g_nvariants == MAX_VARIANTS) die("too many enum variants");
		g_variants[g_nvariants++] = arena_strndup(&g_perm, g_nodes[p].text, g_nodes[p].len);
		nv++;
	}
	if ((old = find_named_type(g_nodes[name].text)) != T_NONE) {
		same_type(name, old, K_ENUM, first, nv, repl);
		return;
	}
	self = new_type(name, K_ENUM);
	g_tys[self].first = first;
	g_tys[self].n = nv;
	g_tys[self].cells = 1;
	g_tys[self].broken = (u8)(bad > 0);
}

/* fn_shape DEFN NAME PARAMS RET BODY: the parts of (defn name ((p : type) ...) [: type] body...) */
static int fn_shape(i32 n, i32 *name, i32 *params, i32 *ret, i32 *body)
{
	*name = next_of(first_of(n));
	*params = next_of(*name);
	*ret = -1;
	*body = next_of(*params);
	if (!is_sym(*name)) { err_node(*name < 0 ? n : *name, "defn needs a name: (defn name ((x : int)) : int body)"); return 0; }
	if (*params < 0 || g_nodes[*params].kind != N_LIST) {
		err_node(*params < 0 ? n : *params, "defn " NAME_FMT " needs a parameter list, () when it takes none", g_nodes[*name].text);
		return 0;
	}
	if (sym_is(*body, ":")) {
		*ret = next_of(*body);
		*body = next_of(*ret);
	}
	return 1;
}

/* owner_ok NAME: NAME is Type.name - a function of a struct or an enum declared in this gem (Vec2.len): a method, or a
 * hook that a form calls for values of the type. 0 after an error it reported */
static int owner_ok(i32 name)
{
	const char *s = g_nodes[name].text, *dot = strchr(s, '.');
	char base[256];
	Ty t;
	if (dot == s || !dot[1] || strchr(dot + 1, '.') || (size_t)(dot - s) >= sizeof(base)) {
		err_node(name, NAME_FMT ": a function's name has a dot only between a type and a name, like Vec2.len", s);
		return 0;
	}
	memcpy(base, s, (size_t)(dot - s));
	base[dot - s] = 0;
	t = find_named_type(base);
	if (t == T_NONE) {
		err_node(name, NAME_FMT ": %.64s is not a type; a dot in a function's name comes after the struct or enum it belongs to, like Vec2.len", s, base);
		return 0;
	}
	if (!is_struct(t) && !is_enum(t)) {
		err_node(name, NAME_FMT ": %.64s is a built-in type; only structs and enums have functions of their own", s, base);
		return 0;
	}
	if (g_tys[t].gem != g_unit_gem) {
		err_node(name, NAME_FMT ": %.64s is %s%.64s's; a type's functions are declared in the gem that declares it", s, base,
		         g_tys[t].gem ? "gem " : "", g_tys[t].gem ? g_gems[g_tys[t].gem].name : "the program");
		return 0;
	}
	if (is_enum(t) && find_variant(t, dot + 1, strlen(dot + 1)) >= 0) {
		err_node(name, NAME_FMT " is a variant of %.64s", s, base);
		return 0;
	}
	return 1;
}

/* declare_fn DEFN REPL: register the signature, so calls can come before the definition */
static o declare_fn(i32 n, int repl)
{
	i32 name, params, ret, body, p, fi;
	Fn fn;
	if (!fn_shape(n, &name, &params, &ret, &body)) return;
	memset(&fn, 0, sizeof(fn));
	if (is_reserved(g_nodes[name].text)) {
		err_node(name, NAME_FMT " is a built-in name", g_nodes[name].text);
		return;
	}
	if (strchr(g_nodes[name].text, '.') && !owner_ok(name)) return;
	if (find_named_type(g_nodes[name].text) != T_NONE) {
		err_node(name, NAME_FMT " is already a type", g_nodes[name].text);
		return;
	}
	if (find_macro(g_nodes[name].text) >= 0) {
		err_node(name, NAME_FMT " is already a macro", g_nodes[name].text);
		return;
	}
	for (p = first_of(params); p >= 0; p = next_of(p)) {
		i32 pn = first_of(p), colon = next_of(pn), pt = next_of(colon), q;
		if (g_nodes[p].kind != N_LIST || !is_sym(pn) || !sym_is(colon, ":") || pt < 0 || next_of(pt) >= 0) {
			err_node(p, "a parameter is (name : type)");
			return;
		}
		if (is_reserved(g_nodes[pn].text) || strchr(g_nodes[pn].text, '.'))
			err_node(pn, NAME_FMT " cannot be a parameter name", g_nodes[pn].text);
		for (q = first_of(params); q != p; q = next_of(q)) {
			if (!strcmp(g_nodes[first_of(q)].text, g_nodes[pn].text)) err_node(pn, "parameter " NAME_FMT " twice", g_nodes[pn].text);
		}
		if (fn.nparams == MAX_PARAMS) { err_node(p, "at most %d parameters", MAX_PARAMS); return; }
		fn.params[fn.nparams] = parse_type(pt);
		if (fn.params[fn.nparams] == T_UNIT) err_node(pt, "a parameter cannot be unit");
		fn.pcells += cells(fn.params[fn.nparams]);
		fn.nparams++;
	}
	fn.ret = ret >= 0 ? parse_type(ret) : (Ty)T_UNIT;
	fn.line = g_nodes[n].line;
	fn.file = g_unit.file;
	fn.entry = -1;
	fn.decl_unit = g_unit_id;
	fn.decl_node = n;
	fn.gem = g_unit_gem;
	if ((fi = find_fn(g_nodes[name].text)) >= 0) {
		Fn *old = &g_fns[fi];
		if (!repl || old->nparams != fn.nparams || old->ret != fn.ret || memcmp(old->params, fn.params, fn.nparams * sizeof(Ty))) {
			err_node(name, NAME_FMT " is already defined (%s)", g_nodes[name].text, where(old->file, old->line));
			return;
		}
		old->decl_unit = g_unit_id; /* REPL: same signature, the new body replaces the old one */
		old->decl_node = n;
		if (old->file && strcmp(old->file, "<repl>") && g_nreplaced < MAX_REPLACED) {
			g_replaced[g_nreplaced].name = old->name;
			g_replaced[g_nreplaced].file = old->file;
			g_replaced[g_nreplaced].line = old->line;
			g_nreplaced++;
		}
		return;
	}
	if (find_global(g_nodes[name].text) >= 0) {
		err_node(name, NAME_FMT " is already a global", g_nodes[name].text);
		return;
	}
	if (g_nfns == MAX_FNS) die("too many functions");
	fn.name = arena_strndup(&g_perm, g_nodes[name].text, g_nodes[name].len);
	names_put(fn.name)->fn = g_nfns;
	g_fns[g_nfns++] = fn;
}

/* fn_body FN PARAMS BODY AT FIELD: the code of function FN - its parameters named by the list PARAMS (-1: none), its
 * body the forms from BODY on. A body that does not end with the return type is reported at AT: as a function's, or
 * with FIELD as a field's default */
static o fn_body(i32 fi, i32 params, i32 body, i32 at, const char *field)
{
	i32 p, jmp, k = 0, line = g_nodes[at].line;
	i32 s_slots = g_slots, s_max = g_maxslots, s_locals = g_nlocals, s_floor = g_local_floor;
	i32 s_depth = g_depth, s_maxdepth = g_maxdepth, s_base = g_fn_base, s_tbase = g_fn_tbase, s_index = g_fn_index;
	Ty t, rt, s_ret = g_fn_ret;
	jmp = emit(OP_JMP, -1, 0, line);
	g_fns[fi].entry = g_ncode;
	g_slots = g_maxslots = 0;
	g_depth = g_maxdepth = 0;
	g_local_floor = g_nlocals; /* the top level's locals live in another frame */
	for (p = params >= 0 ? first_of(params) : -1; p >= 0; p = next_of(p), k++)
		add_local(first_of(p), LK_PARAM, g_nodes[first_of(p)].text, g_nodes[first_of(p)].stamp, g_fns[fi].params[k],
		          new_slots(cells(g_fns[fi].params[k])));
	rt = g_fns[fi].ret;
	push_body(body, 1, rt);
	g_work[g_nwork - 1].tail = 1;
	g_fn_base = g_nwork - 1;
	g_fn_ret = rt;
	g_fn_tbase = g_ntypes;
	g_fn_index = fi;
	run_work(g_nwork - 1);
	g_fn_base = s_base;
	g_fn_ret = s_ret;
	g_fn_tbase = s_tbase;
	g_fn_index = s_index;
	t = pop_type();
	for (k = 0; k < g_fns[fi].nparams; k++) /* the parameters die; the result on top stays */
		emit_release(0, g_locals[g_local_floor + k].slot, g_fns[fi].params[k], line);
	if (rt == T_UNIT) {
		emit_pop(t, line);
	} else if (t != T_ERR && rt != T_ERR && t != rt) {
		if (field) err_node(at, "the default of %.64s is %s, the field is %s", field, tn(t), tn(rt));
		else err_node(at, NAME_FMT " must return %s, its body ends with %s", g_nodes[at].text, tn(rt), tn(t));
	}
	emit_ret(rt, line);
	g_fns[fi].nslots = g_maxslots;
	g_fns[fi].maxdepth = g_maxdepth;
	g_slots = s_slots;
	g_maxslots = s_max;
	g_depth = s_depth;
	g_maxdepth = s_maxdepth;
	g_nlocals = s_locals;
	g_local_floor = s_floor;
	patch(jmp, g_ncode);
}

static o compile_fn(i32 n)
{
	i32 name, params, ret, body, fi;
	if (!fn_shape(n, &name, &params, &ret, &body)) return;
	fi = find_fn(g_nodes[name].text);
	if (fi < 0 || g_fns[fi].decl_unit != g_unit_id || g_fns[fi].decl_node != n) return; /* declare_fn said why */
	fn_body(fi, params, body, name, 0);
}

/* compile_defaults STRUCT: the functions of the default values its fields have; only the unit that declared the
 * struct compiles them (in the REPL the same struct may be read again) */
static o compile_defaults(i32 n)
{
	i32 name = next_of(first_of(n)), p;
	Ty t = is_sym(name) ? find_named_type(g_nodes[name].text) : (Ty)T_NONE;
	if (!is_struct(t)) return;
	for (p = next_of(name); p >= 0; p = next_of(p)) {
		i32 fname = first_of(p), d = next_of(next_of(next_of(fname)));
		const Field *fd = d >= 0 && is_sym(fname) ? find_field(t, g_nodes[fname].text, g_nodes[fname].len) : 0;
		if (!fd || fd->def < 0 || g_fns[fd->def].decl_unit != g_unit_id || g_fns[fd->def].decl_node != d) continue;
		fn_body(fd->def, -1, d, d, fd->name);
	}
}

/* compile_def DEF REPL: (def name value) or (def name : type value) - a global */
static o compile_def(i32 n, int repl, int exported)
{
	i32 name = next_of(first_of(n)), x, value, gi;
	Ty declared = T_NONE, t, type;
	const char *s;
	if (!is_sym(name)) { err_node(name < 0 ? n : name, "def needs a name: (def hp : int 100)"); return; }
	s = g_nodes[name].text;
	x = next_of(name);
	value = x;
	if (sym_is(x, ":")) {
		declared = parse_type(next_of(x));
		value = next_of(next_of(x));
	}
	if (value < 0) { err_node(name, NAME_FMT " has no value", s); return; }
	if (next_of(value) >= 0) err_node(next_of(value), "def has one value");
	if (strchr(s, '.')) { err_node(name, NAME_FMT ": a global name cannot contain a dot", s); return; }
	if (is_reserved(s)) { err_node(name, NAME_FMT " is a built-in name", s); return; }
	if (find_fn(s) >= 0) { err_node(name, NAME_FMT " is already a function", s); return; }
	if (find_named_type(s) != T_NONE) { err_node(name, NAME_FMT " is already a type", s); return; }
	if (find_macro(s) >= 0) { err_node(name, NAME_FMT " is already a macro", s); return; }
	compile_expr(value, declared);
	t = pop_type();
	type = declared != T_NONE ? declared : t;
	if (declared != T_NONE && declared != T_ERR && t != T_ERR && t != declared)
		err_node(name, NAME_FMT " is declared %s, its value is %s", s, tn(declared), tn(t));
	if (type == T_UNIT) err_node(name, NAME_FMT " would hold no value (unit)", s);
	if ((gi = find_global(s)) >= 0) {
		if (!repl || g_globals[gi].type != type) {
			err_node(name, NAME_FMT " is already defined (%s)", s, where(g_globals[gi].file, g_globals[gi].line));
			return;
		}
	} else {
		if (g_nglobals == MAX_GLOBALS) die("too many globals");
		if (g_nglobal_cells + cells(type) > MAX_GLOBAL_CELLS) die("globals too large (MAX_GLOBAL_CELLS)");
		gi = g_nglobals++;
		g_globals[gi].name = arena_strndup(&g_perm, s, g_nodes[name].len);
		g_globals[gi].gem = g_unit_gem;
		names_put(g_globals[gi].name)->global = gi;
		g_globals[gi].type = type;
		g_globals[gi].cell = g_nglobal_cells;
		g_globals[gi].line = g_nodes[name].line;
		g_globals[gi].file = g_unit.file;
		g_nglobal_cells += cells(type);
	}
	g_globals[gi].exported |= (u8)exported;
	if (g_nodes[value].kind != N_LIST && g_nodes[value].kind != N_KEY) /* a literal, or a name: shown by gael -x */
		g_globals[gi].value = arena_strndup(&g_perm, g_nodes[value].text, g_nodes[value].len);
	emit_release(1, g_globals[gi].cell, type, g_nodes[name].line); /* the REPL's new value for it; a new global holds 0 */
	emit_store(1, g_globals[gi].cell, type, g_nodes[name].line);
}

/* gem_find NAME: the gem of that name in the table, -1 when there is none */
static i32 gem_find(const char *name)
{
	i32 i;
	for (i = 1; i < g_ngems; i++) {
		if (!strcmp(g_gems[i].name, name)) return i;
	}
	return -1;
}

/* gem_gives G WHAT: gem G is WHAT, or provides it */
static int gem_gives(i32 g, const char *what)
{
	i32 k;
	if (!strcmp(g_gems[g].name, what)) return 1;
	for (k = 0; k < g_gems[g].nprovides; k++) {
		if (!strcmp(g_gems[g].provides[k], what)) return 1;
	}
	return 0;
}

static int gem_is_on(i32 g) { return g_gems[g].state == GEM_ON || g_gems[g].state == GEM_LOADING; }

/* gem_sees FROM TO: the code of gem FROM may use the names of gem TO */
static int gem_sees(i32 from, i32 to)
{
	i32 k;
	if (from == to || from == 0) return 1;
	if (to == 0) return 0;
	for (k = 0; k < g_gems[from].nrequires; k++) {
		if (gem_gives(to, g_gems[from].requires[k])) return 1;
	}
	return 0;
}

/* gem_use N OWNER: N names something of gem OWNER; 0 after an error when the unit being compiled may not use it */
static int gem_use(i32 n, i32 owner)
{
	const char *s = g_nodes[n].text;
	if (owner && g_gems[owner].state == GEM_OFF) {
		err_node(n, NAME_FMT " is in gem %s, which is off; (enable %s) turns it on", s, g_gems[owner].name, g_gems[owner].name);
		return 0;
	}
	if (gem_sees(g_unit_gem, owner)) return 1;
	if (!owner)
		err_node(n, "gem %.64s uses " NAME_FMT ", a name of the program; a gem uses its own names and those of the gems it requires",
		         g_gems[g_unit_gem].name, s);
	else
		err_node(n, "gem %.64s uses " NAME_FMT " of gem %.64s, which it does not require; add %.64s to its :requires",
		         g_gems[g_unit_gem].name, s, g_gems[owner].name, g_gems[owner].name);
	return 0;
}

/* gem_manifest N NAME G: the manifest form N of gem NAME, (gem NAME :version "1.0" :requires (...) :provides (...)
 * :excludes (...)), into G; 0 after an error it reported */
static int gem_manifest(i32 n, const char *name, Gem *g)
{
	i32 x = next_of(first_of(n)), key, val, it;
	memset(g, 0, sizeof(*g));
	if (!head_is(n, "gem")) {
		err_node(n < 0 ? 0 : n, "a gem's main file starts with its manifest: (gem %.64s :requires (...))", name);
		return 0;
	}
	if (!is_sym(x) || strcmp(g_nodes[x].text, name)) {
		err_node(x < 0 ? n : x, "this file is gem %.64s, so its manifest starts (gem %.64s", name, name);
		return 0;
	}
	g->name = arena_strndup(&g_perm, name, strlen(name));
	for (key = next_of(x); key >= 0; key = next_of(val)) {
		const char *k = g_nodes[key].text;
		const char **list;
		i32 *count;
		val = next_of(key);
		if (g_nodes[key].kind != N_KEY || val < 0) {
			err_node(key, "a manifest is (gem name :key value ...); the keys are :version, :requires, :provides and :excludes");
			return 0;
		}
		if (!strcmp(k, ":version")) {
			if (g_nodes[val].kind != N_STR) { err_node(val, ":version is a string, like \"0.1.0\""); return 0; }
			g->version = arena_strndup(&g_perm, g_nodes[val].text, g_nodes[val].len);
			continue;
		}
		if (!strcmp(k, ":requires")) { list = g->requires; count = &g->nrequires; }
		else if (!strcmp(k, ":provides")) { list = g->provides; count = &g->nprovides; }
		else if (!strcmp(k, ":excludes")) { list = g->excludes; count = &g->nexcludes; }
		else {
			err_node(key, "unknown manifest key " NAME_FMT "; the keys are :version, :requires, :provides and :excludes", k);
			return 0;
		}
		if (g_nodes[val].kind != N_LIST) { err_node(val, NAME_FMT " takes a list of names, like (ecs debug-draw)", k); return 0; }
		for (it = first_of(val); it >= 0; it = next_of(it)) {
			if (!is_sym(it)) { err_node(it, "a gem or what one provides is named by a symbol, like ecs"); return 0; }
			if (*count == MAX_GEM_LINKS) { err_node(it, "at most %d names in " NAME_FMT, MAX_GEM_LINKS, k); return 0; }
			list[(*count)++] = arena_strndup(&g_perm, g_nodes[it].text, g_nodes[it].len);
		}
	}
	return 1;
}

/* gem_clash G: why gem G cannot be on with the gems on now - one of them provides what it provides, or one excludes
 * the other - into a message; 0 when nothing stands in its way */
static const char *gem_clash(i32 g)
{
	static char msg[300];
	i32 h, k, j;
	for (h = 1; h < g_ngems; h++) {
		if (h == g || !gem_is_on(h)) continue;
		for (k = -1; k < g_gems[g].nprovides; k++) {
			const char *what = k < 0 ? g_gems[g].name : g_gems[g].provides[k];
			if (gem_gives(h, what)) {
				sprintf(msg, "gems %.60s and %.60s both provide %.60s; turn on one of them", g_gems[h].name, g_gems[g].name, what);
				return msg;
			}
		}
		for (j = 0; j < g_gems[g].nexcludes; j++) {
			if (gem_gives(h, g_gems[g].excludes[j])) {
				sprintf(msg, "gem %.60s excludes %.60s, and gem %.60s is on", g_gems[g].name, g_gems[g].excludes[j], g_gems[h].name);
				return msg;
			}
		}
		for (j = 0; j < g_gems[h].nexcludes; j++) {
			if (gem_gives(g, g_gems[h].excludes[j])) {
				sprintf(msg, "gem %.60s, which is on, excludes %.60s", g_gems[h].name, g_gems[h].excludes[j]);
				return msg;
			}
		}
	}
	return 0;
}

/* gem_needed G: a gem that is on and needs G (requires something G provides that no other gem on provides); -1 */
static i32 gem_needed(i32 g)
{
	i32 h, k, p;
	for (h = 1; h < g_ngems; h++) {
		if (h == g || !gem_is_on(h)) continue;
		for (k = 0; k < g_gems[h].nrequires; k++) {
			const char *what = g_gems[h].requires[k];
			if (!gem_gives(g, what)) continue;
			for (p = 1; p < g_ngems && (p == g || !gem_is_on(p) || !gem_gives(p, what)); p++) {}
			if (p == g_ngems) return h;
		}
	}
	return -1;
}

/* gem_switch N ON: (enable name...) or (disable name...) in the program; the loader has loaded the gems already */
static o gem_switch(i32 n, int on)
{
	i32 x, g, h;
	const char *why;
	if (g_unit_gem) {
		err_node(n, "a gem says what it needs in its manifest, with :requires; (%s ...) is for the program", on ? "enable" : "disable");
		return;
	}
	for (x = next_of(first_of(n)); x >= 0; x = next_of(x)) {
		if (!is_sym(x) || (g = gem_find(g_nodes[x].text)) < 0 || g_gems[g].state < GEM_ON) {
			if (!is_sym(x)) err_node(x, "a gem is named by a symbol: (%s ecs)", on ? "enable" : "disable");
			else if (!on) err_node(x, "there is no gem " NAME_FMT " on", g_nodes[x].text);
			continue; /* enable: the loader said why */
		}
		if (on && g_gems[g].state == GEM_OFF) {
			if ((why = gem_clash(g)) != 0) err_node(x, "%s", why);
			else g_gems[g].state = GEM_ON;
		} else if (!on && g_gems[g].state == GEM_ON) {
			if ((h = gem_needed(g)) >= 0) err_node(x, "gem %.64s requires " NAME_FMT "; turn it off first", g_gems[h].name, g_nodes[x].text);
			else g_gems[g].state = GEM_OFF;
		}
	}
}

/* define_macro N REPL: (defmacro name rule...) at the top level; its name must be free, but the REPL may define a macro
 * again */
static o define_macro(i32 n, int repl)
{
	i32 name = next_of(first_of(n)), mi;
	const char *s;
	if (!is_sym(name)) { err_node(name < 0 ? n : name, "defmacro needs a name: (defmacro swap ((_ a b) template))"); return; }
	s = g_nodes[name].text;
	if (is_reserved(s) || strchr(s, '.')) {
		err_node(name, strchr(s, '.') ? NAME_FMT ": a macro name cannot contain a dot" : NAME_FMT " is a built-in name", s);
		return;
	}
	if (find_fn(s) >= 0) { err_node(name, NAME_FMT " is already a function", s); return; }
	if (find_global(s) >= 0) { err_node(name, NAME_FMT " is already a global", s); return; }
	if (find_named_type(s) != T_NONE) { err_node(name, NAME_FMT " is already a type", s); return; }
	if ((mi = find_macro(s)) >= 0 && !repl) {
		err_node(name, NAME_FMT " is already a macro (%s)", s, where(g_macros[mi].file, g_macros[mi].line));
		return;
	}
	macro_define(n);
}

/* expand_top TOP REPL: the top-level forms with every macro use among them expanded in place, before anything is
 * declared, so a macro can make functions, globals and types. What a macro makes with (do ...) is spliced in, one form
 * per element, and a defmacro it makes is defined right there. Returns the new first form. */
static i32 expand_top(i32 top, int repl)
{
	i32 prev = -1, n = top, head, mi, x, next, last;
	while (n >= 0) {
		next = next_of(n);
		head = first_of(n);
		if (g_nodes[n].kind != N_LIST || !is_sym(head)) { prev = n; n = next; continue; }
		if (!strcmp(g_nodes[head].text, "defmacro") && !g_nodes[n].ival) {
			define_macro(n, repl);
			g_nodes[n].ival = 1;
		}
		if ((mi = find_macro(g_nodes[head].text)) < 0) { prev = n; n = next; continue; }
		x = gem_use(head, g_macros[mi].gem) ? macro_expand(mi, n) : -1;
		if (x >= 0 && head_is(x, "do")) { /* spliced: its elements take its place */
			last = -1;
			for (head = next_of(first_of(x)); head >= 0; head = next_of(head)) last = head;
			if (last >= 0) {
				g_nodes[last].next = next;
				x = next_of(first_of(x));
			} else {
				x = -1;
			}
		} else if (x >= 0) {
			g_nodes[x].next = next;
		}
		if (x < 0) x = next; /* an error, reported, or an empty (do): the form goes */
		if (prev < 0) top = x; else g_nodes[prev].next = x;
		n = x; /* what came out may be a macro use too */
	}
	return top;
}

/* Fusion. Once a unit compiles without errors, runs of simple instructions become one superinstruction each (ops.h):
 * a local or a constant fed straight into arithmetic, a comparison straight into a jump, the step at the end of a for
 * loop. The rest of a run turns into NOPs that the superinstruction skips, so no instruction moves and no jump needs
 * to change. A run never has, after its start, a place code comes to from elsewhere: a jump's target, a function's
 * entry, the instruction after a call or after a coroutine's pause. Before that, a jump to a JMP goes where that one goes,
 * and a JMP to a return becomes the return. */
static const u8 g_bin_ops[] = { OP_ADD_I, OP_SUB_I, OP_MUL_I, OP_ADD_F, OP_SUB_F, OP_MUL_F, OP_DIV_F };
static const u8 g_bin_fused[] = { OP_ADD_I_LL, OP_SUB_I_LL, OP_MUL_I_LL, OP_ADD_F_LL, OP_SUB_F_LL, OP_MUL_F_LL, OP_DIV_F_LL };
static const u8 g_cmp_ops[] = { OP_LT_I, OP_LE_I, OP_GT_I, OP_GE_I, OP_EQ_I, OP_NE_I, OP_LT_F, OP_LE_F, OP_GT_F, OP_GE_F, OP_EQ_F, OP_NE_F };
static const u8 g_cmp_fused[] = { OP_JLT_I_LL, OP_JLE_I_LL, OP_JGT_I_LL, OP_JGE_I_LL, OP_JEQ_I_LL, OP_JNE_I_LL,
	OP_JLT_F_LL, OP_JLE_F_LL, OP_JGT_F_LL, OP_JGE_F_LL, OP_JEQ_F_LL, OP_JNE_F_LL };

static int op_index(const u8 *ops, i32 n, u8 op)
{
	i32 k;
	for (k = 0; k < n; k++) {
		if (ops[k] == op) return k;
	}
	return -1;
}

/* fuse_const INS FLOAT: the constant a run's push puts on the stack, as an index into the int (or float) constants;
 * -1 when it pushes something else */
static i32 fuse_const(const Ins *c, int fl)
{
	if (fl) return c->op == OP_PUSH_F ? c->a : -1;
	if (c->op == OP_PUSH_I) return c->a;
	if (c->op == OP_PUSH_K) return add_int(c->a); /* a bool or an enum value */
	return -1;
}

static int is_jump(u8 op) { return op == OP_JMP || op == OP_JF || op == OP_JT; }
static int is_ret(u8 op) { return op == OP_RET_V || op == OP_RET_U || op == OP_RET_N; }

/* fused SITE OP A B LEN: the run of LEN instructions at SITE becomes OP A B and NOPs */
static o fused(Ins *c, u8 op, i32 a, i32 b, i32 len)
{
	i32 k;
	c->op = op;
	c->a = a;
	c->b = b;
	for (k = 1; k < len; k++) {
		c[k].op = OP_NOP;
		c[k].a = c[k].b = 0;
	}
}

static o fuse(i32 from, i32 to)
{
	u8 *tg;
	i32 i, n = to - from;
	if (n <= 0) return;
	tg = (u8 *)arena_alloc(&g_unit_arena, (size_t)n + 4);
	memset(tg, 0, (size_t)n + 4);
	for (i = from; i < to; i++) { /* a jump to a JMP goes on to its target; a JMP to a return returns */
		Ins *c = &g_code[i];
		i32 hops = 0;
		if (!is_jump(c->op)) continue;
		while (hops++ < 16 && c->a >= 0 && c->a < g_ncode && g_code[c->a].op == OP_JMP) c->a = g_code[c->a].a;
		if (c->op == OP_JMP && c->a >= 0 && c->a < g_ncode && is_ret(g_code[c->a].op)) *c = g_code[c->a];
	}
	for (i = from; i < to; i++) { /* the places code comes to other than from the instruction before */
		const Ins *c = &g_code[i];
		u8 op = c->op;
		if (is_jump(op) && c->a >= from && c->a < to) tg[c->a - from] = 1;
		if (op == OP_CALL || op == OP_CALL_V || op == OP_TAILCALL || op == OP_TAILCALL_V || op == OP_CORO_NEW ||
		    op == OP_SLEEP || op == OP_YIELD || op == OP_CORO_STEP || op == OP_CORO_RUN)
			tg[i + 1 - from] = 1;
	}
	for (i = 0; i < g_nfns; i++) {
		if (g_fns[i].entry >= from && g_fns[i].entry < to) tg[g_fns[i].entry - from] = 1;
	}
	for (i = from; i < to; i++) {
		Ins *c = &g_code[i];
		i32 room = to - i, k, j = -1, run;
		for (run = 1; run < 4 && run < room && !tg[i + run - from]; run++) {} /* how far a run from here may go */
		if (run >= 4 && c[0].op == OP_LOAD_L) {
			/* LOAD_L LOAD_L CMP J and LOAD_L PUSH CMP J: compare-and-jump with both values given */
			if ((k = op_index(g_cmp_ops, (i32)sizeof(g_cmp_ops), c[2].op)) >= 0 && (c[3].op == OP_JT || c[3].op == OP_JF) &&
			    c[0].op == OP_LOAD_L && (c[1].op == OP_LOAD_L || (j = fuse_const(&c[1], k >= 6)) >= 0)) {
				i32 t = c[3].a, sense = c[3].op == OP_JT;
				if (c[1].op == OP_LOAD_L) fused(c, (u8)g_cmp_fused[k], c[0].a, c[1].a, 4);
				else fused(c, (u8)(g_cmp_fused[k] + 1), c[0].a, j, 4);
				c[1].a = t;
				c[1].b = sense;
				i += 3;
				continue;
			}
		}
		if (run >= 3) {
			/* PUSH CMP J: compare the value on top with a constant and jump */
			if ((k = op_index(g_cmp_ops, (i32)sizeof(g_cmp_ops), c[1].op)) >= 0 && (c[2].op == OP_JT || c[2].op == OP_JF) &&
			    (j = fuse_const(&c[0], k >= 6)) >= 0) {
				i32 t = c[2].a, sense = c[2].op == OP_JT;
				fused(c, (u8)(g_cmp_fused[k] + 2), j, 0, 3);
				c[1].a = t;
				c[1].b = sense;
				i += 2;
				continue;
			}
			/* LOAD_L LOAD_L OP and LOAD_L PUSH OP: arithmetic on a local and a local or a constant */
			if (c[0].op == OP_LOAD_L && (k = op_index(g_bin_ops, (i32)sizeof(g_bin_ops), c[2].op)) >= 0) {
				if (c[1].op == OP_LOAD_L) { fused(c, g_bin_fused[k], c[0].a, c[1].a, 3); i += 2; continue; }
				if ((j = fuse_const(&c[1], k >= 3)) >= 0) { fused(c, (u8)(g_bin_fused[k] + 1), c[0].a, j, 3); i += 2; continue; }
			}
			if (c[0].op == OP_LOAD_L && c[2].op == OP_MOD_I && (j = fuse_const(&c[1], 0)) >= 0 && g_ints[j] != 0 && g_ints[j] != -1) {
				fused(c, OP_MOD_I_LK, c[0].a, j, 3);
				i += 2;
				continue;
			}
		}
		if (run >= 2) {
			if ((k = op_index(g_cmp_ops, (i32)sizeof(g_cmp_ops), c[0].op)) >= 0 && (c[1].op == OP_JT || c[1].op == OP_JF)) {
				fused(c, (u8)(g_cmp_fused[k] + 3), c[1].a, c[1].op == OP_JT, 2); /* CMP J */
				i += 1;
				continue;
			}
			if ((k = op_index(g_bin_ops, (i32)sizeof(g_bin_ops), c[1].op)) >= 0) {
				if (c[0].op == OP_LOAD_L) { fused(c, (u8)(g_bin_fused[k] + 2), c[0].a, 0, 2); i += 1; continue; } /* LOAD_L OP */
				if ((j = fuse_const(&c[0], k >= 3)) >= 0) { fused(c, (u8)(g_bin_fused[k] + 3), j, 0, 2); i += 1; continue; } /* PUSH OP */
			}
			if (c[1].op == OP_MOD_I && (j = fuse_const(&c[0], 0)) >= 0 && g_ints[j] != 0 && g_ints[j] != -1) {
				fused(c, OP_MOD_I_SK, j, 0, 2);
				i += 1;
				continue;
			}
			if (c[0].op == OP_INC_L && c[1].op == OP_JMP) { fused(c, OP_INC_JMP, c[0].a, c[1].a, 2); i += 1; continue; }
			if (c[0].op == OP_STORE_L && c[1].op == OP_LOAD_L) { fused(c, OP_STORE_LOAD_L, c[0].a, c[1].a, 2); i += 1; continue; }
			if (c[0].op == OP_LOAD_L && c[1].op == OP_LOAD_L) { fused(c, OP_LOAD2_L, c[0].a, c[1].a, 2); i += 1; continue; }
		}
	}
}

/* show_form NODE: one top-level form as source on stdout, the way a person would lay it out: a list that fits in
 * 118 columns on one line; else its head with the names and values after it and its first list (a defn's parameters,
 * a let's bindings), then each part after that on a line of its own, two spaces further in, laid out the same way */
static o show_form(i32 n)
{
	static char buf[1 << 16];
	static struct {
		i32 node, indent, child;
		u8 open;
	} st[MAX_DEPTH];
	i32 sp = 0, c, k;
	size_t at, line;
	int lists;
	st[sp].node = n;
	st[sp].indent = 0;
	st[sp].open = 0;
	sp++;
	while (sp > 0) {
		i32 me = st[sp - 1].node, ind = st[sp - 1].indent;
		if (st[sp - 1].open) {
			if ((c = st[sp - 1].child) < 0) { putchar(')'); sp--; continue; }
			st[sp - 1].child = next_of(c);
			putchar('\n');
			for (k = 0; k < ind + 2; k++) putchar(' ');
			if (sp == MAX_DEPTH) { fputs("...", stdout); continue; }
			st[sp].node = c;
			st[sp].indent = ind + 2;
			st[sp].open = 0;
			sp++;
			continue;
		}
		at = 0;
		buf[0] = 0;
		nprint(g_nodes, me, buf, sizeof(buf), &at);
		if ((i32)at + ind <= 118 || g_nodes[me].kind != N_LIST || first_of(me) < 0) {
			fputs(buf, stdout);
			sp--;
			continue;
		}
		putchar('(');
		at = 0;
		nprint(g_nodes, first_of(me), buf, sizeof(buf), &at);
		fputs(buf, stdout);
		line = (size_t)ind + 1 + at;
		lists = 0;
		for (c = next_of(first_of(me)); c >= 0; c = next_of(c)) { /* what stays on the head's line */
			at = 0;
			nprint(g_nodes, c, buf, sizeof(buf), &at);
			if (line + 1 + at > 118 || (g_nodes[c].kind == N_LIST && lists)) break;
			putchar(' ');
			fputs(buf, stdout);
			line += 1 + at;
			lists += g_nodes[c].kind == N_LIST;
		}
		st[sp - 1].child = c;
		st[sp - 1].open = 1;
	}
	putchar('\n');
}

/* everything a failed unit may have added, so the REPL can forget it */
static struct {
	i32 ncode, nints, nfloats, nstrs, nfns, nglobals, nglobal_cells, ntys, nfields, nvariants, nfmts, nfmt_tys, nleaves, nguards,
	    ntparams, nmacros, nrules, nmnodes, nmarks, ntests;
	size_t perm, text, mtext;
} g_snap;

/* test_code TOP: the top level with its test code - (@test "name" body...) and a form after @test, which exists for
 * the tests - kept, with the @test marks taken off, or taken out in a gem that gael test is not testing: its tests
 * neither run nor cost anything there. Gives the new first form */
static i32 test_code(i32 top)
{
	int keep = !g_unit_gem || g_gems[g_unit_gem].tested;
	i32 n = top, prev = -1;
	while (n >= 0) {
		i32 next = next_of(n);
		int drop = 0;
		if (sym_is(n, "@test")) { /* @test (defn helper ...): for the tests only */
			drop = 1;
			if (next < 0 || g_nodes[next].kind != N_LIST || head_is(next, "@test"))
				err_node(n, "@test goes before a top-level form that the tests use, like @test (defn sample () ...), or around a test: (@test \"name\" body...)");
			else if (!keep) next = next_of(next);
		} else if (head_is(n, "@test") && !keep) drop = 1;
		if (!drop) prev = n;
		else if (prev < 0) top = next;
		else g_nodes[prev].next = next;
		n = next;
	}
	return top;
}

/* compile_unit TOP REPL: compile every top-level form; returns the entry point, or -1 after errors (and then nothing
 * of the unit stays). In the REPL the value of the last form is shown. */
static i32 compile_unit(i32 top, int repl)
{
	i32 n, entry, i, ended = 0; /* ended: the line of an (exit ...) at the top level; what runs after it never does */
	int exported = 0; /* an @export was just read */
	Ty t;
	g_snap.ncode = g_ncode;
	g_snap.nints = g_nints;
	g_snap.nfloats = g_nfloats;
	g_snap.nstrs = g_nstrs;
	g_snap.nfns = g_nfns;
	g_snap.nglobals = g_nglobals;
	g_snap.nglobal_cells = g_nglobal_cells;
	g_snap.ntys = g_ntys;
	g_snap.nfields = g_nfields;
	g_snap.nvariants = g_nvariants;
	g_snap.nfmts = g_nfmts;
	g_snap.nfmt_tys = g_nfmt_tys;
	g_snap.nleaves = g_nleaves;
	g_snap.ntparams = g_ntparams;
	g_snap.nguards = g_nguards;
	g_snap.perm = g_perm.used;
	g_snap.text = g_text.used;
	g_snap.nmacros = g_nmacros;
	g_snap.nmarks = g_nmarks;
	g_snap.ntests = g_ntests;
	g_snap.nrules = g_nrules;
	g_snap.nmnodes = g_nmnodes;
	g_snap.mtext = g_mtext.used;
	for (i = 0; i < g_nfns; i++) {
		g_snap_entry[i] = g_fns[i].entry;
		g_snap_slots[i] = g_fns[i].nslots;
		g_snap_depth[i] = g_fns[i].maxdepth;
	}
	g_unit_id++;
	g_nreplaced = 0;
	g_nlocals = g_local_floor = 0;
	g_slots = g_maxslots = 0;
	g_depth = g_maxdepth = 0;
	g_ntypes = g_npatches = g_nwork = 0;
	g_nfjumps = g_ntjumps = 0;

	g_expansions = 0;
	top = test_code(top);
	for (n = top; n >= 0; n = next_of(n)) { /* macros first: anything below may use them, and so may the passes below */
		if (head_is(n, "defmacro")) {
			define_macro(n, repl);
			g_nodes[n].ival = 1; /* defined */
		}
	}
	top = expand_top(top, repl);
	for (n = top; n >= 0; n = next_of(n)) { /* each (@test "name" body...) becomes a function, declared with the rest */
		if (head_is(n, "@test")) (void)test_fn(n);
	}
	for (n = top; n >= 0; n = next_of(n)) { /* types first, so signatures and bodies can use them */
		if (head_is(n, "struct")) declare_struct(n, repl);
		else if (head_is(n, "enum")) declare_enum(n, repl);
	}
	for (n = top; n >= 0; n = next_of(n)) {
		if (head_is(n, "defn")) declare_fn(n, repl);
	}
	entry = emit(OP_ENTER, 0, 0, top >= 0 ? g_nodes[top].line : 1);
	for (n = top; n >= 0; n = next_of(n)) {
		int shown = repl && next_of(n) < 0;
		if (is_sym(n) && g_nodes[n].text[0] == '@') { /* an annotation, for the form after it */
			if (strcmp(g_nodes[n].text, "@export"))
				err_node(n, "unknown annotation " NAME_FMT "; gael knows @export (def ...), (@test \"name\" body...), @test (form for the tests), (@debug ...), (@warn \"...\") and (@err \"...\")", g_nodes[n].text);
			else if (!head_is(next_of(n), "def")) err_node(next_of(n) < 0 ? n : next_of(n), "@export goes before a def, like @export (def speed : float 10.0)");
			else exported = 1;
			continue;
		}
		if (head_is(n, "defn")) { compile_fn(n); continue; }
		if (ended > 0 && (head_is(n, "def") || !(head_is(n, "struct") || head_is(n, "enum") || head_is(n, "defmacro") ||
		    head_is(n, "gem") || head_is(n, "enable") || head_is(n, "disable") || head_is(n, "use")))) {
			warn_node(n, "this never runs: the exit on line %ld comes first; remove it, or move it above", (long)ended);
			ended = -1; /* once */
		}
		if (head_is(n, "def")) { compile_def(n, repl, exported); exported = 0; continue; }
		if (head_is(n, "struct")) { compile_defaults(n); continue; }
		if (head_is(n, "enum") || head_is(n, "defmacro")) continue;
		if (head_is(n, "gem")) { /* the loader read it */
			if (!g_unit_main || n != top) err_node(n, "a (gem ...) manifest is the first form of a gem's main file");
			continue;
		}
		if (head_is(n, "enable") || head_is(n, "disable")) {
			gem_switch(n, head_is(n, "enable"));
			continue;
		}
		if (head_is(n, "use")) { /* the loader compiled the file already; here only its shape is checked */
			i32 arg = next_of(first_of(n));
			if (arg < 0 || g_nodes[arg].kind != N_STR || next_of(arg) >= 0) err_node(n, "use takes one file name: (use \"lib.gael\")");
			continue;
		}
		i = g_unit.nerrors;
		{
			i32 skip = g_test_mode && !repl ? emit(OP_JMP, -1, 0, g_nodes[n].line) : -1; /* gael test: checked, not run */
			compile_expr(n, shown ? (Ty)T_NONE : (Ty)T_UNIT);
			t = pop_type();
			if (shown && t != T_UNIT && t != T_ERR) emit(OP_SHOW, t, 0, g_nodes[n].line);
			else {
				if (t != T_UNIT && t != T_ERR && i == g_unit.nerrors && pure_form(n)) thrown_away(n);
				emit_pop(t, g_nodes[n].line);
			}
			if (skip >= 0) patch(skip, g_ncode);
		}
		if (!ended && head_is(n, "exit") && !g_nodes[n].stamp) ended = g_nodes[n].line;
	}
	emit(OP_HALT, 0, 0, 0);
	if (g_show_expanded) { /* gael -m: compiling expanded the macros inside the forms too, in place */
		for (n = top; n >= 0; n = next_of(n)) {
			if (!head_is(n, "defmacro")) show_form(n);
		}
		g_show_expanded = 0;
	}
	g_code[entry].a = g_maxslots;
	g_code[entry].b = g_maxdepth;
	if (!g_unit.nerrors) {
		fuse(g_snap.ncode, g_ncode);
		if (!g_ncode_files || strcmp(g_code_files[g_ncode_files - 1].file, g_unit.file)) {
			if (g_ncode_files == MAX_CODE_FILES) g_ncode_files--; /* past the table, the last file takes the rest */
			g_code_files[g_ncode_files].start = g_snap.ncode;
			g_code_files[g_ncode_files].file = g_unit.file;
			g_ncode_files++;
		}
		return entry;
	}

	g_ncode = g_snap.ncode;
	g_nints = g_snap.nints;
	g_nfloats = g_snap.nfloats;
	g_nstrs = g_snap.nstrs;
	g_nfns = g_snap.nfns;
	g_nglobals = g_snap.nglobals;
	g_nglobal_cells = g_snap.nglobal_cells;
	g_ntys = g_snap.ntys;
	g_nfields = g_snap.nfields;
	g_nvariants = g_snap.nvariants;
	g_nfmts = g_snap.nfmts;
	g_nfmt_tys = g_snap.nfmt_tys;
	g_nleaves = g_snap.nleaves;
	g_ntparams = g_snap.ntparams;
	g_nguards = g_snap.nguards;
	g_perm.used = g_snap.perm;
	g_text.used = g_snap.text;
	g_nmacros = g_snap.nmacros;
	g_nmarks = g_snap.nmarks;
	g_ntests = g_snap.ntests;
	g_nrules = g_snap.nrules;
	g_nmnodes = g_snap.nmnodes;
	g_mtext.used = g_snap.mtext;
	names_rebuild();
	for (i = 0; i < g_nfns; i++) {
		g_fns[i].entry = g_snap_entry[i];
		g_fns[i].nslots = g_snap_slots[i];
		g_fns[i].maxdepth = g_snap_depth[i];
	}
	return -1;
}
