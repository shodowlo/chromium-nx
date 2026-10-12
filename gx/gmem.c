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
#include <stdlib.h>
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
#define BRK_SIZE (1UL << 30)

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
int gmem_lazy;                          /* see lazy_fault() */
int gmem_lazy_pages = DATA_UNIT / GX_PAGE;
int gmem_lazy_wanted_only;              /* see lazy_fault() */
static struct host_lock *lazy_lock;

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
	lazy_lock = host_lock_new();
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
static uint64_t atomics_replaced;          /* see gmem_illegal() */
static uint64_t lazy_commits;              /* see lazy_fault() */
static uint64_t lazy_ahead, eager_commits; /* and what spares it its faults */
static uint64_t counters_replaced;         /* see gmem_counter() */
static uint64_t given_back;                /* pages, see gmem_madvise() */
/* What a program says it no longer needs is given back to the host from this
 * size up; less is cleared and kept, to be used again without a fault. */
#define DONTNEED_GIVEN_BACK (64UL << 10)
static uint64_t spurious_faults;           /* see the end of gmem_fault() */

static void count_committed(int64_t pages, uint64_t last_page)
{
	committed_pages += pages;
	if (committed_pages > committed_peak)
		committed_peak = committed_pages;
	if (last_page > top_page)
		top_page = last_page;
}

void gmem_report(void)
{
	host_log("gx: guest memory: %lu MB committed at most, %lu MB at exit, %lu MB of addresses, "
		 "%d pieces of shared memory at most\n",
		 (unsigned long)(committed_peak >> 8), (unsigned long)(committed_pages >> 8),
		 (unsigned long)(top_page >> 8), chunk_peak);
	{
		/* What the committed memory is: code, what can only be read
		 * (both from files, mostly), what can be written, and views
		 * of shared files (which are counted once per view). */
		uint64_t code = 0, read = 0, written = 0, views = 0;

		for (uint64_t p = 0; p < top_page && p < arena_size / GX_PAGE; p++) {
			uint8_t state = page_state[p];

			if (!(state & PG_COMMITTED))
				continue;
			if (state & (PG_SHARED | PG_COPY))
				views++;
			else if (state & GX_PROT_EXEC)
				code++;
			else if (state & GX_PROT_WRITE)
				written++;
			else
				read++;
		}
		host_log("gx: of which now: %lu MB of code, %lu MB read only, %lu MB written to, %lu MB of shared files\n",
			 (unsigned long)(code >> 8), (unsigned long)(read >> 8), (unsigned long)(written >> 8),
			 (unsigned long)(views >> 8));
	}
	{
		uint64_t pieces = 0;

		for (int i = 0; i < MAX_CHUNKS; i++)
			pieces += chunks[i].size;
		if (pieces)
			host_log("gx: shared memory in use: %lu MB\n", (unsigned long)(pieces >> 20));
	}
	if (spurious_faults)
		host_log("gx: faults on memory that was there, tried again: %lu\n", (unsigned long)spurious_faults);
	if (lazy_commits || eager_commits)
		host_log("gx: memory committed when first used: %lu times (%lu MB of it ahead of a run of them); "
			 "when asked for, being small: %lu times\n", (unsigned long)lazy_commits,
			 (unsigned long)(lazy_ahead >> 8), (unsigned long)eager_commits);
	if (given_back)
		host_log("gx: memory given back when no longer needed: %lu MB\n", (unsigned long)(given_back >> 8));
	if (counters_replaced)
		host_log("gx: reads of the virtual counter replaced: %lu\n", (unsigned long)counters_replaced);
	if (atomics_replaced)
		host_log("gx: atomic instructions the processor does not have, replaced: %lu\n",
			 (unsigned long)atomics_replaced);
	if (wx_to_write || wx_to_exec)
		host_log("gx: compiled code: made writable %lu times, executable %lu times\n",
			 (unsigned long)wx_to_write, (unsigned long)wx_to_exec);
}

static void release(uint64_t addr, uint64_t len)
{
	uint64_t first = page_of(addr), n = len / GX_PAGE;
	uint64_t committed = 0;

	/* Not while a page of it is being committed for its first use
	 * (lazy_fault()), or what is noted here and what the host has would
	 * part ways: a page left with the host though it is free here comes
	 * back, to whoever maps that place next, with what it held. For the
	 * same reason the host is told to let go of all of it where memory
	 * comes that way, whatever is noted. */
	host_lock_acquire(lazy_lock);
	for (uint64_t p = first; p < first + n; p++)
		committed += !!(page_state[p] & PG_COMMITTED);
	count_committed(-(int64_t)committed, 0);
	if (committed || gmem_lazy)
		host_decommit((void *)addr, len);
	memset(&page_state[first], 0, n);
	host_lock_release(lazy_lock);
	if (first < search_hint)
		search_hint = first;
}

static int commit(uint64_t addr, uint64_t len, int prot)
{
	uint8_t state = PG_USED | (prot & PG_PROT);

	if (prot & PG_PROT) {
		if (host_commit((void *)addr, len, prot))
			return -1;
		state |= PG_COMMITTED;
		count_committed(len / GX_PAGE, page_of(addr) + len / GX_PAGE);
	}
	memset(&page_state[page_of(addr)], state, len / GX_PAGE);
	return 0;
}

static struct image *image_of(uint64_t addr);

/* What was last given back of the guest's memory and by what, kept to be
 * said of a page that is found gone while a program still uses it. */
#define FREED_KEPT 8192
static struct freed {
	uint64_t lo, hi, who;
	uint32_t when;
	const char *how;
} freed[FREED_KEPT];
static uint32_t freed_count;

void gmem_note(const char *how, uint64_t lo, uint64_t hi, uint64_t who)
{
	uint32_t n = __atomic_fetch_add(&freed_count, 1, __ATOMIC_SEQ_CST);
	struct freed *f = &freed[n % FREED_KEPT];

	f->lo = lo;
	f->hi = hi;
	f->who = who;
	f->how = how;
	f->when = n;
}

void gmem_say_freed(uint64_t addr)
{
	int said = 0;

	for (uint32_t back = 1; back <= FREED_KEPT && back <= freed_count && said < 12; back++) {
		const struct freed *f = &freed[(freed_count - back) % FREED_KEPT];

		if (f->how && addr >= f->lo && addr < f->hi && ++said)
			host_log("gx:   %u events before: %s, %#lx-%#lx, by %ld\n", back, f->how,
				 (unsigned long)f->lo, (unsigned long)f->hi, (long)f->who);
	}
	if (!said)
		host_log("gx:   nothing kept of who gave it back (%u events in all)\n", freed_count);
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

/* ---------- memory that is there when it is first used ---------- */

/*
 * A program asks for much more memory than it uses: a megabyte or more of
 * stack for each thread, room to grow for every heap. A kernel with an MMU
 * gives each page when it is first touched; a host that commits for real
 * (Horizon) would give it all at once, and a large program does not fit.
 * With gmem_lazy, data the guest maps is noted as wanted and no more: the
 * first access faults, and the unit it is in (DATA_UNIT, the host being
 * short of pieces, not of pages) is committed then.
 *
 * The fault may be the kernel's, on its way to or from a buffer of the
 * guest's, with locks held that the lock of this file must not be taken
 * under: lazy_fault() has a lock of its own and calls nothing but the host.
 * Pages of this kind are: used, not committed, readable or writable (as the
 * guest was told), never executable.
 */

static int is_lazy(uint8_t state)
{
	return (state & (PG_USED | PG_COMMITTED | PG_SHARED | PG_COPY | PG_HOLE | GX_PROT_EXEC)) == PG_USED &&
	       (state & PG_PROT);
}

static int is_data(uint64_t addr, int prot);

/* Where the last pieces committed that way ended, and how large they were. */
#define LAZY_RUNS 16
/* (Half a megabyte. With a whole one, a Steam client that logs in had 470 MB
 * committed ahead in ten seconds; with none, its largest script, 14 MB read
 * in one go, came out wrong two times in three.) */
#define LAZY_RUN_MAX ((512UL << 10) / GX_PAGE)
static struct lazy_run {
	uint64_t end, pages;
} lazy_runs[LAZY_RUNS];
static unsigned lazy_run_next;

/* Memory that is wanted is not left for its first use when there is little
 * of it: a program that asks for a few pages (an allocator that takes what
 * it is about to hand out from what it had reserved, a small buffer) uses
 * them at once, and the fault of that first use is what is to be spared
 * (nx/exception.S). What is left to be committed on use is what is large:
 * the stacks of threads, the reach of heaps. */
uint64_t gmem_eager_mapped = 256UL << 10;      /* mapped readable or writable */
uint64_t gmem_eager_protected = 1UL << 20;     /* made so, having been reserved */
#define EAGER_MAPPED gmem_eager_mapped
#define EAGER_PROTECTED gmem_eager_protected

/* Stacks are the other thing left for their first use, whatever their size: a
 * thread is given megabytes and uses a few pages. The C library says which
 * mappings they are (MAP_STACK), and makes them usable afterwards. */
#define GX_MAP_STACK 0x20000
#define MAX_STACKS 512
static struct { uint64_t lo, hi; } stacks[MAX_STACKS];

static void note_stack(uint64_t lo, uint64_t hi)
{
	static unsigned next;
	int at = -1;

	for (int i = 0; i < MAX_STACKS && at < 0; i++)
		if (!stacks[i].hi || (stacks[i].lo < hi && lo < stacks[i].hi))
			at = i;
	if (at < 0)
		at = next++ % MAX_STACKS;
	stacks[at].lo = lo;
	stacks[at].hi = hi;
}

static int in_stack(uint64_t addr)
{
	for (int i = 0; i < MAX_STACKS; i++)
		if (addr >= stacks[i].lo && addr < stacks[i].hi)
			return 1;
	return 0;
}

static int lazy_fault(uint64_t addr)
{
	const uint64_t unit_pages = gmem_lazy_pages;
	uint64_t at = page_of(addr), first, end, from, to;
	uint8_t state = page_state[at];

	/* Data that is there already: another thread touched it at the same
	 * moment and was first, between this one's fault and now. (Data is
	 * the host's to read and write once it is there: no access to it
	 * faults for any other reason.) */
	if ((state & (PG_USED | PG_COMMITTED | PG_SHARED | PG_COPY | PG_HOLE | GX_PROT_EXEC)) ==
	    (PG_USED | PG_COMMITTED) && gmem_lazy && is_data((uint64_t)arena + at * GX_PAGE, 0))
		return 1;
	if (!is_lazy(state))
		return 0;
	host_lock_acquire(lazy_lock);
	if (!is_lazy(page_state[at])) {
		/* Another thread was first. */
		int there = page_state[at] & PG_COMMITTED;

		host_lock_release(lazy_lock);
		return there != 0;
	}
	first = at & ~(unit_pages - 1);
	end = first + unit_pages;
	if (end > arena_size / GX_PAGE)
		end = arena_size / GX_PAGE;
	/* With it, what else of the unit is wanted or likely to be: one piece
	 * for the host. Where the host is not short of pieces
	 * (gmem_lazy_wanted_only), only what is wanted: what is reserved
	 * around it may never be, and a unit can then be large for nothing. */
#define FILLABLE(p) ((page_state[p] & (PG_USED | PG_COMMITTED | PG_SHARED | PG_COPY | PG_HOLE | GX_PROT_EXEC)) == PG_USED && \
		     ((page_state[p] & PG_PROT) || \
		      (!gmem_lazy_wanted_only && !image_of((uint64_t)arena + (p) * GX_PAGE))))
	for (from = at; from > first && FILLABLE(from - 1); from--)
		;
	for (to = at + 1; to < end && FILLABLE(to); to++)
		;
#undef FILLABLE
	/* A fault where the last piece ended is a program filling what it
	 * mapped from one end to the other: each fault in such a run is one
	 * more chance for the host to lose what the program is copying (see
	 * nx/exception.S), so each piece is twice the one before it, up to
	 * LAZY_RUN_MAX and as far as the memory is wanted. */
	{
		struct lazy_run *run = NULL;
		uint64_t reach;

		for (int i = 0; i < LAZY_RUNS && !run; i++)
			if (lazy_runs[i].end == from && lazy_runs[i].pages)
				run = &lazy_runs[i];
		if (run) {
			reach = run->pages * 2 < LAZY_RUN_MAX ? run->pages * 2 : LAZY_RUN_MAX;
			if (from + reach > arena_size / GX_PAGE)
				reach = arena_size / GX_PAGE - from;
			if (to < from + reach) {
				uint64_t was = to;

				while (to < from + reach && is_lazy(page_state[to]))
					to++;
				lazy_ahead += to - was;
			}
		} else {
			run = &lazy_runs[lazy_run_next++ % LAZY_RUNS];
		}
		run->end = to;
		run->pages = to - from;
	}
	if (host_commit(arena + from * GX_PAGE, (to - from) * GX_PAGE, DATA_PROT)) {
		host_lock_release(lazy_lock);
		host_log("gx: no memory left for a page the guest was promised (%lu MB committed)\n",
			 (unsigned long)(committed_pages >> 8));
		return 0;
	}
	for (uint64_t p = from; p < to; p++)
		page_state[p] |= PG_COMMITTED;
	__atomic_add_fetch(&committed_pages, to - from, __ATOMIC_SEQ_CST);
	if (committed_pages > committed_peak)
		committed_peak = committed_pages;
	if (to > top_page)
		top_page = to;
	lazy_commits++;
	host_lock_release(lazy_lock);
	return 1;
}

/* ---------- what a forked child writes ---------- */

/*
 * The pages watched are made read-only for the host, and stay writable as far
 * as page_state[] and the guest know. The first write to one faults: if it is
 * the child's, the page is copied aside first; either way the page is made
 * writable again and the write goes on. Other threads write too, the guest's
 * and the kernel's: theirs just go through, a few pages at a time.
 *
 * A fault may come from anywhere, with any lock held: nothing here waits for
 * one. A page is claimed by setting its bit, and whoever sets it does the
 * work; a thread that faults on a page already claimed tries its write again
 * until that work is done.
 */
#define WATCH_KEPT 2048            /* pages of the child's that can be put back */
#define WATCH_GROUP 1              /* pages let go at once for other writers: each is one the child's
                                    * writes will not be noticed on */

struct watch_run {
	uint64_t first, pages;     /* pages of the arena */
	uint64_t bit;              /* where its bits start */
};

static struct {
	volatile int active;
	int busy;
	volatile uint64_t child;
	struct watch_run *runs;
	int run_count, run_room;
	uint64_t *bits;
	uint64_t *kept_page;
	uint8_t *kept;
	volatile int kept_count;
} watch;

static int watchable(uint8_t state)
{
	const uint8_t need = PG_COMMITTED | GX_PROT_READ | GX_PROT_WRITE;

	return (state & need) == need && !(state & (PG_SHARED | PG_COPY | PG_HOLE)) && !is_wx(state);
}

/* Claims a page: whether it was free to claim. */
static int watch_claim(const struct watch_run *r, uint64_t page)
{
	uint64_t bit = r->bit + (page - r->first), mask = 1UL << (bit & 63);

	return !(__atomic_fetch_or(&watch.bits[bit >> 6], mask, __ATOMIC_SEQ_CST) & mask);
}

static int watch_fault(uint64_t addr)
{
	uint64_t page = page_of(addr), lo, hi, from = 0;
	const struct watch_run *r = NULL;
	int low = 0, high = watch.run_count - 1, open = 0;

	while (low <= high) {
		int mid = (low + high) / 2;

		if (page < watch.runs[mid].first) {
			high = mid - 1;
		} else if (page >= watch.runs[mid].first + watch.runs[mid].pages) {
			low = mid + 1;
		} else {
			r = &watch.runs[mid];
			break;
		}
	}
	if (!r)
		return 0;
	if (host_thread_self() == watch.child) {
		if (watch_claim(r, page)) {
			int n = __atomic_fetch_add(&watch.kept_count, 1, __ATOMIC_SEQ_CST);

			if (n < WATCH_KEPT) {
				memcpy(watch.kept + (uint64_t)n * GX_PAGE, arena + page * GX_PAGE, GX_PAGE);
				watch.kept_page[n] = page;
			}
			host_protect(arena + page * GX_PAGE, GX_PAGE, GX_PROT_READ | GX_PROT_WRITE);
		}
		return 1;
	}
	lo = page & ~(uint64_t)(WATCH_GROUP - 1);
	hi = lo + WATCH_GROUP;
	if (lo < r->first)
		lo = r->first;
	if (hi > r->first + r->pages)
		hi = r->first + r->pages;
	for (uint64_t p = lo; p <= hi; p++) {
		int mine = p < hi && watch_claim(r, p);

		if (mine && !open) {
			from = p;
			open = 1;
		} else if (!mine && open) {
			host_protect(arena + from * GX_PAGE, (p - from) * GX_PAGE, GX_PROT_READ | GX_PROT_WRITE);
			open = 0;
		}
	}
	return 1;
}

static int watch_add(uint64_t first, uint64_t pages, uint64_t *bits)
{
	if (watch.run_count == watch.run_room) {
		int room = watch.run_room ? watch.run_room * 2 : 256;
		struct watch_run *runs = realloc(watch.runs, room * sizeof(*runs));

		if (!runs)
			return -1;
		watch.runs = runs;
		watch.run_room = room;
	}
	watch.runs[watch.run_count].first = first;
	watch.runs[watch.run_count].pages = pages;
	watch.runs[watch.run_count].bit = *bits;
	watch.run_count++;
	*bits += pages;
	return 0;
}

static int by_lo(const void *a, const void *b)
{
	const struct gx_range *x = a, *y = b;

	return x->lo < y->lo ? -1 : x->lo > y->lo;
}

int gmem_watch(const struct gx_range *ranges, int count, int with_break)
{
	struct gx_range *sorted;
	uint64_t bits = 0, done = 0;
	int failed = 0;

	if (!host_write_faults())
		return -1;
	sorted = malloc((count + 1) * sizeof(*sorted));
	if (!sorted)
		return -1;
	memcpy(sorted, ranges, count * sizeof(*sorted));
	host_lock_acquire(lock);
	if (watch.busy) {
		host_lock_release(lock);
		free(sorted);
		return -1;
	}
	if (with_break && brk_start) {
		sorted[count].lo = brk_start;
		sorted[count].hi = GX_PAGE_UP(brk_cur);
		count++;
	}
	qsort(sorted, count, sizeof(*sorted), by_lo);
	watch.run_count = 0;
	for (int i = 0; i < count && !failed; i++) {
		uint64_t lo = sorted[i].lo > done ? sorted[i].lo : done, hi = sorted[i].hi;

		if (lo >= hi || !in_arena(lo, hi - lo))
			continue;
		done = hi;
		for (uint64_t p = page_of(lo), end = page_of(hi - 1) + 1, q; p < end && !failed; p = q) {
			for (q = p; q < end && watchable(page_state[q]); q++)
				;
			if (q > p)
				failed = watch_add(p, q - p, &bits);
			else
				q++;
		}
	}
	free(sorted);
	watch.bits = failed ? NULL : calloc(bits / 64 + 1, sizeof(*watch.bits));
	watch.kept_page = failed ? NULL : malloc(WATCH_KEPT * sizeof(*watch.kept_page));
	watch.kept = failed ? NULL : malloc(WATCH_KEPT * GX_PAGE);
	if (!watch.bits || !watch.kept_page || !watch.kept) {
		free(watch.bits);
		free(watch.kept_page);
		free(watch.kept);
		host_lock_release(lock);
		return -1;
	}
	watch.kept_count = 0;
	watch.child = 0;
	watch.busy = 1;
	__atomic_store_n(&watch.active, 1, __ATOMIC_SEQ_CST);
	for (int i = 0; i < watch.run_count; i++)
		host_protect(arena + watch.runs[i].first * GX_PAGE, watch.runs[i].pages * GX_PAGE, GX_PROT_READ);
	host_lock_release(lock);
	return 0;
}

void gmem_watch_child(uint64_t host_thread)
{
	watch.child = host_thread;
}

void gmem_unwatch(void)
{
	int kept;

	host_lock_acquire(lock);
	/* Everything writable again that still is meant to be... */
	for (int i = 0; i < watch.run_count; i++) {
		for (uint64_t p = watch.runs[i].first, end = p + watch.runs[i].pages, q; p < end; p = q) {
			for (q = p; q < end && watchable(page_state[q]); q++)
				;
			if (q > p)
				host_protect(arena + p * GX_PAGE, (q - p) * GX_PAGE, GX_PROT_READ | GX_PROT_WRITE);
			else
				q++;
		}
	}
	__atomic_store_n(&watch.active, 0, __ATOMIC_SEQ_CST);
	/* ... and what the child wrote, as it was. */
	kept = watch.kept_count < WATCH_KEPT ? watch.kept_count : WATCH_KEPT;
	if (watch.kept_count > WATCH_KEPT)
		host_log("gx: a child wrote to %d pages of its parent's memory, of which %d were put back\n",
			 watch.kept_count, WATCH_KEPT);
	for (int i = 0; i < kept; i++)
		if (watchable(page_state[watch.kept_page[i]]))
			memcpy(arena + watch.kept_page[i] * GX_PAGE, watch.kept + (uint64_t)i * GX_PAGE, GX_PAGE);
	free(watch.bits);
	free(watch.kept_page);
	free(watch.kept);
	watch.busy = 0;
	host_lock_release(lock);
}

int gmem_fault(uint64_t addr, int exec)
{
	int prot = GX_PROT_READ | (exec ? GX_PROT_EXEC : GX_PROT_WRITE);
	uint64_t first, end, at;
	int ret = 0, waited = 0;

	if (!arena || !in_arena(addr, 1))
		return 0;
	if (!exec && __atomic_load_n(&watch.active, __ATOMIC_SEQ_CST) && watch_fault(addr))
		return 1;
	if (!exec && lazy_fault(addr))
		return 1;
	host_lock_acquire(lock);
	at = page_of(addr);
	if (exec && !is_wx(page_state[at]) &&
	    (page_state[at] & (PG_COMMITTED | GX_PROT_EXEC)) == (PG_COMMITTED | GX_PROT_EXEC)) {
		/* Code that is being written to (gmem_illegal()): it was
		 * done while we waited for the lock. */
		host_lock_release(lock);
		return 1;
	}
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
				failed = host_commit(run, (q - p) * GX_PAGE, prot);
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
	}
	if (!ret) {
		/* Memory that is there and allows what was refused, as far as
		 * we know: the refusal was of a moment (the guest changing a
		 * page's protection and back under a thread that uses it; a
		 * host that takes a mapping apart to change part of it). The
		 * thread tries again, though not for ever at the same place. */
		static uint64_t last_page;
		static unsigned again;
		uint8_t need = PG_COMMITTED | (exec ? GX_PROT_EXEC : GX_PROT_READ);

		/* Code that is mapped but not there yet, likewise: it is on
		 * its way, in the hands of another thread. */
		if (exec && (page_state[at] & (PG_USED | PG_HOLE | GX_PROT_EXEC)) == (PG_USED | GX_PROT_EXEC))
			need = 0;
		if ((page_state[at] & need) == need && !(page_state[at] & PG_HOLE)) {
			again = at == last_page ? again + 1 : 0;
			last_page = at;
			if (again < 4096) {
				ret = 1;
				spurious_faults++;
				waited = !need;
			}
		}
		if (!ret) {
			host_log("gx: a fault that is not ours to mend: %s %#lx, page state %#x, tried %u times\n",
				 exec ? "a jump to" : "an access to", (unsigned long)addr, page_state[at], again);
			gmem_say_freed(addr);
		}
	}
	host_lock_release(lock);
	if (ret && (!exec || waited))
		host_yield();
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

	for (uint64_t unit = addr & ~(DATA_UNIT - 1); unit < end; unit += DATA_UNIT) {
		uint64_t from = unit > addr ? unit : addr;
		uint64_t to = unit + DATA_UNIT < end ? unit + DATA_UNIT : end;
		int whole = in_arena(unit, DATA_UNIT);

		for (uint64_t a = unit; whole && a < unit + DATA_UNIT; a += GX_PAGE)
			whole = (page_state[page_of(a)] &
				 (PG_USED | PG_COMMITTED | PG_SHARED | PG_COPY | PG_HOLE)) == PG_USED;
		if (whole && !image_of(unit) && !image_of(unit + DATA_UNIT - 1)) {
			if (host_commit((void *)unit, DATA_UNIT, DATA_PROT))
				return -1;
			/* All of it is there; the guest has what it asked for. */
			memset(&page_state[page_of(unit)], PG_USED | PG_COMMITTED, DATA_UNIT / GX_PAGE);
			count_committed(DATA_UNIT / GX_PAGE, page_of(unit) + DATA_UNIT / GX_PAGE);
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
			if (host_commit((void *)a, b - a, DATA_PROT))
				return -1;
			memset(&page_state[page_of(a)], PG_USED | PG_COMMITTED, (b - a) / GX_PAGE);
			count_committed((b - a) / GX_PAGE, page_of(b));
			a = b;
		}
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
	/* A veneer of 32 bytes for every svc: one in 128 bytes was plenty for
	 * a browser, and the Steam client, which makes its system calls itself
	 * rather than through libc, has nearly twice that. */
	uint64_t size = GX_PAGE_UP(len / 32);

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
	uint64_t pool = pool_size_for(len), run = 0, above = 0, below = 0;

	for (uint64_t p = page_of(addr + len); p < (arena_size / GX_PAGE) && !above; p++) {
		run = (page_state[p] & PG_USED) ? 0 : run + 1;
		if (run == pool / GX_PAGE)
			above = (uint64_t)arena + (p + 1) * GX_PAGE - pool;
	}
	/* Or the last one before it, if that is nearer: a branch only reaches
	 * so far, and a program with much memory has no hole for a long way
	 * after where a library of its child's is put. */
	run = 0;
	for (uint64_t p = page_of(addr); p-- > 0 && !below;) {
		run = (page_state[p] & PG_USED) ? 0 : run + 1;
		if (run == pool / GX_PAGE)
			below = (uint64_t)arena + p * GX_PAGE;
	}
	if (above && below && addr - below < above + pool - (addr + len))
		above = 0;
	if (!above && !below)
		return NULL;
	return add_image(addr, len, above ? above : below, pool);
}

/*
 * The atomic instructions of ARMv8.1 ("LSE": swap, compare and swap, add and
 * the like, to memory, in one instruction) for a processor that does not have
 * them, which is the Switch's: programs built for newer ones use them without
 * asking (the client of a game store does, some twelve thousand times). Each
 * is replaced, like an svc, by a branch to a veneer, which does the same with
 * the instructions every ARMv8 has (load exclusive, store exclusive, again if
 * something came between) on registers it saves and puts back. Not when the
 * program is loaded, as for svc: what looks like one of them may be numbers
 * kept among the code, and 22 bits of the 32 are free. When the processor
 * refuses one (gmem_illegal()), which only happens to those that are run.
 *
 * Returns how many words the veneer has, 0 if the instruction is not one of
 * these. `back_from_start` is where the veneer returns to, as an offset from
 * its first word.
 */
#define LSE_VENEER 80   /* bytes kept for one: 20 words at most */

static int lse_veneer(uint32_t insn, uint32_t *v, int64_t back_from_start)
{
	int is_casp = (insn & 0xBFA07C00u) == 0x08207C00u;
	int is_cas = (insn & 0x3FA07C00u) == 0x08A07C00u;
	int is_ld = (insn & 0x3F200C00u) == 0x38200000u;
	uint32_t size = is_casp ? 2 + ((insn >> 30) & 1) : insn >> 30;
	uint32_t rs = (insn >> 16) & 31, rn = (insn >> 5) & 31, rt = insn & 31;
	uint32_t sf = size == 3 ? 0x80000000u : 0, opc = (insn >> 12) & 7, o3 = (insn >> 15) & 1;
	int extreme = is_ld && !o3 && opc > 3;   /* the larger or the smaller of the two */
	uint32_t s[5];
	int n = 0, k = 0, loop;

	if (!is_cas && !is_casp && !is_ld)
		return 0;
	if (is_ld && o3 && opc)
		return 0;
	if (is_casp && ((rs & 1) || (rt & 1)))
		return 0;
	/* Registers that the instruction does not name. */
	for (uint32_t r = 9; r <= 17 && k < 5; r++)
		if (r != rs && r != rn && r != rt && !(is_casp && (r == rs + 1 || r == rt + 1)))
			s[k++] = r;
	if (k < (extreme ? 5 : 4))
		return 0;
#define A s[0]   /* what was in memory */
#define B s[1]   /* what goes there (for a pair: the second half of what was) */
#define C s[2]   /* whether it went */
#define D s[3]   /* where */
#define E s[4]   /* the flags, when a comparison is needed */
	if (extreme)
		v[n++] = 0xF81F0FE0u | E;                             /* str E, [sp, #-16]! */
	v[n++] = 0xA9BE0000u | (B << 10) | (31 << 5) | A;             /* stp A, B, [sp, #-32]! */
	v[n++] = 0xA9010000u | (D << 10) | (31 << 5) | C;             /* stp C, D, [sp, #16] */
	if (rn == 31)
		v[n++] = (extreme ? 0x9100C3E0u : 0x910083E0u) | D;   /* add D, sp, #32 (or #48) */
	else
		v[n++] = 0xAA0003E0u | (rn << 16) | D;                /* mov D, Xn */
	if (extreme)
		v[n++] = 0xD53B4200u | E;                             /* mrs E, nzcv */
	loop = n;
	if (is_casp) {
		/* The pair instructions say their width one bit lower. */
		uint32_t wide = size == 3 ? 0x40000000u : 0;
		int skip1, skip2, same;

		v[n++] = wide | 0x887F8000u | (B << 10) | (D << 5) | A;         /* ldaxp A, B, [D] */
		v[n++] = sf | 0x4A000000u | (rs << 16) | (A << 5) | C;        /* eor C, A, Rs */
		skip1 = n++;                                                  /* cbnz C, 2f */
		v[n++] = sf | 0x4A000000u | ((rs + 1) << 16) | (B << 5) | C;  /* eor C, B, Rs+1 */
		skip2 = n++;                                                  /* cbnz C, 2f */
		v[n++] = wide | 0x88208000u | (C << 16) | ((rt + 1) << 10) | (D << 5) | rt;   /* stlxp wC, Rt, Rt+1, [D] */
		v[n] = 0x35000000u | ((uint32_t)((loop - n) & 0x7FFFF) << 5) | C;           /* cbnz wC, 1b */
		n++;
		same = n++;                                                   /* b 3f */
		v[skip1] = sf | 0x35000000u | ((uint32_t)(n - skip1) << 5) | C;
		v[skip2] = sf | 0x35000000u | ((uint32_t)(n - skip2) << 5) | C;
		/* 2: not what was expected. It is put back as it was read, to
		 * know that the two halves were read as one. */
		v[n++] = wide | 0x88208000u | (C << 16) | (B << 10) | (D << 5) | A;   /* stlxp wC, A, B, [D] */
		v[n] = 0x35000000u | ((uint32_t)((loop - n) & 0x7FFFF) << 5) | C;   /* cbnz wC, 1b */
		n++;
		v[same] = 0x14000000u | (uint32_t)(n - same);
		v[n++] = sf | 0x2A0003E0u | (A << 16) | rs;                   /* 3: mov Rs, A */
		v[n++] = sf | 0x2A0003E0u | (B << 16) | (rs + 1);             /* mov Rs+1, B */
	} else if (is_cas) {
		int skip;

		v[n++] = (size << 30) | 0x085FFC00u | (D << 5) | A;   /* ldaxr A, [D] */
		v[n++] = sf | 0x4A000000u | (rs << 16) | (A << 5) | C;   /* eor C, A, Rs */
		if (size < 2)                                         /* and wC, wC, #0xff or #0xffff */
			v[n++] = (size ? 0x12003C00u : 0x12001C00u) | (C << 5) | C;
		skip = n;
		n++;                                                  /* cbnz C, 2f */
		v[n++] = (size << 30) | 0x0800FC00u | (C << 16) | (D << 5) | rt;   /* stlxr wC, Rt, [D] */
		v[n] = 0x35000000u | ((uint32_t)((loop - n) & 0x7FFFF) << 5) | C;  /* cbnz wC, 1b */
		n++;
		v[skip] = sf | 0x35000000u | ((uint32_t)(n - skip) << 5) | C;
		if (rs != 31)                                         /* 2: mov Rs, A */
			v[n++] = sf | 0x2A0003E0u | (A << 16) | rs;
	} else {
		static const uint32_t ops[4] = {
			0x0B000000u /* add */, 0x0A200000u /* bic */, 0x4A000000u /* eor */, 0x2A000000u /* orr */,
		};
		/* Signed larger, signed smaller, unsigned larger, unsigned smaller. */
		static const uint32_t keep_if[4] = { 12 /* gt */, 11 /* lt */, 8 /* hi */, 3 /* lo */ };
		int is_signed = extreme && opc < 6;
		uint32_t stored = rs;

		v[n++] = (size << 30) | 0x085FFC00u | (D << 5) | A;   /* ldaxr A, [D] */
		if (extreme) {
			if (is_signed && size < 2)                    /* sxtb or sxth wA, wA */
				v[n++] = (size ? 0x13003C00u : 0x13001C00u) | (A << 5) | A;
			/* cmp A, Rs, as wide as the operation and signed as it is */
			v[n++] = sf | 0x6B200000u | (rs << 16) | (((is_signed ? 4u : 0u) | size) << 13) | (A << 5) | 31;
			/* csel B, A, Rs, cond */
			v[n++] = sf | 0x1A800000u | (rs << 16) | (keep_if[opc - 4] << 12) | (A << 5) | B;
			stored = B;
		} else if (!o3) {
			v[n++] = sf | ops[opc] | (rs << 16) | (A << 5) | B;   /* op B, A, Rs */
			stored = B;
		}
		v[n++] = (size << 30) | 0x0800FC00u | (C << 16) | (D << 5) | stored;   /* stlxr wC, .., [D] */
		v[n] = 0x35000000u | ((uint32_t)((loop - n) & 0x7FFFF) << 5) | C;      /* cbnz wC, 1b */
		n++;
		if (is_signed && size < 2)                            /* and wA, wA, #0xff or #0xffff */
			v[n++] = (size ? 0x12003C00u : 0x12001C00u) | (A << 5) | A;
		if (extreme)
			v[n++] = 0xD51B4200u | E;                     /* msr nzcv, E */
		if (rt != 31)
			v[n++] = sf | 0x2A0003E0u | (A << 16) | rt;   /* mov Rt, A */
	}
	v[n++] = 0xA9410000u | (D << 10) | (31 << 5) | C;             /* ldp C, D, [sp, #16] */
	v[n++] = 0xA8C20000u | (B << 10) | (31 << 5) | A;             /* ldp A, B, [sp], #32 */
	if (extreme)
		v[n++] = 0xF84107E0u | E;                             /* ldr E, [sp], #16 */
	v[n] = 0x14000000u | ((uint32_t)((back_from_start - n * 4) >> 2) & 0x03FFFFFFu);   /* b back */
	n++;
#undef A
#undef B
#undef C
#undef D
#undef E
	return n;
}

/* The processor refused the instruction at pc, in the guest's code: if it
 * is an atomic one that can be done otherwise it is replaced by a branch to
 * a veneer that does, and the thread is to run it again (returns 1). The code
 * and the veneers are not executable for the time it takes to write to them;
 * threads that run into that try again too (gmem_fault()). */
int gmem_illegal(uint64_t pc)
{
	const uint8_t need = PG_COMMITTED | GX_PROT_READ | GX_PROT_EXEC;
	uint64_t page = GX_PAGE_DOWN(pc), pool_page, pool_span;
	uint32_t insn, scratch[24], *code = (uint32_t *)pc;
	struct image *img;
	int64_t to_veneer;
	int done = 0;

	if (!arena || (pc & 3) || !in_arena(pc, 4))
		return 0;
	host_lock_acquire(lock);
	if ((page_state[page_of(pc)] & need) != need || is_wx(page_state[page_of(pc)]))
		goto out;
	insn = *code;
	if ((insn & 0xFC000000u) == 0x14000000u) {
		done = 1;   /* another thread got here first */
		goto out;
	}
	if (!lse_veneer(insn, scratch, 0))
		goto out;
	img = image_of(pc);
	if (!img)
		img = adopt_image(page, GX_PAGE);
	to_veneer = img ? (int64_t)(img->pool_next - pc) : 0;
	if (!img || img->pool_next + LSE_VENEER > img->pool_end || to_veneer >= BRANCH_RANGE ||
	    to_veneer <= -BRANCH_RANGE) {
		host_log("gx: no veneer to be had for the instruction at %#lx\n", (unsigned long)pc);
		goto out;
	}
	pool_page = GX_PAGE_DOWN(img->pool_next);
	pool_span = GX_PAGE_UP(img->pool_next + LSE_VENEER) - pool_page;
	if (host_protect((void *)pool_page, pool_span, GX_PROT_READ | GX_PROT_WRITE))
		goto out;
	lse_veneer(insn, (uint32_t *)img->pool_next, (int64_t)(pc + 4 - img->pool_next));
	host_protect((void *)pool_page, pool_span, GX_PROT_READ | GX_PROT_EXEC);
	host_flush_icache((void *)img->pool_next, LSE_VENEER);
	if (host_protect((void *)page, GX_PAGE, GX_PROT_READ | GX_PROT_WRITE))
		goto out;
	*code = 0x14000000u | (((uint32_t)(to_veneer >> 2)) & 0x03FFFFFFu);
	host_protect((void *)page, GX_PAGE, page_state[page_of(pc)] & PG_PROT);
	host_flush_icache(code, 4);
	img->pool_next += LSE_VENEER;
	atomics_replaced++;
	done = 1;
out:
	host_lock_release(lock);
	return done;
}

/* A host may refuse the guest the virtual counter and let it have the
 * physical one, which counts the same (Horizon): the instruction at pc, in
 * the guest's code, that reads the first is made to read the second, and the
 * thread is to run it again (returns 1). Each read that is refused is a fault,
 * and a program that times itself by the counter has thousands a second. */
#define MRS_CNTVCT 0xD53BE040u
#define MRS_CNTPCT 0xD53BE020u

int gmem_counter(uint64_t pc)
{
	const uint8_t need = PG_COMMITTED | GX_PROT_READ | GX_PROT_EXEC;
	uint64_t page = GX_PAGE_DOWN(pc);
	uint32_t insn, *code = (uint32_t *)pc;
	int done = 0;

	if (!arena || (pc & 3) || !in_arena(pc, 4))
		return 0;
	host_lock_acquire(lock);
	if ((page_state[page_of(pc)] & need) != need || is_wx(page_state[page_of(pc)]) ||
	    (page_state[page_of(pc)] & (PG_SHARED | PG_COPY | PG_HOLE)))
		goto out;
	insn = *code;
	if ((insn & ~31u) == MRS_CNTPCT) {
		done = 1;   /* another thread got here first */
		goto out;
	}
	if ((insn & ~31u) != MRS_CNTVCT)
		goto out;
	if (host_protect((void *)page, GX_PAGE, GX_PROT_READ | GX_PROT_WRITE))
		goto out;
	*code = MRS_CNTPCT | (insn & 31);
	host_protect((void *)page, GX_PAGE, page_state[page_of(pc)] & PG_PROT);
	host_flush_icache(code, 4);
	counters_replaced++;
	done = 1;
out:
	host_lock_release(lock);
	return done;
}

/* Replaces each svc #0 in [addr, addr+len) by a branch to a veneer. The pool
 * is not executable while veneers are added to it: that is safe as long as an
 * image is patched before any of its code runs. */
static int patch_syscalls(uint64_t addr, uint64_t len)
{
	struct image *img = image_of(addr);
	uint32_t *code = (uint32_t *)addr;
	int patched = 0, ret = 0;

	for (uint64_t k = 0; k < len / 4; k++) {
		uint64_t site = addr + k * 4;
		uint32_t *v;
		int64_t to_veneer, back;
		int is_svc = 1;
		uint64_t need = VENEER_SIZE;

		if (code[k] != SVC0)
			continue;
		if (!img)
			img = adopt_image(addr, len);
		if (!img || img->pool_next + need > img->pool_end) {
			host_log("gx: no veneer left for the instruction at %#lx\n", (unsigned long)site);
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
		if (to_veneer >= BRANCH_RANGE || to_veneer <= -BRANCH_RANGE || back >= BRANCH_RANGE ||
		    back <= -BRANCH_RANGE) {
			host_log("gx: the instruction at %#lx is too far from its veneer pool\n",
				 (unsigned long)site);
			ret = -1;
			break;
		}
		if (is_svc) {
			v[0] = 0xA9BF47F0u; /* stp x16, x17, [sp, #-16]! */
			v[1] = 0x10000070u; /* adr x16, exit */
			v[2] = 0x58000091u; /* ldr x17, trampoline */
			v[3] = 0xD61F0220u; /* br  x17 */
			v[4] = 0xA8C147F0u; /* exit: ldp x16, x17, [sp], #16 */
			v[5] = 0x14000000u | (((uint32_t)(back >> 2)) & 0x03FFFFFFu);
			*(uint64_t *)&v[6] = (uint64_t)gx_svc_trampoline;
		}
		code[k] = 0x14000000u | (((uint32_t)(to_veneer >> 2)) & 0x03FFFFFFu);
		img->pool_next += need;
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

/* What is read here is in the guest's memory from now on, and the copy the
 * kernel keeps of everything it reads serves nobody: on a kernel with a few
 * hundred megabytes for all it does, a library of that size, read at once,
 * took them all, and the kernel had nothing left to work with. So: a few
 * megabytes at a time, and the kernel is told after each that it may forget
 * them. */
#define READ_PIECE (4UL << 20)

static long read_file(int fd, uint64_t off, uint8_t *dst, uint64_t len)
{
	uint64_t done = 0, forgotten = 0;

	while (done < len) {
		uint64_t want = len - done < READ_PIECE ? len - done : READ_PIECE;
		long n = lkl_sys_pread64(fd, (char *)dst + done, want, off + done);

		if (n < 0)
			return n;
		if (n == 0)
			break;
		done += n;
		if (done - forgotten >= READ_PIECE || done == len) {
			long params[6] = { fd, (long)(off + forgotten), (long)(done - forgotten), 4 /* POSIX_FADV_DONTNEED */ };

			lkl_syscall(__lkl__NR_fadvise64, params);
			forgotten = done;
		}
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

/* The files without a view are too many: the dead go, then the oldest in the
 * table until half are left. */
static void collect_shared(void)
{
	for (int i = 0; i < MAX_SHARED; i++)
		if (shared[i].kept && is_dead(&shared[i]))
			discard_shared(&shared[i]);
	for (int i = 0; i < MAX_SHARED && kept_count > KEEP_MAX / 2; i++)
		if (shared[i].kept)
			close_shared(&shared[i]);
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

/* One address for a place in memory that may be seen at several: a file that
 * is shared has a view per mapping, in every program that maps it, and they
 * are all the same memory. Its place in our own mapping of it stands for
 * them all; any other address stands for itself. */
uint64_t gmem_same_place(uint64_t addr)
{
	uint64_t place = addr;

	if (!arena || !in_arena(addr, 1) || !(page_state[page_of(addr)] & PG_SHARED))
		return addr;
	host_lock_acquire(lock);
	for (int i = 0; i < MAX_VIEWS; i++) {
		const struct view *v = &views[i];

		if (v->pages && addr >= v->addr && addr < v->addr + v->pages * GX_PAGE) {
			if (v->object && v->object->chunk)
				place = v->object->chunk->addr + (addr - v->base);
			break;
		}
	}
	host_lock_release(lock);
	return place;
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
		if ((addr & (GX_PAGE - 1)) || !in_arena(addr, size)) {
			host_log("gx: no mapping at %#lx+%#lx: %s\n", (unsigned long)addr, (unsigned long)size,
				 addr & (GX_PAGE - 1) ? "not the start of a page" : "outside the guest's addresses");
			return -GX_ENOMEM;
		}
		for (uint64_t p = page_of(addr); p < page_of(addr) + size / GX_PAGE; p++)
			if (page_state[p] & PG_HOLE) {
				host_log("gx: no mapping at %#lx+%#lx: %#lx is the host's\n", (unsigned long)addr,
					 (unsigned long)size, (unsigned long)((uint64_t)arena + p * GX_PAGE));
				return -GX_ENOMEM;
			}
		if (flags & GX_MAP_FIXED_NOREPLACE) {
			for (uint64_t p = page_of(addr); p < page_of(addr) + size / GX_PAGE; p++)
				if (page_state[p] & PG_USED)
					return -17; /* EEXIST */
		}
		gmem_note("a mapping at a fixed address", addr, addr + size, prot);
		unmap_range(addr, size);
	} else {
		/* A library is loaded into room that was asked for first, with
		 * nothing in it and no file (and, by the C library's loader, as
		 * not to be written to: the one use the flag still has). Room
		 * of that kind gets its veneers next to it like a file's: code
		 * comes there, and veneers found for it later may be too far. */
		int for_library = anonymous && !(prot & PG_PROT) && (flags & GX_MAP_DENYWRITE);

		addr = anonymous && !for_library ? find_free_anonymous(size) : new_image(size);
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
		if (flags & GX_MAP_STACK)
			note_stack(addr, addr + size);
		if (gmem_lazy && (prot & PG_PROT) && is_data(addr, prot) &&
		    (size > EAGER_MAPPED || (flags & GX_MAP_STACK))) {
			/* When it is first used: see lazy_fault(). */
			memset(&page_state[page_of(addr)], PG_USED | (prot & PG_PROT), size / GX_PAGE);
			return (long)addr;
		}
		if (gmem_lazy && (prot & PG_PROT) && is_data(addr, prot))
			eager_commits++;
		if ((prot & PG_PROT) && is_data(addr, prot)) {
			if (host_commit((void *)addr, size, DATA_PROT))
				return -GX_ENOMEM;
			count_committed(size / GX_PAGE, page_of(addr) + size / GX_PAGE);
			memset(&page_state[page_of(addr)], PG_USED | PG_COMMITTED | (prot & PG_PROT),
			       size / GX_PAGE);
			return (long)addr;
		}
		return commit(addr, size, prot) ? -GX_ENOMEM : (long)addr;
	}

	if (commit(addr, size, GX_PROT_READ | GX_PROT_WRITE))
		return -GX_ENOMEM;
	ret = read_file(fd, off, (uint8_t *)addr, len);
	if (ret < 0) {
		release(addr, size);
		return ret;
	}
	if (prot & GX_PROT_EXEC) {
		if (patch_syscalls(addr, size) < 0) {
			release(addr, size);
			return -GX_ENOMEM;
		}
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

uint64_t gmem_code(const uint32_t *code, int words)
{
	uint64_t addr;

	host_lock_acquire(lock);
	addr = new_image(GX_PAGE);
	if (addr && commit(addr, GX_PAGE, GX_PROT_READ | GX_PROT_WRITE))
		addr = 0;
	if (addr) {
		memcpy((void *)addr, code, (size_t)words * 4);
		if (patch_syscalls(addr, GX_PAGE) < 0 ||
		    host_protect((void *)addr, GX_PAGE, GX_PROT_READ | GX_PROT_EXEC)) {
			addr = 0;
		} else {
			memset(&page_state[page_of(addr)],
			       PG_USED | PG_COMMITTED | GX_PROT_READ | GX_PROT_EXEC, 1);
			host_flush_icache((void *)addr, GX_PAGE);
		}
	}
	host_lock_release(lock);
	return addr;
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

		int gone;

		if (!img->end || addr + size <= img->start || addr >= img->end)
			continue;
		/* An image goes when the last of it does, in however many
		 * pieces: the loader gives back what it asked too much of
		 * (either end of the room it had a library in) and a program
		 * that ends gives back the rest, piece by piece. An image that
		 * was kept after that stood for addresses that others had by
		 * then: their code was sent to its veneers, wherever those had
		 * been, and its veneers were given back from under whoever had
		 * that place since. What goes from either end is no longer the
		 * image's either. */
		if (addr <= img->start && addr + size >= img->end) {
			gone = 1;
		} else {
			if (addr <= img->start)
				img->start = addr + size;
			else if (addr + size >= img->end)
				img->end = addr;
			gone = 1;
			for (uint64_t a = img->start; a < img->end && gone; a += GX_PAGE)
				gone = !(page_state[page_of(a)] & PG_USED);
		}
		if (!gone)
			continue;
		gmem_note("the veneers of an image that was unmapped", img->pool, img->pool_end, img->start);
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

		/* What a run is and what becomes of it, as one: a page of it
		 * may be on its way to being committed (lazy_fault()). */
		host_lock_acquire(lazy_lock);
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
		} else if (is_lazy(page_state[p]) ||
			   (gmem_lazy && !(page_state[p] & PG_COMMITTED) && (prot & PG_PROT) &&
			    !(prot & GX_PROT_EXEC) && is_data(run_addr, prot))) {
			/* Wanted, or no longer: there when it is first used, or
			 * now if it is little (see EAGER_PROTECTED). */
			if ((prot & GX_PROT_EXEC)) {
				if (commit(run_addr, (q - p) * GX_PAGE, prot))
					ret = -GX_ENOMEM;
			} else if ((prot & PG_PROT) && (q - p) * GX_PAGE <= EAGER_PROTECTED && !in_stack(run_addr) &&
				   !host_commit((void *)run_addr, (q - p) * GX_PAGE, DATA_PROT)) {
				memset(&page_state[p], PG_USED | PG_COMMITTED | (prot & PG_PROT), q - p);
				count_committed(q - p, q);
				eager_commits++;
			} else {
				memset(&page_state[p], PG_USED | (prot & PG_PROT), q - p);
			}
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
		host_lock_release(lazy_lock);
		p = q;
	}
	host_lock_release(lock);
	return ret;
}

long gmem_madvise(uint64_t addr, uint64_t len, int advice)
{
	uint64_t size = GX_PAGE_UP(len);

	if ((addr & (GX_PAGE - 1)) || !in_arena(addr, size))
		return -GX_EINVAL;
	if (advice != MADV_DONTNEED)
		return 0;
	/* Private anonymous memory reads as zeroes afterwards, and allocators
	 * count on it. */
	host_lock_acquire(lock);
	for (uint64_t p = page_of(addr), end = p + size / GX_PAGE; p < end;) {
		uint8_t state;
		uint64_t q = p;

		host_lock_acquire(lazy_lock);   /* as in gmem_mprotect() */
		state = page_state[p];
		while (q < end && page_state[q] == state)
			q++;
		if (!(state & PG_COMMITTED) || (state & (PG_SHARED | PG_COPY))) {
			/* nothing there, or not private */
		} else if (is_wx(state)) {
			/* Not to be written to from here: it may be executable. */
			host_decommit(arena + p * GX_PAGE, (q - p) * GX_PAGE);
			memset(&page_state[p], PG_USED | WX_PROT, q - p);
			count_committed(-(int64_t)(q - p), 0);
		} else if (gmem_lazy && gmem_lazy_wanted_only && !(state & GX_PROT_EXEC) &&
			   is_data((uint64_t)arena + p * GX_PAGE, 0) &&
			   (!(state & PG_PROT) || (q - p) * GX_PAGE >= DONTNEED_GIVEN_BACK)) {
			/* Given back for real where the host has pieces to spare:
			 * this is how a browser's allocators let go of memory
			 * (they then take away all access, or unmap), and kept
			 * it was never anyone's again: the 28 MB of a large
			 * script, taken and let go some twenty-five times while
			 * a Steam client logs in, stayed committed each time.
			 * What is still wanted is there again when it is used. */
			host_decommit(arena + p * GX_PAGE, (q - p) * GX_PAGE);
			memset(&page_state[p], PG_USED | (state & PG_PROT), q - p);
			count_committed(-(int64_t)(q - p), 0);
			given_back += q - p;
		} else if ((state & GX_PROT_WRITE) ||
			   (!(state & PG_PROT) && is_data((uint64_t)arena + p * GX_PAGE, 0))) {
			/* Cleared and kept, also what has no access (how
			 * PartitionAlloc decommits): giving it back would cut the
			 * host's mapping in pieces. */
			memset(arena + p * GX_PAGE, 0, (q - p) * GX_PAGE);
		} else if (!(state & PG_PROT)) {
			host_decommit(arena + p * GX_PAGE, (q - p) * GX_PAGE);
			memset(&page_state[p], PG_USED, q - p);
			count_committed(-(int64_t)(q - p), 0);
		}
		host_lock_release(lazy_lock);
		p = q;
	}
	host_lock_release(lock);
	return 0;
}

/* How much of [addr, addr+max) can be read and written right now, from addr
 * on: memory that is there, not memory that would be on first use, and
 * memory of the program's own, not a file it shares. */
uint64_t gmem_writable(uint64_t addr, uint64_t max)
{
	const uint8_t need = PG_COMMITTED | GX_PROT_READ | GX_PROT_WRITE;
	uint64_t end = addr;

	if (!in_arena(addr, 1))
		return 0;
	while (end < addr + max && in_arena(end, 1) && (page_state[page_of(end)] & need) == need &&
	       !(page_state[page_of(end)] & (PG_SHARED | PG_COPY)) && !is_wx(page_state[page_of(end)]))
		end = GX_PAGE_DOWN(end) + GX_PAGE;
	return end > addr + max ? max : end - addr;
}

int gmem_accessible(uint64_t addr, uint64_t len, int write)
{
	uint8_t need = PG_COMMITTED | (write ? GX_PROT_WRITE : GX_PROT_READ);

	if (!len || !in_arena(addr, 1))
		return 1;   /* not ours to judge: the host's own memory */
	if (!in_arena(addr, len))
		return 0;
	for (uint64_t p = page_of(addr); p <= page_of(addr + len - 1); p++)
		if ((page_state[p] & need) != need && !is_wx(page_state[p]) &&
		    !(is_lazy(page_state[p]) && (page_state[p] & need & PG_PROT)))
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

/* Where the break is: from where it started to where it is now. */
void gmem_break(uint64_t *start, uint64_t *end)
{
	*start = brk_start;
	*end = GX_PAGE_UP(brk_cur);
}

long gmem_brk(uint64_t addr)
{
	host_lock_acquire(lock);
	if (brk_start && addr >= brk_start && addr <= brk_start + BRK_SIZE) {
		uint64_t old_top = GX_PAGE_UP(brk_cur), new_top = GX_PAGE_UP(addr);

		/* As one with lazy_fault(), which may have committed the
		 * bottom of what is kept for the break along with a neighbour. */
		host_lock_acquire(lazy_lock);
		if (new_top > old_top) {
			int failed = 0;

			/* In runs: each call is a piece of memory of its own for
			 * the host. */
			for (uint64_t a = old_top, b; a < new_top && !failed; a = b) {
				int there = page_state[page_of(a)] & PG_COMMITTED;

				for (b = a + GX_PAGE; b < new_top &&
				     (page_state[page_of(b)] & PG_COMMITTED) == there; b += GX_PAGE)
					;
				if (there)
					memset(&page_state[page_of(a)],
					       PG_USED | PG_COMMITTED | GX_PROT_READ | GX_PROT_WRITE, (b - a) / GX_PAGE);
				else
					failed = commit(a, b - a, GX_PROT_READ | GX_PROT_WRITE);
			}
			if (!failed)
				brk_cur = addr;
		} else {
			if (new_top < old_top) {
				host_decommit((void *)new_top, old_top - new_top);
				memset(&page_state[page_of(new_top)], PG_USED,
				       (old_top - new_top) / GX_PAGE);
			}
			brk_cur = addr;
		}
		host_lock_release(lazy_lock);
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
