/*
 * gsys.c - what a guest syscall becomes.
 *
 * Memory, threads, futexes and process exit are handled here, because LKL has
 * no MMU, no user processes and is built without futexes. Everything else
 * (files, pipes, sockets, time) goes to the kernel through lkl_syscall().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lkl.h>
#include "gx.h"

#define CLONE_VM 0x100
#define CLONE_THREAD 0x10000
#define CLONE_SETTLS 0x80000
#define CLONE_PARENT_SETTID 0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define CLONE_CHILD_SETTID 0x1000000

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CLOCK_REALTIME 256

struct gx_tp_slot gx_tp_tab[GX_MAX_THREADS] = {
	[0 ... GX_MAX_THREADS - 1] = { ~0UL, NULL },
};
struct gx_thread *gx_main_thread;
int gx_trace;

static int cpus = 1;

/* Programs size their thread pools on this. The kernel has one processor of
 * its own, so what it shows in /sys is covered with what we want read there;
 * the threads are the host's to schedule either way. */
void gx_set_cpus(int count)
{
	static const char dir[] = "/sys/devices/system/cpu";
	static const char *const files[] = { "online", "possible", "present" };
	char path[64], range[16];
	int len = 0;

	if (count < 1 || count > 64 || lkl_sys_mount("tmpfs", (char *)dir, "tmpfs", 0, NULL))
		return;
	cpus = count;
	range[len++] = '0';
	if (count > 1) {
		range[len++] = '-';
		if (count > 10)
			range[len++] = '0' + (count - 1) / 10;
		range[len++] = '0' + (count - 1) % 10;
	}
	range[len++] = '\n';
	for (int i = 0; i < 3; i++) {
		int fd;

		strcpy(path, dir);
		strcat(path, "/");
		strcat(path, files[i]);
		fd = lkl_sys_open(path, LKL_O_WRONLY | LKL_O_CREAT, 0444);
		if (fd >= 0) {
			lkl_sys_write(fd, range, len);
			lkl_sys_close(fd);
		}
	}
	for (int i = 0; i < count; i++) {
		int at;

		strcpy(path, dir);
		strcat(path, "/cpu");
		at = strlen(path);
		if (i >= 10)
			path[at++] = '0' + i / 10;
		path[at++] = '0' + i % 10;
		path[at] = 0;
		lkl_sys_mkdir(path, 0555);
	}
}

static struct gx_thread threads[GX_MAX_THREADS];
static int thread_used[GX_MAX_THREADS];
static int next_tid = 101;
static int live_threads = 1;
static struct host_lock *big_lock;

static void lock(void)
{
	if (!big_lock) {
		/* Still single-threaded the first time: clone() takes it first. */
		big_lock = host_lock_new();
	}
	host_lock_acquire(big_lock);
}

static void unlock(void) { host_lock_release(big_lock); }

/* ---------- futexes ---------- */

struct waiter {
	uint32_t *addr;
	uint32_t bitset;
	int woken;
	struct host_cond *cond;
	struct waiter *next;
};
static struct waiter *waiters;

/*
 * clock_gettime() without the kernel. Linux programs read the clock all the
 * time (Chromium, tens of thousands of times a second) because it costs them
 * nothing there: the kernel answers in the program's own memory, without a
 * syscall. Here every syscall is a real one and they all take turns on the
 * one processor the kernel has. The kernel's clocks are the host's clock plus
 * a constant, measured once per clock.
 */
#define CLOCKS 12

static int64_t clock_offset[CLOCKS];
static int clock_known[CLOCKS];

static long fast_clock_gettime(long id, struct lkl_timespec *ts)
{
	uint64_t ns;

	if (id == 3 /* CLOCK_THREAD_CPUTIME_ID */) {
		ns = host_thread_cpu_ns(host_thread_self());
	} else if (id == 0 || id == 1 || (id >= 4 && id <= 7) /* real, monotonic, coarse, boot */) {
		if (!__atomic_load_n(&clock_known[id], __ATOMIC_ACQUIRE)) {
			struct lkl_timespec kernel;
			long params[6] = { id, (long)&kernel };
			uint64_t before = host_now_ns();
			long ret = lkl_syscall(__lkl__NR_clock_gettime, params);

			if (ret)
				return ret;
			/* Against the middle of the time the question took. */
			clock_offset[id] = (int64_t)kernel.tv_sec * 1000000000LL + kernel.tv_nsec -
					   (int64_t)(before + (host_now_ns() - before) / 2);
			__atomic_store_n(&clock_known[id], 1, __ATOMIC_RELEASE);
		}
		ns = host_now_ns() + clock_offset[id];
	} else {
		return 1;   /* the kernel's to answer */
	}
	ts->tv_sec = ns / 1000000000ULL;
	ts->tv_nsec = ns % 1000000000ULL;
	return 0;
}

static int64_t timeout_ns(const struct lkl_timespec *ts, int absolute, int clock)
{
	int64_t ns;

	if (!ts)
		return -1;
	ns = (int64_t)ts->tv_sec * 1000000000LL + ts->tv_nsec;
	if (absolute) {
		/* The deadline is on the clock the guest reads. */
		struct lkl_timespec now;

		if (fast_clock_gettime(clock, &now) == 0)
			ns -= (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
	}
	return ns < 0 ? 0 : ns;
}

static long futex_wait(struct gx_thread *self, uint32_t *addr, uint32_t val, int64_t ns,
		       uint32_t bitset)
{
	struct waiter w = { addr, bitset, 0, NULL, NULL }, **p;
	long ret = 0;

	lock();
	if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) != val) {
		unlock();
		return -GX_EAGAIN;
	}
	if (!self->cond)
		self->cond = host_cond_new();
	w.cond = self->cond;
	w.next = waiters;
	waiters = &w;
	while (!w.woken) {
		if (host_cond_wait(w.cond, big_lock, ns)) {
			ret = w.woken ? 0 : -GX_ETIMEDOUT;
			break;
		}
	}
	for (p = &waiters; *p; p = &(*p)->next) {
		if (*p == &w) {
			*p = w.next;
			break;
		}
	}
	unlock();
	return ret;
}

/* Wakes up to n waiters on addr and moves up to n2 of the others to addr2. */
static long futex_wake(uint32_t *addr, int n, uint32_t bitset, uint32_t *addr2, int n2)
{
	long count = 0;

	lock();
	for (struct waiter *w = waiters; w; w = w->next) {
		if (w->addr != addr || w->woken || !(w->bitset & bitset))
			continue;
		if (count < n) {
			w->woken = 1;
			host_cond_signal(w->cond);
			count++;
		} else if (addr2 && n2 > 0) {
			w->addr = addr2;
			n2--;
			count++;
		}
	}
	unlock();
	return count;
}

static long do_futex(struct gx_frame *f, struct gx_thread *self)
{
	uint32_t *addr = (uint32_t *)f->x[0];
	int op = (int)f->x[1], cmd = op & 0x7f;
	uint32_t val = (uint32_t)f->x[2];
	int clock = (op & FUTEX_CLOCK_REALTIME) ? 0 : 1;

	switch (cmd) {
	case FUTEX_WAIT:
		return futex_wait(self, addr, val, timeout_ns((void *)f->x[3], 0, clock), ~0u);
	case FUTEX_WAIT_BITSET:
		return futex_wait(self, addr, val, timeout_ns((void *)f->x[3], 1, clock),
				  (uint32_t)f->x[5]);
	case FUTEX_WAKE:
		return futex_wake(addr, (int)val, ~0u, NULL, 0);
	case FUTEX_WAKE_BITSET:
		return futex_wake(addr, (int)val, (uint32_t)f->x[5], NULL, 0);
	case FUTEX_CMP_REQUEUE:
		if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) != (uint32_t)f->x[5])
			return -GX_EAGAIN;
		/* fall through */
	case FUTEX_REQUEUE:
		return futex_wake(addr, (int)val, ~0u, (uint32_t *)f->x[4], (int)f->x[3]);
	}
	return -GX_ENOSYS;
}

/* ---------- threads ---------- */

static void thread_main(void *arg)
{
	struct gx_thread *t = arg;

	gx_enter_frame(t->start, t);
}

static long do_clone(struct gx_frame *f)
{
	uint64_t flags = f->x[0], stack = f->x[1], tls = f->x[3];
	int *ptid = (int *)f->x[2], *ctid = (int *)f->x[4];
	struct gx_thread *t = NULL;
	struct gx_frame *child;
	int slot = -1;

	if (!(flags & CLONE_VM) || !(flags & CLONE_THREAD) || !stack) {
		host_log("gx: clone(%#lx) asks for a process, which is not supported\n",
			 (unsigned long)flags);
		return -GX_ENOSYS;
	}
	lock();
	for (int i = 0; i < GX_MAX_THREADS; i++) {
		if (!thread_used[i]) {
			thread_used[i] = 1;
			t = &threads[i];
			slot = i;
			break;
		}
	}
	if (!t) {
		unlock();
		return -GX_EAGAIN;
	}
	t->clear_tid = NULL;
	t->nr = -1;
	t->host_id = 0;
	t->name[0] = 0;
	t->syscalls = t->reported_syscalls = t->reported_cpu_ns = 0;
	t->tid = next_tid++;
	live_threads++;
	unlock();

	/* The child resumes after the same svc as its parent: its frame is a copy
	 * of the parent's, placed under the two words the exit veneer pops. */
	stack &= ~15UL;
	memcpy((void *)(stack - 16), (uint8_t *)f + GX_FRAME_SIZE, 16);
	child = (struct gx_frame *)(stack - 16 - GX_FRAME_SIZE);
	memcpy(child, f, GX_FRAME_SIZE);
	child->result = 0;
	if (flags & CLONE_SETTLS)
		child->gtp = tls;
	t->start = child;
	if (flags & CLONE_PARENT_SETTID)
		*ptid = t->tid;
	if (flags & CLONE_CHILD_SETTID)
		*ctid = t->tid;
	if (flags & CLONE_CHILD_CLEARTID)
		t->clear_tid = ctid;
	gx_tp_tab[slot].thread = t;
	__atomic_store_n(&gx_tp_tab[slot].guest_tp, child->gtp, __ATOMIC_SEQ_CST);

	if (host_thread_start(thread_main, t)) {
		lock();
		gx_tp_tab[slot].guest_tp = ~0UL;
		thread_used[slot] = 0;
		live_threads--;
		unlock();
		return -GX_EAGAIN;
	}
	return t->tid;
}

static void do_exit_thread(struct gx_thread *t, int code)
{
	int *clear_tid = t->clear_tid;
	int last;

	/* Out of the table first, then the joiner is told. Once told, the
	 * guest may give this thread's stack and TLS block to a new thread,
	 * which then has the same TLS pointer: tramp.S knows a thread by it,
	 * and found this one, still in the table, for the new thread's
	 * syscalls, which ran on this thread's host stack while it was still
	 * using it (seen as a crash in a lock, with its stack gone to zeroes,
	 * when a decoder made and ended threads by the dozen). */
	lock();
	last = --live_threads == 0;
	if (t != gx_main_thread) {
		int slot = t - threads;

		gx_tp_tab[slot].guest_tp = ~0UL;
		thread_used[slot] = 0;
	}
	unlock();
	if (clear_tid) {
		*clear_tid = 0;
		futex_wake((uint32_t *)clear_tid, 1, ~0u, NULL, 0);
	}
	if (last)
		host_exit(code);
	host_thread_exit();
}

/* ---------- everything else ---------- */

static long translate_open_flags(long af)
{
	long lf = af & 03;

	if (af & 0100) lf |= 0100;        /* O_CREAT */
	if (af & 0200) lf |= 0200;        /* O_EXCL */
	if (af & 0400) lf |= 0400;        /* O_NOCTTY */
	if (af & 01000) lf |= 01000;      /* O_TRUNC */
	if (af & 02000) lf |= 02000;      /* O_APPEND */
	if (af & 04000) lf |= 04000;      /* O_NONBLOCK */
	if (af & 040000) lf |= 0200000;   /* O_DIRECTORY */
	if (af & 0100000) lf |= 0400000;  /* O_NOFOLLOW */
	if (af & 0200000) lf |= 040000;   /* O_DIRECT */
	if (af & 0400000) lf |= 0100000;  /* O_LARGEFILE */
	if (af & 02000000) lf |= 02000000; /* O_CLOEXEC */
	if (af & 010000000) lf |= 010000000; /* O_PATH */
	return lf;
}

struct gx_iovec { void *base; uint64_t len; };

static void hostcall_missing(void)
{
	host_log("gx: the guest called a host function that the host does not have\n");
	host_exit(127);
}

/* The guest's thread with that id; 0 is the caller. */
static struct gx_thread *thread_of(struct gx_thread *self, int tid)
{
	if (!tid || tid == self->tid)
		return self;
	for (int i = 0; i < GX_MAX_THREADS; i++)
		if (gx_tp_tab[i].guest_tp != ~0UL && gx_tp_tab[i].thread &&
		    gx_tp_tab[i].thread->tid == tid)
			return gx_tp_tab[i].thread;
	return NULL;
}

/* What a thread is asked to run at: a real-time policy (SCHED_FIFO 1,
 * SCHED_RR 2) or a nice value. */
static long set_thread_priority(struct gx_thread *self, int tid, int policy, int rt, int nice)
{
	struct gx_thread *t = thread_of(self, tid);
	static int said;

	int level = policy == 1 || policy == 2 ? 2 : nice < 0 ? 1 : 0;

	if (!t)
		return -3;   /* ESRCH */
	/* Except the renderer's main thread, which runs the pages' scripts: a
	 * browser in one process asks for it what it asks for display threads,
	 * and above the time-sliced priority it held a core for as long as a
	 * page's script ran, the video's threads waiting behind it. */
	if (!strncmp(t->name, "Chrome_InProcRe", 15))
		level = 0;
	if (level && said < 40 && __atomic_fetch_add(&said, 1, __ATOMIC_SEQ_CST) < 40)
		host_log("gx: thread %d (%s) asks for policy %d, priority %d, nice %d\n",
			 t->tid, t->name, policy, rt, nice);
	if (gx_no_priorities)
		return 0;
	/* The caller's own handle when it is the caller: another thread's is
	 * only known once it has started. */
	if (t == self)
		host_thread_priority(host_thread_self(), level);
	else if (t->host_id)
		host_thread_priority(t->host_id, level);
	return 0;
}

int gx_no_priorities;

static long dispatch(struct gx_frame *f, struct gx_thread *self)
{
	long nr = f->x[8];
	long params[6] = { f->x[0], f->x[1], f->x[2], f->x[3], f->x[4], f->x[5] };
	int fd = (int)f->x[0];

	/* The kernel has no MMU to stop it at memory the guest cannot use, and
	 * programs do probe memory with these (Chromium reads into a page it
	 * made read-only). */
	switch (nr) {
	case 63: case 67: case 207:        /* read, pread64, recvfrom */
		if (!gmem_accessible(f->x[1], f->x[2], 1))
			return -GX_EFAULT;
		break;
	case 64: case 68: case 206:        /* write, pwrite64, sendto */
		if (!gmem_accessible(f->x[1], f->x[2], 0))
			return -GX_EFAULT;
		break;
	case 261:                          /* prlimit64: the old limit */
		if (f->x[3] && !gmem_accessible(f->x[3], 16, 1))
			return -GX_EFAULT;
		break;
	case 113: {                        /* clock_gettime */
		long ret;

		if (!gmem_accessible(f->x[1], 16, 1))
			return -GX_EFAULT;
		ret = fast_clock_gettime(f->x[0], (struct lkl_timespec *)f->x[1]);
		if (ret <= 0)
			return ret;
		break;
	}
	case 169: {                        /* gettimeofday: CLOCK_REALTIME, in microseconds */
		struct lkl_timespec ts;

		if (f->x[1]) {
			/* The kernel's time zone, which nothing here ever sets: UTC. */
			if (!gmem_accessible(f->x[1], 8, 1))
				return -GX_EFAULT;
			((int32_t *)f->x[1])[0] = 0;
			((int32_t *)f->x[1])[1] = 0;
		}
		if (f->x[0]) {
			if (!gmem_accessible(f->x[0], 16, 1))
				return -GX_EFAULT;
			if (fast_clock_gettime(0, &ts))
				break;
			((int64_t *)f->x[0])[0] = ts.tv_sec;
			((int64_t *)f->x[0])[1] = ts.tv_nsec / 1000;
		}
		return 0;
	}
	case 114: case 170:                /* clock_settime, settimeofday: the constants are no more */
		memset(clock_known, 0, sizeof(clock_known));
		break;
	}

	switch (nr) {
	case 222: {
		long ret = gmem_mmap(f->x[0], f->x[1], (int)f->x[2], (int)f->x[3], (int)f->x[4], f->x[5]);
		static int said;

		/* A program that is refused memory seldom says so before it
		 * stops: the first few refusals are worth a line. (Not those of
		 * hundreds of gigabytes of addresses: a browser tries sizes.) */
		if (ret < 0 && ret > -4096 && ret != -19 && f->x[1] < (1UL << 30) && said < 12 && ++said)
			host_log("gx: mmap of %lu KB (protection %d, flags %#x, file %d) refused: %ld\n",
				 (unsigned long)(f->x[1] >> 10), (int)f->x[2], (int)f->x[3], (int)f->x[4], ret);
		return ret;
	}
	case 215: return gmem_munmap(f->x[0], f->x[1]);
	case 226: return gmem_mprotect(f->x[0], f->x[1], (int)f->x[2]);
	case 214: return gmem_brk(f->x[0]);
	case 216: return -GX_ENOMEM;       /* mremap: glibc falls back to malloc+copy */
	case 233: return gmem_madvise(f->x[0], f->x[1], (int)f->x[2]);

	case GX_SYS_HOST_SYMBOL: {
		void *fn = host_symbol((const char *)f->x[0]);

		if (!fn && gx_trace)
			host_log("gx: no host function %s\n", (const char *)f->x[0]);
		return (long)fn;
	}
	case GX_SYS_HOSTCALL:
		return f->x[0] ? (long)hostcall_missing : (long)gx_hostcall;

	case 220: return do_clone(f);
	case 435: return -GX_ENOSYS;       /* clone3: glibc falls back to clone */
	case 98:  return do_futex(f, self);
	case 93:  do_exit_thread(self, (int)f->x[0]); return 0;
	case 94:
		if (gx_trace) {
			gmem_report();
			gx_report_kernel_memory();
		}
		host_exit((int)f->x[0]);
		return 0;
	case 96:  self->clear_tid = (int *)f->x[0]; return self->tid;
	case 178: return self->tid;        /* gettid */
	case 172: return 100;              /* getpid */
	case 173: return 1;                /* getppid */
	case 99:  return 0;                /* set_robust_list */
	case 293: return -GX_ENOSYS;       /* rseq */
	case 124: host_yield(); return 0;  /* sched_yield */
	/* The kernel does not know the guest's threads by the ids they have
	 * here: scheduling is the host's business. */
	case 118: case 122:                /* sched_setparam, _setaffinity */
		return 0;
	case 119:                          /* sched_setscheduler(tid, policy, param) */
		return set_thread_priority(self, (int)f->x[0], (int)f->x[1],
					   f->x[2] ? *(int *)f->x[2] : 0, 0);
	case 140:                          /* setpriority(which, who, nice) */
		if (f->x[0] != 0)          /* PRIO_PROCESS: a thread, here */
			return 0;
		return set_thread_priority(self, (int)f->x[1], 0, 0, (int)f->x[2]);
	case 274:                          /* sched_setattr(tid, attr, flags) */
		/* struct sched_attr: size, policy, flags, nice, priority */
		return set_thread_priority(self, (int)f->x[0], ((int *)f->x[1])[1],
					   ((int *)f->x[1])[5], ((int *)f->x[1])[4]);
	case 120: return 0;                /* sched_getscheduler: SCHED_OTHER */
	case 121:                          /* sched_getparam */
		*(int *)f->x[1] = 0;
		return 0;
	case 123:                          /* sched_getaffinity */
		if (f->x[1] < 8)
			return -GX_EINVAL;
		*(uint64_t *)f->x[2] = cpus >= 64 ? ~0UL : (1UL << cpus) - 1;
		return 8;

	case 134:                          /* rt_sigaction */
	case 135:                          /* rt_sigprocmask */
	case 132:                          /* sigaltstack */
		return 0;
	case 129: case 130: case 131:      /* kill, tkill, tgkill */
		host_log("gx: the guest raised signal %ld\n",
			 nr == 131 ? (long)f->x[2] : (long)f->x[1]);
		host_exit(128 + (int)(nr == 131 ? f->x[2] : f->x[1]));
		return 0;

	case 78:                           /* readlinkat */
		if (!strcmp((const char *)f->x[1], "/proc/self/exe")) {
			size_t len = strlen(gx_exe_path);

			if (len > f->x[3])
				len = f->x[3];
			memcpy((void *)f->x[2], gx_exe_path, len);
			return len;
		}
		break;
	case 279: {                        /* memfd_create */
		/* The kernel has none without an MMU: an unlinked file of the
		 * memory filesystem is the same thing. */
		static int serial;
		char path[64] = "/dev/shm/gx-memfd-";
		int n = __atomic_add_fetch(&serial, 1, __ATOMIC_SEQ_CST), at = strlen(path);
		long mfd;

		if (f->x[1] & ~0x1fUL)     /* programs test for memfd with bad flags */
			return -GX_EINVAL;
		for (int digit = 100000000; digit; digit /= 10)
			path[at++] = '0' + n / digit % 10;
		path[at] = 0;
		mfd = lkl_sys_open(path, LKL_O_RDWR | LKL_O_CREAT | LKL_O_EXCL |
				   ((f->x[1] & 1) ? LKL_O_CLOEXEC : 0), 0600);
		if (mfd >= 0)
			lkl_sys_unlink(path);
		return mfd;
	}
	case 46: {                         /* ftruncate */
		/* The memory filesystem of a kernel without an MMU wants one
		 * block of memory for a file that grows from nothing: it
		 * refuses large ones, and fills the others for nothing when
		 * the file is shared memory, which lives in gmem.c. Growing
		 * the file by a write of its last byte does neither. */
		struct lkl_stat st;
		long ret;

		if ((long)f->x[1] > 0 && !lkl_sys_fstat(fd, &st) && !st.st_size &&
		    (st.st_mode & LKL_S_IFMT) == LKL_S_IFREG && gmem_in_memory_fs(fd) &&
		    lkl_sys_pwrite64(fd, "", 1, f->x[1] - 1) == 1)
			return 0;
		ret = lkl_syscall(nr, params);

		if ((ret == -27 /* EFBIG */ || ret == -GX_ENOMEM) && (long)f->x[1] > 0 &&
		    lkl_sys_pwrite64(fd, "", 1, f->x[1] - 1) == 1)
			ret = 0;
		if (ret < 0) {
			static int said;

			if (said < 12 && ++said)
				host_log("gx: a file could not be made %lu KB long: %ld\n",
					 (unsigned long)(f->x[1] >> 10), ret);
		}
		return ret;
	}
	case 25:                           /* fcntl */
		if (f->x[1] == 1033 /* F_ADD_SEALS */ || f->x[1] == 1034 /* F_GET_SEALS */)
			return 0;              /* only memfd has seals */
		break;

	case 278:                          /* getrandom */
		/* From the host: the kernel has no source of entropy of its own
		 * and would make callers wait for some. */
		host_random((void *)f->x[0], f->x[1]);
		return f->x[1];

	case 261:                          /* prlimit64 */
		if (f->x[1] == 3 /* RLIMIT_STACK */ && !f->x[2] && f->x[3]) {
			uint64_t *lim = (uint64_t *)f->x[3];

			/* Also the default size of thread stacks, which have to be
			 * committed in full on a host that cannot do it lazily. */
			lim[0] = 2UL << 20;
			lim[1] = ~0UL;
			return 0;
		}
		break;

	case 64:                           /* write */
		if (fd == 1 || fd == 2) {
			host_console_write(fd, (void *)f->x[1], f->x[2]);
			return f->x[2];
		}
		break;
	case 66:                           /* writev */
		if (fd == 1 || fd == 2) {
			struct gx_iovec *iov = (struct gx_iovec *)f->x[1];
			long total = 0;

			for (int i = 0; i < (int)f->x[2]; i++) {
				host_console_write(fd, iov[i].base, iov[i].len);
				total += iov[i].len;
			}
			return total;
		}
		break;
	case 80:                           /* fstat */
		if (fd >= 0 && fd <= 2) {
			memset((void *)f->x[1], 0, 128);
			*(uint32_t *)(f->x[1] + 16) = 0020000 | 0666; /* a tty */
			return 0;
		}
		break;
	case 79:                           /* newfstatat with AT_EMPTY_PATH */
		if (fd >= 0 && fd <= 2 && (f->x[3] & 0x1000)) {
			memset((void *)f->x[2], 0, 128);
			*(uint32_t *)(f->x[2] + 16) = 0020000 | 0666;
			return 0;
		}
		break;
	case 29:                           /* ioctl */
		if (fd >= 0 && fd <= 2)
			return f->x[1] == 0x5401 /* TCGETS */ ? -GX_ENOTTY : 0;
		break;
	case 57:                           /* close */
		if (fd >= 0 && fd <= 2)
			return 0;
		break;
	case 56:                           /* openat */
		params[2] = translate_open_flags(params[2]);
		break;
	}
	return lkl_syscall(nr, params);
}

static uint64_t syscall_count;

static uint64_t reported_at;

/* One line per thread that did something since the last report: how much of
 * a processor it used, how many syscalls it made, and where it is now. */
static void report_thread(struct gx_thread *t, uint64_t elapsed)
{
	uint64_t cpu = t->host_id ? host_thread_cpu_ns(t->host_id) : 0;
	uint64_t used = cpu - t->reported_cpu_ns, calls = t->syscalls - t->reported_syscalls;
	long nr = t->nr;

	t->reported_cpu_ns = cpu;
	t->reported_syscalls = t->syscalls;
	if (elapsed && used * 100 / elapsed < 1 && !calls)
		return;
	host_log("  [%d] %-15s %3lu%% of a processor, %6lu syscalls, now in ", t->tid,
		 t->name[0] ? t->name : "-", elapsed ? (unsigned long)(used * 100 / elapsed) : 0,
		 (unsigned long)calls);
	if (nr < 0)
		host_log("its own code\n");
	else
		host_log("syscall %ld(%#lx, %#lx, %#lx)\n", nr, (unsigned long)t->args[0],
			 (unsigned long)t->args[1], (unsigned long)t->args[2]);
}

void gx_report_kernel_memory(void)
{
	static const char *const wanted[] = { "MemTotal", "MemFree", "Cached", "Unevictable", "Slab" };
	char text[2048], line[160] = "gx: kernel memory:";
	long free_kb = 0, cached_kb = 0;
	int fd = lkl_sys_open("/proc/meminfo", LKL_O_RDONLY, 0);
	long n = fd < 0 ? -1 : lkl_sys_read(fd, text, sizeof(text) - 1);

	if (fd >= 0)
		lkl_sys_close(fd);
	if (n <= 0)
		return;
	text[n] = 0;
	for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
		const char *at = text;
		size_t len = strlen(wanted[i]);

		/* The line that starts with the name: "Cached:      1234 kB". */
		while (at && (strncmp(at, wanted[i], len) || at[len] != ':')) {
			at = strchr(at, '\n');
			if (at)
				at++;
		}
		if (at) {
			long kb = 0;

			for (at += len + 1; *at == ' '; at++)
				;
			for (; *at >= '0' && *at <= '9'; at++)
				kb = kb * 10 + *at - '0';
			if (i == 1)
				free_kb = kb;
			else if (i == 2)
				cached_kb = kb;
			if (strlen(line) < sizeof(line) - 40) {
				char *end = line + strlen(line);
				char digits[16];
				int d = 0;

				*end++ = ' ';
				strcpy(end, wanted[i]);
				end += len;
				*end++ = ' ';
				kb >>= 10;
				do
					digits[d++] = '0' + kb % 10;
				while (kb /= 10);
				while (d)
					*end++ = digits[--d];
				strcpy(end, " MB,");
			}
		}
	}
	line[strlen(line) - 1] = '\n';
	host_log("%s", line);
	/* The kernel keeps what it read of files in its memory, all of it it
	 * can: a root image is read once into the guest's own memory and that
	 * copy serves nothing, but it leaves the kernel's memory in crumbs, and
	 * what needs a piece of some size (shared memory, a socket's buffer)
	 * is then refused. It is told to let go when little is left. */
	if (free_kb < (160 << 10) && cached_kb > (128 << 10)) {
		static int said;
		int drop = lkl_sys_open("/proc/sys/vm/drop_caches", LKL_O_WRONLY, 0);

		if (drop >= 0) {
			lkl_sys_write(drop, "1\n", 2);
			lkl_sys_close(drop);
		} else if (!said++) {
			host_log("gx: the kernel cannot be told to drop what it cached (%d)\n", drop);
		}
	}
	/* And how many tasks it has, its own threads and the guest's: each of
	 * its own is a thread of the host, which may have few to give. The
	 * fourth word of /proc/loadavg is "running/all". */
	fd = lkl_sys_open("/proc/loadavg", LKL_O_RDONLY, 0);
	n = fd < 0 ? -1 : lkl_sys_read(fd, text, sizeof(text) - 1);
	if (fd >= 0)
		lkl_sys_close(fd);
	if (n > 0) {
		const char *slash;

		text[n] = 0;
		slash = strchr(text, '/');
		if (slash)
			host_log("gx: kernel tasks: %ld\n", strtol(slash + 1, NULL, 10));
	}
}

void gx_report_threads(void)
{
	uint64_t now = host_now_ns(), elapsed = reported_at ? now - reported_at : 0;

	reported_at = now;
	host_log("gx: %lu syscalls so far, %d threads; the busy ones:\n",
		 (unsigned long)__atomic_load_n(&syscall_count, __ATOMIC_RELAXED), live_threads);
	if (gx_main_thread)
		report_thread(gx_main_thread, elapsed);
	for (int i = 0; i < GX_MAX_THREADS; i++)
		if (thread_used[i])
			report_thread(&threads[i], elapsed);
}

/*
 * How long each kind of syscall takes, for the ones that should not wait: a
 * syscall here goes through the kernel's one processor, and what a browser
 * does between two pictures is mostly made of them. Those that wait for
 * something (futex, poll, epoll, nanosleep, reads of pipes and sockets) are
 * counted but say nothing about cost.
 */
#define SYSCALL_KINDS 300
static uint64_t kind_calls[SYSCALL_KINDS], kind_ns[SYSCALL_KINDS];

void gx_report_syscall_cost(void)
{
	static uint64_t last_calls[SYSCALL_KINDS], last_ns[SYSCALL_KINDS];
	static uint64_t calls[SYSCALL_KINDS], ns[SYSCALL_KINDS];
	char line[400];
	int len;

	for (int k = 0; k < SYSCALL_KINDS; k++) {
		uint64_t c = kind_calls[k], t = kind_ns[k];

		calls[k] = c - last_calls[k];
		ns[k] = t - last_ns[k];
		last_calls[k] = c;
		last_ns[k] = t;
	}
	len = snprintf(line, sizeof(line), "gx: syscall time in 10 s (number: calls, ms):");
	for (int n = 0; n < 8; n++) {
		int best = -1;

		for (int k = 0; k < SYSCALL_KINDS; k++) {
			/* futex, epoll_pwait, ppoll, pselect, nanosleep, clock_nanosleep,
			 * read, recvfrom, recvmsg, sched_yield: they wait. */
			if (k == 98 || k == 22 || k == 73 || k == 72 || k == 101 || k == 115 ||
			    k == 63 || k == 207 || k == 212 || k == 124)
				continue;
			if (ns[k] >= 1000000 && (best < 0 || ns[k] > ns[best]))
				best = k;
		}
		if (best < 0)
			break;
		len += snprintf(line + len, sizeof(line) - len, " %d: %lu, %lu;", best,
				(unsigned long)calls[best], (unsigned long)(ns[best] / 1000000));
		ns[best] = 0;
		if (len >= (int)sizeof(line) - 40)
			break;
	}
	host_log("%s\n", line);
	/* And the most frequent, waiting or not. */
	len = snprintf(line, sizeof(line), "gx: syscalls in 10 s (number: calls):");
	for (int n = 0; n < 12; n++) {
		int best = -1;

		for (int k = 0; k < SYSCALL_KINDS; k++)
			if (calls[k] >= 100 && (best < 0 || calls[k] > calls[best]))
				best = k;
		if (best < 0)
			break;
		len += snprintf(line + len, sizeof(line) - len, " %d: %lu;", best,
				(unsigned long)calls[best]);
		calls[best] = 0;
		if (len >= (int)sizeof(line) - 40)
			break;
	}
	host_log("%s\n", line);
}

long gx_do_syscall(struct gx_frame *f)
{
	struct gx_thread *self = (struct gx_thread *)f->ctx;
	long ret;

	for (int i = 0; i < 4; i++)
		self->args[i] = f->x[i];
	self->nr = f->x[8];
	self->syscalls++;
	if (!self->host_id)
		self->host_id = host_thread_self();
	if (f->x[8] == 167 && f->x[0] == 15 /* prctl(PR_SET_NAME) */ && f->x[1]) {
		strncpy(self->name, (const char *)f->x[1], sizeof(self->name) - 1);
		self->name[sizeof(self->name) - 1] = 0;
	}
	__atomic_add_fetch(&syscall_count, 1, __ATOMIC_RELAXED);
	{
		uint64_t t0 = host_now_ns();
		unsigned nr = (unsigned)f->x[8];

		ret = dispatch(f, self);
		if (nr < SYSCALL_KINDS) {
			__atomic_add_fetch(&kind_calls[nr], 1, __ATOMIC_RELAXED);
			__atomic_add_fetch(&kind_ns[nr], host_now_ns() - t0, __ATOMIC_RELAXED);
		}
	}
	self->nr = -1;

	if (gx_trace) {
		if (f->x[8] == 56 || f->x[8] == 48 || f->x[8] == 78)
			host_log("[%d] sys %ld(%ld, \"%s\", %#lx) = %ld\n", self->tid,
				 (long)f->x[8], (long)f->x[0], (char *)f->x[1],
				 (unsigned long)f->x[2], ret);
		else
			host_log("[%d] sys %ld(%#lx, %#lx, %#lx, %#lx, %#lx, %#lx) = %#lx\n",
				 self->tid, (long)f->x[8], (unsigned long)f->x[0],
				 (unsigned long)f->x[1], (unsigned long)f->x[2],
				 (unsigned long)f->x[3], (unsigned long)f->x[4],
				 (unsigned long)f->x[5], (unsigned long)ret);
	}
	return ret;
}
