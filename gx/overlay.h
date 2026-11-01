/*
 * overlay - the panel + opens over the browser: full screen, the way the
 * pad is used, the keyboard, quitting. See overlay.c.
 */
#ifndef GX_OVERLAY_H
#define GX_OVERLAY_H

#include <stdint.h>

/* What the user chose. */
enum {
	OVERLAY_NONE,
	OVERLAY_CLOSE,         /* the panel goes away, nothing else */
	OVERLAY_FULL_SCREEN,   /* full screen on or off */
	OVERLAY_KEYBOARD,      /* text to type into the page */
	OVERLAY_QUIT,
};

/* Called from the thread that reads the pad. full_screen: whether the window
 * fills the screen now. */
void overlay_open(int full_screen);
void overlay_close(void);
int overlay_is_open(void);
/* Buttons just pressed (libnx's HidNpadButton_*), or a finger put down at
 * (x, y): what they chose. */
int overlay_pad(uint64_t down);
int overlay_touch(int x, int y);
/* The window filled the screen or stopped: drawn again if it changed. */
void overlay_full_screen(int full_screen);
/* What the pad is for the page, chosen in the panel. */
enum {
	OVERLAY_BROWSER,      /* touch, and the pad to move between links */
	OVERLAY_KEYBOARD_MOUSE, /* a keyboard and a mouse, by the bindings */
	OVERLAY_CONTROLLER,   /* a game controller (the Gamepad API) */
};
int overlay_mode(void);
/* The keyboard and mouse mode's bindings, to show them: a key per input of
 * keys.h (GX_INPUTS of them). */
void overlay_bindings(const int *keys);

/* For glwin.c (struct glwin_host): the picture, while the panel is open. */
const uint32_t *overlay_picture(int *x, int *y, int *width, int *height, unsigned *serial);

#endif
