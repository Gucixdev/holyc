/* See LICENSE file for copyright and license details. */
/* Default settings; can be overriden by command line. */

static int topbar = 1;                      /* -b  option; if 0, dmenu appears at bottom     */
/* -fn option overrides fonts[0]; default X11 font or font set */
static const char *fonts[] = {
	"TempleOS Bitmap:pixelsize=16",   /* bitmap .bdf, 8x8 scaled x2 - the theme's main font */
	"Fixed6x13:pixelsize=13",         /* bitmap .bdf, fills in what TempleOS lacks (Polish letters, ...) */
	"TempleOS:size=14"                /* the .ttf, last resort */
};
static const char *prompt      = NULL;      /* -p  option; prompt to the left of input field */
static const char *colors[SchemeLast][2] = {
	/*     fg         bg       */
	[SchemeNorm] = { "#00ff88", "#000000" },   /* hacker green on black */
	[SchemeSel]  = { "#000000", "#00ff88" },
	[SchemeOut]  = { "#000000", "#00cc6a" },
	/* rofi look: window bg is translucent (#rrggbbaa), the input bar and the accent are opaque */
	[SchemeInput]  = { "#00ff88", "#000000" },   /* text in the input bar */
	[SchemePrompt] = { "#00ff88", "#000000" },   /* prompt, and the accent colour of the border */
	[SchemeHint]   = { "#666666", "#000000" },   /* placeholder text */
};
/*
 * The rofi look: a centred box (the same numbers as ~/.config/rofi/config.rasi), a translucent window with an
 * accent border, an input bar and a vertical list. -T on the command line gives the classic top bar instead.
 */
static int rofimode = 1;
static const int rofi_width    = 560;   /* window width */
static const int rofi_border   = 2;     /* accent border */
static const int rofi_pad      = 14;    /* window padding (inside the border) */
static const int rofi_gap      = 10;    /* between the input bar and the list */
static const int rofi_in_padx  = 12, rofi_in_pady = 10, rofi_in_gap = 8;   /* input bar padding, prompt-entry gap */
static const int rofi_el_padx  = 10, rofi_el_pady = 8, rofi_el_gap = 2;    /* list row padding, gap between rows */
static const unsigned int rofi_lines = 8;
static const char *rofi_bg       = "#000000e6";   /* window background (90% black) */
static const char *rofi_placeholder = "Szukaj...";

/* -l option; if nonzero, dmenu uses vertical list with given number of lines */
static unsigned int lines      = 0;

/*
 * Characters not considered part of a word while deleting words
 * for example: " /?\"&[]"
 */
static const char worddelimiters[] = " ";
