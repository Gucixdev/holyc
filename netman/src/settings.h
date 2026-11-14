/* settings.h - the SETTINGS SCHEMA of netman: what every key of config.h's NC_KEYS table is for a settings UI or a machine client.
 *
 * NC_KEYS (config.h) stays the one list of keys: section, name, type, default, documentation. settings.c adds only what that table cannot say:
 * the tab a section is shown under, whether acting on it needs root, whether a change needs a restart of the thing that reads it, and which
 * keys are hidden while their module is switched off. Everything else (kind, range, choices, one-line description, source) is DERIVED, so
 * nothing is typed twice; tests/settings_test.c fails when a key has no schema entry.
 *
 * The same data feeds: `netman settings ...` (CLI), `netman settings --dump-api` (JSON for a launcher or a panel), `netman settings schema`
 * (the pipe format of desktop's settings.schema: key|type|extra|apply|default|label), the TUI panel (key S), `netman help settings` and the
 * man page. Standalone: needs only config.h/config.c. Included by netman.c; tests link settings.c next to config.c.
 */
#ifndef NETMAN_SETTINGS_H
#define NETMAN_SETTINGS_H
#include "config.h"
#include <stdio.h>

enum { NSET_BOOL, NSET_INT, NSET_ENUM, NSET_STRING, NSET_PATH, NSET_CMD };

/* ---- schema (pure functions of the key index) ---------------------------------------------------------------------- */
int nset_kind(int k);                                   /* NSET_*; url and list keys are NSET_STRING (see nset_format) */
const char *nset_kind_name(int k);                      /* "bool" "int" "enum" "string" "path" "cmd" */
const char *nset_format(int k);                         /* "url", "list" or "" */
int nset_range(int k, long *lo, long *hi);              /* int keys: 1 and the bounds */
int nset_choices(int k, char out[][24], int max);       /* enum keys (bool: yes, no): how many */
const char *nset_tab(int k);                            /* tab title: "Wi-Fi", "VPN", ... */
int nset_ntabs(void);
int nset_nsecs(void);                                    /* the section table (for the consistency test) */
const char *nset_secname(int i);
const char *nset_tabname(int i);
int nset_tab_index(int k);
int nset_needs_root(int k);                             /* acting on the setting needs root (the file edit never does) */
int nset_restart(int k);                                /* a change needs a restart of the daemon/tunnel/etc that read it at start */
const char *nset_apply(int k);                          /* "live" or "restart" (desktop's apply column) */
int nset_is_gate(int k);                                /* the section's `enabled` switch */
int nset_gate_key(int k);                               /* index of <section>.enabled for k, or -1 when the section has none */
int nset_hidden(const netman_config *c, int k);         /* the module of k is switched off (and k is not the switch itself) */
void nset_short(int k, char *out, size_t n);            /* first sentence of the documentation, <= 90 bytes */
int nset_find(const char *key);                         /* "sec.name" -> index, or -1 */
int nset_is_default(const netman_config *c, int k);
/* effective source: "default", "system" (/etc), "user" (a user file), "env" (environment variable); detail gets the origin text */
const char *nset_source(const netman_config *c, int k);

/* ---- operations (what the CLI rows and the TUI call) --------------------------------------------------------------- */
/* all return 0 ok, 1 failure with the message in err (callers print it), 2 usage */
int nset_list(const netman_config *c, FILE *o, const char *section, int all);
int nset_get(const netman_config *c, const char *key, FILE *o, char *err, size_t en);
int nset_set(const char *path, const char *key, const char *value, FILE *o, char *err, size_t en);
int nset_toggle(const char *path, const char *key, FILE *o, char *err, size_t en);
int nset_reset(const char *path, const char *key, FILE *o, char *err, size_t en);
int nset_check(FILE *o, int json);                      /* validate files + env; 0 ok, 1 broken */
void nset_dump_api(const netman_config *c, const char *path, FILE *o);
void nset_schema(FILE *o);                              /* desktop's settings.schema format */
void nset_menu(const netman_config *c, FILE *o, const char *section, int all);   /* one dmenu line per setting */
/* read the chosen menu line from IN, then ask for the value (through $NETMAN_MENU, default `dmenu`) and set it. choices_only: print the
 * choices instead (one per line); value: apply this without asking. */
int nset_pick(const netman_config *c, const char *path, FILE *in, FILE *o, int choices_only, const char *value, char *err, size_t en);
#endif
