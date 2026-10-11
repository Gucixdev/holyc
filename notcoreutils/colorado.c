/* colorado - generic stdin colorizer (real scope: log-level words,
 * quoted strings, IPv4 addresses, numbers - same idea as grc/ccze,
 * not a config-file-driven rule engine, just a fixed set of common
 * patterns). Passes everything else through unchanged, so `cmd |
 * colorado` is always safe even on output it doesn't recognize. */

#define BASE_TYPE_IMPLEMENTATION
#include "base.h"
#include "patches.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define COLOR_RESET "\033[0m"
#define COLOR_NUM "\033[0;33m"
#define COLOR_STR "\033[0;32m"
#define COLOR_IP "\033[0;36m"
#define COLOR_ERROR "\033[1;31m"
#define COLOR_WARN "\033[1;33m"
#define COLOR_INFO "\033[0;34m"
#define COLOR_DEBUG "\033[2m"

static i32 colorado_is_ipv4_start(const char *p)
{
    i32 dots = 0, digits = 0;
    const char *s = p;
    while (*s) {
        if (isdigit((unsigned char)*s)) { digits++; s++; }
        else if (*s == '.' && digits > 0) { dots++; digits = 0; s++; if (dots > 3) return 0; }
        else break;
    }
    return dots == 3 && digits > 0 && (s == p || !isalnum((unsigned char)*s));
}

static i32 colorado_match_word(const char *p, const char *word)
{
    size_t len = strlen(word);
    return strncmp(p, word, len) == 0 && !isalnum((unsigned char)p[len]);
}

int main(void)
{
    /* getline keeps the length, so a NUL byte inside a line is passed through
     * instead of ending it, and a line of any length is one unit */
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    while ((n = getline(&line, &cap, stdin)) > 0) {
        const char *p = line;
        const char *end = line + n;
        while (p < end) {
            if (colorado_match_word(p, "ERROR") || colorado_match_word(p, "FATAL")) {
                const char *w = colorado_match_word(p, "ERROR") ? "ERROR" : "FATAL";
                printf(COLOR_ERROR "%s" COLOR_RESET, w);
                p += strlen(w);
            } else if (colorado_match_word(p, "WARN") || colorado_match_word(p, "WARNING")) {
                const char *w = colorado_match_word(p, "WARNING") ? "WARNING" : "WARN";
                printf(COLOR_WARN "%s" COLOR_RESET, w);
                p += strlen(w);
            } else if (colorado_match_word(p, "INFO")) {
                printf(COLOR_INFO "INFO" COLOR_RESET);
                p += 4;
            } else if (colorado_match_word(p, "DEBUG")) {
                printf(COLOR_DEBUG "DEBUG" COLOR_RESET);
                p += 5;
            } else if (colorado_is_ipv4_start(p)) {
                const char *start = p;
                while (p < end && (isdigit((unsigned char)*p) || *p == '.')) p++;
                printf(COLOR_IP "%.*s" COLOR_RESET, (int)(p - start), start);
            } else if (*p == '"') {
                const char *start = p;
                p++;
                while (p < end && *p != '"') p++;
                if (p < end && *p == '"') p++;
                printf(COLOR_STR "%.*s" COLOR_RESET, (int)(p - start), start);
            } else if (isdigit((unsigned char)*p)) {
                const char *start = p;
                while (p < end && (isdigit((unsigned char)*p) || *p == '.')) p++;
                printf(COLOR_NUM "%.*s" COLOR_RESET, (int)(p - start), start);
            } else {
                putchar(*p);
                p++;
            }
        }
    }
    free(line);
    return 0;
}
