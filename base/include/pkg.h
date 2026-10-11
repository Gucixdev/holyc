#ifndef HOLYC_PKG_H_
#define HOLYC_PKG_H_

/* Application package/bundle manifest: name, version, entry point, and
 * a dependency list (name + version-constraint string pairs, e.g.
 * "sdl2" + ">=2.0"). Deliberately just a plain data structure with
 * arena-backed storage + a fixed-capacity dependency array, not a real
 * package MANAGER (no resolution, no fetching, no version-constraint
 * PARSING/comparison logic at all - the constraint is stored as an
 * opaque string, interpreting it is a real, separate, much bigger
 * undertaking left to whatever actually needs it). This is the
 * manifest shape a future build/package tool would read and write,
 * not that tool itself. */

#include "arena_alloc.h"

#ifndef PKG_MAX_DEPS
#define PKG_MAX_DEPS 32
#endif

typedef struct {
    const char *name;        /* owned, NUL-terminated */
    const char *version_constraint; /* owned, NUL-terminated, opaque - e.g. ">=2.0", "1.4.2", "*" */
} Pkg_Dependency;

typedef struct {
    const char *name;
    const char *version;
    const char *entry_point; /* e.g. a path or a function/symbol name, caller-defined meaning */
    Pkg_Dependency deps[PKG_MAX_DEPS];
    i32 dep_count;
} Pkg;

void pkg_init(Arena *a, Pkg *p, const char *name, const char *version, const char *entry_point);

/* returns 1 on success, 0 if already at PKG_MAX_DEPS - a real,
 * checkable failure, matching signal.h's own fixed-capacity stance. */
i32 pkg_add_dependency(Arena *a, Pkg *p, const char *name, const char *version_constraint);

/* NULL if no dependency with that exact name was added. */
const Pkg_Dependency *pkg_find_dependency(const Pkg *p, const char *name);

#endif /* HOLYC_PKG_H_ */

#ifdef PKG_IMPLEMENTATION
#ifndef PKG_IMPLEMENTATION_
#define PKG_IMPLEMENTATION_

static const char *pkg_strdup_(Arena *a, const char *s)
{
    size_t len = 0;
    char *out;
    while (s[len]) len++;
    out = (char *)arena_alloc(a, len + 1);
    {
        size_t i;
        for (i = 0; i < len; i++) out[i] = s[i];
    }
    out[len] = '\0';
    return out;
}

static i32 pkg_streq_(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

void pkg_init(Arena *a, Pkg *p, const char *name, const char *version, const char *entry_point)
{
    p->name = pkg_strdup_(a, name);
    p->version = pkg_strdup_(a, version);
    p->entry_point = pkg_strdup_(a, entry_point);
    p->dep_count = 0;
}

i32 pkg_add_dependency(Arena *a, Pkg *p, const char *name, const char *version_constraint)
{
    if (p->dep_count >= PKG_MAX_DEPS) return 0;
    p->deps[p->dep_count].name = pkg_strdup_(a, name);
    p->deps[p->dep_count].version_constraint = pkg_strdup_(a, version_constraint);
    p->dep_count++;
    return 1;
}

const Pkg_Dependency *pkg_find_dependency(const Pkg *p, const char *name)
{
    i32 i;
    for (i = 0; i < p->dep_count; i++) {
        if (pkg_streq_(p->deps[i].name, name)) return &p->deps[i];
    }
    return NULL;
}

#endif /* PKG_IMPLEMENTATION_ */
#endif /* PKG_IMPLEMENTATION */
