#ifndef HOLYC_TOKEN_TABLE_H_
#define HOLYC_TOKEN_TABLE_H_

/* String interner / symbol table: hand a string in, get back a small
 * dense integer id - the same string interned twice always returns the
 * same id, so callers can compare tokens/identifiers by a single i32
 * compare instead of repeated strcmp, and can use the id directly as a
 * dense array index (a lexer/compiler's own symbol table, or anything
 * else that repeatedly sees the same small set of distinct strings -
 * keywords, identifiers, tags). Built on hash/hashmap.h for the
 * string->id direction rather than reimplementing open addressing here;
 * this file's own job is just the id<->string bookkeeping on top. */

#include "core/arena_alloc.h"
#include "data/hash/hashmap.h"

typedef struct {
    Hash_Map map;           /* interned string bytes -> boxed i32 id (arena-owned) */
    const char **strings;    /* id -> owned, NUL-terminated copy of the string - arena array, grows like arena_da_append */
    i32 count, capacity;
} Token_Table;

void token_table_init(Arena *a, Token_Table *t, size_t capacity_hint);

/* returns the existing id if `s` (len bytes, need not be NUL-terminated
 * itself) was already interned, otherwise copies it into the arena and
 * assigns the next dense id. Ids are assigned in first-seen order
 * starting at 0. */
i32 token_table_intern(Arena *a, Token_Table *t, const char *s, size_t len);

/* NULL for an id outside [0, count) - never happens for an id this
 * same table itself returned from token_table_intern, only relevant if
 * a caller hands back a garbage/foreign id. */
const char *token_table_str(const Token_Table *t, i32 id);

#endif /* HOLYC_TOKEN_TABLE_H_ */

#ifdef TOKEN_TABLE_IMPLEMENTATION
#ifndef TOKEN_TABLE_IMPLEMENTATION_
#define TOKEN_TABLE_IMPLEMENTATION_

void token_table_init(Arena *a, Token_Table *t, size_t capacity_hint)
{
    hash_map_init(a, &t->map, capacity_hint);
    t->strings = NULL;
    t->count = 0;
    t->capacity = 0;
}

i32 token_table_intern(Arena *a, Token_Table *t, const char *s, size_t len)
{
    i32 *existing = (i32 *)hash_map_get(&t->map, s, len);
    char *owned;
    i32 *boxed_id;
    size_t i;

    if (existing) return *existing;

    if (t->count >= t->capacity) {
        i32 new_capacity = t->capacity == 0 ? 16 : t->capacity * 2;
        const char **grown = (const char **)arena_alloc(a, (size_t)new_capacity * sizeof(const char *));
        for (i = 0; i < (size_t)t->count; i++) grown[i] = t->strings[i];
        t->strings = grown;
        t->capacity = new_capacity;
    }

    owned = (char *)arena_alloc(a, len + 1);
    for (i = 0; i < len; i++) owned[i] = s[i];
    owned[len] = '\0';

    boxed_id = (i32 *)arena_alloc(a, sizeof(i32));
    *boxed_id = t->count;

    hash_map_put(a, &t->map, owned, len, boxed_id);
    t->strings[t->count] = owned;
    return t->count++;
}

const char *token_table_str(const Token_Table *t, i32 id)
{
    if (id < 0 || id >= t->count) return NULL;
    return t->strings[id];
}

#endif /* TOKEN_TABLE_IMPLEMENTATION_ */
#endif /* TOKEN_TABLE_IMPLEMENTATION */
