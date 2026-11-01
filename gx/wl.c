/*
 * wl.c - a Wayland compositor for one client with one window.
 *
 * A Chromium that has a window (chromium-shell with --ozone-platform=wayland)
 * wants a display server. This is one, cut down to what that takes: it
 * listens on a socket of the guest's kernel, speaks the wire protocol by
 * hand, and has a single screen that the client's window fills. The window's
 * pixels are in shared memory the client made (wl_shm); that memory is ours
 * already (gmem.c keeps the one copy of every shared file), so a frame is
 * shown straight from where the client drew it, with nothing in between.
 *
 * What is here is what Chromium uses, found by running it: the core globals,
 * xdg-shell for the window, a seat with touch and pointer. Requests that
 * change nothing for a single full-screen window are read and dropped.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <lkl.h>
#include "gx.h"
#include "cdp.h"
#include "wl.h"

#define SOCKET_PATH "/run/wayland-0"
#define KEYMAP_PATH "/usr/share/gx/keymap.xkb"
#define MAX_OBJECTS 4096
#define MAX_POOLS 64
#define MAX_CALLBACKS 16
#define FRAME_NS 16666666ULL

enum {
	T_NONE, T_DISPLAY, T_REGISTRY, T_COMPOSITOR, T_SHM, T_POOL, T_BUFFER, T_SURFACE,
	T_REGION, T_SEAT, T_TOUCH, T_POINTER, T_KEYBOARD, T_OUTPUT, T_SUBCOMPOSITOR,
	T_SUBSURFACE, T_WM_BASE, T_POSITIONER, T_XDG_SURFACE, T_TOPLEVEL, T_POPUP,
	T_DECORATION_MANAGER, T_DECORATION, T_DATA_MANAGER, T_DATA_SOURCE, T_DATA_DEVICE,
};

static const char *const type_names[] = {
	"?", "wl_display", "wl_registry", "wl_compositor", "wl_shm", "wl_shm_pool", "wl_buffer",
	"wl_surface", "wl_region", "wl_seat", "wl_touch", "wl_pointer", "wl_keyboard", "wl_output",
	"wl_subcompositor", "wl_subsurface", "xdg_wm_base", "xdg_positioner", "xdg_surface",
	"xdg_toplevel", "xdg_popup", "zxdg_decoration_manager_v1", "zxdg_toplevel_decoration_v1",
	"wl_data_device_manager", "wl_data_source", "wl_data_device",
};

/* What the registry announces; the name of a global is its place here + 1. */
static const struct {
	const char *interface;
	uint32_t version;
	uint8_t type;
} globals[] = {
	{ "wl_compositor", 4, T_COMPOSITOR },
	{ "wl_subcompositor", 1, T_SUBCOMPOSITOR },
	{ "wl_shm", 1, T_SHM },
	{ "wl_output", 2, T_OUTPUT },
	{ "wl_seat", 5, T_SEAT },
	{ "xdg_wm_base", 2, T_WM_BASE },
	/* So that we can say the window's frame is ours to draw, and the client
	 * draws no title bar buttons: there is nothing to minimise to. */
	{ "zxdg_decoration_manager_v1", 1, T_DECORATION_MANAGER },
	/* The clipboard, of which we only read what the client copies: that is
	 * how the text of a field gets to the on-screen keyboard to be edited
	 * (see edit_step()). */
	{ "wl_data_device_manager", 3, T_DATA_MANAGER },
};

struct object {
	uint8_t type;
	uint8_t version;
	/* wl_buffer: its pool, and where the picture is in it. wl_surface: the
	 * buffer attached and not shown yet, and its roles. xdg_surface and
	 * xdg_toplevel: the wl_surface. */
	uint32_t pool, surface, pending, xdg, toplevel;
	int32_t offset, width, height, stride;
	uint8_t attached, configured;
	uint8_t full_screen;   /* wl_surface of a window: asked for and not undone */
};

struct pool {
	int fd, refs;   /* refs is 0 when the slot is free */
};

static struct object objects[MAX_OBJECTS];
static struct pool pools[MAX_POOLS];
static int client = -1;
static uint32_t serial = 1;
static uint32_t main_surface, output_id, touch_id, pointer_id, pointer_inside;
static uint32_t keyboard_id, keyboard_inside;
static int has_keymap;
/* The client's clipboard: the wl_data_source it last set as the selection,
 * the kind of text it offers (0: none), and a count of the times it changed. */
static uint32_t selection, selection_changes;
static char selection_mime[48];
/* The kind of text each of the last few data sources offers, by id. */
static char source_mime[8][48];
static uint32_t source_mime_id[8];
static uint32_t callbacks[MAX_CALLBACKS];
static int callback_count;
static uint64_t last_frame;
static int frames, counted;
static uint64_t count_since, present_ns;

/*
 * Windows on top of the window: menus, the list under an address being typed.
 * The client says where each goes relative to what it comes out of (a
 * positioner), and draws it like the window; we keep its picture, put it over
 * the window's, and give it the touches that land on it.
 */
#define MAX_POPUPS 8
#define MAX_POSITIONERS 8

struct positioner {
	uint32_t id;
	int32_t width, height;               /* of the popup */
	int32_t x, y, w, h;                  /* what it is anchored to, in its parent */
	uint32_t anchor, gravity;
	int32_t dx, dy;
};

struct popup {
	uint32_t id, surface, xdg;           /* id is 0 when the slot is free */
	int32_t x, y;                        /* where its picture goes on the screen */
	int32_t from_x, from_y;              /* a subsurface: where its parent's is */
	int32_t width, height;               /* of the picture, once there is one */
	uint8_t *pixels;
};

static struct positioner positioners[MAX_POSITIONERS];
static struct popup popups[MAX_POPUPS];   /* the later ones are on top */
static uint32_t main_buffer;              /* the window's picture: kept until the next */
static uint8_t *canvas;                   /* the screen when there are popups */
static uint32_t touch_surface;            /* what the finger went down on */
static int32_t touch_dx, touch_dy;

/* The kernel's socket structures, which LKL's headers do not have. */
struct wl_msghdr {
	void *name;
	int name_len;
	void *iov;
	uint64_t iov_len;
	void *control;
	uint64_t control_len;
	unsigned flags;
};
struct wl_iovec { void *base; uint64_t len; };
struct wl_cmsg { uint64_t len; int level, type; int fds[28]; };

static int received_fds[64];
static int fd_head, fd_count;

/* ---------- events, to the client ---------- */

static uint8_t out[1 << 16];
static size_t out_len;

static void flush(void)
{
	for (size_t done = 0; client >= 0 && done < out_len;) {
		long n = lkl_sys_write(client, (char *)out + done, out_len - done);

		if (n <= 0)
			break;
		done += n;
	}
	out_len = 0;
}

/* Queues an event. The format says what follows: u an unsigned word, i a
 * signed one, f a number of pixels (sent as Wayland's fixed point), s a
 * string, a an array of words (a pointer, then how many). */
static void event(uint32_t object, uint16_t opcode, const char *format, ...)
{
	uint32_t *msg, n = 2;
	va_list ap;

	if (out_len + 512 > sizeof(out))
		flush();
	msg = (uint32_t *)(out + out_len);
	va_start(ap, format);
	for (; *format; format++) {
		if (*format == 'u' || *format == 'i') {
			msg[n++] = va_arg(ap, uint32_t);
		} else if (*format == 'f') {
			msg[n++] = (uint32_t)(va_arg(ap, int32_t) * 256);
		} else if (*format == 's') {
			const char *text = va_arg(ap, const char *);
			uint32_t len = strlen(text) + 1;

			msg[n++] = len;
			msg[n + (len - 1) / 4] = 0;
			memcpy(&msg[n], text, len);
			n += (len + 3) / 4;
		} else if (*format == 'a') {
			const uint32_t *words = va_arg(ap, const uint32_t *);
			uint32_t count = va_arg(ap, uint32_t);

			msg[n++] = count * 4;
			memcpy(&msg[n], words, count * 4);
			n += count;
		}
	}
	va_end(ap);
	msg[0] = object;
	msg[1] = n * 4 << 16 | opcode;
	out_len += n * 4;
}

static void delete_id(uint32_t id)
{
	if (id < MAX_OBJECTS)
		memset(&objects[id], 0, sizeof(objects[id]));
	event(1, 1, "u", id);   /* wl_display.delete_id */
}

static struct object *create(uint32_t id, uint8_t type, uint8_t version)
{
	if (!id || id >= MAX_OBJECTS)
		return &objects[0];   /* a scrap to write to: the id is unusable */
	memset(&objects[id], 0, sizeof(objects[id]));
	objects[id].type = type;
	objects[id].version = version;
	return &objects[id];
}

/* ---------- the window's picture ---------- */

static void pool_unref(uint32_t index)
{
	if (index < MAX_POOLS && pools[index].refs && !--pools[index].refs)
		lkl_sys_close(pools[index].fd);
}

/* The pixels of a buffer, if they are all there. */
static uint8_t *buffer_pixels(uint32_t id)
{
	struct object *buffer = id && id < MAX_OBJECTS ? &objects[id] : NULL;
	uint64_t size = 0;
	uint8_t *memory;

	if (!buffer || buffer->type != T_BUFFER || buffer->pool >= MAX_POOLS || !pools[buffer->pool].refs)
		return NULL;
	memory = gmem_shared_peek(pools[buffer->pool].fd, &size);
	if (!memory || buffer->width <= 0 || buffer->height <= 0 || buffer->offset < 0 ||
	    (uint64_t)buffer->offset + (uint64_t)buffer->stride * buffer->height > size)
		return NULL;
	return memory + buffer->offset;
}

static struct popup *popup_of(uint32_t surface)
{
	for (int i = 0; i < MAX_POPUPS; i++)
		if (popups[i].id && popups[i].surface == surface)
			return &popups[i];
	return NULL;
}

/* Puts the screen together and shows it: the window, and the popups over it
 * in the order they came. Their pixels come with their colours already
 * multiplied by their opacity, which is how they are laid over. */
static void compose(void)
{
	struct object *buffer = &objects[main_buffer < MAX_OBJECTS ? main_buffer : 0];
	uint8_t *window = buffer_pixels(main_buffer);
	uint64_t start = host_now_ns(), took;
	int covered = 0;

	if (!window)
		return;
	for (int i = 0; i < MAX_POPUPS; i++)
		covered |= popups[i].id && popups[i].pixels;
	if (covered && !canvas)
		canvas = malloc(WL_SCREEN_WIDTH * WL_SCREEN_HEIGHT * 4);
	if (!covered || !canvas || buffer->width != WL_SCREEN_WIDTH || buffer->height != WL_SCREEN_HEIGHT) {
		host_present(window, buffer->width, buffer->height, buffer->stride);
	} else {
		for (int y = 0; y < WL_SCREEN_HEIGHT; y++)
			memcpy(canvas + (size_t)y * WL_SCREEN_WIDTH * 4, window + (size_t)y * buffer->stride,
			       WL_SCREEN_WIDTH * 4);
		for (int i = 0; i < MAX_POPUPS; i++) {
			const struct popup *p = &popups[i];

			if (!p->id || !p->pixels)
				continue;
			for (int y = 0; y < p->height; y++) {
				const uint8_t *src = p->pixels + (size_t)y * p->width * 4;
				int sy = p->y + y;

				if (sy < 0 || sy >= WL_SCREEN_HEIGHT)
					continue;
				for (int x = 0; x < p->width; x++, src += 4) {
					int sx = p->x + x;
					uint8_t *dst;
					unsigned keep = 255 - src[3];

					if (sx < 0 || sx >= WL_SCREEN_WIDTH || !src[3])
						continue;
					dst = canvas + ((size_t)sy * WL_SCREEN_WIDTH + sx) * 4;
					if (!keep) {
						memcpy(dst, src, 4);
						continue;
					}
					for (int c = 0; c < 3; c++)
						dst[c] = src[c] + dst[c] * keep / 255;
				}
			}
		}
		host_present(canvas, WL_SCREEN_WIDTH, WL_SCREEN_HEIGHT, WL_SCREEN_WIDTH * 4);
	}
	took = host_now_ns() - start;
	if (!frames++ || gx_trace)
		host_log("gx: wayland: picture %d, %dx%d\n", frames, buffer->width, buffer->height);
	/* How smooth it is, every five seconds something was shown. */
	if (!count_since)
		count_since = start;
	counted++;
	present_ns += took;
	if (start - count_since >= 5000000000ULL) {
		host_log("gx: wayland: %d pictures in %lu ms, %lu ms each to show\n", counted,
			 (unsigned long)((start - count_since) / 1000000),
			 (unsigned long)(present_ns / counted / 1000000));
		count_since = start;
		counted = 0;
		present_ns = 0;
	}
}

/* Shows the buffer a surface just committed, if it is the window's or a
 * popup's. The window's buffer stays ours until its next one: a popup that
 * comes or goes needs the window drawn again under it. A popup's is copied. */
static void show(struct object *surface, uint32_t surface_id)
{
	struct popup *p = popup_of(surface_id);
	struct object *buffer = surface->pending < MAX_OBJECTS ? &objects[surface->pending] : NULL;

	if (!buffer || buffer->type != T_BUFFER)
		return;
	if (surface_id == main_surface) {
		if (main_buffer && main_buffer != surface->pending)
			event(main_buffer, 0, "");   /* wl_buffer.release */
		main_buffer = surface->pending;
		compose();
		return;
	}
	if (p) {
		const uint8_t *pixels = buffer_pixels(surface->pending);
		struct object *xdg = &objects[p->xdg < MAX_OBJECTS ? p->xdg : 0];

		if (pixels && buffer->width <= 4096 && buffer->height <= 4096) {
			if (!p->pixels || p->width != buffer->width || p->height != buffer->height) {
				free(p->pixels);
				p->pixels = malloc((size_t)buffer->width * buffer->height * 4);
				p->width = buffer->width;
				p->height = buffer->height;
			}
			for (int y = 0; p->pixels && y < p->height; y++)
				memcpy(p->pixels + (size_t)y * p->width * 4,
				       pixels + (size_t)y * buffer->stride, p->width * 4);
			(void)xdg;
			compose();
		}
	}
	/* We keep nothing of it: the client may draw in it again. */
	event(surface->pending, 0, "");   /* wl_buffer.release */
}

/* For the host's GL (glwin.c), whose windows are not pictures that come
 * through here: where a surface goes. 1 for the window, 2 and up for the
 * popups in the order they are laid, 0 for anything else. */
int wl_surface_place(uint32_t surface, int *x, int *y)
{
	int order = 0;

	if (!surface)
		return 0;
	*x = *y = 0;
	if (surface == main_surface)
		return 1;
	for (int i = 0; i < MAX_POPUPS; i++) {
		if (!popups[i].id)
			continue;
		if (popups[i].surface == surface) {
			*x = popups[i].x;
			*y = popups[i].y;
			return 2 + order;
		}
		order++;
	}
	return 0;
}

/* And how large it has drawn it, which says what a finger is on. */
void wl_surface_size(uint32_t surface, int width, int height)
{
	struct popup *p = popup_of(surface);

	if (p && !p->pixels) {
		p->width = width;
		p->height = height;
	}
}

static void popup_gone(struct popup *p)
{
	free(p->pixels);
	memset(p, 0, sizeof(*p));
	compose();
}

/* Where a popup goes: the point of its anchor the positioner names, then the
 * side of that point it is to be on, kept on the screen. (x, y) is where the
 * parent's own (0, 0) is on the screen. */
static void place_popup(const struct positioner *q, int32_t x, int32_t y, int32_t *px, int32_t *py)
{
	static const int8_t side[9][2] = {
		{ 0, 0 }, { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 }, { -1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 },
	};
	const int8_t *a = side[q->anchor < 9 ? q->anchor : 0], *g = side[q->gravity < 9 ? q->gravity : 0];

	x += q->x + (a[0] < 0 ? 0 : a[0] > 0 ? q->w : q->w / 2) + q->dx;
	y += q->y + (a[1] < 0 ? 0 : a[1] > 0 ? q->h : q->h / 2) + q->dy;
	x -= g[0] < 0 ? q->width : g[0] > 0 ? 0 : q->width / 2;
	y -= g[1] < 0 ? q->height : g[1] > 0 ? 0 : q->height / 2;
	if (x + q->width > WL_SCREEN_WIDTH)
		x = WL_SCREEN_WIDTH - q->width;
	if (y + q->height > WL_SCREEN_HEIGHT)
		y = WL_SCREEN_HEIGHT - q->height;
	*px = x < 0 ? 0 : x;
	*py = y < 0 ? 0 : y;
}

static void keyboard_focus(void);

/* The window is the screen, whatever it asks to be: maximized (1), which is
 * what makes a browser drop the margin it draws its shadow in, and active
 * (4); full-screen (2) when it asks for that, which is a page that wants the
 * whole screen for a video. */
static void configure_window(struct object *surface)
{
	static const uint32_t states[] = { 1, 4 }, full_states[] = { 2, 4 };

	if (surface->full_screen)
		event(surface->toplevel, 0, "iia", WL_SCREEN_WIDTH, WL_SCREEN_HEIGHT, full_states, 2);
	else
		event(surface->toplevel, 0, "iia", WL_SCREEN_WIDTH, WL_SCREEN_HEIGHT, states, 2);
	event(surface->xdg, 0, "u", serial++);   /* xdg_surface.configure */
}

static void commit(uint32_t id)
{
	struct object *surface = &objects[id];

	if (surface->toplevel && !surface->configured) {
		surface->configured = 1;
		main_surface = id;
		configure_window(surface);
		if (output_id)
			event(id, 0, "u", output_id);    /* wl_surface.enter */
		keyboard_focus();
	}
	if (surface->attached) {
		struct popup *p = popup_of(id);

		surface->attached = 0;
		if (surface->pending) {
			show(surface, id);
		} else if (p && p->pixels) {
			free(p->pixels);
			p->pixels = NULL;
			p->width = p->height = 0;
			compose();
		}
		surface->pending = 0;
	}
}

/* Sends what is queued, the last event of which carries a file descriptor
 * (it is not in the event's words: it travels beside them). */
static void flush_with_fd(int fd)
{
	struct wl_iovec iov = { out, out_len };
	struct wl_cmsg control = { 16 + 4, 1 /* SOL_SOCKET */, 1 /* SCM_RIGHTS */, { fd } };
	struct wl_msghdr msg = { NULL, 0, &iov, 1, &control, 24, 0 };
	long params[6] = { client, (long)&msg, 0 };

	if (client >= 0 && out_len)
		lkl_syscall(__lkl__NR_sendmsg, params);
	out_len = 0;
}

/* The window has the keyboard as soon as there are both: Chromium only takes
 * keys for a window that has it. */
static void keyboard_focus(void)
{
	static const uint32_t none[1];

	if (!keyboard_id || !main_surface || keyboard_inside)
		return;
	keyboard_inside = 1;
	/* enter, then "modifiers" as a real keyboard would send, unless there
	 * is no key map: Chromium then has no keyboard state to put them in,
	 * and crashes on it. */
	event(keyboard_id, 1, "uua", serial++, main_surface, none, 0);
	if (has_keymap)
		event(keyboard_id, 4, "uuuuu", serial++, 0, 0, 0, 0);
}

/* Tells the client it may draw the next frame, at the pace of the screen. */
static void frame_tick(uint64_t now)
{
	if (!callback_count || now - last_frame < FRAME_NS)
		return;
	last_frame = now;
	for (int i = 0; i < callback_count; i++) {
		event(callbacks[i], 0, "u", (uint32_t)(now / 1000000));   /* wl_callback.done */
		event(1, 1, "u", callbacks[i]);
	}
	callback_count = 0;
}

/* Leaving: see wl_close(). A browser run as one process closes its window at
 * once and then takes seconds to write things down, without ever ending:
 * with 2.5 s after the window was gone its cookies were not on the disk yet,
 * with 8 s in all they were. */
#define CLOSE_NS 8000000000ULL

static uint64_t closing_since;

/* ---------- requests, from the client ---------- */

static int take_fd(void)
{
	int fd;

	if (!fd_count)
		return -1;
	fd = received_fds[fd_head];
	fd_head = (fd_head + 1) % 64;
	fd_count--;
	return fd;
}

static void bind_global(const uint32_t *w)
{
	uint32_t name = w[0], len = w[1];
	const uint32_t *rest = w + 2 + (len + 3) / 4;
	uint32_t version = rest[0], id = rest[1];
	struct object *o;

	if (!name || name > sizeof(globals) / sizeof(globals[0]))
		return;
	if (gx_trace)
		host_log("gx: wayland: bound %s version %u as %u\n", globals[name - 1].interface, version, id);
	o = create(id, globals[name - 1].type, version);
	switch (o->type) {
	case T_SHM:
		event(id, 0, "u", 0);   /* ARGB8888 */
		event(id, 0, "u", 1);   /* XRGB8888 */
		break;
	case T_SEAT:
		event(id, 0, "u", 1 | 2 | 4);   /* a pointer, a keyboard and a touch screen */
		if (version >= 2)
			event(id, 1, "s", "seat0");
		break;
	case T_OUTPUT:
		output_id = id;
		event(id, 0, "iiiiissi", 0, 0, 137, 77, 0, "gx", "screen", 0);
		event(id, 1, "uiii", 3, WL_SCREEN_WIDTH, WL_SCREEN_HEIGHT, 60000);
		if (version >= 2) {
			event(id, 3, "i", 1);   /* scale */
			event(id, 2, "");       /* done */
		}
		break;
	}
}

static void request(uint32_t id, uint16_t opcode, const uint32_t *w, uint32_t words)
{
	struct object *o = id < MAX_OBJECTS ? &objects[id] : &objects[0];

	if (gx_trace)
		host_log("gx: wayland: %s@%u request %u (%u words)\n",
			 type_names[o->type], id, opcode, words);
	switch (o->type) {
	case T_DISPLAY:
		if (opcode == 0) {            /* sync */
			event(w[0], 0, "u", serial++);
			event(1, 1, "u", w[0]);
		} else if (opcode == 1) {     /* get_registry */
			create(w[0], T_REGISTRY, 1);
			for (uint32_t i = 0; i < sizeof(globals) / sizeof(globals[0]); i++)
				event(w[0], 0, "usu", i + 1, globals[i].interface, globals[i].version);
		}
		break;
	case T_REGISTRY:
		if (opcode == 0)
			bind_global(w);
		break;
	case T_COMPOSITOR:
		create(w[0], opcode == 0 ? T_SURFACE : T_REGION, o->version);
		break;
	case T_SUBCOMPOSITOR:
		if (opcode == 1) {            /* get_subsurface(new id, surface, parent) */
			/* A surface that goes with another, at a place the client
			 * gives outright: what a browser draws the list under its
			 * address in. It is laid over the window like a popup. */
			struct popup *p = NULL;

			create(w[0], T_SUBSURFACE, 1)->surface = w[1];
			for (int i = 0; i < MAX_POPUPS && !p; i++)
				if (!popups[i].id)
					p = &popups[i];
			if (p) {
				struct popup *above = popup_of(w[2]);

				p->id = w[0];
				p->surface = w[1];
				p->from_x = above ? above->x : 0;
				p->from_y = above ? above->y : 0;
				p->x = p->from_x;
				p->y = p->from_y;
			}
		}
		break;
	case T_SHM:
		if (opcode == 0) {            /* create_pool: an fd came with it */
			struct object *pool = create(w[0], T_POOL, 1);
			int fd = take_fd();

			pool->pool = MAX_POOLS;
			for (uint32_t i = 0; i < MAX_POOLS && fd >= 0; i++) {
				if (pools[i].refs)
					continue;
				pools[i].fd = fd;
				pools[i].refs = 1;
				pool->pool = i;
				fd = -1;
			}
			if (fd >= 0)
				lkl_sys_close(fd);
		}
		break;
	case T_POOL:
		if (opcode == 0) {            /* create_buffer */
			struct object *buffer = create(w[0], T_BUFFER, 1);

			buffer->pool = o->pool;
			buffer->offset = w[1];
			buffer->width = w[2];
			buffer->height = w[3];
			buffer->stride = w[4];
			if (o->pool < MAX_POOLS)
				pools[o->pool].refs++;
		} else if (opcode == 1) {     /* destroy: its buffers keep the memory */
			pool_unref(o->pool);
			delete_id(id);
		}
		break;
	case T_BUFFER:
		if (opcode == 0) {
			if (id == main_buffer)
				main_buffer = 0;
			pool_unref(o->pool);
			delete_id(id);
		}
		break;
	case T_SURFACE:
		if (opcode == 0) {            /* destroy */
			if (id == main_surface)
				main_surface = 0;
			if (popup_of(id))
				popup_gone(popup_of(id));
			delete_id(id);
		} else if (opcode == 1) {     /* attach */
			o->pending = w[0];
			o->attached = 1;
		} else if (opcode == 3) {     /* frame */
			if (callback_count < MAX_CALLBACKS)
				callbacks[callback_count++] = w[0];
			else
				event(1, 1, "u", w[0]);
		} else if (opcode == 6) {
			commit(id);
		}
		break;
	case T_SEAT:
		if (opcode == 0) {
			create(w[0], T_POINTER, o->version);
			pointer_id = w[0];
			pointer_inside = 0;
		} else if (opcode == 1) {
			/* The key map is a file of the guest's (a standard one,
			 * put there when its root was made); without it, a key
			 * map of format 0, "none", and no keys. */
			int map = lkl_sys_open(KEYMAP_PATH, LKL_O_RDONLY, 0);
			struct lkl_stat st;

			create(w[0], T_KEYBOARD, o->version);
			keyboard_id = w[0];
			keyboard_inside = 0;
			has_keymap = map >= 0 && !lkl_sys_fstat(map, &st) && st.st_size > 0;
			if (!has_keymap) {
				if (map >= 0)
					lkl_sys_close(map);
				map = lkl_sys_open("/dev/null", LKL_O_RDONLY, 0);
			}
			if (map >= 0) {
				flush();
				event(w[0], 0, "uu", has_keymap ? 1 : 0, has_keymap ? (uint32_t)st.st_size : 0);
				flush_with_fd(map);
				lkl_sys_close(map);
			}
			if (has_keymap && o->version >= 4)
				event(w[0], 5, "ii", 0, 0);   /* repeat_info: we do the repeating */
			keyboard_focus();
		} else if (opcode == 2) {
			create(w[0], T_TOUCH, o->version);
			touch_id = w[0];
		}
		break;
	case T_WM_BASE:
		if (opcode == 0) {
			delete_id(id);
		} else if (opcode == 1) {
			static int next;

			create(w[0], T_POSITIONER, 1);
			memset(&positioners[next], 0, sizeof(positioners[next]));
			positioners[next].id = w[0];
			next = (next + 1) % MAX_POSITIONERS;
		} else if (opcode == 2) {     /* get_xdg_surface */
			create(w[0], T_XDG_SURFACE, o->version)->surface = w[1];
			if (w[1] < MAX_OBJECTS)
				objects[w[1]].xdg = w[0];
		}
		break;
	case T_XDG_SURFACE:
		if (opcode == 0) {
			delete_id(id);
		} else if (opcode == 1) {     /* get_toplevel */
			create(w[0], T_TOPLEVEL, o->version)->surface = o->surface;
			if (o->surface < MAX_OBJECTS)
				objects[o->surface].toplevel = w[0];
		} else if (opcode == 2) {     /* get_popup(new id, parent, positioner) */
			struct object *parent = &objects[w[1] < MAX_OBJECTS ? w[1] : 0];
			struct popup *above = popup_of(parent->surface), *p = NULL;
			const struct positioner *q = NULL;
			/* Where the parent's own (0, 0) is on the screen: a window's
			 * is where its frame starts in its picture. */
			int32_t x0 = above ? above->x + parent->offset : parent->offset;
			int32_t y0 = above ? above->y + parent->stride : parent->stride;
			int32_t x = 0, y = 0;

			create(w[0], T_POPUP, o->version)->surface = o->surface;
			for (int i = 0; i < MAX_POSITIONERS; i++)
				if (positioners[i].id == w[2])
					q = &positioners[i];
			for (int i = 0; i < MAX_POPUPS && !p; i++)
				if (!popups[i].id)
					p = &popups[i];
			if (q && p) {
				place_popup(q, x0, y0, &x, &y);
				p->id = w[0];
				p->surface = o->surface;
				p->xdg = id;
				/* Its picture has a frame of its own around what
				 * the positioner speaks of. */
				p->x = x - o->offset;
				p->y = y - o->stride;
			}
			event(w[0], 0, "iiii", x - x0, y - y0, q ? q->width : 1, q ? q->height : 1);
			event(id, 0, "u", serial++);
		} else if (opcode == 3) {     /* set_window_geometry: the frame in the picture */
			struct popup *p = popup_of(o->surface);

			if (p) {
				p->x += o->offset - (int32_t)w[0];
				p->y += o->stride - (int32_t)w[1];
			}
			o->offset = w[0];
			o->stride = w[1];
		}
		break;
	case T_TOUCH:
	case T_POINTER:
	case T_KEYBOARD:
		if ((o->type == T_POINTER && opcode == 1) || (o->type != T_POINTER && opcode == 0)) {
			if (id == touch_id)
				touch_id = 0;
			if (id == pointer_id)
				pointer_id = 0;
			if (id == keyboard_id)
				keyboard_id = 0;
			delete_id(id);
		}
		break;
	case T_POSITIONER:
		for (int i = 0; i < MAX_POSITIONERS; i++) {
			struct positioner *q = &positioners[i];

			if (q->id != id)
				continue;
			if (opcode == 0) {
				q->id = 0;
			} else if (opcode == 1) {
				q->width = w[0];
				q->height = w[1];
			} else if (opcode == 2) {
				q->x = w[0];
				q->y = w[1];
				q->w = w[2];
				q->h = w[3];
			} else if (opcode == 3) {
				q->anchor = w[0];
			} else if (opcode == 4) {
				q->gravity = w[0];
			} else if (opcode == 6) {
				q->dx = w[0];
				q->dy = w[1];
			}
		}
		if (opcode == 0)
			delete_id(id);
		break;
	case T_POPUP:
		if (opcode == 0) {
			if (popup_of(o->surface) && popup_of(o->surface)->id == id)
				popup_gone(popup_of(o->surface));
			delete_id(id);
		}
		break;
	case T_TOPLEVEL:
		if (opcode == 0) {
			delete_id(id);
		} else if (opcode >= 9 && opcode <= 12 && o->surface < MAX_OBJECTS &&
			   objects[o->surface].configured) {
			/* set and unset maximized, set and unset full-screen: it
			 * is told what it is, which it waits for. Full-screen
			 * until it is undone: a browser going full-screen asks to
			 * be maximized no more, and was told it was not full-screen
			 * either, and gave up on it two seconds later. */
			static int said;
			static const char *const names[] = { "maximized", "not maximized",
							     "full-screen", "not full-screen" };

			if (said < 40 && ++said)
				host_log("gx: wayland: the window asks to be %s\n", names[opcode - 9]);
			if (opcode == 11 || opcode == 12)
				objects[o->surface].full_screen = opcode == 11;
			configure_window(&objects[o->surface]);
		}
		break;
	case T_SUBSURFACE:
		if (opcode == 0 || opcode == 1) {
			struct popup *p = popup_of(o->surface);

			if (p && p->id == id && opcode == 0) {
				popup_gone(p);
			} else if (p && p->id == id) {   /* set_position, in its parent */
				p->x = p->from_x + (int32_t)w[0];
				p->y = p->from_y + (int32_t)w[1];
			}
		}
		if (opcode == 0)
			delete_id(id);
		break;
	case T_REGION:
		if (opcode == 0)
			delete_id(id);
		break;
	case T_OUTPUT:
		if (opcode == 0)
			delete_id(id);
		break;
	case T_DATA_MANAGER:
		create(w[0], opcode == 0 ? T_DATA_SOURCE : T_DATA_DEVICE, o->version);
		break;
	case T_DATA_SOURCE:
		if (opcode == 0) {            /* offer: a kind of data it can give */
			const char *mime = (const char *)&w[1];

			/* The first kind of plain text is the one we will ask for. */
			if (!o->attached && !strncmp(mime, "text/plain", 10) &&
			    w[0] <= sizeof(source_mime[0])) {
				o->attached = 1;
				memcpy(source_mime[id % 8], mime, w[0]);
				source_mime_id[id % 8] = id;
			}
		} else if (opcode == 1) {
			if (id == selection)
				selection = 0;
			delete_id(id);
		}
		break;
	case T_DATA_DEVICE:
		if (opcode == 1) {            /* set_selection: something was copied */
			if (selection && selection != w[0] && selection < MAX_OBJECTS &&
			    objects[selection].type == T_DATA_SOURCE)
				event(selection, 2, "");   /* cancelled */
			selection = w[0];
			selection_mime[0] = 0;
			if (selection && source_mime_id[selection % 8] == selection)
				strcpy(selection_mime, source_mime[selection % 8]);
			selection_changes++;
		} else if (opcode == 2) {
			delete_id(id);
		}
		break;
	case T_DECORATION_MANAGER:
		if (opcode == 0) {
			delete_id(id);
		} else if (opcode == 1) {     /* get_toplevel_decoration */
			struct object *toplevel = w[1] < MAX_OBJECTS ? &objects[w[1]] : &objects[0];

			create(w[0], T_DECORATION, 1)->surface = toplevel->surface;
			event(w[0], 0, "u", 2);   /* configure: the server's side */
			if (toplevel->surface < MAX_OBJECTS && objects[toplevel->surface].configured)
				event(objects[toplevel->surface].xdg, 0, "u", serial++);
		}
		break;
	case T_DECORATION:
		if (opcode == 0) {
			delete_id(id);
		} else {                      /* set_mode, unset_mode: ours all the same */
			event(id, 0, "u", 2);
			if (o->surface < MAX_OBJECTS && objects[o->surface].configured)
				event(objects[o->surface].xdg, 0, "u", serial++);
		}
		break;
	}
}

/* ---------- what the user does ---------- */

static void edit_start(void);

/*
 * Text reaches the client as the keys of a keyboard, because chromium-shell
 * takes it no other way: it replaces its input method with a dummy, so the
 * text-input protocols an on-screen keyboard would use go unanswered. The key
 * map we hand out is the standard US one, which is what turns these numbers
 * (Linux's own for each key) back into characters. Characters that keyboard
 * does not have are dropped.
 */
static void press_key(uint32_t key, int shift, uint32_t ms)
{
	if (!keyboard_id || !keyboard_inside || !has_keymap)
		return;
	if (shift) {
		event(keyboard_id, 3, "uuuu", serial++, ms, 42, 1);     /* left shift */
		event(keyboard_id, 4, "uuuuu", serial++, 1, 0, 0, 0);   /* modifiers */
	}
	event(keyboard_id, 3, "uuuu", serial++, ms, key, 1);
	event(keyboard_id, 3, "uuuu", serial++, ms, key, 0);
	if (shift) {
		event(keyboard_id, 3, "uuuu", serial++, ms, 42, 0);
		event(keyboard_id, 4, "uuuuu", serial++, 0, 0, 0, 0);
	}
}

/* A key with Control held. */
static void press_control(uint32_t key, uint32_t ms)
{
	if (!keyboard_id || !keyboard_inside || !has_keymap)
		return;
	event(keyboard_id, 3, "uuuu", serial++, ms, 29, 1);     /* left control */
	event(keyboard_id, 4, "uuuuu", serial++, 4, 0, 0, 0);
	event(keyboard_id, 3, "uuuu", serial++, ms, key, 1);
	event(keyboard_id, 3, "uuuu", serial++, ms, key, 0);
	event(keyboard_id, 3, "uuuu", serial++, ms, 29, 0);
	event(keyboard_id, 4, "uuuuu", serial++, 0, 0, 0, 0);
}

static void type_text(const char *text, uint32_t ms)
{
	/* The keys of a US keyboard, by the character each gives alone and
	 * with shift, in the order of their numbers from 2 on. */
	static const char plain[] = "1234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./";
	static const char shifted[] = "!@#$%^&*()_+\0\0QWERTYUIOP{}\0\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";

	for (; *text; text++) {
		if (*text == ' ') {
			press_key(57, 0, ms);
			continue;
		}
		for (int i = 0; i < (int)sizeof(plain) - 1; i++) {
			if (plain[i] == *text || shifted[i] == *text) {
				press_key(2 + i, plain[i] != *text, ms);
				break;
			}
		}
	}
}

/* Where the window has its address: chromium-shell's, right of its buttons,
 * unless the host says otherwise (wl_address_bar()). */
#define ADDRESS_FOCUS_NS 200000000ULL

static int address_left = 296, address_top, address_right = WL_SCREEN_WIDTH, address_bottom = 42;

void wl_address_bar(int left, int top, int right, int bottom)
{
	address_left = left;
	address_top = top;
	address_right = right;
	address_bottom = bottom;
}

static uint64_t address_touched;
static int edit_is_address;

static void input(const struct cdp_input *in, uint32_t ms)
{
	if (!main_surface)
		return;
	switch (in->type) {
	case CDP_TOUCH_DOWN:
		/* On the popup that is there, the topmost one, or on the window. */
		touch_surface = main_surface;
		touch_dx = touch_dy = 0;
		for (int i = 0; i < MAX_POPUPS; i++) {
			const struct popup *p = &popups[i];

			if (p->id && p->width > 0 && in->x >= p->x && in->x < p->x + p->width &&
			    in->y >= p->y && in->y < p->y + p->height) {
				touch_surface = p->surface;
				touch_dx = p->x;
				touch_dy = p->y;
			}
		}
		if (touch_id) {
			event(touch_id, 0, "uuuiff", serial++, ms, touch_surface, 0, in->x - touch_dx,
			      in->y - touch_dy);
			event(touch_id, 3, "");   /* frame */
		}
		break;
	case CDP_TOUCH_MOVE:
		if (touch_id) {
			event(touch_id, 2, "uiff", ms, 0, in->x - touch_dx, in->y - touch_dy);
			event(touch_id, 3, "");
		}
		break;
	case CDP_TOUCH_UP:
		if (touch_id) {
			event(touch_id, 1, "uui", serial++, ms, 0);
			event(touch_id, 3, "");
		}
		/* A finger on the address: the keyboard, once the window has
		 * made the address what keys go to. The page says when its own
		 * text is touched (cdp.c); the window's has no one to say it,
		 * but is always in the same place. */
		if (touch_surface == main_surface && in->y >= address_top && in->y < address_bottom &&
		    in->x >= address_left && in->x < address_right)
			address_touched = host_now_ns();
		break;
	case CDP_BACK:
	case CDP_RELOAD:
		cdp_page(in->type);
		break;
	case CDP_SCROLL:
		if (pointer_id) {
			if (!pointer_inside) {
				pointer_inside = 1;
				event(pointer_id, 0, "uuff", serial++, main_surface,
				      WL_SCREEN_WIDTH / 2, WL_SCREEN_HEIGHT / 2);
			}
			event(pointer_id, 4, "uuf", ms, 0, in->y);   /* axis: vertical */
			if (objects[pointer_id].version >= 5)
				event(pointer_id, 5, "");                /* frame */
		}
		break;
	case CDP_TEXT:
		type_text(in->text, ms);
		break;
	case CDP_KEY:
		press_key(in->x, 0, ms);
		break;
	case CDP_ACTIVATE:
		/* Text is edited, anything else gets the Enter key; the page
		 * knows which (cdp.c), and so does the main loop a moment
		 * later. Enter if there is no asking. */
		if (!cdp_activate())
			press_key(28, 0, ms);
		break;
	case CDP_KEYBOARD:
		/* The user asks for the keyboard: to go somewhere, so on the
		 * address, reached the way a finger reaches it. */
		if (touch_id) {
			event(touch_id, 0, "uuuiff", serial++, ms, main_surface, 0,
			      (address_left + address_right) / 2, (address_top + address_bottom) / 2);
			event(touch_id, 3, "");
			event(touch_id, 1, "uui", serial++, ms, 0);
			event(touch_id, 3, "");
			address_touched = host_now_ns();
		}
		break;
	case CDP_ENTER:
		press_key(28, 0, ms);
		break;
	case CDP_BACKSPACE:
		press_key(14, 0, ms);
		break;
	case CDP_QUIT:
		wl_close();
		break;
	}
}

/*
 * Leaving. The window is asked to close, as a click on its cross would: a
 * browser then writes what it had not got round to writing (its cookies, a
 * page's storage, where it was) and ends by itself. It is given a while, and
 * the program is ended without it if it does not.
 */
static int close_wanted, client_gone;

void wl_close(void)
{
	__atomic_store_n(&close_wanted, 1, __ATOMIC_SEQ_CST);
}

int wl_closed(void)
{
	return __atomic_load_n(&client_gone, __ATOMIC_SEQ_CST);
}

static void leave(void)
{
	if (gx_trace) {
		gmem_report();
		gx_report_kernel_memory();
	}
	host_exit(0);
}

/*
 * Editing the text of a field with the on-screen keyboard. The keyboard has
 * to start from what the field holds, and nothing tells us what that is; but
 * the client will say if asked the way a user would: select everything (so
 * that what is typed afterwards replaces it), copy, and the text is on the
 * clipboard, which is ours to read. A field that is empty, or that does not
 * let itself be copied (a password), simply gives nothing to start from.
 */
enum { EDIT_IDLE, EDIT_COPYING, EDIT_READING };
#define EDIT_COPY_NS 250000000ULL   /* a copy takes 45 ms under qemu */

static int edit_state, edit_pipe = -1;
static uint64_t edit_since;
static uint32_t edit_changes;
static struct cdp_input edited;
static size_t edited_len;

static void edit_start(void)
{
	uint32_t ms = (uint32_t)(host_now_ns() / 1000000);

	if (edit_state != EDIT_IDLE)
		return;
	edit_state = EDIT_COPYING;
	edit_since = host_now_ns();
	edit_changes = selection_changes;
	edited_len = 0;
	press_control(30, ms);   /* A: select all */
	press_control(46, ms);   /* C: copy */
}

/* The text is known (or there is none): over to the user. */
static void edit_finish(void)
{
	if (edit_pipe >= 0)
		lkl_sys_close(edit_pipe);
	edit_pipe = -1;
	edit_state = EDIT_IDLE;
	edited.text[edited_len] = 0;
	flush();
	if (host_ask_text(edited.text, sizeof(edited.text))) {
		type_text(edited.text, (uint32_t)(host_now_ns() / 1000000));
		/* An address is typed to go there. */
		if (edit_is_address) {
			press_key(28, 0, (uint32_t)(host_now_ns() / 1000000));
			flush();
			cdp_page(CDP_NAVIGATE);   /* and the keys are the page's again */
		}
	}
	edit_is_address = 0;
}

static void edit_step(uint64_t now)
{
	/* The caller's clock may be from just before the edit started. */
	if (now < edit_since)
		now = edit_since;
	if (edit_state == EDIT_COPYING) {
		int ends[2];

		if (selection_changes == edit_changes) {
			/* Nothing was copied, as far as we can wait to tell: an
			 * empty field gives no sign, and the user is waiting. */
			if (now - edit_since > EDIT_COPY_NS)
				edit_finish();
			return;
		}
		if (gx_trace)
			host_log("gx: wayland: copied after %lu ms\n",
				 (unsigned long)((now - edit_since) / 1000000));
		if (!selection || !selection_mime[0] || lkl_sys_pipe(ends)) {
			edit_finish();
			return;
		}
		/* wl_data_source.send: write it there, please. */
		flush();
		event(selection, 1, "s", selection_mime);
		flush_with_fd(ends[1]);
		lkl_sys_close(ends[1]);
		edit_pipe = ends[0];
		edit_state = EDIT_READING;
		edit_since = now;
	} else if (edit_state == EDIT_READING) {
		struct lkl_pollfd pfd = { .fd = edit_pipe, .events = LKL_POLLIN };

		while (lkl_sys_poll(&pfd, 1, 0) > 0) {
			long n = lkl_sys_read(edit_pipe, edited.text + edited_len,
					      sizeof(edited.text) - 1 - edited_len);

			if (n <= 0 || edited_len + n >= sizeof(edited.text) - 1) {
				edited_len += n > 0 ? n : 0;
				edit_finish();
				return;
			}
			edited_len += n;
		}
		if (now - edit_since > 1500000000ULL)
			edit_finish();
	}
}

/* ---------- the connection ---------- */

static uint8_t in_buf[1 << 16];
static size_t in_len;

/* Reads what the client sent, descriptors included. Returns 0 when it left. */
static int receive(void)
{
	struct wl_iovec iov = { in_buf + in_len, sizeof(in_buf) - in_len };
	struct wl_cmsg control;
	struct wl_msghdr msg = { NULL, 0, &iov, 1, &control, sizeof(control), 0 };
	long params[6] = { client, (long)&msg, 0x40000000 /* MSG_CMSG_CLOEXEC */ };
	long n = lkl_syscall(__lkl__NR_recvmsg, params);

	if (n <= 0)
		return 0;
	in_len += n;
	if (msg.control_len >= 16 && control.level == 1 /* SOL_SOCKET */ &&
	    control.type == 1 /* SCM_RIGHTS */) {
		int count = (control.len - 16) / 4;

		for (int i = 0; i < count && i < 28; i++) {
			if (fd_count == 64) {
				lkl_sys_close(control.fds[i]);
				continue;
			}
			received_fds[(fd_head + fd_count++) % 64] = control.fds[i];
		}
	}
	while (in_len >= 8) {
		uint32_t *msg_words = (uint32_t *)in_buf;
		uint32_t size = msg_words[1] >> 16;

		if (size < 8 || size > sizeof(in_buf)) {
			in_len = 0;   /* not Wayland: nothing to make of the rest */
			break;
		}
		if (size > in_len)
			break;
		request(msg_words[0], msg_words[1] & 0xffff, msg_words + 2, size / 4 - 2);
		memmove(in_buf, in_buf + size, in_len - size);
		in_len -= size;
	}
	return 1;
}

static int listener = -1;

static void wl_thread(void *arg)
{
	long params[6] = { listener, 0, 0 };

	client = lkl_syscall(__lkl__NR_accept, params);
	if (client < 0) {
		host_log("gx: wayland: no client (%d)\n", client);
		return;
	}
	host_log("gx: wayland: a client is here\n");
	create(1, T_DISPLAY, 1);
	for (;;) {
		struct lkl_pollfd pfd = { .fd = client, .events = LKL_POLLIN };
		struct cdp_input in;
		uint64_t now;

		/* Short waits: frames are paced and the user looked at in between. */
		if (lkl_sys_poll(&pfd, 1, 4) > 0 && !receive())
			break;
		now = host_now_ns();
		while (host_input(&in))
			input(&in, (uint32_t)(now / 1000000));
		if (address_touched && now - address_touched > ADDRESS_FOCUS_NS) {
			address_touched = 0;
			edit_is_address = 1;
			edit_start();
		}
		switch (cdp_wanted()) {
		case CDP_KEYBOARD:
			edit_start();
			break;
		case CDP_ENTER:
			press_key(28, 0, (uint32_t)(now / 1000000));
			break;
		}
		edit_step(now);
		frame_tick(now);
		if (__atomic_load_n(&close_wanted, __ATOMIC_SEQ_CST) && !closing_since) {
			struct object *window = &objects[main_surface < MAX_OBJECTS ? main_surface : 0];

			closing_since = now ? now : 1;
			if (window->toplevel)
				event(window->toplevel, 1, "");   /* xdg_toplevel.close */
		}
		if (closing_since && now - closing_since > CLOSE_NS) {
			host_log("gx: wayland: the client did not close by itself\n");
			leave();
		}
		flush();
	}
	host_log("gx: wayland: the client left after %d pictures\n", frames);
	__atomic_store_n(&client_gone, 1, __ATOMIC_SEQ_CST);
	if (closing_since)
		leave();
}

int wl_start(void)
{
	struct {
		uint16_t family;
		char path[108];
	} address = { 1 /* AF_UNIX */, SOCKET_PATH };
	long params[6] = { 1, 1 /* SOCK_STREAM */, 0 };

	listener = lkl_syscall(__lkl__NR_socket, params);
	if (listener < 0)
		return -1;
	params[0] = listener;
	params[1] = (long)&address;
	params[2] = sizeof(address.family) + sizeof(SOCKET_PATH);
	if (lkl_syscall(__lkl__NR_bind, params))
		return -1;
	params[1] = 1;
	if (lkl_syscall(__lkl__NR_listen, params))
		return -1;
	return host_thread_start(wl_thread, NULL);
}
