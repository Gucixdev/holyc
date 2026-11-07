/* desktop - one executable: the compositor, the bar, the menu and the small tools of the dwl desktop.
 * Multi-call, busybox style: the applet is argv[0]'s basename (a symlink or hardlink named dwl, dmenu, ...) or the first argument.
 *   desktop dwl [-s cmd]      desktop dmenu ...      desktop lemonbar ...      desktop menu      desktop stest ...
 * Every program keeps its own main renamed to <name>_main; its other symbols are made local at link time (see the Makefile),
 * so nothing clashes and nothing is slower than the separate binaries were. A table lookup, no exec, no shell. */
#include <stdio.h>
#include <string.h>
#include <libgen.h>

int dwl_main(int, char **);
int dmenu_main(int, char **);
int stest_main(int, char **);
int menu_main(int, char **);
int lemonbar_main(int, char **);
int bar_main(int, char **);

static const struct { const char *name; int (*fn)(int, char **); } applets[] = {
	{ "dwl", dwl_main }, { "dmenu", dmenu_main }, { "stest", stest_main },
	{ "menu", menu_main }, { "lemonbar", lemonbar_main }, { "bar", bar_main },
};
#define NAPPLETS (sizeof applets / sizeof *applets)

static int run(const char *name, int argc, char **argv, int *found)
{
	for (unsigned i = 0; i < NAPPLETS; i++)
		if (!strcmp(name, applets[i].name)) {
			*found = 1;
			return applets[i].fn(argc, argv);
		}
	*found = 0;
	return 0;
}

int main(int argc, char **argv)
{
	char *self = strdup(argv[0]);
	int found, rc = run(basename(self), argc, argv, &found);
	if (!found && argc > 1)
		rc = run(argv[1], argc - 1, argv + 1, &found);
	if (!found) {
		fputs("usage: desktop applet [args]   applets:", stderr);
		for (unsigned i = 0; i < NAPPLETS; i++)
			fprintf(stderr, " %s", applets[i].name);
		fputc('\n', stderr);
		return 2;
	}
	return rc;
}
