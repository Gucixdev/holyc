#ifndef HOLYC_PY_OPS_H_
#define HOLYC_PY_OPS_H_

/* Python's dunder-method calling SYNTAX, not its semantics - C89 has no
 * operator overloading, no constructors/destructors, no real dispatch
 * table attached to a type. What IS real and portable: a NAMING
 * CONVENTION (Type_init/Type_eq/Type_str/Type_len/Type_del, each a
 * plain function a type's own author writes by hand) plus these
 * macros, which just spell out `Type##_method(...)` so every type that
 * follows the convention gets uniform, Python-flavored call-site
 * syntax - PY_INIT2(Point, &p, 1, 2) instead of point_init(&p, 1, 2).
 * These macros generate NO code and enforce NOTHING at the type-
 * definition site (a type that doesn't define e.g. Type_eq just fails
 * to compile at the PY_EQ call site with a normal "undeclared
 * function" error, not a silent no-op) - purely a readability/
 * consistency layer over a convention this project's own types can
 * choose to opt into, not a new object system.
 *
 * PY_INITn is numbered by argument count (0/1/2/3), NOT a single
 * variadic PY_INIT(...) - this project builds strict `-std=c89
 * -pedantic`, and variadic macros (__VA_ARGS__) are a C99 feature; an
 * honest C89 fixed-arity family instead of a C99 shortcut this build
 * would reject outright. 4+ own args is a real, documented gap (pass
 * a struct/params-object by pointer instead, the same way this
 * project's own multi-field constructors already tend to work) not
 * silently missing.
 *
 * PY_DEL is worth calling out specifically: this project already has a
 * real automatic-destruction mechanism (async.h's DEFER, raii.h's
 * Raii_Guard, both built on GCC's __attribute__((cleanup))) - PY_DEL
 * is NOT a replacement for either of those, just the same naming-
 * convention wrapper for a MANUAL "call this type's own cleanup
 * function right here" call site, for a type that doesn't need/want
 * scope-exit automation. */

#define PY_INIT0(Type, obj_ptr) Type##_init(obj_ptr)
#define PY_INIT1(Type, obj_ptr, a) Type##_init(obj_ptr, a)
#define PY_INIT2(Type, obj_ptr, a, b) Type##_init(obj_ptr, a, b)
#define PY_INIT3(Type, obj_ptr, a, b, c) Type##_init(obj_ptr, a, b, c)

#define PY_EQ(Type, a_ptr, b_ptr) Type##_eq(a_ptr, b_ptr)

#define PY_STR0(Type, obj_ptr) Type##_str(obj_ptr)
#define PY_STR1(Type, obj_ptr, a) Type##_str(obj_ptr, a)
#define PY_STR2(Type, obj_ptr, a, b) Type##_str(obj_ptr, a, b)

#define PY_LEN(Type, obj_ptr) Type##_len(obj_ptr)
#define PY_DEL(Type, obj_ptr) Type##_del(obj_ptr)

#endif /* HOLYC_PY_OPS_H_ */
