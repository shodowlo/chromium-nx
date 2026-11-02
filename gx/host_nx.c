/*
 * host_nx.c - the host layer and entry point for Horizon (Nintendo Switch,
 * libnx). The LKL host operations are the ones of nro/source/nx_host.c.
 *
 * On the SD card:
 *   sdmc:/gx/sysroot/      copied to the root of the LKL filesystem
 *   sdmc:/gx/programs.txt  one command per line, offered as a menu, under the
 *                          name in brackets if it starts with one. Before the
 *                          program: -t traces the guest's syscalls, -b gives
 *                          a headless Chromium the screen and the controls
 *                          (cdp.c; the keys are at host_input()), -w is a
 *                          Wayland display for one with a window (wl.c), @<image>
 *                          takes an ext4 image of sdmc:/gx as the root instead
 *                          of the sysroot, ><file> copies a guest file to
 *                          sdmc:/gx when the guest exits
 *   sdmc:/gx/gx.log        everything gx and the guest print
 *
 * The screen is the text console until the guest creates an EGL window
 * surface, which takes the screen over; the console comes back when the guest
 * calls eglTerminate(). Meanwhile text only goes to the log.
 *
 * Guest memory. Horizon has no mmap: the only way to put memory at an address
 * of our choosing, and the only memory that can be made executable, is "code
 * memory", an alias of heap pages (svcMapProcessCodeMemory). So the arena is
 * a reserved range of addresses, and committing pages maps pages of a pool
 * taken from the heap. Protections follow the kernel's rules:
 *   - never writable and executable at once;
 *   - a page stays "code" (None, R or RX at will) until it is made writable,
 *     which turns it into "data" (None, R or RW at will) for good;
 *   - a data page becomes code again only by unmapping and remapping it,
 *     which keeps its contents but leaves a short window without the page.
 */
#include <arpa/inet.h>
#include <dirent.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <switch.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <png.h>
#include <turbojpeg.h>
#include <zlib.h>
#include <lkl.h>
#include <lkl_host.h>
#include "gx.h"
#include "nat.h"
#include "cdp.h"
#include "wl.h"
#include "glwin.h"
#include "video.h"
#include "overlay.h"
#include "keys.h"
#include <math.h>

/* Where everything is on the SD card: the root image next to the program, the
 * log, and programs.txt if there is to be a menu. */
#define GX_DIR "sdmc:/switch/chromium-nx"
/* What runs when there is no programs.txt: the whole browser with its window
 * (-w) and the debugging pipe (-b), drawing with the GPU, V8 with its
 * compilers. build-nx.sh writes the same line, with the flags of its build of
 * the image, in programs.txt. */
#define DEFAULT_COMMAND \
	"-w -b -c3 @chromium-full.ext4 =profile.ext4 -A225,46,1150,80 /usr/lib/chromium/chromium " \
	"--no-sandbox --single-process --no-zygote --ozone-platform=wayland --user-data-dir=/root/profile " \
	"--no-first-run --no-default-browser-check --disable-breakpad --disable-crashpad-for-testing " \
	"--test-type --password-store=basic --disable-background-networking --disable-component-update " \
	"--disk-cache-size=33554432 --media-cache-size=33554432 --disable-quic --hide-crash-restore-bubble " \
	"--enable-spatial-navigation --remote-debugging-pipe --disable-blink-features=AutomationControlled " \
	"--disable-features=OptimizationHints,OptimizationGuideModelDownloading,OptimizationGuideOnDeviceModel," \
	"OnDeviceModelBackgroundDownload,MediaRouter,DialMediaRouteProvider,BackForwardCache " \
	"--force-gpu-mem-available-mb=128 --force-gpu-mem-discardable-limit-mb=32 " \
	"--force-max-texture-size=4096 " \
	/* nouveau's GPU clock drifts: Chromium then sees no frame presented and \
	   leaves fullscreen after 1.5 s. */ \
	"--disable-gl-extensions=GL_EXT_disjoint_timer_query,GL_EXT_disjoint_timer_query_webgl2," \
	"GL_ARB_timer_query,GL_EXT_timer_query " \
	"--js-flags=--no-sparkplug " \
	"--use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist --disable-gpu-sandbox --in-process-gpu " \
	"https://www.google.com/"
/* The kernel's memory holds every file that is not on the root image (its
 * /tmp, a browser's profile and cache) and its network buffers: a browser on
 * a busy site went through 192 MB and the kernel panicked. With a profile
 * on a disk of its own and the page cache dropped when little is free
 * (gsys.c), 400 of 512 MB stayed free while a browser played videos. At
 * 384 MB, 200 to 300 stayed free, most of the rest a cache it can drop, while
 * the guest ran out of its 1.6 GB (a game of a page, then another site). At
 * 256, 130 stayed free and 40 to 60 could not be dropped (/tmp and shared
 * memory files): 192, and the guest has the difference.
 *
 * nohz=off: the kernel's tick keeps going when it has nothing to do. Without
 * it, each time it went idle and back (several times a syscall when threads
 * take turns) it stopped and restarted its tick, each a wake of the host
 * thread that is its timer: two threads making syscalls at once paid 55 us a
 * call and 35 with it, and a page animating 400 boxes went from 27 to 38
 * pictures a second. -xh in programs.txt starts the kernel without it. */
#define KERNEL_MEM_MB 192
#define STR_(x) #x
#define STR(x) STR_(x)
#define KERNEL_ARGS "mem=" STR(KERNEL_MEM_MB) "M loglevel=4 nohz=off"
#define KERNEL_ARGS_NOHZ "mem=" STR(KERNEL_MEM_MB) "M loglevel=4"
static const size_t kernel_mem = (size_t)KERNEL_MEM_MB << 20;
static size_t peak_host_heap;
extern int lkl_blk_sync;   /* gx/lkl/virtio_blk.c */
/* Left to malloc for stacks (a megabyte a thread, eighty threads), libnx, the
 * GPU driver and the video decoder, whose pictures are here too: with 256 MB a
 * third 1080p video in a row left no stack for a new thread, and a game of a
 * page (WebGL) then a video had the host use 320 MB and its shaders fail at
 * 300. YouTube, going from page to page with a video decoder open (60 MB
 * each) and a preview opening another, went to 470 MB and lost the GPU at
 * 448. Less when the homebrew runs as an applet and the whole heap is a few
 * hundred megabytes. */
#define HEAP_KEPT (512UL << 20)
#define HEAP_KEPT_SMALL (48UL << 20)
#define POOL_WORTH_IT (192UL << 20)
#define POOL_MAX ((2048UL - 16) << 20)   /* see host_reserve() */
/* Host functions called for the guest (the GL driver) run on these too. */
#define MAIN_STACK (4UL << 20)
#define THREAD_STACK (1UL << 20)
/* The only priority Horizon time-slices: guest threads may spin. */
#define GUEST_PRIO 0x3B

static Mutex print_lock, mem_lock, screen_lock;
static FILE *log_file;
static volatile int finished;
/* Something died that the guest cannot go on without: the main thread then
 * says so on the screen, where the browser's last picture would otherwise
 * stay as if it had frozen, and ends the program. ran_out: memory was
 * refused before it, the likely reason. */
static volatile int crashed, ran_out;
static volatile int console_on = 1;   /* changed under screen_lock */

/*
 * The console and whoever takes the screen from it. The console draws under
 * screen_lock, and only under it: when libnx fails to get a buffer to draw in
 * (which happens, for a reason still unknown), the thread that draws stops
 * for good inside libnx, lock in hand, and nothing else may depend on it.
 */
static void console_off(void)
{
	int locked = 0;

	for (int i = 0; i < 50 && !(locked = mutexTryLock(&screen_lock)); i++)
		svcSleepThread(10000000ULL);
	if (!locked) {
		/* The thread that draws is stuck: take the window as it is. */
		console_on = 0;
		nwindowReleaseBuffers(nwindowGetDefault());
		return;
	}
	if (console_on) {
		console_on = 0;
		consoleExit(NULL);
	}
	mutexUnlock(&screen_lock);
}

static void console_back(void)
{
	if (!mutexTryLock(&screen_lock))
		return;
	if (!console_on) {
		consoleInit(NULL);
		console_on = 1;
	}
	mutexUnlock(&screen_lock);
}

/* ---------- console, logging ---------- */

/* Where the thread that has print_lock is, for the watchdog to tell when it
 * never lets go: 1 the console, 2 the log, 3 the card. */
static volatile int print_stage;

void host_console_write(int fd, const void *buf, size_t len)
{
	mutexLock(&print_lock);
	print_stage = 1;
	/* The console only under screen_lock: libnx takes a buffer of the
	 * screen when text is drawn as well as in consoleUpdate(), and two
	 * threads doing so at once stopped both for good, this one with the
	 * log in hand and every thread of the kernel waiting on it. Not for
	 * long either: the text is in the log anyway. */
	if (console_on) {
		int locked = 0;

		for (int i = 0; i < 20 && !(locked = mutexTryLock(&screen_lock)); i++)
			svcSleepThread(1000000ULL);
		if (locked) {
			if (console_on)
				fwrite(buf, 1, len, stdout);
			mutexUnlock(&screen_lock);
		}
	}
	print_stage = 2;
	if (log_file) {
		static u64 synced;
		u64 now = armGetSystemTick();

		/* What is not committed to the card is lost if the console is
		 * switched off or the program is killed. */
		static u64 started;

		fwrite(buf, 1, len, log_file);
		/* Every line while things start: a start that hangs is seen
		 * once in a while, and its last lines are what tells where.
		 * After, a quarter of a second's lines at once (and all of them
		 * at the end of each of the watchdog's reports, and when
		 * something dies: sync_log()): a line at a time, the report
		 * every ten seconds was eighty writes to the card in a row,
		 * and a video stopped for a quarter of a second each time. */
		if (!started)
			started = now;
		if (armTicksToNs(now - started) < 20000000000ULL ||
		    armTicksToNs(now - synced) > (gx_trace ? 20000000ULL : 250000000ULL)) {
			print_stage = 3;
			fflush(log_file);
			fsync(fileno(log_file));
			synced = now;
		}
	}
	print_stage = 0;
	mutexUnlock(&print_lock);
}

static void sync_log(void)
{
	mutexLock(&print_lock);
	if (log_file) {
		fflush(log_file);
		fsync(fileno(log_file));
	}
	mutexUnlock(&print_lock);
}

void host_log(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int n;

	static u64 started;
	int at = 0;

	/* The lines of our own say when, in seconds from the start. */
	if (!started)
		started = armGetSystemTick();
	if (fmt[0] == 'g' && fmt[1] == 'x' && fmt[2] == ':') {
		u64 ms = armTicksToNs(armGetSystemTick() - started) / 1000000;

		at = snprintf(buf, sizeof(buf), "[%4lu.%03lu] ", (unsigned long)(ms / 1000),
			      (unsigned long)(ms % 1000));
	}
	va_start(ap, fmt);
	n = at + vsnprintf(buf + at, sizeof(buf) - at, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(buf) - 1)
		n = sizeof(buf) - 1;
	host_console_write(2, buf, n);
}

/* ---------- memory ---------- */

#define PG_MAPPED 0x80
#define PG_DATA 0x40   /* was made writable: no longer "code" for the kernel */
/* The protection the kernel has for the page, which it wants uniform over
 * the range of a call; a page just mapped has one of its own. */
#define PG_PERM 0x07
enum { P_FRESH, P_NONE, P_R, P_RW, P_RX };

static uint8_t *arena;
static size_t shared_budget;   /* what __libnx_initheap() left out of the heap */
/* What is known of each arena page, in leaves made as the arena fills: it is
 * tens of gigabytes of addresses, nearly all of them never used. */
#define LEAF_PAGES 1024
struct leaf {
	uint32_t src[LEAF_PAGES];      /* pool page behind each arena page */
	uint8_t flags[LEAF_PAGES];
};
static struct leaf **leaves;

static uint8_t flags_at(size_t p)
{
	struct leaf *l = leaves[p / LEAF_PAGES];

	return l ? l->flags[p % LEAF_PAGES] : 0;
}

static uint32_t src_at(size_t p)
{
	struct leaf *l = leaves[p / LEAF_PAGES];

	return l ? l->src[p % LEAF_PAGES] : 0;
}

static void set_flags(size_t p, size_t n, uint8_t flags)
{
	for (; n; p++, n--)
		if (leaves[p / LEAF_PAGES])
			leaves[p / LEAF_PAGES]->flags[p % LEAF_PAGES] = flags;
}

static int set_page(size_t p, uint32_t src, uint8_t flags)
{
	struct leaf **l = &leaves[p / LEAF_PAGES];

	if (!*l && !(*l = calloc(1, sizeof(**l))))
		return -1;
	(*l)->src[p % LEAF_PAGES] = src;
	(*l)->flags[p % LEAF_PAGES] = flags;
	return 0;
}
static uint8_t *pool, *pool_used;
static size_t pool_pages, pool_hint, pool_free;
static Handle self;

/*
 * Where the arena goes. The address space of the process is 512 GB with a
 * Homebrew Loader that asks for 39 bits (64 GB otherwise), but the kernel puts
 * the regions it keeps for the heap, the stacks and its aliases anywhere in
 * it, and things are mapped here and there: no single free run is sure to be
 * large enough for a program like Chromium. So the arena is a window of the
 * address space, the one with the most free addresses in it, and what is not
 * free in the window is reported to the core as holes.
 */
#define MAX_SPANS 64
#define SPAN_MARGIN (16UL << 20)   /* left free around what others mapped */
#define SPAN_MIN (256UL << 20)

static struct span {
	u64 start, end;
} spans[MAX_SPANS];                /* the free runs of the window, in order */
static int span_count;
static u64 window_start, window_end;

/* Lists the free runs of the address space where code memory may go. */
static void find_spans(void)
{
	enum { ALIAS, HEAP, STACK, ASLR, REGIONS };
	static const InfoType types[REGIONS][2] = {
		{ InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
		{ InfoType_HeapRegionAddress, InfoType_HeapRegionSize },
		{ InfoType_StackRegionAddress, InfoType_StackRegionSize },
		{ InfoType_AslrRegionAddress, InfoType_AslrRegionSize },
	};
	u64 start[REGIONS] = { 0 }, len[REGIONS] = { 0 };
	u64 addr, end;

	for (int i = 0; i < REGIONS; i++) {
		svcGetInfo(&start[i], types[i][0], CUR_PROCESS_HANDLE, 0);
		svcGetInfo(&len[i], types[i][1], CUR_PROCESS_HANDLE, 0);
	}
	addr = start[ASLR];
	end = start[ASLR] + len[ASLR];
	while (addr < end) {
		MemoryInfo info;
		u32 page_info;
		u64 from, to;

		if (R_FAILED(svcQueryMemory(&info, &page_info, addr)) || !info.size)
			break;
		from = addr;
		to = info.addr + info.size < end ? info.addr + info.size : end;
		addr = info.addr + info.size;
		if (info.type != MemType_Unmapped)
			continue;
		/* The free block, cut at each region it runs into. */
		while (from < to) {
			u64 stop = to, next = to, lo, hi;

			for (int i = 0; i < ASLR; i++) {
				if (from >= start[i] && from < start[i] + len[i]) {
					stop = from;              /* inside one: skip it */
					next = start[i] + len[i];
				} else if (start[i] > from && start[i] < stop) {
					stop = start[i];
					next = start[i] + len[i];
				}
			}
			lo = (from + SPAN_MARGIN + 0x1fffff) & ~0x1fffffUL;
			hi = stop > SPAN_MARGIN ? (stop - SPAN_MARGIN) & ~0x1fffffUL : 0;
			if (hi > lo && hi - lo >= SPAN_MIN && span_count < MAX_SPANS) {
				spans[span_count].start = lo;
				spans[span_count].end = hi;
				span_count++;
			}
			from = next > from ? next : to;
		}
	}
}

/* Keeps the window of at most len bytes with the most free addresses. */
static void choose_window(u64 len)
{
	u64 best = 0, best_start = 0, total = 0;
	int kept = 0;

	for (int i = 0; i < span_count; i++) {
		u64 free_bytes = 0;

		for (int k = i; k < span_count && spans[k].start < spans[i].start + len; k++)
			free_bytes += (spans[k].end < spans[i].start + len ? spans[k].end
									   : spans[i].start + len) -
				      spans[k].start;
		if (free_bytes > best) {
			best = free_bytes;
			best_start = spans[i].start;
		}
	}
	window_start = best_start;
	window_end = best_start;
	for (int i = 0; i < span_count; i++) {
		if (spans[i].start < best_start || spans[i].start >= best_start + len)
			continue;
		spans[kept] = spans[i];
		if (spans[kept].end > best_start + len)
			spans[kept].end = best_start + len;
		window_end = spans[kept].end;
		total += spans[kept].end - spans[kept].start;
		kept++;
	}
	span_count = kept;
	host_log("gx: %lu GB of addresses free in %d runs, between %#lx and %#lx\n",
		 (unsigned long)(total >> 30), span_count, (unsigned long)window_start,
		 (unsigned long)window_end);
}

/* The holes are what lies between the free runs of the window. */
int host_reserved_hole(int index, void **addr, size_t *len)
{
	if (index + 1 >= span_count)
		return 0;
	*addr = (void *)spans[index].end;
	*len = spans[index + 1].start - spans[index].end;
	return 1;
}

void *host_reserve(size_t *len)
{
	size_t size = 0, kept = HEAP_KEPT;
	void *addr;

	find_spans();
	choose_window(*len);
	if (!span_count)
		return NULL;
	/* So that libnx maps nothing of its own where the guest will be. */
	virtmemLock();
	for (int i = 0; i < span_count; i++)
		virtmemAddReservation((void *)spans[i].start, spans[i].end - spans[i].start);
	virtmemUnlock();
	addr = (void *)window_start;
	*len = window_end - window_start;
	arena = addr;
	leaves = calloc(*len / GX_PAGE / LEAF_PAGES + 1, sizeof(*leaves));

	/* The pool is all the heap but what the host keeps for itself, by
	 * count: what the heap has not handed out yet, less what is kept. Not
	 * by asking for pool and kept at once, as before, nor by asking for
	 * what is kept after the pool: newlib's malloc refuses 2 GB or more
	 * (which held the pool to 1648 MB when the heap had 1940 for it), and
	 * says yes to sizes it does not have. */
	{
		extern char *fake_heap_start, *fake_heap_end;
		struct mallinfo mi = mallinfo();
		size_t heap = fake_heap_end - fake_heap_start;
		size_t free_now = heap > mi.uordblks ? heap - mi.uordblks : 0;

		if (free_now < kept + POOL_WORTH_IT)
			kept = HEAP_KEPT_SMALL;
		size = free_now > kept ? (free_now - kept) & ~((16UL << 20) - 1) : 0;
		if (size > POOL_MAX)
			size = POOL_MAX;
		for (; size >= (32UL << 20) && !pool; size -= 16UL << 20)
			pool = memalign(GX_PAGE, size);
		if (pool)
			size += 16UL << 20;   /* the loop went one step past it */
	}
	if (!leaves || !pool) {
		host_log("gx: not enough memory for the guest\n");
		return NULL;
	}
	pool_pages = pool_free = size / GX_PAGE;
	pool_used = calloc(pool_pages, 1);
	if (!pool_used)
		return NULL;
	self = envGetOwnProcessHandle();
	{
		extern char *fake_heap_start, *fake_heap_end;
		struct mallinfo mi = mallinfo();

		host_log("gx: %lu MB for the guest, %lu MB for shared memory (heap %lu MB, "
			 "%lu MB of it in use; pool at %p, heap at %p)\n",
			 (unsigned long)(size >> 20), (unsigned long)(shared_budget >> 20),
			 (unsigned long)((fake_heap_end - fake_heap_start) >> 20),
			 (unsigned long)(mi.uordblks >> 20), pool, fake_heap_start);
	}
	return addr;
}

static size_t page_of(void *addr) { return ((uint8_t *)addr - arena) / GX_PAGE; }

/* Finds free pool pages, n at most and in one piece. Large pieces come from
 * the top of the pool, at places that are multiples of their usual size, and
 * small ones from the bottom: memory that is given back in pieces of every
 * size otherwise leaves the pool, and with it the heap it is made of, cut
 * into thousands of runs the kernel keeps a record of each. */
#define POOL_LARGE 64   /* pages: the unit data is committed by (gmem.c) */

static size_t pool_take(size_t n, size_t *first)
{
	if (n >= POOL_LARGE && n <= pool_pages) {
		/* All of it in one run, the highest there is; in pieces as for
		 * small ones if the pool has no such run left. */
		for (size_t p = (pool_pages - n) & ~(size_t)(POOL_LARGE - 1);; p -= POOL_LARGE) {
			if (!pool_used[p] && !pool_used[p + n - 1] && !memchr(&pool_used[p], 1, n)) {
				memset(&pool_used[p], 1, n);
				pool_free -= n;
				*first = p;
				return n;
			}
			if (p < POOL_LARGE)
				break;
		}
	}
	/* A small piece: the lowest run that holds all of it. Taking whatever
	 * came first, a page here and three there, made one mapping of every
	 * crumb: a browser had 1300 of them after a while, two records each. */
	while (pool_hint < pool_pages && pool_used[pool_hint])
		pool_hint++;
	for (size_t p = pool_hint; p + n <= pool_pages;) {
		const uint8_t *used = memchr(&pool_used[p], 1, n);

		if (!used) {
			memset(&pool_used[p], 1, n);
			pool_free -= n;
			*first = p;
			return n;
		}
		/* On to the next free page after what is in the way. */
		p = used - pool_used + 1;
		while (p < pool_pages && pool_used[p])
			p++;
	}
	/* No such run: in pieces, lowest first. */
	for (size_t p = pool_hint; p < pool_pages; p++) {
		size_t run = 0;

		if (pool_used[p])
			continue;
		while (run < n && p + run < pool_pages && !pool_used[p + run])
			run++;
		memset(&pool_used[p], 1, run);
		pool_free -= run;
		*first = p;
		return run;
	}
	return 0;
}

static void pool_give(size_t first, size_t n)
{
	memset(&pool_used[first], 0, n);
	pool_free += n;
	if (first < pool_hint)
		pool_hint = first;
}

/* The pages from p on that share flags and are backed by consecutive pool
 * pages, n at most: what one call to the kernel can cover. */
static size_t run_of(size_t p, size_t n)
{
	size_t run = 1;

	while (run < n && flags_at(p + run) == flags_at(p) &&
	       src_at(p + run) == src_at(p) + run)
		run++;
	return run;
}

static int unmap_pages(size_t p, size_t n)
{
	while (n) {
		size_t run;
		Result rc;

		if (!(flags_at(p) & PG_MAPPED)) {
			p++;
			n--;
			continue;
		}
		run = run_of(p, n);
		rc = svcUnmapProcessCodeMemory(self, (u64)(arena + p * GX_PAGE),
					       (u64)(pool + (u64)src_at(p) * GX_PAGE),
					       run * GX_PAGE);
		if (R_FAILED(rc)) {
			host_log("gx: unmap of %p failed (%#x)\n", arena + p * GX_PAGE, rc);
			return -1;
		}
		pool_give(src_at(p), run);
		set_flags(p, run, 0);
		p += run;
		n -= run;
	}
	return 0;
}

static int set_protection(size_t p, size_t n, int prot)
{
	int write = prot & GX_PROT_WRITE, exec = prot & GX_PROT_EXEC;
	u32 perm = write ? Perm_Rw : exec ? Perm_Rx : (prot & GX_PROT_READ) ? Perm_R : Perm_None;
	uint8_t code = write ? P_RW : exec ? P_RX : (prot & GX_PROT_READ) ? P_R : P_NONE;

	if (write && exec)
		return -1;
	while (n) {
		size_t run = run_of(p, n);
		u64 addr = (u64)(arena + p * GX_PAGE), len = run * GX_PAGE;
		u64 src = (u64)(pool + (u64)src_at(p) * GX_PAGE);
		Result rc;

		if (!(flags_at(p) & PG_MAPPED))
			return -1;
		if ((flags_at(p) & PG_PERM) == code) {
			rc = 0;
		} else if (!(flags_at(p) & PG_DATA)) {
			rc = svcSetProcessMemoryPermission(self, addr, len, perm);
			if (R_SUCCEEDED(rc))
				set_flags(p, run, PG_MAPPED | (write ? PG_DATA : 0) | code);
		} else if (!exec) {
			rc = svcSetMemoryPermission((void *)addr, len, perm);
			if (R_SUCCEEDED(rc))
				set_flags(p, run, PG_MAPPED | PG_DATA | code);
		} else {
			/* Data to code: only a fresh mapping is code again. What
			 * was just written there (a compiler's code) goes to
			 * memory first, out of the data cache, whatever the
			 * kernel does to the caches when it maps the pages again;
			 * and the instruction cache forgets the range after. A
			 * page of code just compiled was once run as if it held
			 * zeroes (an undefined instruction, in a page that was
			 * mapped and executable). */
			if (exec)
				armDCacheFlush((void *)addr, len);
			rc = svcUnmapProcessCodeMemory(self, addr, src, len);
			if (R_SUCCEEDED(rc))
				rc = svcMapProcessCodeMemory(self, addr, src, len);
			if (R_SUCCEEDED(rc)) {
				set_flags(p, run, PG_MAPPED | P_FRESH);
				rc = svcSetProcessMemoryPermission(self, addr, len, perm);
			}
			if (R_SUCCEEDED(rc)) {
				set_flags(p, run, PG_MAPPED | code);
				if (exec)
					armICacheInvalidate((void *)addr, len);
			}
		}
		if (R_FAILED(rc)) {
			host_log("gx: protection %d of %p+%#lx failed (%#x)\n", prot,
				 (void *)addr, (unsigned long)len, rc);
			return -1;
		}
		p += run;
		n -= run;
	}
	return 0;
}

/* The kernel's records of this process, as last counted (report_kernel_use()).
 * Past about ten thousand, the kernel stops serving the process: it cannot
 * even be told so, the calls to the SD card being the first to fail. Refusing
 * memory before that at least ends in a message. */
static volatile unsigned long kernel_blocks;
#define KERNEL_BLOCKS_MAX 8500

int host_commit(void *addr, size_t len, int prot)
{
	size_t first = page_of(addr), n = len / GX_PAGE, done = 0;
	int ret = 0;

	if (kernel_blocks > KERNEL_BLOCKS_MAX) {
		static int said;

		if (!said++)
			host_log("gx: the kernel is short of memory records (%lu): no more memory "
				 "for the guest\n", kernel_blocks);
		return -1;
	}
	mutexLock(&mem_lock);
	if (n > pool_free) {
		ran_out = 1;
		host_log("gx: out of guest memory (%lu KB asked, %lu KB left)\n",
			 (unsigned long)(len >> 10), (unsigned long)(pool_free * 4));
		ret = -1;
	}
	while (!ret && done < n) {
		size_t src = 0, run = pool_take(n - done, &src);
		Result rc;

		/* The pool is ordinary memory until it is mapped: zero it here. */
		memset(pool + src * GX_PAGE, 0, run * GX_PAGE);
		rc = svcMapProcessCodeMemory(self, (u64)addr + done * GX_PAGE,
					     (u64)(pool + src * GX_PAGE), run * GX_PAGE);
		if (R_FAILED(rc)) {
			MemoryInfo si = { 0 }, di = { 0 };
			u32 pi;

			/* What the kernel says of both ends: their state is what it
			 * refuses (0xd401). */
			svcQueryMemory(&si, &pi, (u64)(pool + src * GX_PAGE));
			svcQueryMemory(&di, &pi, (u64)addr + done * GX_PAGE);
			host_log("gx: map of %p failed (%#x): %lu KB from pool page %lu of %lu; source type %#x "
				 "attr %#x perm %#x %#lx+%#lx, destination type %#x %#lx+%#lx\n",
				 (uint8_t *)addr + done * GX_PAGE, rc, (unsigned long)(run * 4),
				 (unsigned long)src, (unsigned long)pool_pages, si.type, si.attr, si.perm,
				 (unsigned long)si.addr, (unsigned long)si.size, di.type,
				 (unsigned long)di.addr, (unsigned long)di.size);
			/* Pages of the pool the kernel will not map (an attribute
			 * left on them: memset() could write them, so they are
			 * there) are set aside for good, and others taken: a browser
			 * died of it, unable to make a thread. Not if the fault is
			 * the destination's. */
			static int set_aside;

			if ((si.type != MemType_Heap || si.attr || si.perm != Perm_Rw) && set_aside < 64) {
				set_aside++;
				host_log("gx: %lu KB of the pool set aside\n", (unsigned long)(run * 4));
				continue;
			}
			pool_give(src, run);
			ret = -1;
			break;
		}
		for (size_t i = 0; i < run; i++) {
			set_page(first + done + i, src + i, PG_MAPPED | P_FRESH);
		}
		/* Two more records for the kernel, until they are next counted:
		 * the guard at the top looks at this number. */
		kernel_blocks += 2;
		done += run;
	}
	if (!ret)
		ret = set_protection(first, n, prot);
	if (ret)
		unmap_pages(first, done);
	mutexUnlock(&mem_lock);
	return ret;
}

int host_protect(void *addr, size_t len, int prot)
{
	int ret;

	mutexLock(&mem_lock);
	ret = set_protection(page_of(addr), len / GX_PAGE, prot);
	mutexUnlock(&mem_lock);
	return ret;
}

/* Short: less than a quarter of the pool left. A browser's pages come and go
 * by hundreds of megabytes (a game of a page, then another site in the same
 * tab, ran out of the 1.6 GB). */
int host_memory_low(void)
{
	return pool_free < pool_pages / 4;
}

int host_decommit(void *addr, size_t len)
{
	int ret;

	mutexLock(&mem_lock);
	ret = unmap_pages(page_of(addr), len / GX_PAGE);
	mutexUnlock(&mem_lock);
	return ret;
}

/*
 * Shared memory is the kernel's, the one kind of memory it maps at several
 * addresses of a process. It does not come out of the heap but out of what
 * the process has not claimed, and the Homebrew Menu claims everything for
 * the heap: __libnx_initheap() below gives some back.
 */
#define MAX_SHARED_HANDLES 512
static Handle shared_handles[MAX_SHARED_HANDLES];

/* The kernel has 80 shared memory objects for the whole console, and the
 * system uses some: Chromium alone makes fifty small ones. */
size_t host_shared_chunk(void) { return 16UL << 20; }

int host_shared_new(size_t len)
{
	int slot = -1;

	if (!shared_budget)
		return -1;
	mutexLock(&mem_lock);
	for (int i = 0; i < MAX_SHARED_HANDLES && slot < 0; i++)
		if (!shared_handles[i])
			slot = i;
	if (slot >= 0) {
		Result rc = svcCreateSharedMemory(&shared_handles[slot], len, Perm_Rw, Perm_Rw);

		if (R_FAILED(rc)) {
			host_log("gx: no shared memory of %lu KB (%#x)\n", (unsigned long)(len >> 10), rc);
			shared_handles[slot] = 0;
			slot = -1;
		}
	}
	mutexUnlock(&mem_lock);
	return slot;
}

int host_shared_map(int handle, void *addr, size_t len, int prot)
{
	Result rc;

	/* Always as it was created: the kernel refuses its owner any other
	 * permission, so a read-only view is only read-only by convention.
	 * Under the lock like every change to the address space: the console
	 * has been seen to fail to draw (twice, in runs that mapped shared
	 * memory) and it only draws with this lock held. */
	mutexLock(&mem_lock);
	rc = svcMapSharedMemory(shared_handles[handle], addr, len, Perm_Rw);
	mutexUnlock(&mem_lock);

	if (R_FAILED(rc))
		host_log("gx: cannot map shared memory at %p (%#x)\n", addr, rc);
	return R_FAILED(rc) ? -1 : 0;
}

void host_shared_unmap(int handle, void *addr, size_t len)
{
	mutexLock(&mem_lock);
	svcUnmapSharedMemory(shared_handles[handle], addr, len);
	mutexUnlock(&mem_lock);
}

void host_shared_free(int handle)
{
	svcCloseHandle(shared_handles[handle]);
	shared_handles[handle] = 0;
}

/* Replaces libnx's: the heap the Homebrew Menu hands over is all the memory
 * there is, and its top goes back to the system for shared memory. Leaving
 * through the Homebrew Menu is no longer possible after that (it believes the
 * heap is whole), which is why gx always ends the process. */
#define SHARED_BUDGET (384UL << 20)
#define HEAP_WORTH_SHARING (1UL << 30)

/* The program is the launcher too (gx/launcher/main.c), which loads it again
 * as the browser with --browser. The launcher hands the process back to the
 * Homebrew loader, so its heap is left as the loader gave it. */
static int browser_mode;

const char *gx_default_command(void)
{
	return DEFAULT_COMMAND;
}

void __libnx_initheap(void)
{
	extern char *fake_heap_start, *fake_heap_end;
	u64 region = 0, total;
	void *addr = NULL;
	size_t size;
	const char *args = envGetArgv();

	browser_mode = args && strstr(args, "--browser");
	if (envHasHeapOverride() && !browser_mode) {
		fake_heap_start = envGetHeapOverrideAddr();
		fake_heap_end = (char *)envGetHeapOverrideAddr() + envGetHeapOverrideSize();
		return;
	}
	if (envHasHeapOverride()) {
		addr = envGetHeapOverrideAddr();
		size = envGetHeapOverrideSize();
		svcGetInfo(&region, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
		total = (u64)addr + size - region;
		/* Only when the numbers are what a heap looks like. */
		if (size >= HEAP_WORTH_SHARING && region && region <= (u64)addr &&
		    !(total & 0x1fffff)) {
			void *base;

			if (R_SUCCEEDED(svcSetHeapSize(&base, total - SHARED_BUDGET))) {
				size -= SHARED_BUDGET;
				shared_budget = SHARED_BUDGET;
			}
		}
	} else {
		u64 available = 0, used = 0;

		svcGetInfo(&available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
		svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
		size = available > used + 0x200000 ? (available - used - 0x200000) & ~0x1fffffUL : 0;
		if (size >= HEAP_WORTH_SHARING) {
			size -= SHARED_BUDGET;
			shared_budget = SHARED_BUDGET;
		}
		if (!size)
			size = 0x2000000 * 16;
		if (R_FAILED(svcSetHeapSize(&addr, size)))
			diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
	}
	fake_heap_start = addr;
	fake_heap_end = (char *)addr + size;
}

void host_flush_icache(void *addr, size_t len)
{
	/* The kernel flushes the instruction cache when it makes pages
	 * executable, which is the only way code appears here. */
}

/* ---------- threads ---------- */

struct start {
	Thread thread;         /* first: threadGetSelf() of the thread finds this */
	void (*fn)(void *);
	void *arg;
	struct start *next;    /* among the threads that ended */
};

/* A thread cannot give back its own stack and its record in the kernel; the
 * ones that ended wait here for another thread to do it. A browser playing a
 * video goes through hundreds of short-lived threads, a megabyte of stack
 * each, and the kernel counts the threads a process may have. */
static struct start *ended;
static Mutex ended_lock;

static void forget_thread(Handle handle);

static void bury_threads(void)
{
	struct start *s;

	mutexLock(&ended_lock);
	s = ended;
	ended = NULL;
	mutexUnlock(&ended_lock);
	while (s) {
		struct start *next = s->next;

		/* It is on its way out, if not out already. */
		threadWaitForExit(&s->thread);
		forget_thread(s->thread.handle);
		threadClose(&s->thread);
		free(s);
		s = next;
	}
}

static int next_core;

/* Every thread of the process, by its handle: when everything stops, where
 * each of them is is all there is to go by (see report_stuck()). */
#define MAX_KNOWN 512
static struct {
	Handle handle;
	const char *what;
} known[MAX_KNOWN];
static int known_count;

static void know_thread(Handle handle, const char *what)
{
	int slot = __atomic_fetch_add(&known_count, 1, __ATOMIC_SEQ_CST);

	if (slot < MAX_KNOWN) {
		known[slot].what = what;
		known[slot].handle = handle;
	}
}

/* For nx_host.c, which makes the kernel's timer threads. */
void gx_know_thread(Handle handle, const char *what)
{
	know_thread(handle, what);
}

static int count_threads(void)
{
	int count = 0;

	for (int i = 0; i < known_count && i < MAX_KNOWN; i++)
		count += known[i].handle != 0;
	return count;
}

static void forget_thread(Handle handle)
{
	for (int i = 0; i < known_count && i < MAX_KNOWN; i++)
		if (known[i].handle == handle)
			known[i].handle = 0;
}

static void thread_start(void *p)
{
	struct start *s = p;

	s->fn(s->arg);
}

static int start_thread(void (*fn)(void *), void *arg, size_t stack)
{
	struct start *s = malloc(sizeof(*s));   /* lives as long as the thread */
	int core = __atomic_fetch_add(&next_core, 1, __ATOMIC_RELAXED) % 3;
	Result rc;

	if (!s)
		return -1;
	bury_threads();
	s->fn = fn;
	s->arg = arg;
	rc = threadCreate(&s->thread, thread_start, s, NULL, stack, GUEST_PRIO, core);
	if (R_SUCCEEDED(rc)) {
		/* It starts on its core but may run on any of the three: a thread
		 * tied to one core waits for the others tied to it, a time slice
		 * at a time, while the other cores sit idle (Chromium ran at 28
		 * frames a second with no thread above 40% of a processor). */
		svcSetThreadCoreMask(s->thread.handle, core, 0x7);
		know_thread(s->thread.handle, "ours");
		rc = threadStart(&s->thread);
		if (R_FAILED(rc))
			threadClose(&s->thread);
	}
	if (R_FAILED(rc)) {
		host_log("gx: cannot start a thread (%#x), with %d of them\n", rc, count_threads());
		free(s);
		return -1;
	}
	return 0;
}

int host_thread_start(void (*fn)(void *), void *arg)
{
	return start_thread(fn, arg, THREAD_STACK);
}

void host_thread_exit(void)
{
	struct start *s = (struct start *)threadGetSelf();

	mutexLock(&ended_lock);
	s->next = ended;
	ended = s;
	mutexUnlock(&ended_lock);
	threadExit();
	__builtin_unreachable();
}

void host_yield(void) { svcSleepThread(YieldType_WithCoreMigration); }

/*
 * Every syscall of the kernel, the guest's and ours, runs at the kernel's
 * priority (the NRO is linked with --wrap=lkl_syscall). The kernel has one
 * processor, which threads hand to each other through semaphores; a guest
 * thread woken to take it had to wait for its core, busy with guest threads
 * of the same priority as itself, and a syscall took half a millisecond when
 * a browser was busy. And one of our threads below the kernel's priority
 * that held that processor was kept off its core by those that wanted it.
 */
#define KERNEL_PRIO 0x2C

long __real_lkl_syscall(long no, long *params);
static int no_kernel_prio;   /* -xb */

long __wrap_lkl_syscall(long no, long *params)
{
	s32 prio = KERNEL_PRIO;
	long ret;

	if (no_kernel_prio)
		return __real_lkl_syscall(no, params);
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	if (prio <= KERNEL_PRIO)
		return __real_lkl_syscall(no, params);
	svcSetThreadPriority(CUR_THREAD_HANDLE, KERNEL_PRIO);
	ret = __real_lkl_syscall(no, params);
	svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
	return ret;
}

/* What the guest asks of a thread's priority (gsys.c): 0 as the others, 1
 * before them (Chromium's display, input and I/O threads ask for that, with a
 * nice value below 0), 2 before those too (real time: audio). Horizon only
 * time-slices GUEST_PRIO, so a thread above it runs until it waits: only
 * threads that wait often are put there. Syscalls still run at KERNEL_PRIO
 * (__wrap_lkl_syscall() restores what was there). */
int host_thread_priority(uint64_t thread, int level)
{
	static const int prio[] = { GUEST_PRIO, GUEST_PRIO - 1, GUEST_PRIO - 2 };

	if (level < 0 || level > 2)
		return -1;
	return R_FAILED(svcSetThreadPriority((Handle)thread, prio[level])) ? -1 : 0;
}

void host_in_kernel(int in)
{
	svcSetThreadPriority(CUR_THREAD_HANDLE, in ? KERNEL_PRIO : GUEST_PRIO);
}

uint64_t host_thread_self(void) { return threadGetCurHandle(); }

uint64_t host_thread_cpu_ns(uint64_t thread)
{
	u64 ticks = 0;

	/* On every core (-1). */
	if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, (Handle)thread, (u64)-1)))
		return 0;
	return armTicksToNs(ticks);
}

static void export_file(void);

void host_exit(int code)
{
	export_file();
	gmem_report();
	host_log("\ngx: the program exited with code %d. Press + to quit.\n", code);
	sync_log();
	finished = 1;
	/* Other guest threads are left alone: Horizon cannot stop them, and
	 * quitting ends the whole process. */
	for (;;)
		svcSleepThread(1000000000ULL);
}

struct host_lock { Mutex m; };
struct host_cond { CondVar c; };

struct host_lock *host_lock_new(void)
{
	struct host_lock *l = malloc(sizeof(*l));

	mutexInit(&l->m);
	return l;
}

void host_lock_acquire(struct host_lock *l) { mutexLock(&l->m); }
void host_lock_release(struct host_lock *l) { mutexUnlock(&l->m); }

struct host_cond *host_cond_new(void)
{
	struct host_cond *c = malloc(sizeof(*c));

	condvarInit(&c->c);
	return c;
}

int host_cond_wait(struct host_cond *c, struct host_lock *l, int64_t ns)
{
	if (ns < 0) {
		condvarWait(&c->c, &l->m);
		return 0;
	}
	return R_FAILED(condvarWaitTimeout(&c->c, &l->m, ns));
}

void host_cond_signal(struct host_cond *c) { condvarWakeOne(&c->c); }

/* ---------- the rest of the host interface ---------- */

void host_random(void *buf, size_t len) { randomGet(buf, len); }

uint64_t host_now_ns(void) { return armTicksToNs(armGetSystemTick()); }

uint64_t host_hwcap(int which)
{
	/* The Cortex-A57 of the Switch: fp, asimd, aes, pmull, sha1, sha2, crc32. */
	return which ? 0 : 0xfb;
}

/* ---------- host functions for the guest: EGL and GLES ---------- */

/* Every EGL and GLES function of the libraries linked in (Mesa), made by
 * build-nx.sh: there is no dlsym() here. */
extern const struct gx_gl_entry {
	const char *name;
	void (*fn)(void);
} gx_gl_table[];

static EGLDisplay gl_get_display(void *native)
{
	return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static EGLDisplay gl_get_platform_display(EGLenum platform, void *native, const EGLAttrib *attribs)
{
	return eglGetDisplay(EGL_DEFAULT_DISPLAY);
}

static EGLSurface window_surface = EGL_NO_SURFACE;

static volatile unsigned gl_swaps;

/* A function of the EGL and GLES linked in, the real one, by its name. */
static void *gl_raw(const char *name)
{
	for (const struct gx_gl_entry *e = gx_gl_table; e->name; e++)
		if (!strcmp(name, e->name))
			return (void *)e->fn;
	return (void *)eglGetProcAddress(name);
}

/* The screen, for glwin.c, which the console gives up. */
static void *gl_screen(void *dpy, void *config)
{
	NWindow *win = nwindowGetDefault();
	int portrait = wl_height > wl_width;

	console_off();
	/* In pixels: on a TV, 1.5 of them to the window's every unit (wl.h). */
	u32 w = (u32)(wl_width * wl_scale + 0.5f), h = (u32)(wl_height * wl_scale + 0.5f);

	/* Portrait: the window's pictures are as tall as the screen is wide.
	 * The screen is a square as long as that, so that they fit, and the
	 * console shows its top, as wide as the screen, where glwin.c lays the
	 * picture turned a quarter to the left (its top on the left of the
	 * screen). The console's own turning is not used: a TV shows nothing
	 * of a picture taller than itself. See poll_input() for the touch
	 * screen and the pad. */
	nwindowReleaseBuffers(win);
	nwindowSetTransform(win, 0);
	if (portrait) {
		nwindowSetDimensions(win, h, h);
		nwindowSetCrop(win, 0, 0, h, w);
	} else {
		nwindowSetDimensions(win, w, h);
		nwindowSetCrop(win, 0, 0, w, h);
	}
	window_surface = eglCreateWindowSurface(dpy, config, win, NULL);
	if (window_surface == EGL_NO_SURFACE)
		host_log("gx: gl: no window surface (%#x)\n", eglGetError());
	else
		host_log("gx: gl: the guest has the screen\n");
	return window_surface == EGL_NO_SURFACE ? NULL : window_surface;
}

/* How smooth the pictures came: gaps between two of them over 50 and 100 ms,
 * and the longest, since the watchdog last said. */
static volatile unsigned gl_gaps50, gl_gaps100;
static volatile uint64_t gl_gap_max, gl_last_ns;

static int profiling;   /* -p: see profiler() */

static void gl_presented(void *dpy, void *screen)
{
	uint64_t now = host_now_ns(), gap = gl_last_ns ? now - gl_last_ns : 0;

	gl_last_ns = now;
	gl_swaps++;
	if (gap > 50000000ULL)
		gl_gaps50++;
	if (gap > 100000000ULL) {
		gl_gaps100++;
		/* When, to be set against what else happened then. */
		if (profiling)
			host_log("gx: gl: no picture for %lu ms\n", (unsigned long)(gap / 1000000));
	}
	if (gap > gl_gap_max)
		gl_gap_max = gap;
}

static const uint32_t *gl_cursor(int *x, int *y, int *width, int *height);
static const struct glwin_host gl_host = { gl_raw, gl_screen, gl_presented, overlay_picture, gl_cursor };

/* The guest's windows are glwin.c's: it puts them together on the screen. */
static EGLSurface gl_create_window_surface(EGLDisplay dpy, EGLConfig config, void *window,
					   const void *attribs)
{
	void *surface = glwin_create(dpy, config, window);

	if (!surface)
		host_log("gx: gl: no surface for a window (%#x)\n", eglGetError());
	return surface ? surface : EGL_NO_SURFACE;
}

static EGLSurface gl_get_current_surface(EGLint which)
{
	return glwin_current_surface(which);
}

static EGLBoolean gl_surface_attrib(EGLDisplay dpy, EGLSurface surface, EGLint attribute, EGLint value)
{
	return eglSurfaceAttrib(dpy, glwin_surface(surface), attribute, value);
}

static EGLBoolean gl_destroy_surface(EGLDisplay dpy, EGLSurface surface)
{
	int done = glwin_destroy(dpy, surface);

	return done < 0 ? eglDestroySurface(dpy, surface) : done;
}

/* Mesa on the Switch says a window surface is 0 by 0 (seen on hardware, at
 * least before the first frame); the window knows better. */
static EGLBoolean gl_query_surface(EGLDisplay dpy, EGLSurface surface, EGLint attribute,
				   EGLint *value)
{
	EGLBoolean ret;
	u32 width = 0, height = 0;

	int done = glwin_query(dpy, surface, attribute, value);

	if (done >= 0)
		return done;
	ret = eglQuerySurface(dpy, surface, attribute, value);
	if (ret && value && *value <= 0 && surface == window_surface &&
	    (attribute == EGL_WIDTH || attribute == EGL_HEIGHT) &&
	    R_SUCCEEDED(nwindowGetDimensions(nwindowGetDefault(), &width, &height)))
		*value = attribute == EGL_WIDTH ? width : height;
	return ret;
}

static EGLBoolean gl_terminate(EGLDisplay dpy)
{
	EGLBoolean ret = eglTerminate(dpy);

	console_back();
	return ret;
}

/* What follows only watches: a guest that draws with the GPU (a Chromium told
 * to) says little about why it gave up, and the log is all there is to go by. */
static EGLBoolean gl_swap_buffers(EGLDisplay dpy, EGLSurface surface)
{
	int done = glwin_swap(dpy, surface);

	return done < 0 ? eglSwapBuffers(dpy, surface) : done;
}

/* With the parts that changed: the whole of it is shown all the same. */
static EGLBoolean gl_swap_buffers_with_damage(EGLDisplay dpy, EGLSurface surface,
					      const EGLint *rects, EGLint count)
{
	return gl_swap_buffers(dpy, surface);
}

static EGLBoolean gl_initialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
	EGLBoolean ret = eglInitialize(dpy, major, minor);

	if (!ret)
		host_log("gx: gl: eglInitialize failed (%#x)\n", eglGetError());
	else
		host_log("gx: gl: EGL %s, %s\n", eglQueryString(dpy, EGL_VERSION),
			 eglQueryString(dpy, EGL_EXTENSIONS));
	return ret;
}

static EGLBoolean gl_choose_config(EGLDisplay dpy, const EGLint *attribs, EGLConfig *configs,
				   EGLint size, EGLint *count)
{
	EGLBoolean ret = eglChooseConfig(dpy, attribs, configs, size, count);

	if (!ret || (count && !*count)) {
		host_log("gx: gl: eglChooseConfig found nothing (%#x) for", ret ? 0 : eglGetError());
		for (int i = 0; attribs && attribs[i] != EGL_NONE && i < 40; i += 2)
			host_log(" %#x=%#x", attribs[i], attribs[i + 1]);
		host_log("\n");
	}
	return ret;
}

static EGLContext gl_create_context(EGLDisplay dpy, EGLConfig config, EGLContext share,
				    const EGLint *attribs)
{
	EGLContext ret = eglCreateContext(dpy, config, share, attribs);

	if (ret == EGL_NO_CONTEXT) {
		host_log("gx: gl: eglCreateContext failed (%#x) for", eglGetError());
		for (int i = 0; attribs && attribs[i] != EGL_NONE && i < 40; i += 2)
			host_log(" %#x=%#x", attribs[i], attribs[i + 1]);
		host_log("\n");
	}
	return ret;
}

static EGLSurface gl_create_pbuffer_surface(EGLDisplay dpy, EGLConfig config, const EGLint *attribs)
{
	EGLSurface ret = eglCreatePbufferSurface(dpy, config, attribs);

	if (ret == EGL_NO_SURFACE)
		host_log("gx: gl: eglCreatePbufferSurface failed (%#x)\n", eglGetError());
	return ret;
}

static EGLBoolean gl_make_current(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext context)
{
	static int said, failures;
	EGLBoolean ret = glwin_make_current(dpy, draw, read, context) > 0;

	if (!ret && failures++ < 8) {
		host_log("gx: gl: eglMakeCurrent failed (%#x), surface %p, context %p\n",
			 eglGetError(), draw, context);
	} else if (ret && context != EGL_NO_CONTEXT && !said) {
		said = 1;
		host_log("gx: gl: %s, %s\n", (const char *)glGetString(GL_RENDERER),
			 (const char *)glGetString(GL_VERSION));
	}
	return ret;
}

/* The driver would call the guest's function from the host's side of things,
 * which cannot be done (see gx_hostcall): it gets no messages. */
static void gl_debug_message_callback(void *callback, const void *param)
{
}

/* Textures of 32 MB or more: the GPU's memory comes out of the host's heap
 * in one piece, which a heap in pieces may not have although it has as much
 * in all; when the driver refuses one, the browser's GPU context is lost and
 * it dies. Said, to know what asked. */
void glTexStorage2D(GLenum, GLsizei, GLenum, GLsizei, GLsizei);

static void big_texture(const char *what, int w, int h)
{
	static int said;

	if ((uint64_t)w * h * 4 >= (32UL << 20) && said < 20 && ++said)
		host_log("gx: gl: %s of %dx%d (%lu MB)\n", what, w, h,
			 (unsigned long)((uint64_t)w * h * 4 >> 20));
}

static void gl_tex_storage_2d(GLenum target, GLsizei levels, GLenum format, GLsizei w, GLsizei h)
{
	big_texture("texture", w, h);
	glTexStorage2D(target, levels, format, w, h);
}

static void gl_tex_image_2d(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
			    GLint border, GLenum format, GLenum type, const void *pixels)
{
	if (!level)
		big_texture("image", w, h);
	glTexImage2D(target, level, internal, w, h, border, format, type, pixels);
}

/* ---------- host functions for the guest: sound ---------- */

/*
 * What the guest's libasound (guest/asound.c) plays goes to the console's
 * audio output: 48000 frames a second of two samples of 16 bits, which is
 * also what a browser asks for. The guest's thread leaves the frames in a
 * ring; a thread of ours hands them to the system a chunk at a time, keeping
 * a couple of chunks ahead of what is being played.
 */
#define AUDIO_RING 16384                /* frames: a third of a second */
#define AUDIO_CHUNK 1024                /* frames: a page of memory, as the system wants */
#define AUDIO_CHUNKS 4
#define AUDIO_AHEAD 3

static Mutex audio_lock;
static s16 audio_ring[AUDIO_RING * 2];
static u64 audio_written, audio_taken;  /* frames into the ring, and out of it */
static volatile int audio_pending;      /* chunks the system has and has not played */
static int audio_started;

static void audio_feed(void *arg)
{
	static AudioOutBuffer buffers[AUDIO_CHUNKS];
	AudioOutBuffer *released;
	u32 count;
	int next = 0;

	for (int i = 0; i < AUDIO_CHUNKS; i++)
		buffers[i].buffer = aligned_alloc(0x1000, AUDIO_CHUNK * 4);
	for (;;) {
		u64 have;
		s16 *out = buffers[next].buffer;

		count = 0;
		if (audio_pending && R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &count)))
			audio_pending -= count;
		mutexLock(&audio_lock);
		have = audio_written - audio_taken;
		/* A whole chunk, or what there is when nothing else is left to
		 * play (the end of a sound). */
		if (!out || !(have >= AUDIO_CHUNK || (have && !audio_pending))) {
			mutexUnlock(&audio_lock);
			svcSleepThread(2000000ULL);
			continue;
		}
		if (have > AUDIO_CHUNK)
			have = AUDIO_CHUNK;
		for (u64 i = 0; i < have; i++)
			memcpy(out + i * 2, audio_ring + (audio_taken + i) % AUDIO_RING * 2, 4);
		memset(out + have * 2, 0, (AUDIO_CHUNK - have) * 4);
		audio_taken += have;
		mutexUnlock(&audio_lock);

		buffers[next].next = NULL;
		buffers[next].buffer_size = AUDIO_CHUNK * 4;
		buffers[next].data_size = AUDIO_CHUNK * 4;
		buffers[next].data_offset = 0;
		if (R_FAILED(audoutAppendAudioOutBuffer(&buffers[next]))) {
			svcSleepThread(10000000ULL);
			continue;
		}
		audio_pending++;
		next = (next + 1) % AUDIO_CHUNKS;
		while (audio_pending >= AUDIO_AHEAD) {
			count = 0;
			if (R_FAILED(audoutWaitPlayFinish(&released, &count, 100000000ULL)))
				break;
			audio_pending -= count;
		}
	}
}

static long gx_audio_open(long rate, long channels)
{
	static Thread thread;
	Result rc = 0;

	if (rate != 48000 || channels != 2) {
		host_log("gx: audio: %ld Hz on %ld channels is not what the console plays\n", rate, channels);
		return -1;
	}
	mutexLock(&audio_lock);
	if (!audio_started) {
		rc = audoutInitialize();
		if (R_SUCCEEDED(rc))
			rc = audoutStartAudioOut();
		if (R_SUCCEEDED(rc))
			rc = threadCreate(&thread, audio_feed, NULL, NULL, 0x8000, 0x2B, -2);
		if (R_SUCCEEDED(rc)) {
			svcSetThreadCoreMask(thread.handle, -3, 0x7);
			know_thread(thread.handle, "audio");
			rc = threadStart(&thread);
		}
		audio_started = R_SUCCEEDED(rc);
	}
	mutexUnlock(&audio_lock);
	if (R_FAILED(rc))
		host_log("gx: audio: no output (%#x)\n", rc);
	else
		host_log("gx: audio: playing\n");
	return audio_started ? 0 : -1;
}

static long gx_audio_write(const s16 *frames, long count)
{
	u64 room;

	mutexLock(&audio_lock);
	room = AUDIO_RING - (audio_written - audio_taken);
	if ((u64)count > room)
		count = room;
	for (long i = 0; i < count; i++)
		memcpy(audio_ring + (audio_written + i) % AUDIO_RING * 2, frames + i * 2, 4);
	audio_written += count;
	mutexUnlock(&audio_lock);
	return count;
}

static long gx_audio_queued(void)
{
	long queued;

	mutexLock(&audio_lock);
	queued = (long)(audio_written - audio_taken) + (long)audio_pending * AUDIO_CHUNK;
	mutexUnlock(&audio_lock);
	return queued;
}

/* What is not played yet is not to be: the ring is emptied, the chunks the
 * system has are short enough to let them go. */
static void gx_audio_drop(void)
{
	mutexLock(&audio_lock);
	audio_taken = audio_written;
	mutexUnlock(&audio_lock);
}

static void gx_audio_close(void)
{
	gx_audio_drop();
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
		{ "eglCreateWindowSurface", gl_create_window_surface },
		{ "eglCreatePlatformWindowSurface", gl_create_window_surface },
		{ "eglCreatePlatformWindowSurfaceEXT", gl_create_window_surface },
		{ "eglQuerySurface", gl_query_surface },
		{ "eglTerminate", gl_terminate },
		{ "eglSwapBuffers", gl_swap_buffers },
		{ "eglDestroySurface", gl_destroy_surface },
		{ "eglGetCurrentSurface", gl_get_current_surface },
		{ "eglSurfaceAttrib", gl_surface_attrib },
		{ "eglSwapBuffersWithDamageKHR", gl_swap_buffers_with_damage },
		{ "eglSwapBuffersWithDamageEXT", gl_swap_buffers_with_damage },
		{ "eglInitialize", gl_initialize },
		{ "eglChooseConfig", gl_choose_config },
		{ "eglCreateContext", gl_create_context },
		{ "eglCreatePbufferSurface", gl_create_pbuffer_surface },
		{ "eglMakeCurrent", gl_make_current },
		{ "glDebugMessageCallback", gl_debug_message_callback },
		{ "glDebugMessageCallbackKHR", gl_debug_message_callback },
		{ "glTexStorage2D", gl_tex_storage_2d },
		{ "glTexImage2D", gl_tex_image_2d },
	};

	if (!strcmp(name, "gx_audio_open"))
		return gx_audio_open;
	if (!strcmp(name, "gx_audio_write"))
		return gx_audio_write;
	if (!strcmp(name, "gx_audio_queued"))
		return gx_audio_queued;
	if (!strcmp(name, "gx_audio_drop"))
		return gx_audio_drop;
	if (!strcmp(name, "gx_audio_close"))
		return gx_audio_close;
	if (!strncmp(name, "gx_video_", 9))
		return video_symbol(name);
	if (strncmp(name, "egl", 3) && strncmp(name, "gl", 2))
		return NULL;
	glwin_init(&gl_host);
	for (size_t i = 0; i < sizeof(ours) / sizeof(ours[0]); i++)
		if (!strcmp(name, ours[i].name))
			return ours[i].fn;
	for (const struct gx_gl_entry *e = gx_gl_table; e->name; e++)
		if (!strcmp(name, e->name))
			return (void *)e->fn;
	return (void *)eglGetProcAddress(name);
}

/* ---------- network ---------- */


/* The device has to exist before the kernel starts. Returns its id, or a
 * negative value: the guest then runs without a network. */
static int net_add(void)
{
	u32 addr = 0, mask = 0, gateway = 0, dns = 0, dns2 = 0;
	struct lkl_netdev_args args;
	struct lkl_netdev *nd;
	static uint8_t mac[6];
	int id;

	{
		/* More room than libnx asks for by default: the system gives
		 * out sockets from a store whose size comes from these numbers,
		 * and a browser wants a hundred where the default gave about
		 * twenty (ENOBUFS after that). Small buffers for each socket to
		 * start with, which the NAT's own make up for, and a store
		 * sized for large ones. */
		SocketInitConfig config = *socketGetDefaultInitConfig();
		Result rc;

		config.tcp_tx_buf_size = 0x4000;
		config.tcp_rx_buf_size = 0x8000;
		config.tcp_tx_buf_max_size = 0x100000;
		config.tcp_rx_buf_max_size = 0x100000;
		config.sb_efficiency = 8;
		rc = socketInitialize(&config);
		if (R_FAILED(rc)) {
			host_log("gx: no sockets with room for a browser (%#x): the usual ones\n", rc);
			rc = socketInitializeDefault();
		}
		if (R_FAILED(rc)) {
			host_log("gx: no sockets, no network\n");
			return -1;
		}
	}
	if (R_SUCCEEDED(nifmInitialize(NifmServiceType_User))) {
		nifmGetCurrentIpConfigInfo(&addr, &mask, &gateway, &dns, &dns2);
		nifmExit();
	}
	if (!dns)
		dns = inet_addr("1.1.1.1");
	nd = nat_create(dns, dns2, mac);
	memset(&args, 0, sizeof(args));
	args.mac = mac;
	id = nd ? lkl_netdev_add(nd, &args) : -1;
	host_log("gx: network device %d, resolver %s", id, inet_ntoa((struct in_addr){ dns }));
	host_log(" and %s\n", dns2 ? inet_ntoa((struct in_addr){ dns2 }) : "no other");
	return id;
}

static void net_up(int id)
{
	int ifindex = lkl_netdev_get_ifindex(id);
	int ret = ifindex < 0 ? ifindex : lkl_if_up(ifindex);

	lkl_if_up(1); /* lo */
	if (!ret)
		ret = lkl_if_set_ipv4(ifindex, htonl(NAT_GUEST), NAT_PREFIX);
	if (!ret)
		ret = lkl_set_ipv4_gateway(htonl(NAT_GATEWAY));
	if (ret)
		host_log("gx: cannot configure the network (%d)\n", ret);
}

/* ---------- crash report ---------- */

extern char _start[];   /* the first instruction of the NRO (libnx's crt0) */

alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static void report_address(const char *label, uint64_t addr)
{
	uint64_t base = 0;
	int kind = gmem_describe(addr, &base);

	if (kind == 2)
		host_log("  %s %#lx (image at %#lx, offset %#lx)\n", label, (unsigned long)addr,
			 (unsigned long)base, (unsigned long)(addr - base));
	else
		host_log("  %s %#lx (%s)\n", label, (unsigned long)addr,
			 kind ? "guest memory" : "host");
}

/* Runs on the faulting thread, which never leaves: the screen and the log
 * stay readable until + is pressed. */
/* Called by the exception entry (nx/exception.S) for memory that refused a
 * jump or an access, on the stack of the thread it happened to: returns
 * whether the thread is to try again. */
/*
 * A fault in memory the guest may use as it did, which the kernel had out of
 * the way for a moment: another thread was mapping it or memory next to it
 * again (memory that turns from data to code is unmapped and mapped back, and
 * when the kernel changes how a range is mapped it may first take away the
 * whole larger block around it, as the processor requires). Seen on hardware
 * as a crash at a page the kernel then said was executable, once in code just
 * compiled and once in the browser's own. The thread waits for the page to be
 * back and tries again; a fault that keeps coming back at the same place is a
 * real one.
 */
static __thread u64 last_transient_addr;
static __thread int transient_count;

static int transient_fault(u64 addr, int exec)
{
	int state = gmem_page_state(addr);

	/* In use and there for the guest, with what the access needs. */
	if (state < 0 || (state & 0xc0) != 0xc0 || !(state & (exec ? GX_PROT_EXEC : GX_PROT_READ)))
		return 0;
	if (addr == last_transient_addr && ++transient_count > 20)
		return 0;
	if (addr != last_transient_addr) {
		last_transient_addr = addr;
		transient_count = 1;
	}
	for (int i = 0; i < 200; i++) {
		MemoryInfo info = { 0 };
		u32 page_info;

		if (R_SUCCEEDED(svcQueryMemory(&info, &page_info, addr)) &&
		    (info.perm & (exec ? Perm_X : Perm_R)))
			return 1;
		svcSleepThread(100000ULL);
	}
	return 0;
}

int gx_nx_fault(u64 addr, int exec)
{
	static int said, said_transient;
	int fixed = gmem_fault(addr, exec);

	if (!fixed && transient_fault(addr, exec)) {
		if (said_transient < 8 && __atomic_fetch_add(&said_transient, 1, __ATOMIC_SEQ_CST) < 8)
			host_log("gx: a %s at %#lx found the page being mapped again; tried again\n",
				 exec ? "jump" : "read or write", (unsigned long)addr);
		return 1;
	}

	/* The first ones, to tell in the log how far this got. */
	if (fixed && said < 4 && __atomic_fetch_add(&said, 1, __ATOMIC_SEQ_CST) < 4) {
		host_log("gx: compiled code: %s at %#lx\n", exec ? "a jump" : "a write",
			 (unsigned long)addr);
		sync_log();
	}
	return fixed;
}

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
	static int reported;

	/* The first few: one fault can hide another that came after it. */
	if (__atomic_fetch_add(&reported, 1, __ATOMIC_SEQ_CST) < 3) {
		/* The thread may have died holding the lock; do without it then. */
		if (!mutexTryLock(&print_lock))
			mutexInit(&print_lock);
		else
			mutexUnlock(&print_lock);
		host_log("\ngx: fault, exception %#x%s\n", ctx->error_desc,
			 gmem_describe(ctx->pc.x, &(uint64_t){ 0 }) ? "" : " in the host");
		/* For addresses in the host: gx.map has them from this base. */
		host_log("  gx is loaded at %p\n", _start);
		report_address("pc ", ctx->pc.x);
		report_address("lr ", ctx->lr.x);
		host_log("  far %#lx\n  sp  %#lx\n  esr %#x (tried %d times at this address)\n",
			 (unsigned long)ctx->far.x, (unsigned long)ctx->sp.x, ctx->esr,
			 ctx->far.x == last_transient_addr ? transient_count : 0);
		{
			/* What the guest's memory and the kernel each say of the
			 * page: a fault in memory that is writable and executable
			 * by turns should have been turned around. */
			MemoryInfo info = { 0 };
			u32 page_info;

			svcQueryMemory(&info, &page_info, ctx->far.x);
			host_log("  page state %#x; for the kernel, type %#x, permission %#x, %#lx+%#lx\n",
				 gmem_page_state(ctx->far.x), info.type, info.perm,
				 (unsigned long)info.addr, (unsigned long)info.size);
		}
		for (int i = 0; i < 9; i++)
			host_log("  x%d  %#lx\n", i, (unsigned long)ctx->cpu_gprs[i].x);
		{
			/* Whose thread: a guest's (its name), or the host's own
			 * (the kernel's, the disk's, the display's...). */
			Handle me = threadGetCurHandle();
			const char *name = "the host's";
			Thread *self_thread = threadGetSelf();

			for (int i = 0; i < GX_MAX_THREADS; i++)
				if (gx_tp_tab[i].guest_tp != ~0UL && gx_tp_tab[i].thread &&
				    gx_tp_tab[i].thread->host_id == me)
					name = gx_tp_tab[i].thread->name;
			for (int i = 0; i < known_count && i < MAX_KNOWN; i++)
				if (known[i].handle == me && name[0] == 't')
					name = known[i].what;
			if (self_thread)
				host_log("  stack %p+%#lx\n", self_thread->stack_mirror,
					 (unsigned long)self_thread->stack_sz);
			uint64_t fp = ctx->fp.x;

			host_log("  thread %#x (%s); called from:", me, name);
			/* The frames, as far as they look like frames: offsets
			 * in gx.map for the host's code. */
			for (int i = 0; i < 8 && fp && !(fp & 7) && fp - ctx->sp.x < (1UL << 20); i++) {
				uint64_t lr = ((uint64_t *)fp)[1];

				host_log(" %#lx", (unsigned long)(lr - (uint64_t)_start));
				fp = ((uint64_t *)fp)[0];
			}
			host_log("\n");
		}
		host_log("Press + to quit.\n");
		sync_log();
		finished = 1;
		crashed = 1;
	}
	for (;;)
		svcSleepThread(1000000000ULL);
}

/* ---------- the root filesystem ---------- */

/* An ext4 image on the SD card as a disk of the kernel. The root is one, too
 * big to copy into memory and only read; the profile is another, which is the
 * guest's /root and is written: what a browser keeps from one run to the
 * next. */
struct image {
	FILE *file;
	Mutex lock;
	int written;       /* since it was last committed to the card */
	/* The root, which is only read, is read with the file system's own
	 * calls: no stdio buffer to copy through and to seek in, and no lock,
	 * so that the kernel's requests do not wait on each other. */
	FsFile direct;
	int has_direct;
};

static struct image root_image, profile_image;
static struct lkl_disk root_disk = { .handle = &root_image };
static struct lkl_disk profile_disk = { .handle = &profile_image };
static int profile_mounted;
/* Once the kernel has started: nothing may ask it anything before (the
 * watchdog did, while a first profile was being unpacked, and crashed on the
 * kernel's lock, which did not exist yet). */
static volatile int kernel_running;

static int image_capacity(struct lkl_disk disk, unsigned long long *res)
{
	struct image *image = disk.handle;

	mutexLock(&image->lock);
	fseeko(image->file, 0, SEEK_END);
	*res = ftello(image->file);
	mutexUnlock(&image->lock);
	return 0;
}

/* What the card costs: bytes read and the time spent reading them. */
static uint64_t disk_read_ns, disk_read_bytes;

static int image_request(struct lkl_disk disk, struct lkl_blk_req *req)
{
	struct image *image = disk.handle;
	int status = LKL_DEV_BLK_STATUS_OK;

	if (req->type == LKL_DEV_BLK_TYPE_FLUSH || req->type == LKL_DEV_BLK_TYPE_FLUSH_OUT) {
		/* The kernel wants what it wrote to be safe: on the card, not in
		 * a buffer of ours. Its journal counts on it. */
		mutexLock(&image->lock);
		if (image->written) {
			if (fflush(image->file) || fsync(fileno(image->file)))
				status = LKL_DEV_BLK_STATUS_IOERR;
			image->written = 0;
		}
		mutexUnlock(&image->lock);
		return status;
	}
	if (req->type != LKL_DEV_BLK_TYPE_READ &&
	    (req->type != LKL_DEV_BLK_TYPE_WRITE || image != &profile_image))
		return LKL_DEV_BLK_STATUS_UNSUP;
	if (req->type == LKL_DEV_BLK_TYPE_READ && image->has_direct) {
		uint64_t at = req->sector * 512, t0 = host_now_ns();

		for (int i = 0; i < req->count && status == LKL_DEV_BLK_STATUS_OK; i++) {
			u64 done = 0;
			size_t len = req->buf[i].iov_len;

			if (R_FAILED(fsFileRead(&image->direct, at, req->buf[i].iov_base, len,
						FsReadOption_None, &done)) || done != len)
				status = LKL_DEV_BLK_STATUS_IOERR;
			at += len;
			__atomic_add_fetch(&disk_read_bytes, done, __ATOMIC_RELAXED);
		}
		__atomic_add_fetch(&disk_read_ns, host_now_ns() - t0, __ATOMIC_RELAXED);
		return status;
	}
	mutexLock(&image->lock);
	if (fseeko(image->file, req->sector * 512, SEEK_SET))
		status = LKL_DEV_BLK_STATUS_IOERR;
	for (int i = 0; i < req->count && status == LKL_DEV_BLK_STATUS_OK; i++) {
		size_t len = req->buf[i].iov_len, done;

		if (req->type == LKL_DEV_BLK_TYPE_READ) {
			uint64_t t0 = host_now_ns();

			done = fread(req->buf[i].iov_base, 1, len, image->file);
			disk_read_ns += host_now_ns() - t0;
			disk_read_bytes += done;
		} else {
			done = fwrite(req->buf[i].iov_base, 1, len, image->file);
			image->written = 1;
		}
		if (done != len)
			status = LKL_DEV_BLK_STATUS_IOERR;
	}
	mutexUnlock(&image->lock);
	return status;
}

/* The guest's home: /root is the profile's image, and in it the downloads are
 * a folder of the card (hostfs_nx.c), seen and copied from it as they are. */
#define SD_DIR "/switch/chromium-nx"
static char old_profile_path[512];

/* The profile, the first time: an empty filesystem cannot be made here, so
 * one is in the program, packed (it packs to nothing: build-nx.sh), and is
 * unpacked next to it. */
extern const unsigned char gx_profile_gz[];
extern const unsigned int gx_profile_gz_len;

static void unpack_profile(const char *path)
{
	static u8 buffer[1 << 20];
	z_stream z = { 0 };
	FILE *out;
	int rc = Z_OK, ok = 1;
	long total = 0;

	if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK)
		return;
	out = fopen(path, "wb");
	host_log("gx: making %s, where the browser keeps what is to last (once, a moment)\n", path);
	z.next_in = (Bytef *)gx_profile_gz;
	z.avail_in = gx_profile_gz_len;
	while (out && rc == Z_OK) {
		z.next_out = buffer;
		z.avail_out = sizeof(buffer);
		rc = inflate(&z, Z_NO_FLUSH);
		if (rc != Z_OK && rc != Z_STREAM_END)
			break;
		if (fwrite(buffer, 1, sizeof(buffer) - z.avail_out, out) != sizeof(buffer) - z.avail_out) {
			ok = 0;
			break;
		}
		total += sizeof(buffer) - z.avail_out;
	}
	inflateEnd(&z);
	if (out && fclose(out))
		ok = 0;
	if (!out || !ok || rc != Z_STREAM_END || !total) {
		host_log("gx: could not make it (is the card full?)\n");
		remove(path);
	}
}

static void mount_home(int disk)
{
	int err = -1;

	if (disk >= 0) {
		/* Before the root is the root: its /root is where the profile
		 * goes, and the device's name cannot be made on a root that is
		 * read-only. */
		static const char profile_node[] = "/gx-profile-disk";
		long args[6] = { LKL_AT_FDCWD, (long)profile_node, LKL_S_IFBLK | 0600 };
		uint32_t dev = 0;

		err = lkl_get_virtio_blkdev(disk, 0, &dev);
		args[3] = dev;
		if (!err)
			err = lkl_syscall(__lkl__NR_mknodat, args);
		if (!err)
			err = lkl_sys_mount((char *)profile_node, "/gx-root/root", "ext4", 0, NULL);
		if (err)
			host_log("gx: cannot mount %s (%d): nothing will be kept\n", old_profile_path, err);
		else
			host_log("gx: what is to last is kept in %s\n", old_profile_path);
	}
	profile_mounted = !err;
	if (err && lkl_sys_mount("tmpfs", "/gx-root/root", "tmpfs", 0, "size=32m,mode=0755"))
		host_log("gx: no /root in memory either\n");

	/* The downloads: a folder of the card (hostfs_nx.c), over the image. */
	hostfs_nx_mkdirs(SD_DIR "/Downloads");
	lkl_sys_mkdir("/gx-root/root/Downloads", 0755);
	err = lkl_sys_mount("none", "/gx-root/root/Downloads", "hostfs", 0,
			    (void *)(SD_DIR "/Downloads" + 1));
	if (err)
		host_log("gx: cannot mount the downloads folder (%d)\n", err);
	else
		host_log("gx: downloads go to sdmc:" SD_DIR "/Downloads\n");

	/* The whole card, read only: where the files a page asks for are taken
	 * from (the file picker, overlay.c). Nothing is read of it until then. */
	lkl_sys_mkdir("/gx-root/root/SD", 0755);
	err = lkl_sys_mount("none", "/gx-root/root/SD", "hostfs", 1 /* MS_RDONLY */, (void *)"");
	if (err)
		host_log("gx: cannot show the SD card to pages (%d)\n", err);
}

/*
 * Chromium draws its own title bar buttons (minimize, maximize, close) unless
 * its "Use system title bar and borders" setting is on; then it leaves the
 * frame to the compositor, which here draws none: the window is the screen
 * and cannot be minimized or closed but from the overlay. The setting is
 * turned on in the profile, unless it has been set already (by the user, in
 * Chromium's settings, whichever way).
 */
static void system_title_bar(void)
{
	static const char path[] = "/root/profile/Default/Preferences", fresh[] = "/root/profile/Default/Preferences.gx";
	static const char setting[] = "\"custom_chrome_frame\":false";
	struct lkl_stat st;
	char *text, *out, *at;
	long n = 0;
	int fd;

	if (lkl_sys_stat(path, &st)) {
		/* A new profile: Chromium fills in the rest. */
		lkl_sys_mkdir("/root/profile", 0700);
		lkl_sys_mkdir("/root/profile/Default", 0700);
		fd = lkl_sys_open(path, LKL_O_WRONLY | LKL_O_CREAT | LKL_O_EXCL, 0600);
		if (fd >= 0) {
			lkl_sys_write(fd, "{\"browser\":{\"custom_chrome_frame\":false}}", 41);
			lkl_sys_close(fd);
		}
		return;
	}
	if (st.st_size <= 2 || st.st_size > (16 << 20) || !(text = malloc(st.st_size + 1)))
		return;
	fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
	while (fd >= 0 && n < st.st_size) {
		long got = lkl_sys_read(fd, text + n, st.st_size - n);

		if (got <= 0)
			break;
		n += got;
	}
	if (fd >= 0)
		lkl_sys_close(fd);
	text[n] = 0;
	if (n != st.st_size || text[0] != '{' || strstr(text, "\"custom_chrome_frame\"") ||
	    !(out = malloc(n + 64))) {
		free(text);
		return;
	}
	/* Into the "browser" object if there is one, else a new one at the top. */
	at = strstr(text, "\"browser\":{");
	if (at) {
		at += strlen("\"browser\":{");
		n = snprintf(out, n + 64, "%.*s%s%s%s", (int)(at - text), text, setting, *at == '}' ? "" : ",", at);
	} else {
		n = snprintf(out, n + 64, "{\"browser\":{%s}%s%s", setting, text[1] == '}' ? "" : ",", text + 1);
	}
	fd = lkl_sys_open(fresh, LKL_O_WRONLY | LKL_O_CREAT | LKL_O_TRUNC, 0600);
	if (fd >= 0) {
		int whole = lkl_sys_write(fd, out, n) == n;

		lkl_sys_close(fd);
		if (whole && !lkl_sys_rename(fresh, path))
			host_log("gx: Chromium is told to leave the title bar to us\n");
		else
			lkl_sys_unlink(fresh);
	}
	free(out);
	free(text);
}

/* A browser leaves these in its profile to say it is running, and takes them
 * away when it ends as it should; here it never does, it is ended. */
static void unlock_profile(void)
{
	lkl_sys_unlink("/root/profile/SingletonLock");
	lkl_sys_unlink("/root/profile/SingletonCookie");
	lkl_sys_unlink("/root/profile/SingletonSocket");
}

/* Under the name LKL's disk code looks for its default operations. */
struct lkl_dev_blk_ops lkl_dev_blk_ops = {
	.get_capacity = image_capacity,
	.request = image_request,
};

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

/* ---------- files out of the guest ---------- */

/*
 * The screen once the console has given it up: one picture at a time, drawn
 * as a texture over the whole of it. With GLES like the guest's own graphics:
 * a plain libnx framebuffer made after the console is gone fails on its first
 * frame (seen on hardware). All of it happens on the thread that calls
 * screen_show() first.
 */
static EGLDisplay screen_display;
static EGLSurface screen_surface;
static GLint screen_blue_first;
static int screen_ready;

static int screen_open(void)
{
	static const char vertex_source[] =
		"attribute vec2 p; varying vec2 t;\n"
		"void main() { t = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); gl_Position = vec4(p, 0.0, 1.0); }\n";
	/* b is 1 for pixels that come blue first (a Wayland client's). */
	static const char fragment_source[] =
		"precision mediump float; uniform sampler2D s; uniform float b; varying vec2 t;\n"
		"void main() { vec4 c = texture2D(s, t); gl_FragColor = vec4(mix(c.rgb, c.bgr, b), 1.0); }\n";
	static const GLfloat corners[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	static const EGLint config_attribs[] = {
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
	};
	static const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	const char *sources[2] = { vertex_source, fragment_source };
	EGLContext context;
	EGLConfig config;
	EGLint count = 0;
	GLuint program, texture;

	console_off();

	screen_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(screen_display, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API) ||
	    !eglChooseConfig(screen_display, config_attribs, &config, 1, &count) || count < 1) {
		host_log("gx: no EGL for the screen (%#x)\n", eglGetError());
		return -1;
	}
	screen_surface = eglCreateWindowSurface(screen_display, config, nwindowGetDefault(), NULL);
	context = eglCreateContext(screen_display, config, EGL_NO_CONTEXT, context_attribs);
	if (screen_surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
	    !eglMakeCurrent(screen_display, screen_surface, screen_surface, context)) {
		host_log("gx: no EGL surface for the screen (%#x)\n", eglGetError());
		return -1;
	}

	program = glCreateProgram();
	for (int i = 0; i < 2; i++) {
		GLuint shader = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(shader, 1, &sources[i], NULL);
		glCompileShader(shader);
		glAttachShader(program, shader);
	}
	glBindAttribLocation(program, 0, "p");
	glLinkProgram(program);
	glUseProgram(program);
	screen_blue_first = glGetUniformLocation(program, "b");
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corners);
	glEnableVertexAttribArray(0);
	glViewport(0, 0, 1280, 720);
	return 0;
}

/* Shows pixels of four bytes, red first or blue first; twice when the picture
 * is to stay (there are two buffers, and the screen shows either). */
static void screen_show(const u8 *pixels, int width, int height, int blue_first, int passes)
{
	if (!screen_ready && screen_open())
		return;
	screen_ready = 1;
	glUniform1f(screen_blue_first, blue_first ? 1.0f : 0.0f);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	for (int pass = 0; pass < passes; pass++) {
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		eglSwapBuffers(screen_display, screen_surface);
	}
}

/* Puts a PNG on the screen in place of the console: the page a headless
 * Chromium drew when it was asked for a picture. The text that follows only
 * goes to the log. */
static void show_picture(const char *path)
{
	png_image image = { .version = PNG_IMAGE_VERSION };
	u8 *pixels;

	if (!png_image_begin_read_from_file(&image, path))
		return;
	image.format = PNG_FORMAT_RGBA;
	pixels = malloc(PNG_IMAGE_SIZE(image));
	if (!pixels || !png_image_finish_read(&image, NULL, pixels, 0, NULL)) {
		png_image_free(&image);
		free(pixels);
		return;
	}
	host_log("gx: showing %s (%ux%u); the rest is in the log\n", path, image.width, image.height);
	sync_log();
	screen_show(pixels, image.width, image.height, 0, 2);
	host_log("gx: shown (GL error %#x)\n", glGetError());
	free(pixels);
}

/* ---------- a screen and a user for Chromium (cdp.c) ---------- */

/* Writes a number in the top left corner of a picture: the pictures per
 * second, while smoothness is what is being worked on. */
static void draw_number(u8 *pixels, int width, int height, int number)
{
	/* Digits of 3 by 5 dots, a row per three bits, top row first. */
	static const u16 digits[10] = {
		075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717,
	};
	enum { DOT = 6, GAP = 3 };
	char text[8];
	int count = snprintf(text, sizeof(text), "%d", number < 0 ? 0 : number > 999 ? 999 : number);
	int box_w = count * (3 * DOT + GAP) + GAP, box_h = 5 * DOT + 2 * GAP;

	if (box_w > width || box_h > height)
		return;
	for (int y = 0; y < box_h; y++)
		memset(pixels + (size_t)y * width * 4, 0, box_w * 4);
	for (int i = 0; i < count; i++) {
		u16 shape = digits[text[i] - '0'];

		for (int dot = 0; dot < 15; dot++) {
			int x0 = GAP + i * (3 * DOT + GAP) + dot % 3 * DOT, y0 = GAP + dot / 3 * DOT;

			if (!(shape >> (14 - dot) & 1))
				continue;
			for (int y = y0; y < y0 + DOT; y++)
				for (int x = x0; x < x0 + DOT; x++)
					memcpy(pixels + ((size_t)y * width + x) * 4, "\x30\xff\x30\xff", 4);
		}
	}
}

void host_frame(const uint8_t *jpeg, size_t len)
{
	static tjhandle decoder;
	static u8 *pixels;
	static int room, frames;
	int width = 0, height = 0, subsampling = 0, colorspace = 0;

	if (!decoder)
		decoder = tjInitDecompress();
	if (!decoder || tjDecompressHeader3(decoder, jpeg, len, &width, &height, &subsampling,
					    &colorspace) || width <= 0 || height <= 0)
		return;
	if (width * height * 4 > room) {
		free(pixels);
		room = width * height * 4;
		pixels = malloc(room);
	}
	if (!pixels || tjDecompress2(decoder, jpeg, len, pixels, width, 0, height, TJPF_RGBA,
				     TJFLAG_FASTDCT))
		return;
	if (!frames++)
		host_log("gx: first picture of the page (%dx%d); the rest is in the log\n", width, height);
	draw_number(pixels, width, height, cdp_frames_per_second);
	screen_show(pixels, width, height, 0, 1);
}

/* The window of a Chromium that has one (wl.c): its pixels as they are. */
void host_present(const uint8_t *pixels, int width, int height, int stride)
{
	static u8 *packed;
	static int room;

	/* The screen is the guest's own once it draws with the GPU. */
	if (window_surface != EGL_NO_SURFACE)
		return;
	if (stride != width * 4) {
		/* GLES 2 takes rows that follow each other. */
		if (width * height * 4 > room) {
			free(packed);
			room = width * height * 4;
			packed = malloc(room);
		}
		if (!packed)
			return;
		for (int y = 0; y < height; y++)
			memcpy(packed + (size_t)y * width * 4, pixels + (size_t)y * stride, width * 4);
		pixels = packed;
	}
	screen_show(pixels, width, height, 1, 1);
}

/*
 * The user: a finger on the screen is a finger on the page; the sticks
 * scroll; R is the Enter key and L the Backspace key. With the Chromium that
 * has a window (wl.c), the d-pad moves the focus about the page, A chooses
 * what has it (text gets the keyboard) and Y brings up the keyboard on the
 * address. With the headless one
 * (cdp.c), the d-pad scrolls too, A asks for text to type, B goes back, X
 * reloads and Y asks for an address. (+ quits, in main().)
 */
static int ask_text(const char *guide, const char *initial, char *out, size_t room)
{
	SwkbdConfig keyboard;
	Result rc = swkbdCreate(&keyboard, 0);

	out[0] = 0;
	if (R_FAILED(rc))
		return 0;
	swkbdConfigMakePresetDefault(&keyboard);
	swkbdConfigSetGuideText(&keyboard, guide);
	swkbdConfigSetInitialText(&keyboard, initial);
	rc = swkbdShow(&keyboard, out, room);
	swkbdClose(&keyboard);
	return R_SUCCEEDED(rc) && out[0];
}

static int wayland;   /* -w: the program wants a Wayland display (wl.c) */
/* -T: YouTube's TV page. It sends other browsers to the desktop site, so the
 * browser says it is the Switch's own; it fills the screen, and B is its Back
 * key (Escape) rather than the page history. */
static int tv_mode;
static int start_full_screen;   /* -F */
static int stay_720p;           /* -l */

/* 1920 x 1080 in the dock, 1280 x 720 out of it (wl.h); said to wl.c when
 * it changes. */
static u64 screen_changed;   /* when tv_screen() last changed it */

static void tv_screen(void)
{
	static int was = -1;
	int docked = appletGetOperationMode() == AppletOperationMode_Console;

	if (docked == was)
		return;
	if (was >= 0)
		screen_changed = armGetSystemTick();
	was = docked;
	wl_set_screen(docked ? 1920 : 1280, docked ? 1080 : 720);
	host_log("gx: %s: %s\n", docked ? "on a TV" : "handheld", docked ? "1920 x 1080" : "1280 x 720");
}
#define TV_USER_AGENT "Mozilla/5.0 (Nintendo Switch; WebApplet) AppleWebKit/609.4 (KHTML, like Gecko) " \
	"NF/6.0.2.20.5 NintendoBrowser/5.1.0.22023 Dalvik/2.1.0 (Linux; U; Android 5.1.1; AEOBC Build/LVY48f)"

int host_ask_text(char *text, size_t room)
{
	char initial[512];

	snprintf(initial, sizeof(initial), "%s", text);
	return ask_text("Text", initial, text, room);
}

/* What host_input() still has to give, when one look at the pad makes more
 * than one thing. */
#define QUEUED 64
static struct cdp_input queued[QUEUED];
static int queue_head, queue_count;
static int quit_wanted;   /* Quit, chosen in the overlay */
static int to_launcher;   /* and the launcher after (the overlay's Launcher) */

static void push(int type, int x, int y)
{
	struct cdp_input *q;

	if (queue_count == QUEUED)
		return;
	q = &queued[(queue_head + queue_count++) % QUEUED];
	q->type = type;
	q->x = x;
	q->y = y;
	q->finger = 0;
	q->text[0] = 0;
}

static void push_touch(int type, int finger, int x, int y)
{
	push(type, x, y);
	if (queue_count)
		queued[(queue_head + queue_count - 1) % QUEUED].finger = finger;
}

/* Keyboard and mouse mode (chosen in the overlay): the pad is a keyboard and
 * a mouse, for games made for them. What each input gives (keys.h) is the
 * launcher's to change: -K<key>,<key>,... in the order of keys.h. */
static const u64 game_buttons[GX_INPUTS] = {
	HidNpadButton_StickLUp, HidNpadButton_StickLDown, HidNpadButton_StickLLeft, HidNpadButton_StickLRight,
	HidNpadButton_A, HidNpadButton_B, HidNpadButton_X, HidNpadButton_Y,
	HidNpadButton_L, HidNpadButton_R, HidNpadButton_ZL, HidNpadButton_ZR,
	HidNpadButton_Up, HidNpadButton_Down, HidNpadButton_Left, HidNpadButton_Right,
	HidNpadButton_Minus, HidNpadButton_StickL, HidNpadButton_StickR,
};
static int game_bindings[GX_INPUTS];
static u64 game_held;

static void game_bindings_parse(const char *list)
{
	for (int i = 0; i < GX_INPUTS; i++)
		game_bindings[i] = gx_inputs[i].default_key;
	for (int i = 0; list && *list && i < GX_INPUTS; i++) {
		char *end;
		long key = strtol(list, &end, 0);

		if (end == list)
			break;
		game_bindings[i] = (int)key;
		list = *end == ',' ? end + 1 : end;
	}
}

static void game_key(int key, int down)
{
	if (!key)
		return;
	if (key == GX_WHEEL_UP || key == GX_WHEEL_DOWN) {
		/* A notch of the wheel for each press. */
		if (down)
			push(CDP_SCROLL, 0, key == GX_WHEEL_UP ? -53 : 53);
	} else if (key >= 0x110) {
		push(CDP_BUTTON, key, down);
	} else {
		push(down ? CDP_KEY_DOWN : CDP_KEY_UP, key, 0);
	}
}

/* Everything held is let go: when the overlay opens, or the mode changes. */
static void game_release(void)
{
	for (int i = 0; i < GX_INPUTS; i++)
		if (game_held & game_buttons[i])
			game_key(game_bindings[i], 0);
	game_held = 0;
}

static void game_input(PadState *pad)
{
	static u64 last;
	static float carry_x, carry_y;
	u64 held = padGetButtons(pad), now = armGetSystemTick();
	HidAnalogStickState stick = padGetStickPos(pad, 1);
	float seconds = armTicksToNs(now - last) / 1e9f, v[2] = { stick.x / 32767.0f, -stick.y / 32767.0f };

	for (int i = 0; i < GX_INPUTS; i++)
		if ((held ^ game_held) & game_buttons[i])
			game_key(game_bindings[i], !!(held & game_buttons[i]));
	game_held = held;
	/* The right stick moves the mouse: slowly near the middle, up to 1000
	 * pixels a second, however often the pad is looked at. */
	last = now;
	if (seconds > 0.05f)
		seconds = 0.05f;
	for (int i = 0; i < 2; i++) {
		float a = fabsf(v[i]);

		a = a < 0.12f ? 0 : (a - 0.12f) / 0.88f;
		v[i] = copysignf(a * a * 1000.0f * seconds, v[i]);
	}
	carry_x += v[0];
	carry_y += v[1];
	if ((int)carry_x || (int)carry_y) {
		push(CDP_MOTION, (int)carry_x, (int)carry_y);
		carry_x -= (int)carry_x;
		carry_y -= (int)carry_y;
	}
}

/*
 * Controller mode (chosen in the overlay): pages see the pad as a game
 * controller, through the Gamepad API. Chromium finds controllers the way it
 * does on any Linux: devices of the input subsystem in sysfs that udev has
 * marked as joysticks, read through joydev (/dev/input/js*). So the kernel
 * is given one (uinput) when it starts, before Chromium looks, and what udev
 * would have written about it is written here (there is no udev). It says it
 * is an Xbox 360 pad, with the buttons and axes of Linux's driver for one, so
 * that Chromium maps it as a standard controller. The buttons go by where they
 * are: the Switch's B is the Xbox's A.
 */
#define UI_DEV_CREATE 0x5501
#define UI_DEV_SETUP 0x405c5503
#define UI_ABS_SETUP 0x401c5504
#define UI_SET_EVBIT 0x40045564
#define UI_SET_KEYBIT 0x40045565
#define UI_SET_ABSBIT 0x40045567
#define EV_SYN 0
#define EV_KEY 1
#define EV_ABS 3

static int gamepad_fd = -1;

/* Linux's buttons of an Xbox 360 pad, in the order joydev numbers them, and
 * the Switch buttons that press them (0: none, the Xbox button). */
static const struct {
	int code;
	u64 button;
} gamepad_buttons[] = {
	{ 0x130, HidNpadButton_B },       /* BTN_A */
	{ 0x131, HidNpadButton_A },       /* BTN_B */
	{ 0x133, HidNpadButton_Y },       /* BTN_X */
	{ 0x134, HidNpadButton_X },       /* BTN_Y */
	{ 0x136, HidNpadButton_L },       /* BTN_TL */
	{ 0x137, HidNpadButton_R },       /* BTN_TR */
	{ 0x13a, HidNpadButton_Minus },   /* BTN_SELECT */
	{ 0x13b, 0 },                     /* BTN_START: + let go quickly, see gamepad_input() */
	{ 0x13c, 0 },                     /* BTN_MODE */
	{ 0x13d, HidNpadButton_StickL },  /* BTN_THUMBL */
	{ 0x13e, HidNpadButton_StickR },  /* BTN_THUMBR */
};
/* Its axes: the sticks, the triggers (0 to 255), the d-pad (-1 to 1). */
enum { AX_X, AX_Y, AX_Z, AX_RX, AX_RY, AX_RZ, AX_HAT_X, AX_HAT_Y, AXES };
static const int gamepad_axis_codes[AXES] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x10, 0x11 };
static int gamepad_state[sizeof(gamepad_buttons) / sizeof(gamepad_buttons[0]) + AXES];

static void read_small_file(const char *path, char *out, size_t room)
{
	int fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
	long n = fd >= 0 ? lkl_sys_read(fd, out, room - 1) : 0;

	out[n > 0 ? n : 0] = 0;
	if (fd >= 0)
		lkl_sys_close(fd);
}

static void write_small_file(const char *path, const char *text)
{
	int fd = lkl_sys_open(path, LKL_O_WRONLY | LKL_O_CREAT | LKL_O_TRUNC, 0644);

	if (fd >= 0) {
		lkl_sys_write(fd, text, strlen(text));
		lkl_sys_close(fd);
	}
}

static void gamepad_create(void)
{
	struct {
		uint16_t bustype, vendor, product, version;
		char name[80];
		uint32_t ff_effects_max;
	} setup = { 0x03, 0x045e, 0x028e, 0x0114, "Microsoft X-Box 360 pad", 0 };
	struct {
		uint16_t code, pad;
		int32_t value, minimum, maximum, fuzz, flat, resolution;
	} axis;
	char path[64], dev[32];
	int fd;

	lkl_sys_mknod("/dev/uinput", LKL_S_IFCHR | 0600, 10 << 8 | 223);
	fd = lkl_sys_open("/dev/uinput", LKL_O_WRONLY | LKL_O_NONBLOCK, 0);
	if (fd < 0) {
		host_log("gx: no controller for pages: no uinput (%d)\n", fd);
		return;
	}
	lkl_sys_ioctl(fd, UI_SET_EVBIT, EV_KEY);
	for (size_t i = 0; i < sizeof(gamepad_buttons) / sizeof(gamepad_buttons[0]); i++)
		lkl_sys_ioctl(fd, UI_SET_KEYBIT, gamepad_buttons[i].code);
	lkl_sys_ioctl(fd, UI_SET_EVBIT, EV_ABS);
	for (int i = 0; i < AXES; i++) {
		memset(&axis, 0, sizeof(axis));
		axis.code = gamepad_axis_codes[i];
		axis.minimum = i == AX_Z || i == AX_RZ ? 0 : i >= AX_HAT_X ? -1 : -32768;
		axis.maximum = i == AX_Z || i == AX_RZ ? 255 : i >= AX_HAT_X ? 1 : 32767;
		lkl_sys_ioctl(fd, UI_SET_ABSBIT, axis.code);
		lkl_sys_ioctl(fd, UI_ABS_SETUP, (long)&axis);
	}
	if (lkl_sys_ioctl(fd, UI_DEV_SETUP, (long)&setup) < 0 || lkl_sys_ioctl(fd, UI_DEV_CREATE, 0) < 0) {
		host_log("gx: no controller for pages: uinput said no\n");
		lkl_sys_close(fd);
		return;
	}
	gamepad_fd = fd;
	/* Its nodes in /dev, and udev's word that they are a joystick. */
	lkl_sys_mkdir("/dev/input", 0755);
	lkl_sys_mkdir("/run/udev", 0755);
	lkl_sys_mkdir("/run/udev/data", 0755);
	for (int kind = 0; kind < 2; kind++)
		for (int n = 0; n < 8; n++) {
			unsigned major, minor;

			snprintf(path, sizeof(path), "/sys/class/input/%s%d/dev", kind ? "event" : "js", n);
			read_small_file(path, dev, sizeof(dev));
			if (sscanf(dev, "%u:%u", &major, &minor) != 2)
				continue;
			snprintf(path, sizeof(path), "/dev/input/%s%d", kind ? "event" : "js", n);
			lkl_sys_mknod(path, LKL_S_IFCHR | 0666, major << 8 | minor);
			snprintf(path, sizeof(path), "/run/udev/data/c%u:%u", major, minor);
			write_small_file(path, "I:1000000\nE:ID_INPUT=1\nE:ID_INPUT_JOYSTICK=1\n"
					       "E:ID_BUS=usb\nE:ID_VENDOR_ID=045e\nE:ID_MODEL_ID=028e\n");
			host_log("gx: a controller for pages: /dev/input/%s%d (%u:%u)\n", kind ? "event" : "js", n,
				 major, minor);
		}
}

static void gamepad_send(int type, int code, int value)
{
	struct {
		int64_t seconds, microseconds;
		uint16_t type, code;
		int32_t value;
	} event = { 0, 0, (uint16_t)type, (uint16_t)code, value };

	lkl_sys_write(gamepad_fd, (const char *)&event, sizeof(event));
}

/* The pad as it is now (neutral: nothing pressed), to Chromium. + pressed
 * and let go within half a second is Start, held longer the overlay. */
static u64 gamepad_start_until;   /* Start is pressed until then (gamepad_plus()) */

static void gamepad_input(PadState *pad, int neutral)
{
	const int buttons = sizeof(gamepad_buttons) / sizeof(gamepad_buttons[0]);
	u64 held = neutral ? 0 : padGetButtons(pad), now = armGetSystemTick();
	HidAnalogStickState left = neutral ? (HidAnalogStickState){ 0 } : padGetStickPos(pad, 0);
	HidAnalogStickState right = neutral ? (HidAnalogStickState){ 0 } : padGetStickPos(pad, 1);
	int state[sizeof(gamepad_state) / sizeof(gamepad_state[0])], changed = 0;

	if (gamepad_fd < 0)
		return;
	for (int i = 0; i < buttons; i++)
		state[i] = gamepad_buttons[i].button && (held & gamepad_buttons[i].button);
	state[7] = !neutral && gamepad_start_until && now < gamepad_start_until;   /* Start */
	if (!state[7])
		gamepad_start_until = 0;
	state[buttons + AX_X] = left.x;
	state[buttons + AX_Y] = -left.y;
	state[buttons + AX_RX] = right.x;
	state[buttons + AX_RY] = -right.y;
	state[buttons + AX_Z] = held & HidNpadButton_ZL ? 255 : 0;
	state[buttons + AX_RZ] = held & HidNpadButton_ZR ? 255 : 0;
	state[buttons + AX_HAT_X] = (held & HidNpadButton_Right ? 1 : 0) - (held & HidNpadButton_Left ? 1 : 0);
	state[buttons + AX_HAT_Y] = (held & HidNpadButton_Down ? 1 : 0) - (held & HidNpadButton_Up ? 1 : 0);
	for (int i = 0; i < buttons + AXES; i++) {
		if (state[i] == gamepad_state[i])
			continue;
		if (i < buttons)
			gamepad_send(EV_KEY, gamepad_buttons[i].code, state[i]);
		else
			gamepad_send(EV_ABS, gamepad_axis_codes[i - buttons], state[i]);
		gamepad_state[i] = state[i];
		changed = 1;
	}
	if (changed)
		gamepad_send(EV_SYN, 0, 0);
}

/* + in controller mode: 1 when it has been held long enough to open the
 * overlay; let go sooner, it presses Start for a tenth of a second. */
static int gamepad_plus(PadState *pad, u64 down)
{
	static u64 since;
	static int used;
	u64 now = armGetSystemTick();

	if (padGetButtons(pad) & HidNpadButton_Plus) {
		/* Only a press seen from its start: not the + that closed the
		 * overlay, still held. */
		if (!since && !(down & HidNpadButton_Plus))
			return 0;
		if (!since) {
			since = now;
			used = 0;
		}
		if (!used && armTicksToNs(now - since) > 500000000ULL) {
			used = 1;
			return 1;
		}
		return 0;
	}
	if (since && !used)
		gamepad_start_until = now + armNsToTicks(100000000ULL);
	since = 0;
	return 0;
}

/* What was chosen in the overlay; it closes. */
static int overlay_chose(int what, struct cdp_input *in)
{
	if (what == OVERLAY_NONE)
		return 0;
	overlay_close();
	switch (what) {
	case OVERLAY_FULL_SCREEN:
		in->type = CDP_KEY;
		in->x = 87;   /* F11 */
		return 1;
	case OVERLAY_ORIENTATION:
		wl_set_portrait(!wl_portrait());
		return 0;
	case OVERLAY_COPY:
	case OVERLAY_PASTE:
		in->type = CDP_CONTROL_KEY;
		in->x = what == OVERLAY_COPY ? 46 /* C */ : 47 /* V */;
		return 1;
	case OVERLAY_PICKED: {
		/* The card is /root/SD in the guest (mount_home()). */
		char path[1100];

		snprintf(path, sizeof(path), "/root/SD/%s", overlay_picked() + strlen("sdmc:/"));
		cdp_file_chosen(path);
		return 0;
	}
	case OVERLAY_KEYBOARD:
		if (ask_text("Text to type", "", in->text, sizeof(in->text))) {
			in->type = CDP_TEXT;
			return 1;
		}
		return 0;
	case OVERLAY_LAUNCHER:
		to_launcher = 1;
		/* fall through */
	case OVERLAY_QUIT:
		__atomic_store_n(&quit_wanted, 1, __ATOMIC_SEQ_CST);
		return 0;
	}
	return 0;
}

/* The browser's controls (overlay mode Browser): the left stick moves a
 * mouse pointer, which A clicks with while it is shown; the right stick
 * scrolls, both ways. The pointer is drawn over the screen by glwin.c
 * (gl_cursor()); it goes when a finger or the d-pad is used. */
static int cursor_shown, a_is_click;
static u64 cursor_moved;   /* when it last moved or went: the page is drawn meanwhile */

static void cursor_hide(u64 now)
{
	if (cursor_shown) {
		cursor_shown = 0;
		cursor_moved = now;
	}
}

static void browser_sticks(PadState *pad, u64 now)
{
	static u64 last, last_scroll;
	static float carry_x, carry_y;
	HidAnalogStickState l = padGetStickPos(pad, 0), r = padGetStickPos(pad, 1);
	float seconds = last ? armTicksToNs(now - last) / 1e9f : 0, v[2] = { l.x / 32767.0f, -l.y / 32767.0f };
	int moving = 0;

	/* Portrait: the console is held turned, its left side up. */
	if (wl_portrait()) {
		HidAnalogStickState t = l;

		v[0] = t.y / 32767.0f;
		v[1] = t.x / 32767.0f;
		t = r;
		r.x = t.y;
		r.y = -t.x;
	}

	last = now;
	if (seconds > 0.05f)
		seconds = 0.05f;
	for (int i = 0; i < 2; i++) {
		float a = fabsf(v[i]);

		a = a < 0.15f ? 0 : (a - 0.15f) / 0.85f;
		moving |= a > 0;
		/* Up to 900 pixels a second, slowly near the middle. */
		v[i] = copysignf(a * a * 900.0f * seconds, v[i]);
	}
	if (moving) {
		cursor_shown = 1;
		cursor_moved = now;
		carry_x += v[0];
		carry_y += v[1];
		if ((int)carry_x || (int)carry_y) {
			push(CDP_MOTION, (int)carry_x, (int)carry_y);
			carry_x -= (int)carry_x;
			carry_y -= (int)carry_y;
		}
	}
	/* The right stick: sixty steps a second at most, one per picture. A
	 * step of Wayland's unit is about 13 pixels to Chromium, so the steps
	 * are fractions of one: from a fifth (150 pixels a second) just past
	 * the middle to 3.2 (2400 a second) pushed all the way. Whole units,
	 * the slowest was 800 a second and the fastest too fast to read. */
	if (armTicksToNs(now - last_scroll) > 16000000ULL &&
	    (abs(r.x) > 6000 || abs(r.y) > 6000)) {
		float sx = abs(r.x) > 6000 ? (abs(r.x) - 6000) / 26767.0f : 0;
		float sy = abs(r.y) > 6000 ? (abs(r.y) - 6000) / 26767.0f : 0;
		int fx = abs(r.x) > 6000 ? (int)((0.2f + 3.0f * sx * sx) * 256) : 0;
		int fy = abs(r.y) > 6000 ? (int)((0.2f + 3.0f * sy * sy) * 256) : 0;

		last_scroll = now;
		push(CDP_SCROLL_FINE, r.x > 0 ? fx : -fx, r.y > 0 ? -fy : fy);
	}
}

/* The page drawn at every frame while something of ours over it changes:
 * the overlay, or the pointer moving (cdp_repaint()); and for a while after
 * the console went into its dock or out of it, when the screen is made
 * again and shows nothing until it is drawn on (tv_screen()). */
static void update_repaint(u64 now)
{
	static int on;
	int want = overlay_is_open() || (cursor_moved && armTicksToNs(now - cursor_moved) < 500000000ULL) ||
		   (screen_changed && armTicksToNs(now - screen_changed) < 2000000000ULL);

	if (want != on) {
		cdp_repaint(want);
		on = want;
	}
}

static const uint32_t *gl_cursor(int *x, int *y, int *width, int *height)
{
	if (!cursor_shown || overlay_is_open() || !wl_pointer_at(x, y))
		return NULL;
	return overlay_cursor(width, height);
}

static int poll_input(struct cdp_input *in)
{
	static PadState pad;
	static int ready, overlay_finger;
	static u64 last_scroll;
	HidTouchScreenState touch = { 0 };
	HidAnalogStickState left, right;
	u64 down, now = armGetSystemTick();
	int dy;

	if (!ready) {
		padInitializeAny(&pad);
		hidInitializeTouchScreen();
		ready = 1;
	}
	memset(in, 0, sizeof(*in));
	if (wayland) {
		/* A page wants a file: the picker (overlay.c), over anything. */
		if (cdp_file_wanted()) {
			overlay_close();
			game_release();
			overlay_picker_open();
		}
		update_repaint(now);
	}

	/* The fingers on the touch screen, each told apart (two pinch: the page
	 * zooms). Without a window, only the first. */
	{
		static struct {
			int used, x, y;
			u32 id;
		} fingers[WL_FINGERS];
		int count = hidGetTouchScreenStates(&touch, 1) ? (int)touch.count : 0, any = 0;
		int xs[16], ys[16];

		if (count > 16)
			count = 16;
		if (!wayland && count > 1)
			count = 1;
		for (int i = 0; i < count; i++) {
			xs[i] = touch.touches[i].x;
			ys[i] = touch.touches[i].y;
			/* Portrait: the picture's top is on the screen's left (gl_screen()). */
			if (wayland && wl_portrait()) {
				int t = xs[i];

				xs[i] = WL_SCREEN_HEIGHT - 1 - ys[i];
				ys[i] = t;
			}
		}
		for (int f = 0; f < WL_FINGERS; f++)
			any |= fingers[f].used;
		if (count) {
			cursor_hide(now);
			/* A finger put down while the overlay is open is the
			 * overlay's, and all of them until they are lifted. */
			if (!any && !overlay_finger && overlay_is_open()) {
				overlay_finger = 1;
				return overlay_chose(overlay_touch(xs[0], ys[0]), in);
			}
			if (overlay_finger)
				return 0;
		} else {
			overlay_finger = 0;
		}
		/* Lifted. */
		for (int f = 0; f < WL_FINGERS; f++) {
			int still = 0;

			for (int i = 0; i < count && fingers[f].used; i++)
				still |= touch.touches[i].finger_id == fingers[f].id;
			if (fingers[f].used && !still) {
				fingers[f].used = 0;
				push_touch(CDP_TOUCH_UP, f, fingers[f].x, fingers[f].y);
			}
		}
		/* Put down, or moved. */
		for (int i = 0; i < count; i++) {
			int f = -1;

			for (int j = 0; j < WL_FINGERS && f < 0; j++)
				if (fingers[j].used && fingers[j].id == touch.touches[i].finger_id)
					f = j;
			if (f < 0) {
				for (int j = 0; j < WL_FINGERS && f < 0; j++)
					if (!fingers[j].used)
						f = j;
				if (f < 0)
					continue;   /* more fingers than are told apart */
				fingers[f].used = 1;
				fingers[f].id = touch.touches[i].finger_id;
				fingers[f].x = xs[i];
				fingers[f].y = ys[i];
				push_touch(CDP_TOUCH_DOWN, f, xs[i], ys[i]);
			} else if (abs(xs[i] - fingers[f].x) + abs(ys[i] - fingers[f].y) >= 2) {
				fingers[f].x = xs[i];
				fingers[f].y = ys[i];
				push_touch(CDP_TOUCH_MOVE, f, xs[i], ys[i]);
			}
		}
		/* The pad is not looked at while fingers are down. */
		if (count || any)
			return 0;
	}

	padUpdate(&pad);
	down = padGetButtonsDown(&pad);
	/* + opens the overlay (overlay.c), which has the pad until it closes;
	 * held, in controller mode, where a short press is Start. */
	if (wayland && !overlay_is_open() &&
	    (overlay_mode() == OVERLAY_CONTROLLER ? gamepad_plus(&pad, down) :!!(down & HidNpadButton_Plus))) {
		game_release();
		gamepad_input(&pad, 1);
		overlay_open(wl_full_screen());
		return 0;
	}
	if (wayland && overlay_is_open()) {
		overlay_full_screen(wl_full_screen());
		return overlay_chose(overlay_pad(down), in);
	}
	if (wayland && overlay_mode() == OVERLAY_KEYBOARD_MOUSE) {
		game_input(&pad);
		return 0;
	}
	if (game_held)
		game_release();
	if (wayland && overlay_mode() == OVERLAY_CONTROLLER) {
		gamepad_input(&pad, 0);
		return 0;
	}
	if (wayland) {
		u64 up = padGetButtonsUp(&pad);

		browser_sticks(&pad, now);
		/* A is the mouse's button while the pointer is shown, held as
		 * long as A is (a drag). */
		if (cursor_shown && (down & HidNpadButton_A)) {
			push(CDP_BUTTON, 0x110, 1);
			a_is_click = 1;
			down &= ~HidNpadButton_A;
		}
		if (a_is_click && (up & HidNpadButton_A)) {
			push(CDP_BUTTON, 0x110, 0);
			a_is_click = 0;
		}
		if (down & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right))
			cursor_hide(now);
	}
	if ((down & HidNpadButton_B) && tv_mode) {
		in->type = CDP_KEY;
		in->x = 1;   /* Escape */
	} else if (down & HidNpadButton_B) {
		in->type = CDP_BACK;
	} else if (down & HidNpadButton_X) {
		in->type = CDP_RELOAD;
	} else if (down & HidNpadButton_R) {
		in->type = CDP_ENTER;
	} else if (down & HidNpadButton_L) {
		in->type = CDP_BACKSPACE;
	} else if (wayland && (down & HidNpadButton_A)) {
		/* What has the focus is chosen: a link is followed, a button
		 * pressed, and text gets the keyboard. */
		in->type = CDP_ACTIVATE;
	} else if (wayland && (down & HidNpadButton_Y)) {
		/* The keyboard, on the address of the window (none in TV mode). */
		if (!tv_mode)
			in->type = CDP_KEYBOARD;
	} else if (wayland && (down & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left |
				       HidNpadButton_Right))) {
		/* The arrow keys, which move the focus from one thing of the
		 * page to the next in that direction (Chromium is started with
		 * --enable-spatial-navigation). */
		in->type = CDP_KEY;
		in->x = (down & HidNpadButton_Up) ? 103 : (down & HidNpadButton_Down) ? 108 :
			(down & HidNpadButton_Left) ? 105 : 106;
		/* Portrait, the console turned: its up is the picture's right. */
		if (wl_portrait())
			in->x = in->x == 103 ? 106 : in->x == 106 ? 108 : in->x == 108 ? 105 : 103;
	} else if (wayland && (down & (HidNpadButton_B | HidNpadButton_X))) {
		/* Nothing: the window has buttons of its own for those. */
	} else if (down & HidNpadButton_Y) {
		if (ask_text("Address", "https://", in->text, sizeof(in->text)))
			in->type = CDP_NAVIGATE;
	} else if (down & HidNpadButton_A) {
		if (ask_text("Text to type", "", in->text, sizeof(in->text)))
			in->type = CDP_TEXT;
	} else if (down & (HidNpadButton_Up | HidNpadButton_Down)) {
		in->type = CDP_SCROLL;
		in->y = (down & HidNpadButton_Down) ? 240 : -240;
	} else if (!wayland && armTicksToNs(now - last_scroll) > 16000000ULL) {
		/* The sticks, sixty times a second while they are pushed: at
		 * most one step per picture the screen can show. */
		left = padGetStickPos(&pad, 0);
		right = padGetStickPos(&pad, 1);
		dy = abs(left.y) > abs(right.y) ? left.y : right.y;
		if (abs(dy) > 6000) {
			last_scroll = now;
			in->type = CDP_SCROLL;
			in->y = -dy / 1200;
		}
	}
	return in->type != CDP_NONE;
}

int host_input(struct cdp_input *in)
{
	if (!queue_count && poll_input(in))
		return 1;
	if (!queue_count)
		return 0;
	*in = queued[queue_head];
	queue_head = (queue_head + 1) % QUEUED;
	queue_count--;
	return 1;
}

static const char *export_path;   /* a guest file to copy to sdmc:/gx when the guest exits */

static void export_file(void)
{
	static char buf[1 << 16];
	const char *name = export_path ? strrchr(export_path, '/') : NULL;
	char host[512];
	FILE *out;
	long n;
	int in;

	if (!name)
		return;
	in = lkl_sys_open(export_path, LKL_O_RDONLY, 0);
	if (in < 0) {
		host_log("gx: the guest left no %s\n", export_path);
		return;
	}
	snprintf(host, sizeof(host), GX_DIR "%s", name);
	out = fopen(host, "wb");
	while (out && (n = lkl_sys_read(in, buf, sizeof(buf))) > 0)
		fwrite(buf, 1, n, out);
	if (out)
		fclose(out);
	host_log("gx: %s copied to %s\n", export_path, host);
	if (strlen(host) > 4 && !strcmp(host + strlen(host) - 4, ".png"))
		show_picture(host);
}

/* ---------- aborts ---------- */

extern char _start[];

/* These replace libnx's and newlib's, which end the process on the spot: an
 * abort of host code (libnx, Mesa, newlib) would otherwise leave no trace. */
static void report_kernel_use(void);

/* Only the thread that aborted stops: the others may still say something. */
static void stop_here(void)
{
	report_kernel_use();
	host_log("This thread stops here. Press + to quit.\n");
	sync_log();
	crashed = 1;
	for (;;)
		svcSleepThread(1000000000ULL);
}

void diagAbortWithResult(Result res)
{
	/* Not on the screen if it is the console that fails: this very thread
	 * is the one that would have shown it. */
	if (res == MAKERESULT(Module_Libnx, LibnxError_BadGfxDequeueBuffer))
		console_on = 0;
	host_log("\ngx: the host aborted with result %#x (module %u, description %u), called from %p"
		 " (gx is at %p)\n",
		 res, R_MODULE(res), R_DESCRIPTION(res), __builtin_return_address(0), _start);
	stop_here();
	__builtin_unreachable();
}

void abort(void)
{
	host_log("\ngx: the host called abort(), from %p\n", __builtin_return_address(0));
	stop_here();
	__builtin_unreachable();
}

/* ---------- sysroot import ---------- */

static int import_tree(const char *host, const char *guest)
{
	static char buf[1 << 16];
	DIR *d = opendir(host);
	struct dirent *e;

	if (!d) {
		host_log("gx: cannot read %s\n", host);
		return -1;
	}
	while ((e = readdir(d))) {
		char hp[768], gp[768];
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
			FILE *in = fopen(hp, "rb");
			int out = lkl_sys_open(gp, LKL_O_WRONLY | LKL_O_CREAT | LKL_O_TRUNC, 0755);
			size_t n;

			if (!in || out < 0) {
				host_log("gx: cannot import %s (%d)\n", hp, out);
				if (in)
					fclose(in);
				continue;
			}
			while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
				lkl_sys_write(out, buf, n);
			fclose(in);
			lkl_sys_close(out);
		}
	}
	closedir(d);
	return 0;
}

/* ---------- the menu and the run ---------- */

#define MAX_COMMANDS 24
#define MAX_ARGS 64

static char commands[MAX_COMMANDS][4096];
static const char *root_image_name;   /* @<image>: an ext4 image next to the program as the root */
static const char *profile_name;      /* =<image>: one that is the guest's /root, and written */
static int browser;                   /* -b: the program is a Chromium to drive (cdp.c) */
static int cpu_count;                 /* -c<n>: processors the guest is told about */
static int profiling;                 /* -p: see profiler() */
static const char *experiment = "";   /* -x<letters>: b no kernel priority, h the kernel tickless */
static int command_count;
static char *guest_argv[MAX_ARGS + 1];
static int guest_argc;

static void read_commands(void)
{
	FILE *f = fopen(GX_DIR "/launch.txt", "r");
	char line[4096];

	/* What the launcher (gx/launcher) chose, once: it then loaded us in its
	 * place. Started otherwise, programs.txt as before. */
	if (f) {
		if (fgets(line, sizeof(line), f)) {
			line[strcspn(line, "\r\n")] = 0;
			if (line[0])
				strcpy(commands[command_count++], line);
		}
		fclose(f);
		remove(GX_DIR "/launch.txt");
		if (command_count)
			return;
	}
	f = fopen(GX_DIR "/programs.txt", "r");

	while (f && command_count < MAX_COMMANDS && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] && line[0] != '#')
			strcpy(commands[command_count++], line);
	}
	if (f)
		fclose(f);
	/* No list: the default, without a menu (see main()). */
	if (!command_count) {
		strcpy(commands[0], DEFAULT_COMMAND);
		command_count = 1;
	}
}

/* The system's error dialog, over the browser's last picture. */
static void say_crashed(void)
{
	ErrorApplicationConfig c;

	if (R_FAILED(errorApplicationCreate(&c,
			ran_out ? "Chromium NX ran out of memory and has to close."
				: "Chromium NX stopped working and has to close.",
			ran_out ? "The page needed more memory than Chromium NX has.\n\n"
				  "The log is sdmc:/switch/chromium-nx/chromium-nx.log."
				: "The log is sdmc:/switch/chromium-nx/chromium-nx.log.")))
		return;
	errorApplicationShow(&c);
}

/* Returns the command chosen with the d-pad and A, or -1 if + was pressed. */
static int menu(PadState *pad)
{
	int choice = 0, drawn = -1;

	while (appletMainLoop()) {
		u64 down;

		if (drawn != choice) {
			consoleClear();
			printf("Chromium NX\n\n");
			for (int i = 0; i < command_count; i++) {
				/* "[label] command" shows as its label. */
				char *end = commands[i][0] == '[' ? strchr(commands[i], ']') : NULL;

				if (end)
					printf(" %c %.*s\n", i == choice ? '>' : ' ',
					       (int)(end - commands[i] - 1), commands[i] + 1);
				else
					printf(" %c %.74s\n", i == choice ? '>' : ' ', commands[i]);
			}
			printf("\nUp/Down: choose   A: run   +: quit\n");
			drawn = choice;
		}
		consoleUpdate(NULL);
		padUpdate(pad);
		down = padGetButtonsDown(pad);
		if (down & HidNpadButton_Plus)
			return -1;
		if (down & HidNpadButton_A)
			return choice;
		if (down & HidNpadButton_AnyDown)
			choice = (choice + 1) % command_count;
		if (down & HidNpadButton_AnyUp)
			choice = (choice + command_count - 1) % command_count;
		svcSleepThread(16666666ULL);
	}
	return -1;
}

static void kernel_print(const char *str, int len)
{
	host_console_write(2, str, len);
}

/* Lets a thread run on any of the three cores, keeping the one it prefers.
 * Horizon does not share a core between threads of one priority: whoever runs
 * keeps it until it waits for something, and the others tied to that core
 * wait for as long. A start that hung now and then, + doing nothing, was
 * that: the main thread, the kernel's threads and ours were all tied to one
 * core. */
static void free_cores(Handle thread)
{
	svcSetThreadCoreMask(thread, -3, 0x7);
}

static lkl_thread_t (*kernel_thread_create)(void (*)(void *), void *);

static lkl_thread_t create_kernel_thread(void (*fn)(void *), void *arg)
{
	lkl_thread_t t = kernel_thread_create(fn, arg);

	/* What nx_host.c hands out is a Thread of libnx. */
	if (t) {
		free_cores(((Thread *)t)->handle);
		know_thread(((Thread *)t)->handle, "kernel");
	}
	return t;
}

/* What a syscall of the kernel costs here, alone and with a second thread
 * making them at the same time: the floor under everything the guest does. */
static volatile int bench_other_run;

static void bench_other(void *arg)
{
	host_in_kernel(1);
	while (bench_other_run)
		lkl_sys_getppid();
	bench_other_run = -1;
	host_thread_exit();
}

/* The floor: two threads that wake each other in turn, with what LKL's
 * semaphores are made of (a mutex and a condition variable). */
static struct { Mutex m; CondVar cv; int turn; } pingpong;

static void bench_pong(void *arg)
{
	int n = (int)(long)arg;

	for (int i = 0; i < n; i++) {
		mutexLock(&pingpong.m);
		while (pingpong.turn != 1)
			condvarWait(&pingpong.cv, &pingpong.m);
		pingpong.turn = 0;
		condvarWakeAll(&pingpong.cv);
		mutexUnlock(&pingpong.m);
	}
	host_thread_exit();
}

static void bench_pingpong(void)
{
	int n = 1000;
	uint64_t t0;

	mutexInit(&pingpong.m);
	condvarInit(&pingpong.cv);
	if (host_thread_start(bench_pong, (void *)(long)n))
		return;
	t0 = host_now_ns();
	for (int i = 0; i < n; i++) {
		mutexLock(&pingpong.m);
		pingpong.turn = 1;
		condvarWakeAll(&pingpong.cv);
		while (pingpong.turn != 0)
			condvarWait(&pingpong.cv, &pingpong.m);
		mutexUnlock(&pingpong.m);
	}
	host_log("gx: a thread wakes another in %lu ns\n",
		 (unsigned long)((host_now_ns() - t0) / n / 2));
}

/* How fast the processor goes: a chain of dependent additions is one a
 * cycle on a Cortex-A57. */
static void bench_clock(const char *where)
{
	uint64_t t0 = host_now_ns(), x = 0;

	for (int i = 0; i < 10000000; i++)
		__asm__ volatile("add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
				 "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1"
				 : "+r"(x));
	host_log("gx: %s: the processor runs at about %lu MHz\n", where,
		 (unsigned long)(80000000ULL * 1000 / (host_now_ns() - t0)));
}

static void bench_syscalls(void)
{
	uint64_t t0;
	int n = 2000;

	bench_clock("one thread");
	bench_pingpong();
	host_in_kernel(1);
	lkl_sys_getppid();
	t0 = host_now_ns();
	for (int i = 0; i < n; i++)
		lkl_sys_getppid();
	host_log("gx: a syscall of the kernel takes %lu ns alone\n",
		 (unsigned long)((host_now_ns() - t0) / n));
	bench_other_run = 1;
	if (host_thread_start(bench_other, NULL))
		return;
	svcSleepThread(20000000ULL);
	{
		extern unsigned long nx_counts[6];
		unsigned long before[6], calls = 0;

		memcpy(before, nx_counts, sizeof(before));
		t0 = host_now_ns();
		for (int i = 0; i < n; i++)
			lkl_sys_getppid();
		host_log("gx: and %lu ns with another thread making them too\n",
			 (unsigned long)((host_now_ns() - t0) / n));
		/* Both threads' calls: about twice ours. */
		calls = 2 * n;
		host_log("gx: per syscall then: %lu.%02lu sem_up, %lu.%02lu sem_down (%lu.%02lu waited), "
			 "%lu.%02lu timer_set, %lu.%02lu mutex_lock, %lu thread_create\n",
			 (nx_counts[0] - before[0]) / calls, (nx_counts[0] - before[0]) * 100 / calls % 100,
			 (nx_counts[1] - before[1]) / calls, (nx_counts[1] - before[1]) * 100 / calls % 100,
			 (nx_counts[2] - before[2]) / calls, (nx_counts[2] - before[2]) * 100 / calls % 100,
			 (nx_counts[3] - before[3]) / calls, (nx_counts[3] - before[3]) * 100 / calls % 100,
			 (nx_counts[4] - before[4]) / calls, (nx_counts[4] - before[4]) * 100 / calls % 100,
			 nx_counts[5] - before[5]);
	}
	bench_other_run = 0;
	while (bench_other_run != -1)
		svcSleepThread(1000000ULL);
	host_in_kernel(0);
}

static void run(void *arg)
{
	static char *guest_env[] = {
		"PATH=/bin:/usr/bin", "HOME=/root", "LD_LIBRARY_PATH=/lib", "TMPDIR=/tmp",
		/* A browser's H.264 goes to the console's decoder (guest/video.c);
		 * a root without the library says so and goes on. */
		"LD_PRELOAD=/lib/libgxvideo.so",
		/* Where wl.c is, for the programs that look for a display. */
		"XDG_RUNTIME_DIR=/run", "WAYLAND_DISPLAY=wayland-0", NULL,
	};
	struct __lkl__kernel_timespec now = { .tv_sec = time(NULL) };
	long params[6] = { LKL_CLOCK_REALTIME, (long)&now };
	int ret, net_id = -1, disk_id = -1, profile_id = -1;

	host_log("gx: starting the kernel (%s)\n", strchr(experiment, 'h') ? KERNEL_ARGS_NOHZ : KERNEL_ARGS);
	/* Where LKL's virtio devices tell the kernel they exist. */
	lkl_host_ops.virtio_devices = lkl_virtio_devs;
	/* The kernel's messages go where ours do. The function nx_host.c has
	 * for them writes to the console whether there is one or not, and the
	 * console's memory is gone once a program has the screen: a kernel
	 * warning in the middle of a web page then stopped everything. */
	lkl_host_ops.print = kernel_print;
	kernel_thread_create = lkl_host_ops.thread_create;
	lkl_host_ops.thread_create = create_kernel_thread;
	ret = lkl_init(&lkl_host_ops);
	if (!ret)
		net_id = net_add();
	if (!ret && root_image_name) {
		char path[512];

		snprintf(path, sizeof(path), GX_DIR "/%s", root_image_name);
		root_image.file = fopen(path, "rb");
		if (root_image.file) {
			FsFileSystem *sd = fsdevGetDeviceFileSystem("sdmc");

			if (sd && R_SUCCEEDED(fsFsOpenFile(sd, path + strlen("sdmc:"), FsOpenMode_Read,
							   &root_image.direct)))
				root_image.has_direct = 1;
			/* The SD card is slow by small pieces. */
			setvbuf(root_image.file, NULL, _IOFBF, 1 << 20);
			disk_id = lkl_disk_add(&root_disk);
		}
		if (disk_id < 0) {
			host_log("gx: cannot use %s as a disk (%d)\n", path, disk_id);
			host_exit(1);
		}
	}
	/* The profile is an image, made the first time from the one in the
	 * program; the downloads are a folder of the card (hostfs_nx.c). */
	if (hostfs_nx_init())
		host_log("gx: the SD card cannot be shared with the kernel: no downloads folder\n");
	if (!ret && profile_name && disk_id >= 0) {
		snprintf(old_profile_path, sizeof(old_profile_path), GX_DIR "/%s", profile_name);
		profile_image.file = fopen(old_profile_path, "r+b");
		if (!profile_image.file) {
			unpack_profile(old_profile_path);
			profile_image.file = fopen(old_profile_path, "r+b");
		}
		if (profile_image.file) {
			setvbuf(profile_image.file, NULL, _IOFBF, 1 << 16);
			profile_id = lkl_disk_add(&profile_disk);
		}
		if (profile_id < 0)
			host_log("gx: cannot use %s as a disk: nothing will be kept\n", old_profile_path);
	}
	if (!ret)
		ret = lkl_start_kernel(strchr(experiment, 'h') ? KERNEL_ARGS_NOHZ : KERNEL_ARGS);
	if (ret) {
		host_log("gx: the kernel did not start (%d)\n", ret);
		host_exit(1);
	}
	kernel_running = 1;
	/* The kernel has no clock to read the date from and starts in 1970. */
	lkl_syscall(__lkl__NR_clock_settime, params);
	if (net_id >= 0)
		net_up(net_id);

	if (disk_id >= 0) {
		static const char node[] = "/gx-root-disk", mnt[] = "/gx-root";
		long mknod_args[6] = { LKL_AT_FDCWD, (long)node, LKL_S_IFBLK | 0600 };
		uint32_t dev = 0;

		/* By hand rather than with lkl_mount_dev(): LKL's wrappers pass
		 * the device number as a dev_t, which is 16 bits here and loses
		 * the major number of virtio disks. */
		ret = lkl_get_virtio_blkdev(disk_id, 0, &dev);
		mknod_args[3] = dev;
		if (!ret)
			ret = lkl_syscall(__lkl__NR_mknodat, mknod_args);
		if (!ret)
			ret = lkl_sys_mkdir(mnt, 0755);
		if (!ret)
			ret = lkl_sys_mount((char *)node, (char *)mnt, "ext4", LKL_MS_RDONLY, NULL);
		/* Before the root is the root: its /root is where the profile
		 * goes, and nothing can be made on a root that is read-only. */
		if (!ret)
			mount_home(profile_id);
		if (!ret)
			ret = lkl_sys_chroot(mnt);
		if (!ret)
			ret = lkl_sys_chdir("/");
		if (ret) {
			host_log("gx: cannot mount %s (%d)\n", root_image_name, ret);
			host_exit(1);
		}
		host_log("gx: root is %s\n", root_image_name);
	} else {
		host_log("gx: copying %s\n", GX_DIR "/sysroot");
		if (import_tree(GX_DIR "/sysroot", ""))
			host_exit(1);
	}
	populate_root();
	if (wayland) {
		/* Before the browser looks for controllers, which it does once. */
		gamepad_create();
		overlay_bindings(game_bindings);
	}
	if (disk_id >= 0) {
		/* The card reads large pieces far faster than small ones, and what
		 * is read of the root is mostly whole programs and libraries. */
		int fd = lkl_sys_open("/sys/block/vda/queue/read_ahead_kb", LKL_O_WRONLY, 0);

		if (fd >= 0) {
			lkl_sys_write(fd, "2048\n", 5);
			lkl_sys_close(fd);
		}
	}
	if (profile_mounted)
		unlock_profile();
	if (wayland)
		system_title_bar();
	if (cpu_count)
		gx_set_cpus(cpu_count);
	if (profiling)
		bench_syscalls();
	/* Descriptors 0-2 are the console; keep the kernel from handing those
	 * numbers out. */
	for (int i = 0; i < 3; i++)
		lkl_sys_open("/tmp/.stdio", LKL_O_RDWR | LKL_O_CREAT, 0644);

	if (gmem_init()) {
		host_log("gx: cannot set up the guest address space\n");
		host_exit(1);
	}
	/* The pipe first: it wants the descriptors Chromium knows it by. */
	if (browser && cdp_start(wayland))
		host_log("gx: cannot set up the pipe to Chromium\n");
	if (wayland && wl_start())
		host_log("gx: cannot open the Wayland display\n");
	host_log("gx: running %s\n\n", guest_argv[0]);
	gx_exec(guest_argv[0], guest_argc, guest_argv, guest_env);
	host_exit(1);
}

/* What the process costs the kernel: it keeps a record per run of pages that
 * are mapped alike, out of a store that every program on the console shares,
 * and things start failing (IPC first) when the store runs dry. */
static void report_kernel_use(void)
{
	u64 addr = 0, end = 0, size = 0, blocks = 0, code = 0, shared = 0, heap = 0;
	/* The guest's, by size (up to 16 KB, 256 KB, 2 MB, and above) and by
	 * what may be done with them (nothing, read, write, run). */
	unsigned long sizes[4] = { 0 }, perms[4] = { 0 };
	int handles = 0;

	svcGetInfo(&addr, InfoType_AslrRegionAddress, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
	end = addr + size;
	while (addr < end) {
		MemoryInfo info;
		u32 page_info;

		if (R_FAILED(svcQueryMemory(&info, &page_info, addr)) || !info.size)
			break;
		addr = info.addr + info.size;
		if (info.type == MemType_Unmapped || info.type == MemType_Reserved)
			continue;
		blocks++;
		if (info.type == MemType_ModuleCodeStatic || info.type == MemType_ModuleCodeMutable) {
			code++;
			sizes[info.size <= 0x4000 ? 0 : info.size <= 0x40000 ? 1 : info.size <= 0x200000 ? 2 : 3]++;
			perms[(info.perm & Perm_X) ? 3 : (info.perm & Perm_W) ? 2 : (info.perm & Perm_R) ? 1 : 0]++;
		} else if (info.type == MemType_SharedMem)
			shared++;
		else if (info.type == MemType_Heap)
			heap++;
	}
	for (int i = 0; i < MAX_SHARED_HANDLES; i++)
		handles += shared_handles[i] != 0;
	kernel_blocks = blocks;
	host_log("gx: kernel memory blocks: %lu (%lu guest, %lu shared, %lu heap); "
		 "%d shared objects; %lu MB of the pool free\n",
		 (unsigned long)blocks, (unsigned long)code, (unsigned long)shared,
		 (unsigned long)heap, handles, (unsigned long)(pool_free >> 8));
	host_log("gx: guest blocks by size: %lu up to 16 KB, %lu to 256 KB, %lu to 2 MB, %lu larger; "
		 "by use: %lu nothing, %lu read, %lu write, %lu run\n",
		 sizes[0], sizes[1], sizes[2], sizes[3], perms[0], perms[1], perms[2], perms[3]);
	{
		/* What the host itself uses of the heap it kept (HEAP_KEPT):
		 * all in use but the pool and the kernel's memory. */
		struct mallinfo mi = mallinfo();
		size_t used = mi.uordblks - pool_pages * GX_PAGE - kernel_mem;

		if (used > peak_host_heap)
			peak_host_heap = used;
		host_log("gx: host heap: %lu MB in use (at most %lu MB), %lu MB free in it\n",
			 (unsigned long)(used >> 20), (unsigned long)(peak_host_heap >> 20),
			 (unsigned long)(mi.fordblks >> 20));
	}
}

/* Where one thread is: paused for the time it takes to read its registers. */
static void report_where(Handle handle, const char *what)
{
	ThreadContext ctx;
	const char *name = "";
	u64 fp;
	Result rc;

	if (R_FAILED(svcSetThreadActivity(handle, ThreadActivity_Paused)))
		return;
	rc = svcGetThreadContext3(&ctx, handle);
	svcSetThreadActivity(handle, ThreadActivity_Runnable);
	if (R_FAILED(rc))
		return;
	for (int i = 0; i < GX_MAX_THREADS; i++)
		if (gx_tp_tab[i].guest_tp != ~0UL && gx_tp_tab[i].thread &&
		    gx_tp_tab[i].thread->host_id == handle)
			name = gx_tp_tab[i].thread->name;
	host_log(" thread %#x (%s%s%s)\n", handle, what, name[0] ? " " : "", name);
	report_address("pc", ctx.pc.x);
	report_address("lr", ctx.lr);
	/* Who called whom, as far as the frames of the stack tell. */
	fp = ctx.fp;
	for (int depth = 0; depth < 10; depth++) {
		MemoryInfo info;
		u32 page_info;
		u64 next, lr;

		if (!fp || (fp & 15) || R_FAILED(svcQueryMemory(&info, &page_info, fp)) ||
		    !(info.perm & Perm_R) || fp + 16 > info.addr + info.size)
			break;
		next = ((u64 *)fp)[0];
		lr = ((u64 *)fp)[1];
		if (!lr)
			break;
		report_address("  from", lr);
		if (next <= fp)
			break;
		fp = next;
	}
}

/* Everything has stopped: where every thread is, for gx.map to give names to
 * the addresses in the host (they are counted from where gx is loaded). */
static void report_stuck(void)
{
	Handle self = threadGetCurHandle();

	host_log("gx: where the threads are (gx is loaded at %p):\n", _start);
	for (int i = 0; i < known_count && i < MAX_KNOWN; i++)
		if (known[i].handle && known[i].handle != self)
			report_where(known[i].handle, known[i].what);
	host_log("gx: that was all of them\n");
	sync_log();
}

/* Asks the kernel how its memory is, when the watchdog says so: on a thread
 * of its own, since a kernel that has stopped never answers and whoever asks
 * waits with it. */
static volatile int kernel_asked, leaving, left;

static void kernel_probe(void *arg)
{
	for (;;) {
		if (kernel_asked) {
			if (kernel_running)
				gx_report_kernel_memory();
			kernel_asked = 0;
		}
		if (leaving && !left) {
			/* The user quits: what the kernel still has in memory of
			 * the profile goes to its image first. Asked here because
			 * the main thread cannot ask the kernel anything. */
			long none[6] = { 0 };

			if (profile_mounted && kernel_running)
				lkl_syscall(__lkl__NR_sync, none);
			hostfs_nx_flush();
			left = 1;
		}
		svcSleepThread(100000000ULL);
	}
}

/*
 * -p: where the processor goes. Every few milliseconds each guest thread that
 * is not in a syscall is paused for as long as it takes to read where it is;
 * every ten seconds the places seen most, by 4 KB page of code, go to the log.
 * Guest addresses are found in the "code of" lines gmem.c writes, the host's
 * in gx.map (counted from where gx is loaded).
 */
#define PROF_SLOTS 8192
static struct { uint64_t page; unsigned count; } prof[PROF_SLOTS];
static unsigned prof_samples, prof_in_syscall;

static int prof_in_host(uint64_t pc)
{
	return pc >= (uint64_t)_start && pc < (uint64_t)_start + (64UL << 20);
}

/* By page in the guest; in the host, by instruction, and who called it (an
 * address with its lowest bit set). */
static void prof_count(uint64_t pc)
{
	uint64_t page = prof_in_host(pc & ~1UL) ? pc : pc & ~0xfffUL;
	unsigned h = (unsigned)((page >> 12) * 2654435761u) % PROF_SLOTS;

	for (int i = 0; i < 64; i++, h = (h + 1) % PROF_SLOTS) {
		if (prof[h].page == page || !prof[h].page) {
			prof[h].page = page;
			prof[h].count++;
			return;
		}
	}
}

static void prof_sample(struct gx_thread *t)
{
	ThreadContext ctx;
	Result rc;

	if (!t || !t->host_id)
		return;
	if (t->nr >= 0) {
		prof_in_syscall++;
		return;
	}
	if (R_FAILED(svcSetThreadActivity((Handle)t->host_id, ThreadActivity_Paused)))
		return;
	rc = svcGetThreadContext3(&ctx, (Handle)t->host_id);
	svcSetThreadActivity((Handle)t->host_id, ThreadActivity_Runnable);
	if (R_SUCCEEDED(rc)) {
		prof_count(ctx.pc.x);
		if (prof_in_host(ctx.pc.x))
			prof_count(ctx.lr | 1);
		prof_samples++;
	}
}

static void prof_report(void)
{
	uint64_t self = (uint64_t)_start;

	host_log("gx: profile: %u samples in code, %u in syscalls; the busiest pages:\n",
		 prof_samples, prof_in_syscall);
	for (int n = 0; n < 30; n++) {
		int best = -1;

		for (int i = 0; i < PROF_SLOTS; i++)
			if (prof[i].count && (best < 0 || prof[i].count > prof[best].count))
				best = i;
		if (best < 0 || prof[best].count * 200 < prof_samples)
			break;
		if (prof_in_host(prof[best].page & ~1UL))
			host_log("  %5u  host %s+%#lx\n", prof[best].count,
				 (prof[best].page & 1) ? "called from " : "",
				 (unsigned long)((prof[best].page & ~1UL) - self));
		else if (prof[best].page & 1)
			host_log("  %5u  called from %#lx\n", prof[best].count,
				 (unsigned long)(prof[best].page & ~1UL));
		else
			host_log("  %5u  %#lx\n", prof[best].count, (unsigned long)prof[best].page);
		prof[best].count = 0;
	}
	memset(prof, 0, sizeof(prof));
	prof_samples = prof_in_syscall = 0;
}

static void profiler(void *arg)
{
	for (uint64_t next = host_now_ns() + 10000000000ULL;;) {
		svcSleepThread(3000000ULL);
		if (gx_main_thread)
			prof_sample(gx_main_thread);
		for (int i = 0; i < GX_MAX_THREADS; i++)
			if (gx_tp_tab[i].guest_tp != ~0UL && gx_tp_tab[i].thread != gx_main_thread)
				prof_sample(gx_tp_tab[i].thread);
		if (host_now_ns() >= next) {
			prof_report();
			next += 10000000000ULL;
		}
	}
}

/* Every so often, says in the log where the guest's threads are and what the
 * process costs: a guest that hangs leaves nothing else to go by. Nothing
 * here may wait on the kernel or, for long, on the lock of the log: those
 * are what hangs. */
static void watchdog(void *arg)
{
	unsigned swaps = 0;
	int told = 0;

	for (int seconds = 10;; seconds += 10) {
		int locked = 0;

		svcSleepThread(10000000000ULL);
		if (finished)
			break;
		for (int i = 0; i < 300 && !(locked = mutexTryLock(&print_lock)); i++)
			svcSleepThread(10000000ULL);
		if (locked) {
			mutexUnlock(&print_lock);
		} else {
			/* Its holder is not coming back: without it then. */
			int stage = print_stage;

			mutexInit(&print_lock);
			console_on = 0;
			host_log("\ngx: a thread kept the log to itself for 3 s, at stage %d\n", stage);
			if (!told++)
				report_stuck();
		}
		host_log("gx: after %d s\n", seconds);
		bury_threads();
		video_report();
		gmem_report();
		host_log("gx: disk: %lu MB read in %lu ms\n", (unsigned long)(disk_read_bytes >> 20),
			 (unsigned long)(disk_read_ns / 1000000));
		if (gl_swaps != swaps) {
			host_log("gx: gl: %u pictures in 10 s; %u gaps over 50 ms, %u over 100, "
				 "the longest %lu ms\n", gl_swaps - swaps, gl_gaps50, gl_gaps100,
				 (unsigned long)(gl_gap_max / 1000000));
			swaps = gl_swaps;
			gl_gaps50 = gl_gaps100 = 0;
			gl_gap_max = 0;
		}
		report_kernel_use();
		host_log("gx: host threads: %d\n", count_threads() + 1);   /* and this one */
		gx_report_threads();
		gx_report_syscall_cost();
		hostfs_nx_report();
		hostfs_nx_flush();   /* what the profile gathered: on the card within 10 s */
		if (kernel_asked) {
			host_log("gx: the kernel has not answered for 10 s\n");
			if (!told++)
				report_stuck();
		} else {
			kernel_asked = 1;
		}
		sync_log();
	}
}

/* The only thread that draws: consoleUpdate() is not thread-safe, and the
 * console may stop for good in there (see console_off()). */
static void display(void *arg)
{
	for (;;) {
		mutexLock(&screen_lock);
		if (console_on)
			consoleUpdate(NULL);
		mutexUnlock(&screen_lock);
		svcSleepThread(16666666ULL);
	}
}

/* Ends the process. An application (Chromium NX installed as a forwarder) has
 * to tell AM it is leaving, as the Homebrew Loader does when a program
 * returns: ended with svcExitProcess() alone, the HOME menu says "the
 * software was closed because an error occurred". AM then ends us. */
static void NX_NORETURN leave(void)
{
	AppletType type = appletGetAppletType();

	if (type == AppletType_Application || type == AppletType_SystemApplication) {
		Service *self = appletGetServiceSession_SelfController();

		if (self && serviceIsActive(self) && R_SUCCEEDED(serviceDispatch(self, 0)))
			for (int i = 0; i < 500; i++)
				svcSleepThread(10000000ULL);
	}
	svcExitProcess();
	__builtin_unreachable();
}

/* Chromium reserves more addresses than a program started the usual way has:
 * the Homebrew Loader gives its programs 36 bits of them (64 GB), and a
 * program installed as a title of its own can be given 39 (512 GB). Says so
 * and waits for + if this is the small kind; returns whether it is large. */
static int enough_addresses(PadState *pad)
{
	u64 size = 0;

	svcGetInfo(&size, InfoType_AslrRegionSize, CUR_PROCESS_HANDLE, 0);
	if (size >= (256ULL << 30))
		return 1;
	printf("Chromium NX\n\n"
	       "Chromium needs a 39-bit address space, and this one is smaller\n"
	       "(%lu GB of addresses, 512 GB are needed).\n\n"
	       "Install Chromium NX as a forwarder with Sphaira, choosing a\n"
	       "39-bit address space for it, then start it from the HOME menu.\n\n"
	       "+: quit\n", (unsigned long)(size >> 30));
	while (appletMainLoop()) {
		consoleUpdate(NULL);
		padUpdate(pad);
		if (padGetButtonsDown(pad) & HidNpadButton_Plus)
			break;
		svcSleepThread(16666666ULL);
	}
	return 0;
}

int main(int argc, char **argv)
{
	static Thread display_thread, watchdog_thread, probe_thread;
	PadState pad;
	int choice;

	if (!browser_mode) {
		extern int launcher_main(int argc, char **argv);

		return launcher_main(argc, argv);
	}
	consoleInit(NULL);
	/* Any number of players, so that Joy-Con taken off, put back or paired
	 * while we run are let in (with one, only the HOME menu takes them);
	 * all of them read as one pad. */
	padConfigureInput(8, HidNpadStyleSet_NpadStandard);
	padInitializeAny(&pad);
	if (!enough_addresses(&pad)) {
		consoleExit(NULL);
		leave();   /* not a return: see __libnx_initheap() */
	}
	mkdir(GX_DIR, 0777);
	read_commands();
	/* A menu only when there is something to choose, or when L is held. */
	padUpdate(&pad);
	choice = command_count > 1 || (padGetButtons(&pad) & HidNpadButton_L) ? menu(&pad) : 0;
	if (choice < 0) {
		consoleExit(NULL);
		leave();   /* not a return: see __libnx_initheap() */
	}
	consoleClear();
	log_file = fopen(GX_DIR "/chromium-nx.log", "w");
	/* Written in large pieces once things run: see host_console_write(). */
	if (log_file)
		setvbuf(log_file, NULL, _IOFBF, 64 << 10);

	if (commands[choice][0] == '[' && strchr(commands[choice], ']'))
		memmove(commands[choice], strchr(commands[choice], ']') + 1,
			strlen(strchr(commands[choice], ']')));
	game_bindings_parse(NULL);
	/* The options, before the program (launch.txt, from the launcher):
	 *   -w / -b       a Wayland display / the browser's debugging pipe
	 *   @img / =img   the root image / the profile image, next to the NRO
	 *   -c<n>         processors the guest is told about
	 *   -T            YouTube's TV page, full screen
	 *   -F / -R       start full screen / in portrait (My sites)
	 *   -M<n>         the pad's mode: 0 browser, 1 keyboard and mouse, 2 controller
	 *   -K<k>,...     the keyboard and mouse keys, in keys.h's order
	 *   -N<url>       a page to go to once open (chrome:// ones)
	 *   -A<l>,<t>,<r>,<b>  where the address bar is
	 *   -l            720p on a TV too
	 *   -p            diagnostics in the log; -t traces every syscall
	 *   -x<letters>   experiments (b, h, p, d: see below) */
	for (char *word = strtok(commands[choice], " "); word && guest_argc < MAX_ARGS;
	     word = strtok(NULL, " ")) {
		/* Before the program: -t, -b, @<root image>, ><guest file to copy out>. */
		if (!guest_argc && !strcmp(word, "-t"))
			gx_trace = 1;
		else if (!guest_argc && !strcmp(word, "-b"))
			browser = 1;
		else if (!guest_argc && !strcmp(word, "-w"))
			wayland = 1;
		else if (!guest_argc && !strcmp(word, "-T"))
			tv_mode = 1;
		else if (!guest_argc && !strcmp(word, "-l"))
			stay_720p = 1;   /* not 1080p on a TV: faster */
		else if (!guest_argc && !strcmp(word, "-F"))
			start_full_screen = 1;   /* a site of the launcher's, full screen */
		else if (!guest_argc && !strncmp(word, "-M", 2))
			overlay_set_mode(atoi(word + 2));   /* the pad's mode from the start */
		else if (!guest_argc && !strcmp(word, "-R"))
			wl_set_portrait(1);   /* a site of the launcher's, in portrait */
		else if (!guest_argc && !strncmp(word, "-N", 2))
			cdp_start_url(word + 2);   /* a page Chromium will not open from its command line */
		else if (!guest_argc && !strncmp(word, "-K", 2))
			game_bindings_parse(word + 2);   /* the keyboard and mouse mode's keys */
		else if (!guest_argc && !strcmp(word, "-p"))
			profiling = cdp_network_log = 1;
		else if (!guest_argc && !strncmp(word, "-x", 2)) {
			experiment = word + 2;
			no_kernel_prio = !!strchr(experiment, 'b');
			gx_no_priorities = !!strchr(experiment, 'p');
			lkl_blk_sync = !!strchr(experiment, 'd');
		}
		else if (!guest_argc && !strncmp(word, "-A", 2)) {
			int l, t, r, b;   /* -A<left>,<top>,<right>,<bottom>: the address bar */

			if (sscanf(word + 2, "%d,%d,%d,%d", &l, &t, &r, &b) == 4)
				wl_address_bar(l, t, r, b);
		}
		else if (!guest_argc && !strncmp(word, "-c", 2) && word[2])
			cpu_count = atoi(word + 2);
		else if (!guest_argc && word[0] == '@')
			root_image_name = word + 1;
		else if (!guest_argc && word[0] == '=')
			profile_name = word + 1;
		else if (!guest_argc && word[0] == '>')
			export_path = word + 1;
		else
			guest_argv[guest_argc++] = word;
	}
	if (!guest_argc)
		guest_argv[guest_argc++] = "/bin/hello";
	/* The console's theme, which Chromium has no way to ask about here (it
	 * asks GTK or the desktop over D-Bus): dark, its windows and the pages'
	 * prefers-color-scheme are dark too, as its "Device" setting means. */
	if (wayland && guest_argc < MAX_ARGS) {
		ColorSetId theme = ColorSetId_Light;

		if (R_SUCCEEDED(setsysInitialize())) {
			setsysGetColorSetId(&theme);
			setsysExit();
		}
		if (theme == ColorSetId_Dark)
			guest_argv[guest_argc++] = "--force-dark-mode";
	}
	/* On a TV (the console in its dock), 1080p: the screen is made
	 * 1920 x 1080 (gl_screen()) and Chromium is told to draw at one and a
	 * half times (wl.c, wp_fractional_scale_v1), so that pages are laid out
	 * as on the console's own screen, only sharper. It follows the console
	 * in and out of the dock (tv_screen()). */
	if (wayland && !stay_720p)
		tv_screen();
	if (start_full_screen && !tv_mode && guest_argc < MAX_ARGS)
		guest_argv[guest_argc++] = "--start-fullscreen";
	if (tv_mode && guest_argc + 2 <= MAX_ARGS) {
		static char features[1024];

		/* Nor does it say what Chromium it is in other headers (client
		 * hints). Chromium takes only the last --disable-features. */
		for (int i = 1; i < guest_argc; i++)
			if (!strncmp(guest_argv[i], "--disable-features=", 19)) {
				snprintf(features, sizeof(features), "%s,UserAgentClientHint", guest_argv[i]);
				guest_argv[i] = features;
			}
		guest_argv[guest_argc++] = "--user-agent=" TV_USER_AGENT;
		guest_argv[guest_argc++] = "--start-fullscreen";
		wl_address_bar(0, 0, 0, 0);   /* none on the screen */
	}

	/* Everything runs on created threads: on the main thread of a homebrew,
	 * threadGetSelf() is not what LKL needs it to be (see nro/source/main.c). */
	free_cores(CUR_THREAD_HANDLE);
	know_thread(envGetMainThreadHandle(), "main");
	if (R_SUCCEEDED(threadCreate(&display_thread, display, NULL, NULL, 0x8000, 0x2C, -2))) {
		free_cores(display_thread.handle);
		know_thread(display_thread.handle, "display");
		threadStart(&display_thread);
	}
	/* These two at the guest's priority, the lowest: what they do every ten
	 * seconds (asking the kernel about every block of memory, writing the
	 * log to the card) took the processor from the guest's threads for as
	 * long, and a video being played stopped for a moment each time. */
	if (R_SUCCEEDED(threadCreate(&watchdog_thread, watchdog, NULL, NULL, 0x10000, GUEST_PRIO, -2))) {
		free_cores(watchdog_thread.handle);
		know_thread(watchdog_thread.handle, "watchdog");
		threadStart(&watchdog_thread);
	}
	if (R_SUCCEEDED(threadCreate(&probe_thread, kernel_probe, NULL, NULL, 0x10000, GUEST_PRIO, -2))) {
		free_cores(probe_thread.handle);
		know_thread(probe_thread.handle, "probe");
		threadStart(&probe_thread);
	}
	if (profiling) {
		static Thread profiler_thread;

		/* Above the guest, to catch it where it is. */
		if (R_SUCCEEDED(threadCreate(&profiler_thread, profiler, NULL, NULL, 0x10000, 0x2C, -2))) {
			free_cores(profiler_thread.handle);
			know_thread(profiler_thread.handle, "profiler");
			threadStart(&profiler_thread);
		}
	}
	if (start_thread(run, NULL, MAIN_STACK))
		printf("gx: cannot start\n");

	while (appletMainLoop()) {
		padUpdate(&pad);
		if (wayland && !stay_720p)
			tv_screen();
		/* With a window, + opens the overlay and Quit is in it. */
		if (!wayland && (padGetButtonsDown(&pad) & HidNpadButton_Plus))
			break;
		if (__atomic_load_n(&quit_wanted, __ATOMIC_SEQ_CST))
			break;
		if (crashed) {
			svcSleepThread(1000000000ULL);   /* what the others have to say */
			say_crashed();
			/* Straight out: the way below asks the browser to close
			 * and waits on locks the dead thread may hold. The log
			 * was written when it died. */
			leave();
		}
		svcSleepThread(16666666ULL);
	}
	/* Kernel and guest threads cannot be stopped one by one; ending the
	 * process is the only clean way out (back to the HOME menu). */
	/* A browser first: asked to close, it writes what it was keeping in
	 * memory (cookies among it) and ends by itself. */
	if (wayland) {
		wl_close();
		for (int i = 0; i < 1000 && !finished && !wl_closed(); i++)
			svcSleepThread(10000000ULL);
		svcSleepThread(300000000ULL);
	}
	leaving = 1;
	for (int i = 0; i < 500 && profile_mounted && !left; i++)
		svcSleepThread(10000000ULL);   /* five seconds at most: the kernel may be stuck */
	for (int i = 0; i < 100 && !mutexTryLock(&print_lock); i++)
		svcSleepThread(10000000ULL);   /* not for ever: its holder may be stuck */
	if (log_file)
		fclose(log_file);
	if (profile_image.file && mutexTryLock(&profile_image.lock))
		fclose(profile_image.file);
	/* Back to the launcher: the program started again, as its title does
	 * it (no --browser, no launch.txt: the launcher). Only a title can; from
	 * the album, Quit. */
	if (to_launcher && (appletGetAppletType() == AppletType_Application ||
			    appletGetAppletType() == AppletType_SystemApplication) &&
	    R_SUCCEEDED(appletRestartProgram(NULL, 0)))
		svcSleepThread(5000000000ULL);   /* the system ends this process meanwhile */
	leave();
	return 0;
}
