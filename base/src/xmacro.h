#ifndef HOLYC_XMACRO_H_
#define HOLYC_XMACRO_H_

/* X-Macro pattern helpers - the classic C technique for generating two
 * or more related artifacts (an enum, a name-lookup table, a switch
 * dispatcher, a struct's field list) from exactly ONE canonical list,
 * instead of hand-maintaining each one separately and hoping they never
 * drift out of sync (the classic bug: someone adds an enum value but
 * forgets to add the matching string-table entry). this is the standard X-macro technique.
 *
 * Usage - define your own list ONCE:
 *   #define COLOR_LIST(X) \
 *       X(COLOR_RED)   \
 *       X(COLOR_GREEN) \
 *       X(COLOR_BLUE)
 *
 * ...then generate an enum and its matching name strings from it:
 *   typedef enum { XM_ENUM(COLOR_LIST) COLOR_COUNT_UNUSED_ } Color;
 *   static const char *color_names[] = { XM_STRINGS(COLOR_LIST) };
 *
 * fabric.h builds on this same LIST_MACRO shape for factory/dispatch
 * generation - see its own header for that use case. lut.h is a
 * different, non-X-macro pattern (runtime-built lookup tables), kept
 * separate since it solves a genuinely different problem (data, not
 * code generation). */

#define XM_ENUM_EXPAND_(name) name,
#define XM_STRING_EXPAND_(name) #name,
#define XM_COUNT_EXPAND_(name) +1

/* expands to "name0, name1, name2," - wrap in your own
 * `typedef enum { ... } MyEnum;` */
#define XM_ENUM(LIST_MACRO) LIST_MACRO(XM_ENUM_EXPAND_)

/* expands to '"name0", "name1", "name2",' - wrap in your own
 * `static const char *my_names[] = { ... };` (index with the enum
 * XM_ENUM generated - they're built from the same list, so they can
 * never disagree on ordering or count). */
#define XM_STRINGS(LIST_MACRO) LIST_MACRO(XM_STRING_EXPAND_)

/* expands to "0 +1 +1 +1" - a real count computed from the list itself
 * at compile time, not a hand-maintained "_COUNT" sentinel enum value
 * that silently goes stale the moment someone adds an entry above it
 * without noticing the sentinel below. Use as an array size or a loop
 * bound: `for (i = 0; i < XM_COUNT(COLOR_LIST); i++)`. */
#define XM_COUNT(LIST_MACRO) (0 LIST_MACRO(XM_COUNT_EXPAND_))

#endif /* HOLYC_XMACRO_H_ */
