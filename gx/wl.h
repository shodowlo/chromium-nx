/*
 * wl - the display a Chromium with a window draws on: a Wayland compositor
 * small enough to live in the host. See wl.c.
 */
#ifndef GX_WL_H
#define GX_WL_H

#include <stddef.h>
#include <stdint.h>

#define WL_SCREEN_WIDTH 1280
#define WL_SCREEN_HEIGHT 720
/* Fingers on the touch screen told apart (struct cdp_input's finger). */
#define WL_FINGERS 5

/* Opens the display (the socket /run/wayland-0 of the guest) and starts the
 * thread that serves it. To be called once the kernel runs and its /run is
 * there, before the guest starts; the guest finds the display with
 * XDG_RUNTIME_DIR=/run and WAYLAND_DISPLAY=wayland-0. */
int wl_start(void);

/* Asks the window to close, the way its user would, so that the program
 * behind it ends as it means to; wl_closed() says when it is gone. If it does
 * not go, host_exit() is called after a few seconds all the same. */
void wl_close(void);
int wl_closed(void);

/* For glwin.c, by the number a wl_surface has on the wire: where it goes on
 * the screen (1 if it is the window, 2 and up for popups in the order they
 * are laid, 0 if it is neither), and the size it is drawn at. */
int wl_surface_place(uint32_t surface, int *x, int *y);
void wl_surface_size(uint32_t surface, int width, int height);

/* Where the window has the address of the page, for the keyboard to come up
 * when it is touched or asked for: chromium-shell's if this is not called. */
void wl_address_bar(int left, int top, int right, int bottom);

/* Whether the window has asked to fill the screen and not undone it. */
int wl_full_screen(void);

/* Where the mouse pointer is; 0 if it has not come onto the window. */
int wl_pointer_at(int *x, int *y);

/* The screen as the window has it: WL_SCREEN_WIDTH x WL_SCREEN_HEIGHT, or
 * turned a quarter (portrait) when wl_set_portrait() asked for it; the turn
 * happens on wl.c's thread, and wl_portrait() says when it has. glwin.c
 * draws at this size and has the host's screen turned to match. */
extern int wl_width, wl_height;
void wl_set_portrait(int on);
/* A screen of more pixels than the console's own (1920 x 1080 on a TV), or
 * back to 1280 x 720: before the client comes, or while it is there (the
 * console put in its dock or taken out); a client that is never to change
 * scale is not called this. The window keeps its size in Wayland's units,
 * 1280 x 720, as do the touch screen, the pointer and the overlay; its
 * pictures are wl_scale times as large (the client is told so with
 * wp_fractional_scale_v1, wl.c), and glwin.c lays them on a screen as
 * large. */
extern float wl_scale;
void wl_set_screen(int width, int height);
int wl_portrait(void);

/* What the host provides, called from the one thread of wl.c, together with
 * host_input() of cdp.h for what the user does. */

/* The user wants to type: text holds what the field has now, and gets what it
 * should have instead. Returns 0 if the user gave up. May take as long as
 * the user does. */
int host_ask_text(char *text, size_t room);

/* The window's picture: rows of stride bytes, four per pixel in the order
 * blue, green, red, unused. Valid until the call returns. */
void host_present(const uint8_t *pixels, int width, int height, int stride);

#endif
