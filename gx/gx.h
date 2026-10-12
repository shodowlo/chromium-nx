/*
 * gx - guest executor: runs dynamically linked, multi-threaded aarch64 Linux
 * programs on top of LKL, in one address space and without a host MMU API
 * richer than reserve/commit/protect.
 *
 * The core (gmem.c, gelf.c, gsys.c, tramp.S) only talks to the host through
 * the host_* functions below, so the same code runs under qemu-user on a PC
 * (host_posix.c) and, later, on Horizon.
 */
#ifndef GX_H
#define GX_H

#include <stddef.h>
#include <stdint.h>

#define GX_PAGE 0x1000UL
#define GX_PAGE_UP(x) (((uint64_t)(x) + GX_PAGE - 1) & ~(GX_PAGE - 1))
#define GX_PAGE_DOWN(x) ((uint64_t)(x) & ~(GX_PAGE - 1))

/* Linux aarch64 values, as the guest passes them. */
#define GX_PROT_READ 1
#define GX_PROT_WRITE 2
#define GX_PROT_EXEC 4
#define GX_MAP_SHARED 0x01
#define GX_MAP_FIXED 0x10
#define GX_MAP_ANONYMOUS 0x20
#define GX_MAP_DENYWRITE 0x800
#define GX_MAP_FIXED_NOREPLACE 0x100000

#define GX_ENOENT 2
#define GX_EINTR 4
#define GX_EAGAIN 11
#define GX_ENOMEM 12
#define GX_EFAULT 14
#define GX_EINVAL 22
#define GX_ENOTTY 25
#define GX_ENOSYS 38
#define GX_ETIMEDOUT 110

/* The guest state the trampoline saves at every syscall (tramp.S). */
struct gx_frame {
	uint64_t x[16];       /*   0: x0-x15 */
	uint64_t x18, x30;    /* 128 */
	uint64_t exit, gtp;   /* 144: where to resume, guest TPIDR_EL0 */
	uint64_t nzcv, fpcr;  /* 160 */
	uint64_t fpsr, x19;   /* 176 */
	uint64_t x20, x21;    /* 192 */
	uint64_t result, ctx; /* 208 */
	uint8_t q[32][16];    /* 224 */
	uint64_t x22_29[8];   /* 736 */
};                            /* 800 */
#define GX_FRAME_SIZE 800

/* One per guest thread. The trampoline reads the first three fields. */
struct gx_thread {
	uint64_t host_tp;     /*  0 */
	uint64_t host_sp;     /*  8 */
	/* How many other threads run with this one's TPIDR_EL0 (children that
	 * were made to run beside it, see gsys.c): while there are some, who
	 * makes a syscall is told from where its stack is. */
	uint64_t shadowed;    /* 16 */
	int tid;
	int *clear_tid;
	struct gx_frame *start;
	struct host_cond *cond;   /* what this thread sleeps on in futex_wait */
	/* The syscall the thread is in (-1: it is running guest code) and its
	 * first arguments, for gx_report_threads(). */
	volatile long nr;
	volatile uint64_t args[4];
	/* For the same report: what the host knows the thread as, the name the
	 * guest gave it, and what it had used when last reported. */
	uint64_t host_id;
	char name[16];
	uint64_t syscalls, reported_syscalls, reported_cpu_ns;
	/* Signals: the ones sent to this thread and not delivered yet, the
	 * ones it does not want now (a bit each, signal 1 first), and what
	 * its handlers interrupted, innermost last (see gsys.c). */
	uint64_t pending, blocked;
	struct gx_frame *interrupted[4];
	uint64_t interrupted_blocked[4];
	int handling;
	/* Processes (see gsys.c): the one this thread is of; what the kernel
	 * knows the thread as; its first stack if it is the first thread of a
	 * program, which is how it is found before it has a TPIDR_EL0 of its
	 * own; that it is to end at its next syscall, its process being over;
	 * and, while it is being started, whose files it is to take, whether
	 * as a copy (a new process), who waits for that and that it is done. */
	struct gx_process *proc;
	int kernel_tid;
	uint64_t stack_lo, stack_hi;
	struct gx_thread *shadow_of;   /* the thread whose TPIDR_EL0 this one runs with */
	int leader;                    /* the first thread of its process */
	volatile int dying;
	int join, forked;         /* a new process: the files it starts from are its parent's, kept as join */
	/* Waiting on a futex (see gsys.c): where, for which wakers, whether
	 * it was woken, and until when if not for ever. */
	uint32_t *wait_addr;
	uint32_t wait_bitset;
	int waiting, wait_woken;
	uint64_t wait_deadline;
	struct gx_thread *wait_next;
	/* Without a thread of the host for now: what it comes back with, to
	 * which frame (or that it has not started yet), and its turn. */
	struct gx_frame *resume;
	long resume_ret;
	int resume_start;
	struct gx_thread *run_next;
	int watched;              /* what it writes is being noticed (gmem_watch()) */
	int *set_tid;             /* where a forked child is to find its number */
	/* For a process that is held still while a child borrows its memory:
	 * that the thread is in a syscall, that it waits there for the child
	 * to be done, that the host stopped it where it was. */
	volatile int in_host, parked, stopped;
	/* That the syscall it is in writes nothing to the guest's memory
	 * until it has asked (before_output()), and where such a syscall has
	 * the kernel put what it reads meanwhile. */
	volatile int quiet;
	uint8_t *bounce;
	/* What it told set_robust_list(), to tell get_robust_list(). */
	uint64_t robust_head, robust_len;
};

#define GX_MAX_THREADS 256
struct gx_tp_slot {
	uint64_t guest_tp;
	struct gx_thread *thread;
};
extern struct gx_tp_slot gx_tp_tab[GX_MAX_THREADS];
extern struct gx_thread *gx_main_thread;

/* Syscalls of gx itself, for guest libraries that forward to the host
 * (guest/hostlib.c). */
#define GX_SYS_HOST_SYMBOL 0x47580001 /* (name): address of a host function, or 0 */
#define GX_SYS_HOSTCALL 0x47580002    /* (0): gx_hostcall; (1): what to call for a missing function */
#define GX_SYS_FORK_KIND 0x47580003   /* 1 if a forked child cannot harm its parent, whatever it does */

/* tramp.S */
void gx_svc_trampoline(void);
void gx_hostcall(void);
void gx_enter_frame(struct gx_frame *frame, struct gx_thread *self) __attribute__((noreturn));
void gx_resume(struct gx_frame *frame) __attribute__((noreturn));

/* gmem.c */
/* Data the guest maps is committed when it is first used rather than when it
 * is mapped: for a host where committing is for real, and a guest that asks
 * for more than fits. To be set before the guest starts. */
extern int gmem_lazy;
/* How much at a time, in pages (a power of two): a megabyte unless the host,
 * which may be short of pieces rather than of pages, says it can be less. */
extern int gmem_lazy_pages;
/* Of a unit, only what the guest asked to read or write is committed with
 * the page that was touched, not what it reserved next to it. */
extern int gmem_lazy_wanted_only;
/* Up to how much memory is committed when it is asked for and not when it is
 * first used: by a mapping, and by making usable what was reserved. A
 * thread's stack is always left for its first use. */
extern uint64_t gmem_eager_mapped, gmem_eager_protected;
int gmem_init(void);
long gmem_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off);
long gmem_munmap(uint64_t addr, uint64_t len);
long gmem_mprotect(uint64_t addr, uint64_t len, int prot);
long gmem_brk(uint64_t addr);
long gmem_madvise(uint64_t addr, uint64_t len, int advice);
/* Whether the guest may read (or write) all of [addr, addr+len). */
int gmem_accessible(uint64_t addr, uint64_t len, int write);
uint64_t gmem_writable(uint64_t addr, uint64_t max);
int gmem_in_memory_fs(int fd);
/* Where the contents of a file the guest has shared mappings of are, for a
 * host that shows them (wl.c): NULL if it has none. */
uint8_t *gmem_shared_peek(int fd, uint64_t *size);
uint64_t gmem_same_place(uint64_t addr);
void gmem_set_brk(uint64_t start);
void gmem_break(uint64_t *start, uint64_t *end);
/* What a forked child writes, to be undone (see gsys.c on processes): from
 * gmem_watch() on, the first write to each page of these ranges (and of the
 * break, if asked) is noticed, and the page kept as it was if the writer is
 * the thread gmem_watch_child() names; gmem_unwatch() puts those pages back.
 * One watch at a time: -1 if there is one, or if the host cannot do this. */
struct gx_range {
	uint64_t lo, hi;
};
int gmem_watch(const struct gx_range *ranges, int count, int with_break);
void gmem_watch_child(uint64_t host_thread);
void gmem_unwatch(void);
/* Reserves room for an ELF image plus the veneers its syscalls will need. */
long gmem_reserve_image(uint64_t span);
/* A few instructions of ours for the guest to run, their syscalls patched
 * like a program's: where they are, or 0. */
uint64_t gmem_code(const uint32_t *code, int words);
int gmem_describe(uint64_t addr, uint64_t *base);
void gmem_report(void);
/* For the host to call when the guest's memory at addr refused an access (a
 * jump to it if exec, a read or a write otherwise): returns whether the access
 * is one to try again. See gmem.c on memory writable and executable. */
int gmem_fault(uint64_t addr, int exec);
/* For the host to call when the processor refused the instruction at pc of
 * the guest's: returns whether it is one to run again (it was replaced by
 * what the processor does have). */
void gmem_note(const char *how, uint64_t lo, uint64_t hi, uint64_t who);
void gmem_say_freed(uint64_t addr);
int gmem_illegal(uint64_t pc);
int gmem_counter(uint64_t pc);
int gmem_page_state(uint64_t addr);

/* gelf.c */
struct gx_image {
	uint64_t base, start, entry, phdr, end;
	int phnum, phent;
	char interp[256];
};
/* A program loaded and ready to start: the frame to enter, and its stack. */
struct gx_start {
	struct gx_frame *frame;
	uint64_t stack_lo, stack_hi, image_end;
	uint64_t loaded[2][2];   /* where the program and its interpreter are: from, to */
	uint64_t aux[40];        /* what the program is told of where it runs (/proc/self/auxv) */
	int aux_words;
};
int gelf_load(const char *path, struct gx_image *img);
/* Loads a program next to whatever is loaded already; -1 if it cannot be. */
int gx_load(const char *path, int argc, char **argv, char **envp, struct gx_start *start);
/* Runs the first program: returns only if it cannot be loaded. */
int gx_exec(const char *path, int argc, char **argv, char **envp);

/* gsys.c */
long gx_do_syscall(struct gx_frame *frame);
/* Enters the first program, as process 100. */
void gx_start_first(const char *path, const struct gx_start *start) __attribute__((noreturn));
/* For the trampoline: the thread that makes a syscall with a TPIDR_EL0 no
 * thread is known by, its stack pointer being sp. */
struct gx_thread *gx_thread_at(uint64_t tp, uint64_t sp);
/* Says where every guest thread is: what a host shows when the guest seems
 * stuck. */
void gx_report_threads(void);
/* For a host that reports a fault: says which guest thread has this thread
 * pointer or this stack, and what it was last doing. */
void gx_report_fault(uint64_t tp, uint64_t sp);
/* Says how the kernel's own memory is doing (its /proc/meminfo, in short):
 * files of its memory filesystems and its network buffers all come out of
 * what it was started with, and it panics when that is gone. */
void gx_report_kernel_memory(void);
extern int gx_trace;
/* Guest threads that wait on a futex give up their thread of the host, and
 * come back on whichever is free: for programs with more threads than the
 * host allows. (0: each guest thread has its own for life, which what a
 * host library keeps per thread, a GL context, counts on.) To be set before
 * the guest starts. At most gx_pool_cap threads of the host are made. */
extern int gx_pool, gx_pool_cap;
/* Arguments added to those of every program the guest runs whose path ends
 * in gx_more_for: a way to tell a program something that the one starting it
 * does not let through (a browser engine inside a client). */
extern const char *gx_more_for, *gx_more[16];
extern int gx_more_count;
/* Says in the log when a process of the guest starts, runs a program, ends. */
extern int gx_processes;
extern int gx_report_idle;   /* gx_report_threads() says of the idle threads too */
/* How many processors the guest is told about; to be called once the kernel
 * runs and its /sys is mounted, before the guest starts. One by default. */
void gx_set_cpus(int count);

/* Host layer. */
/* A host may refuse memory that is writable and executable at once (Horizon
 * does); the core never asks for it on its own account. */
/* Reserves *len bytes of addresses, or the most there is if that is less;
 * *len becomes what was reserved. The reservation may have holes, ranges the
 * host could not get: host_reserved_hole() gives them one by one, and returns
 * 0 when index is past the last. */
void *host_reserve(size_t *len);
int host_reserved_hole(int index, void **addr, size_t *len);
int host_commit(void *addr, size_t len, int prot);   /* zero-filled */
int host_protect(void *addr, size_t len, int prot);
int host_decommit(void *addr, size_t len);
void host_flush_icache(void *addr, size_t len);
/* Whether a write to memory that host_protect() made read-only, by the guest
 * or by the host or the kernel on its behalf, comes to gmem_fault() and can
 * be tried again: what gmem_watch() takes. */
int host_write_faults(void);
/* Stops a thread if it is running the guest's code, and says whether it did:
 * a thread that is in the host's (a syscall, a function of the host called
 * for the guest) is left alone, it may hold what others wait for. Stopped
 * threads stay so until host_threads_go(). */
int host_thread_stop(uint64_t thread);
void host_threads_go(void);
/* Memory that can be mapped at several addresses of the arena at once, for
 * shared file mappings; always all of it (len is its size), which is the only
 * way Horizon has. A host without such memory returns -1 from
 * host_shared_new(). */
int host_shared_new(size_t len);
/* The size of the pieces to put small shared files together in, for a host
 * that is short of pieces; 0 gives each file its own. At most 16 MB. */
size_t host_shared_chunk(void);
int host_shared_map(int handle, void *addr, size_t len, int prot);
void host_shared_unmap(int handle, void *addr, size_t len);
void host_shared_free(int handle);
int host_thread_start(void (*fn)(void *), void *arg);
void host_thread_exit(void) __attribute__((noreturn));
void host_yield(void);
/* The calling thread, as something host_thread_cpu_ns() takes from any
 * thread: the processor time the thread has used. */
uint64_t host_thread_self(void);
uint64_t host_thread_cpu_ns(uint64_t thread);
void host_exit(int code) __attribute__((noreturn));
void host_console_write(int fd, const void *buf, size_t len);
uint64_t host_hwcap(int which);
uint64_t host_now_ns(void);   /* a clock that only goes forward */
void host_random(void *buf, size_t len);
/* A host function the guest may call through gx_hostcall (EGL, GLES), or NULL. */
void *host_symbol(const char *name);
void host_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

struct host_lock;
struct host_cond;
struct host_lock *host_lock_new(void);
void host_lock_acquire(struct host_lock *l);
void host_lock_release(struct host_lock *l);
struct host_cond *host_cond_new(void);
/* Waits at most ns nanoseconds (forever if ns < 0). Returns 0, or 1 on timeout. */
int host_cond_wait(struct host_cond *c, struct host_lock *l, int64_t ns);
void host_cond_signal(struct host_cond *c);

#endif
