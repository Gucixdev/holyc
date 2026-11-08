/* gtkcss.c - feed a css file to the real GTK css parser (GTK3 or GTK4, selected at compile time) and fail on any parsing error. No display needed. */
#include <gtk/gtk.h>
#include <stdio.h>
static int errors;
static void on_err(GtkCssProvider *p, GtkCssSection *s, GError *e, gpointer d) { (void)p; (void)s; (void)d; fprintf(stderr, "css error: %s\n", e->message); errors++; }
int main(int argc, char **argv) {
	if (argc != 2) return 2;
	char *buf; gsize len; GError *err = NULL;
	if (!g_file_get_contents(argv[1], &buf, &len, &err)) { fprintf(stderr, "%s\n", err->message); return 2; }
	GtkCssProvider *p = gtk_css_provider_new();
	g_signal_connect(p, "parsing-error", G_CALLBACK(on_err), NULL);
#if GTK_MAJOR_VERSION >= 4
	gtk_css_provider_load_from_data(p, buf, (gssize)len);
#else
	gtk_css_provider_load_from_data(p, buf, (gssize)len, NULL);
#endif
	printf("gtk%d parsed %s: %d errors\n", GTK_MAJOR_VERSION, argv[1], errors);
	return errors ? 1 : 0;
}
