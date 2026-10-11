/* sudo - minimal, doas-style privilege escalation: `sudo CMD [ARGS...]`
 * prompts for YOUR OWN password (not root's - same real model real
 * sudo defaults to), and if you're a member of the "wheel" group,
 * execs CMD as root. That's the whole tool.
 *
 * Deliberately doas-sized, not sudo-sized: no /etc/sudoers file/
 * grammar, no per-command rule matching, no session/timestamp
 * caching (every invocation re-prompts - a real, deliberate choice,
 * not a missing feature: no cached-auth window for another process
 * on the same tty to ride), no NOPASSWD, no logging pipeline. One
 * group check, one password check, one exec - the real security
 * model (you must know a real authorized user's password to run as
 * root) with none of real sudo's configuration surface.
 *
 * Must be installed setuid-root (chown root:root; chmod 4755) to
 * work at all - that's what lets a normal user's invocation reach
 * getspnam() (shadow passwords are root-readable only) and the final
 * setuid(0). Refuses cleanly, not silently, if it isn't.
 *
 * usage: sudo CMD [ARGS...]
 */

/* getpass() needs _DEFAULT_SOURCE - no longer defined locally here,
 * the global build CFLAGS already provide it project-wide (a
 * redundant local #define conflicts with that command-line -D under
 * -Werror - see power.c's own comment on the exact same bug, found
 * building that file first). */
#define BASE_TYPE_IMPLEMENTATION
#include "base.h"
#include "patches.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>

#define SUDO_WHEEL_GROUP "wheel"

/* true if `pw` is a member of "wheel" - either as their primary gid,
 * or listed in wheel's own supplementary-member list. Both checked
 * since real systems put admin users in "wheel" either way depending
 * on how the account was created (useradd -G wheel vs -g wheel). */
static i32 sudo_in_wheel(const struct passwd *pw)
{
    struct group *wheel = getgrnam(SUDO_WHEEL_GROUP);
    char **m;

    if (!wheel) return 0;
    if (pw->pw_gid == wheel->gr_gid) return 1;
    for (m = wheel->gr_mem; *m; m++) {
        if (strcmp(*m, pw->pw_name) == 0) return 1;
    }
    return 0;
}

/* verifies `password` against pw's own real shadow entry - crypt(3)
 * against the stored hash, same real mechanism this project's own
 * ubase login.c/passwd.c already use, not a hand-rolled check. */
static i32 sudo_check_password(const struct passwd *pw, const char *password)
{
    struct spwd *sp = getspnam(pw->pw_name);
    char *hashed;

    if (!sp || !sp->sp_pwdp || sp->sp_pwdp[0] == '\0') return 0; /* no shadow entry, or a locked/empty-password account - refuse, never treat as "no password needed" */
    hashed = crypt(password, sp->sp_pwdp);
    if (!hashed) return 0;
    return strcmp(hashed, sp->sp_pwdp) == 0;
}

int main(int argc, char **argv)
{
    uid_t ruid = getuid();
    struct passwd *pw;
    char *password;

    if (argc < 2) {
        fprintf(stderr, "sudo: usage: sudo CMD [ARGS...]\n");
        return 1;
    }

    if (geteuid() != 0) {
        fprintf(stderr, "sudo: not installed setuid-root - refusing (chown root:root; chmod 4755 this binary)\n");
        return 1;
    }

    pw = getpwuid(ruid);
    if (!pw) {
        fprintf(stderr, "sudo: no passwd entry for uid %d\n", (i32)ruid);
        return 1;
    }

    if (!sudo_in_wheel(pw)) {
        fprintf(stderr, "sudo: %s is not in the %s group\n", pw->pw_name, SUDO_WHEEL_GROUP);
        return 1;
    }

    password = getpass("Password: ");
    if (!password) {
        fprintf(stderr, "sudo: getpass failed\n");
        return 1;
    }
    if (!sudo_check_password(pw, password)) {
        memset(password, 0, strlen(password)); /* real hygiene: don't leave the typed password sitting in memory (getpass's own static buffer) past the point it's needed */
        fprintf(stderr, "sudo: incorrect password\n");
        return 1;
    }
    memset(password, 0, strlen(password));

    if (setgid(0) != 0 || setuid(0) != 0) {
        fprintf(stderr, "sudo: setuid/setgid failed\n");
        return 1;
    }

    execvp(argv[1], argv + 1);
    fprintf(stderr, "sudo: cannot exec %s\n", argv[1]);
    return 127;
}
