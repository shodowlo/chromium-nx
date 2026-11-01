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
