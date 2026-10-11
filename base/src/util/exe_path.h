#ifndef HOLYC_EXE_PATH_H_
#define HOLYC_EXE_PATH_H_

/* Ported from whereami (Gregory Pakosz, dual WTFPL-2/MIT) - Linux and
 * FreeBSD only (Linux and FreeBSD). The original supports Windows, macOS,
 * Solaris and Android too, each with its own platform API
 * (GetModuleFileNameW, _NSGetExecutablePath, /proc/self/maps parsing
 * for the "module" as opposed to "executable" path...) - not ported,
 * not a target. The "module path" variant (path of the .so containing
 * the CALLING code, found by walking /proc/self/maps for the return
 * address) was dropped entirely too - this project doesn't build
 * shared libraries, there is no module distinct from the executable
 * to ask about.
 *
 * Linux: readlink() on /proc/self/exe rather than the original's
 * realpath() - the kernel already hands back a fully resolved absolute
 * path through that symlink, so realpath()'s extra canonicalization
 * buys nothing here, and readlink is plain POSIX.1-2001 (covered by this
 * project's existing _POSIX_C_SOURCE=200112L) where realpath needs
 * glibc's __USE_XOPEN_EXTENDED (_XOPEN_SOURCE >= 500) - not worth
 * widening the project's feature-test macros for.
 *
 * FreeBSD: sysctl(CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1) - the
 * standard FreeBSD way to ask the kernel directly for the running
 * process's own executable path, and deliberately NOT a /proc/self/exe
 * readlink like the Linux branch - FreeBSD's procfs (linprocfs) isn't
 * mounted by default, sysctl always works with zero mount dependency.
 * Unverified on real FreeBSD as of writing (no FreeBSD build/test
 * environment yet) - matches the
 * documented, stable FreeBSD sysctl ABI, but flagging the gap honestly
 * rather than claiming tested. */

#include "types/base.h"

/* own local memcpy_ - own copy per this project's standing "no cross-
 * IMPLEMENTATION-guard dependency" rule, avoiding a <string.h>
 * dependency for one small byte loop. */
static void *exe_path_memcpy_(void *dst, const void *src, size_t n)
{
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    size_t i;
    for (i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

/* two-call convention, same as the original: call with out=NULL (capacity
 * ignored) to get the required length back; call again with a buffer of
 * at least that many bytes to fill it. dirname_length, if non-NULL, is
 * set to the length of the directory portion (index of the last '/') -
 * only on the second call, once `out` is actually filled. Returns the
 * path length (excluding the NUL you still need to add yourself) on
 * success, -1 on failure. */
i32 exe_path_get(char *out, i32 capacity, i32 *dirname_length);

#endif /* HOLYC_EXE_PATH_H_ */

#ifdef EXE_PATH_IMPLEMENTATION
#ifndef EXE_PATH_IMPLEMENTATION_
#define EXE_PATH_IMPLEMENTATION_

#include <limits.h>
#include <unistd.h>

#if defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

i32 exe_path_get(char *out, i32 capacity, i32 *dirname_length)
{
    char buffer[PATH_MAX];
    i32 length;

#if defined(__FreeBSD__)
    {
        i32 mib[4];
        size_t bufsize = sizeof(buffer);
        mib[0] = CTL_KERN;
        mib[1] = KERN_PROC;
        mib[2] = KERN_PROC_PATHNAME;
        mib[3] = -1; /* -1 = calling process, per sysctl(3) */
        if (sysctl(mib, 4, buffer, &bufsize, NULL, 0) != 0 || bufsize == 0) return -1;
        length = (i32)bufsize - 1; /* sysctl includes the trailing NUL in bufsize, exe_path_get's own length convention excludes it */
    }
#elif defined(__linux__)
    {
        i64 n = readlink("/proc/self/exe", buffer, sizeof(buffer));
        if (n <= 0) return -1;
        length = (i32)n;
    }
#else
#error "exe_path_get: only Linux and FreeBSD are supported (the two supported targets)"
#endif

    if (out && length <= capacity) {
        i32 i;
        exe_path_memcpy_(out, buffer, (size_t)length);
        /* reverse scan for the last '/' - a genuine early-exit search,
         * not a value select, so it stays a loop */
        if (dirname_length) {
            for (i = length - 1; i >= 0; i--) {
                if (out[i] == '/') { *dirname_length = i; break; }
            }
        }
    }

    return length;
}

#endif /* EXE_PATH_IMPLEMENTATION_ */
#endif /* EXE_PATH_IMPLEMENTATION */
