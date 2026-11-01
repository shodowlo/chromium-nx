/*
 * glwin - the windows a guest draws with the GPU, put together on the screen.
 *
 * A program with one window can be given the screen itself to draw on. A
 * browser has more: its menus and the list under its address are windows of
 * their own, each with its own EGL surface, and Wayland (wl.c) only says
 * where they go. A host may have nothing to draw on but the screen (Horizon's
 * EGL has no off-screen surfaces), so this is how they share it:
 *
 * every window surface the guest asks for is a name of ours for the one
 * surface there is. The guest draws a window there, in the bottom left corner
 * (a window believes it is as large as it says, not as the screen), and when
 * it presents it, what it drew is copied into a texture kept for that window;
 * then the screen is drawn again from the textures of all the windows, the
 * window first and the popups over it where wl.c has them, and shown. The
 * same when a window goes away.
 *
 * The copying and the drawing are done in a context of our own, on the
 * guest's thread that draws and inside the EGL calls it makes, so that
 * nothing of the guest's GL state is touched; what it had current is put back
 * before returning. This relies on a guest that draws a window whole and
 * presents it before it draws another, which is what a browser does.
 *
 * The host gives the functions of its EGL and GLES by name, and the surface
 * that is the screen; its own EGL functions for the guest call the ones here.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "gx.h"
#include "wl.h"
#include "glwin.h"

#define EGL_NONE 0x3038
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define EGL_DRAW 0x3059
#define EGL_READ 0x305A
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define GL_TRIANGLE_STRIP 0x0005
#define GL_ONE 1
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_BLEND 0x0BE2
#define GL_TEXTURE_2D 0x0DE1
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LINEAR 0x2601
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31

#define MAX_WINDOWS 16

/* What libwayland-egl hands to eglCreateWindowSurface for a window, as the
 * guest has it in its memory: its size, and the wl_surface it is for, whose
 * number on the wire is what wl.c knows it by. */
struct guest_window {
	intptr_t version;
	int32_t width, height, dx, dy, attached_width, attached_height;
	void *driver_private;
	void *resize_callback, *destroy_callback;
	const struct guest_proxy {
		const void *interface, *implementation;
		uint32_t id;
	} *surface;
};

/* A window: the address of this is the surface the guest knows. */
struct window {
	int used;
	const struct guest_window *guest;
	uint32_t id;
	int32_t width, height;
	unsigned texture;              /* its picture, once it has presented one */
	int drawn;
};

static struct window windows[MAX_WINDOWS];
static struct window *current;       /* the one the guest has current to draw in */
static const struct glwin_host *host;
static void *screen, *context;
static int ready, failed, opaque;

/* The host's functions, found by name once. */
static void *(*CreateContext)(void *, void *, void *, const int32_t *);
static unsigned (*MakeCurrent)(void *, void *, void *, void *);
static void *(*GetCurrentContext)(void);
static void *(*GetCurrentSurface)(int32_t);
static unsigned (*SwapBuffers)(void *, void *);
static unsigned (*QuerySurface)(void *, void *, int32_t, int32_t *);
static int32_t (*GetError)(void);
static unsigned (*GlError)(void);
static void (*Viewport)(int32_t, int32_t, int32_t, int32_t);
static void (*ClearColor)(float, float, float, float);
static void (*Clear)(unsigned);
static void (*GenTextures)(int32_t, unsigned *);
static void (*DeleteTextures)(int32_t, const unsigned *);
static void (*BindTexture)(unsigned, unsigned);
static void (*TexParameteri)(unsigned, unsigned, int32_t);
static void (*CopyTexImage2D)(unsigned, int32_t, unsigned, int32_t, int32_t, int32_t, int32_t, int32_t);
static void (*TexImage2D)(unsigned, int32_t, int32_t, int32_t, int32_t, int32_t, unsigned, unsigned, const void *);
static void (*Enable)(unsigned);
static void (*Disable)(unsigned);
static void (*BlendFunc)(unsigned, unsigned);
static unsigned (*CreateProgram)(void);
static unsigned (*CreateShader)(unsigned);
static void (*ShaderSource)(unsigned, int32_t, const char *const *, const int32_t *);
static void (*CompileShader)(unsigned);
static void (*AttachShader)(unsigned, unsigned);
static void (*BindAttribLocation)(unsigned, unsigned, const char *);
static void (*LinkProgram)(unsigned);
static void (*UseProgram)(unsigned);
static int32_t (*GetUniformLocation)(unsigned, const char *);
static void (*Uniform4f)(int32_t, float, float, float, float);
static void (*VertexAttribPointer)(unsigned, int32_t, unsigned, unsigned char, int32_t, const void *);
static void (*EnableVertexAttribArray)(unsigned);
static void (*DrawArrays)(unsigned, int32_t, int32_t);

static int32_t where;   /* the uniform: the rectangle a picture is drawn in */

static int find_functions(void)
{
	static const struct {
		const char *name;
		void *slot;
	} wanted[] = {
		{ "eglCreateContext", &CreateContext },
		{ "eglMakeCurrent", &MakeCurrent }, { "eglGetCurrentContext", &GetCurrentContext },
		{ "eglGetCurrentSurface", &GetCurrentSurface }, { "eglSwapBuffers", &SwapBuffers },
		{ "eglQuerySurface", &QuerySurface }, { "eglGetError", &GetError }, { "glGetError", &GlError },
		{ "glViewport", &Viewport }, { "glClearColor", &ClearColor }, { "glClear", &Clear },
		{ "glGenTextures", &GenTextures }, { "glDeleteTextures", &DeleteTextures },
		{ "glBindTexture", &BindTexture }, { "glTexParameteri", &TexParameteri },
		{ "glCopyTexImage2D", &CopyTexImage2D }, { "glTexImage2D", &TexImage2D }, { "glEnable", &Enable }, { "glDisable", &Disable },
		{ "glBlendFunc", &BlendFunc }, { "glCreateProgram", &CreateProgram },
		{ "glCreateShader", &CreateShader }, { "glShaderSource", &ShaderSource },
		{ "glCompileShader", &CompileShader }, { "glAttachShader", &AttachShader },
		{ "glBindAttribLocation", &BindAttribLocation }, { "glLinkProgram", &LinkProgram },
		{ "glUseProgram", &UseProgram }, { "glGetUniformLocation", &GetUniformLocation },
		{ "glUniform4f", &Uniform4f }, { "glVertexAttribPointer", &VertexAttribPointer },
		{ "glEnableVertexAttribArray", &EnableVertexAttribArray }, { "glDrawArrays", &DrawArrays },
	};

	if (DrawArrays)
		return 0;
	for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
		void *fn = host ? host->symbol(wanted[i].name) : NULL;

		if (!fn) {
			host_log("gx: gl: the host has no %s\n", wanted[i].name);
			return -1;
		}
		memcpy(wanted[i].slot, &fn, sizeof(fn));
	}
	return 0;
}

void glwin_init(const struct glwin_host *h)
{
	host = h;
}

static struct window *window_of(void *surface)
{
	struct window *w = surface;

	return w >= windows && w < windows + MAX_WINDOWS && w->used ? w : NULL;
}

/* What the guest says of its window now, while the window is surely there. */
static void measure(struct window *w)
{
	/* A program that has no window to speak of has the screen. */
	w->width = w->guest ? w->guest->width : WL_SCREEN_WIDTH;
	w->height = w->guest ? w->guest->height : WL_SCREEN_HEIGHT;
	if (w->guest && w->guest->surface)
		w->id = w->guest->surface->id;
	if (w->width < 1 || w->width > WL_SCREEN_WIDTH)
		w->width = WL_SCREEN_WIDTH;
	if (w->height < 1 || w->height > WL_SCREEN_HEIGHT)
		w->height = WL_SCREEN_HEIGHT;
}

/* Our context, current on the screen: made the first time, with the little
 * it takes to draw a picture in a rectangle. */
static int prepare(void *dpy, void *config)
{
	static const int32_t version[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static const char *const vertex =
		"attribute vec2 p; uniform vec4 r; varying vec2 t;\n"
		"void main() { t = p; gl_Position = vec4(mix(r.xy, r.zw, p), 0.0, 1.0); }\n";
	static const char *const fragment =
		"precision mediump float; uniform sampler2D s; varying vec2 t;\n"
		"void main() { gl_FragColor = texture2D(s, t); }\n";
	static const float corners[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
	unsigned program, shader;

	if (failed || !screen)
		return -1;
	if (!context && config)
		context = CreateContext(dpy, config, NULL, version);
	if (!context || !MakeCurrent(dpy, screen, screen, context)) {
		host_log("gx: gl: cannot draw on the screen (%#x)\n", GetError());
		failed = 1;
		return -1;
	}
	if (ready)
		return 0;
	ready = 1;
	program = CreateProgram();
	shader = CreateShader(GL_VERTEX_SHADER);
	ShaderSource(shader, 1, &vertex, NULL);
	CompileShader(shader);
	AttachShader(program, shader);
	shader = CreateShader(GL_FRAGMENT_SHADER);
	ShaderSource(shader, 1, &fragment, NULL);
	CompileShader(shader);
	AttachShader(program, shader);
	BindAttribLocation(program, 0, "p");
	LinkProgram(program);
	UseProgram(program);
	where = GetUniformLocation(program, "r");
	VertexAttribPointer(0, 2, GL_FLOAT, 0, 0, corners);
	EnableVertexAttribArray(0);
	Viewport(0, 0, WL_SCREEN_WIDTH, WL_SCREEN_HEIGHT);
	BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	return 0;
}

/* Keeps what the guest just drew for a window: the bottom left corner of the
 * screen's buffer, of the window's size, into the window's texture. With its
 * opacity if the screen has one to give (popups have round corners and a
 * shadow), without if it has not. */
static void keep(struct window *w)
{
	if (!w->texture) {
		GenTextures(1, &w->texture);
		BindTexture(GL_TEXTURE_2D, w->texture);
		TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	BindTexture(GL_TEXTURE_2D, w->texture);
	while (GlError())
		;
	if (!opaque) {
		CopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, w->width, w->height, 0);
		if (GlError()) {
			host_log("gx: gl: the screen has no opacity to copy: popups will be plain rectangles\n");
			opaque = 1;
		}
	}
	if (opaque)
		CopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, w->width, w->height, 0);
	w->drawn = 1;
}

/* Draws one window where it goes; (x, y) is its top left corner. */
static void draw(const struct window *w, int32_t x, int32_t y, int blend)
{
	float left = 2.0f * x / WL_SCREEN_WIDTH - 1.0f;
	float right = 2.0f * (x + w->width) / WL_SCREEN_WIDTH - 1.0f;
	float top = 1.0f - 2.0f * y / WL_SCREEN_HEIGHT;
	float bottom = 1.0f - 2.0f * (y + w->height) / WL_SCREEN_HEIGHT;

	BindTexture(GL_TEXTURE_2D, w->texture);
	if (blend && !opaque)
		Enable(GL_BLEND);
	else
		Disable(GL_BLEND);
	Uniform4f(where, left, bottom, right, top);
	DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* What the host lays over the windows (its overlay), if anything: its
 * picture goes into a texture of ours when it has changed. */
static void draw_overlay(void)
{
	static unsigned texture, serial = ~0u;
	const uint32_t *picture;
	int x, y, width, height;
	unsigned now;
	float left, right, top, bottom;

	if (!host->overlay || !(picture = host->overlay(&x, &y, &width, &height, &now)))
		return;
	if (!texture) {
		GenTextures(1, &texture);
		BindTexture(GL_TEXTURE_2D, texture);
		TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	BindTexture(GL_TEXTURE_2D, texture);
	if (now != serial) {
		TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, picture);
		serial = now;
	}
	/* Its rows go from the top, where the windows' go from the bottom. */
	left = 2.0f * x / WL_SCREEN_WIDTH - 1.0f;
	right = 2.0f * (x + width) / WL_SCREEN_WIDTH - 1.0f;
	top = 1.0f - 2.0f * y / WL_SCREEN_HEIGHT;
	bottom = 1.0f - 2.0f * (y + height) / WL_SCREEN_HEIGHT;
	Enable(GL_BLEND);
	Uniform4f(where, left, top, right, bottom);
	DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* The screen, from all the windows there are; keeping first what was just
 * drawn for one of them, if any. In our context, and what the guest had
 * current is put back. */
static void present(void *dpy, struct window *presented, unsigned gone)
{
	void *was_context = GetCurrentContext();
	void *was_draw = GetCurrentSurface(EGL_DRAW), *was_read = GetCurrentSurface(EGL_READ);
	int32_t x, y;

	if (!prepare(dpy, NULL)) {
		if (presented)
			keep(presented);
		if (gone)
			DeleteTextures(1, &gone);
		ClearColor(0, 0, 0, 1);
		Clear(GL_COLOR_BUFFER_BIT);
		/* The window, then the popups in the order wl.c has them, which
		 * is the order they came in. */
		for (int i = 0; i < MAX_WINDOWS; i++)
			if (windows[i].used && windows[i].drawn &&
			    (!windows[i].guest || wl_surface_place(windows[i].id, &x, &y) == 1))
				draw(&windows[i], 0, 0, 0);
		for (int order = 0; order < MAX_WINDOWS; order++)
			for (int i = 0; i < MAX_WINDOWS; i++)
				if (windows[i].used && windows[i].drawn && windows[i].guest &&
				    wl_surface_place(windows[i].id, &x, &y) == 2 + order)
						draw(&windows[i], x, y, 1);
		draw_overlay();
		if (host->presented)
			host->presented(dpy, screen);
		SwapBuffers(dpy, screen);
	}
	MakeCurrent(dpy, was_draw, was_read, was_context);
}

void *glwin_create(void *dpy, void *config, void *guest_window)
{
	struct window *w = NULL;

	if (find_functions())
		return NULL;
	for (int i = 0; i < MAX_WINDOWS && !w; i++)
		if (!windows[i].used)
			w = &windows[i];
	if (!w)
		return NULL;
	if (!screen) {
		void *was_context = GetCurrentContext();
		void *was_draw = GetCurrentSurface(EGL_DRAW), *was_read = GetCurrentSurface(EGL_READ);

		screen = host->screen(dpy, config);
		if (!screen)
			return NULL;
		prepare(dpy, config);
		MakeCurrent(dpy, was_draw, was_read, was_context);
	}
	memset(w, 0, sizeof(*w));
	w->used = 1;
	w->guest = guest_window;
	measure(w);
	if (gx_trace)
		host_log("gx: gl: a window surface for wl_surface %u, %dx%d\n", w->id, w->width, w->height);
	return w;
}

int glwin_swap(void *dpy, void *surface)
{
	struct window *w = window_of(surface);

	if (!w)
		return -1;
	measure(w);
	if (gx_trace) {
		int x = 0, y = 0, place = wl_surface_place(w->id, &x, &y);

		host_log("gx: gl: wl_surface %u presents %dx%d, place %d at %d,%d\n", w->id, w->width,
			 w->height, place, x, y);
	}
	/* A popup that is drawn this way has no picture of the other kind to
	 * give wl.c its size, which it needs to tell what a finger is on. */
	wl_surface_size(w->id, w->width, w->height);
	present(dpy, w, 0);
	return 1;
}

int glwin_destroy(void *dpy, void *surface)
{
	struct window *w = window_of(surface);
	unsigned texture;

	if (!w)
		return -1;
	/* What it showed is taken off the screen by drawing the rest again. */
	texture = w->texture;
	if (current == w)
		current = NULL;
	memset(w, 0, sizeof(*w));
	if (ready)
		present(dpy, NULL, texture);
	return 1;
}

int glwin_query(void *dpy, void *surface, int32_t attribute, int32_t *value)
{
	struct window *w = window_of(surface);

	if (!w)
		return -1;
	if (value && (attribute == EGL_WIDTH || attribute == EGL_HEIGHT)) {
		measure(w);
		*value = attribute == EGL_WIDTH ? w->width : w->height;
		return 1;
	}
	return QuerySurface(dpy, screen, attribute, value) ? 1 : 0;
}

void *glwin_surface(void *surface)
{
	return window_of(surface) ? screen : surface;
}

int glwin_make_current(void *dpy, void *draw, void *read, void *ctx)
{
	struct window *w = window_of(draw);

	if (find_functions())
		return -1;
	if (!MakeCurrent(dpy, glwin_surface(draw), glwin_surface(read), ctx))
		return 0;
	/* Which of the windows it is now drawing in: they are all the same
	 * surface underneath. */
	if (w || !draw || glwin_surface(draw) != screen)
		current = w;
	return 1;
}

void *glwin_current_surface(int32_t which)
{
	void *real;

	if (find_functions())
		return NULL;
	real = GetCurrentSurface(which);
	return real && real == screen && current ? (void *)current : real;
}
