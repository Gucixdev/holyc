/* read.c - source text to a flat tree of s-expressions.
 *
 * Every form is a Node in one array; a list links its children by index (first, next). The reader keeps its own
 * stack of open lists instead of recursing, so nesting depth is a number in this file, not the C stack.
 * Errors carry the line and column of the character that caused them. */

typedef enum { N_LIST, N_INT, N_FLOAT, N_STR, N_SYM, N_KEY } NodeKind;

typedef struct {
	u8 kind;
	i32 first, next; /* children and siblings by index, -1 for none */
	i32 line, col;
	i64 ival;
	u8 big;   /* an int literal above 2^63 - 1: its bits are in ival, and only a u64 holds it */
	u8 lty;   /* the type it got when compiled, when that is a built-in one (kernel.c writes WGSL with it) */
	i32 src;  /* the node this one is a copy of, which then gets its lty too; -1 */
	f64 fval;
	const char *text; /* symbols, keywords, strings: NUL-terminated, in the unit arena */
	u32 len;
	u32 stamp; /* the macro expansion that wrote it, 0 for the program's own text (macro.c) */
} Node;

#define MAX_NODES (1 << 21)
#define UNIT_BYTES ((size_t)64 << 20) /* the source of one unit and its names */
#define MAX_DEPTH 256
#define MAX_ERRORS 32

static Node *g_nodes; /* reserved address space for MAX_NODES, and the arena too (compile.c's tables_init) */
static i32 g_nnodes;

static Arena g_unit_arena = { 0, 0, 0, "unit" };

typedef struct {
	const char *file;
	const char *src;
	size_t len;
	i32 nerrors, nwarnings;
} Unit;

static Unit g_unit;
static i32 g_nwarnings; /* every warning of the whole run: a release build refuses to go on with any */

static o show_source_line(i32 line, i32 col)
{
	const char *p = g_unit.src, *end = g_unit.src + g_unit.len, *eol;
	i32 l = 1, k;
	while (p < end && l < line) {
		if (*p++ == '\n') l++;
	}
	if (l != line || p >= end) return;
	eol = p;
	while (eol < end && *eol != '\n') eol++;
	fprintf(stderr, "  %.*s\n  ", (int)GAEL_MIN(eol - p, 200), p);
	for (k = 1; k < col && k < 200; k++) fputc(p[k - 1] == '\t' ? '\t' : ' ', stderr);
	fputs("^\n", stderr);
}

/* said_before FILE LINE COL MSG: the diagnostic just printed was this one - code the compiler wrote twice, like the
 * two (- x) of (abs x), would say it twice. 0, and remembered, when it is new */
static int said_before(const char *file, i32 line, i32 col, const char *msg)
{
	static char last[1024];
	static const char *last_file;
	static i32 last_line = -1, last_col = -1;
	if (file == last_file && line == last_line && col == last_col && !strcmp(msg, last)) return 1;
	last_file = file;
	last_line = line;
	last_col = col;
	strcpy(last, msg);
	return 0;
}

/* error_at LINE COL FORMAT ...: one diagnostic, "file:line:col: error: ...", with the source line under it. The
 * arguments must stay short (names are passed through %.*s with a cap), so the fixed buffer cannot overflow. */
static o error_at(i32 line, i32 col, const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	g_unit.nerrors++;
	if (g_unit.nerrors > MAX_ERRORS) {
		if (g_unit.nerrors == MAX_ERRORS + 1) fprintf(stderr, "%s: too many errors, stopping here\n", g_unit.file);
		return;
	}
	va_start(ap, fmt);
	vsprintf(msg, fmt, ap);
	va_end(ap);
	if (said_before(g_unit.file, line, col, msg)) return;
	fprintf(stderr, "%s:%ld:%ld: error: %s\n", g_unit.file, (long)line, (long)col, msg);
	show_source_line(line, col);
}

/* warning_at LINE COL FORMAT ...: "file:line:col: warning: ...", for code that runs but is almost surely a mistake. The
 * program still runs; only a release build stops at one */
static o warning_at(i32 line, i32 col, const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	g_unit.nwarnings++;
	g_nwarnings++;
	if (g_unit.nwarnings > MAX_ERRORS) {
		if (g_unit.nwarnings == MAX_ERRORS + 1) fprintf(stderr, "%s: too many warnings, showing no more\n", g_unit.file);
		return;
	}
	va_start(ap, fmt);
	vsprintf(msg, fmt, ap);
	va_end(ap);
	if (said_before(g_unit.file, line, col, msg)) return;
	fprintf(stderr, "%s:%ld:%ld: warning: %s\n", g_unit.file, (long)line, (long)col, msg);
	show_source_line(line, col);
}

static i32 new_node(NodeKind kind, i32 line, i32 col)
{
	Node *n;
	if (g_nnodes == MAX_NODES) die("too many forms in one unit (MAX_NODES)");
	n = &g_nodes[g_nnodes];
	memset(n, 0, sizeof(*n));
	n->kind = (u8)kind;
	n->first = n->next = n->src = -1;
	n->line = line;
	n->col = col;
	return g_nnodes++;
}

static int is_delim(int c)
{
	return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '(' || c == ')' || c == '"' || c == ';';
}

/* digit_value C: the value of a digit in bases up to 16, 99 for anything else */
static u64 digit_value(char c)
{
	if (c >= '0' && c <= '9') return (u64)(c - '0');
	if (c >= 'a' && c <= 'f') return (u64)(c - 'a' + 10);
	if (c >= 'A' && c <= 'F') return (u64)(c - 'A' + 10);
	return 99;
}

/* int_base TEXT LEN AT: 16 for 0x..., 2 for 0b..., else 10; *AT moves past the prefix */
static u64 int_base(const char *s, size_t n, size_t *at)
{
	if (*at + 1 < n && s[*at] == '0' && (s[*at + 1] == 'x' || s[*at + 1] == 'X' || s[*at + 1] == 'b' || s[*at + 1] == 'B')) {
		*at += 2;
		return s[*at - 1] == 'x' || s[*at - 1] == 'X' ? 16 : 2;
	}
	return 10;
}

/* atom TEXT LEN: the kind of a token - an int (42, 0xff, 0b1010), a float, a :keyword, else a symbol */
static NodeKind atom_kind(const char *s, size_t n)
{
	size_t i = 0, digits = 0;
	u64 base;
	if (n > 1 && (s[0] == '-' || s[0] == '+')) i = 1;
	if ((base = int_base(s, n, &i)) != 10) {
		while (i < n && digit_value(s[i]) < base) i++, digits++;
		return digits && i == n ? N_INT : N_SYM;
	}
	while (i < n && s[i] >= '0' && s[i] <= '9') i++, digits++;
	if (digits && i == n) return N_INT;
	if (digits && i < n && s[i] == '.') {
		size_t frac = 0;
		i++;
		while (i < n && s[i] >= '0' && s[i] <= '9') i++, frac++;
		if (frac && i < n && (s[i] == 'e' || s[i] == 'E')) {
			size_t exp = 0;
			i++;
			if (i < n && (s[i] == '-' || s[i] == '+')) i++;
			while (i < n && s[i] >= '0' && s[i] <= '9') i++, exp++;
			if (!exp) return N_SYM;
		}
		if (frac && i == n) return N_FLOAT;
	}
	if (s[0] == ':' && n > 1) return N_KEY;
	return N_SYM;
}

/* like_number TEXT LEN: starts with a digit (after an optional sign) or with '.' and a digit; such a token that is
 * not an int or a float is a typo in a number, not a name */
static int like_number(const char *s, size_t n)
{
	size_t i = (n > 1 && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
	if (i + 1 < n && s[i] == '.') i++;
	return i < n && s[i] >= '0' && s[i] <= '9';
}

/* number_fix TEXT LEN OUT: the float that the typo TEXT most likely means - 1e-9 as 1.0e-9, .5 as 0.5, 5. as 5.0 - in
 * OUT (80 bytes); 0 when there is none */
static int number_fix(const char *s, size_t n, char *out)
{
	size_t lead = (s[0] == '-' || s[0] == '+') ? 1 : 0, o = 0, k;
	if (n > 60) return 0;
	for (k = 0; k < n; k++) {
		if (s[k] == '.' && k == lead) out[o++] = '0';
		if ((s[k] == 'e' || s[k] == 'E') && !memchr(s, '.', k)) {
			out[o++] = '.';
			out[o++] = '0';
		}
		out[o++] = s[k];
		if (s[k] == '.' && (k + 1 == n || s[k + 1] == 'e' || s[k + 1] == 'E')) out[o++] = '0';
	}
	out[o] = 0;
	return atom_kind(out, o) == N_FLOAT;
}

/* Paren mistakes, read from the indentation the way parinfer reads it; used only when the parens do not balance. The
 * first column of a code line closes, at the end of the code line before it, every list opened at that column or
 * right of it; the closing parens that line ends with are counted against that, and where the counts differ a ')' is
 * missing or one too many. A line that starts with ')' adds those to the code line before. At the end of the file
 * every list still open closes. */
#define PAREN_TRAIL 64
typedef struct {
	i32 line, col;
} ParenAt;

typedef struct {
	ParenAt st[MAX_DEPTH];         /* the lists open by the indentation */
	i32 sp;
	i32 line, end;                 /* the last code line: its number, the column after its last character */
	ParenAt trail[PAREN_TRAIL];    /* the closing parens it ends with */
	i32 ntrail;
	i32 found, net;                /* mistakes found; ')' missing minus ')' too many */
	int quiet;                     /* count only, report nothing */
} ParenRead;

static ParenRead g_pr;

/* paren_close NEXT COL: the last code line is over; the next one is NEXT and starts at column COL (NEXT 0: the end of
 * the file), so the lists from COL on close at the end of it. Reports where that disagrees with its closing parens */
static o paren_close(i32 next, i32 col)
{
	ParenRead *pr = &g_pr;
	char what[200];
	i32 k = 0, t = pr->ntrail, i, n;
	size_t used = 0;
	while (pr->sp > 0 && (!next || pr->st[pr->sp - 1].col >= col)) {
		pr->sp--;
		k++;
	}
	pr->ntrail = 0;
	if (k == t || !pr->line) return;
	pr->found++;
	pr->net += k - t;
	if (pr->quiet) return;
	what[0] = 0;
	if (k > t) { /* the outermost k - t of the lists closed here have no ')' */
		n = k - t;
		for (i = pr->sp + n - 1; i >= pr->sp && used + 40 < sizeof(what); i--) {
			sprintf(what + used, "%sthe ( at %ld:%ld", i == pr->sp + n - 1 ? "" : i == pr->sp ? " and " : ", ",
			        (long)pr->st[i].line, (long)pr->st[i].col);
			used = strlen(what);
		}
		if (next && n == 1)
			error_at(pr->line, pr->end, "a ')' is missing at the end of line %ld: line %ld is indented as if %s ended there",
			         (long)pr->line, (long)next, what);
		else if (next)
			error_at(pr->line, pr->end, "%ld ')' are missing at the end of line %ld: line %ld is indented as if %s ended there",
			         (long)n, (long)pr->line, (long)next, what);
		else if (n == 1)
			error_at(pr->line, pr->end, "a ')' is missing at the end of line %ld, the last: %s is still open", (long)pr->line, what);
		else
			error_at(pr->line, pr->end, "%ld ')' are missing at the end of line %ld, the last: %s are still open", (long)n,
			         (long)pr->line, what);
		return;
	}
	n = t - k; /* the closing parens from the (k + 1)th on would close what the indentation says goes on */
	if (pr->sp > 0 && next)
		sprintf(what, "line %ld is indented as part of the ( at %ld:%ld, which %s would close", (long)next,
		        (long)pr->st[pr->sp - 1].line, (long)pr->st[pr->sp - 1].col, n == 1 ? "it" : "they");
	else
		strcpy(what, "nothing is open there");
	i = GAEL_MIN(k, PAREN_TRAIL - 1);
	if (n == 1) error_at(pr->trail[i].line, pr->trail[i].col, "a ')' too many at the end of line %ld: %s; remove it", (long)pr->line, what);
	else error_at(pr->trail[i].line, pr->trail[i].col, "%ld ')' too many at the end of line %ld: %s; remove them", (long)n,
	              (long)pr->line, what);
}

/* paren_scan: one pass over the unit's text with g_pr; 0 when it nests too deep to tell anything */
static int paren_scan(void)
{
	ParenRead *pr = &g_pr;
	const char *s = g_unit.src;
	size_t i = 0, n = g_unit.len;
	i32 line = 1, col = 1, npend = 0, k;
	ParenAt pend[PAREN_TRAIL]; /* ')' after the last other token of this line: they end it, or more code follows */
	int in_str = 0, start = 1, code = 0; /* start: nothing but ')' on this line yet; code: it has some */
	pr->sp = pr->line = pr->end = pr->ntrail = pr->found = pr->net = 0;
	while (i <= n) {
		char c = i < n ? s[i] : '\n';
		if (in_str) { /* a string runs on, maybe over lines; a line that starts inside one is no code line */
			if (i == n) break;
			if (c == '\\' && i + 1 < n && s[i + 1] != '\n') { i += 2, col += 2; continue; }
			if (c == '"') { in_str = 0; pr->line = line; pr->end = col + 1; }
			if (c == '\n') { line++; col = 1; start = 0; } else col++;
			i++;
			continue;
		}
		if (c == '\n') { /* the closing parens a code line ends with are its trail */
			if (code) {
				for (k = 0; k < npend && k < PAREN_TRAIL; k++) pr->trail[k] = pend[k];
				pr->ntrail = npend;
			}
			npend = 0;
			code = 0;
			start = 1;
			if (i == n) break;
			line++, col = 1, i++;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\r') { i++, col++; continue; }
		if (c == ';') { while (i < n && s[i] != '\n') i++, col++; continue; }
		if (c == ')') {
			if (start) { /* a line that starts with ')': they end the code line before */
				if (pr->ntrail < PAREN_TRAIL) {
					pr->trail[pr->ntrail].line = line;
					pr->trail[pr->ntrail].col = col;
				}
				pr->ntrail++;
			} else {
				if (npend < PAREN_TRAIL) {
					pend[npend].line = line;
					pend[npend].col = col;
				}
				npend++;
				pr->end = col + 1;
			}
			i++, col++;
			continue;
		}
		if (start) { /* the first column of a code line closes what it says */
			paren_close(line, col);
			start = 0;
		}
		code = 1;
		for (k = 0; k < npend; k++) { /* ')' with more code after them on the line close lists now */
			if (pr->sp > 0) {
				pr->sp--;
				continue;
			}
			pr->found++;
			pr->net--;
			if (!pr->quiet) {
				const ParenAt *at = &pend[GAEL_MIN(k, PAREN_TRAIL - 1)];
				error_at(at->line, at->col, "unexpected ')': nothing is open here; remove it");
			}
		}
		npend = 0;
		pr->line = line;
		if (c == '(') {
			if (pr->sp == MAX_DEPTH) return 0;
			pr->st[pr->sp].line = line;
			pr->st[pr->sp].col = col;
			pr->sp++;
			i++, col++;
			pr->end = col;
		} else if (c == '"') {
			in_str = 1;
			i++, col++;
		} else { /* an atom */
			while (i < n && !is_delim((unsigned char)s[i])) i++, col++;
			pr->end = col;
		}
	}
	paren_close(0, 0);
	return 1;
}

/* paren_hints NET: report the paren mistakes the indentation shows and return 1; 0, reporting nothing, when it shows
 * none or they do not add up to NET - the ')' the file misses, minus those it has too many - so it cannot be trusted */
static int paren_hints(i32 net)
{
	g_pr.quiet = 1;
	if (!paren_scan() || !g_pr.found || g_pr.net != net) return 0;
	g_pr.quiet = 0;
	return paren_scan();
}

/* read_unit: parse g_unit.src; returns the first top-level node (the rest follow by `next`), -1 when empty */
static i32 read_unit(void)
{
	const char *s = g_unit.src;
	size_t i = 0, n = g_unit.len;
	i32 line = 1, col = 1;
	i32 open[MAX_DEPTH];      /* the lists not closed yet */
	i32 last[MAX_DEPTH + 1];  /* last[0]: last top-level form; last[d]: last child of open[d - 1] */
	i32 depth = 0, top = -1, node, k;
	ParenAt extra[MAX_ERRORS]; /* ')' with nothing open: told after the whole unit, from its indentation if it can */
	i32 nextra = 0;
	int open_str = 0;          /* a string not closed took the rest of the text: the parens in it are not missing */

	last[0] = -1;
	while (i < n) {
		char c = s[i];
		i32 l0 = line, c0 = col;
		if (c == '\n') { i++; line++; col = 1; continue; }
		if (c == ' ' || c == '\t' || c == '\r') { i++; col++; continue; }
		if (c == ';') {
			while (i < n && s[i] != '\n') i++, col++;
			continue;
		}
		if (c == ')') {
			i++, col++;
			if (!depth) {
				if (nextra < MAX_ERRORS) {
					extra[nextra].line = l0;
					extra[nextra].col = c0;
				}
				nextra++;
				continue;
			}
			depth--;
			continue;
		}
		if (c == '(') {
			i++, col++;
			node = new_node(N_LIST, l0, c0);
		} else if (c == '"') {
			char *buf;
			size_t len = 0, j = i + 1;
			int closed = 0;
			/* escapes only shrink a string: the distance to the closing quote is enough room */
			while (j < n && s[j] != '"') j += (s[j] == '\\' && j + 1 < n) ? 2 : 1;
			buf = (char *)arena_alloc(&g_unit_arena, j - i + 1);
			i++, col++;
			while (i < n) {
				char d = s[i];
				if (d == '"') { i++, col++; closed = 1; break; }
				if (d == '\n') { line++; col = 1; buf[len++] = d; i++; continue; }
				if (d == '\\' && i + 1 < n) {
					char e = s[i + 1];
					i += 2, col += 2;
					switch (e) {
					case 'n': buf[len++] = '\n'; break;
					case 't': buf[len++] = '\t'; break;
					case '\\': buf[len++] = '\\'; break;
					case '"': buf[len++] = '"'; break;
					case '0': buf[len++] = '\0'; break;
					default: error_at(line, col - 2, "unknown escape \\%c in a string", e); break;
					}
					continue;
				}
				buf[len++] = d;
				i++, col++;
			}
			buf[len] = 0;
			if (!closed) {
				error_at(l0, c0, "string not closed");
				open_str = 1;
			}
			node = new_node(N_STR, l0, c0);
			g_nodes[node].text = buf;
			g_nodes[node].len = (u32)len;
		} else {
			size_t start = i, len;
			NodeKind kind;
			i32 prev = last[depth];
			while (i < n && !is_delim((unsigned char)s[i])) i++, col++;
			len = i - start;
			if (s[start] == '.' && len > 1 && start > 0 && s[start - 1] == ')' && prev >= 0 && g_nodes[prev].kind == N_LIST) {
				/* (f x).pos.x reads as (. (f x) pos.x): the list just closed becomes that form, in place */
				i32 copy = new_node(N_LIST, g_nodes[prev].line, g_nodes[prev].col);
				i32 dot = new_node(N_SYM, l0, c0), path = new_node(N_SYM, l0, c0 + 1);
				g_nodes[copy].first = g_nodes[prev].first;
				g_nodes[dot].text = ".";
				g_nodes[dot].len = 1;
				g_nodes[path].text = arena_strndup(&g_unit_arena, s + start + 1, len - 1);
				g_nodes[path].len = (u32)(len - 1);
				g_nodes[prev].first = dot;
				g_nodes[dot].next = copy;
				g_nodes[copy].next = path;
				continue;
			}
			kind = atom_kind(s + start, len);
			node = new_node(kind, l0, c0);
			g_nodes[node].text = arena_strndup(&g_unit_arena, s + start, len);
			g_nodes[node].len = (u32)len;
			if (kind == N_INT) {
				u64 v = 0, limit, base;
				int neg = s[start] == '-';
				size_t j = (s[start] == '-' || s[start] == '+') ? 1 : 0;
				base = int_base(s + start, len, &j);
				limit = neg ? (u64)1 << 63 : ~(u64)0;
				for (; j < len; j++) {
					u64 dgt = digit_value(s[start + j]);
					if (v > (limit - dgt) / base) { error_at(l0, c0, "integer does not fit in 64 bits"); v = 0; break; }
					v = v * base + dgt;
				}
				g_nodes[node].ival = neg ? (i64)(0 - v) : (i64)v;
				g_nodes[node].big = !neg && v > ((u64)1 << 63) - 1;
			} else if (kind == N_FLOAT) {
				g_nodes[node].fval = strtod(g_nodes[node].text, 0);
			} else if (kind == N_SYM && like_number(s + start, len)) {
				char fix[80];
				if (number_fix(s + start, len, fix))
					error_at(l0, c0, "%.*s is not a number; a float has digits on both sides of its point: %s", (int)len, s + start, fix);
				else
					error_at(l0, c0, "%.*s is not a number; an int is written 42, a float 4.2 or 4.2e3",
						(int)(len < 64 ? len : 64), s + start); /* msg is 1024 bytes */
			}
		}
		/* link the new form under the innermost open list, or at the top level */
		if (!depth) {
			if (last[0] < 0) top = node; else g_nodes[last[0]].next = node;
			last[0] = node;
		} else {
			i32 parent = open[depth - 1];
			if (g_nodes[parent].first < 0) g_nodes[parent].first = node; else g_nodes[last[depth]].next = node;
			last[depth] = node;
		}
		if (g_nodes[node].kind == N_LIST) {
			if (depth == MAX_DEPTH) { error_at(l0, c0, "nested deeper than %d lists", MAX_DEPTH); return top; }
			open[depth++] = node;
			last[depth] = -1;
		}
	}
	if ((nextra || depth) && !open_str && !paren_hints(depth - nextra)) {
		for (k = 0; k < nextra && k < MAX_ERRORS; k++) error_at(extra[k].line, extra[k].col, "unexpected ')'");
		for (k = depth; k > 0; k--) error_at(g_nodes[open[k - 1]].line, g_nodes[open[k - 1]].col, "'(' not closed");
	}
	return top;
}
