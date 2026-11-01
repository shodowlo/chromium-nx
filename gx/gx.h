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

/* One per guest thread. The trampoline reads the first two fields. */
struct gx_thread {
	uint64_t host_tp;     /*  0 */
	uint64_t host_sp;     /*  8 */
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

/* tramp.S */
void gx_svc_trampoline(void);
void gx_hostcall(void);
void gx_enter_frame(struct gx_frame *frame, struct gx_thread *self) __attribute__((noreturn));

/* gmem.c */
int gmem_init(void);
long gmem_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off);
long gmem_munmap(uint64_t addr, uint64_t len);
long gmem_mprotect(uint64_t addr, uint64_t len, int prot);
long gmem_brk(uint64_t addr);
long gmem_madvise(uint64_t addr, uint64_t len, int advice);
/* Whether the guest may read (or write) all of [addr, addr+len). */
int gmem_accessible(uint64_t addr, uint64_t len, int write);
int gmem_in_memory_fs(int fd);
/* Where the contents of a file the guest has shared mappings of are, for a
 * host that shows them (wl.c): NULL if it has none. */
uint8_t *gmem_shared_peek(int fd, uint64_t *size);
void gmem_set_brk(uint64_t start);
/* Reserves room for an ELF image plus the veneers its syscalls will need. */
long gmem_reserve_image(uint64_t span);
int gmem_describe(uint64_t addr, uint64_t *base);
void gmem_report(void);
/* For the host to call when the guest's memory at addr refused an access (a
 * jump to it if exec, a read or a write otherwise): returns whether the access
 * is one to try again. See gmem.c on memory writable and executable. */
int gmem_fault(uint64_t addr, int exec);
int gmem_page_state(uint64_t addr);

/* gelf.c */
struct gx_image {
	uint64_t base, entry, phdr, end;
	int phnum, phent;
	char interp[256];
};
int gelf_load(const char *path, struct gx_image *img);
int gx_exec(const char *path, int argc, char **argv, char **envp);
extern char gx_exe_path[256];   /* what /proc/self/exe points to */

/* gsys.c */
long gx_do_syscall(struct gx_frame *frame);
/* Says where every guest thread is: what a host shows when the guest seems
 * stuck. */
void gx_report_threads(void);
void gx_report_syscall_cost(void);
/* Says how the kernel's own memory is doing (its /proc/meminfo, in short):
 * files of its memory filesystems and its network buffers all come out of
 * what it was started with, and it panics when that is gone. */
void gx_report_kernel_memory(void);
extern int gx_trace;
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
/* Whether the memory left to commit is getting short: what the guest lets go
 * is then given back to the host rather than kept for it. */
int host_memory_low(void);
void host_flush_icache(void *addr, size_t len);
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
/* Makes the calling thread one of the kernel's for as long as it is in a
 * syscall of the kernel (1), or one of the guest's again (0). */
void host_in_kernel(int in);
/* 0: as every guest thread; 1: before them; 2: before those too. */
int host_thread_priority(uint64_t thread, int level);
/* Folders of the SD card as filesystems of the kernel (hostfs_nx.c): before
 * the kernel starts, then "hostfs" mounts with a path of the card. */
int hostfs_nx_init(void);
int hostfs_nx_mkdirs(const char *path);
int hostfs_nx_exists(const char *path);
int hostfs_nx_remove_tree(const char *path);
void hostfs_nx_report(void);   /* every 10 s, from the watchdog */
void hostfs_nx_flush(void);    /* what was gathered for writing, to the card */
/* -xp in programs.txt: what the guest asks of priorities is not done. */
extern int gx_no_priorities;
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
