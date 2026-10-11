/* patches.h - this tool's own patch registry, empty/off by default.
 * Real suckless patches are literal .diff files applied before
 * compiling; this project's own convention (per explicit user
 * direction) reinterprets that as real, optional C instead - same
 * idea (opt-in extra behavior, toggled in one place, off by default),
 * no diff/patch tooling required to apply or remove one.
 *
 * A patch is either:
 *   - a single file: patches/NAME.h - the common case, most patches
 *     are one focused change (see uname/patches/verbose_os.h or
 *     ls/patches/colorize.h for real, shipped examples).
 *   - a whole subfolder: patches/NAME/ containing several .h files -
 *     for a patch substantial enough to need its own internal
 *     structure (several real suckless patches - st's scrollback,
 *     dwl's layouts - are exactly this shape upstream: multiple
 *     source-file changes bundled as one named patch).
 * Either way, enabling one is two lines here:
 *   #define TOOL_PATCH_NAME 1
 *   #include "patches/NAME.h"   (or "patches/NAME/NAME.h" for a folder)
 * and a matching `#if TOOL_PATCH_NAME` guard at the real call site in
 * main.c (undefined == 0 in a preprocessor #if, so leaving this file
 * untouched keeps every patch off without main.c needing to know
 * which ones exist). */
