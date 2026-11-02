/*
 * glwin - the windows a guest draws with the GPU, put together on the screen.
 * See glwin.c. For a host's EGL functions to call: they stand in for the ones
 * a guest calls with its window surfaces, which are not surfaces of the
 * host's EGL but names glwin.c gives out.
 */
#ifndef GX_GLWIN_H
#define GX_GLWIN_H

#include <stdint.h>

struct glwin_host {
	/* A function of the host's EGL or GLES by its name: the real one. */
	void *(*symbol)(const char *name);
	/* Makes the surface that is the screen, for a configuration. */
	void *(*screen)(void *dpy, void *config);
	/* Called with the screen drawn and current, before it is shown: for a
	 * host that wants to look at it. May be NULL. */
	void (*presented)(void *dpy, void *screen);
	/* What is laid over the windows, if anything: a picture (RGBA, colours
	 * already multiplied by their opacity, rows from the top), where it goes
	 * on the screen and its size, and a number that changes when the
	 * picture does. NULL when there is nothing. May be NULL. */
	const uint32_t *(*overlay)(int *x, int *y, int *width, int *height, unsigned *serial);
	/* The mouse pointer to draw, if it is to be: its picture (as the
	 * overlay's, never changing), and where its tip is. May be NULL. */
	const uint32_t *(*cursor)(int *x, int *y, int *width, int *height);
};

/* To be called before anything else here. */
void glwin_init(const struct glwin_host *host);

/* eglCreateWindowSurface: guest_window is what the guest passed as its native
 * window. Returns the surface, or NULL. */
void *glwin_create(void *dpy, void *config, void *guest_window);

/* eglSwapBuffers, eglDestroySurface and eglQuerySurface: 1 if done, 0 if it
 * failed, -1 if the surface is not a window's (the host's own function is to
 * be called). */
int glwin_swap(void *dpy, void *surface);
int glwin_destroy(void *dpy, void *surface);
int glwin_query(void *dpy, void *surface, int32_t attribute, int32_t *value);

/* eglMakeCurrent (1 if done, 0 if it failed, -1 if it could not be tried) and
 * eglGetCurrentSurface, whatever the surfaces are. */
int glwin_make_current(void *dpy, void *draw, void *read, void *ctx);
void *glwin_current_surface(int32_t which);

/* The host's surface behind one the guest knows, for any other EGL function
 * that takes a surface: the same if it is not a window's. */
void *glwin_surface(void *surface);

#endif
