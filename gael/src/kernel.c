/* kernel.c - gpu-each: a loop over a vec whose body runs on the GPU, once per element, all at once.
 *
 *   (gpu-each (p particles) (set p.x (+ p.x (* p.vx dt))))
 *   (gpu-each (p particles i) ...)            ; i: the element's index, a u32
 *
 * The body reads and sets the fields of p that are f32, i32 or u32, reads the variables around it of those types (they
 * go to the GPU once per run, as constants), and uses let, set, if, when, do, arithmetic, comparisons, logic, bits,
 * math, min, max, abs, clamp and the conversions between the three types. Anything else does not run on a GPU and is
 * a compile error that says what to write. The same body is compiled for the VM too, as a for over the vec that puts
 * each element back; that runs when there is no GPU, so a program works the same with or without one, but for the
 * GPU's floats: WGSL lets division and sin be a few ulps off. Where the VM stops with an error - an int divided by
 * zero, a float too big for an int, a shift past the bits - the kernel calls a function that marks the run failed
 * instead of giving WGSL's number; then nothing comes back and the VM runs the loop, which stops where it should.
 *
 * Here the body becomes WGSL: one storage buffer per field it uses (a vec's fields are columns already, so each is
 * copied as it is), a uniform with the count and the values from around, and the body as statements, element idx.
 * The WGSL is written after the VM's compile of the body, which gives every literal and every form its type (2.0f,
 * and whether a / divides ints). gpu.c runs it through wgpu. No recursion: the body is walked with a stack. */

#define MAX_KERNELS 256
#define MAX_KCOLS 8     /* WebGPU's default for storage buffers in one stage */
#define MAX_KCAPS 16
#define KERNEL_TEXT 32768

enum { KT_F32, KT_I32, KT_U32 };
static const char *const g_kt_wgsl[] = { "f32", "i32", "u32" };

typedef struct {
	const char *wgsl, *file;
	i32 line;
	Ty elem;                    /* the vec's element type */
	u8 vwhere;                  /* the vec: AT_LOCAL (a slot of the frame) or AT_GLOBAL (a cell) */
	i32 vat;
	i32 ncols, ncaps;
	u8 col_cell[MAX_KCOLS];     /* the element's cell (its column) */
	u8 col_ty[MAX_KCOLS];       /* KT_ */
	u8 col_set[MAX_KCOLS];      /* the body sets it, so it comes back */
	u8 cap_where[MAX_KCAPS];
	u8 cap_ty[MAX_KCAPS];
	i32 cap_at[MAX_KCAPS];
	i32 body, enode, inode;     /* the body and the names of p and i, for the WGSL, written after the VM's compile */
	u8 fault;                   /* it divides ints, makes ints of floats or shifts: it can fail where the VM stops */
} Kernel;

static Kernel g_kernels[MAX_KERNELS];
static i32 g_nkernels;
static o gpu_forget(i32 k); /* gpu.c: what the GPU keeps for kernel K goes, its number is used again */

enum { KW_STMT, KW_EXPR, KW_TEXT, KW_INDENT, KW_DEDENT, KW_DECLARE, KW_SCOPE };
typedef struct {
	u8 kind;
	i32 node;
	const char *text;
} KWork;

static struct {
	char out[KERNEL_TEXT];
	size_t len;
	int bad, indent;
	i32 elem, index;            /* the nodes that name p and i (-1 without an index) */
	Ty ety;
	Kernel k;
	struct {
		const char *name;
		u32 stamp;
	} locals[64];
	i32 nlocals;
	KWork st[2048];
	i32 sp;
} g_kw;

static o kw_put(const char *s)
{
	size_t n = strlen(s);
	if (g_kw.len + n + 1 >= sizeof(g_kw.out)) {
		if (!g_kw.bad) err_node(g_kw.elem, "gpu-each: the body is too long for one GPU kernel; split it");
		g_kw.bad |= 1;
		return;
	}
	memcpy(g_kw.out + g_kw.len, s, n + 1);
	g_kw.len += n;
}

static o kw_indent(void)
{
	i32 k;
	for (k = 0; k < g_kw.indent + 1; k++) kw_put("  ");
}

static o kw_push(u8 kind, i32 node, const char *text)
{
	if (g_kw.sp == (i32)GAEL_COUNT(g_kw.st)) {
		if (!g_kw.bad) err_node(g_kw.elem, "gpu-each: the body is too deep for one GPU kernel; split it");
		g_kw.bad |= 1;
		return;
	}
	g_kw.st[g_kw.sp].kind = kind;
	g_kw.st[g_kw.sp].node = node;
	g_kw.st[g_kw.sp].text = text;
	g_kw.sp++;
}

static o kw_bad(i32 n, const char *fmt, const char *a)
{
	err_node(n, fmt, a);
	g_kw.bad |= 1;
}

static i32 mk_int(i32 at, i64 v) /* an int literal where node AT is */
{
	i32 n = new_node(N_INT, g_nodes[at].line, g_nodes[at].col);
	char buf[32];
	sprintf(buf, "%ld", (long)v);
	g_nodes[n].text = arena_strndup(&g_unit_arena, buf, strlen(buf));
	g_nodes[n].len = (u32)strlen(buf);
	g_nodes[n].ival = v;
	return n;
}

/* kw_name NAME OUT: a gael name as a WGSL one, v_ and the name with every character that WGSL does not take escaped:
 * - is _m, ? _q, ! _x, _ itself __, anything else _ and its two hex digits; different names stay different */
static o kw_name(const char *s, char *out, size_t size)
{
	size_t o = 0;
	static const char hex[] = "0123456789abcdef";
	out[o++] = 'v';
	out[o++] = '_';
	for (; *s && o + 4 < size; s++) {
		unsigned char c = (unsigned char)*s;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[o++] = (char)c;
		else {
			out[o++] = '_';
			if (c == '_') out[o++] = '_';
			else if (c == '-') out[o++] = 'm';
			else if (c == '?') out[o++] = 'q';
			else if (c == '!') out[o++] = 'x';
			else {
				out[o++] = hex[c >> 4];
				out[o++] = hex[c & 15];
			}
		}
	}
	out[o] = 0;
}

static i32 kw_kt(Ty t) { return t == T_F32 ? KT_F32 : t == T_I32 ? KT_I32 : t == T_U32 ? KT_U32 : -1; }

/* kw_field PATH SET: the column of p.PATH (PATH is the text after "p."), added to the kernel's columns; -1 after an
 * error it reported */
static i32 kw_field(i32 n, const char *path, int set)
{
	Ty t = g_kw.ety;
	i32 off = 0, k;
	const char *p = path, *end;
	char names[256];
	for (;;) {
		const Field *fd;
		size_t len;
		end = strchr(p, '.');
		len = end ? (size_t)(end - p) : strlen(p);
		if (!is_struct(t)) { kw_bad(n, "gpu-each: %s has no fields", g_nodes[n].text); return -1; }
		if (!(fd = find_field(t, p, len))) {
			err_node(n, "%s has no field %.*s; its fields are %s", tn(t), (int)(len < 64 ? len : 64), p, names_of(t, names, sizeof(names)));
			g_kw.bad |= 1;
			return -1;
		}
		off += fd->offset;
		t = fd->type;
		if (!end) break;
		p = end + 1;
	}
	if (is_struct(t)) { kw_bad(n, "gpu-each: %s is a struct; the GPU works on its fields, like p.pos.x", g_nodes[n].text); return -1; }
	if (t == T_BOOL) {
		kw_bad(n, "gpu-each: %s is a bool, which a vec keeps as one bit and a GPU cannot take; make the field u32, 0 or 1", g_nodes[n].text);
		return -1;
	}
	if (kw_kt(t) < 0) {
		err_node(n, "gpu-each: %s is %s; a GPU works on f32, i32 and u32: make the field f32 (or i32, u32)", g_nodes[n].text, tn(t));
		g_kw.bad |= 1;
		return -1;
	}
	for (k = 0; k < g_kw.k.ncols && g_kw.k.col_cell[k] != off; k++) {}
	if (k == g_kw.k.ncols) {
		if (k == MAX_KCOLS) { kw_bad(n, "gpu-each: the body uses more than 8 fields; a GPU takes 8 at a time here, so split it (%s is the ninth)", g_nodes[n].text); return -1; }
		g_kw.k.col_cell[k] = (u8)off;
		g_kw.k.col_ty[k] = (u8)kw_kt(t);
		g_kw.k.col_set[k] = 0;
		g_kw.k.ncols++;
	}
	if (set) g_kw.k.col_set[k] = 1;
	return k;
}

/* kw_local NAME STAMP: the body's own variable of that name, innermost first; -1 when there is none */
static i32 kw_local(const char *s, u32 stamp)
{
	i32 i;
	for (i = g_kw.nlocals - 1; i >= 0; i--) {
		if (g_kw.locals[i].stamp == stamp && !strcmp(g_kw.locals[i].name, s)) return i;
	}
	return -1;
}

static int kw_same(i32 a, i32 b) { return a >= 0 && b >= 0 && g_nodes[a].stamp == g_nodes[b].stamp && !strcmp(g_nodes[a].text, g_nodes[b].text); }

/* kw_symbol N SET: write the WGSL for the name N - a field of p, the index, a variable of the body or one from around
 * (a constant of the run); SET: N is what a set changes */
static o kw_symbol(i32 n, int set)
{
	const char *s = g_nodes[n].text, *en = g_nodes[g_kw.elem].text;
	size_t el = strlen(en);
	char buf[300];
	Place pl;
	i32 k;
	int r;
	if (!strncmp(s, en, el) && s[el] == '.' && g_nodes[n].stamp == g_nodes[g_kw.elem].stamp) {
		if ((k = kw_field(n, s + el + 1, set)) < 0) return;
		sprintf(buf, "c%ld[idx]", (long)k);
		kw_put(buf);
		return;
	}
	if (kw_same(n, g_kw.elem)) { kw_bad(n, "gpu-each: the GPU works on the fields of %s, not on all of it: %s.x", s); return; }
	if (kw_same(n, g_kw.index)) {
		if (set) { kw_bad(n, "gpu-each: %s is the element's index; it cannot be set", s); return; }
		kw_put("idx");
		return;
	}
	if (!strcmp(s, "true") || !strcmp(s, "false")) { kw_put(s); return; }
	if (kw_local(s, g_nodes[n].stamp) >= 0) {
		kw_name(s, buf, sizeof(buf));
		kw_put(buf);
		return;
	}
	if (set) { kw_bad(n, "gpu-each: the body sets the fields of the element and its own variables; %s is outside the GPU", s); return; }
	pl.where = AT_LOCAL;
	pl.at = 0;
	pl.type = T_ERR;
	r = resolve_place(n, &pl);
	if (r <= 0) { g_kw.bad |= r < 0 ? 2 : 1; return; } /* unknown: the VM's compile of the body says so */
	if (pl.where == AT_ENV) { kw_bad(n, "gpu-each inside a fn: %s is the fn's, and the GPU cannot see it; put it in a variable of the fn first", s); return; }
	if (pl.type == T_BOOL) { kw_bad(n, "gpu-each: %s is a bool; give the GPU an u32, 0 or 1", s); return; }
	if (kw_kt(pl.type) < 0) {
		err_node(n, "gpu-each: %s is %s; what the GPU reads from around is f32, i32 or u32, like (def dt : f32 0.016)", s, tn(pl.type));
		g_kw.bad |= 1;
		return;
	}
	for (k = 0; k < g_kw.k.ncaps && (g_kw.k.cap_where[k] != pl.where || g_kw.k.cap_at[k] != pl.at); k++) {}
	if (k == g_kw.k.ncaps) {
		if (k == MAX_KCAPS) { kw_bad(n, "gpu-each: the body reads more than 16 values from around; put them in fields", s); return; }
		g_kw.k.cap_where[k] = (u8)pl.where;
		g_kw.k.cap_at[k] = pl.at;
		g_kw.k.cap_ty[k] = (u8)kw_kt(pl.type);
		g_kw.k.ncaps++;
	}
	sprintf(buf, "u.k%ld", (long)k);
	kw_put(buf);
}

/* the forms of an expression that a GPU runs, and how each is written in WGSL */
enum { KF_FOLD, KF_MINUS, KF_UNARY, KF_NOTFOLD, KF_CALL, KF_NEST, KF_SHIFT, KF_SELECT };
static const struct {
	const char *g, *w;
	u8 shape;
	i8 args;  /* exactly so many, -1 two or more, 0 one or more (- negates one) */
} g_kops[] = {
	{ "+", " + ", KF_FOLD, -1 }, { "-", " - ", KF_MINUS, 0 }, { "*", " * ", KF_FOLD, -1 }, { "/", " / ", KF_FOLD, -1 },
	{ "%", " % ", KF_FOLD, -1 }, { "<", " < ", KF_FOLD, 2 }, { "<=", " <= ", KF_FOLD, 2 }, { ">", " > ", KF_FOLD, 2 },
	{ ">=", " >= ", KF_FOLD, 2 }, { "=", " == ", KF_FOLD, 2 }, { "!=", " != ", KF_FOLD, 2 },
	{ "and", " && ", KF_FOLD, -1 }, { "or", " || ", KF_FOLD, -1 }, { "not", "!", KF_UNARY, 1 },
	{ "nand", " && ", KF_NOTFOLD, -1 }, { "nor", " || ", KF_NOTFOLD, -1 }, { "xor", " != ", KF_FOLD, -1 },
	{ "xnor", " != ", KF_NOTFOLD, -1 }, { "bit-and", " & ", KF_FOLD, -1 }, { "bit-or", " | ", KF_FOLD, -1 },
	{ "bit-xor", " ^ ", KF_FOLD, -1 }, { "bit-not", "~", KF_UNARY, 1 }, { "shl", " << ", KF_SHIFT, 2 },
	{ "shr", " >> ", KF_SHIFT, 2 }, { "sqrt", "sqrt", KF_CALL, 1 }, { "sin", "sin", KF_CALL, 1 },
	{ "cos", "cos", KF_CALL, 1 }, { "tan", "tan", KF_CALL, 1 }, { "asin", "asin", KF_CALL, 1 },
	{ "acos", "acos", KF_CALL, 1 }, { "atan", "atan", KF_CALL, 1 }, { "exp", "exp", KF_CALL, 1 },
	{ "ln", "log", KF_CALL, 1 }, { "floor", "floor", KF_CALL, 1 }, { "ceil", "ceil", KF_CALL, 1 },
	{ "atan2", "atan2", KF_CALL, 2 }, { "pow", "pow", KF_CALL, 2 }, { "f32", "f32", KF_CALL, 1 },
	{ "i32", "i32", KF_CALL, 1 }, { "u32", "u32", KF_CALL, 1 }, { "min", "min", KF_NEST, -1 },
	{ "max", "max", KF_NEST, -1 }, { "abs", "abs", KF_CALL, 1 }, { "clamp", "clamp", KF_CALL, 3 },
	{ "if", 0, KF_SELECT, 3 }
};

/* kw_soft_ok NAME STAMP: min and the like mean the built-in ones here: nothing of the program has their name */
static int kw_soft_ok(const char *s, u32 stamp)
{
	return find_fn(s) < 0 && find_macro(s) < 0 && find_global(s) < 0 && find_local(s, stamp) < 0 && kw_local(s, stamp) < 0;
}

/* kw_literal N: an int or a float as written; 0b1010 has no WGSL spelling, so it goes as decimal */
static o kw_literal(i32 n)
{
	char buf[64];
	const char *s = g_nodes[n].text;
	if (g_nodes[n].kind == N_INT && (strstr(s, "0b") || strstr(s, "0B"))) {
		if (g_nodes[n].big) sprintf(buf, "%lu", (unsigned long)(u64)g_nodes[n].ival);
		else sprintf(buf, "%ld", (long)g_nodes[n].ival);
		kw_put(buf);
		return;
	}
	if (g_nodes[n].kind == N_INT && g_nodes[n].ival < 0) kw_put("("); /* -5 in WGSL is a minus and a 5 */
	kw_put(s);
	if (g_nodes[n].lty == T_I32) kw_put("i"); /* the type it took in the VM's compile: 5u, 2.0f */
	else if (g_nodes[n].lty == T_U32) kw_put("u");
	else if (g_nodes[n].lty == T_F32) kw_put("f");
	if (g_nodes[n].kind == N_INT && g_nodes[n].ival < 0) kw_put(")");
}

/* kw_expr N: the expression N, its pieces pushed to be written in order */
static o kw_expr(i32 n)
{
	static i32 args[64];
	i32 head, x, na = 0, k;
	size_t op;
	const char *s;
	if (g_nodes[n].kind == N_INT || g_nodes[n].kind == N_FLOAT) { kw_literal(n); return; }
	if (g_nodes[n].kind == N_SYM) { kw_symbol(n, 0); return; }
	if (g_nodes[n].kind != N_LIST || !is_sym(head = first_of(n))) {
		kw_bad(n, "gpu-each: %s does not run on a GPU; the body works on f32, i32 and u32 numbers", g_nodes[n].kind == N_STR ? "a string" : "this");
		return;
	}
	s = g_nodes[head].text;
	for (x = next_of(head); x >= 0 && na < (i32)GAEL_COUNT(args); x = next_of(x)) args[na++] = x;
	for (op = 0; op < GAEL_COUNT(g_kops) && strcmp(s, g_kops[op].g); op++) {}
	if (op < GAEL_COUNT(g_kops) && (g_kops[op].shape == KF_NEST || !strcmp(s, "abs") || !strcmp(s, "clamp")) &&
	    !kw_soft_ok(s, g_nodes[head].stamp))
		op = GAEL_COUNT(g_kops); /* the program's own min: a call */
	if (op == GAEL_COUNT(g_kops)) {
		if (!strcmp(s, "let") || !strcmp(s, "do") || !strcmp(s, "when") || !strcmp(s, "set"))
			kw_bad(n, "gpu-each: %s gives no value on a GPU; put it around the set, as a statement", s);
		else if (find_fn(s) >= 0 || find_macro(s) >= 0)
			kw_bad(n, "gpu-each: %s is a function of the program, and a GPU kernel calls none; write what it does in the body", s);
		else if (!strcmp(s, "int") || !strcmp(s, "float") || !strcmp(s, "i64") || !strcmp(s, "f64") || !strcmp(s, "u64"))
			kw_bad(n, "gpu-each: %s is 64-bit, and a GPU works on 32-bit numbers: f32, i32, u32", s);
		else
			kw_bad(n, "gpu-each: %s does not run on a GPU; the body has let, set, if, when, do, arithmetic, comparisons, logic, bits, math and min, max, abs, clamp", s);
		return;
	}
	if ((g_kops[op].args > 0 && na != g_kops[op].args) || (g_kops[op].args == -1 && na < 2) || na == 0) {
		g_kw.bad |= 2; /* the wrong number of values: the VM's compile of the body says what is wrong */
		return;
	}
	/* where the VM stops with an error - an int divided by zero, a float too big for an int, a shift past the bits -
	 * WGSL would give a number; these call a function that marks the run failed instead, and the VM runs it again */
	if ((!strcmp(s, "/") || !strcmp(s, "%")) && (g_nodes[n].lty == T_I32 || g_nodes[n].lty == T_U32)) {
		static char name[3][2][16];
		i32 t = g_nodes[n].lty == T_I32 ? 0 : 1, o = s[0] == '/' ? 0 : 1;
		sprintf(name[0][0], "gael_div_i32");
		sprintf(name[0][1], "gael_rem_i32");
		sprintf(name[1][0], "gael_div_u32");
		sprintf(name[1][1], "gael_rem_u32");
		g_kw.k.fault = 1;
		for (k = na - 1; k >= 1; k--) {
			kw_push(KW_TEXT, -1, ")");
			kw_push(KW_EXPR, args[k], 0);
			kw_push(KW_TEXT, -1, ", ");
		}
		kw_push(KW_EXPR, args[0], 0);
		for (k = 1; k < na; k++) {
			kw_put(name[t][o]);
			kw_put("(");
		}
		return;
	}
	if ((!strcmp(s, "i32") || !strcmp(s, "u32")) && na == 1 && g_nodes[args[0]].lty == T_F32) {
		g_kw.k.fault = 1;
		kw_push(KW_TEXT, -1, ")");
		kw_push(KW_EXPR, args[0], 0);
		kw_put(s[0] == 'i' ? "gael_f2i32(" : "gael_f2u32(");
		return;
	}
	if (g_kops[op].shape == KF_SHIFT && (g_nodes[n].lty == T_I32 || g_nodes[n].lty == T_U32)) {
		g_kw.k.fault = 1;
		kw_push(KW_TEXT, -1, "))");
		kw_push(KW_EXPR, args[1], 0);
		kw_push(KW_TEXT, -1, ", u32(");
		kw_push(KW_EXPR, args[0], 0);
		kw_put(s[2] == 'l' ? (g_nodes[n].lty == T_I32 ? "gael_shl_i32(" : "gael_shl_u32(") : (g_nodes[n].lty == T_I32 ? "gael_shr_i32(" : "gael_shr_u32("));
		return;
	}
	switch (g_kops[op].shape) {
	case KF_MINUS:
		if (na == 1) {
			kw_push(KW_TEXT, -1, ")");
			kw_push(KW_EXPR, args[0], 0);
			kw_put("(-");
			return;
		}
		/* fall through */
	case KF_FOLD:
	case KF_NOTFOLD: /* ((a + b) + c), and !((a && b) && c) for nand */
		for (k = na - 1; k >= 1; k--) {
			kw_push(KW_TEXT, -1, ")");
			kw_push(KW_EXPR, args[k], 0);
			kw_push(KW_TEXT, -1, g_kops[op].w);
		}
		kw_push(KW_EXPR, args[0], 0);
		if (g_kops[op].shape == KF_NOTFOLD) kw_put("!");
		for (k = 1; k < na; k++) kw_put("(");
		return;
	case KF_UNARY:
		kw_push(KW_TEXT, -1, ")");
		kw_push(KW_EXPR, args[0], 0);
		kw_put("(");
		kw_put(g_kops[op].w);
		return;
	case KF_CALL:
		kw_push(KW_TEXT, -1, ")");
		for (k = na - 1; k >= 0; k--) {
			kw_push(KW_EXPR, args[k], 0);
			if (k) kw_push(KW_TEXT, -1, ", ");
		}
		kw_put(g_kops[op].w);
		kw_put("(");
		return;
	case KF_NEST: /* min(min(a, b), c) */
		for (k = na - 1; k >= 1; k--) {
			kw_push(KW_TEXT, -1, ")");
			kw_push(KW_EXPR, args[k], 0);
			kw_push(KW_TEXT, -1, ", ");
		}
		kw_push(KW_EXPR, args[0], 0);
		for (k = 1; k < na; k++) {
			kw_put(g_kops[op].w);
			kw_put("(");
		}
		return;
	case KF_SHIFT: /* (a << u32(n)): WGSL shifts by a u32 */
		kw_push(KW_TEXT, -1, "))");
		kw_push(KW_EXPR, args[1], 0);
		kw_push(KW_TEXT, -1, "u32(");
		kw_push(KW_TEXT, -1, g_kops[op].w);
		kw_push(KW_EXPR, args[0], 0);
		kw_put("(");
		return;
	default: /* KF_SELECT: (if c a b) is select(b, a, c); both sides are worked out, which costs nothing here */
		kw_push(KW_TEXT, -1, ")");
		kw_push(KW_EXPR, args[0], 0);
		kw_push(KW_TEXT, -1, ", ");
		kw_push(KW_EXPR, args[1], 0);
		kw_push(KW_TEXT, -1, ", ");
		kw_push(KW_EXPR, args[2], 0);
		kw_put("select(");
		return;
	}
}

/* kw_stmt N: the statement N - its pieces pushed to be written in order */
static o kw_stmt(i32 n)
{
	i32 head, x, k, nb = 0;
	const char *s;
	if (g_nodes[n].kind != N_LIST || !is_sym(head = first_of(n))) { /* a value on its own: no effect */
		kw_indent();
		kw_put("_ = ");
		kw_push(KW_TEXT, -1, ";\n");
		kw_push(KW_EXPR, n, 0);
		return;
	}
	s = g_nodes[head].text;
	if (!strcmp(s, "set")) {
		i32 to = next_of(head), v = next_of(to);
		if (!is_sym(to) || v < 0 || next_of(v) >= 0) { g_kw.bad |= 2; return; } /* the VM's compile says what is wrong */
		kw_indent();
		x = g_kw.bad;
		kw_symbol(to, 1);
		if ((g_kw.bad & 1) && !(x & 1)) return; /* the target is wrong: its value would only add a second message */
		kw_push(KW_TEXT, -1, ";\n");
		kw_push(KW_EXPR, v, 0);
		kw_push(KW_TEXT, -1, " = ");
		return;
	}
	if (!strcmp(s, "do")) {
		static i32 body[256];
		for (x = next_of(head); x >= 0 && nb < (i32)GAEL_COUNT(body); x = next_of(x)) body[nb++] = x;
		for (k = nb - 1; k >= 0; k--) kw_push(KW_STMT, body[k], 0);
		return;
	}
	if (!strcmp(s, "when") || !strcmp(s, "if")) {
		static i32 body[256];
		i32 c = next_of(head);
		if (c < 0) { g_kw.bad |= 2; return; }
		for (x = next_of(c); x >= 0 && nb < (i32)GAEL_COUNT(body); x = next_of(x)) body[nb++] = x;
		kw_indent();
		kw_put("if (");
		kw_push(KW_DEDENT, -1, "}\n");
		if (s[0] == 'i' && nb >= 2) { /* (if c a b) */
			kw_push(KW_STMT, body[1], 0);
			kw_push(KW_INDENT, -1, 0);
			kw_push(KW_DEDENT, -1, "} else {\n");
			kw_push(KW_STMT, body[0], 0);
		} else {
			for (k = nb - 1; k >= 0; k--) kw_push(KW_STMT, body[k], 0);
		}
		kw_push(KW_INDENT, -1, 0);
		kw_push(KW_TEXT, -1, ") {\n");
		kw_push(KW_EXPR, c, 0);
		return;
	}
	if (!strcmp(s, "let")) {
		static i32 binds[64], body[256];
		i32 bl = next_of(head), nl = 0;
		if (bl < 0 || g_nodes[bl].kind != N_LIST) { g_kw.bad |= 2; return; }
		for (x = first_of(bl); x >= 0 && nl < (i32)GAEL_COUNT(binds); x = next_of(x)) binds[nl++] = x;
		for (x = next_of(bl); x >= 0 && nb < (i32)GAEL_COUNT(body); x = next_of(x)) body[nb++] = x;
		kw_indent();
		kw_put("{\n");
		g_kw.indent++;
		kw_push(KW_SCOPE, nl, 0);
		kw_push(KW_DEDENT, -1, "}\n");
		for (k = nb - 1; k >= 0; k--) kw_push(KW_STMT, body[k], 0);
		for (k = nl - 1; k >= 0; k--) { /* var v_x: f32 = value;, its name seen from the next binding on */
			i32 name = first_of(binds[k]), val, ty = -1;
			if (g_nodes[binds[k]].kind != N_LIST || !is_sym(name) || (val = next_of(name)) < 0) { g_kw.bad |= 2; return; }
			if (sym_is(val, ":")) {
				ty = next_of(val);
				val = next_of(ty);
				if (!is_sym(ty) || val < 0) { g_kw.bad |= 2; return; }
				if (strcmp(g_nodes[ty].text, "f32") && strcmp(g_nodes[ty].text, "i32") && strcmp(g_nodes[ty].text, "u32") && strcmp(g_nodes[ty].text, "bool")) {
					kw_bad(ty, "gpu-each: a variable of the body is f32, i32, u32 or bool, not %s", g_nodes[ty].text);
					return;
				}
			} else if (g_nodes[val].kind == N_INT || g_nodes[val].kind == N_FLOAT) {
				err_node(val, "gpu-each: write the type of %s - on its own %s would be 64-bit, and a GPU works on 32-bit numbers: (%s : %s %s)",
				         g_nodes[name].text, g_nodes[val].text, g_nodes[name].text, g_nodes[val].kind == N_INT ? "i32" : "f32", g_nodes[val].text);
				g_kw.bad |= 1;
				return;
			}
			kw_push(KW_DECLARE, name, 0);
			kw_push(KW_TEXT, -1, ";\n");
			kw_push(KW_EXPR, val, 0);
			kw_push(KW_TEXT, -1, " = ");
			if (ty >= 0) kw_push(KW_TEXT, -1, g_nodes[ty].text);
			if (ty >= 0) kw_push(KW_TEXT, -1, ": ");
			kw_push(KW_DECLARE, -(name + 2), 0); /* writes "var v_x" at the binding's turn */
		}
		return;
	}
	if (!strcmp(s, "gpu-each")) { kw_bad(n, "gpu-each inside gpu-each: the body already runs once per element; %s", "use one"); return; }
	if (!strcmp(s, "while") || !strcmp(s, "for")) {
		kw_bad(n, "gpu-each: %s does not run on the GPU yet; the body runs once per element, so a loop over the elements is the gpu-each itself", s);
		return;
	}
	kw_indent();
	kw_put("_ = ");
	kw_push(KW_TEXT, -1, ";\n");
	kw_push(KW_EXPR, n, 0);
}

/* kernel_body EACH: the WGSL of the gpu-each EACH, (gpu-each (p v [i]) body...), into a new kernel - checked against
 * what a GPU runs; its number, or -1 after an error it reported */
static i32 kernel_body(i32 each, i32 body)
{
	i32 k;
	char buf[400];
	g_kw.len = 0;
	g_kw.bad = 0;
	g_kw.indent = 0;
	g_kw.nlocals = 0;
	g_kw.sp = 0;
	g_kw.out[0] = 0;
	{ /* the body's forms, pushed last first so they come out in order */
		static i32 forms[256];
		i32 nf = 0, x;
		for (x = body; x >= 0 && nf < (i32)GAEL_COUNT(forms); x = next_of(x)) forms[nf++] = x;
		for (k = nf - 1; k >= 0; k--) kw_push(KW_STMT, forms[k], 0);
	}
	while (g_kw.sp > 0) {
		KWork w = g_kw.st[--g_kw.sp];
		switch (w.kind) {
		case KW_STMT: kw_stmt(w.node); break;
		case KW_EXPR: kw_expr(w.node); break;
		case KW_TEXT: kw_put(w.text); break;
		case KW_INDENT: g_kw.indent++; break;
		case KW_DEDENT:
			g_kw.indent--;
			kw_indent();
			kw_put(w.text);
			break;
		case KW_SCOPE: g_kw.nlocals -= w.node; break;
		default: /* KW_DECLARE: the name of a binding, written (node -2 - n) or seen from here on (node n) */
			if (w.node <= -2) {
				kw_indent();
				kw_put("var ");
				kw_name(g_nodes[-w.node - 2].text, buf, sizeof(buf));
				kw_put(buf);
			} else if (g_kw.nlocals == (i32)GAEL_COUNT(g_kw.locals)) {
				kw_bad(w.node, "gpu-each: the body has more than 64 variables; %s is one too many", g_nodes[w.node].text);
			} else {
				g_kw.locals[g_kw.nlocals].name = g_nodes[w.node].text;
				g_kw.locals[g_kw.nlocals].stamp = g_nodes[w.node].stamp;
				g_kw.nlocals++;
			}
			break;
		}
	}
	(void)each;
	return g_kw.bad ? -1 : 0;
}

static o kt_add(char *to, size_t size, size_t *at, const char *s) /* S at the end of TO, as much as fits */
{
	while (*s && *at + 1 < size) to[(*at)++] = *s++;
	to[*at] = 0;
}

/* kernel_wgsl: the whole shader of the kernel being built, around the body in g_kw.out, into the permanent arena */
static const char *kernel_wgsl(void)
{
	static char text[KERNEL_TEXT + 4096];
	char line[200];
	size_t at = 0;
	i32 k;
	text[0] = 0;
#define KADD(s) kt_add(text, sizeof(text), &at, s)
	KADD("struct U {\n  n: u32,\n");
	for (k = 0; k < g_kw.k.ncaps; k++) {
		sprintf(line, "  k%ld: %s,\n", (long)k, g_kt_wgsl[g_kw.k.cap_ty[k]]);
		KADD(line);
	}
	KADD("}\n@group(0) @binding(0) var<uniform> u: U;\n");
	for (k = 0; k < g_kw.k.ncols; k++) {
		sprintf(line, "@group(0) @binding(%ld) var<storage, read_write> c%ld: array<%s>;\n", (long)k + 1, (long)k, g_kt_wgsl[g_kw.k.col_ty[k]]);
		KADD(line);
	}
	if (g_kw.k.fault) { /* the run fails here as the VM's loop would: the VM runs it again and says where */
		sprintf(line, "@group(0) @binding(%ld) var<storage, read_write> fault: atomic<u32>;\n", (long)g_kw.k.ncols + 1);
		KADD(line);
		KADD("fn gael_fail() { atomicStore(&fault, 1u); }\n");
		KADD("fn gael_div_i32(a: i32, b: i32) -> i32 { if (b == 0i) { gael_fail(); return 0i; } return a / b; }\n");
		KADD("fn gael_rem_i32(a: i32, b: i32) -> i32 { if (b == 0i) { gael_fail(); return 0i; } return a % b; }\n");
		KADD("fn gael_div_u32(a: u32, b: u32) -> u32 { if (b == 0u) { gael_fail(); return 0u; } return a / b; }\n");
		KADD("fn gael_rem_u32(a: u32, b: u32) -> u32 { if (b == 0u) { gael_fail(); return 0u; } return a % b; }\n");
		KADD("fn gael_f2i32(x: f32) -> i32 { if (!(x >= -2147483648.0 && x < 2147483648.0)) { gael_fail(); return 0i; } return i32(x); }\n");
		KADD("fn gael_f2u32(x: f32) -> u32 { if (!(x > -1.0 && x < 4294967296.0)) { gael_fail(); return 0u; } return u32(x); }\n");
		KADD("fn gael_shl_i32(a: i32, n: u32) -> i32 { if (n >= 32u) { gael_fail(); return 0i; } return a << n; }\n");
		KADD("fn gael_shr_i32(a: i32, n: u32) -> i32 { if (n >= 32u) { gael_fail(); return 0i; } return a >> n; }\n");
		KADD("fn gael_shl_u32(a: u32, n: u32) -> u32 { if (n >= 32u) { gael_fail(); return 0u; } return a << n; }\n");
		KADD("fn gael_shr_u32(a: u32, n: u32) -> u32 { if (n >= 32u) { gael_fail(); return 0u; } return a >> n; }\n");
	}
	KADD("@compute @workgroup_size(64)\n");
	KADD("fn main(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(num_workgroups) nwg: vec3<u32>) {\n");
	KADD("  let idx = gid.x + gid.y * nwg.x * 64u;\n  if (idx >= u.n) {\n    return;\n  }\n");
	KADD(g_kw.out);
	KADD("}\n");
#undef KADD
	return arena_strndup(&g_perm, text, at);
}

/* kernel_text K: the WGSL of kernel K, from its body once the VM's compile of it gave every literal its type (2.0f,
 * 5u); a second walk, which finds the same fields and values from around as the first */
static o kernel_text(i32 k)
{
	Kernel *kn = &g_kernels[k];
	if (g_unit.nerrors) return; /* nothing of the unit runs */
	g_kw.elem = kn->enode;
	g_kw.index = kn->inode;
	g_kw.ety = kn->elem;
	memset(&g_kw.k, 0, sizeof(g_kw.k));
	if (kernel_body(-1, kn->body) < 0 || g_kw.k.ncols != kn->ncols || g_kw.k.ncaps != kn->ncaps) {
		kn->wgsl = 0; /* cannot happen: the first walk found no error */
		return;
	}
	g_kw.k.vwhere = kn->vwhere; /* the rest as the first walk found it */
	g_kw.k.vat = kn->vat;
	kn->wgsl = kernel_wgsl();
	kn->fault = g_kw.k.fault; /* which only this walk can tell: it needs the types the VM's compile found */
}

/* gpu_each FRAME: (gpu-each (p v [i]) body...) - its kernel for the GPU, and in its place
 * (when (not (gpu kernel K)) (for (the index 0 (len v)) (let ((p (get v the index)) (i (u32 the index))) body... (put v the index p))))
 * for the VM, which runs when the GPU does not. 0 after an error that leaves nothing to compile */
static int gpu_each(Frame *f)
{
	i32 bl = next_of(first_of(f->node)), p = bl >= 0 && g_nodes[bl].kind == N_LIST ? first_of(bl) : -1;
	i32 v = next_of(p), ix = next_of(v), body = next_of(bl), at = f->node, kn = -1;
	i32 idx, loop, let, binds, get, put, gk, last, x;
	u32 stamp = ++g_stamps;
	Place pl;
	Ty vt;
	int r;
	if (!is_sym(p) || !is_sym(v) || (ix >= 0 && (!is_sym(ix) || next_of(ix) >= 0)) || body < 0) {
		err_node(f->node, "gpu-each takes the element, the vec and a body: (gpu-each (p particles) (set p.x (+ p.x p.vx)))");
		return 0;
	}
	if (strchr(g_nodes[p].text, '.') || is_reserved(g_nodes[p].text) || (ix >= 0 && (strchr(g_nodes[ix].text, '.') || is_reserved(g_nodes[ix].text)))) {
		err_node(ix >= 0 && (strchr(g_nodes[ix].text, '.') || is_reserved(g_nodes[ix].text)) ? ix : p, NAME_FMT " cannot be a variable name",
		         g_nodes[ix >= 0 && (strchr(g_nodes[ix].text, '.') || is_reserved(g_nodes[ix].text)) ? ix : p].text);
		return 0;
	}
	pl.where = AT_LOCAL;
	pl.at = 0;
	pl.type = T_ERR;
	r = resolve_place(v, &pl);
	if (r < 0) { err_node(v, "unknown name " NAME_FMT "; gpu-each goes over a vec in a variable: (gpu-each (p particles) ...)", g_nodes[v].text); return 0; }
	if (r == 0) return 0;
	vt = pl.type;
	if (!is_vec(vt)) {
		if (vt != T_ERR) err_node(v, "gpu-each goes over a vec, and " NAME_FMT " is %s", g_nodes[v].text, tn(vt));
		return 0;
	}
	if (pl.where == AT_ENV || strchr(g_nodes[v].text, '.')) {
		err_node(v, "gpu-each goes over a vec in a variable of its own: (let ((ps %s)) (gpu-each (p ps) ...))", g_nodes[v].text);
		return 0;
	}
	g_kw.elem = p;
	g_kw.index = ix;
	g_kw.ety = g_tys[vt].elem;
	memset(&g_kw.k, 0, sizeof(g_kw.k));
	kernel_body(f->node, body);
	if (g_kw.bad & 1) return 0; /* said what a GPU does not run: the VM's compile of the same body would only repeat it */
	if (!g_kw.bad) {
		if (g_nkernels == MAX_KERNELS) die("too many gpu-each (MAX_KERNELS)");
		kn = g_nkernels++;
		g_kw.k.wgsl = 0; /* (gpu wgsl K), after the VM's compile of the body */
		g_kw.k.body = body;
		g_kw.k.enode = p;
		g_kw.k.inode = ix;
		g_kw.k.file = g_unit.file;
		g_kw.k.line = g_nodes[f->node].line;
		g_kw.k.elem = g_kw.ety;
		g_kw.k.vwhere = (u8)pl.where;
		g_kw.k.vat = pl.at;
		g_kernels[kn] = g_kw.k;
		gpu_forget(kn);
	}
	/* the VM's loop */
	idx = mk_sym("the index", at, stamp);
	get = mk_list(at, mk_sym("get", at, 0), node_copy(v), mk_sym("the index", at, stamp), -1);
	binds = mk_list(at, mk_list(at, node_copy(p), get, -1, -1), -1, -1, -1);
	if (ix >= 0) g_nodes[first_of(binds)].next = mk_list(at, node_copy(ix), mk_list(at, mk_sym("u32", at, 0), mk_sym("the index", at, stamp), -1, -1), -1, -1);
	put = mk_list(at, mk_sym("put", at, 0), node_copy(v), mk_sym("the index", at, stamp), node_copy(p));
	let = mk_list(at, mk_sym("let", at, 0), binds, -1, -1);
	last = binds;
	for (x = body; x >= 0; x = next_of(x)) { /* the body's forms, each on its own, so the tree as written stays */
		g_nodes[last].next = node_copy(x);
		last = g_nodes[last].next;
	}
	g_nodes[last].next = put;
	loop = mk_list(at, mk_sym("for", at, 0), mk_list(at, idx, mk_int(at, 0), mk_list(at, mk_sym("len", at, 0), node_copy(v), -1, -1), -1), let, -1);
	if (kn >= 0) { /* (do (when (not (gpu kernel K)) loop) (gpu wgsl K)) */
		gk = mk_list(at, mk_sym("gpu kernel", at, 0), mk_int(at, kn), -1, -1);
		loop = mk_list(at, mk_sym("when", at, 0), mk_list(at, mk_sym("not", at, 0), gk, -1, -1), loop, -1);
		loop = mk_list(at, mk_sym("do", at, 0), loop, mk_list(at, mk_sym("gpu wgsl", at, 0), mk_int(at, kn), -1, -1), -1);
	}
	f->node = loop;
	f->cur = -1;
	f->k = 0;
	return 1;
}
