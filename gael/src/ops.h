/* ops.h - every VM instruction, once. The enum, the names for the disassembler and the operand count all come
 * from this list (an x-macro), so they cannot drift apart.
 *
 * X(NAME, operands): operand a is a constant, string, slot, function, type, format or jump target; b is a count.
 * Stack effects are in the comments; i = int, f = float, b = bool, s = string, x... = a value of several cells.
 * An instruction consumes the values it takes: the strings, vecs and maps in them lose an owner, unless the
 * instruction keeps them (a vec keeps the element pushed into it). A value it pushes is owned by whoever takes it.
 * Where an operand says "borrowed", the value is a variable's that the instruction only reads: it was pushed without
 * RETAIN and is not released. */

#ifndef GAEL_OPS_H_
#define GAEL_OPS_H_

#define GAEL_OPS(X) \
	X(HALT, 0)     /* stop the program */ \
	X(ENTER, 2)    /* -- 0...      a: slots of the top-level frame, zeroed; b: its deepest operand stack */ \
	X(PUSH_I, 1)   /* -- i         a: index into the int constants */ \
	X(PUSH_F, 1)   /* -- f         a: index into the float constants */ \
	X(PUSH_S, 1)   /* -- s         a: string constant */ \
	X(PUSH_K, 1)   /* -- k         a: the value itself (a bool, an enum variant, an option tag) */ \
	X(ZERO, 1)     /* -- 0...      a: how many zero cells (the inside of none) */ \
	X(POP, 0)      /* x -- */ \
	X(POPN, 1)     /* x... --      a: cells */ \
	X(LOAD_L, 1)   /* -- x         a: local slot */ \
	X(STORE_L, 1)  /* x --         a: local slot */ \
	X(LOAD_G, 1)   /* -- x         a: global cell */ \
	X(STORE_G, 1)  /* x --         a: global cell */ \
	X(LOADN_L, 2)  /* -- x...      a: first slot, b: cells */ \
	X(STOREN_L, 2) /* x... --      a: first slot, b: cells */ \
	X(LOADN_G, 2)  /* -- x...      a: first global cell, b: cells */ \
	X(STOREN_G, 2) /* x... --      a: first global cell, b: cells */ \
	X(RETAIN, 1)   /* x -- x       a: the type; the objects in the value on top get one owner more (it is a copy) */ \
	X(DROP, 1)     /* x --         a: the type; throw the value away, its objects lose an owner */ \
	X(RELEASE_L, 2) /* --          a: first slot, b: the type; the value kept there dies */ \
	X(RELEASE_G, 2) /* --          a: first global cell, b: the type */ \
	X(ADD_I, 0) X(SUB_I, 0) X(MUL_I, 0) X(DIV_I, 0) X(MOD_I, 0) X(NEG_I, 0) \
	X(DIV_U, 0) X(MOD_U, 0) /* u64 */ \
	X(BAND, 0) X(BOR, 0) X(BXOR, 0) X(BNOT, 0) /* the bits of ints */ \
	X(SHL, 1) X(SHR, 1) /* i n -- i  a: the int type; n from 0 to its bits - 1; shr keeps the sign of a signed type */ \
	X(WRAP, 1)     /* i -- i       a: an int type narrower than 64 bits; the value wraps around into it */ \
	X(F32, 0)      /* f -- f       round to f32 (too big for it: infinity) */ \
	X(ADD_F, 0) X(SUB_F, 0) X(MUL_F, 0) X(DIV_F, 0) X(NEG_F, 0) \
	X(LT_I, 0) X(LE_I, 0) X(GT_I, 0) X(GE_I, 0) X(EQ_I, 0) X(NE_I, 0) \
	X(LT_F, 0) X(LE_F, 0) X(GT_F, 0) X(GE_F, 0) X(EQ_F, 0) X(NE_F, 0) \
	X(LT_U, 0) X(LE_U, 0) X(GT_U, 0) X(GE_U, 0) /* u64 */ \
	X(EQ_S, 1) X(NE_S, 1) /* a: bit 0 the first string is borrowed, bit 1 the second */ \
	X(NOT, 0)      /* b -- b */ \
	X(JMP, 1)      /* --           a: target */ \
	X(JF, 1)       /* b --         jump to a when false */ \
	X(JT, 1)       /* b --         jump to a when true */ \
	X(I2F, 0)      /* i -- f */ \
	X(U2F, 0)      /* u64 -- f */ \
	X(F2I, 1)      /* f -- i       truncates toward zero; a: the int type, a value outside it is a runtime error */ \
	X(LEN_S, 1)    /* s -- i       length in bytes; a: borrowed */ \
	X(CALL, 1)     /* args -- r    a: function index */ \
	X(CALL_V, 1)   /* args f -- r  call the function value on top (its number + 1); a: its type */ \
	X(TAILCALL, 1) /* args --      a call that is the function's last act: the callee takes over this frame */ \
	X(TAILCALL_V, 1) /* args f --  the same through a function value */ \
	X(RET_V, 0)    /* r --         return with a one-cell value */ \
	X(RET_N, 1)    /* r... --      return with a value of a cells */ \
	X(RET_U, 0)    /* --           return without a value */ \
	X(PRINT, 2)    /* args --      a: format record (the string and the type of each value), b: 1 for stderr */ \
	X(PRINTLN, 2)  /* args --      the same, then a newline */ \
	X(FORMAT, 1)   /* args -- s    the same, into a new string */ \
	X(SH, 0)       /* s -- s       run a shell command, its output without the newlines at the end */ \
	X(SYSTEM, 0)   /* s -- i       run a shell command, its exit status */ \
	X(RUN, 0)      /* s -- b       run a shell command; true when it exits with 0 */ \
	X(PARSE_I, 0)  /* s -- o       (option int): the string as an int, or none */ \
	X(PARSE_F, 0)  /* s -- o       (option float) */ \
	X(SUBSTR, 0)   /* s i i -- s   the bytes from the first index up to the second (a copy; of the program's text, a view) */ \
	X(BYTE, 1)     /* s i -- i     one byte, 0 to 255; a: the string is borrowed */ \
	X(FIND, 0)     /* s s -- o     (option int): where the second string first starts in the first */ \
	X(SPLIT, 0)    /* s s -- v     (vec string): the pieces between the separators */ \
	X(JOIN, 0)     /* v s -- s     the strings of a vec with the separator between them, a new string */ \
	X(CHAR, 0)     /* i -- s       the one-byte string of 0 to 255 */ \
	X(TRIM, 0)     /* s -- s       without the spaces, tabs and newlines around it */ \
	X(MATH, 1)     /* f -- f       a: sqrt sin cos tan asin acos atan exp ln floor ceil, in that order */ \
	X(MATH2, 1)    /* f f -- f     a: atan2 (y x), pow (x y) */ \
	X(READ_FILE, 0)  /* s -- o     (option string): the whole file, or none when it cannot be read */ \
	X(WRITE_FILE, 2) /* s s -- b   write a string to a file, false on failure; a: 1 appends, b: 1 when the text comes first */ \
	X(READ_LINE, 0)  /* -- o       (option string): one line of standard input without its newline; none at the end */ \
	X(GETENV, 0)   /* s -- o       (option string): an environment variable */ \
	X(ARGS, 0)     /* -- v         (vec string): the arguments after the file name */ \
	X(EXIT, 0)     /* i --         end the program with this exit status */ \
	X(SHPIPE, 2)   /* s... -- s    a: strings: commands joined by | (b: 1 when the first is text for its stdin); the output */ \
	X(PID, 0)      /* -- i         the process id, bash's $$ */ \
	X(ENV, 0)      /* s -- s       an environment variable, "" when it is not set */ \
	X(INC_L, 1)    /* --           a: local slot; adds 1 to the int in it */ \
	X(VEC_NEW, 2)  /* x... -- v    a: elements on the stack, b: the vec type */ \
	X(VEC_LEN, 1)  /* v -- i       a: borrowed */ \
	X(VEC_PUSH, 2) /* v x... --    a: cells of an element; b: the vec is borrowed */ \
	X(VEC_GET, 2)  /* v i -- x...  a: cells of an element, b: borrowed; the index is checked; a copy, the vec keeps its own */ \
	X(VEC_PUT, 2)  /* v i x... --  a: cells of an element, b: borrowed; the index is checked; the element it replaces dies */ \
	X(VEC_POP, 2)  /* v -- o...    (option element): the last one, taken off; none when empty; b: borrowed */ \
	X(MAP_NEW, 2)  /* k x... -- m  a: entries on the stack, b: the map type */ \
	X(MAP_INSERT, 2) /* m k x... -- a: cells of a value; adds the entry or replaces its value; b: the map is borrowed */ \
	X(MAP_LOOKUP, 2) /* m k -- o... (option value), a copy; b: bit 0 the map is borrowed, bit 1 the key */ \
	X(MAP_HAS, 1)  /* m k -- b     a: bit 0 the map is borrowed, bit 1 the key */ \
	X(MAP_REMOVE, 2) /* m k -- o... (option value): the value it had; b: as for MAP_LOOKUP */ \
	X(MAP_LEN, 1)  /* m -- i       a: borrowed */ \
	X(MAP_KEYS, 2) /* m -- v       (vec key) in the order they went in; a: that vec type, b: the map is borrowed */ \
	X(MAP_VALUES, 2) /* m -- v     (vec value) in the same order; a: that vec type, b: the map is borrowed */ \
	X(POOL_NEW, 2) /* i -- p       a pool of i slots; b: the pool type */ \
	X(POOL_SPAWN, 2) /* p x... -- h the element goes into a free slot; a: cells of an element, b: the pool is borrowed */ \
	X(POOL_GET, 2) /* p h -- x...  a copy of the element; a stale handle is an error; a: cells, b: borrowed */ \
	X(POOL_PUT, 2) /* p h x... --  replace the element (the old one dies); a stale handle is an error */ \
	X(POOL_LOOKUP, 2) /* p h -- o... (option element), a copy; none for a stale handle; b: bit 0 borrowed */ \
	X(POOL_HAS, 1) /* p h -- b     the handle's element is still there; a: bit 0 borrowed */ \
	X(POOL_REMOVE, 2) /* p h -- o... (option element): the element moves out and its slot is free again */ \
	X(POOL_LEN, 1) /* p -- i       elements in the pool; a: borrowed */ \
	X(POOL_NEXT, 1) /* p i -- i h  the first slot from i on that holds an element, and its handle; -1 and 0 if none */ \
	X(ARENA_BEGIN, 0) /* --        an arena starts: new strings, vecs, maps and pools go into it */ \
	X(ARENA_END, 0) /* --          it ends: its cells come back at once; anything made in it and still held is an error */ \
	X(KEEP, 1)     /* x -- x       a: the type; what the value holds from the innermost arena is copied onto the heap */ \
	X(CLOSURE, 2)  /* v... -- f    a closure of fn a keeping the values (b: their type, a struct) */ \
	X(ENV_LOAD, 2) /* -- x...      a value the running fn keeps in its closure (slot 0): a: its offset, b: its cells */ \
	X(ENV_STORE, 2) /* x... --     a new value for it */ \
	X(ENV_RELEASE, 2) /* --        it dies: b is its type */ \
	X(CORO_NEW, 2) /* args -- c    a coroutine runs function a (b: the coroutine type) until it first pauses; then this goes on */ \
	X(CORO_POLL, 1) /* c -- o...   (option result): some once the coroutine has ended, none before; never waits. a: its type */ \
	X(SLEEP, 0)    /* f --         a coroutine pauses for f seconds of coroutine time; the main program sleeps, running them */ \
	X(YIELD, 0)    /* --           a coroutine pauses until the next pass of the scheduler */ \
	X(CORO_STEP, 0) /* f -- i     coroutine time goes f seconds on; every coroutine that can go on runs until it pauses again */ \
	X(CORO_RUN, 0) /* --          run the coroutines until there are none, sleeping in real time when they all sleep */ \
	X(CORO_DONE, 0) /* t -- b      the coroutine has ended */ \
	X(CORO_TIME, 0) /* -- f        the coroutine time in seconds */ \
	X(CORO_END, 0) /* r... --      where a coroutine's function returns to: the result goes into the coroutine, which ends */ \
	X(SHOW, 1)     /* x... --      the REPL's "=> value : type"; a: the type */ \
	X(FAIL, 1)     /* --           stop with a runtime error saying string a (assert) */ \
	X(GPU, 1)      /* -- b         run gpu-each kernel a on the GPU: true when it ran, false to run the loop on the VM */ \
	X(GFX, 2)      /* x... -- [v]  drawing builtin a (gfx.c) on its b arguments; a value when the builtin gives one */ \
	X(NOP, 2)      /* --           never run: the place of an instruction fused into the one before it */ \
	GAEL_FUSED(X)

/* Superinstructions: a run of simple instructions that the compiler fuses into its first one after a unit is compiled
 * (fuse in compile.c); the others stay in place as NOPs, skipped. LL: two locals, a and b; LK: local a and constant b;
 * SL: the value on top and local a; SK: the value on top and constant a (an index into the int or float constants).
 * The arithmetic ones push the result, as the run did. A compare-and-jump jumps when the comparison is the sense: the
 * next instruction (a NOP) holds the target in a and the sense in b - 1 for a JT the run ended with, 0 for a JF; the SS
 * form, a comparison of the two values on top, holds them itself. */
#define GAEL_BIN_FORMS(X, op) X(op##_LL, 2) X(op##_LK, 2) X(op##_SL, 1) X(op##_SK, 1)
#define GAEL_JMP_FORMS(X, op) X(J##op##_LL, 2) X(J##op##_LK, 2) X(J##op##_SK, 1) X(J##op##_SS, 2)
#define GAEL_FUSED(X) \
	GAEL_BIN_FORMS(X, ADD_I) GAEL_BIN_FORMS(X, SUB_I) GAEL_BIN_FORMS(X, MUL_I) \
	GAEL_BIN_FORMS(X, ADD_F) GAEL_BIN_FORMS(X, SUB_F) GAEL_BIN_FORMS(X, MUL_F) GAEL_BIN_FORMS(X, DIV_F) \
	X(MOD_I_LK, 2) X(MOD_I_SK, 1) /* the constant is neither 0 nor -1 */ \
	GAEL_JMP_FORMS(X, LT_I) GAEL_JMP_FORMS(X, LE_I) GAEL_JMP_FORMS(X, GT_I) GAEL_JMP_FORMS(X, GE_I) \
	GAEL_JMP_FORMS(X, EQ_I) GAEL_JMP_FORMS(X, NE_I) \
	GAEL_JMP_FORMS(X, LT_F) GAEL_JMP_FORMS(X, LE_F) GAEL_JMP_FORMS(X, GT_F) GAEL_JMP_FORMS(X, GE_F) \
	GAEL_JMP_FORMS(X, EQ_F) GAEL_JMP_FORMS(X, NE_F) \
	X(LOAD2_L, 2)  /* -- x y       locals a and b */ \
	X(STORE_LOAD_L, 2) /* x -- y   x into local a, then local b */ \
	X(INC_JMP, 2)  /* --           add 1 to the int in local a, jump to b: the end of a for loop */

#define GAEL_OP_ENUM_(name, n) OP_##name,
typedef enum { GAEL_OPS(GAEL_OP_ENUM_) OP_COUNT_ } Op;

#endif
