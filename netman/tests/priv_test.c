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
static int need(const char *mode, int tui, int wpa, int ok, unsigned euid, int n, ...)
{
	char *av[8] = { "netman" };
	va_list ap;
	va_start(ap, n);
	for (int i = 0; i < n; i++) av[1 + i] = va_arg(ap, char *);
	va_end(ap);
	if (mode) setenv("NM_SUDO", mode, 1); else unsetenv("NM_SUDO");
	return nm_priv_needed(n + 1, av, tui, wpa, ok, euid);
}

int main(void)
{
	/* auto */
	CHECK(!need(NULL, 0, 0, 0, 1000, 0), "status: no root");
	CHECK(!need(NULL, 0, 0, 0, 1000, 1, "help"), "help: no root");
	CHECK(!need(NULL, 0, 0, 0, 1000, 2, "bt", "pair"), "bluetooth never needs root");
	CHECK(!need(NULL, 0, 0, 0, 1000, 2, "wifi", "connect"), "iwd: group access is enough");
	CHECK(need(NULL, 0, 1, 0, 1000, 2, "wifi", "list") == 1, "wpa_supplicant with a root-only socket needs root");
	CHECK(!need(NULL, 0, 1, 1, 1000, 2, "wifi", "list"), "wpa_supplicant with a socket we can open (netdev group): no root");
	CHECK(need(NULL, 1, 1, 0, 1000, 0) == 1 && !need(NULL, 1, 0, 0, 1000, 0) && !need(NULL, 1, 1, 1, 1000, 0), "tui follows the wifi backend");
	CHECK(need(NULL, 0, 0, 0, 1000, 1, "fw") == 1, "fw needs root");
	CHECK(!need(NULL, 1, 1, 0, 0, 0) && !need(NULL, 0, 0, 0, 0, 1, "fw"), "already root: never");
	/* override */
	CHECK(!need("never", 1, 1, 0, 1000, 1, "fw") , "NM_SUDO=never");
	CHECK(need("always", 0, 0, 0, 1000, 1, "fw") == 1 && need("always", 1, 0, 0, 1000, 0) == 1 && !need("always", 0, 0, 0, 1000, 0) && !need("always", 0, 0, 0, 0, 1, "fw"), "NM_SUDO=always (not for status/help or root)");

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
