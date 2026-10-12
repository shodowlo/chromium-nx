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
/* What handles a fault does without the vector registers (nx/plain.c), and
 * most of this file may be reached from there. */
#pragma GCC target("general-regs-only")

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

/* Where everything is on the SD card: the root image next to the program, the
 * log, and programs.txt if there is to be a menu. */
#define GX_DIR "sdmc:/switch/chromium-nx"
/* What runs when there is no programs.txt: Chromium with its window (-w) and
 * the debugging pipe (-b), drawing with the GPU, V8 with its compilers. */
#define DEFAULT_COMMAND \
	"-w -b @chromium.ext4 /usr/lib/chromium/chromium-shell --no-sandbox --single-process " \
	"--no-zygote --use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist " \
	"--disable-gpu-sandbox --in-process-gpu --ozone-platform=wayland " \
	"--enable-spatial-navigation --remote-debugging-pipe https://www.google.com/"
/* The kernel's memory holds every file that is not on the root image (its
 * /tmp, a browser's profile and cache) and its network buffers: a browser on
 * a busy site went through 192 MB and the kernel panicked. With a profile
 * on a disk of its own and the page cache dropped when little is free
 * (gsys.c), 400 of 512 MB stayed free while a browser played videos. */
#define KERNEL_MB 384
/* Left to malloc for stacks (a megabyte a thread, eighty threads), libnx, the
 * GPU driver and the video decoder, whose pictures are here too: with 256 MB a
 * third 1080p video in a row left no stack for a new thread. Less when the
 * homebrew runs as an applet and the whole heap is a few hundred megabytes. */
#define HEAP_KEPT (384UL << 20)
#define HEAP_KEPT_SMALL (48UL << 20)
/* How many pieces of memory mapped alike before more is refused (see
 * host_commit()): what the kernel's shared store was seen to bear, unless the
 * process has a store of its own (a loader made by build-hbl.sh gives it
 * one: host_reserve() finds out). */
static unsigned long kernel_blocks_max = 8500;
static int lazy_pages_asked;   /* -L<n> */
static size_t heap_kept = HEAP_KEPT;  /* -m<n> in the menu's line: megabytes left to the host */
#define POOL_WORTH_IT (192UL << 20)
/* Host functions called for the guest (the GL driver) run on these too. */
#define MAIN_STACK (4UL << 20)
#define THREAD_STACK (1UL << 20)
/* The only priority Horizon time-slices: guest threads may spin. */
#define GUEST_PRIO 0x3B

static Mutex print_lock, mem_lock, screen_lock;

/* The lock on the guest's memory, taken and left. A thread that takes it
 * while it has it would be let through by the C library's lock and leave it
 * open under itself: said of, should it happen again (it did when faults
 * were handled on the guest's stack, see fault_stack()). */
static void *mem_owner;

void host_log(const char *fmt, ...);

/* A second door behind a lock of the C library's, which must find it open:
 * whoever has the lock is alone. Said of when it is not so, and waited at. */
static void gate_in(void **owner, const char *what)
{
	static int said;
	void *none = NULL, *self = armGetTls();

	while (!__atomic_compare_exchange_n(owner, &none, self, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
		if (said < 8 && ++said)
			host_log("gx: the lock on %s was had by two threads (%p, and %p already there)\n",
				 what, self, none);
		none = NULL;
		svcSleepThread(0);
	}
}

static void mem_take(void)
{
	static int said;

	if (mem_owner == armGetTls() && said < 4 && ++said)
		host_log("gx: the lock on the guest's memory is taken twice by one thread\n");
	mutexLock(&mem_lock);
	gate_in(&mem_owner, "the guest's memory");
}

static void mem_leave(void)
{
	__atomic_store_n(&mem_owner, NULL, __ATOMIC_SEQ_CST);
	mutexUnlock(&mem_lock);
}
static FILE *log_file;
static volatile int finished;
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
	if (console_on)
		fwrite(buf, 1, len, stdout);
	print_stage = 2;
	if (log_file) {
		static u64 synced;
		u64 now = armGetSystemTick();

		fwrite(buf, 1, len, log_file);
		fflush(log_file);
		/* What is not committed to the card is lost if the console is
		 * switched off or the program is killed. */
		static u64 started;

		/* Every line while things start: a start that hangs is seen
		 * once in a while, and its last lines are what tells where. */
		if (!started)
			started = now;
		if (armTicksToNs(now - started) < 20000000000ULL ||
		    armTicksToNs(now - synced) > (gx_trace ? 20000000ULL : 250000000ULL)) {
			print_stage = 3;
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
	if (log_file)
		fsync(fileno(log_file));
	mutexUnlock(&print_lock);
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
/* The arena page each pool page is behind, plus one: the table above read the
 * other way, kept to tell when the two no longer agree (see unmap_pages()). */
static uint32_t *pool_owner;
static unsigned long table_slips;   /* times they did not */
/* What last happened to each pool page and when, by the count of such events:
 * said of a page that turns out not to be ours (host_commit()). */
enum { POOL_NEVER, POOL_MAPPED, POOL_UNMAPPED, POOL_REFUSED };
static uint8_t *pool_last;
static uint32_t *pool_when, pool_events;
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
	size_t size = 0, kept = heap_kept;
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

	/* The pool is all the heap but what the host keeps for itself: the far
	 * end of it, taken from under the C library, whose malloc() cannot
	 * give two gigabytes in one piece (with what is kept asked for along
	 * with it, the pool stopped at 1856 MB however much there was). */
	{
		extern char *fake_heap_end;
		char *top = (char *)((u64)fake_heap_end & ~(u64)(GX_PAGE - 1)), *now = sbrk(0);

		if (now != (char *)-1 && top > now && (size_t)(top - now) > kept + POOL_WORTH_IT) {
			size = ((size_t)(top - now) - kept) & ~((2UL << 20) - 1);
			pool = (uint8_t *)top - size;
			fake_heap_end = (char *)pool;
			host_log("gx: %lu MB of the heap not in use, %lu MB of them kept for the host\n",
				 (unsigned long)((size_t)(top - now) >> 20), (unsigned long)(kept >> 20));
		}
	}
	while (!pool) {
		for (size = 3072UL << 20; size >= (32UL << 20); size -= 16UL << 20) {
			void *probe = memalign(GX_PAGE, size + kept);

			if (probe) {
				free(probe);
				break;
			}
		}
		if (size < POOL_WORTH_IT && kept != HEAP_KEPT_SMALL) {
			kept = HEAP_KEPT_SMALL;
			continue;
		}
		if (size < (32UL << 20))
			break;
		pool = memalign(GX_PAGE, size);
		if (!pool)
			break;
	}
	if (!leaves || !pool) {
		host_log("gx: not enough memory for the guest\n");
		return NULL;
	}
	pool_pages = pool_free = size / GX_PAGE;
	pool_used = calloc(pool_pages, 1);
	pool_owner = calloc(pool_pages, sizeof(*pool_owner));
	pool_last = calloc(pool_pages, 1);
	pool_when = calloc(pool_pages, sizeof(*pool_when));
	if (!pool_used || !pool_owner || !pool_last || !pool_when)
		return NULL;
	self = envGetOwnProcessHandle();
	host_log("gx: %lu MB for the guest, %lu MB for shared memory\n",
		 (unsigned long)(size >> 20), (unsigned long)(shared_budget >> 20));
	{
		/* Memory the process was given for the kernel's records of it:
		 * with that, many more pieces can be mapped, and memory that is
		 * committed when first used can come in small ones. */
		u64 total = 0, used = 0;

		svcGetInfo(&total, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
		svcGetInfo(&used, InfoType_SystemResourceSizeUsed, CUR_PROCESS_HANDLE, 0);
		if (total >= (16UL << 20)) {
			/* A record is under 100 bytes; half the store is left to
			 * the page tables. */
			kernel_blocks_max = total / 2 / 100;
			/* Pieces to spare: each is what was asked for and no
			 * more, so it can be large, and a fault, which is where
			 * the kernel may lose the guest's vector registers
			 * (nx/exception.S), brings more with it. */
			gmem_lazy_pages = lazy_pages_asked ? lazy_pages_asked : 64;
			gmem_lazy_wanted_only = 1;
			/* And few faults at all: what a program asks for is there
			 * at once unless it is a thread's stack or very large.
			 * A Steam client on a PC then has 50 MB more committed
			 * and a fifth of the faults. */
			gmem_eager_mapped = gmem_eager_protected = 32UL << 20;
			/* (A lower limit does not spare memory where it is short:
			 * the software GL driver takes 250 pieces of 812 KB for
			 * every context, one for each texture a shader could have,
			 * and writes a word every 16 KB of each: all of it is
			 * touched, in six thousand faults when it is not there
			 * at once. 200 MB a context, and a Steam client has a
			 * context a window. That is Mesa's to cure.) */
			host_log("gx: %lu MB of the process's own for the kernel's records (%lu KB used): "
				 "up to %lu pieces of memory\n", (unsigned long)(total >> 20),
				 (unsigned long)(used >> 10), kernel_blocks_max);
		} else {
			/* Few pieces to be had: larger ones, though not the megabyte
			 * that had a Steam client commit half as much again as it
			 * used and run out of memory before it ran out of pieces. */
			gmem_lazy_pages = 64;
			host_log("gx: the kernel's records of our memory are in its shared store: %lu pieces at most "
				 "(a loader made by build-hbl.sh lifts that)\n", kernel_blocks_max);
		}
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

static size_t pool_take_any(size_t n, size_t *first);

/* Free pool pages, n at most and in one piece: see pool_take_any(). What it
 * finds is checked against the other record there is of the pool, the arena
 * page each of its pages is behind: pages noted free have been found to be
 * behind code that was still mapped, and handing those out again is the end
 * of whoever has them. Such pages are noted as used again, and said of. */
static size_t pool_take(size_t n, size_t *first)
{
	for (;;) {
		size_t run = pool_take_any(n, first), bad = 0;

		for (size_t i = 0; i < run; i++)
			bad += pool_owner[*first + i] != 0;
		if (!bad)
			return run;
		for (size_t i = 0; i < run; i++) {
			size_t at = *first + i;

			if (pool_owner[at]) {
				if (table_slips++ < 24)
					host_log("gx: memory table: pool page %#lx was noted free while behind arena page %#x "
						 "(flags there %#x, pool page there %#x; last event %d, %u events ago)\n",
						 (unsigned long)at, pool_owner[at] - 1, flags_at(pool_owner[at] - 1),
						 src_at(pool_owner[at] - 1), pool_last[at], pool_events - pool_when[at]);
				continue;   /* stays noted as used */
			}
			pool_used[at] = 0;
			pool_free++;
			if (at < pool_hint)
				pool_hint = at;
		}
	}
}

/* The first page noted as used of the n from p on, or NULL. By hand, and
 * against the C library's memchr() for a while: pages that were in use have
 * been handed out as free, and one of the two it must be that said so. */
static const uint8_t *pool_first_used(size_t p, size_t n)
{
	const uint8_t *before = NULL, *quick, *sure = NULL;

	for (size_t i = 0; i < n && !before; i++)
		if (pool_used[p + i])
			before = &pool_used[p + i];
	quick = memchr(&pool_used[p], 1, n);
	for (size_t i = 0; i < n && !sure; i++)
		if (pool_used[p + i])
			sure = &pool_used[p + i];
	if ((quick != sure || before != sure) && table_slips++ < 24)
		host_log("gx: memory table: of %lu pool pages from %#lx, the first in use is %ld by a loop, "
			 "%ld by memchr(), %ld by the loop again\n", (unsigned long)n, (unsigned long)p,
			 before ? (long)(before - &pool_used[p]) : -1L, quick ? (long)(quick - &pool_used[p]) : -1L,
			 sure ? (long)(sure - &pool_used[p]) : -1L);
	return sure;
}

static size_t pool_take_any(size_t n, size_t *first)
{
	if (n >= POOL_LARGE && n <= pool_pages) {
		/* All of it in one run, the highest there is; in pieces as for
		 * small ones if the pool has no such run left. */
		for (size_t p = (pool_pages - n) & ~(size_t)(POOL_LARGE - 1);; p -= POOL_LARGE) {
			if (!pool_used[p] && !pool_used[p + n - 1] && !pool_first_used(p, n)) {
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
		const uint8_t *used = pool_first_used(p, n);

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
	for (size_t i = 0; i < n; i++)
		if ((pool_owner[first + i] || !pool_used[first + i]) && table_slips++ < 24)
			host_log("gx: memory table: pool page %#lx is given back while %s (arena page %#x, "
				 "last event %d, %u events ago)\n", (unsigned long)(first + i),
				 pool_owner[first + i] ? "behind a page" : "free already",
				 pool_owner[first + i] - 1, pool_last[first + i], pool_events - pool_when[first + i]);
	memset(&pool_used[first], 0, n);
	pool_free += n;
	if (first < pool_hint)
		pool_hint = first;
}

static size_t pool_lost;   /* pages of the pool found not to be usable: see host_commit() */

/* The kernel's records of this process, as last counted (report_kernel_use()).
 * Past about ten thousand, the kernel stops serving the process: it cannot
 * even be told so, the calls to the SD card being the first to fail. Refusing
 * memory before that at least ends in a message. */
static volatile unsigned long kernel_blocks;

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
	int failed = 0;

	while (n) {
		size_t run;
		Result rc;

		if (!(flags_at(p) & PG_MAPPED)) {
			p++;
			n--;
			continue;
		}
		run = run_of(p, n);
		/* The kernel takes our word for which pool pages are behind
		 * these: with the wrong ones it frees pages still in use
		 * elsewhere and keeps the right ones for good. */
		for (size_t i = 0; i < run; i++) {
			if (src_at(p + i) < pool_pages && pool_owner[src_at(p + i)] == p + i + 1)
				continue;
			if (table_slips++ < 16)
				host_log("gx: memory table: arena page %#lx is noted on pool page %#x, "
					 "which is noted behind %#x (unmap of %lu pages at %#lx, flags %#x)\n",
					 (unsigned long)(p + i), src_at(p + i),
					 src_at(p + i) < pool_pages ? pool_owner[src_at(p + i)] - 1 : 0,
					 (unsigned long)run, (unsigned long)p, flags_at(p));
			break;
		}
		rc = svcUnmapProcessCodeMemory(self, (u64)(arena + p * GX_PAGE),
					       (u64)(pool + (u64)src_at(p) * GX_PAGE),
					       run * GX_PAGE);
		if (R_FAILED(rc)) {
			MemoryInfo there = { 0 }, behind = { 0 };
			u32 page_info;

			svcQueryMemory(&there, &page_info, (u64)(arena + p * GX_PAGE));
			svcQueryMemory(&behind, &page_info, (u64)(pool + (u64)src_at(p) * GX_PAGE));
			host_log("gx: unmap of %p (%lu pages, from the pool at %#lx) failed (%#x): flags %#x; for the "
				 "kernel it is type %#x, permission %#x, and the pool page type %#x, permission %#x, "
				 "attributes %#x; that pool page is noted behind arena page %#x, last event %d\n",
				 arena + p * GX_PAGE, (unsigned long)run,
				 (unsigned long)(pool + (u64)src_at(p) * GX_PAGE), rc, flags_at(p), there.type,
				 there.perm, behind.type, behind.perm, behind.attr,
				 pool_owner[src_at(p)] - 1, pool_last[src_at(p)]);
			/* The rest of what was asked is still to go. */
			p += run;
			n -= run;
			failed = -1;
			continue;
		}
		/* The pool pages are ours to write to again, or they do not go
		 * back to the pool. */
		{
			MemoryInfo info = { 0 };
			u32 page_info;
			u64 from = (u64)(pool + (u64)src_at(p) * GX_PAGE), to = from + run * GX_PAGE;
			int ours = 1;

			for (u64 at = from; at < to && ours; at = info.addr + info.size) {
				if (R_FAILED(svcQueryMemory(&info, &page_info, at)) || !info.size)
					break;
				ours = info.perm == Perm_Rw;
			}
			pool_events++;
			for (size_t i = 0; i < run; i++) {
				pool_owner[src_at(p) + i] = 0;
				pool_last[src_at(p) + i] = POOL_UNMAPPED;
				pool_when[src_at(p) + i] = pool_events;
			}
			if (ours) {
				pool_give(src_at(p), run);
			} else {
				pool_lost += run;
				if (table_slips++ < 16)
					host_log("gx: memory table: %lu pool pages at %#lx stay the kernel's after the unmap "
						 "of arena page %#lx (%#lx+%#lx: type %#x, permission %#x, attributes %#x)\n",
						 (unsigned long)run, (unsigned long)from, (unsigned long)p,
						 (unsigned long)info.addr, (unsigned long)info.size, info.type,
						 info.perm, info.attr);
			}
		}
		set_flags(p, run, 0);
		/* What mapping it added to the count of the kernel's records. */
		if (kernel_blocks >= 2)
			kernel_blocks -= 2;
		p += run;
		n -= run;
	}
	return failed;
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
			/* Data to code: only a fresh mapping is code again. */
			rc = svcUnmapProcessCodeMemory(self, addr, src, len);
			if (R_SUCCEEDED(rc))
				rc = svcMapProcessCodeMemory(self, addr, src, len);
			if (R_SUCCEEDED(rc)) {
				set_flags(p, run, PG_MAPPED | P_FRESH);
				rc = svcSetProcessMemoryPermission(self, addr, len, perm);
			}
			if (R_SUCCEEDED(rc))
				set_flags(p, run, PG_MAPPED | code);
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


int host_commit(void *addr, size_t len, int prot)
{
	size_t first = page_of(addr), n = len / GX_PAGE, done = 0;
	int ret = 0;

	if (kernel_blocks > kernel_blocks_max) {
		static int said;

		if (!said++)
			host_log("gx: the kernel is short of memory records (%lu): no more memory "
				 "for the guest\n", kernel_blocks);
		return -1;
	}
	mem_take();
	if (n > pool_free) {
		host_log("gx: out of guest memory (%lu KB asked, %lu KB left)\n",
			 (unsigned long)(len >> 10), (unsigned long)(pool_free * 4));
		ret = -1;
	}
	while (!ret && done < n) {
		size_t src = 0, run = pool_take(n - done, &src);
		Result rc;

		if (!run) {
			ret = -1;
			break;
		}
		/* What the pool hands out is to be memory of ours to write to.
		 * A piece of it was once seen not to be, though free by our
		 * books (the kernel still had it as the other end of a mapping,
		 * and writing zeroes to it ended the program): such a piece is
		 * left aside for good, said of in the log, and another taken. */
		{
			MemoryInfo info = { 0 };
			u32 page_info;
			u64 from = (u64)(pool + src * GX_PAGE), to = from + run * GX_PAGE;
			int ours = 1;

			for (u64 at = from; at < to && ours; at = info.addr + info.size) {
				if (R_FAILED(svcQueryMemory(&info, &page_info, at)) || !info.size)
					break;
				ours = info.perm == Perm_Rw;
			}
			if (!ours) {
				static int said;

				pool_lost += run;
				if (said < 16 && ++said)
					host_log("gx: %lu pages of the pool at %#lx are not ours to write to (%#lx+%#lx: "
						 "type %#x, permission %#x, attributes %#x): left aside, %lu so far\n",
						 (unsigned long)run, (unsigned long)from, (unsigned long)info.addr,
						 (unsigned long)info.size, info.type, info.perm, info.attr,
						 (unsigned long)pool_lost);
				if (said <= 16) {
					size_t at = ((uint8_t *)info.addr - pool) / GX_PAGE;

					if ((u64)info.addr < from)
						at = src;
					host_log("gx:   its first such page: last event %d, %u events ago, noted behind arena "
						 "page %#x (flags there %#x, pool page there %#x); the one before it: "
						 "event %d, %u ago, used %d\n",
						 pool_last[at], pool_events - pool_when[at], pool_owner[at] - 1,
						 pool_owner[at] ? flags_at(pool_owner[at] - 1) : 0,
						 pool_owner[at] ? src_at(pool_owner[at] - 1) : 0,
						 at ? pool_last[at - 1] : -1, at ? pool_events - pool_when[at - 1] : 0,
						 at ? pool_used[at - 1] : -1);
				}
				continue;   /* still noted as used: never handed out again */
			}
		}
		/* The pool is ordinary memory until it is mapped: zero it here. */
		memset(pool + src * GX_PAGE, 0, run * GX_PAGE);
		rc = svcMapProcessCodeMemory(self, (u64)addr + done * GX_PAGE,
					     (u64)(pool + src * GX_PAGE), run * GX_PAGE);
		if (R_FAILED(rc)) {
			{
				/* What the kernel has there, and behind it: the map of
				 * a page that was not there should not be refused. */
				MemoryInfo there = { 0 }, behind = { 0 };
				u32 page_info;
				static int said;

				svcQueryMemory(&there, &page_info, (u64)addr + done * GX_PAGE);
				svcQueryMemory(&behind, &page_info, (u64)(pool + src * GX_PAGE));
				if (said < 12 && ++said)
					host_log("gx: map of %p failed (%#x): %lu pages from the pool at %#lx; there the kernel "
						 "has %#lx+%#lx type %#x permission %#x, flags noted %#x, pool page noted %#x; "
						 "the pool page is type %#x permission %#x attributes %#x, noted behind %#x\n",
						 (uint8_t *)addr + done * GX_PAGE, rc, (unsigned long)run,
						 (unsigned long)(pool + src * GX_PAGE), (unsigned long)there.addr,
						 (unsigned long)there.size, there.type, there.perm,
						 flags_at(first + done), src_at(first + done), behind.type, behind.perm,
						 behind.attr, pool_owner[src] - 1);
			}
			pool_events++;
			for (size_t i = 0; i < run; i++) {
				pool_last[src + i] = POOL_REFUSED;
				pool_when[src + i] = pool_events;
			}
			pool_give(src, run);
			ret = -1;
			break;
		}
		for (size_t i = 0; i < run; i++) {
			if (flags_at(first + done + i) & PG_MAPPED && table_slips++ < 16)
				host_log("gx: memory table: arena page %#lx was mapped anew while noted on pool page %#x\n",
					 (unsigned long)(first + done + i), src_at(first + done + i));
			if (set_page(first + done + i, src + i, PG_MAPPED | P_FRESH)) {
				/* Nothing to note it in: not to be kept, then. */
				host_log("gx: no memory for the table of the guest's pages\n");
				svcUnmapProcessCodeMemory(self, (u64)addr + (done + i) * GX_PAGE,
							  (u64)(pool + (src + i) * GX_PAGE), (run - i) * GX_PAGE);
				pool_give(src + i, run - i);
				run = i;
				ret = -1;
				break;
			}
			pool_owner[src + i] = first + done + i + 1;
			pool_last[src + i] = POOL_MAPPED;
			pool_when[src + i] = pool_events + 1;
		}
		pool_events++;
		/* Two more records for the kernel, until they are next counted:
		 * the guard at the top looks at this number. */
		kernel_blocks += 2;
		done += run;
	}
	if (!ret)
		ret = set_protection(first, n, prot);
	if (ret)
		unmap_pages(first, done);
	mem_leave();
	return ret;
}

int host_protect(void *addr, size_t len, int prot)
{
	int ret;

	mem_take();
	ret = set_protection(page_of(addr), len / GX_PAGE, prot);
	mem_leave();
	return ret;
}

int host_decommit(void *addr, size_t len)
{
	int ret;

	mem_take();
	ret = unmap_pages(page_of(addr), len / GX_PAGE);
	mem_leave();
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
	mem_take();
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
	mem_leave();
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
	mem_take();
	rc = svcMapSharedMemory(shared_handles[handle], addr, len, Perm_Rw);
	mem_leave();

	if (R_FAILED(rc))
		host_log("gx: cannot map shared memory at %p (%#x)\n", addr, rc);
	return R_FAILED(rc) ? -1 : 0;
}

void host_shared_unmap(int handle, void *addr, size_t len)
{
	mem_take();
	svcUnmapSharedMemory(shared_handles[handle], addr, len);
	mem_leave();
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
/* (576 MB since a Steam client: with 384 its browser was refused a piece once
 * its windows were up, and stops itself when that happens.) */
#define SHARED_BUDGET (576UL << 20)
#define HEAP_WORTH_SHARING (1UL << 30)

void __libnx_initheap(void)
{
	extern char *fake_heap_start, *fake_heap_end;
	u64 region = 0, total;
	void *addr = NULL;
	size_t size;

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

static int next_core, one_core;

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

/* Where each of our threads handles a fault (nx/exception.S): the far end of
 * its own stack, which it does not otherwise get to. Not the stack it was on
 * when the fault came: that may be the guest's, with pages that are not there
 * yet, and a fault within the handling of a fault takes the same locks again. */
#define FAULT_STACKS 128
#define FAULT_STACK_ROOM (64UL << 10)
struct {
	u64 tls, top;
} gx_fault_stacks[FAULT_STACKS];
/* The same place, where the entry finds it without looking: in a slot of the
 * thread's local storage, this far from its start (0: no such slot). The
 * time the entry takes is time in which the kernel loses the guest's vector
 * registers if it takes the processor away: see nx/exception.S. */
u32 gx_fault_slot;
/* Faults whose way in took long enough for that to have happened, and ways
 * back that were begun again for it (nx/exception.S counts). */
u64 gx_fault_away, gx_fault_again;
/* And times the kernel wanted the processor meanwhile and was made to wait. */
u64 gx_fault_pinned;

static void fault_slot(u64 top)
{
	static s32 slot = -2;
	u64 tls = (u64)armGetTls(), at = 0;

	if (__atomic_load_n(&slot, __ATOMIC_SEQ_CST) == -2) {
		s32 none = -2, got = threadTlsAlloc(NULL);

		if (!__atomic_compare_exchange_n(&slot, &none, got, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) &&
		    got >= 0)
			threadTlsFree(got);
	}
	if (slot < 0)
		return;
	threadTlsSet(slot, (void *)top);
	if (!top || gx_fault_slot)
		return;
	/* Where the C library keeps the slot, between what the kernel uses of
	 * the local storage and what the library keeps of the thread: found by
	 * what was just put there, which nothing else there can hold. */
	for (u64 off = 0x108; off <= 0x1d8; off += 8)
		if (*(u64 *)(tls + off) == top)
			at = at ? ~0UL : off;
	if (at && at != ~0UL)
		__atomic_store_n(&gx_fault_slot, (u32)at, __ATOMIC_SEQ_CST);
}

static void fault_stack(int on)
{
	u64 tls = (u64)armGetTls();
	Thread *self = threadGetSelf();

	if (!on)
		fault_slot(0);
	for (int i = 0; i < FAULT_STACKS; i++) {
		u64 none = 0;

		if (!on) {
			if (gx_fault_stacks[i].tls == tls)
				__atomic_store_n(&gx_fault_stacks[i].tls, 0, __ATOMIC_SEQ_CST);
			continue;
		}
		/* Claimed with a value no thread has, then filled in. */
		if (!self || !self->stack_mirror || self->stack_sz < 4 * FAULT_STACK_ROOM ||
		    !__atomic_compare_exchange_n(&gx_fault_stacks[i].tls, &none, ~0UL, 0, __ATOMIC_SEQ_CST,
						 __ATOMIC_SEQ_CST))
			continue;
		gx_fault_stacks[i].top = (u64)self->stack_mirror + FAULT_STACK_ROOM;
		/* Nothing kept there yet (nx/exception.S). */
		memset((void *)(gx_fault_stacks[i].top - 1024), 0, 1024);
		__atomic_store_n(&gx_fault_stacks[i].tls, tls, __ATOMIC_SEQ_CST);
		fault_slot(gx_fault_stacks[i].top);
		return;
	}
}

static void thread_start(void *p)
{
	struct start *s = p;

	fault_stack(1);
	s->fn(s->arg);
	fault_stack(0);
}

static int start_thread(void (*fn)(void *), void *arg, size_t stack)
{
	struct start *s = malloc(sizeof(*s));   /* lives as long as the thread */
	/* All on one processor with -C1: they then run one at a time, which
	 * tells what goes wrong only when two run at once from what goes wrong
	 * anyway. (How many the guest is told it has, -c, is another matter:
	 * a browser told it has one does things differently.) */
	int core = one_core ? 0 : __atomic_fetch_add(&next_core, 1, __ATOMIC_RELAXED) % 3;
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
		svcSetThreadCoreMask(s->thread.handle, core, one_core ? 0x1 : 0x7);
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

	fault_stack(0);
	mutexLock(&ended_lock);
	s->next = ended;
	ended = s;
	mutexUnlock(&ended_lock);
	threadExit();
	__builtin_unreachable();
}

void host_yield(void) { svcSleepThread(YieldType_WithCoreMigration); }

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

struct host_lock { Mutex m; void *owner; };
struct host_cond { CondVar c; };

struct host_lock *host_lock_new(void)
{
	struct host_lock *l = malloc(sizeof(*l));

	mutexInit(&l->m);
	l->owner = NULL;
	return l;
}

void host_lock_acquire(struct host_lock *l)
{
	mutexLock(&l->m);
	gate_in(&l->owner, "a part of gx");
}

void host_lock_release(struct host_lock *l)
{
	__atomic_store_n(&l->owner, NULL, __ATOMIC_SEQ_CST);
	mutexUnlock(&l->m);
}

struct host_cond *host_cond_new(void)
{
	struct host_cond *c = malloc(sizeof(*c));

	condvarInit(&c->c);
	return c;
}

int host_cond_wait(struct host_cond *c, struct host_lock *l, int64_t ns)
{
	int ret = 0;

	__atomic_store_n(&l->owner, NULL, __ATOMIC_SEQ_CST);
	if (ns < 0)
		condvarWait(&c->c, &l->m);
	else
		ret = R_FAILED(condvarWaitTimeout(&c->c, &l->m, ns));
	gate_in(&l->owner, "a part of gx");
	return ret;
}

void host_cond_signal(struct host_cond *c) { condvarWakeOne(&c->c); }

/* ---------- the rest of the host interface ---------- */

void host_random(void *buf, size_t len) { randomGet(buf, len); }

/* Not yet: the exception handler would have to take faults of the kernel's
 * and stand on a stack of its own (see gmem_watch()). */
int host_write_faults(void) { return 0; }
int host_thread_stop(uint64_t thread) { (void)thread; return 0; }
void host_threads_go(void) {}

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
	console_off();
	window_surface = eglCreateWindowSurface(dpy, config, nwindowGetDefault(), NULL);
	if (window_surface == EGL_NO_SURFACE)
		host_log("gx: gl: no window surface (%#x)\n", eglGetError());
	else
		host_log("gx: gl: the guest has the screen\n");
	return window_surface == EGL_NO_SURFACE ? NULL : window_surface;
}

static void gl_presented(void *dpy, void *screen)
{
	gl_swaps++;
}

static const struct glwin_host gl_host = { gl_raw, gl_screen, gl_presented };

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
 * jump or an access, or (exec is 2, addr where it is) for an instruction the
 * processor does not have, on the stack of the thread it happened to: returns
 * whether the thread is to try again. */
/* The last faults that came here and what was made of them, for the report
 * of one that could not be turned around. */
static struct fault_seen {
	u64 addr, tp;
	int kind, fixed;
} faults_seen[16];
static unsigned faults_count;

/* Does the processor keep its vector registers for us? memchr() looks through
 * memory that never changes: a wrong answer is the registers changing under
 * it. Counted where faults are handled and where they are not. */
static const uint8_t still[64] = { [37] = 1 };
static unsigned long vector_tries[2], vector_slips[2];

static void try_vectors(int where)
{
	for (int i = 0; i < 64; i++) {
		const uint8_t *found = memchr(still + (i & 7), 1, 48);

		vector_tries[where]++;
		if (found != still + 37 && vector_slips[where]++ < 4)
			host_log("gx: memchr() of memory that does not change went wrong %s (%ld for %d)\n",
				 where ? "where faults are handled" : "elsewhere",
				 found ? (long)(found - still) : -1L, 37);
	}
}

/* The guest's registers as gx_fault_tramp keeps them (nx/exception.S): x0-x8,
 * lr, sp and pc as the kernel gave them, then x9-x18 and x29, and x19-x28. */
static u64 *reg_at(u64 *kept, unsigned r)
{
	if (r <= 8)
		return &kept[r];
	if (r <= 18)
		return &kept[16 + (r - 9)];
	if (r <= 28)
		return &kept[96 + (r - 19)];
	return r == 29 ? &kept[26] : &kept[9];
}

/* An atomic instruction of ARMv8.1 the processor does not have, in code that
 * cannot be given a veneer for it (gmem_illegal()): what a compiler inside
 * the program wrote, which is neither where it will stay nor ours to write
 * to. Done here instead, on the registers kept: returns whether it was one. */
static int atomic_by_hand(u64 *kept)
{
	u32 insn = *(u32 *)kept[11];
	int is_casp = (insn & 0xBFA07C00u) == 0x08207C00u;
	int is_cas = (insn & 0x3FA07C00u) == 0x08A07C00u;
	int is_ld = (insn & 0x3F200C00u) == 0x38200000u;
	unsigned size = is_casp ? 2 + ((insn >> 30) & 1) : insn >> 30;
	unsigned rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
	unsigned opc = (insn >> 12) & 7, o3 = (insn >> 15) & 1;
	u64 mask = size == 3 ? ~0UL : (1UL << (8 << size)) - 1;
	u64 where = rn == 31 ? kept[10] : *reg_at(kept, rn);
	u64 vs = rs == 31 ? 0 : *reg_at(kept, rs), vt = rt == 31 ? 0 : *reg_at(kept, rt);
	u64 old = 0;

	if (!is_cas && !is_casp && !is_ld)
		return 0;
	if ((is_ld && o3 && opc) || (is_casp && ((rs & 1) || (rt & 1) || rs == 31 || rt == 31)))
		return 0;
	if (!gmem_accessible(where, (is_casp ? 2UL : 1UL) << size, 1))
		return 0;
	if (is_casp) {
		/* Two words as one: nothing the processor has does that in one
		 * go but its exclusive pair, which is what this is. */
		u64 e0 = vs & mask, e1 = *reg_at(kept, rs + 1) & mask;
		u64 n0 = vt & mask, n1 = *reg_at(kept, rt + 1) & mask, o0, o1;

		if (size == 3) {
			u32 failed;

			__asm__ volatile(
				"1: ldaxp %[o0], %[o1], [%[at]]\n"
				"   cmp %[o0], %[e0]\n"
				"   ccmp %[o1], %[e1], #0, eq\n"
				"   b.ne 2f\n"
				"   stlxp %w[f], %[n0], %[n1], [%[at]]\n"
				"   cbnz %w[f], 1b\n"
				"   b 3f\n"
				"2: stlxp %w[f], %[o0], %[o1], [%[at]]\n"
				"   cbnz %w[f], 1b\n"
				"3:\n"
				: [o0] "=&r"(o0), [o1] "=&r"(o1), [f] "=&r"(failed)
				: [at] "r"(where), [e0] "r"(e0), [e1] "r"(e1), [n0] "r"(n0), [n1] "r"(n1)
				: "cc", "memory");
		} else {
			u64 expected = (e1 << 32) | e0;

			__atomic_compare_exchange_n((u64 *)where, &expected, (n1 << 32) | n0, 0,
						    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
			o0 = expected & mask;
			o1 = expected >> 32;
		}
		*reg_at(kept, rs) = o0;
		*reg_at(kept, rs + 1) = o1;
		kept[11] += 4;
		return 1;
	}
#define BY_SIZE(type) do { \
		type *at = (type *)where, s = (type)vs, t = (type)vt; \
		if (is_cas) { \
			type expected = s; \
			__atomic_compare_exchange_n(at, &expected, t, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
			old = expected; \
		} else if (o3) { \
			old = __atomic_exchange_n(at, s, __ATOMIC_SEQ_CST); \
		} else if (opc == 0) { \
			old = __atomic_fetch_add(at, s, __ATOMIC_SEQ_CST); \
		} else if (opc == 1) { \
			old = __atomic_fetch_and(at, (type)~s, __ATOMIC_SEQ_CST); \
		} else if (opc == 2) { \
			old = __atomic_fetch_xor(at, s, __ATOMIC_SEQ_CST); \
		} else if (opc == 3) { \
			old = __atomic_fetch_or(at, s, __ATOMIC_SEQ_CST); \
		} else { \
			/* The larger or the smaller of the two, signed or not. */ \
			type was = __atomic_load_n(at, __ATOMIC_SEQ_CST), put; \
			do { \
				int keep = opc == 4 ? signed_of(was) > signed_of(s) : \
					   opc == 5 ? signed_of(was) < signed_of(s) : \
					   opc == 6 ? was > s : was < s; \
				put = keep ? was : s; \
			} while (!__atomic_compare_exchange_n(at, &was, put, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); \
			old = was; \
		} \
	} while (0)
	if (size == 0) {
#define signed_of(v) ((long)(int8_t)(v))
		BY_SIZE(uint8_t);
#undef signed_of
	} else if (size == 1) {
#define signed_of(v) ((long)(int16_t)(v))
		BY_SIZE(uint16_t);
#undef signed_of
	} else if (size == 2) {
#define signed_of(v) ((long)(int32_t)(v))
		BY_SIZE(uint32_t);
#undef signed_of
	} else {
#define signed_of(v) ((long)(v))
		BY_SIZE(uint64_t);
#undef signed_of
	}
#undef BY_SIZE
	/* What was there goes to the register that held what was expected (a
	 * compare and swap) or to the one named for it. */
	if (is_cas) {
		if (rs != 31)
			*reg_at(kept, rs) = old & mask;
	} else if (rt != 31) {
		*reg_at(kept, rt) = old & mask;
	}
	kept[11] += 4;
	return 1;
}

int gx_nx_fault(u64 addr, int exec, u64 *kept)
{
	static int said;
	struct fault_seen *seen = &faults_seen[__atomic_fetch_add(&faults_count, 1, __ATOMIC_SEQ_CST) % 16];
	int fixed;

	try_vectors(1);
	seen->addr = addr;
	seen->kind = exec;
	seen->fixed = -1;
	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(seen->tp));
	if (exec == 2) {
		static unsigned long by_hand;

		fixed = gmem_illegal(addr);
		if (!fixed && kept && gmem_describe(addr, &(uint64_t){ 0 }) && atomic_by_hand(kept)) {
			fixed = 1;
			by_hand++;
			if (!(by_hand & (by_hand - 1)))
				host_log("gx: atomic instructions of compiled code done by hand: %lu\n", by_hand);
		}
		return seen->fixed = fixed;
	}
	if (exec == 3)
		return seen->fixed = gmem_counter(addr);
	fixed = seen->fixed = gmem_fault(addr, exec);

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

	if (!__atomic_exchange_n(&reported, 1, __ATOMIC_SEQ_CST)) {
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
		if (!(ctx->pc.x & 3) && gmem_accessible(ctx->pc.x, 4, 0) && gmem_describe(ctx->pc.x, &(uint64_t){ 0 })) {
			host_log("  the instruction there is %#x\n", *(u32 *)ctx->pc.x);
			/* And what is around it, and around where it was called from:
			 * code that a program wrote for itself is nowhere else. */
			for (int which = 0; which < 2; which++) {
				u64 at = ((which ? ctx->lr.x : ctx->pc.x) & ~3UL) - 96;

				if (!gmem_accessible(at, 192, 0))
					continue;
				host_log("  from %#lx:", (unsigned long)at);
				for (int i = 0; i < 48; i++)
					host_log("%s%08x", i % 8 ? " " : "\n   ", ((u32 *)at)[i]);
				host_log("\n");
			}
		}
		report_address("lr ", ctx->lr.x);
		host_log("  far %#lx\n  sp  %#lx\n  esr %#x (class %#x)\n", (unsigned long)ctx->far.x,
			 (unsigned long)ctx->sp.x, ctx->esr, ctx->esr >> 26);
		host_log("  the faults before it, %u in all (kind 0 an access, 1 a jump, 2 an instruction):\n",
			 faults_count);
		for (unsigned i = 0; i < 16 && i < faults_count; i++) {
			struct fault_seen *seen = &faults_seen[(faults_count - 1 - i) % 16];

			host_log("    %#lx kind %d, answered %d, thread pointer %#lx\n", (unsigned long)seen->addr,
				 seen->kind, seen->fixed, (unsigned long)seen->tp);
		}
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
		for (int i = 0; i < 29; i++)
			host_log("  x%d  %#lx\n", i, (unsigned long)ctx->cpu_gprs[i].x);
		host_log("  fp  %#lx\n  pstate %#x\n", (unsigned long)ctx->fp.x, ctx->pstate);
		{
			/* Whose thread of the guest's, if any: by the thread pointer
			 * it has now, and by where its stack is. */
			u64 tp;

			__asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
			host_log("  tpidr_el0 %#lx\n", (unsigned long)tp);
			gx_report_fault(tp, ctx->sp.x);
		}
		/* What is on its stack, if that is memory that can be read. */
		{
			MemoryInfo info = { 0 };
			u32 page_info;
			u64 sp = ctx->sp.x & ~7UL;

			if (R_SUCCEEDED(svcQueryMemory(&info, &page_info, sp)) && (info.perm & Perm_R) &&
			    info.addr + info.size >= sp + 48 * 8) {
				for (int i = 0; i < 48; i += 4)
					host_log("  sp+%-3d %#lx %#lx %#lx %#lx\n", i * 8,
						 (unsigned long)((u64 *)sp)[i], (unsigned long)((u64 *)sp)[i + 1],
						 (unsigned long)((u64 *)sp)[i + 2], (unsigned long)((u64 *)sp)[i + 3]);
			}
		}
		host_log("Press + to quit.\n");
		sync_log();
		finished = 1;
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
};

static struct image root_image, profile_image;
static struct lkl_disk root_disk = { .handle = &root_image };
static struct lkl_disk profile_disk = { .handle = &profile_image };
static int profile_mounted;

static int image_capacity(struct lkl_disk disk, unsigned long long *res)
{
	struct image *image = disk.handle;

	mutexLock(&image->lock);
	fseeko(image->file, 0, SEEK_END);
	*res = ftello(image->file);
	mutexUnlock(&image->lock);
	return 0;
}

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
	mutexLock(&image->lock);
	if (fseeko(image->file, req->sector * 512, SEEK_SET))
		status = LKL_DEV_BLK_STATUS_IOERR;
	for (int i = 0; i < req->count && status == LKL_DEV_BLK_STATUS_OK; i++) {
		size_t len = req->buf[i].iov_len, done;

		if (req->type == LKL_DEV_BLK_TYPE_READ) {
			done = fread(req->buf[i].iov_base, 1, len, image->file);
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

/* The profile, the first time: an empty filesystem cannot be made here, so one
 * comes packed next to the program (it packs to nothing) and is unpacked. */
static void unpack_profile(const char *path)
{
	static u8 buffer[1 << 20];
	char packed[520];
	gzFile in;
	FILE *out;
	int n, ok = 1;
	long total = 0;

	snprintf(packed, sizeof(packed), "%s.gz", path);
	in = gzopen(packed, "rb");
	if (!in)
		return;
	out = fopen(path, "wb");
	host_log("gx: making %s, where the browser keeps what is to last (once, a moment)\n", path);
	while (out && (n = gzread(in, buffer, sizeof(buffer))) > 0) {
		if (fwrite(buffer, 1, n, out) != (size_t)n) {
			ok = 0;
			break;
		}
		total += n;
	}
	gzclose(in);
	if (out && fclose(out))
		ok = 0;
	if (!out || !ok || !total) {
		host_log("gx: could not make it (is the card full?)\n");
		remove(path);
	}
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
/* (With the vector registers: it hands numbers that are not whole to the
 * drawing library, and no fault is handled through here.) */
#pragma GCC push_options
#pragma GCC reset_options
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
#pragma GCC pop_options

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

int host_ask_text(char *text, size_t room)
{
	char initial[512];

	snprintf(initial, sizeof(initial), "%s", text);
	return ask_text("Text", initial, text, room);
}

int host_input(struct cdp_input *in)
{
	static PadState pad;
	static int ready, touching, last_x, last_y;
	static u64 last_scroll;
	HidTouchScreenState touch = { 0 };
	HidAnalogStickState left, right;
	u64 down, now = armGetSystemTick();
	int dy;

	if (!ready) {
		padInitializeDefault(&pad);
		hidInitializeTouchScreen();
		ready = 1;
	}
	memset(in, 0, sizeof(*in));

	if (hidGetTouchScreenStates(&touch, 1) && touch.count) {
		int x = touch.touches[0].x, y = touch.touches[0].y;

		if (!touching || abs(x - last_x) + abs(y - last_y) >= 2) {
			in->type = touching ? CDP_TOUCH_MOVE : CDP_TOUCH_DOWN;
			in->x = last_x = x;
			in->y = last_y = y;
			touching = 1;
			return 1;
		}
		return 0;
	}
	if (touching) {
		touching = 0;
		in->type = CDP_TOUCH_UP;
		in->x = last_x;
		in->y = last_y;
		return 1;
	}

	padUpdate(&pad);
	down = padGetButtonsDown(&pad);
	if (down & HidNpadButton_B) {
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
		/* The keyboard, on the address of the window. */
		in->type = CDP_KEYBOARD;
	} else if (wayland && (down & (HidNpadButton_Up | HidNpadButton_Down | HidNpadButton_Left |
				       HidNpadButton_Right))) {
		/* The arrow keys, which move the focus from one thing of the
		 * page to the next in that direction (Chromium is started with
		 * --enable-spatial-navigation). */
		in->type = CDP_KEY;
		in->x = (down & HidNpadButton_Up) ? 103 : (down & HidNpadButton_Down) ? 108 :
			(down & HidNpadButton_Left) ? 105 : 106;
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
	} else if (armTicksToNs(now - last_scroll) > 16000000ULL) {
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
#define MAX_ARGS 32

static char commands[MAX_COMMANDS][1024];
static const char *root_image_name;   /* @<image>: an ext4 image next to the program as the root */
static const char *profile_name;      /* =<image>: one that is the guest's /root, and written */
static int browser;                   /* -b: the program is a Chromium to drive (cdp.c) */
static int cpu_count;                 /* -c<n>: processors the guest is told about */
static int kernel_mb = KERNEL_MB;     /* -k<n>: megabytes the kernel has */
static char *more_env[16];            /* -e<name>=<value>: one more variable for the guest, ahead of ours */
static int more_env_count;
static int command_count;
static char *guest_argv[MAX_ARGS + 1];
static int guest_argc;

static void read_commands(void)
{
	FILE *f = fopen(GX_DIR "/programs.txt", "r");
	char line[1024];

	while (f && command_count < MAX_COMMANDS && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] && line[0] != '#')
			strcpy(commands[command_count++], line);
	}
	if (f)
		fclose(f);
	/* No list: no menu either, see main(). */
	if (!command_count)
		strcpy(commands[0], DEFAULT_COMMAND);
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

	static char *all_env[48];
	char kernel_args[64];
	int env_count = 0;

	/* The guest's variables: what the menu's line gives, then ours. */
	for (int i = 0; i < more_env_count; i++)
		all_env[env_count++] = more_env[i];
	for (int i = 0; guest_env[i] && env_count < 47; i++) {
		/* Not one the line gave: a name twice is the first to some
		 * (getenv()) and the last to others (the loader). */
		size_t name = strcspn(guest_env[i], "=") + 1;
		int given = 0;

		for (int j = 0; j < more_env_count; j++)
			given |= !strncmp(more_env[j], guest_env[i], name);
		if (!given)
			all_env[env_count++] = guest_env[i];
	}
	all_env[env_count] = NULL;
	snprintf(kernel_args, sizeof(kernel_args), "mem=%dM loglevel=4", kernel_mb);
	host_log("gx: starting the kernel (%s)\n", kernel_args);
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
			/* The SD card is slow by small pieces. */
			setvbuf(root_image.file, NULL, _IOFBF, 1 << 20);
			disk_id = lkl_disk_add(&root_disk);
		}
		if (disk_id < 0) {
			host_log("gx: cannot use %s as a disk (%d)\n", path, disk_id);
			host_exit(1);
		}
	}
	if (!ret && profile_name && disk_id >= 0) {
		char path[512];

		snprintf(path, sizeof(path), GX_DIR "/%s", profile_name);
		profile_image.file = fopen(path, "r+b");
		if (!profile_image.file) {
			unpack_profile(path);
			profile_image.file = fopen(path, "r+b");
		}
		if (profile_image.file) {
			setvbuf(profile_image.file, NULL, _IOFBF, 1 << 16);
			profile_id = lkl_disk_add(&profile_disk);
		}
		if (profile_id < 0)
			host_log("gx: cannot use %s as a disk: nothing will be kept\n", path);
	}
	if (!ret)
		ret = lkl_start_kernel(kernel_args);
	if (ret) {
		host_log("gx: the kernel did not start (%d)\n", ret);
		host_exit(1);
	}
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
		if (!ret && profile_id >= 0) {
			/* Before the root is the root: its /root is where the
			 * profile goes, and the device's name cannot be made on a
			 * root that is read-only. */
			static const char profile_node[] = "/gx-profile-disk";
			long args[6] = { LKL_AT_FDCWD, (long)profile_node, LKL_S_IFBLK | 0600 };
			int err = lkl_get_virtio_blkdev(profile_id, 0, &dev);

			args[3] = dev;
			if (!err)
				err = lkl_syscall(__lkl__NR_mknodat, args);
			if (!err)
				err = lkl_sys_mount((char *)profile_node, "/gx-root/root", "ext4", 0, NULL);
			if (err)
				host_log("gx: cannot mount %s (%d): nothing will be kept\n", profile_name, err);
			else
				host_log("gx: what is to last is kept in %s\n", profile_name);
			profile_mounted = !err;
		}
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
	if (profile_mounted)
		unlock_profile();
	if (cpu_count)
		gx_set_cpus(cpu_count);
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
	if (gx_pool || gmem_lazy)
		host_log("gx: %s%s\n", gx_pool ? "guest threads share the host's; " : "",
			 gmem_lazy ? "memory is committed when first used" : "");
	gx_exec(guest_argv[0], guest_argc, guest_argv, all_env);
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
	for (int i = 0; i < 1000; i++)
		try_vectors(0);
	/* The vector registers of a thread that came back from a fault: do
	 * they last, when the processor is taken from it for a moment? */
	{
		extern unsigned long gx_vector_test(unsigned long mode, unsigned long turns);
		static unsigned long lost[3], rounds;

		for (int mode = 0; mode < 3; mode++)
			lost[mode] += gx_vector_test(mode, 15000000);
		rounds++;
		host_log("gx: vector registers lost, in %lu rounds: %lu with no fault before, %lu after a fault, "
			 "%lu after a fault and a call to the kernel\n", rounds, lost[0], lost[1], lost[2]);
		{
			extern unsigned long gx_vector_test_across(unsigned long times, unsigned long turns);
			static unsigned long across[2];

			across[0] += gx_vector_test_across(2000, 0);
			across[1] += gx_vector_test_across(12, 20000000);
			host_log("gx: vector registers lost across a fault: %lu of %lu at once, %lu of %lu after a while "
				 "without them\n", across[0], rounds * 2000, across[1], rounds * 12);
		}
		/* And for the guest's own faults: how many there were, how many
		 * times the processor may have been taken away on the way in
		 * (when the registers are lost) and on the way back (when they
		 * are put back again). */
		host_log("gx: faults: %u in all; the processor away on the way in %lu times, on the way back %lu "
			 "(begun again); kept from being taken away %lu times; found by slot %#x\n", faults_count,
			 (unsigned long)gx_fault_away, (unsigned long)gx_fault_again,
			 (unsigned long)gx_fault_pinned, gx_fault_slot);
	}
	if (vector_slips[0] || vector_slips[1])
		host_log("gx: memchr() wrong %lu times of %lu where faults are handled, %lu of %lu elsewhere\n",
			 vector_slips[1], vector_tries[1], vector_slips[0], vector_tries[0]);
	/* The two records of the pool, one against the other. */
	{
		size_t wrong = 0, where = 0;
		uint8_t as = 0;

		mem_take();
		for (size_t p = 0; p < pool_pages; p++)
			if (pool_owner[p] ? pool_used[p] != 1 : pool_used[p] > 1) {
				if (!wrong++) {
					where = p;
					as = pool_used[p];
				}
			}
		if (wrong)
			host_log("gx: memory table: %lu pool pages are noted wrongly, the first %#lx as %#x while behind "
				 "arena page %#x (last event %d, %u events ago)\n", (unsigned long)wrong,
				 (unsigned long)where, as, pool_owner[where] - 1, pool_last[where],
				 pool_events - pool_when[where]);
		mem_leave();
	}
	host_log("gx: guest blocks by size: %lu up to 16 KB, %lu to 256 KB, %lu to 2 MB, %lu larger; "
		 "by use: %lu nothing, %lu read, %lu write, %lu run\n",
		 sizes[0], sizes[1], sizes[2], sizes[3], perms[0], perms[1], perms[2], perms[3]);
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
			gx_report_kernel_memory();
			kernel_asked = 0;
		}
		if (leaving && !left) {
			/* The user quits: what the kernel still has in memory of
			 * the profile goes to its image first. Asked here because
			 * the main thread cannot ask the kernel anything. */
			long none[6] = { 0 };

			if (profile_mounted)
				lkl_syscall(__lkl__NR_sync, none);
			left = 1;
		}
		svcSleepThread(100000000ULL);
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
		if (gl_swaps != swaps) {
			host_log("gx: gl: %u pictures in 10 s\n", gl_swaps - swaps);
			swaps = gl_swaps;
		}
		report_kernel_use();
		host_log("gx: host threads: %d\n", count_threads() + 1);   /* and this one */
		gx_report_threads();
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

	consoleInit(NULL);
	padConfigureInput(1, HidNpadStyleSet_NpadStandard);
	padInitializeDefault(&pad);
	if (!enough_addresses(&pad)) {
		consoleExit(NULL);
		svcExitProcess();   /* see __libnx_initheap() */
	}
	mkdir(GX_DIR, 0777);
	read_commands();
	choice = command_count ? menu(&pad) : 0;
	if (choice < 0) {
		consoleExit(NULL);
		svcExitProcess();   /* see __libnx_initheap() */
	}
	consoleClear();
	log_file = fopen(GX_DIR "/chromium-nx.log", "w");

	if (commands[choice][0] == '[' && strchr(commands[choice], ']'))
		memmove(commands[choice], strchr(commands[choice], ']') + 1,
			strlen(strchr(commands[choice], ']')));
	for (char *word = strtok(commands[choice], " "); word && guest_argc < MAX_ARGS;
	     word = strtok(NULL, " ")) {
		/* Before the program: -t, -b, @<root image>, ><guest file to copy out>. */
		if (!guest_argc && !strcmp(word, "-t"))
			gx_trace = 1;
		else if (!guest_argc && !strcmp(word, "-b"))
			browser = 1;
		else if (!guest_argc && !strcmp(word, "-w"))
			wayland = 1;
		else if (!guest_argc && !strncmp(word, "-A", 2)) {
			int l, t, r, b;   /* -A<left>,<top>,<right>,<bottom>: the address bar */

			if (sscanf(word + 2, "%d,%d,%d,%d", &l, &t, &r, &b) == 4)
				wl_address_bar(l, t, r, b);
		}
		else if (!guest_argc && !strncmp(word, "-c", 2) && word[2])
			cpu_count = atoi(word + 2);
		else if (!guest_argc && !strcmp(word, "-C1"))
			one_core = 1;       /* all threads on one processor of the console's */
		else if (!guest_argc && !strncmp(word, "-k", 2) && atoi(word + 2) >= 64)
			kernel_mb = atoi(word + 2);
		else if (!guest_argc && !strncmp(word, "-m", 2) && atoi(word + 2) >= 48)
			heap_kept = (size_t)atoi(word + 2) << 20;
		else if (!guest_argc && !strncmp(word, "-P", 2)) {
			/* -P[n]: guest threads that wait give their host thread
			 * up, and at most n are made (gsys.c). */
			gx_pool = 1;
			if (atoi(word + 2) > 0)
				gx_pool_cap = atoi(word + 2);
		}
		else if (!guest_argc && !strncmp(word, "-x", 2) && word[2])
			gx_more_for = word + 2;   /* -x<end of a program's path>, then -X<argument> for each */
		else if (!guest_argc && !strncmp(word, "-X", 2) && word[2] && gx_more_count < 16)
			gx_more[gx_more_count++] = word + 2;
		else if (!guest_argc && !strcmp(word, "-v"))
			gx_processes = 1;   /* the log says when processes start and end */
		else if (!guest_argc && !strcmp(word, "-L"))
			gmem_lazy = 1;      /* memory is committed when first used (gmem.c) */
		else if (!guest_argc && !strncmp(word, "-L", 2) && atoi(word + 2) > 0)
			gmem_lazy = 1, lazy_pages_asked = atoi(word + 2);   /* and in pieces of so many pages */
		else if (!guest_argc && !strncmp(word, "-e", 2) && strchr(word, '=') && more_env_count < 16)
			more_env[more_env_count++] = word + 2;
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
		threadStart(&watchdog_thread);
	}
	if (R_SUCCEEDED(threadCreate(&probe_thread, kernel_probe, NULL, NULL, 0x10000, GUEST_PRIO, -2))) {
		free_cores(probe_thread.handle);
		know_thread(probe_thread.handle, "probe");
		threadStart(&probe_thread);
	}
	if (start_thread(run, NULL, MAIN_STACK))
		printf("gx: cannot start\n");

	while (appletMainLoop()) {
		padUpdate(&pad);
		if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
			break;
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
	svcExitProcess();
	return 0;
}
