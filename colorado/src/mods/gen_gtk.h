/* gen_gtk.h - GTK2 (gtkrc), GTK3 and GTK4 (settings.ini + gtk.css overriding named colours, incl. libadwaita). stb-style single header.
 *
 * GTK3/GTK4 files: ~/.config/gtk-3.0/{settings.ini,gtk.css}, ~/.config/gtk-4.0/{settings.ini,gtk.css}. settings.ini is merged key by key
 * ([Settings]); gtk.css gets a marker block so the user's own css survives. Named colours: the Adwaita/GTK3 set (theme_bg_color ...) and
 * the libadwaita set (accent_bg_color, window_bg_color, headerbar_*, card_*, ...), plus the --accent-* css variables the installed
 * example on this machine uses. GTK2: ~/.gtkrc-2.0 block with gtk-color-scheme (honoured only by themes that read it).
 */
#ifndef COLORADO_GEN_GTK_H
#define COLORADO_GEN_GTK_H
#include "gen.h"

static void co_gtk_settings(const co_theme *t, co_file *f) {
	co_sb_f(&f->body, "gtk-application-prefer-dark-theme=%d\n", t->dark);
	co_sb_f(&f->body, "gtk-icon-theme-name=%s\n", t->icon_theme);
	co_sb_f(&f->body, "gtk-cursor-theme-name=%s\n", t->cursor_theme);
	co_sb_f(&f->body, "gtk-cursor-theme-size=%d\n", t->cursor_size);
	co_sb_f(&f->body, "gtk-font-name=%s %d\n", t->font, t->font_size);
	if (t->gtk_theme[0]) co_sb_f(&f->body, "gtk-theme-name=%s\n", t->gtk_theme);
}

static void co_gtk_css(const co_theme *t, co_file *f, int adw) {
	co_sb *o = &f->body; const co_rgb *c = t->c;
#define DC(n, v) co_sb_f(o, "@define-color %s %s;\n", n, co_h(v))
	DC("theme_bg_color", c[CR_BG]); DC("theme_fg_color", c[CR_FG]); DC("theme_base_color", c[CR_BG_ALT]); DC("theme_text_color", c[CR_FG]);
	DC("theme_selected_bg_color", c[CR_SEL_BG]); DC("theme_selected_fg_color", c[CR_SEL_FG]);
	DC("theme_unfocused_bg_color", c[CR_BG]); DC("theme_unfocused_fg_color", c[CR_FG_DIM]);
	DC("theme_unfocused_base_color", c[CR_BG_ALT]); DC("theme_unfocused_text_color", c[CR_FG_DIM]);
	DC("insensitive_bg_color", c[CR_BG]); DC("insensitive_fg_color", c[CR_FG_DIM]);
	DC("borders", c[CR_BORDER]); DC("unfocused_borders", c[CR_BORDER]);
	DC("warning_color", c[CR_WARN]); DC("error_color", c[CR_URGENT]); DC("success_color", c[CR_OK]);
	DC("link_color", c[CR_LINK]); DC("visited_link_color", co_mix(c[CR_LINK], c[CR_FG_DIM], 0.5));
	DC("tooltip_bg_color", c[CR_BG_ALT]); DC("tooltip_fg_color", c[CR_FG]);
	if (adw) {
		DC("accent_color", c[CR_ACCENT]); DC("accent_bg_color", c[CR_ACCENT]); DC("accent_fg_color", c[CR_ACCENT_FG]);
		DC("destructive_color", c[CR_URGENT]); DC("destructive_bg_color", c[CR_URGENT]); DC("destructive_fg_color", c[CR_BG]);
		DC("success_bg_color", c[CR_OK]); DC("success_fg_color", c[CR_BG]);
		DC("warning_bg_color", c[CR_WARN]); DC("warning_fg_color", c[CR_BG]);
		DC("error_bg_color", c[CR_URGENT]); DC("error_fg_color", c[CR_BG]);
		DC("window_bg_color", c[CR_BG]); DC("window_fg_color", c[CR_FG]);
		DC("view_bg_color", c[CR_BG_ALT]); DC("view_fg_color", c[CR_FG]);
		DC("headerbar_bg_color", c[CR_BG_ALT]); DC("headerbar_fg_color", c[CR_FG]);
		DC("headerbar_border_color", c[CR_BORDER]); DC("headerbar_backdrop_color", c[CR_BG]);
		DC("card_bg_color", c[CR_BG_ALT]); DC("card_fg_color", c[CR_FG]);
		DC("dialog_bg_color", c[CR_BG_ALT]); DC("dialog_fg_color", c[CR_FG]);
		DC("popover_bg_color", c[CR_BG_ALT]); DC("popover_fg_color", c[CR_FG]);
		DC("sidebar_bg_color", c[CR_BG_ALT]); DC("sidebar_fg_color", c[CR_FG]); DC("sidebar_backdrop_color", c[CR_BG]);
		co_sb_f(o, ":root {\n    --accent-color: %s;\n    --accent-bg-color: %s;\n    --accent-fg-color: %s;\n    --window-bg-color: %s;\n    --window-fg-color: %s;\n    --view-bg-color: %s;\n    --view-fg-color: %s;\n}\n",
			co_h(c[CR_ACCENT]), co_h(c[CR_ACCENT]), co_h(c[CR_ACCENT_FG]), co_h(c[CR_BG]), co_h(c[CR_FG]), co_h(c[CR_BG_ALT]), co_h(c[CR_FG]));
	}
#undef DC
	co_sb_f(o, "button, entry, .card { border-radius: %dpx; }\n", t->radius);
}

static void co_gen_gtk2(const co_theme *t, co_set *s) {
	co_file *f = co_set_block(s, "gtk2", W_HOME, ".gtkrc-2.0", "#", "");
	const co_rgb *c = t->c;
	co_sb_f(&f->body, "gtk-color-scheme = \"bg_color:%s\\nfg_color:%s\\nbase_color:%s\\ntext_color:%s\\nselected_bg_color:%s\\nselected_fg_color:%s\\ntooltip_bg_color:%s\\ntooltip_fg_color:%s\"\n",
		co_h(c[CR_BG]), co_h(c[CR_FG]), co_h(c[CR_BG_ALT]), co_h(c[CR_FG]), co_h(c[CR_SEL_BG]), co_h(c[CR_SEL_FG]), co_h(c[CR_BG_ALT]), co_h(c[CR_FG]));
	co_sb_f(&f->body, "gtk-icon-theme-name = \"%s\"\ngtk-cursor-theme-name = \"%s\"\ngtk-cursor-theme-size = %d\ngtk-font-name = \"%s %d\"\n", t->icon_theme, t->cursor_theme, t->cursor_size, t->font, t->font_size);
	co_sb_add(&s->notes, "gtk2: gtk-color-scheme is only read by GTK2 themes that use it (Clearlooks/Murrine-based, Adwaita-gtk2); no GTK2 install here to check against.\n");
}

static void co_gen_gtk3(const co_theme *t, co_set *s) {
	co_gtk_settings(t, co_set_ini(s, "gtk3", W_CONFIG, "gtk-3.0/settings.ini", "Settings"));
	co_gtk_css(t, co_set_block(s, "gtk3", W_CONFIG, "gtk-3.0/gtk.css", "/*", " */"), 0);
	co_sb_add(&s->notes, "gtk3: running GTK3 apps pick settings.ini up on restart; gtk.css named colours only affect themes built on those names (Adwaita, Breeze-GTK variants use suffixed names).\n");
}

static void co_gen_gtk4(const co_theme *t, co_set *s) {
	co_gtk_settings(t, co_set_ini(s, "gtk4", W_CONFIG, "gtk-4.0/settings.ini", "Settings"));
	co_gtk_css(t, co_set_block(s, "gtk4", W_CONFIG, "gtk-4.0/gtk.css", "/*", " */"), 1);
	co_sb_add(&s->notes, "gtk4: libadwaita apps ignore gtk-theme-name and read ~/.config/gtk-4.0/gtk.css; the @define-color names are the libadwaita documented ones.\n");
}
#endif
