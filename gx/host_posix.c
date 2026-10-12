/*
 * host_posix.c - the host layer and entry point for an aarch64 Linux host
 * (qemu-user on a PC). This is the development loop: the same core is meant
 * to run on Horizon with another host layer.
 *
 * usage: gx [-t] [-n] [-b] [-o <guest file>=<host file>] <sysroot> <program> [args...]
 *   <sysroot> is a host directory copied to the root of the LKL filesystem,
 *   or an ext4 image mounted read-only as that root (what a large sysroot
 *   needs: a copy lives in memory); <program> is a path inside it.
 *   -t traces the guest's syscalls; -n gives the kernel a network (nat.c);
 *   -o <guest file>=<host file> copies a file out when the guest exits;
 *   -c <n> tells the guest it has n processors (one otherwise);
 *   -e <name>=<value> adds a variable to the guest's environment;
 *   -w is a Wayland display for a Chromium with a window (wl.c), shown and
 *   used the way -b is;
 *   -b drives a headless Chromium (cdp.c): the pictures of the page go to the
 *   directory GX_FRAMES as JPEG files, and the user is the script GX_INPUT.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <lkl.h>
#include <lkl_host.h>
#include "gx.h"
#include "nat.h"
#include "cdp.h"
#include "wl.h"
#include "glwin.h"
#include "video.h"

static void export_file(void);
static const char *export_spec;

/* ---------- memory ---------- */

static char *reserved;

/* None here, unless GX_HOLES asks for the kind the Switch has, to try them:
 * 4 GB taken out at 6 GB, and 30 GB at 40 GB. */
int host_reserved_hole(int index, void **addr, size_t *len)
{
	static const size_t holes[][2] = { { 6UL << 30, 4UL << 30 }, { 40UL << 30, 30UL << 30 } };

	if (!getenv("GX_HOLES") || index >= 2)
		return 0;
	*addr = reserved + holes[index][0];
	*len = holes[index][1];
	return 1;
}

void *host_reserve(size_t *len)
{
	/* GX_ARENA_GB makes the arena smaller, to see how a program takes a
	 * host with less address space (the Switch). */
	const char *limit = getenv("GX_ARENA_GB");
	void *p;

	if (limit && (size_t)atol(limit) << 30 < *len)
		*len = (size_t)atol(limit) << 30;
	p = mmap(NULL, *len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	reserved = p == MAP_FAILED ? NULL : p;
	return reserved;
}

/* Linux would allow it, Horizon does not: refusing it here too keeps what
 * runs on the PC honest about what can run on the Switch. */
static int writable_and_executable(int prot)
{
	return (prot & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC);
}

/* The pieces memory was committed in: on a host that keeps a record or two
 * per piece and has a limit to them (Horizon), what a program costs. Each
 * page remembers the piece it came with (GX_REPORT_EVERY says how many pieces
 * are left, a piece cut in two by what was given back counting twice). */
static uint32_t *piece_of;
static uint32_t pieces_made;

static void note_pieces(void *addr, size_t len, int commit)
{
	uint32_t id = commit ? __atomic_add_fetch(&pieces_made, 1, __ATOMIC_RELAXED) : 0;
	size_t first = ((uint8_t *)addr - (uint8_t *)reserved) / 4096;

	if (!getenv("GX_REPORT_EVERY") || !reserved)
		return;
	if (!piece_of) {
		void *p = mmap(NULL, (256UL << 30) / 4096 * sizeof(*piece_of), PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

		if (p == MAP_FAILED)
			return;
		piece_of = p;
	}
	for (size_t i = 0; i < len / 4096; i++)
		piece_of[first + i] = id;
}

static unsigned long pieces_left(void)
{
	unsigned long count = 0;
	uint32_t last = 0;

	for (size_t i = 0; piece_of && i < (48UL << 30) / 4096; i++) {
		count += piece_of[i] && piece_of[i] != last;
		last = piece_of[i];
	}
	return count;
}

int host_commit(void *addr, size_t len, int prot)
{
	note_pieces(addr, len, 1);
	return writable_and_executable(prot) ? -1 : mprotect(addr, len, prot);
}

int host_protect(void *addr, size_t len, int prot)
{
	return writable_and_executable(prot) ? -1 : mprotect(addr, len, prot);
}

int host_decommit(void *addr, size_t len)
{
	/* A fresh mapping drops the pages, so the next commit reads zeroes. */
	void *p;

	note_pieces(addr, len, 0);
	p = mmap(addr, len, PROT_NONE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);

	return p == MAP_FAILED ? -1 : 0;
}

/* Linux has no need of them, but the Switch does and what runs here should
 * run there: the same size, unless GX_SHARED_CHUNK_MB says otherwise (0 gives
 * each file its own piece). */
size_t host_shared_chunk(void)
{
	const char *mb = getenv("GX_SHARED_CHUNK_MB");

	return mb ? (size_t)atol(mb) << 20 : 16UL << 20;
}

int host_shared_new(size_t len)
{
	int fd = memfd_create("gx-shared", MFD_CLOEXEC);

	if (fd >= 0 && ftruncate(fd, len)) {
		close(fd);
		fd = -1;
	}
	return fd;
}

int host_shared_map(int handle, void *addr, size_t len, int prot)
{
	return mmap(addr, len, prot, MAP_SHARED | MAP_FIXED, handle, 0) == MAP_FAILED ? -1 : 0;
}

void host_shared_unmap(int handle, void *addr, size_t len) { host_decommit(addr, len); }

void host_shared_free(int handle) { close(handle); }

void host_flush_icache(void *addr, size_t len)
{
	__builtin___clear_cache((char *)addr, (char *)addr + len);
}

/* ---------- threads ---------- */

struct start {
	void (*fn)(void *);
	void *arg;
};

/* Faults are handled on a stack of their own: the one the thread is on may
 * be what cannot be written to (gmem_watch()). */
static void fault_stack(void)
{
	stack_t stack = { .ss_size = 64 << 10 };

	stack.ss_sp = malloc(stack.ss_size);
	if (stack.ss_sp)
		sigaltstack(&stack, NULL);
}

/* GX_LIGHT_FORK=1: as on a host that cannot do this (the Switch, so far), to
 * see here what a program does there. */
int host_write_faults(void)
{
	static int light = -1;

	if (light < 0)
		light = getenv("GX_LIGHT_FORK") != NULL;
	return !light;
}

/* Stopping a thread in the guest's code: it is sent a signal, and its handler
 * says where the thread was and, if that was the guest's code, does not
 * return until told. One at a time. */
static volatile int stop_answer;
static volatile uint64_t stop_round;   /* goes up each time the stopped threads are let go */
static volatile int stop_only_says;    /* the signal asks where the thread is, no more */
static void put_hex(const char *label, uint64_t v);
extern char __executable_start[], etext[];

static void on_stop(int sig, siginfo_t *info, void *uctx)
{
	ucontext_t *uc = uctx;
	uint64_t base;
	int in_guest = gmem_describe(uc->uc_mcontext.pc, &base) != 0;
	uint64_t round = __atomic_load_n(&stop_round, __ATOMIC_SEQ_CST);

	(void)sig;
	(void)info;
	if (stop_only_says) {
		/* Where it is, and what of gx called that, innermost first:
		 * the words of its stack that are addresses of gx's code. */
		const uint64_t *sp = (const uint64_t *)uc->uc_mcontext.sp;
		uint64_t from = (uint64_t)__executable_start, to = (uint64_t)etext;

		put_hex(in_guest ? "    in the guest at " : "    in the host at  ", uc->uc_mcontext.pc - (in_guest ? base : 0));
		for (int i = 0, said = 0; !in_guest && i < 600 && said < 10; i++) {
			if (sp[i] >= from && sp[i] < to) {
				put_hex("      gx + ", sp[i] - from);
				said++;
			}
		}
		__atomic_store_n(&stop_answer, 1, __ATOMIC_SEQ_CST);
		return;
	}
	__atomic_store_n(&stop_answer, in_guest ? 2 : 1, __ATOMIC_SEQ_CST);
	while (in_guest && __atomic_load_n(&stop_round, __ATOMIC_SEQ_CST) == round) {
		/* The thread pointer is the guest's: nothing of the C
		 * library's here, the bare syscall (nanosleep). */
		struct timespec ts = { 0, 200000 };
		register long x8 __asm__("x8") = 101;
		register long x0 __asm__("x0") = (long)&ts;
		register long x1 __asm__("x1") = 0;

		__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
	}
}

int host_thread_stop(uint64_t thread)
{
	__atomic_store_n(&stop_answer, 0, __ATOMIC_SEQ_CST);
	if (pthread_kill((pthread_t)thread, SIGUSR2))
		return 0;
	while (!__atomic_load_n(&stop_answer, __ATOMIC_SEQ_CST))
		sched_yield();
	return stop_answer == 2;
}

void host_threads_go(void)
{
	__atomic_add_fetch(&stop_round, 1, __ATOMIC_SEQ_CST);
}

static void *thread_start(void *p)
{
	struct start s = *(struct start *)p;

	free(p);
	fault_stack();
	s.fn(s.arg);
	return NULL;
}

int host_thread_start(void (*fn)(void *), void *arg)
{
	struct start *s = malloc(sizeof(*s));
	pthread_attr_t attr;
	pthread_t t;
	int ret;

	s->fn = fn;
	s->arg = arg;
	pthread_attr_init(&attr);
	/* Host functions called for the guest (a GL driver) run on it too. */
	pthread_attr_setstacksize(&attr, 8 << 20);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	ret = pthread_create(&t, &attr, thread_start, s);
	pthread_attr_destroy(&attr);
	return ret;
}

void host_thread_exit(void) { pthread_exit(NULL); }
void host_yield(void) { sched_yield(); }

uint64_t host_thread_self(void) { return (uint64_t)pthread_self(); }

uint64_t host_thread_cpu_ns(uint64_t thread)
{
	struct timespec ts = { 0 };
	clockid_t clock;

	if (pthread_getcpuclockid((pthread_t)thread, &clock) || clock_gettime(clock, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int profile_mounted;

void host_exit(int code)
{
	export_file();
	if (profile_mounted) {
		/* What is still in the kernel's memory goes to the image. */
		long none[6] = { 0 };

		lkl_syscall(__lkl__NR_sync, none);
	}
	fflush(NULL);
	_exit(code);
}

struct host_lock { pthread_mutex_t m; };
struct host_cond { pthread_cond_t c; };

struct host_lock *host_lock_new(void)
{
	struct host_lock *l = malloc(sizeof(*l));

	pthread_mutex_init(&l->m, NULL);
	return l;
}

void host_lock_acquire(struct host_lock *l) { pthread_mutex_lock(&l->m); }
void host_lock_release(struct host_lock *l) { pthread_mutex_unlock(&l->m); }

struct host_cond *host_cond_new(void)
{
	struct host_cond *c = malloc(sizeof(*c));
	pthread_condattr_t attr;

	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&c->c, &attr);
	return c;
}

int host_cond_wait(struct host_cond *c, struct host_lock *l, int64_t ns)
{
	struct timespec ts;

	if (ns < 0)
		return pthread_cond_wait(&c->c, &l->m) ? 1 : 0;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	ts.tv_sec += ns / 1000000000LL;
	ts.tv_nsec += ns % 1000000000LL;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000L;
	}
	return pthread_cond_timedwait(&c->c, &l->m, &ts) == ETIMEDOUT;
}

void host_cond_signal(struct host_cond *c) { pthread_cond_signal(&c->c); }

/* ---------- console, logging ---------- */

void host_console_write(int fd, const void *buf, size_t len)
{
	const char *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);

		if (n <= 0)
			break;
		p += n;
		len -= n;
	}
}

void host_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 1)
		n = sizeof(buf) - 1;
	host_console_write(2, buf, n);
}

void host_random(void *buf, size_t len)
{
	for (size_t done = 0; done < len;) {
		ssize_t n = getrandom((char *)buf + done, len - done, 0);

		if (n > 0)
			done += n;
	}
}

uint64_t host_now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

uint64_t host_hwcap(int which)
{
	/* HWCAP_CPUID (bit 11) would make glibc read ID registers with mrs,
	 * which only a kernel that emulates them can answer. */
	return which ? getauxval(AT_HWCAP2) : (getauxval(AT_HWCAP) & ~(1UL << 11));
}

/* ---------- host functions for the guest: EGL and GLES ---------- */

/*
 * The guest's libEGL and libGLESv2 are thunks (guest/hostlib.c) to the host's.
 * Here the host's are Mesa, without a window system: the window surface of
 * the guest becomes an off-screen buffer of the size of the Switch's screen,
 * and GX_FRAME=<file> saves each frame the guest presents as a PPM image.
 */
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_WINDOW_BIT 0x0004
#define EGL_NONE 0x3038
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define SCREEN_WIDTH 1280
#define SCREEN_HEIGHT 720

static pthread_once_t gl_once = PTHREAD_ONCE_INIT;
static void *egl_lib, *gles_lib;

static void gl_open(void)
{
	egl_lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	gles_lib = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
	if (!egl_lib || !gles_lib)
		fprintf(stderr, "gx: no EGL or GLES on this host (%s)\n", dlerror());
}

static void *gl_real(const char *name)
{
	void *fn = egl_lib ? dlsym(egl_lib, name) : NULL;

	return fn || !gles_lib ? fn : dlsym(gles_lib, name);
}

static void *gl_get_display(void *native)
{
	void *(*get)(unsigned, void *, const intptr_t *) = gl_real("eglGetPlatformDisplay");

	return get(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
}

static void *gl_get_platform_display(unsigned platform, void *native, const intptr_t *attribs)
{
	return gl_get_display(NULL);
}

/* The guest asks for configurations it can make a window with. */
static unsigned gl_choose_config(void *dpy, const int32_t *attribs, void **configs,
				 int32_t size, int32_t *count)
{
	unsigned (*choose)(void *, const int32_t *, void **, int32_t, int32_t *) =
		gl_real("eglChooseConfig");
	int32_t copy[128];
	int n = 0;

	for (; attribs && attribs[n] != EGL_NONE && n < 126; n += 2) {
		copy[n] = attribs[n];
		copy[n + 1] = attribs[n + 1];
		if (copy[n] == EGL_SURFACE_TYPE && (copy[n + 1] & EGL_WINDOW_BIT))
			copy[n + 1] = (copy[n + 1] & ~EGL_WINDOW_BIT) | EGL_PBUFFER_BIT;
	}
	copy[n] = EGL_NONE;
	return choose(dpy, copy, configs, size, count);
}

/* And when it asks what a configuration can do, one that can make an
 * off-screen buffer can make "a window", since that is what a window is here. */
static unsigned gl_get_config_attrib(void *dpy, void *config, int32_t attribute, int32_t *value)
{
	unsigned (*get)(void *, void *, int32_t, int32_t *) = gl_real("eglGetConfigAttrib");
	unsigned ret = get(dpy, config, attribute, value);

	if (ret && attribute == EGL_SURFACE_TYPE && (*value & EGL_PBUFFER_BIT))
		*value |= EGL_WINDOW_BIT;
	return ret;
}

static void save_frame(const char *path, void *dpy, void *surface);

/* The screen, for glwin.c: an off-screen buffer of its size. */
static void *gl_screen(void *dpy, void *config)
{
	static const int32_t screen[] = {
		EGL_WIDTH, SCREEN_WIDTH, EGL_HEIGHT, SCREEN_HEIGHT, EGL_NONE,
	};
	void *(*create)(void *, void *, const int32_t *) = gl_real("eglCreatePbufferSurface");

	return create(dpy, config, screen);
}

static void gl_presented(void *dpy, void *screen)
{
	const char *path = getenv("GX_FRAME");

	if (path)
		save_frame(path, dpy, screen);
}

static const struct glwin_host gl_host = { gl_real, gl_screen, gl_presented };

/* The guest's windows are glwin.c's: it puts them together on the screen, and
 * the surfaces the guest has for them are its names for that. */
static void *gl_create_window_surface(void *dpy, void *config, void *window, const void *attribs)
{
	return glwin_create(dpy, config, window);
}

static unsigned gl_make_current(void *dpy, void *draw, void *read, void *ctx)
{
	return glwin_make_current(dpy, draw, read, ctx) > 0;
}

static void *gl_get_current_surface(int32_t which)
{
	return glwin_current_surface(which);
}

static unsigned gl_surface_attrib(void *dpy, void *surface, int32_t attribute, int32_t value)
{
	unsigned (*set)(void *, void *, int32_t, int32_t) = gl_real("eglSurfaceAttrib");

	return set(dpy, glwin_surface(surface), attribute, value);
}

static unsigned gl_destroy_surface(void *dpy, void *surface)
{
	unsigned (*destroy)(void *, void *) = gl_real("eglDestroySurface");
	int done = glwin_destroy(dpy, surface);

	return done < 0 ? destroy(dpy, surface) : (unsigned)done;
}

static unsigned gl_query_surface(void *dpy, void *surface, int32_t attribute, int32_t *value)
{
	unsigned (*query)(void *, void *, int32_t, int32_t *) = gl_real("eglQuerySurface");
	int done = glwin_query(dpy, surface, attribute, value);

	return done < 0 ? query(dpy, surface, attribute, value) : (unsigned)done;
}

static void save_frame(const char *path, void *dpy, void *surface)
{
	unsigned (*query)(void *, void *, int32_t, int32_t *) = gl_real("eglQuerySurface");
	void (*get_integer)(unsigned, int32_t *) = gl_real("glGetIntegerv");
	void (*bind)(unsigned, unsigned) = gl_real("glBindFramebuffer");
	void (*read_pixels)(int32_t, int32_t, int32_t, int32_t, unsigned, unsigned, void *) =
		gl_real("glReadPixels");
	int32_t width = 0, height = 0, bound = 0;
	char tmp[1024];
	uint8_t *pixels;
	FILE *f;

	if (!query(dpy, surface, EGL_WIDTH, &width) || !query(dpy, surface, EGL_HEIGHT, &height) ||
	    width <= 0 || height <= 0 || !(pixels = malloc((size_t)width * height * 4)))
		return;
	get_integer(GL_FRAMEBUFFER_BINDING, &bound);
	bind(GL_FRAMEBUFFER, 0);
	read_pixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	bind(GL_FRAMEBUFFER, bound);

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	f = fopen(tmp, "wb");
	if (f) {
		fprintf(f, "P6\n%d %d\n255\n", width, height);
		for (int y = height - 1; y >= 0; y--)   /* GL rows go upwards */
			for (int x = 0; x < width; x++)
				fwrite(pixels + ((size_t)y * width + x) * 4, 1, 3, f);
		fclose(f);
		rename(tmp, path);
	}
	free(pixels);
}

/* The driver would call the guest's function from the host's side of things,
 * which cannot be done (see gx_hostcall): it gets no messages. */
static void gl_debug_message_callback(void *callback, const void *param)
{
}

static unsigned gl_swap_buffers(void *dpy, void *surface)
{
	unsigned (*swap)(void *, void *) = gl_real("eglSwapBuffers");
	int done = glwin_swap(dpy, surface);

	return done < 0 ? swap(dpy, surface) : (unsigned)done;
}

static unsigned gl_swap_buffers_with_damage(void *dpy, void *surface, const int32_t *rects, int32_t count)
{
	return gl_swap_buffers(dpy, surface);
}

/* ---------- host functions for the guest: sound ---------- */

/*
 * What the guest's libasound (guest/asound.c) plays. There is nothing to hear
 * here: the samples go to the file GX_AUDIO names, if it names one, and are
 * counted as played at the pace a sound card would play them.
 */
static pthread_mutex_t audio_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *audio_file;
static long audio_rate;
static uint64_t audio_written, audio_loud, audio_start_ns;

static long audio_queued_locked(void)
{
	uint64_t played = audio_rate ? (host_now_ns() - audio_start_ns) * audio_rate / 1000000000ULL : 0;

	if (played >= audio_written) {
		/* All of it is played: what comes next starts from now. */
		audio_start_ns = host_now_ns() - audio_written * 1000000000ULL / (audio_rate ? audio_rate : 1);
		return 0;
	}
	return audio_written - played;
}

static long gx_audio_open(long rate, long channels)
{
	const char *path = getenv("GX_AUDIO");

	if (channels != 2 || rate <= 0)
		return -1;
	pthread_mutex_lock(&audio_lock);
	audio_rate = rate;
	audio_written = 0;
	audio_start_ns = host_now_ns();
	if (path && !audio_file)
		audio_file = fopen(path, "wb");
	pthread_mutex_unlock(&audio_lock);
	fprintf(stderr, "gx: audio: %ld Hz\n", rate);
	return 0;
}

static long gx_audio_write(const int16_t *frames, long count)
{
	pthread_mutex_lock(&audio_lock);
	audio_queued_locked();
	audio_written += count;
	for (long i = 0; i < count * 2; i++)
		audio_loud += frames[i] > 256 || frames[i] < -256;
	if (audio_file)
		fwrite(frames, 4, count, audio_file);
	pthread_mutex_unlock(&audio_lock);
	return count;
}

static long gx_audio_queued(void)
{
	long queued;

	pthread_mutex_lock(&audio_lock);
	queued = audio_queued_locked();
	pthread_mutex_unlock(&audio_lock);
	return queued;
}

static void gx_audio_drop(void)
{
	pthread_mutex_lock(&audio_lock);
	audio_start_ns = host_now_ns() - audio_written * 1000000000ULL / (audio_rate ? audio_rate : 1);
	pthread_mutex_unlock(&audio_lock);
}

static void gx_audio_close(void)
{
	pthread_mutex_lock(&audio_lock);
	if (audio_file)
		fflush(audio_file);
	pthread_mutex_unlock(&audio_lock);
	fprintf(stderr, "gx: audio: closed after %lu frames, %lu samples that were not silence\n",
		(unsigned long)audio_written, (unsigned long)audio_loud);
}

static void *audio_symbol(const char *name)
{
	static const struct {
		const char *name;
		void *fn;
	} ours[] = {
		{ "gx_audio_open", gx_audio_open },   { "gx_audio_write", gx_audio_write },
		{ "gx_audio_queued", gx_audio_queued }, { "gx_audio_drop", gx_audio_drop },
		{ "gx_audio_close", gx_audio_close },
	};

	for (size_t i = 0; i < sizeof(ours) / sizeof(ours[0]); i++)
		if (!strcmp(name, ours[i].name))
			return ours[i].fn;
	return NULL;
}

void *host_symbol(const char *name)
{
	static const struct {
		const char *name;
		void *fn;
	} ours[] = {
		{ "eglGetDisplay", gl_get_display },
		{ "eglGetPlatformDisplay", gl_get_platform_display },
		{ "eglGetPlatformDisplayEXT", gl_get_platform_display },
		{ "eglChooseConfig", gl_choose_config },
		{ "eglGetConfigAttrib", gl_get_config_attrib },
		{ "eglCreateWindowSurface", gl_create_window_surface },
		{ "eglCreatePlatformWindowSurface", gl_create_window_surface },
		{ "eglCreatePlatformWindowSurfaceEXT", gl_create_window_surface },
		{ "eglSwapBuffers", gl_swap_buffers },
		{ "eglSwapBuffersWithDamageKHR", gl_swap_buffers_with_damage },
		{ "eglSwapBuffersWithDamageEXT", gl_swap_buffers_with_damage },
		{ "eglDestroySurface", gl_destroy_surface },
		{ "eglQuerySurface", gl_query_surface },
		{ "eglMakeCurrent", gl_make_current },
		{ "eglGetCurrentSurface", gl_get_current_surface },
		{ "eglSurfaceAttrib", gl_surface_attrib },
		{ "glDebugMessageCallback", gl_debug_message_callback },
		{ "glDebugMessageCallbackKHR", gl_debug_message_callback },
	};
	void *(*get_proc)(const char *);
	void *fn;

	if (!strncmp(name, "gx_audio_", 9))
		return audio_symbol(name);
	if (!strncmp(name, "gx_video_", 9))
		return video_symbol(name);
	if (strncmp(name, "egl", 3) && strncmp(name, "gl", 2))
		return NULL;
	pthread_once(&gl_once, gl_open);
	glwin_init(&gl_host);
	if (!egl_lib || !gles_lib)
		return NULL;
	for (size_t i = 0; i < sizeof(ours) / sizeof(ours[0]); i++)
		if (!strcmp(name, ours[i].name))
			return ours[i].fn;
	fn = gl_real(name);
	if (!fn && (get_proc = dlsym(egl_lib, "eglGetProcAddress")))
		fn = get_proc(name);
	return fn;
}

/* ---------- files out of the guest ---------- */

/* export_spec is the argument of -o: <guest file>=<host file>. */

/* The guest's files live in the kernel and die with it: this is how one of
 * them (a screenshot) gets out. */
static void export_file(void)
{
	static char buf[1 << 16];
	char guest[512];
	const char *eq = export_spec ? strchr(export_spec, '=') : NULL;
	int in, out;
	long n;

	if (!eq || (size_t)(eq - export_spec) >= sizeof(guest))
		return;
	memcpy(guest, export_spec, eq - export_spec);
	guest[eq - export_spec] = 0;
	in = lkl_sys_open(guest, LKL_O_RDONLY, 0);
	if (in < 0) {
		fprintf(stderr, "gx: the guest left no %s\n", guest);
		return;
	}
	out = open(eq + 1, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	while (out >= 0 && (n = lkl_sys_read(in, buf, sizeof(buf))) > 0)
		if (write(out, buf, n) != n)
			break;
	if (out >= 0)
		close(out);
}

/* ---------- a screen and a user for Chromium (cdp.c) ---------- */

/* There is no screen here: each picture becomes a file, which is enough to
 * see what a Switch would show. */
void host_frame(const uint8_t *jpeg, size_t len)
{
	static int count;
	const char *dir = getenv("GX_FRAMES");
	char path[1024];
	FILE *f;

	count++;
	if (count % 50 == 0 && getenv("GX_REPORT"))
		gx_report_threads();
	if (!dir)
		return;
	snprintf(path, sizeof(path), "%s/frame-%04d.jpg", dir, count);
	f = fopen(path, "wb");
	if (f) {
		fwrite(jpeg, 1, len, f);
		fclose(f);
	}
}

/* The on-screen keyboard: here it says what it was given to edit, and always
 * answers the words of GX_TEXT. */
int host_ask_text(char *text, size_t room)
{
	const char *answer = getenv("GX_TEXT");

	fprintf(stderr, "gx: keyboard asked to edit \"%s\"\n", text);
	if (!answer)
		return 0;
	snprintf(text, room, "%s", answer);
	return 1;
}

/* The same for the window of a Chromium that has one (wl.c): the pixels as
 * they are, in files a viewer opens (PPM). Not all of them: they are large. */
void host_present(const uint8_t *pixels, int width, int height, int stride)
{
	static int count;
	const char *dir = getenv("GX_FRAMES");
	char path[1024];
	FILE *f;

	count++;
	if (!dir || (count > 40 && count % 20))
		return;
	snprintf(path, sizeof(path), "%s/frame-%04d.ppm", dir, count);
	f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", width, height);
	for (int y = 0; y < height; y++) {
		const uint8_t *row = pixels + (size_t)y * stride;

		for (int x = 0; x < width; x++) {
			fputc(row[x * 4 + 2], f);
			fputc(row[x * 4 + 1], f);
			fputc(row[x * 4], f);
		}
	}
	fclose(f);
}

/*
 * The user is a script, one action per line, each with the time it happens
 * at in milliseconds after the first look:
 *   <ms> tap <x> <y> | drag <x1> <y1> <x2> <y2> | scroll <dy> | back | reload
 *   <ms> nav <url> | text <words> | enter | quit
 */
int host_input(struct cdp_input *in)
{
	static struct cdp_input queue[16];
	static int queued, next, done;
	static FILE *script;
	static struct timespec start;
	static long due = -1;
	static char action[600];
	struct timespec now;
	long elapsed;
	int x1, y1, x2, y2;

	if (next < queued) {
		*in = queue[next++];
		return 1;
	}
	if (done)
		return 0;
	if (!script) {
		const char *path = getenv("GX_INPUT");

		script = path ? fopen(path, "r") : NULL;
		if (!script) {
			done = 1;
			return 0;
		}
		clock_gettime(CLOCK_MONOTONIC, &start);
	}
	if (due < 0) {
		char line[700];

		if (!fgets(line, sizeof(line), script)) {
			done = 1;
			return 0;
		}
		line[strcspn(line, "\r\n")] = 0;
		if (sscanf(line, "%ld %599[^\n]", &due, action) != 2) {
			due = -1;
			return 0;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	elapsed = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000;
	if (elapsed < due)
		return 0;
	due = -1;
	queued = next = 0;
	memset(queue, 0, sizeof(queue));
	if (sscanf(action, "tap %d %d", &x1, &y1) == 2) {
		queue[queued++] = (struct cdp_input){ CDP_TOUCH_DOWN, x1, y1, "" };
		queue[queued++] = (struct cdp_input){ CDP_TOUCH_UP, x1, y1, "" };
	} else if (sscanf(action, "drag %d %d %d %d", &x1, &y1, &x2, &y2) == 4) {
		queue[queued++] = (struct cdp_input){ CDP_TOUCH_DOWN, x1, y1, "" };
		for (int i = 1; i <= 8; i++)
			queue[queued++] = (struct cdp_input){ CDP_TOUCH_MOVE, x1 + (x2 - x1) * i / 8,
							      y1 + (y2 - y1) * i / 8, "" };
		queue[queued++] = (struct cdp_input){ CDP_TOUCH_UP, x2, y2, "" };
	} else if (sscanf(action, "scroll %d", &y1) == 1) {
		queue[queued++] = (struct cdp_input){ CDP_SCROLL, 0, y1, "" };
	} else if (!strcmp(action, "back")) {
		queue[queued++].type = CDP_BACK;
	} else if (!strcmp(action, "reload")) {
		queue[queued++].type = CDP_RELOAD;
	} else if (sscanf(action, "key %d", &x1) == 1) {
		queue[queued++] = (struct cdp_input){ CDP_KEY, x1, 0, "" };
	} else if (!strcmp(action, "activate")) {
		queue[queued++].type = CDP_ACTIVATE;
	} else if (!strcmp(action, "enter")) {
		queue[queued++].type = CDP_ENTER;
	} else if (!strcmp(action, "edit")) {
		queue[queued++].type = CDP_KEYBOARD;
	} else if (!strcmp(action, "backspace")) {
		queue[queued++].type = CDP_BACKSPACE;
	} else if (!strcmp(action, "quit")) {
		queue[queued++].type = CDP_QUIT;
	} else if (!strncmp(action, "nav ", 4) || !strncmp(action, "text ", 5)) {
		int nav = action[0] == 'n';

		queue[queued].type = nav ? CDP_NAVIGATE : CDP_TEXT;
		strncpy(queue[queued].text, action + (nav ? 4 : 5), sizeof(queue[queued].text) - 1);
		queued++;
	}
	return host_input(in);
}

/* ---------- crash report ---------- */

static void put_hex(const char *label, uint64_t v)
{
	char buf[64];
	int n = 0;

	while (label[n]) {
		buf[n] = label[n];
		n++;
	}
	for (int shift = 60; shift >= 0; shift -= 4)
		buf[n++] = "0123456789abcdef"[(v >> shift) & 15];
	buf[n++] = '\n';
	write(2, buf, n);
}

/* Runs on a guest thread pointer: nothing here may touch host TLS. */
static void on_fault(int sig, siginfo_t *info, void *uctx)
{
	ucontext_t *uc = uctx;
	uint64_t pc = uc->uc_mcontext.pc, base = 0;
	int kind = gmem_describe(pc, &base);

	if (sig == SIGSEGV) {
		/* Memory that is writable or executable by turns (gmem.c)? On
		 * the host's thread pointer to find out: this may be the guest
		 * running. */
		uint64_t tp, addr = (uint64_t)info->si_addr;
		int fixed;

		__asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
		for (int i = 0; i < GX_MAX_THREADS; i++)
			if (gx_tp_tab[i].guest_tp == tp)
				__asm__ volatile("msr tpidr_el0, %0" : : "r"(gx_tp_tab[i].thread->host_tp));
		fixed = gmem_fault(addr, addr == pc);
		__asm__ volatile("msr tpidr_el0, %0" : : "r"(tp));
		if (fixed) {
			/* qemu-user does not say where a `dc zva` faulted but where
			 * the block of code it is in began, with the registers as
			 * they were at the fault. The C library's memset() has
			 * `add xN, xN, #64; dc zva, xN` at the top of its loop:
			 * run again from there, the add is done twice, 64 bytes
			 * are left as they were and 64 past the end are cleared,
			 * for every fault in the loop (memory committed when first
			 * used has one per unit). The add is stepped over. */
			if (kind && !(pc & 3) && gmem_describe(pc + 4, &base)) {
				uint32_t first = *(uint32_t *)pc, second = *(uint32_t *)(pc + 4);
				unsigned reg = second & 31;

				if ((second & ~31u) == 0xD50B7420u && reg < 31 &&
				    (first & 0xFFC003FFu) == (0x91000000u | (reg << 5) | reg) &&
				    uc->uc_mcontext.regs[reg] == addr)
					uc->uc_mcontext.pc = pc + 4;
			}
			return;
		}
	}
	if (sig == SIGILL && kind) {
		/* An instruction this processor does not have, that can be
		 * done otherwise (gmem.c)? */
		uint64_t tp;
		int fixed;

		__asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
		for (int i = 0; i < GX_MAX_THREADS; i++)
			if (gx_tp_tab[i].guest_tp == tp)
				__asm__ volatile("msr tpidr_el0, %0" : : "r"(gx_tp_tab[i].thread->host_tp));
		fixed = gmem_illegal(pc);
		__asm__ volatile("msr tpidr_el0, %0" : : "r"(tp));
		if (fixed)
			return;
	}
	put_hex("gx: fault, signal ", sig);
	put_hex("  address ", (uint64_t)info->si_addr);
	put_hex("  pc      ", pc);
	if (kind) {
		put_hex(kind == 2 ? "  image   " : "  arena   ", base);
		put_hex("  offset  ", pc - base);
	} else {
		write(2, "  (pc is in the host)\n", 22);
	}
	put_hex("  lr      ", uc->uc_mcontext.regs[30]);
	if (gmem_describe(uc->uc_mcontext.regs[30], &base) == 2)
		put_hex("  lr off  ", uc->uc_mcontext.regs[30] - base);
	put_hex("  sp      ", uc->uc_mcontext.sp);
	for (int i = 0; i < 9; i++)
		put_hex("  x       ", uc->uc_mcontext.regs[i]);
	if (kind) {
		/* Which thread of the guest, and who called what it was in. */
		const uint64_t *fp = (const uint64_t *)uc->uc_mcontext.regs[29];
		uint64_t guest_tp;

		__asm__ volatile("mrs %0, tpidr_el0" : "=r"(guest_tp));
		for (int i = 0; i < GX_MAX_THREADS; i++)
			if (gx_tp_tab[i].guest_tp == guest_tp && gx_tp_tab[i].thread)
				put_hex("  thread  ", gx_tp_tab[i].thread->tid);
		put_hex("  frame   ", (uint64_t)fp);
		for (int i = 0; i < 24 && fp && !((uint64_t)fp & 7) && gmem_describe((uint64_t)fp, &base) &&
				gmem_accessible((uint64_t)fp, 16, 0); i++) {
			if (gmem_describe(fp[1], &base) == 2)
				put_hex("  called from image + ", fp[1] - base);
			else
				put_hex("  called from ", fp[1]);
			if ((const uint64_t *)fp[0] <= fp)
				break;
			fp = (const uint64_t *)fp[0];
		}
	}
	if (kind && export_spec) {
		/* The guest died in its own code, so the kernel is in order:
		 * back on this thread's host thread pointer, what the guest
		 * was asked to leave can still be fetched. */
		struct gx_thread *self = gx_main_thread;
		uint64_t guest_tp;

		__asm__ volatile("mrs %0, tpidr_el0" : "=r"(guest_tp));
		for (int i = 0; i < GX_MAX_THREADS; i++)
			if (gx_tp_tab[i].guest_tp == guest_tp)
				self = gx_tp_tab[i].thread;
		__asm__ volatile("msr tpidr_el0, %0" : : "r"(self->host_tp));
		export_file();
		/* What the guest wrote before it fell (its own account of
		 * why, often) is worth having on the disk. */
		lkl_sys_sync();
	}
	_exit(128 + sig);
}

/* ---------- sysroot import ---------- */

static int import_tree(const char *host, const char *guest)
{
	DIR *d = opendir(host);
	struct dirent *e;
	static char buf[1 << 16];

	if (!d) {
		fprintf(stderr, "gx: cannot read %s\n", host);
		return -1;
	}
	while ((e = readdir(d))) {
		char hp[1024], gp[1024];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		snprintf(hp, sizeof(hp), "%s/%s", host, e->d_name);
		snprintf(gp, sizeof(gp), "%s/%s", guest, e->d_name);
		if (stat(hp, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			lkl_sys_mkdir(gp, 0755);
			import_tree(hp, gp);
		} else if (S_ISREG(st.st_mode)) {
			int in = open(hp, O_RDONLY);
			int out = lkl_sys_open(gp, LKL_O_WRONLY | LKL_O_CREAT | LKL_O_TRUNC, 0755);
			ssize_t n;

			if (in < 0 || out < 0) {
				fprintf(stderr, "gx: cannot import %s (%d)\n", hp, out);
				if (in >= 0)
					close(in);
				continue;
			}
			while ((n = read(in, buf, sizeof(buf))) > 0)
				lkl_sys_write(out, buf, n);
			close(in);
			lkl_sys_close(out);
		}
	}
	closedir(d);
	return 0;
}

/* ---------- network ---------- */

/* The first IPv4 resolver of the host, for the NAT to forward DNS to. */
static uint32_t host_resolver(void)
{
	FILE *f = fopen("/etc/resolv.conf", "r");
	struct in_addr addr = { 0 };
	char line[256], text[64];

	while (f && fgets(line, sizeof(line), f))
		if (sscanf(line, "nameserver %63s", text) == 1 && inet_aton(text, &addr))
			break;
	if (f)
		fclose(f);
	return addr.s_addr ? addr.s_addr : inet_addr("8.8.8.8");
}

/* The device has to exist before the kernel starts. */
static int net_add(void)
{
	uint8_t mac[6];
	struct lkl_netdev *nd = nat_create(host_resolver(), 0, mac);
	struct lkl_netdev_args args = { .mac = mac };
	int id = nd ? lkl_netdev_add(nd, &args) : -1;

	if (id < 0)
		fprintf(stderr, "gx: cannot add the network device (%d)\n", id);
	return id;
}

static int net_up(int id)
{
	int ifindex = lkl_netdev_get_ifindex(id);
	int ret;

	lkl_if_up(1); /* lo */
	if (ifindex < 0)
		return ifindex;
	ret = lkl_if_up(ifindex);
	if (!ret)
		ret = lkl_if_set_ipv4(ifindex, htonl(NAT_GUEST), NAT_PREFIX);
	if (!ret)
		ret = lkl_set_ipv4_gateway(htonl(NAT_GATEWAY));
	return ret;
}

/* ---------- the root filesystem ---------- */

static struct lkl_disk root_disk;
/* -p <image>: an ext4 image that is the guest's /root, where what a program
 * keeps from one run to the next goes (a browser's profile). Read and
 * written, unlike the root. */
static struct lkl_disk profile_disk;
static const char *profile_path;

/* A browser leaves these in its profile to say it is running, and takes them
 * away when it ends as it should; here it never does, it is ended. */
static void unlock_profile(void)
{
	lkl_sys_unlink("/root/profile/SingletonLock");
	lkl_sys_unlink("/root/profile/SingletonCookie");
	lkl_sys_unlink("/root/profile/SingletonSocket");
}

/* What a program expects around its files, whatever the root is made of. The
 * mount points have to exist already when the root is a read-only image. */
static void populate_root(void)
{
	static const char *const dirs[] = { "/proc", "/sys", "/dev", "/tmp", "/run", "/root" };
	static const struct {
		const char *path;
		int minor;
	} nodes[] = {
		{ "/dev/null", 3 }, { "/dev/zero", 5 }, { "/dev/full", 7 },
		{ "/dev/random", 8 }, { "/dev/urandom", 9 },
	};

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		lkl_sys_mkdir(dirs[i], 0755);
	lkl_sys_mount("proc", "/proc", "proc", 0, NULL);
	lkl_sys_mount("sysfs", "/sys", "sysfs", 0, NULL);
	/* "tmpfs" is ramfs in a kernel without an MMU. */
	lkl_sys_mount("tmpfs", "/dev", "tmpfs", 0, NULL);
	lkl_sys_mount("tmpfs", "/tmp", "tmpfs", 0, NULL);
	lkl_sys_mount("tmpfs", "/run", "tmpfs", 0, NULL);
	if (!profile_mounted)
		lkl_sys_mount("tmpfs", "/root", "tmpfs", 0, NULL);
	lkl_sys_mkdir("/dev/shm", 01777);
	for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
		lkl_sys_mknod(nodes[i].path, LKL_S_IFCHR | 0666, 1 << 8 | nodes[i].minor);
}

/* GX_REPORT_EVERY=<seconds>: says where every guest thread is, that often. */
static void *report_threads(void *arg)
{
	int seconds = atoi(arg);

	gx_report_idle = 1;
	for (;;) {
		sleep(seconds > 0 ? seconds : 10);
		gx_report_threads();
		gmem_report();
		gx_report_kernel_memory();
		host_log("gx: memory was committed in %u pieces, of which %lu are left\n", pieces_made,
			 pieces_left());
		if (!getenv("GX_REPORT_STACKS"))
			continue;
		/* And where each is in the host (addresses for addr2line). */
		stop_only_says = 1;
		for (int i = 0; i < GX_MAX_THREADS; i++) {
			const struct gx_thread *t = gx_tp_tab[i].thread;

			if (gx_tp_tab[i].guest_tp == ~0UL || !t || !t->host_id)
				continue;
			host_log("  [%d]\n", t->tid);
			stop_answer = 0;
			if (!pthread_kill((pthread_t)t->host_id, SIGUSR2))
				for (int tries = 0; !stop_answer && tries < 2000; tries++)
					usleep(500);
		}
		stop_only_says = 0;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	static char *guest_env[32] = {
		"PATH=/bin:/usr/bin", "HOME=/root", "LD_LIBRARY_PATH=/lib", "TMPDIR=/tmp",
	};
	int env_count = 4;
	struct sigaction sa = { 0 };
	struct stat root;
	int disk_id = -1, profile_id = -1;
	int arg = 1, net = 0, net_id = -1, browser = 0, wayland = 0, cpu_count = 0, ret;

	for (; arg < argc && argv[arg][0] == '-'; arg++) {
		if (!strcmp(argv[arg], "-t"))
			gx_trace = 1;
		else if (!strcmp(argv[arg], "-n"))
			net = 1;
		else if (!strcmp(argv[arg], "-b"))
			browser = 1;
		else if (!strcmp(argv[arg], "-w"))
			wayland = 1;
		else if (!strcmp(argv[arg], "-p") && arg + 1 < argc)
			profile_path = argv[++arg];
		else if (!strcmp(argv[arg], "-A") && arg + 1 < argc) {
			int l, t, r, b;

			if (sscanf(argv[++arg], "%d,%d,%d,%d", &l, &t, &r, &b) == 4)
				wl_address_bar(l, t, r, b);
		}
		else if (!strcmp(argv[arg], "-e") && arg + 1 < argc && env_count < 31)
			guest_env[env_count++] = argv[++arg];
		else if (!strcmp(argv[arg], "-c") && arg + 1 < argc)
			cpu_count = atoi(argv[++arg]);
		else if (!strcmp(argv[arg], "-o") && arg + 1 < argc)
			export_spec = argv[++arg];
		else
			break;
	}
	if (argc < arg + 2 || argv[arg][0] == '-') {
		fprintf(stderr, "usage: gx [-t] [-n] [-b] [-o <guest file>=<host file>] <sysroot> <program> [args...]\n");
		return 2;
	}

	if (stat(argv[arg], &root)) {
		fprintf(stderr, "gx: cannot read %s\n", argv[arg]);
		return 1;
	}
	ret = lkl_init(&lkl_host_ops);
	if (!ret && net && (net_id = net_add()) < 0)
		return 1;
	if (!ret && S_ISREG(root.st_mode)) {
		root_disk.fd = open(argv[arg], O_RDONLY);
		disk_id = root_disk.fd < 0 ? -1 : lkl_disk_add(&root_disk);
		if (disk_id < 0) {
			fprintf(stderr, "gx: cannot use %s as a disk (%d)\n", argv[arg], disk_id);
			return 1;
		}
	}
	if (!ret && profile_path) {
		profile_disk.fd = open(profile_path, O_RDWR);
		profile_id = profile_disk.fd < 0 ? -1 : lkl_disk_add(&profile_disk);
		if (profile_id < 0)
			fprintf(stderr, "gx: cannot use %s as a disk: nothing will be kept\n", profile_path);
	}
	if (!ret)
		ret = lkl_start_kernel("mem=512M loglevel=0");
	if (ret) {
		fprintf(stderr, "gx: the kernel did not start (%d)\n", ret);
		return 1;
	}
	/* The kernel has no clock to read the date from and starts in 1970,
	 * which no TLS certificate survives. */
	{
		struct timespec now;
		struct __lkl__kernel_timespec kts;
		long params[6] = { LKL_CLOCK_REALTIME, (long)&kts };

		clock_gettime(CLOCK_REALTIME, &now);
		kts.tv_sec = now.tv_sec;
		kts.tv_nsec = now.tv_nsec;
		ret = lkl_syscall(__lkl__NR_clock_settime, params);
		if (ret)
			fprintf(stderr, "gx: cannot set the kernel clock (%s)\n", lkl_strerror(ret));
	}
	if (net && (ret = net_up(net_id))) {
		fprintf(stderr, "gx: cannot configure the network (%s)\n", lkl_strerror(ret));
		return 1;
	}
	if (disk_id >= 0) {
		char mnt[64];

		ret = lkl_mount_dev(disk_id, 0, "ext4", LKL_MS_RDONLY, NULL, mnt, sizeof(mnt));
		if (!ret && profile_id >= 0) {
			/* Before the root is the root: its /root is where the
			 * profile goes, and the device's name cannot be made on a
			 * root that is read-only. */
			static const char node[] = "/gx-profile-disk";
			char target[96];
			uint32_t dev = 0;
			int err = lkl_get_virtio_blkdev(profile_id, 0, &dev);

			snprintf(target, sizeof(target), "%s/root", mnt);
			if (!err)
				err = lkl_sys_mknod(node, LKL_S_IFBLK | 0600, dev);
			if (!err)
				err = lkl_sys_mount((char *)node, target, "ext4", 0, NULL);
			if (err)
				fprintf(stderr, "gx: cannot mount %s (%s): nothing will be kept\n",
					profile_path, lkl_strerror(err));
			profile_mounted = !err;
		}
		if (!ret)
			ret = lkl_sys_chroot(mnt);
		if (!ret)
			ret = lkl_sys_chdir("/");
		if (ret) {
			fprintf(stderr, "gx: cannot mount %s (%s)\n", argv[arg], lkl_strerror(ret));
			return 1;
		}
	} else if (import_tree(argv[arg], "")) {
		return 1;
	}
	populate_root();
	if (profile_mounted)
		unlock_profile();
	if (cpu_count)
		gx_set_cpus(cpu_count);
	/* Descriptors 0-2 are the host console; keep the kernel from handing
	 * those numbers out. */
	for (int i = 0; i < 3; i++)
		lkl_sys_open("/tmp/.stdio", LKL_O_RDWR | LKL_O_CREAT, 0644);

	if (gmem_init()) {
		fprintf(stderr, "gx: cannot reserve the guest address space\n");
		return 1;
	}
	/* The pipe first: it wants the descriptors Chromium knows it by. */
	if (browser && cdp_start(wayland)) {
		fprintf(stderr, "gx: cannot set up the pipe to Chromium\n");
		return 1;
	}
	if (wayland && wl_start()) {
		fprintf(stderr, "gx: cannot open the Wayland display\n");
		return 1;
	}
	fault_stack();
	sa.sa_sigaction = on_fault;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGTRAP, &sa, NULL);
	sa.sa_sigaction = on_stop;
	sigaction(SIGUSR2, &sa, NULL);

	fflush(NULL);
	if (getenv("GX_PROCESSES"))
		gx_processes = 1;
	if (getenv("GX_MORE_ARGS")) {
		/* GX_MORE_ARGS="<end of a program's path> <argument>...": see gx_more_for. */
		static char more[1024];

		strncpy(more, getenv("GX_MORE_ARGS"), sizeof(more) - 1);
		gx_more_for = strtok(more, " ");
		for (char *word; gx_more_for && gx_more_count < 16 && (word = strtok(NULL, " "));)
			gx_more[gx_more_count++] = word;
	}
	if (getenv("GX_LAZY")) {
		gmem_lazy = 1;
		if (atoi(getenv("GX_LAZY")) > 1)
			gmem_lazy_pages = atoi(getenv("GX_LAZY"));   /* in pieces of so many pages */
		if (getenv("GX_EAGER_MB"))                       /* all but stacks and what is larger */
			gmem_eager_mapped = gmem_eager_protected = (uint64_t)atoi(getenv("GX_EAGER_MB")) << 20;
		if (getenv("GX_EAGER_KB"))
			gmem_eager_mapped = gmem_eager_protected = (uint64_t)atoi(getenv("GX_EAGER_KB")) << 10;
		if (getenv("GX_LAZY_WANTED"))
			gmem_lazy_wanted_only = 1;               /* and nothing of what is only reserved */
	}
	if (getenv("GX_POOL")) {
		/* GX_POOL=<n>: guest threads share at most n threads of the host. */
		gx_pool = 1;
		if (atoi(getenv("GX_POOL")) > 0)
			gx_pool_cap = atoi(getenv("GX_POOL"));
	}
	if (getenv("GX_REPORT_EVERY")) {
		pthread_t reporter;

		pthread_create(&reporter, NULL, report_threads, getenv("GX_REPORT_EVERY"));
	}
	gx_exec(argv[arg + 1], argc - arg - 1, argv + arg + 1, guest_env);
	return 1;
}
