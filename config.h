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
};
/* -l option; if nonzero, dmenu uses vertical list with given number of lines */
static unsigned int lines      = 0;

/*
 * Characters not considered part of a word while deleting words
 * for example: " /?\"&[]"
 */
static const char worddelimiters[] = " ";
