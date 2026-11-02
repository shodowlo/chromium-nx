/*
 * gmem.c - the guest's address space.
 *
 * LKL has no MMU, so mmap/munmap/mprotect/brk are ours. One arena is reserved
 * from the host and a byte per page says what is mapped there. File mappings
 * are private copies read through LKL. Executable file mappings get their
 * `svc #0` instructions patched as they are mapped, which is what lets ld.so
 * and dlopen() load libraries we never saw.
 *
 * Anonymous memory that is not code ("data": heaps, stacks) is always readable
 * and writable for the host, whatever protection the guest gives it, and is
 * committed 256 KB at a time when the guest commits pieces of a reservation.
 * The guest cannot tell: there are no signals here, so a protection it sets is
 * only ever a fault it does not want. The host can: its kernel keeps a record
 * per run of pages mapped alike, out of a store that runs out (Horizon stops
 * serving the process at about ten thousand), and an allocator that commits
 * its memory sixteen kilobytes at a time makes one run of each.
 *
 * Shared file mappings (MAP_SHARED) are views of one copy of the file, made
 * of memory the host can map at several addresses. The copy is written back
 * to the file when its last view goes, so that a later mapping finds it. On
 * a host without such memory, every view of a file is at the same address.
 */
#include <stdio.h>
#include <string.h>
#include <lkl.h>
#include "gx.h"

/* How much address space the guest gets. Reserving costs nothing, and some
 * programs reserve a lot (Chromium's allocators take tens of gigabytes). */
#ifndef GX_ARENA_SIZE
#define GX_ARENA_SIZE (4UL << 30)
#endif
#define ARENA_MAX_PAGES (GX_ARENA_SIZE / GX_PAGE)
#define ARENA_MIN (1UL << 30)
#define BRK_SIZE (256UL << 20)

#define PG_USED 0x80
#define PG_COMMITTED 0x40
#define PG_SHARED 0x20   /* a view of a shared file */
#define PG_COPY 0x10     /* the one copy of a shared file, where views cannot be told apart */
#define PG_HOLE 0x08     /* not ours: the host could not reserve it */
#define PG_PROT 0x07

#define SVC0 0xD4000001u
#define VENEER_SIZE 32
#define BRANCH_RANGE (128L << 20)
#define MAX_IMAGES 1024
/* What data is committed by, inside reservations. Large, because a host may
 * pay for every piece it is asked to map whatever its size: Horizon keeps two
 * records per piece, never merged with their neighbours, and has room for
 * about ten thousand in all; a browser with a gigabyte of data in pieces of
 * 256 KB went through them. */
#define DATA_UNIT (1UL << 20)
#define DATA_PROT (GX_PROT_READ | GX_PROT_WRITE)
#define MAX_SHARED 512
#define MAX_VIEWS 4096
#define MAX_CHUNKS 256
#define KEEP_MAX 24            /* files kept without a view before a clean-up */
#define RAMFS_MAGIC 0x858458f6L
#define CHUNK_MAX_PAGES 4096   /* 16 MB */
#define MADV_DONTNEED 4

/* An image is one non-fixed file mapping (what ld.so makes first for each
 * library) followed by the pool its veneers are taken from. */
struct image {
	uint64_t start, end;
	uint64_t pool, pool_end, pool_next;
};

/*
 * A piece of the host's shared memory. Hosts only map such a piece whole, and
 * may have few to give (Horizon has 80 for the whole console): a large shared
 * file has a chunk of its own, and small ones are put together in chunks of
 * the size the host suggests.
 */
struct chunk {
	uint64_t addr, size;   /* our own mapping of it; size is 0 when the slot is free */
	uint64_t used;         /* pages of it that files have */
	int handle;
	int pooled;            /* shared between files, by chunk_pages[] */
};

/* A file with shared mappings: its contents while it has some. */
struct shared {
	uint64_t dev, ino;
	uint64_t addr, size;   /* where we see all of it, to load and save it */
	struct chunk *chunk;   /* NULL: the host has no shared memory, views are at addr */
	uint64_t off;          /* where the file is in its chunk */
	int views;             /* mappings the guest has of it */
	int kept;              /* without views, but the file may be mapped again */
	int fd;                /* the file, to write the copy back */
	int own;               /* fd is an opening of our own: see is_dead() */
	int written;           /* some view was writable */
};

/* A view maps the whole chunk of its file; the guest has the part it asked
 * for. */
struct view {
	uint64_t base;         /* where the chunk is mapped for this view */
	uint64_t addr, pages;  /* the guest's part; pages is 0 when the slot is free */
	struct shared *object;
};

static struct chunk chunks[MAX_CHUNKS];
static uint8_t chunk_pages[MAX_CHUNKS][CHUNK_MAX_PAGES];
static struct shared shared[MAX_SHARED];
static struct view views[MAX_VIEWS];
static uint8_t *arena;
static uint8_t page_state[ARENA_MAX_PAGES];
static uint64_t arena_size;    /* what the host could reserve */
static uint64_t search_hint;
static struct image images[MAX_IMAGES];
static uint64_t brk_start, brk_cur;
static struct host_lock *lock;

static inline uint64_t page_of(uint64_t addr) { return (addr - (uint64_t)arena) / GX_PAGE; }
static inline int in_arena(uint64_t addr, uint64_t len)
{
	return addr >= (uint64_t)arena && len <= arena_size &&
	       addr - (uint64_t)arena <= arena_size - len;
}

int gmem_init(void)
{
	size_t size = GX_ARENA_SIZE;

	/* As much as asked for if the host has it, less otherwise: programs
	 * that reserve a lot will find out. */
	arena = host_reserve(&size);
	arena_size = GX_PAGE_DOWN(size);
	void *hole;

	if (!arena || arena_size < ARENA_MIN || arena_size > GX_ARENA_SIZE)
		return -1;
	for (int i = 0; host_reserved_hole(i, &hole, &size); i++) {
		uint64_t from = GX_PAGE_DOWN(hole), to = GX_PAGE_UP((uint64_t)hole + size);

		if (in_arena(from, to - from))
			memset(&page_state[page_of(from)], PG_USED | PG_HOLE, (to - from) / GX_PAGE);
	}
	lock = host_lock_new();
	return 0;
}

/* Finds free pages at an address that is a multiple of align bytes. */
static uint64_t find_free_aligned(uint64_t pages, uint64_t align)
{
	for (int pass = 0; pass < 2; pass++) {
		uint64_t addr = (uint64_t)arena + (pass ? 0 : search_hint) * GX_PAGE;

		for (;;) {
			uint64_t first, p;

			addr = (addr + align - 1) & ~(align - 1);
			first = page_of(addr);
			if (first >= (arena_size / GX_PAGE) || pages > (arena_size / GX_PAGE) - first)
				break;
			for (p = first; p < first + pages && !(page_state[p] & PG_USED); p++)
				;
			if (p == first + pages) {
				if (align == GX_PAGE)
					search_hint = p;
				return addr;
			}
			addr = (uint64_t)arena + (p + 1) * GX_PAGE;
		}
	}
	return 0;
}

static uint64_t find_free(uint64_t pages)
{
	return find_free_aligned(pages, GX_PAGE);
}

/* Large anonymous mappings are given the alignment of their size: allocators
 * that need aligned memory (glibc's arenas, PartitionAlloc's pools of tens of
 * gigabytes) otherwise map twice as much to cut an aligned piece out of it,
 * which the arena may not have. */
static uint64_t find_free_anonymous(uint64_t size)
{
	uint64_t align = GX_PAGE;

	if (size >= (2UL << 20))
		while (align * 2 <= size && align < (32UL << 30))
			align *= 2;
	return find_free_aligned(size / GX_PAGE, align);
}

/* What a host that commits for real has to find: pages committed now and at
 * most, and how far into the arena the guest got. */
static uint64_t committed_pages, committed_peak, top_page;
static int chunk_count, chunk_peak;   /* pieces of the host's shared memory */
static uint64_t wx_to_write, wx_to_exec;   /* see gmem_fault() */
/* And what they cost: all of it, the longest, and the longest wait for the lock. */
static uint64_t wx_ns, wx_ns_max, wx_wait_max;
/* What MADV_DONTNEED was given, in pages: compiled code, data kept (cleared),
 * data given back. */
static uint64_t madv_wx, madv_kept_rw, madv_kept_none, madv_released;

static void count_committed(int64_t pages, uint64_t last_page)
{
	committed_pages += pages;
	if (committed_pages > committed_peak)
		committed_peak = committed_pages;
	if (last_page > top_page)
		top_page = last_page;
}

/* Where the time of mapping files goes: reading them, and patching their
 * code. */
static uint64_t file_read_ns, file_read_bytes, patch_ns;

/* Where the committed memory is, when there is a lot of it: the largest runs
 * of committed pages and the 4 GB pieces of the arena they are in (each of
 * Chromium's allocators has its own: V8's cage, PartitionAlloc's pools). Said
 * again each time it grew by 128 MB. */
static uint64_t madv_calls[3];   /* MADV_DONTNEED of < 64 KB, < 1 MB, more */
static uint64_t advice_calls[32], advice_pages[32];   /* every advice, by number */
static uint64_t commit_mmap, commit_mprotect;   /* pages, all along */

static void report_regions(int now)
{
	static uint64_t said_at;
	uint64_t top[8][2] = { { 0 } }, zones[64] = { 0 };
	uint64_t pages = arena_size / GX_PAGE;

	if (!now && (committed_pages < (1024UL << 8) || committed_pages < said_at + (128UL << 8)))
		return;
	said_at = committed_pages;
	for (uint64_t p = 0; p < pages;) {
		uint64_t q = p;

		if (!(page_state[p] & PG_COMMITTED)) {
			p++;
			continue;
		}
		while (q < pages && (page_state[q] & PG_COMMITTED))
			q++;
		zones[((p * GX_PAGE) >> 32) & 63] += q - p;
		for (int i = 0; i < 8; i++) {
			if (q - p > top[i][1]) {
				memmove(top[i + 1], top[i], (7 - i) * sizeof(top[0]));
				top[i][0] = p;
				top[i][1] = q - p;
				break;
			}
		}
		p = q;
	}
	host_log("gx: committed by mmap %lu MB, by mprotect %lu MB; MADV_DONTNEED calls "
		 "< 64 KB %lu, < 1 MB %lu, larger %lu\n",
		 (unsigned long)(commit_mmap >> 8), (unsigned long)(commit_mprotect >> 8),
		 (unsigned long)madv_calls[0], (unsigned long)madv_calls[1],
		 (unsigned long)madv_calls[2]);
	for (int i = 0; i < 64; i++)
		if (zones[i] >= (16UL << 8))
			host_log("gx:   arena +%d GB: %lu MB committed\n", i * 4,
				 (unsigned long)(zones[i] >> 8));
	for (int i = 0; i < 32; i++)
		if (advice_calls[i])
			host_log("gx:   madvise %d: %lu calls, %lu MB\n", i,
				 (unsigned long)advice_calls[i], (unsigned long)(advice_pages[i] >> 8));
	for (int i = 0; i < 8 && top[i][1]; i++)
		host_log("gx:   run at %#lx: %lu MB, state %#x\n",
			 (unsigned long)(arena + top[i][0] * GX_PAGE),
			 (unsigned long)(top[i][1] >> 8), page_state[top[i][0]]);
}

void gmem_report(void)
{
	if (file_read_bytes)
		host_log("gx: mapped files: %lu MB read in %lu ms, %lu ms patching code\n",
			 (unsigned long)(file_read_bytes >> 20), (unsigned long)(file_read_ns / 1000000),
			 (unsigned long)(patch_ns / 1000000));
	host_log("gx: guest memory: %lu MB committed at most, %lu MB at exit, %lu MB of addresses, "
		 "%d pieces of shared memory at most\n",
		 (unsigned long)(committed_peak >> 8), (unsigned long)(committed_pages >> 8),
		 (unsigned long)(top_page >> 8), chunk_peak);
	if (madv_wx || madv_kept_rw || madv_kept_none || madv_released)
		host_log("gx: memory let go: %lu MB of code given back, %lu MB of data given back, "
			 "%lu MB kept (%lu MB of it accessible)\n",
			 (unsigned long)(madv_wx >> 8), (unsigned long)(madv_released >> 8),
			 (unsigned long)((madv_kept_rw + madv_kept_none) >> 8),
			 (unsigned long)(madv_kept_rw >> 8));
	if (wx_to_write || wx_to_exec)
		host_log("gx: compiled code: made writable %lu times, executable %lu times, "
			 "in %lu ms (%lu ms at most, %lu ms of it waiting)\n",
			 (unsigned long)wx_to_write, (unsigned long)wx_to_exec,
			 (unsigned long)(wx_ns / 1000000), (unsigned long)(wx_ns_max / 1000000),
			 (unsigned long)(wx_wait_max / 1000000));
	report_regions(0);
}

static struct image *image_of(uint64_t addr);
static uint64_t swept_pages, sweeps;

/*
 * When the host has no memory left: gives back the data units that are all
 * inaccessible to the guest and all zeroes. Allocators make what they let go
 * inaccessible and discard it (gmem_madvise() clears it), but in small pieces,
 * so that the units of it are left committed; a browser had half a gigabyte
 * of them when it ran out. Such a unit can go without the guest knowing: it
 * can only touch it after making it accessible, which commits it anew, and it
 * would find zeroes there either way. Called with the lock held, which
 * mprotect takes too.
 */
static uint64_t sweep(void)
{
	const uint64_t unit = DATA_UNIT / GX_PAGE;
	uint64_t pages = top_page < arena_size / GX_PAGE ? top_page : arena_size / GX_PAGE;
	uint64_t freed = 0;

	for (uint64_t p = 0; p + unit <= pages; p += unit) {
		const uint64_t *word;
		uint64_t i, a = (uint64_t)arena + p * GX_PAGE;

		for (i = 0; i < unit && page_state[p + i] == (PG_USED | PG_COMMITTED); i++)
			;
		if (i < unit || image_of(a) || image_of(a + DATA_UNIT - 1))
			continue;
		word = (const uint64_t *)a;
		for (i = 0; i < DATA_UNIT / 8 && !word[i]; i++)
			;
		if (i < DATA_UNIT / 8 || host_decommit((void *)a, DATA_UNIT))
			continue;
		memset(&page_state[p], PG_USED, unit);
		count_committed(-(int64_t)unit, 0);
		freed += unit;
	}
	swept_pages += freed;
	sweeps++;
	host_log("gx: no memory left: %lu MB given back that the guest had let go\n",
		 (unsigned long)(freed >> 8));
	return freed;
}

/* host_commit(), and when it fails, once more after a sweep. */
static int commit_host(void *addr, uint64_t len, int prot)
{
	static uint64_t fruitless_at;

	if (!host_commit(addr, len, prot))
		return 0;
	/* Not again right after a sweep that found nothing: every allocation
	 * then fails, and each would go through the whole arena. */
	if (fruitless_at && host_now_ns() - fruitless_at < 1000000000ULL)
		return -1;
	if (!sweep()) {
		static int said;

		if (!said++)
			report_regions(1);
		fruitless_at = host_now_ns();
		return -1;
	}
	fruitless_at = 0;
	return host_commit(addr, len, prot);
}

static void release(uint64_t addr, uint64_t len)
{
	uint64_t first = page_of(addr), n = len / GX_PAGE;
	uint64_t committed = 0;

	for (uint64_t p = first; p < first + n; p++)
		committed += !!(page_state[p] & PG_COMMITTED);
	count_committed(-(int64_t)committed, 0);
	if (committed)
		host_decommit((void *)addr, len);
	memset(&page_state[first], 0, n);
	if (first < search_hint)
		search_hint = first;
}

static int commit(uint64_t addr, uint64_t len, int prot)
{
	uint8_t state = PG_USED | (prot & PG_PROT);

	if (prot & PG_PROT) {
		if (commit_host((void *)addr, len, prot))
			return -1;
		state |= PG_COMMITTED;
		count_committed(len / GX_PAGE, page_of(addr) + len / GX_PAGE);
	}
	memset(&page_state[page_of(addr)], state, len / GX_PAGE);
	return 0;
}

/*
 * Memory the guest wants writable and executable at once: what a compiler
 * that runs inside a program (V8's) writes its code to, one large piece asked
 * for once. A host may not have such memory (Horizon does not), so a page of
 * it is one or the other at any time, whichever was needed last: nothing is
 * there until the page is touched, and the fault of a write to a page that is
 * executable, or of a jump to one that is writable, turns it into the other
 * (gmem_fault(), which the host calls). Such a page is known by its
 * protection, all three; PG_COMMITTED says whether it is there.
 *
 * Whole units change together: the host keeps a record per run of pages
 * mapped alike, and compiled code comes in small pieces.
 */
#define WX_UNIT (256UL << 10)
#define WX_PROT (GX_PROT_READ | GX_PROT_WRITE | GX_PROT_EXEC)

static int is_wx(uint8_t state)
{
	return (state & (PG_USED | PG_SHARED | PG_COPY | PG_HOLE | PG_PROT)) == (PG_USED | WX_PROT);
}

/* What is known of the page at addr, for a host to say when a fault there is
 * not understood: the bits of page_state[], or -1 outside the arena. */
int gmem_page_state(uint64_t addr)
{
	return arena && in_arena(addr, 1) ? page_state[page_of(addr)] : -1;
}

int gmem_fault(uint64_t addr, int exec)
{
	int prot = GX_PROT_READ | (exec ? GX_PROT_EXEC : GX_PROT_WRITE);
	uint64_t first, end, at;
	int ret = 0;

	if (!arena || !in_arena(addr, 1))
		return 0;
	at = page_of(addr);
	uint64_t t0 = host_now_ns(), t1;

	host_lock_acquire(lock);
	t1 = host_now_ns();
	if (t1 - t0 > wx_wait_max && is_wx(page_state[at]))
		wx_wait_max = t1 - t0;
	if (is_wx(page_state[at])) {
		first = at & ~(WX_UNIT / GX_PAGE - 1);
		end = first + WX_UNIT / GX_PAGE;
		if (end > arena_size / GX_PAGE)
			end = arena_size / GX_PAGE;
		ret = 1;
		for (uint64_t p = first, q; p < end; p = q) {
			uint8_t state = page_state[p];
			void *run = arena + p * GX_PAGE;
			int failed;

			for (q = p; q < end && page_state[q] == state; q++)
				;
			if (!is_wx(state))
				continue;
			if (state & PG_COMMITTED) {
				failed = host_protect(run, (q - p) * GX_PAGE, prot);
			} else {
				failed = commit_host(run, (q - p) * GX_PAGE, prot);
				if (!failed) {
					memset(&page_state[p], state | PG_COMMITTED, q - p);
					count_committed(q - p, q);
				}
			}
			if (failed && at >= p && at < q)
				ret = 0;
			else if (!failed && exec)
				host_flush_icache(run, (q - p) * GX_PAGE);
		}
		if (exec)
			wx_to_exec++;
		else
			wx_to_write++;
		t1 = host_now_ns() - t0;
		wx_ns += t1;
		if (t1 > wx_ns_max)
			wx_ns_max = t1;
	}
	host_lock_release(lock);
	return ret;
}

/* Whether memory at addr, for a given protection, is data in the sense above:
 * not code, and not part of a file's image (where a page is what its segment
 * says). */
static int is_data(uint64_t addr, int prot)
{
	return !(prot & GX_PROT_EXEC) && !image_of(addr);
}

/* Commits [addr, addr+len), reserved and not committed yet, as data the guest
 * sees with prot. Whole units where a unit is all reservation, so that the
 * pieces a guest commits one after the other are one mapping for the host. */
static int commit_data(uint64_t addr, uint64_t len, int prot)
{
	uint64_t end = addr + len;

	if (len >= (32UL << 20))
		host_log("gx: %lu MB made accessible at %#lx\n", (unsigned long)(len >> 20),
			 (unsigned long)addr);

	for (uint64_t unit = addr & ~(DATA_UNIT - 1); unit < end; unit += DATA_UNIT) {
		uint64_t from = unit > addr ? unit : addr;
		uint64_t to = unit + DATA_UNIT < end ? unit + DATA_UNIT : end;
		uint64_t asked = from, asked_end = to;
		int whole = in_arena(unit, DATA_UNIT);

		for (uint64_t a = unit; whole && a < unit + DATA_UNIT; a += GX_PAGE)
			whole = (page_state[page_of(a)] &
				 (PG_USED | PG_COMMITTED | PG_SHARED | PG_COPY | PG_HOLE)) == PG_USED;
		if (whole && !image_of(unit) && !image_of(unit + DATA_UNIT - 1)) {
			if (commit_host((void *)unit, DATA_UNIT, DATA_PROT))
				return -1;
			/* All of it is there; the guest has what it asked for. */
			memset(&page_state[page_of(unit)], PG_USED | PG_COMMITTED, DATA_UNIT / GX_PAGE);
			/* Its protection now, not at the end: what is committed
			 * and inaccessible may be taken back by sweep() when a
			 * later unit finds no memory. */
			memset(&page_state[page_of(asked)], PG_USED | PG_COMMITTED | (prot & PG_PROT),
			       (asked_end - asked) / GX_PAGE);
			count_committed(DATA_UNIT / GX_PAGE, page_of(unit) + DATA_UNIT / GX_PAGE);
			commit_mprotect += DATA_UNIT / GX_PAGE;
			continue;
		}
		/* A unit with something else in it: what is asked and, of the
		 * unit, the plain reservation on either side of it, which is
		 * likely to be asked for next. */
		while (from > unit && in_arena(from - GX_PAGE, GX_PAGE) &&
		       page_state[page_of(from - GX_PAGE)] == PG_USED && !image_of(from - GX_PAGE))
			from -= GX_PAGE;
		while (to < unit + DATA_UNIT && in_arena(to, GX_PAGE) &&
		       page_state[page_of(to)] == PG_USED && !image_of(to))
			to += GX_PAGE;
		for (uint64_t a = from; a < to;) {
			uint64_t b = a;

			if (page_state[page_of(a)] & PG_COMMITTED) {
				a += GX_PAGE;
				continue;
			}
			while (b < to && !(page_state[page_of(b)] & PG_COMMITTED))
				b += GX_PAGE;
			if (commit_host((void *)a, b - a, DATA_PROT))
				return -1;
			memset(&page_state[page_of(a)], PG_USED | PG_COMMITTED, (b - a) / GX_PAGE);
			count_committed((b - a) / GX_PAGE, page_of(b));
			commit_mprotect += (b - a) / GX_PAGE;
			a = b;
		}
		memset(&page_state[page_of(asked)], PG_USED | PG_COMMITTED | (prot & PG_PROT),
		       (asked_end - asked) / GX_PAGE);
	}
	memset(&page_state[page_of(addr)], PG_USED | PG_COMMITTED | (prot & PG_PROT), len / GX_PAGE);
	return 0;
}

static struct image *image_of(uint64_t addr)
{
	for (int i = 0; i < MAX_IMAGES; i++)
		if (images[i].end && addr >= images[i].start && addr < images[i].end)
			return &images[i];
	return NULL;
}

static uint64_t pool_size_for(uint64_t len)
{
	uint64_t size = GX_PAGE_UP(len / 128);

	return size < 16 * GX_PAGE ? 16 * GX_PAGE : size;
}

/* Records [addr, addr+len) as an image whose veneers live at pool. */
static struct image *add_image(uint64_t addr, uint64_t len, uint64_t pool, uint64_t pool_size)
{
	for (int i = 0; i < MAX_IMAGES; i++) {
		if (images[i].end)
			continue;
		/* Never writable and executable at once, which Horizon forbids:
		 * patch_syscalls() opens the pool for writing while it fills it. */
		if (commit(pool, pool_size, GX_PROT_READ | GX_PROT_EXEC))
			return NULL;
		images[i].start = addr;
		images[i].end = addr + len;
		images[i].pool = images[i].pool_next = pool;
		images[i].pool_end = pool + pool_size;
		return &images[i];
	}
	return NULL;
}

/* Finds len bytes plus a veneer pool, and records them as an image. */
static uint64_t new_image(uint64_t len)
{
	uint64_t pool = pool_size_for(len);
	uint64_t addr = find_free((len + pool) / GX_PAGE);

	if (!addr || !add_image(addr, len, addr + len, pool))
		return 0;
	return addr;
}

/* Makes an image of a file mapped at an address the guest chose. ld.so does
 * that for every library aligned to more than a page: it reserves anonymous
 * memory, then maps the file inside it. The pool goes in the first hole after
 * the mapping. */
static struct image *adopt_image(uint64_t addr, uint64_t len)
{
	uint64_t pool = pool_size_for(len), run = 0;

	for (uint64_t p = page_of(addr + len); p < (arena_size / GX_PAGE); p++) {
		run = (page_state[p] & PG_USED) ? 0 : run + 1;
		if (run == pool / GX_PAGE)
			return add_image(addr, len, (uint64_t)arena + (p + 1) * GX_PAGE - pool, pool);
	}
	return NULL;
}

/* Replaces each svc #0 in [addr, addr+len) by a branch to a veneer. The pool
 * is not executable while veneers are added to it: that is safe as long as an
 * image is mapped entirely before any of it runs, which is what ld.so does. */
static int patch_syscalls(uint64_t addr, uint64_t len)
{
	struct image *img = image_of(addr);
	uint32_t *code = (uint32_t *)addr;
	int patched = 0, ret = 0;

	for (uint64_t k = 0; k < len / 4; k++) {
		uint64_t site = addr + k * 4;
		uint32_t *v;
		int64_t to_veneer, back;

		if (code[k] != SVC0)
			continue;
		if (!img)
			img = adopt_image(addr, len);
		if (!img || img->pool_next + VENEER_SIZE > img->pool_end) {
			host_log("gx: no veneer left for the svc at %#lx\n", (unsigned long)site);
			ret = -1;
			break;
		}
		if (!patched && host_protect((void *)img->pool, img->pool_end - img->pool,
					     GX_PROT_READ | GX_PROT_WRITE)) {
			host_log("gx: cannot open the veneer pool at %#lx\n",
				 (unsigned long)img->pool);
			return -1;
		}
		v = (uint32_t *)img->pool_next;
		to_veneer = (int64_t)(img->pool_next - site);
		back = (int64_t)(site + 4 - (img->pool_next + 5 * 4));
		if (to_veneer >= BRANCH_RANGE || back <= -BRANCH_RANGE) {
			host_log("gx: the svc at %#lx is too far from its veneer pool\n",
				 (unsigned long)site);
			ret = -1;
			break;
		}
		v[0] = 0xA9BF47F0u; /* stp x16, x17, [sp, #-16]! */
		v[1] = 0x10000070u; /* adr x16, exit */
		v[2] = 0x58000091u; /* ldr x17, trampoline */
		v[3] = 0xD61F0220u; /* br  x17 */
		v[4] = 0xA8C147F0u; /* exit: ldp x16, x17, [sp], #16 */
		v[5] = 0x14000000u | (((uint32_t)(back >> 2)) & 0x03FFFFFFu);
		*(uint64_t *)&v[6] = (uint64_t)gx_svc_trampoline;
		code[k] = 0x14000000u | (((uint32_t)(to_veneer >> 2)) & 0x03FFFFFFu);
		img->pool_next += VENEER_SIZE;
		patched++;
	}
	if (patched) {
		if (host_protect((void *)img->pool, img->pool_end - img->pool,
				 GX_PROT_READ | GX_PROT_EXEC)) {
			host_log("gx: cannot close the veneer pool at %#lx\n",
				 (unsigned long)img->pool);
			return -1;
		}
		host_flush_icache((void *)img->pool, img->pool_next - img->pool);
	}
	return ret ? ret : patched;
}

static long read_file(int fd, uint64_t off, uint8_t *dst, uint64_t len)
{
	uint64_t done = 0;

	while (done < len) {
		long n = lkl_sys_pread64(fd, (char *)dst + done, len - done, off + done);

		if (n < 0)
			return n;
		if (n == 0)
			break;
		done += n;
	}
	return 0;
}

/* Takes one mapping of a chunk out of the arena. */
static void unmap_chunk(struct chunk *c, uint64_t base)
{
	host_shared_unmap(c->handle, (void *)base, c->size);
	memset(&page_state[page_of(base)], 0, c->size / GX_PAGE);
	if (page_of(base) < search_hint)
		search_hint = page_of(base);
}

/* Where a mapping of a chunk goes. On a 2 MB boundary when the chunk is that
 * large: the host's kernel can then map it in 2 MB pieces instead of pages,
 * and its page tables are a resource that runs out (on Horizon, a few hundred
 * mappings of 16 MB made of pages take them all, and the next one fails). */
static uint64_t find_chunk_place(uint64_t size)
{
	return find_free_aligned(size / GX_PAGE, size >= (2UL << 20) ? 2UL << 20 : GX_PAGE);
}

/* Gets a chunk of the host's shared memory, mapped for ourselves. */
static struct chunk *new_chunk(uint64_t size, int pooled)
{
	struct chunk *c = NULL;

	for (int i = 0; i < MAX_CHUNKS && !c; i++)
		if (!chunks[i].size)
			c = &chunks[i];
	if (!c)
		return NULL;
	c->addr = find_chunk_place(size);
	c->handle = c->addr ? host_shared_new(size) : -1;
	if (c->handle < 0)
		return NULL;
	if (host_shared_map(c->handle, (void *)c->addr, size, GX_PROT_READ | GX_PROT_WRITE)) {
		host_shared_free(c->handle);
		return NULL;
	}
	memset(&page_state[page_of(c->addr)], PG_USED | PG_COMMITTED | GX_PROT_READ | GX_PROT_WRITE,
	       size / GX_PAGE);
	count_committed(size / GX_PAGE, page_of(c->addr) + size / GX_PAGE);
	if (++chunk_count > chunk_peak)
		chunk_peak = chunk_count;
	c->size = size;
	c->pooled = pooled;
	c->used = 0;
	if (pooled)
		memset(chunk_pages[c - chunks], 0, CHUNK_MAX_PAGES);
	return c;
}

/* Finds the memory of a shared file: a place in a chunk that small files
 * share, or a chunk of its own. Returns 0 if the host has none to give. */
static int alloc_shared(struct shared *s)
{
	uint64_t chunk_size = host_shared_chunk(), pages = s->size / GX_PAGE;
	struct chunk *c;

	if (chunk_size > CHUNK_MAX_PAGES * GX_PAGE)
		chunk_size = CHUNK_MAX_PAGES * GX_PAGE;
	if (!chunk_size || s->size > chunk_size / 4) {
		c = new_chunk(s->size, 0);
		if (!c)
			return 0;
		c->used = pages;
		s->chunk = c;
		s->off = 0;
		return 1;
	}
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < MAX_CHUNKS; i++) {
			uint8_t *used = chunk_pages[i];
			uint64_t run = 0;

			c = &chunks[i];
			if (!c->size || !c->pooled)
				continue;
			for (uint64_t p = 0; p < c->size / GX_PAGE; p++) {
				run = used[p] ? 0 : run + 1;
				if (run < pages)
					continue;
				memset(&used[p + 1 - pages], 1, pages);
				c->used += pages;
				s->chunk = c;
				s->off = (p + 1 - pages) * GX_PAGE;
				/* The last file that was here left its contents. */
				memset((void *)(c->addr + s->off), 0, s->size);
				return 1;
			}
		}
		if (pass || !new_chunk(chunk_size, 1))
			break;
	}
	return 0;
}

static void free_shared(struct shared *s)
{
	struct chunk *c = s->chunk;

	if (c->pooled)
		memset(&chunk_pages[c - chunks][s->off / GX_PAGE], 0, s->size / GX_PAGE);
	c->used -= s->size / GX_PAGE;
	if (c->used)
		return;
	unmap_chunk(c, c->addr);
	count_committed(-(int64_t)(c->size / GX_PAGE), 0);
	host_shared_free(c->handle);
	chunk_count--;
	memset(c, 0, sizeof(*c));
}

static int kept_count;

/* Whether a file is of the kernel's memory filesystem, where a file that has
 * no page is a file that was never written. */
int gmem_in_memory_fs(int fd)
{
	struct lkl_statfs fs;
	long params[6] = { fd, (long)&fs };

	return !lkl_syscall(__lkl__NR_fstatfs, params) && (long)fs.f_type == RAMFS_MAGIC;
}

/* Opens the file behind a guest descriptor for ourselves. */
static int open_again(int fd)
{
	char path[32] = "/proc/self/fd/";
	int at = strlen(path), ours;

	for (int digit = 100000; digit; digit /= 10)
		if (fd >= digit || digit == 1)
			path[at++] = '0' + fd / digit % 10;
	path[at] = 0;
	ours = lkl_sys_open(path, LKL_O_RDWR, 0);
	return ours >= 0 ? ours : lkl_sys_open(path, LKL_O_RDONLY, 0);
}

/* Whether nothing can map the file again: it has no name, and nobody but us
 * has it open, not even in a message on its way between two sockets. The
 * kernel grants a write lease on exactly that condition. */
static int is_dead(const struct shared *s)
{
	long params[6] = { s->fd, LKL_F_SETLEASE, LKL_F_WRLCK };
	struct lkl_stat st;

	return s->own && !lkl_sys_fstat(s->fd, &st) && !st.st_nlink &&
	       !lkl_syscall(__lkl__NR_fcntl, params);
}

/* Fills the copy from the file, without making the kernel read holes: a file
 * of memory that was only sized, as shared memory is before it is mapped, is
 * all zeroes but for the page its last byte made (see ftruncate in gsys.c). */
static void load_shared(const struct shared *s, int fd, const struct lkl_stat *st)
{
	if (gmem_in_memory_fs(fd)) {
		uint64_t last = GX_PAGE_DOWN(st->st_size - 1);
		struct { void *base; uint64_t len; } iov = { (void *)(s->addr + last), st->st_size - last };
		long params[6] = { fd, (long)&iov, 1, last, 0, LKL_RWF_NOWAIT };

		if (!st->st_blocks)
			return;
		/* That page alone, if it is in memory: nothing else is. */
		if ((uint64_t)st->st_blocks * 512 <= GX_PAGE && (uint64_t)st->st_size > GX_PAGE &&
		    lkl_syscall(__lkl__NR_preadv2, params) == (long)iov.len)
			return;
	}
	read_file(fd, 0, (uint8_t *)s->addr, st->st_size);
}

/* Gives up the copy of a shared file: its contents go back to the file. */
static void close_shared(struct shared *s)
{
	struct lkl_stat st;

	if (s->kept)
		kept_count--;

	if (s->fd >= 0) {
		if (s->written && !lkl_sys_fstat(s->fd, &st)) {
			uint64_t len = (uint64_t)st.st_size < s->size ? (uint64_t)st.st_size : s->size;

			for (uint64_t done = 0; done < len;) {
				long n = lkl_sys_pwrite64(s->fd, (char *)s->addr + done, len - done, done);

				if (n <= 0)
					break;
				done += n;
			}
		}
		lkl_sys_close(s->fd);
	}
	if (s->chunk)
		free_shared(s);
	else
		release(s->addr, s->size);
	memset(s, 0, sizeof(*s));
}

/* Forgets a shared file that is dead: nothing of it is worth writing. */
static void discard_shared(struct shared *s)
{
	lkl_sys_close(s->fd);
	kept_count--;
	free_shared(s);
	memset(s, 0, sizeof(*s));
}

/* What the kernel has free (not counting what it could drop of its caches). */
static uint64_t kernel_free(void)
{
	struct lkl_sysinfo info;
	long params[6] = { (long)&info };

	if (lkl_syscall(__lkl__NR_sysinfo, params))
		return ~0ULL;
	return (uint64_t)info.freeram * (info.mem_unit ? info.mem_unit : 1);
}

#define KERNEL_RESERVE (64UL << 20)

/* The files without a view are too many: the dead go, then the oldest in the
 * table until half are left. A small file of the memory filesystem that was
 * written stays a copy of ours, though, while the kernel is short: given
 * back, its contents would be the kernel's, every page of it (the memory
 * filesystem drops none), and a page that loads many at once (GitHub's) had
 * half of them given back together, which filled the kernel's 188 MB in three
 * seconds; it panicked. Our memory has room. Not so the host's shared memory
 * objects, which large files take one each: those go back all the same, and
 * so does everything once four times as many are kept. */
static void collect_shared(void)
{
	for (int i = 0; i < MAX_SHARED; i++)
		if (shared[i].kept && is_dead(&shared[i]))
			discard_shared(&shared[i]);
	for (int i = 0; i < MAX_SHARED && kept_count > KEEP_MAX / 2; i++) {
		struct shared *s = &shared[i];

		if (!s->kept)
			continue;
		if (s->written && s->chunk && s->chunk->pooled && kept_count < KEEP_MAX * 4 &&
		    kernel_free() < s->size + KERNEL_RESERVE && gmem_in_memory_fs(s->fd))
			continue;
		close_shared(s);
	}
}

/* The last view of a shared file is gone. The copy stays for a while: shared
 * memory is often unmapped by who filled it before who reads it maps it, and
 * writing it to the file and reading it back each time is most of the cost
 * of a picture going from one thread of Chromium to another. */
static void retire_shared(struct shared *s)
{
	if (!s->chunk || !s->own) {
		close_shared(s);
		return;
	}
	s->kept = 1;
	if (++kept_count > KEEP_MAX)
		collect_shared();
}

uint8_t *gmem_shared_peek(int fd, uint64_t *size)
{
	struct lkl_stat st;
	uint8_t *found = NULL;

	if (lkl_sys_fstat(fd, &st))
		return NULL;
	host_lock_acquire(lock);
	for (int i = 0; i < MAX_SHARED && !found; i++) {
		if ((shared[i].views || shared[i].kept) && shared[i].dev == st.st_dev &&
		    shared[i].ino == st.st_ino) {
			found = (uint8_t *)shared[i].addr;
			*size = shared[i].size;
		}
	}
	host_lock_release(lock);
	return found;
}

/* How much of the arena a view takes: all of its file's chunk. */
static uint64_t view_span(const struct view *v)
{
	return v->object->chunk ? v->object->chunk->size : v->object->size;
}

/* Unmaps [addr, addr+size), which may hold views of shared files. */
static void unmap_range(uint64_t addr, uint64_t size)
{
	uint64_t end = addr + size;

	while (addr < end) {
		uint8_t kind = page_state[page_of(addr)] & (PG_SHARED | PG_COPY | PG_HOLE);
		uint64_t run = GX_PAGE;
		struct view *v = NULL;

		while (addr + run < end &&
		       (page_state[page_of(addr + run)] & (PG_SHARED | PG_COPY | PG_HOLE)) == kind)
			run += GX_PAGE;
		if (kind & PG_HOLE) {
			addr += run;   /* never the guest's */
			continue;
		}
		if (!kind) {
			release(addr, run);
			addr += run;
			continue;
		}
		for (int i = 0; i < MAX_VIEWS && !v; i++)
			if (views[i].pages && addr >= views[i].base &&
			    addr < views[i].base + view_span(&views[i]))
				v = &views[i];
		if (!v) {
			addr += GX_PAGE;   /* not the guest's to unmap */
			continue;
		}
		if (run > v->base + view_span(v) - addr)
			run = v->base + view_span(v) - addr;
		/* The mapping goes when the guest has unmapped as much as it
		 * was given, in however many pieces. */
		v->pages -= run / GX_PAGE < v->pages ? run / GX_PAGE : v->pages;
		if (!v->pages) {
			struct shared *s = v->object;

			if (kind == PG_SHARED)
				unmap_chunk(s->chunk, v->base);
			if (!--s->views)
				retire_shared(s);
		}
		addr += run;
	}
}

/* A shared mapping of a regular file. Returns 0 if this one cannot be shared
 * (not a regular file, a file that outgrew its copy...) and is to be mapped
 * as a private copy instead. */
static long map_shared(uint64_t len, int prot, int fd, uint64_t off)
{
	uint64_t size = GX_PAGE_UP(len), base = 0;
	struct shared *s = NULL, *slot = NULL;
	struct view *v = NULL;
	struct lkl_stat st;

	if (lkl_sys_fstat(fd, &st) || (st.st_mode & LKL_S_IFMT) != LKL_S_IFREG)
		return 0;
	/* A view is a copy of the file, and what write() puts in the file later
	 * does not show in it. Whoever maps a file it can write, in a view it
	 * cannot write through, means to see those writes (SQLite does, with
	 * its databases): better no view than one that lies, and such programs
	 * read the file instead when there is none. A file without a name is
	 * shared memory, which nobody writes to with write(). */
	if (!(prot & GX_PROT_WRITE) && st.st_nlink > 0 &&
	    (lkl_sys_fcntl(fd, LKL_F_GETFL, 0) & LKL_O_ACCMODE) == LKL_O_RDWR)
		return -19;   /* ENODEV */
	for (int i = 0; i < MAX_VIEWS && !v; i++)
		if (!views[i].pages)
			v = &views[i];
	for (int i = 0; i < MAX_SHARED; i++) {
		if (!shared[i].views && !shared[i].kept) {
			if (!slot)
				slot = &shared[i];
		} else if (shared[i].dev == st.st_dev && shared[i].ino == st.st_ino) {
			s = &shared[i];
		}
	}
	if (s && s->kept && off + size > s->size) {
		/* It grew since: start again from the file. */
		close_shared(s);
		slot = s;
		s = NULL;
	}
	if (!v || (!s && !slot))
		return 0;

	if (!s) {
		s = slot;
		memset(s, 0, sizeof(*s));
		s->size = GX_PAGE_UP((uint64_t)st.st_size > off + len ? (uint64_t)st.st_size : off + len);
		if (alloc_shared(s)) {
			s->addr = s->chunk->addr + s->off;
		} else {
			/* No such memory on this host, or no more of it: one
			 * plain copy, which every view will be. Programs that
			 * tell their mappings apart by address (Chromium) do
			 * not survive it for long. */
			static int warned;

			if (!warned++)
				host_log("gx: no shared memory for a file of %lu KB: its mappings "
					 "will all be at one address\n", (unsigned long)(s->size >> 10));
			s->chunk = NULL;
			s->addr = find_free(s->size / GX_PAGE);
			if (!s->addr || commit(s->addr, s->size, GX_PROT_READ | GX_PROT_WRITE))
				return -GX_ENOMEM;
			memset(&page_state[page_of(s->addr)],
			       PG_USED | PG_COMMITTED | PG_COPY | GX_PROT_READ | GX_PROT_WRITE,
			       s->size / GX_PAGE);
		}
		load_shared(s, fd, &st);
		s->dev = st.st_dev;
		s->ino = st.st_ino;
		s->fd = open_again(fd);
		s->own = s->fd >= 0;
		if (!s->own)
			s->fd = lkl_sys_dup(fd);
		s->views = 1;   /* held until this view exists or fails */
	} else {
		if (s->kept) {
			s->kept = 0;
			kept_count--;
		}
		s->views++;
	}

	if (off + size > s->size) {
		host_log("gx: a shared file grew under its mappings; mapping a copy\n");
	} else if (!s->chunk) {
		base = s->addr;
		v->addr = base + off;
	} else {
		base = find_chunk_place(s->chunk->size);
		if (base && host_shared_map(s->chunk->handle, (void *)base, s->chunk->size, prot))
			base = 0;
		if (base)
			memset(&page_state[page_of(base)],
			       PG_USED | PG_COMMITTED | PG_SHARED | (prot & PG_PROT),
			       s->chunk->size / GX_PAGE);
		v->addr = base + s->off + off;
	}
	if (!base) {
		if (!--s->views)
			close_shared(s);
		return 0;
	}
	v->base = base;
	v->pages = size / GX_PAGE;
	v->object = s;
	s->written |= prot & GX_PROT_WRITE;
	return (long)v->addr;
}

static long do_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
	uint64_t size = GX_PAGE_UP(len);
	int anonymous = (flags & GX_MAP_ANONYMOUS) || fd < 0;
	long ret;

	if (!len)
		return -GX_EINVAL;

	if (!anonymous && (flags & GX_MAP_SHARED) &&
	    !(flags & (GX_MAP_FIXED | GX_MAP_FIXED_NOREPLACE)) && !(prot & GX_PROT_EXEC) &&
	    !(off & (GX_PAGE - 1))) {
		ret = map_shared(len, prot, fd, off);
		if (ret)
			return ret;
	}

	if (flags & (GX_MAP_FIXED | GX_MAP_FIXED_NOREPLACE)) {
		if ((addr & (GX_PAGE - 1)) || !in_arena(addr, size))
			return -GX_ENOMEM;
		for (uint64_t p = page_of(addr); p < page_of(addr) + size / GX_PAGE; p++)
			if (page_state[p] & PG_HOLE)
				return -GX_ENOMEM;
		if (flags & GX_MAP_FIXED_NOREPLACE) {
			for (uint64_t p = page_of(addr); p < page_of(addr) + size / GX_PAGE; p++)
				if (page_state[p] & PG_USED)
					return -17; /* EEXIST */
		}
		unmap_range(addr, size);
	} else {
		addr = anonymous ? find_free_anonymous(size) : new_image(size);
		if (!addr)
			return -GX_ENOMEM;
	}

	if (anonymous && (prot & PG_PROT) == WX_PROT) {
		/* Nothing of it is there until it is touched: see gmem_fault(). */
		if (commit(addr, size, 0))
			return -GX_ENOMEM;
		memset(&page_state[page_of(addr)], PG_USED | WX_PROT, size / GX_PAGE);
		return (long)addr;
	}
	if (anonymous) {
		/* Data is the host's to read and write whatever the guest says. */
		if ((prot & PG_PROT) && is_data(addr, prot)) {
			if (commit_host((void *)addr, size, DATA_PROT))
				return -GX_ENOMEM;
			count_committed(size / GX_PAGE, page_of(addr) + size / GX_PAGE);
			commit_mmap += size / GX_PAGE;
			memset(&page_state[page_of(addr)], PG_USED | PG_COMMITTED | (prot & PG_PROT),
			       size / GX_PAGE);
			return (long)addr;
		}
		return commit(addr, size, prot) ? -GX_ENOMEM : (long)addr;
	}

	if (commit(addr, size, GX_PROT_READ | GX_PROT_WRITE))
		return -GX_ENOMEM;
	{
		uint64_t t0 = host_now_ns();
		/* Read in large pieces, then let go of the kernel's copy: what
		 * is mapped is a copy of ours, and the kernel's memory is small
		 * (a browser's 300 MB of code filled it and it had to fight for
		 * every page it needed after). */
		long advice[6] = { fd, (long)off, (long)len, 2 /* POSIX_FADV_SEQUENTIAL */ };

		/* Without the lock while the card is read: the range is ours
		 * already, committed, and its address not given yet; with the
		 * lock, every other thread's mmap, mprotect and turn of a page
		 * of compiled code waited for the read (88 ms seen, seconds
		 * for the browser's own code at start). */
		host_lock_release(lock);
		if (len >= (1UL << 20))
			lkl_syscall(__lkl__NR_fadvise64, advice);
		ret = read_file(fd, off, (uint8_t *)addr, len);
		if (len >= (1UL << 20)) {
			advice[3] = 4;   /* POSIX_FADV_DONTNEED */
			lkl_syscall(__lkl__NR_fadvise64, advice);
		}
		host_lock_acquire(lock);
		file_read_ns += host_now_ns() - t0;
		file_read_bytes += len;
		if (len >= (8UL << 20))
			host_log("gx: mmap of %lu MB of file %d at %#lx (prot %d, flags %#x): %lu ms\n",
				 (unsigned long)(len >> 20), fd, (unsigned long)off, prot, flags,
				 (unsigned long)((host_now_ns() - t0) / 1000000));
	}
	if (ret < 0) {
		release(addr, size);
		return ret;
	}
	if (prot & GX_PROT_EXEC) {
		uint64_t t0 = host_now_ns();
		char link[32], path[160];
		long n;

		/* Which file the code is from: what a crash or a profile gives
		 * is an address, and this is how to tell which library it is in. */
		snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
		{
			long args[6] = { LKL_AT_FDCWD, (long)link, (long)path, sizeof(path) - 1 };

			n = lkl_syscall(__lkl__NR_readlinkat, args);
		}
		path[n > 0 ? n : 0] = 0;
		host_log("gx: code of %s at %#lx-%#lx (file offset %#lx)\n", n > 0 ? path : "?",
			 (unsigned long)addr, (unsigned long)(addr + size), (unsigned long)off);

		if (patch_syscalls(addr, size) < 0) {
			release(addr, size);
			return -GX_ENOMEM;
		}
		patch_ns += host_now_ns() - t0;
		host_flush_icache((void *)addr, size);
	}
	if (prot != (GX_PROT_READ | GX_PROT_WRITE)) {
		if (prot & PG_PROT) {
			if (host_protect((void *)addr, size, prot)) {
				release(addr, size);
				return -GX_ENOMEM;
			}
			memset(&page_state[page_of(addr)],
			       PG_USED | PG_COMMITTED | (prot & PG_PROT), size / GX_PAGE);
		} else {
			release(addr, size);
			commit(addr, size, 0);
		}
	}
	return (long)addr;
}

long gmem_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
	long ret;

	host_lock_acquire(lock);
	ret = do_mmap(addr, len, prot, flags, fd, off);
	host_lock_release(lock);
	return ret;
}

long gmem_reserve_image(uint64_t span)
{
	uint64_t addr;

	host_lock_acquire(lock);
	addr = new_image(GX_PAGE_UP(span));
	if (addr)
		commit(addr, GX_PAGE_UP(span), 0);
	host_lock_release(lock);
	return addr ? (long)addr : -GX_ENOMEM;
}

long gmem_munmap(uint64_t addr, uint64_t len)
{
	uint64_t size = GX_PAGE_UP(len);

	if ((addr & (GX_PAGE - 1)) || !in_arena(addr, size))
		return -GX_EINVAL;
	host_lock_acquire(lock);
	unmap_range(addr, size);
	for (int i = 0; i < MAX_IMAGES; i++) {
		struct image *img = &images[i];

		if (!img->end || addr > img->start || addr + size < img->end)
			continue;
		release(img->pool, img->pool_end - img->pool);
		memset(img, 0, sizeof(*img));
	}
	host_lock_release(lock);
	return 0;
}

long gmem_mprotect(uint64_t addr, uint64_t len, int prot)
{
	uint64_t size = GX_PAGE_UP(len);
	uint64_t first, n;
	long ret = 0;

	if ((addr & (GX_PAGE - 1)) || !in_arena(addr, size))
		return -GX_ENOMEM;
	host_lock_acquire(lock);
	first = page_of(addr);
	n = size / GX_PAGE;
	/* Pages are handled in runs that share a state, so that a run mapped
	 * PROT_NONE is committed and the others only change protection. */
	for (uint64_t p = first; p < first + n && !ret;) {
		uint64_t q = p;
		uint64_t run_addr = (uint64_t)arena + p * GX_PAGE;

		while (q < first + n && page_state[q] == page_state[p])
			q++;
		if (!(page_state[p] & PG_USED) || (page_state[p] & PG_HOLE)) {
			ret = -GX_ENOMEM;
		} else if (page_state[p] & PG_COPY) {
			/* One protection cannot suit every view of the copy. */
		} else if ((prot & PG_PROT) == WX_PROT && !(page_state[p] & PG_SHARED)) {
			/* The host's protection follows when the page is touched:
			 * see gmem_fault(). */
			memset(&page_state[p], PG_USED | WX_PROT | (page_state[p] & PG_COMMITTED), q - p);
		} else if (!(page_state[p] & PG_COMMITTED)) {
			if (!(prot & PG_PROT))
				;   /* reserved it was, reserved it stays */
			else if (is_data(run_addr, prot) ? commit_data(run_addr, (q - p) * GX_PAGE, prot)
							 : commit(run_addr, (q - p) * GX_PAGE, prot))
				ret = -GX_ENOMEM;
		} else if (!(page_state[p] & (PG_SHARED | GX_PROT_EXEC)) && is_data(run_addr, prot)) {
			/* Data: the protection is the guest's idea of it, no more. */
			memset(&page_state[p], PG_USED | PG_COMMITTED | (prot & PG_PROT), q - p);
		} else {
			/* Dropping all access keeps the contents: the guest may restore it. */
			if (host_protect((void *)run_addr, (q - p) * GX_PAGE, prot)) {
				/* A view of a shared file may be stuck with the
				 * protection it was mapped with. */
				if (!(page_state[p] & PG_SHARED))
					ret = -13; /* EACCES: the host refuses this protection */
			} else
				memset(&page_state[p], PG_USED | PG_COMMITTED | (prot & PG_PROT) |
				       (page_state[p] & PG_SHARED), q - p);
		}
		p = q;
	}
	host_lock_release(lock);
	return ret;
}

/*
 * Data the guest made inaccessible and let go (how PartitionAlloc decommits),
 * when memory is short: the units of it that are whole go back to the host and
 * become a reservation again, and the rest is cleared and kept (giving back
 * less than a unit would cut the host's mappings in pieces, see DATA_UNIT).
 * The guest only uses it again by making it accessible, which commits it anew
 * (commit_data()). What is still accessible is only cleared: the guest may
 * touch it at any time, and nothing would commit it then.
 */
static void release_data(uint64_t p, uint64_t q)
{
	const uint64_t unit = DATA_UNIT / GX_PAGE;

	while (p < q) {
		uint64_t u = (p + unit) & ~(unit - 1);
		uint64_t a = (uint64_t)arena + p * GX_PAGE;

		if (u > q)
			u = q;
		if (!(p & (unit - 1)) && u - p == unit && !image_of(a) &&
		    !image_of(a + DATA_UNIT - 1)) {
			host_decommit((void *)a, DATA_UNIT);
			memset(&page_state[p], PG_USED, unit);
			count_committed(-(int64_t)unit, 0);
			madv_released += unit;
		} else {
			memset((void *)a, 0, (u - p) * GX_PAGE);
			madv_kept_none += u - p;
		}
		p = u;
	}
}

long gmem_madvise(uint64_t addr, uint64_t len, int advice)
{
	uint64_t size = GX_PAGE_UP(len);
	int low;

	if ((addr & (GX_PAGE - 1)) || !in_arena(addr, size))
		return -GX_EINVAL;
	advice_calls[advice & 31]++;
	advice_pages[advice & 31] += size / GX_PAGE;
	if (advice != MADV_DONTNEED)
		return 0;
	madv_calls[size < (64UL << 10) ? 0 : size < (1UL << 20) ? 1 : 2]++;
	/* Private anonymous memory reads as zeroes afterwards, and allocators
	 * count on it. */
	host_lock_acquire(lock);
	low = host_memory_low();
	for (uint64_t p = page_of(addr), end = p + size / GX_PAGE; p < end;) {
		uint8_t state = page_state[p];
		uint64_t q = p;

		while (q < end && page_state[q] == state)
			q++;
		if (!(state & PG_COMMITTED) || (state & (PG_SHARED | PG_COPY))) {
			/* nothing there, or not private */
		} else if (is_wx(state)) {
			/* Not to be written to from here: it may be executable. */
			host_decommit(arena + p * GX_PAGE, (q - p) * GX_PAGE);
			memset(&page_state[p], PG_USED | WX_PROT, q - p);
			count_committed(-(int64_t)(q - p), 0);
			madv_wx += q - p;
		} else if (low && !(state & PG_PROT) && is_data((uint64_t)arena + p * GX_PAGE, 0)) {
			release_data(p, q);
		} else if (state & GX_PROT_WRITE) {
			/* Cleared and kept. */
			memset(arena + p * GX_PAGE, 0, (q - p) * GX_PAGE);
			madv_kept_rw += q - p;
		} else if (!(state & PG_PROT) && is_data((uint64_t)arena + p * GX_PAGE, 0)) {
			memset(arena + p * GX_PAGE, 0, (q - p) * GX_PAGE);
			madv_kept_none += q - p;
		} else if (!(state & PG_PROT)) {
			host_decommit(arena + p * GX_PAGE, (q - p) * GX_PAGE);
			memset(&page_state[p], PG_USED, q - p);
			count_committed(-(int64_t)(q - p), 0);
		}
		p = q;
	}
	host_lock_release(lock);
	return 0;
}

int gmem_accessible(uint64_t addr, uint64_t len, int write)
{
	uint8_t need = PG_COMMITTED | (write ? GX_PROT_WRITE : GX_PROT_READ);

	if (!len || !in_arena(addr, 1))
		return 1;   /* not ours to judge: the host's own memory */
	if (!in_arena(addr, len))
		return 0;
	for (uint64_t p = page_of(addr); p <= page_of(addr + len - 1); p++)
		if ((page_state[p] & need) != need && !is_wx(page_state[p]))
			return 0;
	return 1;
}

void gmem_set_brk(uint64_t start)
{
	(void)start;
	host_lock_acquire(lock);
	/* The break lives in its own reservation; nothing needs it to follow
	 * the program image. */
	brk_start = find_free(BRK_SIZE / GX_PAGE);
	if (brk_start)
		memset(&page_state[page_of(brk_start)], PG_USED, BRK_SIZE / GX_PAGE);
	brk_cur = brk_start;
	host_lock_release(lock);
}

long gmem_brk(uint64_t addr)
{
	host_lock_acquire(lock);
	if (brk_start && addr >= brk_start && addr <= brk_start + BRK_SIZE) {
		uint64_t old_top = GX_PAGE_UP(brk_cur), new_top = GX_PAGE_UP(addr);

		if (new_top > old_top) {
			if (commit(old_top, new_top - old_top, GX_PROT_READ | GX_PROT_WRITE) == 0)
				brk_cur = addr;
		} else {
			if (new_top < old_top) {
				host_decommit((void *)new_top, old_top - new_top);
				memset(&page_state[page_of(new_top)], PG_USED,
				       (old_top - new_top) / GX_PAGE);
			}
			brk_cur = addr;
		}
	}
	addr = brk_cur;
	host_lock_release(lock);
	return (long)addr;
}

/* For crash reports: the image addr belongs to, if any. */
int gmem_describe(uint64_t addr, uint64_t *base)
{
	struct image *img;

	if (!in_arena(addr, 1))
		return 0;
	img = image_of(addr);
	*base = img ? img->start : (uint64_t)arena;
	return img ? 2 : 1;
}
