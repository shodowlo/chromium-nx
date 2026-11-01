/*
 * cdp - a screen and inputs for a headless Chromium, over its remote
 * debugging pipe. See cdp.c.
 */
#ifndef GX_CDP_H
#define GX_CDP_H

#include <stddef.h>
#include <stdint.h>

enum {
	CDP_NONE,
	CDP_TOUCH_DOWN,   /* x, y: a finger on the page */
	CDP_TOUCH_MOVE,
	CDP_TOUCH_UP,
	CDP_SCROLL,       /* y: pixels, down if positive; x likewise */
	CDP_BACK,
	CDP_RELOAD,
	CDP_NAVIGATE,     /* text: a URL */
	CDP_TEXT,         /* text: typed into whatever has the focus */
	CDP_KEYBOARD,     /* the user wants the on-screen keyboard, to edit what has the focus */
	CDP_ENTER,
	CDP_BACKSPACE,
	CDP_KEY,          /* x: a key pressed and let go, by the number Linux gives it */
	CDP_ACTIVATE,     /* what has the focus is chosen: edited if it is text, pressed otherwise */
	CDP_QUIT,
};

struct cdp_input {
	int type;
	int x, y;
	char text[512];
};

/* Makes the pipe Chromium is told about with --remote-debugging-pipe (its
 * descriptors 3 and 4) and starts the thread at the other end. To be called
 * once the kernel runs and before the guest does, and before anything else
 * opens descriptors for it.
 *
 * With window, Chromium has a window of its own (wl.c shows it and gives it
 * what the user does) and the pipe is only there for what a window does not
 * say: whether what has the focus is text to edit. */
int cdp_start(int window);

/* For wl.c. cdp_activate() asks what CDP_ACTIVATE is to do, and returns 0 if
 * there is no one to ask. cdp_wanted() is the answer when it has come, once:
 * CDP_KEYBOARD (also when the user touched some text: no one asked then) or
 * CDP_ENTER, and CDP_NONE the rest of the time. */
int cdp_activate(void);
int cdp_wanted(void);
/* Has the page go back (CDP_BACK), load again (CDP_RELOAD), or take the
 * keyboard back from the address above it (CDP_NAVIGATE); returns 0 if there
 * is no one to tell. */
int cdp_page(int what);

/* Pictures shown during the last whole second, for a host that wants to
 * display it. */
extern int cdp_frames_per_second;
/* Failed requests go to the log (with -p): see handle_window_message(). */
extern int cdp_network_log;

/* What the host provides. Both are called from the one thread of cdp.c. */

/* A new picture of the page, as a JPEG. */
void host_frame(const uint8_t *jpeg, size_t len);
/* The next thing the user did, if any: returns 0 when there is nothing. */
int host_input(struct cdp_input *in);

#endif
