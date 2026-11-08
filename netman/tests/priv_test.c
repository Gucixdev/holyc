/* priv.h tests: the decision table and the sudo argv (no password on it). Nothing is run as root; the "sudo" is a recording script.
 *   cc -std=gnu11 -Wall -Wextra -O2 -I.. -o /tmp/priv_test priv_test.c && /tmp/priv_test
 */
#define _GNU_SOURCE
#include "priv.h"
#include <sys/wait.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d  %s\n      ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define N(root, mode, wpa, ok, euid) nm_priv_needed(root, mode, wpa, ok, euid)

int main(void)
{
	/* auto */
	CHECK(!N(NM_ROOT_NO, NULL, 0, 0, 1000), "NO: no root");
	CHECK(!N(NM_ROOT_NO, "auto", 1, 0, 1000), "NO stays NO whatever the backend");
	CHECK(!N(NM_ROOT_WPA, NULL, 0, 0, 1000), "iwd: group access is enough");
	CHECK(N(NM_ROOT_WPA, NULL, 1, 0, 1000) == 1, "wpa_supplicant with a root-only socket needs root");
	CHECK(!N(NM_ROOT_WPA, NULL, 1, 1, 1000), "wpa_supplicant with a socket we can open (netdev group): no root");
	CHECK(N(NM_ROOT_YES, NULL, 0, 0, 1000) == 1 && N(NM_ROOT_YES, "auto", 0, 1, 1000) == 1, "YES always needs root");
	CHECK(!N(NM_ROOT_YES, NULL, 0, 0, 0) && !N(NM_ROOT_WPA, NULL, 1, 0, 0), "already root: never");
	/* override */
	CHECK(!N(NM_ROOT_YES, "never", 1, 0, 1000), "privileges.sudo=never");
	CHECK(N(NM_ROOT_NO, "always", 0, 0, 1000) == 1 && N(NM_ROOT_WPA, "always", 0, 0, 1000) == 1, "privileges.sudo=always");
	CHECK(!N(NM_ROOT_NEVER, "always", 0, 0, 1000) && !N(NM_ROOT_NEVER, NULL, 0, 0, 1000), "NEVER (help, status, config) is never escalated");
	CHECK(!N(NM_ROOT_YES, "always", 0, 0, 0), "always + already root: no");

	/* escalate: a recording fake sudo; the child process execs it, the parent reads what it was given */
	{
		char dir[] = "/tmp/nm_priv_XXXXXX", fake[300], log[300], buf[2000] = "";
		pid_t pid;
		FILE *f;
		char *av[] = { "netman", "wifi", "connect", "My Net", "s3cret-not-here?", NULL };
		if (!mkdtemp(dir)) return 2;
		snprintf(fake, sizeof fake, "%s/sudo", dir);
		snprintf(log, sizeof log, "%s/argv", dir);
		f = fopen(fake, "w");
		fprintf(f, "#!/bin/sh\nfor a in \"$@\"; do printf '%%s\\n' \"$a\"; done > %s\nexit 0\n", log);
		fclose(f);
		chmod(fake, 0755);
		setenv("NM_SUDO_CMD", fake, 1);
		pid = fork();
		if (pid == 0) {
			int dn = open("/dev/null", O_WRONLY);
			dup2(dn, 2);
			nm_priv_escalate(4, av);
			_exit(99);
		}
		waitpid(pid, NULL, 0);
		f = fopen(log, "r");
		if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
		CHECK(strstr(buf, "-p\n[netman] password for %p: \n--\n") != NULL, "sudo gets -p PROMPT -- : '%s'", buf);
		CHECK(strstr(buf, "--priv-done\nwifi\nconnect\nMy Net\n") != NULL, "original args follow --priv-done, spaces kept: '%s'", buf);
		CHECK(strstr(buf, "/priv_test") || strstr(buf, "netman") || strchr(buf, '/'), "absolute path of ourselves is passed");
		/* a missing sudo: escalate fails with -1 and returns */
		setenv("NM_SUDO_CMD", "/nonexistent/sudo", 1);
		pid = fork();
		if (pid == 0) { int dn = open("/dev/null", O_WRONLY); dup2(dn, 2); _exit(nm_priv_escalate(4, av) == -1 ? 0 : 1); }
		{ int st; waitpid(pid, &st, 0); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "missing sudo -> returns -1, no crash"); }
		unlink(fake); unlink(log); rmdir(dir);
	}
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
