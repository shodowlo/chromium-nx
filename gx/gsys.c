/*
 * gsys.c - what a guest syscall becomes.
 *
 * Memory, threads, futexes, signals and processes are handled here, because
 * LKL has no MMU, no user processes and is built without futexes. Everything
 * else (files, pipes, sockets, time) goes to the kernel through lkl_syscall().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lkl.h>
#include "gx.h"

#define CLONE_VM 0x100
#define CLONE_FS 0x200
#define CLONE_FILES 0x400
#define CLONE_VFORK 0x4000
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
int gx_pool, gx_pool_cap = 64;
const char *gx_more_for, *gx_more[16];
int gx_more_count;
int gx_processes;
int gx_report_idle;

static int cpus = 1;

static int cpuinfo_made;
#define CPUINFO_PATH "/sys/devices/system/cpu/cpuinfo"

static void put_file(const char *path, const char *text)
{
	int fd = lkl_sys_open(path, LKL_O_WRONLY | LKL_O_CREAT, 0444);

	if (fd >= 0) {
		lkl_sys_write(fd, text, strlen(text));
		lkl_sys_close(fd);
	}
}

/* What /proc/cpuinfo says on arm64, which the kernel here says nothing of:
 * one paragraph per processor, with what the host's processor can do. */
static void make_cpuinfo(int count)
{
	static const char *const names[] = {
		"fp", "asimd", "evtstrm", "aes", "pmull", "sha1", "sha2", "crc32", "atomics", "fphp",
		"asimdhp", "cpuid", "asimdrdm", "jscvt", "fcma", "lrcpc", "dcpop", "sha3", "sm3", "sm4",
		"asimddp", "sha512", "sve", "asimdfhm", "dit", "uscat", "ilrcpc", "flagm", "ssbs", "sb",
		"paca", "pacg",
	};
	uint64_t caps = host_hwcap(0);
	char features[512] = "", text[1024];
	int fd = lkl_sys_open(CPUINFO_PATH, LKL_O_WRONLY | LKL_O_CREAT, 0444);

	if (fd < 0)
		return;
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (caps & (1UL << i)) {
			strcat(features, " ");
			strcat(features, names[i]);
		}
	}
	for (int i = 0; i < count; i++) {
		/* A Cortex-A57, which is what a Switch has. */
		int len = snprintf(text, sizeof(text),
				   "processor\t: %d\nBogoMIPS\t: 38.40\nFeatures\t:%s\n"
				   "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\n"
				   "CPU part\t: 0xd07\nCPU revision\t: 1\n\n",
				   i, features);

		lkl_sys_write(fd, text, len);
	}
	lkl_sys_close(fd);
	cpuinfo_made = 1;
}

/* Programs size their thread pools on this. The kernel has one processor of
 * its own, so what it shows in /sys is covered with what we want read there;
 * the threads are the host's to schedule either way. */
void gx_set_cpus(int count)
{
	static const char dir[] = "/sys/devices/system/cpu";
	static const char *const files[] = { "online", "possible", "present" };
	char path[96], range[16];
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
		/* Where each is and how fast it goes, which programs look up
		 * and do not all take a missing answer well. */
		strcpy(path + at, "/topology");
		lkl_sys_mkdir(path, 0555);
		strcpy(path + at, "/topology/physical_package_id");
		put_file(path, "0\n");
		strcpy(path + at, "/topology/core_id");
		snprintf(range, sizeof(range), "%d\n", i);
		put_file(path, range);
		strcpy(path + at, "/cpufreq");
		lkl_sys_mkdir(path, 0555);
		strcpy(path + at, "/cpufreq/cpuinfo_max_freq");
		put_file(path, "1020000\n");
		strcpy(path + at, "/cpufreq/cpuinfo_min_freq");
		put_file(path, "1020000\n");
		strcpy(path + at, "/cpufreq/scaling_cur_freq");
		put_file(path, "1020000\n");
	}
	make_cpuinfo(count);
}

static struct gx_thread threads[GX_MAX_THREADS];
static int thread_used[GX_MAX_THREADS];
static int next_tid = 101;
static int live_threads = 1;
static struct host_lock *big_lock;

#define SIGNALS 64
#define MAX_PROCESSES 64
#define FIRST_PID 100
#define MAX_RANGES 32768

enum { P_FREE, P_RUNNING, P_ENDING, P_ENDED };

/* A program that runs: see "processes" below. */
struct gx_process {
	int state;
	int pid, parent;
	int main_ktid;            /* what the kernel knows its first thread as: 0 if that one is gone */
	int status;               /* how it ended, as wait4() says it */
	int threads;
	int own_files;            /* its open files are not the first program's */
	int files_slot, files_id; /* what the kernel keeps them as, and a number no other files had */
	int files_lent;           /* they are its parent's, which goes on using them */
	int leaving;              /* its last thread is putting its files and memory away: not to be
				   * given to a new process before that is done */
	int has_break;            /* its memory is one that brk() grows */
	uint64_t brk_base, brk_cur;   /* or a break of its own making: see own_brk() */
	int real_std;             /* which of files 0-2 are the kernel's, a bit each */
	struct {
		uint64_t handler, flags, mask;   /* as rt_sigaction has them; handler 0 default, 1 ignore */
	} actions[SIGNALS + 1];
	char exe[256];            /* what /proc/self/exe points to */
	uint64_t aux[40];         /* what /proc/self/auxv holds */
	int aux_words;
	/* While it still runs in its parent's memory: the thread that made
	 * it, which waits, where that thread is looked up, and whether that
	 * memory is put back as it was afterwards (so that what it frees
	 * there is not to be freed). */
	struct gx_thread *held;
	int held_slot;
	int borrowed;
	/* Held still: a child of it runs in its memory and none of its
	 * threads is to see what the child does there. */
	volatile int frozen;
	/* The memory it has of its own, which is what a child may spoil
	 * (untracked: more pieces than are followed). */
	struct gx_range *ranges;
	int range_count, range_room, untracked;
};
static struct gx_process processes[MAX_PROCESSES];

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

static struct gx_thread *waiters;   /* the threads that wait on a futex */

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

/*
 * A guest thread and the thread of the host that runs it need not be the
 * same for ever. With gx_pool, a guest thread that waits on a futex, which is
 * most of the threads of a large program most of the time, lets go of its
 * host thread: what it was doing is all in the frame of its futex syscall
 * (tramp.S), and when it is woken any host thread can take it up again from
 * there. Host threads that have nothing to run wait in worker_loop(); one
 * more is made when there is something to run and none waits, up to
 * gx_pool_cap. A guest thread that waits in the kernel keeps its host thread.
 */
static struct gx_thread *run_first, *run_last;
static int run_length, pool_workers, pool_idle, clock_runs;
static struct host_cond *pool_cond, *clock_cond;

static void worker_loop(uint64_t base, int attached) __attribute__((noreturn));
static void worker_main(void *arg);
static void clock_main(void *arg);

/* With the lock held, as all of these. */
static void unlist(struct gx_thread *t)
{
	for (struct gx_thread **p = &waiters; *p; p = &(*p)->wait_next) {
		if (*p == t) {
			*p = t->wait_next;
			break;
		}
	}
	t->waiting = 0;
}

/* t, which has no host thread, is to go on, its syscall returning ret. */
static void make_runnable(struct gx_thread *t, long ret)
{
	t->resume_ret = ret;
	t->run_next = NULL;
	if (run_last)
		run_last->run_next = t;
	else
		run_first = t;
	run_last = t;
	run_length++;
	if (!pool_cond)
		pool_cond = host_cond_new();
	if (pool_idle) {
		host_cond_signal(pool_cond);
	} else if (pool_workers < gx_pool_cap) {
		pool_workers++;
		if (host_thread_start(worker_main, NULL))
			pool_workers--;   /* it waits for one that is busy now */
	}
}

/* Has t leave its wait, on a futex or on its own condition, to look at what
 * has changed for it (a signal, the end of its process). */
static void kick(struct gx_thread *t)
{
	if (gx_pool && t->waiting) {
		unlist(t);
		make_runnable(t, -GX_EINTR);
	} else if (t->cond) {
		host_cond_signal(t->cond);
	}
}

static long futex_wait(struct gx_thread *self, struct gx_frame *f, uint32_t *addr, uint32_t val,
		       int64_t ns, uint32_t bitset)
{
	long ret = 0;

	lock();
	if (__atomic_load_n(addr, __ATOMIC_SEQ_CST) != val) {
		unlock();
		return -GX_EAGAIN;
	}
	/* Waiters are known by where their word is, which for memory shared
	 * between programs is not the address each has it at. */
	self->wait_addr = (uint32_t *)gmem_same_place((uint64_t)addr);
	self->wait_bitset = bitset;
	self->wait_woken = 0;
	self->wait_deadline = 0;
	self->waiting = 1;
	self->wait_next = waiters;
	waiters = self;
	if (gx_pool) {
		if (ns >= 0) {
			self->wait_deadline = host_now_ns() + (uint64_t)ns + 1;
			if (!clock_cond)
				clock_cond = host_cond_new();
			if (!clock_runs) {
				clock_runs = 1;
				host_thread_start(clock_main, NULL);
			}
			host_cond_signal(clock_cond);
		}
		self->resume = f;
		self->resume_start = 0;
		worker_loop(self->host_sp, self->proc->files_id);
	}
	if (!self->cond)
		self->cond = host_cond_new();
	while (!self->wait_woken) {
		if (host_cond_wait(self->cond, big_lock, ns)) {
			ret = self->wait_woken ? 0 : -GX_ETIMEDOUT;
			break;
		}
		/* Woken for a signal (send_signal()): the wait is over, the
		 * guest waits again if it still has reason to. */
		if (!self->wait_woken && (self->dying ||
					  (__atomic_load_n(&self->pending, __ATOMIC_SEQ_CST) & ~self->blocked))) {
			ret = -GX_EINTR;
			break;
		}
	}
	unlist(self);
	unlock();
	return ret;
}

/* Wakes up to n waiters on addr and moves up to n2 of the others to addr2. */
static long futex_wake(uint32_t *addr, int n, uint32_t bitset, uint32_t *addr2, int n2)
{
	long count = 0;

	addr = (uint32_t *)gmem_same_place((uint64_t)addr);
	if (addr2)
		addr2 = (uint32_t *)gmem_same_place((uint64_t)addr2);
	lock();
	for (struct gx_thread *t = waiters, *next; t; t = next) {
		next = t->wait_next;
		if (t->wait_addr != addr || t->wait_woken || !(t->wait_bitset & bitset))
			continue;
		if (count < n) {
			t->wait_woken = 1;
			if (gx_pool) {
				unlist(t);
				make_runnable(t, 0);
			} else {
				host_cond_signal(t->cond);
			}
			count++;
		} else if (addr2 && n2 > 0) {
			t->wait_addr = addr2;
			n2--;
			count++;
		}
	}
	unlock();
	return count;
}

/* The one thread that watches the time for the waiters that have none. */
static void clock_main(void *arg)
{
	(void)arg;
	lock();
	for (;;) {
		uint64_t now = host_now_ns(), first = 0;

		for (struct gx_thread *t = waiters, *next; t; t = next) {
			next = t->wait_next;
			if (!t->wait_deadline)
				continue;
			if (t->wait_deadline <= now) {
				unlist(t);
				make_runnable(t, -GX_ETIMEDOUT);
			} else if (!first || t->wait_deadline < first) {
				first = t->wait_deadline;
			}
		}
		host_cond_wait(clock_cond, big_lock, first ? (int64_t)(first - now) : -1);
	}
}


/* ---------- signals ---------- */

/*
 * Signals a guest sends itself, from one of its threads to another: the C
 * library does (to have every thread change its user, to cancel one), and so
 * do programs. One is delivered when the thread it is for comes out of a
 * syscall: its handler is entered instead of the code that made the syscall,
 * on the same stack further down, and returns into two instructions of ours
 * that make the rt_sigreturn syscall, which goes back to that code. A thread
 * waiting on a futex is woken for it; one waiting in the kernel is not, and
 * gets it when it comes back. Nothing here comes from the kernel: its own
 * signals (a timer's) are not delivered. What each signal does is the
 * process's; a child's end is told its parent from here (SIGCHLD).
 */
#define SIG_BIT(sig) (1UL << ((sig) - 1))
#define SVC0_WORD 0xD4000001u
#define SA_NODEFER 0x40000000UL
#define SA_RESETHAND 0x80000000UL

static uint64_t sigreturn_code;

static void end_process(struct gx_process *p, int status);
static void do_exit_thread(struct gx_thread *t, int status);
static int claim_end(struct gx_process *p);
static void to_guest(struct gx_thread *self);

static struct gx_thread *thread_of(int tid)
{
	for (int i = 0; i < GX_MAX_THREADS; i++)
		if (thread_used[i] && threads[i].tid == tid)
			return &threads[i];
	return NULL;
}

static struct gx_process *process_of(int pid)
{
	for (int i = 0; i < MAX_PROCESSES; i++)
		if (processes[i].state != P_FREE && processes[i].pid == pid)
			return &processes[i];
	return NULL;
}

/* A thread of p for a signal sent to all of p: one that lets it in if there
 * is one, self if it does. */
static struct gx_thread *thread_for(struct gx_process *p, struct gx_thread *self, int sig)
{
	struct gx_thread *any = NULL;

	if (self && self->proc == p && !(self->blocked & (1UL << (sig - 1))))
		return self;
	for (int i = 0; i < GX_MAX_THREADS; i++) {
		if (!thread_used[i] || threads[i].proc != p || threads[i].dying)
			continue;
		if (!(threads[i].blocked & (1UL << (sig - 1))))
			return &threads[i];
		if (!any)
			any = &threads[i];
	}
	return any;
}

/* Enters the handler of a signal that waits for this thread, if there is
 * one: does not return then. f is the syscall the thread comes out of and
 * ret what it returns. */
static void deliver_signal(struct gx_frame *f, struct gx_thread *self, long ret)
{
	static const uint32_t code[] = { 0xD2801168u /* mov x8, #139 */, SVC0_WORD };
	uint64_t ready = __atomic_load_n(&self->pending, __ATOMIC_SEQ_CST) & ~self->blocked;
	struct gx_process *p = self->proc;
	uint64_t base, handler, flags;
	struct gx_frame *h;
	int sig;

	if (!ready || self->handling >= 4)
		return;
	sig = __builtin_ctzl(ready) + 1;
	__atomic_and_fetch(&self->pending, ~SIG_BIT(sig), __ATOMIC_SEQ_CST);
	handler = p->actions[sig].handler;
	flags = p->actions[sig].flags;
	if (handler <= 1)
		return;
	if (!sigreturn_code)
		sigreturn_code = gmem_code(code, 2);
	if (!sigreturn_code)
		return;
	/* Below the frame of the syscall: the handler's frame, then what a
	 * handler is told (who sent the signal, what was interrupted), of
	 * which we fill in little. */
	base = ((uint64_t)f - 8192) & ~15UL;
	memset((void *)base, 0, 8192);
	h = (struct gx_frame *)base;
	*h = *f;
	*(int32_t *)(base + 1024) = sig;          /* siginfo: si_signo */
	*(int32_t *)(base + 1024 + 8) = -6;       /* si_code: sent to a thread */
	*(int32_t *)(base + 1024 + 16) = p->pid;  /* si_pid */
	h->result = sig;      /* x0, which a frame has as what its syscall returned */
	h->x[1] = base + 1024;
	h->x[2] = base + 1280;
	h->x30 = sigreturn_code;
	h->exit = handler;
	f->result = ret;
	self->interrupted[self->handling] = f;
	self->interrupted_blocked[self->handling] = self->blocked;
	self->handling++;
	self->blocked |= p->actions[sig].mask | ((flags & SA_NODEFER) ? 0 : SIG_BIT(sig));
	if (flags & SA_RESETHAND)
		p->actions[sig].handler = 0;
	if (gx_trace)
		host_log("[%d] signal %d: handler at %#lx\n", self->tid, sig, (unsigned long)handler);
	gx_resume(h);
}

static long do_sigaction(struct gx_frame *f, struct gx_thread *self)
{
	struct gx_process *p = self->proc;
	long sig = (long)f->x[0];
	uint64_t *act = (uint64_t *)f->x[1], *old = (uint64_t *)f->x[2];

	if (sig < 1 || sig > SIGNALS)
		return -GX_EINVAL;
	if (old) {
		old[0] = p->actions[sig].handler;
		old[1] = p->actions[sig].flags;
		old[2] = p->actions[sig].mask;
	}
	if (act) {
		p->actions[sig].handler = act[0];
		p->actions[sig].flags = act[1];
		p->actions[sig].mask = act[2];
	}
	return 0;
}

static long do_sigprocmask(struct gx_frame *f, struct gx_thread *self)
{
	const uint64_t *set = (const uint64_t *)f->x[1];
	uint64_t *old = (uint64_t *)f->x[2], was = self->blocked;

	if (set) {
		if (f->x[0] == 0)
			self->blocked |= *set;
		else if (f->x[0] == 1)
			self->blocked &= ~*set;
		else if (f->x[0] == 2)
			self->blocked = *set;
		else
			return -GX_EINVAL;
	}
	if (old)
		*old = was;
	return 0;
}

/* Has thread `to` of p get a signal, from self (NULL if from no thread). */
static long post_signal(struct gx_process *p, struct gx_thread *to, int sig, struct gx_thread *self)
{
	uint64_t handler = p->actions[sig].handler;

	if (handler == 1)
		return 0;
	if (!handler) {
		/* What nothing handles ends the program, but for the few
		 * that are ignored then: a child's end, a window's size... */
		if (sig == 17 || sig == 18 || sig == 23 || sig == 28)
			return 0;
		if (p == &processes[0]) {
			host_log("gx: the guest raised signal %d\n", sig);
			host_exit(128 + sig);
		}
		if (gx_trace || gx_processes)
			host_log("gx: signal %d ends process %d (sent by thread %d)\n", sig, p->pid,
				 self ? self->tid : 0);
		if (claim_end(p))
			end_process(p, sig);
		return 0;
	}
	__atomic_or_fetch(&to->pending, SIG_BIT(sig), __ATOMIC_SEQ_CST);
	if (to != self) {
		/* Out of its sleep, if it sleeps on a futex. */
		lock();
		kick(to);
		unlock();
	}
	return 0;
}

/* kill, tkill and tgkill. Signal 0 is no signal: it asks whether what it
 * would be sent to is there (a program looks for another copy of itself
 * that way). */
static long send_signal(struct gx_frame *f, struct gx_thread *self, long nr)
{
	long sig = nr == 131 ? (long)f->x[2] : (long)f->x[1];
	struct gx_process *p;
	struct gx_thread *to;

	if (sig < 0 || sig > SIGNALS)
		return -GX_EINVAL;
	if (nr == 129) {
		long pid = (long)f->x[0];

		/* A group of processes is its first one here. */
		p = pid == 0 || pid == -1 ? self->proc : process_of(pid < 0 ? -pid : pid);
		if (!p || p->state == P_FREE)
			return -3;   /* ESRCH */
		to = sig ? thread_for(p, self, sig) : NULL;
		if (!sig || !to)
			return 0;
	} else {
		to = thread_of((int)(nr == 131 ? f->x[1] : f->x[0]));
		if (!to)
			return sig ? -3 : 0;
		if (!sig)
			return 0;
		p = to->proc;
	}
	return post_signal(p, to, sig, self);
}

static long do_sigreturn(struct gx_thread *self)
{
	struct gx_frame *back;

	if (!self->handling)
		return -GX_EINVAL;
	self->handling--;
	self->blocked = self->interrupted_blocked[self->handling];
	back = self->interrupted[self->handling];
	to_guest(self);
	/* Another one may have come meanwhile. */
	deliver_signal(back, self, (long)back->result);
	gx_resume(back);
}

static long do_futex(struct gx_frame *f, struct gx_thread *self)
{
	uint32_t *addr = (uint32_t *)f->x[0];
	int op = (int)f->x[1], cmd = op & 0x7f;
	uint32_t val = (uint32_t)f->x[2];
	int clock = (op & FUTEX_CLOCK_REALTIME) ? 0 : 1;

	switch (cmd) {
	case FUTEX_WAIT:
		return futex_wait(self, f, addr, val, timeout_ns((void *)f->x[3], 0, clock), ~0u);
	case FUTEX_WAIT_BITSET:
		return futex_wait(self, f, addr, val, timeout_ns((void *)f->x[3], 1, clock),
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

/* ---------- threads and processes ---------- */

/*
 * A guest that starts another program (fork and exec, posix_spawn) gets a
 * process as far as it can tell: its own number, its own open files and
 * directory, its own signal handlers, an end its parent waits for. What it
 * does not get is memory of its own: there is one address space. So:
 *
 * - a new process first runs in its parent's memory, on a thread of its own,
 *   while the thread that made it waits: until it runs a program (execve) or
 *   ends. That is what vfork() promises and what posix_spawn() asks for; for
 *   a fork() it holds as long as the child does what children nearly always
 *   do, which is to arrange its files and run a program. A child changes
 *   what it likes on its way there, which must not be changed for the
 *   parent. If the parent has one thread, all of its memory is put back as
 *   it was when the child is done with it (a shell's child does a shell's
 *   work before it runs anything). With more threads that cannot be, they
 *   use the memory meanwhile: there, where the host can do it, the other
 *   threads are held still (freeze(): a browser's child wipes a table its
 *   parent would stop on at once), and the pages the child writes to are
 *   noticed one by one and put back (gmem_watch()). Where the host cannot,
 *   the stack is all that is put back. A child that goes on for good as a
 *   copy of its parent is not something that can be had;
 * - execve loads the program next to the others and enters it. What the
 *   process had in memory before is left there (it may be its parent's);
 * - the kernel knows none of this: to it each guest thread is a task, all
 *   with the same files. A new process unshares its files and directories,
 *   and its later threads take those of the thread that makes them
 *   (they are kept by number in the kernel, our addition to it: lkl-gx.patch).
 *
 * One kind of child does run beside its parent, which talks to it meanwhile:
 * the one a program makes to have itself looked at when it has gone wrong (a
 * crash reporter's: clone() with a stack, sharing the files and not, it
 * believes, the memory). It is given what it asks for but the memory, and
 * told from its parent, whose TPIDR_EL0 it has, by the stack it is on
 * (beside()).
 *
 * The C library's fork() does more in the child than can be allowed in memory
 * the parent still uses (it forgets every other thread), unless all of it is
 * undone: guest/fork.c, loaded ahead of programs, replaces it by the bare
 * syscall where it is not (GX_SYS_FORK_KIND says).
 */
#define GX_FILES_SAVE 0x47580001L   /* the kernel's side of these: lkl-gx.patch */
#define GX_FILES_USE 0x47580002L
#define GX_FILES_DROP 0x47580003L
#define FORK_STACK (256UL << 10)   /* how much of the parent's stack a child may spoil */
#define WNOHANG 1
#define WNOWAIT 0x01000000

/* The memory a process has of its own: see struct gx_process. With the lock
 * held, or where one thread is all there is. */
static void remove_range(struct gx_process *p, uint64_t lo, uint64_t hi);

static void add_range(struct gx_process *p, uint64_t lo, uint64_t hi)
{
	if (p->untracked || lo >= hi)
		return;
	/* Once only: a program maps over what it has (a loader puts each part
	 * of a library in room it took first), and what is noted twice is
	 * given back twice when the process ends, the second time from under
	 * whoever was given the place in between. */
	remove_range(p, lo, hi);
	for (int i = 0; i < p->range_count; i++) {
		if (p->ranges[i].hi == lo) {
			p->ranges[i].hi = hi;
			return;
		}
		if (p->ranges[i].lo == hi) {
			p->ranges[i].lo = lo;
			return;
		}
	}
	if (p->range_count == p->range_room) {
		int room = p->range_room ? p->range_room * 2 : 64;
		struct gx_range *ranges = room > MAX_RANGES ? NULL : realloc(p->ranges, room * sizeof(*ranges));

		if (!ranges) {
			p->untracked = 1;
			return;
		}
		p->ranges = ranges;
		p->range_room = room;
	}
	p->ranges[p->range_count].lo = lo;
	p->ranges[p->range_count].hi = hi;
	p->range_count++;
}

static void remove_range(struct gx_process *p, uint64_t lo, uint64_t hi)
{
	for (int i = 0; i < p->range_count && !p->untracked; i++) {
		uint64_t rlo = p->ranges[i].lo, rhi = p->ranges[i].hi;

		if (rhi <= lo || rlo >= hi)
			continue;
		if (rlo >= lo && rhi <= hi) {
			p->ranges[i] = p->ranges[--p->range_count];
			i--;
		} else if (rlo < lo && rhi > hi) {
			p->ranges[i].hi = lo;
			add_range(p, hi, rhi);
		} else if (rlo < lo) {
			p->ranges[i].hi = lo;
		} else {
			p->ranges[i].lo = hi;
		}
	}
}

/* Whose the memory is that p maps: its own, but for a child that still runs
 * in its parent's memory with nothing to set it apart (a vfork, or a fork
 * where the host cannot undo what the child does). What such a child maps is
 * there for its parent too, whose C library it is using, and stays the
 * parent's when the child goes on to its own program. */
static struct gx_process *memory_owner(struct gx_process *p)
{
	return p->held && !p->borrowed && p->held->proc ? p->held->proc : p;
}

/* Gives back the memory a process has of its own: at its end, and when it
 * becomes another program. Not while any of it may be in use: by the last of
 * its threads only. */
static void free_ranges(struct gx_process *p)
{
	struct gx_range *ranges;
	int count;

	lock();
	ranges = p->ranges;
	count = p->untracked ? 0 : p->range_count;
	p->ranges = NULL;
	p->range_count = p->range_room = 0;
	unlock();
	for (int i = 0; i < count; i++) {
		gmem_note("the memory of a process that ended or became another", ranges[i].lo, ranges[i].hi, p->pid);
		gmem_munmap(ranges[i].lo, ranges[i].hi - ranges[i].lo);
	}
	free(ranges);
}

/* What a process had in memory when it made a child. */
struct snapshot {
	int count;
	uint8_t *data;
	struct { uint64_t addr, len; } run[1024];
};

static int snapshot_runs(struct snapshot *s, uint64_t lo, uint64_t hi)
{
	while (lo < hi) {
		uint64_t n = gmem_writable(lo, hi - lo);

		if (!n) {
			lo += GX_PAGE;
			continue;
		}
		if (s->count && s->run[s->count - 1].addr + s->run[s->count - 1].len == lo) {
			s->run[s->count - 1].len += n;
		} else {
			if (s->count == (int)(sizeof(s->run) / sizeof(s->run[0])))
				return -1;
			s->run[s->count].addr = lo;
			s->run[s->count].len = n;
			s->count++;
		}
		lo += n;
	}
	return 0;
}

/* All that p can write to, but the part of its stack under `from`, which it
 * does not use: NULL if that cannot be had. */
static struct snapshot *take_snapshot(struct gx_process *p, uint64_t from)
{
	struct snapshot *s = calloc(1, sizeof(*s));
	uint64_t total = 0, at = 0, lo, hi;
	int failed = !s;

	for (int i = 0; i < p->range_count && !failed; i++) {
		lo = p->ranges[i].lo;
		hi = p->ranges[i].hi;
		if (from >= lo && from < hi)
			lo = GX_PAGE_DOWN(from);
		failed = snapshot_runs(s, lo, hi);
	}
	if (!failed && p->has_break) {
		gmem_break(&lo, &hi);
		failed = snapshot_runs(s, lo, hi);
	}
	for (int i = 0; !failed && i < s->count; i++)
		total += s->run[i].len;
	if (!failed)
		s->data = malloc(total ? total : 1);
	if (failed || !s->data) {
		free(s);
		return NULL;
	}
	for (int i = 0; i < s->count; i++) {
		memcpy(s->data + at, (void *)s->run[i].addr, s->run[i].len);
		at += s->run[i].len;
	}
	return s;
}

static void restore_snapshot(struct snapshot *s)
{
	uint64_t at = 0;

	for (int i = 0; i < s->count; i++) {
		uint64_t addr = s->run[i].addr, end = addr + s->run[i].len;

		/* Whatever of it is still there. */
		while (addr < end) {
			uint64_t n = gmem_writable(addr, end - addr);

			if (n)
				memcpy((void *)addr, s->data + at + (addr - s->run[i].addr), n);
			addr += n ? n : GX_PAGE;
		}
		at += s->run[i].len;
	}
	free(s->data);
	free(s);
}

/* Waits, in a syscall, for its process not to be held still any more. */
static void park(struct gx_thread *self)
{
	struct gx_process *p = self->proc;

	if (!self->cond)
		self->cond = host_cond_new();
	lock();
	self->parked = 1;
	while (p->frozen)
		host_cond_wait(self->cond, big_lock, -1);
	self->parked = 0;
	unlock();
}

/* For a thread on its way back to the guest's code. */
static void to_guest(struct gx_thread *self)
{
	__atomic_store_n(&self->in_host, 0, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&self->proc->frozen, __ATOMIC_SEQ_CST))
		park(self);
}

/* Before a syscall that said it would be quiet writes to the guest's memory
 * (see freeze()). */
static void before_output(struct gx_thread *self)
{
	__atomic_store_n(&self->quiet, 0, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&self->proc->frozen, __ATOMIC_SEQ_CST))
		park(self);
	if (self->dying)
		do_exit_thread(self, 0);   /* its process is over: nothing to tell it */
}

/* Whether syscall nr, as dispatch() carries it out, writes nothing to the
 * guest's memory before it calls before_output(): the ones that wait, which
 * are made to (they read into memory of ours first), and the ones that have
 * nothing to write. */
static int is_quiet(long nr)
{
	switch (nr) {
	case 22: case 73: case 63: case 207: case 212: case 202: case 242:   /* see dispatch() */
	case 64: case 66: case 206: case 211: case 203:    /* write, writev, sendto, sendmsg, connect */
	case 57: case 82: case 83: case 32:                /* close, fsync, fdatasync, flock */
	case 98: case 101: case 115: case 124:             /* futex, nanosleep, clock_nanosleep, sched_yield */
	case 260: case 95: case 220: case 93: case 94:     /* wait4, waitid, clone, exit, exit_group */
	case 129: case 130: case 131:                      /* kill, tkill, tgkill */
		return 1;
	}
	return 0;
}

/* Holds every thread of p still but self: when this returns each of them is
 * stopped by the host in the middle of the guest's code, or in a syscall,
 * where it waits when it is done, and one that will not write to the
 * guest's memory before it has waited. None runs the guest's code, and none
 * writes to its memory, until thaw(): what the child finds there is what
 * its parent had, and what is put back afterwards is put back over nothing
 * new. (A syscall of another kind is given the time to finish; one that
 * takes its time all the same is let be, for want of better.) */
static void freeze(struct gx_process *p, struct gx_thread *self)
{
	uint64_t start = host_now_ns();
	int waiting;

	lock();
	__atomic_store_n(&p->frozen, 1, __ATOMIC_SEQ_CST);
	unlock();
	do {
		int patient = host_now_ns() - start < 300000000ULL;

		waiting = 0;
		lock();
		for (int i = 0; i < GX_MAX_THREADS; i++) {
			struct gx_thread *t = &threads[i];

			if (!thread_used[i] || t->proc != p || t == self || t->stopped || t->parked)
				continue;
			if (__atomic_load_n(&t->in_host, __ATOMIC_SEQ_CST)) {
				if (!__atomic_load_n(&t->quiet, __ATOMIC_SEQ_CST) && patient)
					waiting = 1;
				continue;
			}
			if (t->host_id && host_thread_stop(t->host_id))
				t->stopped = 1;
			else
				waiting = 1;
		}
		unlock();
		if (waiting)
			host_yield();
	} while (waiting);
}

static void thaw(struct gx_process *p)
{
	lock();
	__atomic_store_n(&p->frozen, 0, __ATOMIC_SEQ_CST);
	for (int i = 0; i < GX_MAX_THREADS; i++) {
		if (thread_used[i] && threads[i].proc == p) {
			threads[i].stopped = 0;
			if (threads[i].cond)
				host_cond_signal(threads[i].cond);
		}
	}
	unlock();
	host_threads_go();
}

static long kernel_tid(void)
{
	long params[6] = { 0 };

	return lkl_syscall(__lkl__NR_gettid, params);
}

static void close_all_files(void)
{
	long params[6] = { 0, ~0U, 0 };

	lkl_syscall(__lkl__NR_close_range, params);
}

static int next_files_id = 1;

static void files(long what, long n)
{
	long params[6] = { n, what };
	long ret = lkl_syscall(__lkl__NR_setns, params);

	if (ret) {
		static int said;

		if (!said++)
			host_log("gx: the kernel does not keep files apart (%ld): is it built with lkl-gx.patch?\n", ret);
	}
}

/* What a guest thread needs done by the host thread that runs it first. */
static void begin_thread(struct gx_thread *t)
{
	struct gx_process *p = t->proc;

	if (t->forked) {
		/* A new process: a copy of its parent's files, and of where
		 * its parent was among the directories. */
		long share[6] = { CLONE_FILES | CLONE_FS };

		files(GX_FILES_USE, t->join);
		lkl_syscall(__lkl__NR_unshare, share);
		files(GX_FILES_SAVE, p->files_slot);
	} else if (p->files_slot || gx_pool) {
		files(GX_FILES_USE, p->files_slot);
	}
	t->kernel_tid = kernel_tid();
	if (t->leader)
		p->main_ktid = t->kernel_tid;
	t->host_id = host_thread_self();
	if (t->watched)
		gmem_watch_child(t->host_id);
	if (t->set_tid)
		*t->set_tid = t->tid;   /* the child's write, undone with the rest */
}

static void thread_main(void *arg)
{
	struct gx_thread *t = arg;

	begin_thread(t);
	gx_enter_frame(t->start, t);
}

/* Starts a new guest thread on a host thread: 0, or -1 if there is none to
 * be had. */
static int launch(struct gx_thread *t)
{
	if (!gx_pool)
		return host_thread_start(thread_main, t);
	lock();
	t->resume_start = 1;
	make_runnable(t, 0);
	unlock();
	return 0;
}

/* A host thread takes up guest thread t: `base` is where the host thread's
 * stack is free from, and `attached` the files its kernel task has now. */
static void run_thread(struct gx_thread *t, uint64_t base, int attached)
{
	struct gx_frame *f = t->resume;
	uint64_t tp;

	__asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
	t->host_tp = tp;
	t->host_sp = base;
	if (t->resume_start) {
		t->resume_start = 0;
		begin_thread(t);
		gx_resume(t->start);
	}
	if (t->proc->files_id != attached)
		files(GX_FILES_USE, t->proc->files_slot);
	t->host_id = host_thread_self();
	f->result = t->resume_ret;
	t->nr = -1;
	__atomic_store_n(&t->quiet, 0, __ATOMIC_SEQ_CST);
	if (t->dying)
		do_exit_thread(t, 0);
	if (gx_trace)
		host_log("[%d] sys %ld(%#lx, ...) = %#lx, after a wait\n", t->tid, (long)f->x[8],
			 (unsigned long)f->x[0], (unsigned long)t->resume_ret);
	to_guest(t);
	deliver_signal(f, t, t->resume_ret);
	gx_resume(f);
}

/* What a host thread does between guest threads: with the lock held. */
static void worker_loop(uint64_t base, int attached)
{
	struct gx_thread *t;

	if (!pool_cond)
		pool_cond = host_cond_new();
	while (!(t = run_first)) {
		pool_idle++;
		host_cond_wait(pool_cond, big_lock, -1);
		pool_idle--;
	}
	run_first = t->run_next;
	if (!run_first)
		run_last = NULL;
	run_length--;
	unlock();
	run_thread(t, base, attached);
	for (;;)
		;   /* not reached */
}

static void worker_main(void *arg)
{
	uint64_t base = ((uint64_t)__builtin_frame_address(0) - 512) & ~15UL;

	(void)arg;
	lock();
	worker_loop(base, -1);
}

/* With the lock held. */
static struct gx_thread *new_thread(struct gx_process *p)
{
	for (int i = 0; i < GX_MAX_THREADS; i++) {
		if (!thread_used[i]) {
			struct gx_thread *t = &threads[i];
			struct host_cond *cond = t->cond;
			uint8_t *bounce = t->bounce;

			memset(t, 0, sizeof(*t));
			thread_used[i] = 1;
			t->cond = cond;
			t->bounce = bounce;
			t->nr = -1;
			t->tid = next_tid++;
			t->proc = p;
			live_threads++;
			return t;
		}
	}
	return NULL;
}

/* With the lock held. */
static void forget_thread(struct gx_thread *t)
{
	int slot = t - threads;

	gx_tp_tab[slot].guest_tp = ~0UL;
	thread_used[slot] = 0;
	live_threads--;
}

struct gx_thread *gx_thread_at(uint64_t tp, uint64_t sp)
{
	/* A child that runs beside its parent, with its TPIDR_EL0 and on a
	 * stack of its own; or that parent. */
	for (int i = 0; i < GX_MAX_THREADS; i++) {
		struct gx_thread *t = &threads[i];

		if (thread_used[i] && t->shadow_of && sp >= t->stack_lo && sp < t->stack_hi)
			return t;
	}
	for (int i = 0; i < GX_MAX_THREADS; i++)
		if (gx_tp_tab[i].guest_tp == tp && gx_tp_tab[i].thread)
			return gx_tp_tab[i].thread;
	/* The first thread of a program is the one to set its TPIDR_EL0 by
	 * itself, early, on the stack it was started with. */
	for (int i = 0; i < GX_MAX_THREADS; i++) {
		struct gx_thread *t = &threads[i];

		if (thread_used[i] && !t->shadow_of && sp >= t->stack_lo && sp < t->stack_hi) {
			gx_tp_tab[i].thread = t;
			gx_tp_tab[i].guest_tp = tp;
			return t;
		}
	}
	return gx_main_thread;
}

void gx_start_first(const char *path, const struct gx_start *start)
{
	struct gx_process *p = &processes[0];
	struct gx_thread *t = &threads[0];

	p->state = P_RUNNING;
	p->pid = FIRST_PID;
	p->parent = 1;
	p->threads = 1;
	p->has_break = 1;
	p->main_ktid = t->kernel_tid = kernel_tid();
	files(GX_FILES_SAVE, 0);
	memcpy(p->aux, start->aux, sizeof(p->aux));
	p->aux_words = start->aux_words;
	add_range(p, start->loaded[0][0], start->loaded[0][1]);
	add_range(p, start->loaded[1][0], start->loaded[1][1]);
	add_range(p, start->stack_lo, start->stack_hi);
	strncpy(p->exe, path, sizeof(p->exe) - 1);
	thread_used[0] = 1;
	t->tid = FIRST_PID;
	t->nr = -1;
	t->proc = p;
	t->stack_lo = start->stack_lo;
	t->stack_hi = start->stack_hi;
	/* Until it has a TPIDR_EL0 of its own it is known by one no other
	 * thread can have. */
	start->frame->gtp = (uint64_t)t;
	gx_tp_tab[0].thread = t;
	gx_tp_tab[0].guest_tp = (uint64_t)t;
	gx_main_thread = t;
	gx_enter_frame(start->frame, t);
}

/* Lets the thread that made p go on: p is out of its memory. */
static void release_held(struct gx_process *p)
{
	lock();
	if (p->held) {
		__atomic_store_n(&gx_tp_tab[p->held_slot].thread, p->held, __ATOMIC_SEQ_CST);
		host_cond_signal(p->held->cond);
		p->held = NULL;
	}
	unlock();
}

/* A child that runs beside its parent (see above). */
static long beside(struct gx_frame *f, struct gx_thread *self)
{
	uint64_t stack = f->x[1] & ~15UL;
	struct gx_process *parent = self->proc, *p = NULL;
	struct gx_thread *t = NULL;
	struct gx_frame *child;
	int pid;

	lock();
	for (int i = 1; i < MAX_PROCESSES && !p; i++)
		if (processes[i].state == P_FREE && !processes[i].threads && !processes[i].leaving)
			p = &processes[i];
	if (p)
		t = new_thread(p);
	if (!t) {
		unlock();
		return -GX_EAGAIN;
	}
	*p = *parent;   /* its files too: the same ones */
	p->state = P_RUNNING;
	p->pid = pid = t->tid;
	p->parent = parent->pid;
	p->status = 0;
	p->threads = 1;
	p->held = NULL;
	p->borrowed = 0;
	p->ranges = NULL;
	p->range_count = p->range_room = 0;
	p->untracked = 1;       /* what it has is its parent's */
	p->files_lent = 1;
	p->frozen = 0;
	p->main_ktid = 0;
	p->has_break = 0;
	unlock();
	t->blocked = self->blocked;
	t->leader = 1;
	t->shadow_of = self;
	t->stack_hi = stack;
	t->stack_lo = stack - (stack > (1UL << 20) ? (1UL << 20) : stack);
	memcpy(t->name, self->name, sizeof(t->name));
	memcpy((void *)(stack - 16), (uint8_t *)f + GX_FRAME_SIZE, 16);
	child = (struct gx_frame *)(stack - 16 - GX_FRAME_SIZE);
	memcpy(child, f, GX_FRAME_SIZE);
	child->result = 0;
	t->start = child;
	if (gx_trace || gx_processes)
		host_log("[%d] process %d starts beside %d (clone %#lx)\n", self->tid, pid, parent->pid,
			 (unsigned long)f->x[0]);
	__atomic_add_fetch(&self->shadowed, 1, __ATOMIC_SEQ_CST);
	if (launch(t)) {
		__atomic_sub_fetch(&self->shadowed, 1, __ATOMIC_SEQ_CST);
		lock();
		forget_thread(t);
		p->threads = 0;
		p->state = P_FREE;
		unlock();
		return -GX_EAGAIN;
	}
	return pid;
}

/* A new process: fork, vfork, and what posix_spawn asks for (memory shared,
 * a stack given, the parent waiting). */
static long do_fork(struct gx_frame *f, struct gx_thread *self)
{
	uint64_t flags = f->x[0], stack = f->x[1];
	static int turn_taken;
	int *ptid = (int *)f->x[2], *ctid = (int *)f->x[4];
	struct gx_process *parent = self->proc, *p = NULL;
	struct gx_thread *t = NULL;
	struct gx_frame *child = f;
	struct snapshot *all = NULL;
	size_t keep = 0;
	void *saved = NULL;
	int pid, slot = -1, watching = 0, still = 0;

	if (!self->cond)
		self->cond = host_cond_new();
	/* vfork() gives the stack its caller is on. */
	stack &= ~15UL;
	if (stack - 16 - GX_FRAME_SIZE == (uint64_t)f)
		stack = 0;
	if (stack && (flags & CLONE_FILES) && !(flags & (CLONE_VM | CLONE_VFORK)))
		return beside(f, self);
	if (!stack) {
		/* The child runs where its parent is, down to this very
		 * frame: what it may write over is kept, as far up as the
		 * stack of this thread goes and no further (what is above is
		 * somebody else's, who goes on using it). A thread's stack
		 * ends where its TPIDR_EL0 points. */
		uint64_t at = (uint64_t)f, limit = GX_FRAME_SIZE + 16;

		if (at >= self->stack_lo && at < self->stack_hi)
			limit = self->stack_hi - at;
		else if (f->gtp > at && f->gtp - at < (64UL << 20))
			limit = f->gtp - at;
		keep = (flags & CLONE_VM) ? 0 : gmem_writable(at, FORK_STACK);
		if (keep > limit)
			keep = limit;
		if (keep < GX_FRAME_SIZE + 16)
			keep = GX_FRAME_SIZE + 16;
		saved = malloc(keep);
		if (!saved)
			return -GX_ENOMEM;
	}
	if (!(flags & CLONE_VM) && host_write_faults()) {
		/* One child at a time in memory that is watched. */
		lock();
		while (turn_taken)
			host_cond_wait(self->cond, big_lock, 1000000);
		turn_taken = still = 1;
		unlock();
		if (parent->threads > 1)
			freeze(parent, self);
	}
	/* The child's first frame: the parent's, with the child's answer. All
	 * this thread writes there is written before the memory is watched. */
	if (stack) {
		memcpy((void *)(stack - 16), (uint8_t *)f + GX_FRAME_SIZE, 16);
		child = (struct gx_frame *)(stack - 16 - GX_FRAME_SIZE);
		memcpy(child, f, GX_FRAME_SIZE);
	}
	child->result = 0;
	if (saved)
		memcpy(saved, f, keep);
	lock();
	if (!(flags & CLONE_VM) && !parent->untracked) {
		if (parent->threads == 1)
			all = take_snapshot(parent, (uint64_t)f);
		if (!all)
			watching = !gmem_watch(parent->ranges, parent->range_count, parent->has_break);
	}
	if (still && !all && !watching)
		host_log("gx: process %d makes a child and cannot be kept from what the child does\n",
			 parent->pid);
	for (int i = 1; i < MAX_PROCESSES && !p; i++)
		if (processes[i].state == P_FREE && !processes[i].threads && !processes[i].leaving)
			p = &processes[i];
	for (int i = 0; i < GX_MAX_THREADS; i++)
		if (gx_tp_tab[i].guest_tp == f->gtp && gx_tp_tab[i].thread == self)
			slot = i;
	if (p && slot >= 0)
		t = new_thread(p);
	if (!t) {
		unlock();
		free(saved);
		if (all)
			restore_snapshot(all);
		if (watching)
			gmem_unwatch();
		if (still) {
			thaw(parent);
			turn_taken = 0;
		}
		host_log("gx: no room for one more process\n");
		return -GX_EAGAIN;
	}
	*p = *parent;   /* what its signals do, its files 0-2, its name */
	p->state = P_RUNNING;
	p->pid = pid = t->tid;
	p->parent = parent->pid;
	p->status = 0;
	p->threads = 1;
	p->own_files = 1;
	p->files_slot = p - processes;
	p->files_id = next_files_id++;
	p->held = self;
	p->held_slot = slot;
	p->borrowed = all || watching;
	p->ranges = NULL;
	p->range_count = p->range_room = 0;
	p->frozen = 0;
	p->main_ktid = 0;
	p->files_lent = 0;
	t->watched = watching;
	if ((all || watching) && (flags & CLONE_CHILD_SETTID))
		t->set_tid = ctid;
	unlock();
	t->blocked = self->blocked;
	t->join = parent->files_slot;
	t->forked = 1;
	t->leader = 1;
	memcpy(t->name, self->name, sizeof(t->name));
	t->start = child;
	if (gx_trace || gx_processes)
		host_log("[%d] process %d starts in the memory of %d (clone %#lx; %s)\n", self->tid, pid,
			 parent->pid, (unsigned long)flags,
			 all ? "all of it kept" : watching ? "its writes watched" : "the stack kept");
	/* The child has its parent's TPIDR_EL0: its syscalls are the child's
	 * for as long as the parent waits. */
	__atomic_store_n(&gx_tp_tab[slot].thread, t, __ATOMIC_SEQ_CST);
	if (launch(t)) {
		__atomic_store_n(&gx_tp_tab[slot].thread, self, __ATOMIC_SEQ_CST);
		lock();
		forget_thread(t);
		p->threads = 0;
		p->state = P_FREE;
		unlock();
		if (all)
			restore_snapshot(all);
		if (watching)
			gmem_unwatch();
		if (saved && !all && !watching)
			memcpy(f, saved, keep);
		if (still) {
			thaw(parent);
			turn_taken = 0;
		}
		free(saved);
		return -GX_EAGAIN;
	}
	lock();
	while (p->held == self && p->pid == pid)
		host_cond_wait(self->cond, big_lock, -1);
	unlock();
	if (all)
		restore_snapshot(all);
	if (watching)
		gmem_unwatch();
	/* Where nothing better was to be had, the stack at least. */
	if (saved && !all && !watching)
		memcpy(f, saved, keep);
	if (still) {
		thaw(parent);
		turn_taken = 0;
	}
	free(saved);
	if (flags & CLONE_PARENT_SETTID)
		*ptid = pid;
	return pid;
}

static long do_clone(struct gx_frame *f, struct gx_thread *self)
{
	uint64_t flags = f->x[0], stack = f->x[1], tls = f->x[3];
	int *ptid = (int *)f->x[2], *ctid = (int *)f->x[4];
	struct gx_process *p = self->proc;
	struct gx_thread *t;
	struct gx_frame *child;
	int slot, tid;

	if (!(flags & CLONE_THREAD))
		return do_fork(f, self);
	if (!(flags & CLONE_VM) || !stack) {
		host_log("gx: clone(%#lx) is of a kind that is not supported\n", (unsigned long)flags);
		return -GX_ENOSYS;
	}
	if (!self->cond)
		self->cond = host_cond_new();
	lock();
	/* No new thread in a process that is held still. */
	self->parked = 1;
	while (p->frozen)
		host_cond_wait(self->cond, big_lock, -1);
	self->parked = 0;
	t = new_thread(p);
	if (!t) {
		unlock();
		return -GX_EAGAIN;
	}
	p->threads++;
	unlock();
	slot = t - threads;
	tid = t->tid;
	t->blocked = self->blocked;

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
		*ptid = tid;
	if (flags & CLONE_CHILD_SETTID)
		*ctid = tid;
	if (flags & CLONE_CHILD_CLEARTID)
		t->clear_tid = ctid;
	gx_tp_tab[slot].thread = t;
	__atomic_store_n(&gx_tp_tab[slot].guest_tp, child->gtp, __ATOMIC_SEQ_CST);

	if (launch(t)) {
		lock();
		forget_thread(t);
		p->threads--;
		unlock();
		return -GX_EAGAIN;
	}
	return tid;
}

/* Whether p is still running and is now the caller's to end. */
static int claim_end(struct gx_process *p)
{
	int ours;

	lock();
	ours = p->state == P_RUNNING;
	if (ours)
		p->state = P_ENDING;
	unlock();
	return ours;
}

/* The end of a process that was claimed: its threads are told to stop (each
 * does at its next syscall, or out of its sleep on a futex; one that waits in
 * the kernel stays there until the kernel lets it go), and its parent that
 * it has a child to wait for. status is as wait4() gives it. */
static void end_process(struct gx_process *p, int status)
{
	struct gx_process *parent;

	if (p == &processes[0]) {
		host_log("gx: the first program ends (%#x)\n", status);
		if (gx_trace) {
			gmem_report();
			gx_report_kernel_memory();
		}
		host_exit((status & 0x7f) ? 128 + (status & 0x7f) : (status >> 8) & 0xff);
	}
	lock();
	p->status = status;
	for (int i = 0; i < GX_MAX_THREADS; i++) {
		if (thread_used[i] && threads[i].proc == p) {
			threads[i].dying = 1;
			kick(&threads[i]);
		}
	}
	/* Its own children have nobody to wait for them any more. */
	for (int i = 1; i < MAX_PROCESSES; i++) {
		struct gx_process *q = &processes[i];

		if (q->state != P_FREE && q->parent == p->pid) {
			q->parent = 1;
			if (q->state == P_ENDED)
				q->state = P_FREE;
		}
	}
	parent = process_of(p->parent);
	if (parent && parent->state == P_RUNNING && parent->actions[17].handler != 1) {
		p->state = P_ENDED;
		for (int i = 0; i < GX_MAX_THREADS; i++)
			if (thread_used[i] && threads[i].proc == parent && threads[i].cond)
				host_cond_signal(threads[i].cond);
	} else {
		p->state = P_FREE;
		parent = NULL;
	}
	if (p->held) {
		__atomic_store_n(&gx_tp_tab[p->held_slot].thread, p->held, __ATOMIC_SEQ_CST);
		host_cond_signal(p->held->cond);
		p->held = NULL;
	}
	unlock();
	if (gx_trace || gx_processes)
		host_log("gx: process %d ended (%#x)\n", p->pid, status);
	if (parent && parent->actions[17].handler > 1) {
		struct gx_thread *to = thread_for(parent, NULL, 17);

		if (to)
			post_signal(parent, to, 17, NULL);
	}
}

/* The end of a thread; of its process too if it was the last, with status. */
static void do_exit_thread(struct gx_thread *t, int status)
{
	struct gx_process *p = t->proc;
	int last, ending;

	if (t->clear_tid && !t->dying) {
		before_output(t);
		*t->clear_tid = 0;
		futex_wake((uint32_t *)t->clear_tid, 1, ~0u, NULL, 0);
	}
	if (t->shadow_of) {
		__atomic_sub_fetch(&t->shadow_of->shadowed, 1, __ATOMIC_SEQ_CST);
		t->shadow_of = NULL;
	}
	lock();
	last = --p->threads == 0;
	/* Its place among the processes may be asked for the moment it has no
	 * thread and has been waited for (a program that ends by exit_group()
	 * is waited for before its last thread gets here): what follows is
	 * done to this process, not to the next one to have the place, whose
	 * files and memory it would otherwise take away. */
	if (last)
		p->leaving = 1;
	if (t->kernel_tid && p->main_ktid == t->kernel_tid)
		p->main_ktid = 0;
	ending = last && p->state == P_RUNNING;
	if (ending)
		p->state = P_ENDING;
	unlock();
	if (ending && (gx_trace || gx_processes))
		host_log("[%d] %s: the last thread of process %d is gone\n", t->tid, t->name, p->pid);
	/* What it had open is closed with it: whoever reads what it wrote
	 * waits for that. */
	if (last && p->own_files && !p->files_lent) {
		close_all_files();
		files(GX_FILES_DROP, p->files_slot);
	}
	if (last && p != &processes[0])
		free_ranges(p);
	if (ending)
		end_process(p, status);
	{
		uint64_t base = t->host_sp;
		int attached = p->files_id;

		lock();
		if (last)
			p->leaving = 0;
		forget_thread(t);
		if (gx_pool)
			worker_loop(base, attached);   /* the host thread goes on to others */
		unlock();
	}
	host_thread_exit();
}

static long do_exit_group(struct gx_thread *self, int code)
{
	struct gx_process *p = self->proc;

	if (gx_trace || gx_processes)
		host_log("[%d] %s: exit_group(%d) for process %d\n", self->tid, self->name, code, p->pid);

	if (claim_end(p)) {
		if (p->own_files && !p->files_lent)
			close_all_files();
		end_process(p, (code & 0xff) << 8);
	}
	self->dying = 1;
	do_exit_thread(self, 0);
	return 0;
}

/* wait4 and waitid: waits for a child (which > 0: that one) to have ended.
 * Returns its number, 0 if there is none yet and the caller will not wait,
 * or an error. */
static long do_wait(struct gx_thread *self, long which, int options, int *status)
{
	struct gx_process *me = self->proc;
	long ret;

	if (!self->cond)
		self->cond = host_cond_new();
	lock();
	for (;;) {
		struct gx_process *ended = NULL;
		int any = 0;

		for (int i = 1; i < MAX_PROCESSES && !ended; i++) {
			struct gx_process *q = &processes[i];

			if (q->state == P_FREE || q->parent != me->pid || (which > 0 && q->pid != which))
				continue;
			any = 1;
			if (q->state == P_ENDED)
				ended = q;
		}
		if (ended) {
			ret = ended->pid;
			*status = ended->status;
			if (!(options & WNOWAIT))
				ended->state = P_FREE;
			break;
		}
		if (!any) {
			ret = -10;   /* ECHILD */
			break;
		}
		if (options & WNOHANG) {
			ret = 0;
			break;
		}
		if (self->dying || (__atomic_load_n(&self->pending, __ATOMIC_SEQ_CST) & ~self->blocked)) {
			ret = -GX_EINTR;
			break;
		}
		host_cond_wait(self->cond, big_lock, -1);
	}
	unlock();
	return ret;
}

/* execve: the program at path instead of what this process runs. Does not
 * return if it can be loaded. */
static long do_execve(struct gx_frame *f, struct gx_thread *self)
{
	struct gx_process *p = self->proc;
	const char *path = (const char *)f->x[0], *name;
	char **argv = (char **)f->x[1], **envp = (char **)f->x[2];
	char *args[256], heads[4][256], exe[256];
	struct gx_start start;
	int argc = 0, slot = self - threads;

	if (!gmem_accessible((uint64_t)path, 1, 0))
		return -GX_EFAULT;
	while (argv && argv[argc] && argc < 250) {
		args[argc] = argv[argc];
		argc++;
	}
	if (argv && argv[argc])
		return -7;   /* E2BIG */
	if (!argc)
		args[argc++] = (char *)path;
	args[argc] = NULL;
	for (int depth = 0;; depth++) {
		long access[6] = { LKL_AT_FDCWD, (long)path, 1 /* X_OK */ };
		long ret = lkl_syscall(__lkl__NR_faccessat, access), n;
		char *head = heads[depth], *interp, *arg, *end;
		int fd, extra;

		if (ret < 0)
			return ret;
		fd = lkl_sys_open(path, LKL_O_RDONLY, 0);
		if (fd < 0)
			return fd;
		n = lkl_sys_pread64(fd, head, sizeof(heads[0]) - 1, 0);
		lkl_sys_close(fd);
		if (n < 0)
			return -13;   /* EACCES: a directory */
		if (n >= 4 && !memcmp(head, "\x7f" "ELF", 4))
			break;
		/* A script says on its first line what runs it: "#!program",
		 * with one argument at most. */
		if (n < 3 || head[0] != '#' || head[1] != '!' || depth == 3)
			return -8;    /* ENOEXEC */
		head[n] = 0;
		end = strchr(head, '\n');
		if (end)
			*end = 0;
		end = head + strlen(head);
		while (end > head && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
			*--end = 0;
		interp = head + 2;
		while (*interp == ' ' || *interp == '\t')
			interp++;
		arg = interp;
		while (*arg && *arg != ' ' && *arg != '\t')
			arg++;
		if (*arg) {
			*arg++ = 0;
			while (*arg == ' ' || *arg == '\t')
				arg++;
		}
		if (!*interp)
			return -8;
		extra = *arg ? 2 : 1;
		if (argc + extra > 250)
			return -7;
		/* program [argument] script, then what the script was given. */
		memmove(args + extra + 1, args + 1, argc * sizeof(args[0]));
		args[0] = interp;
		if (*arg)
			args[1] = arg;
		args[extra] = (char *)path;
		argc += extra;
		path = interp;
	}
	/* A program that is to be given more than whoever starts it gives it
	 * (gx_more_for: the end of its path): at the end of its arguments. */
	if (gx_more_for && strlen(path) >= strlen(gx_more_for) &&
	    !strcmp(path + strlen(path) - strlen(gx_more_for), gx_more_for)) {
		for (int i = 0; i < gx_more_count && argc < 250; i++)
			args[argc++] = (char *)gx_more[i];
		args[argc] = NULL;
	}
	exe[0] = 0;
	if (path[0] != '/' && lkl_sys_getcwd(exe, sizeof(exe) - 2) > 0 && strcmp(exe, "/"))
		strcat(exe, "/");
	else if (path[0] != '/')
		strcpy(exe, "/");
	strncat(exe, path, sizeof(exe) - strlen(exe) - 1);
	if (gx_load(path, argc, args, envp, &start))
		return -8;

	/* No way back from here. The other threads of the process are not
	 * part of what it becomes. */
	if (gx_trace || gx_processes)
		host_log("[%d] process %d runs %s\n", self->tid, p->pid, exe);
	lock();
	for (int i = 0; i < GX_MAX_THREADS; i++) {
		if (thread_used[i] && threads[i].proc == p && &threads[i] != self) {
			threads[i].dying = 1;
			kick(&threads[i]);
		}
	}
	unlock();
	/* Nor are the files it opened for itself alone. */
	for (int fd = 0; fd < 4096; fd++) {
		long flags;

		if (fd <= 2 && !(p->real_std >> fd & 1))
			continue;
		flags = lkl_sys_fcntl(fd, 1 /* F_GETFD */, 0);
		if (flags > 0 && (flags & 1 /* FD_CLOEXEC */))
			lkl_sys_close(fd);
	}
	/* Nor its handlers: their code is the old program's. */
	for (int sig = 1; sig <= SIGNALS; sig++) {
		if (p->actions[sig].handler > 1)
			p->actions[sig].handler = 0;
		p->actions[sig].flags = p->actions[sig].mask = 0;
	}
	self->handling = 0;
	self->pending = 0;
	self->clear_tid = NULL;
	strcpy(p->exe, exe);
	memcpy(p->aux, start.aux, sizeof(p->aux));
	p->aux_words = start.aux_words;
	name = strrchr(exe, '/');
	strncpy(self->name, name ? name + 1 : exe, sizeof(self->name) - 1);
	self->name[sizeof(self->name) - 1] = 0;
	if (p->held)
		p->has_break = 0;   /* the break is its parent's */
	p->brk_base = p->brk_cur = 0;   /* and so is what it had copied of its parent's */
	p->borrowed = 0;
	/* What it had before is nobody's now (what it had of its parent's
	 * was never counted as its own). */
	if (p->threads == 1)
		free_ranges(p);
	lock();
	p->range_count = p->untracked = 0;
	add_range(p, start.loaded[0][0], start.loaded[0][1]);
	add_range(p, start.loaded[1][0], start.loaded[1][1]);
	add_range(p, start.stack_lo, start.stack_hi);
	unlock();
	self->stack_lo = start.stack_lo;
	self->stack_hi = start.stack_hi;
	start.frame->gtp = (uint64_t)self;
	gx_tp_tab[slot].thread = self;
	__atomic_store_n(&gx_tp_tab[slot].guest_tp, (uint64_t)self, __ATOMIC_SEQ_CST);
	release_held(p);
	self->nr = -1;
	to_guest(self);
	gx_resume(start.frame);
}

/* If path is about a process of the guest in /proc, by the word ("self", the
 * caller's) or by its number or one of its threads': which process, what
 * follows its name in the path, and the number the kernel has for it (0: none
 * that is known). NULL otherwise. The guest's numbers are from here and the
 * kernel has others, for threads only. */
static struct gx_process *proc_path(struct gx_process *p, const char *path, const char **tail, int *kernel)
{
	const char *at = path + 6;
	struct gx_process *q;
	struct gx_thread *t;
	long n = 0;

	if (strncmp(path, "/proc/", 6))
		return NULL;
	if (!strncmp(at, "self", 4) && (at[4] == 0 || at[4] == '/')) {
		*tail = at + 4;
		*kernel = p->main_ktid;
		return p;
	}
	if (*at < '1' || *at > '9')
		return NULL;
	while (*at >= '0' && *at <= '9' && n < 100000000)
		n = n * 10 + (*at++ - '0');
	if (*at && *at != '/')
		return NULL;
	q = process_of((int)n);
	if (q) {
		*kernel = q->main_ktid;
	} else if ((t = thread_of((int)n))) {
		q = t->proc;
		*kernel = t->kernel_tid;
	} else {
		return NULL;
	}
	*tail = at;
	return q;
}

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

/* Who last worked a System V semaphore, which the kernel knows by its own
 * numbers and programs ask by theirs (to tell a semaphore that was left
 * behind from one that is in use: the client of a game store removes the
 * first kind, and must not take its own for one). */
#define SEM_OWNERS 256
static struct {
	int id, pid;
} sem_owners[SEM_OWNERS];

static void sem_worked(int id, int pid)
{
	sem_owners[(unsigned)id % SEM_OWNERS].id = id + 1;
	sem_owners[(unsigned)id % SEM_OWNERS].pid = pid;
}

#define BOUNCE_SIZE (64UL << 10)
#define NOT_BOUNCED (-100000L)

struct gx_msghdr {
	void *name;
	uint32_t namelen;
	struct gx_iovec *iov;
	uint64_t iovlen;
	void *control;
	uint64_t controllen;
	int32_t flags;
};

/* epoll_pwait, ppoll, read, recvfrom, recvmsg, accept and accept4 by way of
 * memory of ours (see dispatch()); NOT_BOUNCED if this one is to be done the
 * plain way. */
static long bounced(struct gx_frame *f, struct gx_thread *self, long *params)
{
	long nr = f->x[8], ret;
	uint8_t *mine;

	if (!self->bounce)
		self->bounce = malloc(BOUNCE_SIZE);
	mine = self->bounce;
	if (!mine)
		return NOT_BOUNCED;
	switch (nr) {
	case 22: {                         /* epoll_pwait */
		long most = (long)f->x[2];

		if (most > (long)(BOUNCE_SIZE / 16))
			most = BOUNCE_SIZE / 16;
		if (most <= 0 || !gmem_accessible(f->x[1], most * 16, 1))
			return NOT_BOUNCED;
		params[1] = (long)mine;
		params[2] = most;
		ret = lkl_syscall(nr, params);
		before_output(self);
		if (ret > 0)
			memcpy((void *)f->x[1], mine, ret * 16);
		return ret;
	}
	case 73: {                         /* ppoll */
		uint64_t size = f->x[1] * 8;
		struct lkl_timespec wait;

		if (f->x[1] > BOUNCE_SIZE / 8 || (size && !gmem_accessible(f->x[0], size, 1)))
			return NOT_BOUNCED;
		memcpy(mine, (void *)f->x[0], size);
		params[0] = (long)mine;
		if (f->x[2]) {
			/* The kernel says there how long was left. */
			wait = *(struct lkl_timespec *)f->x[2];
			params[2] = (long)&wait;
		}
		ret = lkl_syscall(nr, params);
		before_output(self);
		memcpy((void *)f->x[0], mine, size);
		if (f->x[2])
			*(struct lkl_timespec *)f->x[2] = wait;
		return ret;
	}
	case 63:                           /* read */
		if (f->x[2] > BOUNCE_SIZE)
			return NOT_BOUNCED;
		params[1] = (long)mine;
		ret = lkl_syscall(nr, params);
		before_output(self);
		if (ret > 0)
			memcpy((void *)f->x[1], mine, ret);
		return ret;
	case 207: {                        /* recvfrom */
		uint8_t from[128];
		uint32_t from_len = sizeof(from);

		if (f->x[2] > BOUNCE_SIZE)
			return NOT_BOUNCED;
		params[1] = (long)mine;
		if (f->x[4]) {
			params[4] = (long)from;
			params[5] = (long)&from_len;
		}
		ret = lkl_syscall(nr, params);
		before_output(self);
		if (ret > 0)
			memcpy((void *)f->x[1], mine, ret);
		if (ret >= 0 && f->x[4] && f->x[5]) {
			uint32_t *len = (uint32_t *)f->x[5];

			memcpy((void *)f->x[4], from, *len < from_len ? *len : from_len);
			*len = from_len;
		}
		return ret;
	}
	case 212: {                        /* recvmsg */
		struct gx_msghdr *msg = (struct gx_msghdr *)f->x[1], ours;
		struct gx_iovec one;
		uint8_t name[128], control[2048];
		uint64_t total = 0, done = 0;

		if (!gmem_accessible((uint64_t)msg, sizeof(*msg), 1) || msg->iovlen > 64 ||
		    msg->namelen > sizeof(name) || msg->controllen > sizeof(control))
			return NOT_BOUNCED;
		for (uint64_t i = 0; i < msg->iovlen; i++)
			total += msg->iov[i].len;
		if (total > BOUNCE_SIZE)
			return NOT_BOUNCED;
		ours = *msg;
		one.base = mine;
		one.len = total;
		ours.iov = &one;
		ours.iovlen = 1;
		ours.name = msg->name ? name : NULL;
		ours.control = msg->control ? control : NULL;
		params[1] = (long)&ours;
		ret = lkl_syscall(nr, params);
		before_output(self);
		if (ret < 0)
			return ret;
		for (uint64_t i = 0; i < msg->iovlen && done < (uint64_t)ret; i++) {
			uint64_t n = (uint64_t)ret - done < msg->iov[i].len ? (uint64_t)ret - done : msg->iov[i].len;

			memcpy(msg->iov[i].base, mine + done, n);
			done += n;
		}
		if (msg->name)
			memcpy(msg->name, name, ours.namelen < msg->namelen ? ours.namelen : msg->namelen);
		if (msg->control)
			memcpy(msg->control, control, ours.controllen);
		msg->namelen = ours.namelen;
		msg->controllen = ours.controllen;
		msg->flags = ours.flags;
		return ret;
	}
	case 202: case 242: {              /* accept, accept4 */
		uint8_t from[128];
		uint32_t from_len = sizeof(from);

		if (f->x[1]) {
			params[1] = (long)from;
			params[2] = (long)&from_len;
		}
		ret = lkl_syscall(nr, params);
		before_output(self);
		if (ret >= 0 && f->x[1] && f->x[2]) {
			uint32_t *len = (uint32_t *)f->x[2];

			memcpy((void *)f->x[1], from, *len < from_len ? *len : from_len);
			*len = from_len;
		}
		return ret;
	}
	}
	return NOT_BOUNCED;
}

/* A break for a program that is not the first: room of its own, reserved when
 * it first asks where its break is, grown and shrunk inside. Told it had none,
 * the C library took its heap a megabyte at a time from mmap(), never gave any
 * of it back, and left most of each megabyte unused whenever what was asked
 * for next did not fit: a Steam client and its browser had 800 MB that way
 * after logging in. A child still in its parent's memory moves its parent's. */
#define OWN_BRK (1UL << 30)

static long own_brk(struct gx_process *p, uint64_t addr)
{
	struct gx_process *o = memory_owner(p);
	uint64_t old_top, new_top;

	if (!o->brk_base) {
		long base = gmem_mmap(0, OWN_BRK, 0, GX_MAP_ANONYMOUS | 2, -1, 0);

		if (base < 0 && base > -4096)
			return 0;
		lock();
		add_range(o, base, base + OWN_BRK);
		unlock();
		o->brk_base = o->brk_cur = base;
	}
	if (addr < o->brk_base || addr > o->brk_base + OWN_BRK)
		return (long)o->brk_cur;
	old_top = GX_PAGE_UP(o->brk_cur);
	new_top = GX_PAGE_UP(addr);
	if (new_top > old_top) {
		if (gmem_mprotect(old_top, new_top - old_top, GX_PROT_READ | GX_PROT_WRITE) < 0)
			return (long)o->brk_cur;
	} else if (new_top < old_top) {
		/* Given back, and reserved still. */
		gmem_mmap(new_top, old_top - new_top, 0, GX_MAP_ANONYMOUS | GX_MAP_FIXED | 2, -1, 0);
	}
	o->brk_cur = addr;
	return (long)addr;
}

/* Who asks for memory: by process, what it mapped to write to, what it made
 * writable of what was reserved, and what it gave back; and how large the
 * pieces were. For the report, when a program takes more than there is. */
#define ASK_SIZES 6
static const uint64_t ask_upto[ASK_SIZES] = { 64UL << 10, 512UL << 10, 1UL << 20, 4UL << 20, 32UL << 20, ~0UL };
static struct asker {
	int pid;
	char name[20];
	uint64_t mapped, opened, unmapped, calls;
	uint64_t sized[ASK_SIZES];   /* megabytes asked in pieces of each size, in KB */
} askers[48];

static void asked(struct gx_thread *self, int what, uint64_t len, uint64_t at)
{
	struct gx_process *p = self->proc;
	struct asker *a = NULL;
	const char *base = strrchr(p->exe, '/');

	for (int i = 0; i < 48 && !a; i++)
		if (!askers[i].pid || askers[i].pid == p->pid)
			a = &askers[i];
	if (!a)
		a = &askers[47];
	a->pid = p->pid;
	strncpy(a->name, base ? base + 1 : p->exe, sizeof(a->name) - 1);
	if (what == 0)
		a->mapped += len;
	else if (what == 1)
		a->opened += len;
	else
		a->unmapped += len;
	a->calls++;
	/* The large ones one by one, for a while: what asks for them says most. */
	if (what < 2 && len >= (2UL << 20) && gx_processes) {
		static int said;

		if (said < 400 && ++said)
			host_log("[%d] %s: %lu KB %s at %#lx\n", self->tid, self->name, (unsigned long)(len >> 10),
				 what ? "made writable" : "mapped to write", (unsigned long)at);
	}
	for (int k = 0; what < 2 && k < ASK_SIZES; k++) {
		if (len <= ask_upto[k]) {
			a->sized[k] += len >> 10;
			break;
		}
	}
}

static void report_askers(void)
{
	host_log("gx: memory asked for, by process (MB mapped to write, made writable, given back; calls; "
		 "MB in pieces up to 64 KB, 512 KB, 1 MB, 4 MB, 32 MB, more):\n");
	for (int shown = 0; shown < 6; shown++) {
		struct asker *best = NULL;

		for (int i = 0; i < 48; i++)
			if (askers[i].pid && !(askers[i].calls >> 63) &&
			    (!best || askers[i].mapped + askers[i].opened > best->mapped + best->opened))
				best = &askers[i];
		if (!best)
			break;
		host_log("  %5d %-16s %6lu %6lu %6lu %8lu   %lu %lu %lu %lu %lu %lu\n", best->pid, best->name,
			 (unsigned long)(best->mapped >> 20), (unsigned long)(best->opened >> 20),
			 (unsigned long)(best->unmapped >> 20), (unsigned long)best->calls,
			 (unsigned long)(best->sized[0] >> 10), (unsigned long)(best->sized[1] >> 10),
			 (unsigned long)(best->sized[2] >> 10), (unsigned long)(best->sized[3] >> 10),
			 (unsigned long)(best->sized[4] >> 10), (unsigned long)(best->sized[5] >> 10));
		best->calls |= 1UL << 63;   /* shown */
	}
	for (int i = 0; i < 48; i++)
		askers[i].calls &= ~(1UL << 63);
}

static long dispatch(struct gx_frame *f, struct gx_thread *self)
{
	long nr = f->x[8];
	long params[6] = { f->x[0], f->x[1], f->x[2], f->x[3], f->x[4], f->x[5] };
	struct gx_process *p = self->proc;
	int fd = (int)f->x[0];
	/* Files 0-2 are the host's console, until a process puts something
	 * else there (a pipe to whoever started it). */
#define CONSOLE(fd) ((fd) >= 0 && (fd) <= 2 && !(p->real_std >> (fd) & 1))

	/* The kernel has no MMU to stop it at memory the guest cannot use, and
	 * programs do probe memory with these (Chromium reads into a page it
	 * made read-only). */
	switch (nr) {
	case 63: case 67: case 207:        /* read, pread64, recvfrom */
		if (!gmem_accessible(f->x[1], f->x[2], 1)) {
			static int said;

			/* Programs do this on purpose (a browser probes a page
			 * it made read-only), and it is also how memory that
			 * was taken from under a program first shows. */
			if (gx_processes && said < 6 && ++said) {
				uint64_t at = f->x[1];

				while (at < f->x[1] + f->x[2] && gmem_accessible(at, 1, 1))
					at = (at | (GX_PAGE - 1)) + 1;
				host_log("[%d] read into %#lx+%#lx refused: %#lx is not to be written to (page state %#x)\n",
					 self->tid, (unsigned long)f->x[1], (unsigned long)f->x[2], (unsigned long)at,
					 gmem_page_state(at));
				gmem_say_freed(at);
			}
			return -GX_EFAULT;
		}
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
	case 114:                          /* clock_settime: the constants are no more */
		memset(clock_known, 0, sizeof(clock_known));
		break;
	}

	/* The syscalls that wait for something to read. What they read goes
	 * to memory of ours and from there to the guest's, after
	 * before_output(): not while a child has that memory (freeze()). The
	 * ones that are not carried out this way (too much to read at once)
	 * say so by not being quiet any more. */
	if (nr == 22 || nr == 73 || nr == 63 || nr == 207 || nr == 212 || nr == 202 || nr == 242) {
		long ret = bounced(f, self, params);

		if (ret != NOT_BOUNCED)
			return ret;
		__atomic_store_n(&self->quiet, 0, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&p->frozen, __ATOMIC_SEQ_CST))
			park(self);
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
		if (ret >= 0 || ret < -4096) {
			lock();
			add_range(memory_owner(p), ret, ret + GX_PAGE_UP(f->x[1]));
			if (((int)f->x[3] & GX_MAP_ANONYMOUS) && ((int)f->x[2] & GX_PROT_WRITE))
				asked(self, 0, f->x[1], (uint64_t)ret);
			if (((int)f->x[3] & GX_MAP_ANONYMOUS) && ((int)f->x[2] & GX_PROT_WRITE) && gx_processes &&
			    f->x[1] > (512UL << 10) && f->x[1] < (2UL << 20)) {
				static unsigned count;

				if (!(count++ & 255)) {
					uint64_t fp = f->x22_29[7], base = 0;

					host_log("[%d] %s: the %uth map of about a megabyte: %lu KB, called from:", self->tid,
						 self->name, count, (unsigned long)(f->x[1] >> 10));
					gmem_describe(f->x30, &base);
					host_log(" %#lx+%#lx", (unsigned long)base, (unsigned long)(f->x30 - base));
					for (int depth = 0; depth < 24 && fp && !(fp & 7) && gmem_accessible(fp, 16, 0); depth++) {
						uint64_t next = ((uint64_t *)fp)[0], lr = ((uint64_t *)fp)[1];

						base = 0;
						if (!gmem_describe(lr, &base))
							break;
						host_log(" %#lx+%#lx", (unsigned long)base, (unsigned long)(lr - base));
						if (next <= fp)
							break;
						fp = next;
					}
					host_log("\n");
				}
			}
			if (!((int)f->x[3] & GX_MAP_ANONYMOUS) && f->x[1] >= (64UL << 10) && gx_processes) {
				static int said;
				char link[64], name[160];
				long n;

				snprintf(link, sizeof(link), "/proc/thread-self/fd/%d", (int)f->x[4]);
				n = lkl_sys_readlinkat(LKL_AT_FDCWD, link, name, sizeof(name) - 1);
				name[n > 0 ? n : 0] = 0;
				if (said < 1500 && ++said)
					host_log("[%d] %s: %lu KB of a file mapped (protection %d, flags %#x) at %#lx: %s\n",
						 self->tid, self->name, (unsigned long)(f->x[1] >> 10), (int)f->x[2],
						 (int)f->x[3], (unsigned long)ret, name);
			}
			unlock();
		}
		return ret;
	}
	case 215: {
		long ret;

		/* What a child frees of the memory it has from its parent
		 * stays: the parent has it back as it was. */
		if (p->held && p->borrowed)
			return 0;
		gmem_note("munmap", f->x[0], f->x[0] + f->x[1], self->tid);
		/* No longer this process's before it is anybody else's: noted
		 * after, it took away the note of whoever had been given the
		 * place meanwhile. */
		lock();
		remove_range(memory_owner(p), f->x[0], f->x[0] + GX_PAGE_UP(f->x[1]));
		asked(self, 2, f->x[1], f->x[0]);
		if (f->x[1] >= (2UL << 20) && f->x[1] < (1UL << 30) && gx_processes) {
			static int said;

			if (said < 300 && ++said)
				host_log("[%d] %s: %lu KB unmapped at %#lx\n", self->tid, self->name,
					 (unsigned long)(f->x[1] >> 10), (unsigned long)f->x[0]);
		}
		unlock();
		ret = gmem_munmap(f->x[0], f->x[1]);
		return ret;
	}
	case 226:
		if (!((int)f->x[2] & 7) && f->x[1] >= (2UL << 20) && gx_processes) {
			static int said;

			if (said < 300 && ++said)
				host_log("[%d] %s: %lu KB made unusable at %#lx\n", self->tid, self->name,
					 (unsigned long)(f->x[1] >> 10), (unsigned long)f->x[0]);
		}
		if ((int)f->x[2] & GX_PROT_WRITE) {
			lock();
			asked(self, 1, f->x[1], f->x[0]);
			unlock();
		}
		return gmem_mprotect(f->x[0], f->x[1], (int)f->x[2]);
	case 214:
		/* A program run by another has no break of its own to grow: it
		 * is told so, and allocates with mmap instead. */
		if (!p->has_break && !(p->held && p->borrowed))
			return own_brk(p, f->x[0]);
		if (p->held && p->borrowed && f->x[0] && (long)f->x[0] < gmem_brk(0))
			return gmem_brk(0);    /* nor does the break come down */
		return p->has_break ? gmem_brk(f->x[0]) : 0;
	case 216: return -GX_ENOMEM;       /* mremap: glibc falls back to malloc+copy */
	case 233:
		if (f->x[1] >= (2UL << 20) && gx_processes) {
			static int said;

			if (said < 300 && ++said)
				host_log("[%d] %s: %lu KB advised %d at %#lx\n", self->tid, self->name,
					 (unsigned long)(f->x[1] >> 10), (int)f->x[2], (unsigned long)f->x[0]);
		}
		return gmem_madvise(f->x[0], f->x[1], (int)f->x[2]);

	case GX_SYS_HOST_SYMBOL: {
		void *fn = host_symbol((const char *)f->x[0]);

		if (!fn && gx_trace)
			host_log("gx: no host function %s\n", (const char *)f->x[0]);
		return (long)fn;
	}
	case GX_SYS_FORK_KIND:
		return host_write_faults();
	case GX_SYS_HOSTCALL:
		return f->x[0] ? (long)hostcall_missing : (long)gx_hostcall;

	case 220: return do_clone(f, self);
	case 435: return -GX_ENOSYS;       /* clone3: glibc falls back to clone */
	case 221: return do_execve(f, self);
	case 281: return -GX_ENOSYS;       /* execveat */
	case 260: {                        /* wait4 */
		int status = 0;
		long pid = do_wait(self, (long)f->x[0] > 0 ? (long)f->x[0] : 0, (int)f->x[2], &status);

		before_output(self);
		if (pid > 0 && f->x[1])
			*(int *)f->x[1] = status;
		if (pid > 0 && f->x[3])
			memset((void *)f->x[3], 0, 144);   /* what it used: not counted */
		return pid;
	}
	case 95: {                         /* waitid */
		int32_t *info = (int32_t *)f->x[2];
		int status = 0;
		long pid = do_wait(self, f->x[0] == 1 /* P_PID */ ? (long)f->x[1] : 0, (int)f->x[3], &status);

		before_output(self);
		if (pid < 0)
			return pid;
		if (info) {
			memset(info, 0, 128);
			if (pid > 0) {
				info[0] = 17;                             /* SIGCHLD */
				info[2] = (status & 0x7f) ? 2 : 1;        /* killed, or exited */
				info[4] = (int32_t)pid;
				info[6] = (status & 0x7f) ? (status & 0x7f) : (status >> 8) & 0xff;
			}
		}
		return 0;
	}
	case 98:  return do_futex(f, self);
	case 93:  do_exit_thread(self, ((int)f->x[0] & 0xff) << 8); return 0;
	case 94:  return do_exit_group(self, (int)f->x[0]);
	case 96:  self->clear_tid = (int *)f->x[0]; return self->tid;
	case 178: return self->tid;        /* gettid */
	case 172: return p->pid;           /* getpid */
	case 173: return p->parent;        /* getppid */
	case 99:                           /* set_robust_list */
		/* Kept, and not acted on: the locks a thread holds when it
		 * dies are not handed to who waits for them. */
		self->robust_head = f->x[0];
		self->robust_len = f->x[1];
		return 0;
	case 100: {                        /* get_robust_list */
		struct gx_thread *t = f->x[0] ? thread_of((int)f->x[0]) : self;

		if (!t)
			return -3;             /* ESRCH */
		*(uint64_t *)f->x[1] = t->robust_head;
		*(uint64_t *)f->x[2] = t->robust_len;
		return 0;
	}
	case 293: return -GX_ENOSYS;       /* rseq */
	case 124: host_yield(); return 0;  /* sched_yield */
	/* The kernel does not know the guest's threads by the ids they have
	 * here: scheduling is the host's business. */
	case 118: case 119: case 122:      /* sched_setparam, _setscheduler, _setaffinity */
		return 0;
	case 120: return 0;                /* sched_getscheduler: SCHED_OTHER */
	case 121:                          /* sched_getparam */
		*(int *)f->x[1] = 0;
		return 0;
	case 123:                          /* sched_getaffinity */
		if (f->x[1] < 8)
			return -GX_EINVAL;
		*(uint64_t *)f->x[2] = cpus >= 64 ? ~0UL : (1UL << cpus) - 1;
		return 8;

	case 134: return do_sigaction(f, self);
	case 135: return do_sigprocmask(f, self);
	case 139: return do_sigreturn(self);
	case 132:                          /* sigaltstack: handlers run where the thread is */
		return 0;
	case 129: case 130: case 131:      /* kill, tkill, tgkill */
		return send_signal(f, self, nr);

	case 78: {                         /* readlinkat */
		const char *tail = NULL;
		char number[16];
		int kernel;
		struct gx_process *q = proc_path(p, (const char *)f->x[1], &tail, &kernel);
		const char *answer = NULL;

		if (q && !strcmp(tail, "/exe")) {
			answer = q->exe;
		} else if (q && !*tail && ((const char *)f->x[1])[6] == 's') {
			/* /proc/self is a link to the caller's number. */
			snprintf(number, sizeof(number), "%d", p->pid);
			answer = number;
		}
		if (answer) {
			size_t len = strlen(answer);

			if (len > f->x[3])
				len = f->x[3];
			memcpy((void *)f->x[2], answer, len);
			return len;
		}
		break;
	}
	case 192: case 193: {              /* semtimedop, semop */
		long ret = lkl_syscall(nr, params);

		if (!ret)
			sem_worked(fd, p->pid);
		return ret;
	}
	case 191: {                        /* semctl */
		long ret = lkl_syscall(nr, params);

		if (ret >= 0 && (f->x[2] == 16 || f->x[2] == 17))      /* SETVAL, SETALL */
			sem_worked(fd, p->pid);
		if (ret >= 0 && f->x[2] == 11 &&                        /* GETPID */
		    sem_owners[(unsigned)fd % SEM_OWNERS].id == fd + 1)
			return sem_owners[(unsigned)fd % SEM_OWNERS].pid;
		return ret;
	}
	case 24: {                         /* dup3 */
		long ret = lkl_syscall(nr, params);

		if (ret >= 0 && ret <= 2)
			p->real_std |= 1 << ret;
		return ret;
	}
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
		/* By way of memory of ours: the host's kernel does not write
		 * to memory that is being watched (gmem_watch()). */
		for (uint64_t done = 0; done < f->x[1];) {
			uint8_t bytes[256];
			uint64_t n = f->x[1] - done < sizeof(bytes) ? f->x[1] - done : sizeof(bytes);

			host_random(bytes, n);
			memcpy((uint8_t *)f->x[0] + done, bytes, n);
			done += n;
		}
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
		if (fd && CONSOLE(fd)) {
			host_console_write(fd, (void *)f->x[1], f->x[2]);
			return f->x[2];
		}
		break;
	case 66:                           /* writev */
		if (fd && CONSOLE(fd)) {
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
		if (CONSOLE(fd)) {
			memset((void *)f->x[1], 0, 128);
			*(uint32_t *)(f->x[1] + 16) = 0020000 | 0666; /* a tty */
			return 0;
		}
		break;
	case 79:                           /* newfstatat with AT_EMPTY_PATH */
		if (CONSOLE(fd) && (f->x[3] & 0x1000)) {
			memset((void *)f->x[2], 0, 128);
			*(uint32_t *)(f->x[2] + 16) = 0020000 | 0666;
			return 0;
		}
		break;
	case 29:                           /* ioctl */
		if (CONSOLE(fd))
			return f->x[1] == 0x5401 /* TCGETS */ ? -GX_ENOTTY : 0;
		break;
	case 57:                           /* close */
		if (CONSOLE(fd))
			return 0;
		break;
	case 56:                           /* openat */
		params[2] = translate_open_flags(params[2]);
		if (cpuinfo_made && gmem_accessible(params[1], 14, 0) &&
		    !strcmp((const char *)params[1], "/proc/cpuinfo"))
			params[1] = (long)CPUINFO_PATH;
		break;
	case 59:                           /* pipe2: a pipe of packets is O_DIRECT */
		params[1] = translate_open_flags(params[1]);
		break;
	}
	/* The guest's processes have their numbers from here, and the kernel
	 * has other ideas: what one looks up about itself in /proc (its open
	 * files, whether a debugger follows it), by its number or as "self",
	 * is what the kernel has for the thread that asks, which is the one
	 * thing there sure to have the files of that process. */
	/* What is looked up in /proc about a process of the guest (its open
	 * files, when it started, whether a debugger follows it), by its
	 * number or as "self", is what the kernel has for the first thread of
	 * that process, under the kernel's own number for it: the same answer
	 * whoever asks, which programs count on (two of them agree on a name
	 * made from when one of them started). */
	if ((nr == 48 || nr == 56 || nr == 78 || nr == 79 || nr == 291 || nr == 439) && params[1] &&
	    gmem_accessible(params[1], 12, 0)) {
		const char *path = (const char *)params[1], *tail = NULL;
		int kernel = 0, links = 0;
		struct gx_process *q = proc_path(p, path, &tail, &kernel);
		char own[256];
		long ret;

		if (q && q != p && gx_processes) {
			static int said;

			if (said < 60 && ++said)
				host_log("[%d] %s looks at %s (syscall %ld)\n", self->tid, self->name, path, nr);
		}

		/* Which process a program is, by the end of its path: what a
		 * stand-in for lsof reads (guest/lsof.sh), there being nothing
		 * in /proc to tell the guest's processes by. "/proc/gx/of/<end
		 * of a path>" holds its number, its parent's and its user's
		 * the way lsof says them, for the last such process started.
		 * (Its own number as its parent's: a Steam client takes what
		 * connects to it for a child of its browser, and under gx the
		 * browser is one process, which connects itself. Told the
		 * true parent, the client "checked" and rejected it.) */
		if (nr == 56 && !strncmp(path, "/proc/gx/of/", 12) && gmem_accessible(params[1], 12 + 1, 0)) {
			static int serial;
			const char *end = path + 12;
			struct gx_process *found = NULL;
			char text[96];
			long afd;

			lock();
			for (int i = 0; i < MAX_PROCESSES; i++) {
				struct gx_process *o = &processes[i];
				size_t n = strlen(o->exe), m = strlen(end);

				if (o->state == P_RUNNING && m && n >= m && !strcmp(o->exe + n - m, end) &&
				    (!found || o->pid > found->pid))
					found = o;
			}
			if (found)
				snprintf(text, sizeof(text), "p%d\nR%d\nu0\n", found->pid, found->pid);
			unlock();
			if (!found)
				return -GX_ENOENT;
			if (gx_processes && serial < 4)
				host_log("[%d] asked which process %s is: %d, of %d\n", self->tid, end, found->pid, found->parent);
			snprintf(own, sizeof(own), "/dev/shm/gx-of-%d", __atomic_add_fetch(&serial, 1, __ATOMIC_SEQ_CST));
			afd = lkl_sys_open(own, LKL_O_RDWR | LKL_O_CREAT | LKL_O_EXCL | LKL_O_CLOEXEC, 0600);
			if (afd >= 0) {
				lkl_sys_unlink(own);
				lkl_sys_pwrite64(afd, text, strlen(text), 0);
			}
			return afd;
		}
		/* How many threads a process has is read off the links of its
		 * "task" directory (a browser makes sure it has one before it
		 * forks): the kernel counts every thread of every program. */
		if (nr == 79 && (!strcmp(path, "self/task/") || !strcmp(path, "self/task"))) {
			q = p;
			links = 1;
		} else if (nr == 79 && q && (!strcmp(tail, "/task/") || !strcmp(tail, "/task"))) {
			links = 1;
		}
		if (nr == 56 && q && tail && !strcmp(tail, "/auxv")) {
			/* What the program was told when it started, which the
			 * kernel never knew: a file made for the occasion. */
			static int serial;
			int n = __atomic_add_fetch(&serial, 1, __ATOMIC_SEQ_CST);
			long afd;

			snprintf(own, sizeof(own), "/dev/shm/gx-auxv-%d", n);
			afd = lkl_sys_open(own, LKL_O_RDWR | LKL_O_CREAT | LKL_O_EXCL | LKL_O_CLOEXEC, 0600);
			if (afd >= 0) {
				lkl_sys_unlink(own);
				lkl_sys_pwrite64(afd, (const char *)q->aux, q->aux_words * 8, 0);
			}
			return afd;
		}
		if (gx_pool && q == p)
			kernel = 0;   /* the kernel task that was its first thread's runs others now */
		if (q && tail) {
			if (kernel)
				snprintf(own, sizeof(own), "/proc/%d%s", kernel, tail);
			else if (q == p)
				snprintf(own, sizeof(own), "/proc/%s%s", p->own_files || gx_pool ? "thread-self" : "self", tail);
			else
				return -GX_ENOENT;
			params[1] = (long)own;
		}
		ret = lkl_syscall(nr, params);
		if (!ret && links)
			*(uint32_t *)(f->x[2] + 20) = 2 + q->threads;   /* st_nlink */
		return ret;
	}
	return lkl_syscall(nr, params);
#undef CONSOLE
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
	if (elapsed && used * 100 / elapsed < 1 && !calls && !gx_report_idle)
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

void gx_report_fault(uint64_t tp, uint64_t sp)
{
	int found = 0;

	for (int i = 0; i < GX_MAX_THREADS; i++) {
		const struct gx_thread *t = &threads[i], *by_tp = NULL;

		if (gx_tp_tab[i].guest_tp == tp && gx_tp_tab[i].thread)
			by_tp = gx_tp_tab[i].thread;
		if (by_tp)
			host_log("  that thread pointer is guest thread %d's (%s), process %d; last syscall %ld; "
				 "its syscalls run from %#lx; %s%s\n", by_tp->tid, by_tp->name,
				 by_tp->proc ? by_tp->proc->pid : 0, (long)by_tp->nr, (unsigned long)by_tp->host_sp,
				 by_tp->shadowed ? "shares its thread pointer; " : "",
				 by_tp->in_host ? "in a syscall" : "in its own code");
		found |= by_tp != NULL;
		if (!thread_used[i])
			continue;
		if (sp >= t->stack_lo && sp < t->stack_hi)
			host_log("  the stack is the first stack of guest thread %d (%s), process %d\n", t->tid,
				 t->name, t->proc ? t->proc->pid : 0);
		if (t->resume && sp >= (uint64_t)t->resume - 65536 && sp <= (uint64_t)t->resume + 65536)
			host_log("  the stack is near where guest thread %d (%s) last waited (frame %#lx, %s)\n",
				 t->tid, t->name, (unsigned long)t->resume, t->waiting ? "still waiting" : "resumed");
		if (sp <= t->host_sp && t->host_sp - sp < (1UL << 20))
			host_log("  the stack is the host's, where guest thread %d (%s) has its syscalls (from %#lx); "
				 "last syscall %ld\n", t->tid, t->name, (unsigned long)t->host_sp, (long)t->nr);
	}
	if (!found)
		host_log("  no guest thread has that thread pointer: the host's own code, or a thread that just set it\n");
}

void gx_report_threads(void)
{
	uint64_t now = host_now_ns(), elapsed = reported_at ? now - reported_at : 0;

	reported_at = now;
	report_askers();
	host_log("gx: %lu syscalls so far, %d threads; the busy ones:\n",
		 (unsigned long)__atomic_load_n(&syscall_count, __ATOMIC_RELAXED), live_threads);
	if (gx_pool)
		host_log("gx: host threads for them: %d made, %d with nothing to run, %d guest threads waiting for one\n",
			 pool_workers, pool_idle, run_length);
	for (int i = 0; i < GX_MAX_THREADS; i++)
		if (thread_used[i])
			report_thread(&threads[i], elapsed);
}

/*
 * What a program reads of certain files, summed and said in the log, to be
 * held against the files themselves afterwards: where what a Steam client
 * shows comes out wrong (its scripts do not parse on a console), this tells
 * whether they were read wrong or went wrong after. With gx_processes.
 */
#define FOLLOWED 64
static struct followed {
	int files, fd;        /* whose file, and which: files 0 is nobody's */
	uint64_t at;
	char name[56];
} followed[FOLLOWED];

static uint64_t sum_of(const uint8_t *p, uint64_t n, uint64_t sum)
{
	while (n--)
		sum = (sum ^ *p++) * 0x100000001B3UL;
	return sum;
}

static void follow(struct gx_frame *f, struct gx_thread *self, long ret)
{
	int files = self->proc->files_id, nr = (int)f->x[8], fd = nr == 222 ? (int)f->x[4] : (int)f->x[0];
	struct followed *w = NULL;

	if (nr == 56) {
		const char *path = (const char *)f->x[1], *name;
		size_t len;

		if (ret < 0 || !path || !gmem_accessible((uint64_t)path, 1, 0))
			return;
		len = strlen(path);
		if (len < 4 || strcmp(path + len - 3, ".js"))
			return;
		for (int i = 0; i < FOLLOWED && !w; i++)
			if (!followed[i].files)
				w = &followed[i];
		if (!w)
			return;
		name = len > sizeof(w->name) - 1 ? path + len - (sizeof(w->name) - 1) : path;
		strcpy(w->name, name);
		w->fd = (int)ret;
		w->at = 0;
		w->files = files;
		host_log("[%d] read: %s is file %d\n", self->tid, path, w->fd);
		return;
	}
	for (int i = 0; i < FOLLOWED && !w; i++)
		if (followed[i].files == files && followed[i].fd == fd)
			w = &followed[i];
	if (!w)
		return;
	switch (nr) {
	case 57:
		w->files = 0;
		break;
	case 62:
		if (ret >= 0)
			w->at = ret;
		break;
	case 63: case 67:
		if (ret > 0) {
			uint64_t at = nr == 67 ? f->x[3] : w->at;

			host_log("[%d] read: %s at %lu, %ld: %016lx\n", self->tid, w->name, (unsigned long)at, ret,
				 (unsigned long)sum_of((const uint8_t *)f->x[1], ret, 0xCBF29CE484222325UL));
			if (nr == 63)
				w->at += ret;
		}
		break;
	case 65: case 69:
		if (ret > 0) {
			const struct { uint64_t base, len; } *v = (void *)f->x[1];
			uint64_t sum = 0xCBF29CE484222325UL, left = ret, at = nr == 69 ? f->x[3] : w->at;

			for (int i = 0; i < (int)f->x[2] && left; i++) {
				uint64_t n = v[i].len < left ? v[i].len : left;

				sum = sum_of((const uint8_t *)v[i].base, n, sum);
				left -= n;
			}
			host_log("[%d] read: %s at %lu, %ld: %016lx\n", self->tid, w->name, (unsigned long)at, ret,
				 (unsigned long)sum);
			if (nr == 65)
				w->at += ret;
		}
		break;
	case 222:
		if (ret > 0 || ret < -4096)
			host_log("[%d] read: %s mapped at %lu, %lu (protection %d, flags %#x): %016lx\n", self->tid,
				 w->name, (unsigned long)f->x[5], (unsigned long)f->x[1], (int)f->x[2], (int)f->x[3],
				 (int)f->x[2] & 1 ? (unsigned long)sum_of((const uint8_t *)ret, f->x[1],
									    0xCBF29CE484222325UL) : 0UL);
		break;
	}
}

long gx_do_syscall(struct gx_frame *f)
{
	struct gx_thread *self = (struct gx_thread *)f->ctx;
	long ret;

	__atomic_store_n(&self->in_host, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&self->proc->frozen, __ATOMIC_SEQ_CST))
		park(self);
	if (self->dying)
		do_exit_thread(self, 0);
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
	__atomic_store_n(&self->quiet, is_quiet(f->x[8]), __ATOMIC_SEQ_CST);
	ret = dispatch(f, self);
	if (gx_processes && (f->x[8] == 56 || f->x[8] == 57 || f->x[8] == 62 || f->x[8] == 63 || f->x[8] == 65 ||
			     f->x[8] == 67 || f->x[8] == 69 || f->x[8] == 222))
		follow(f, self, ret);
	__atomic_store_n(&self->quiet, 0, __ATOMIC_SEQ_CST);
	self->nr = -1;
	if (self->dying)
		do_exit_thread(self, 0);

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
	to_guest(self);
	deliver_signal(f, self, ret);
	return ret;
}
