/*
 * gelf.c - loads a program the way the Linux kernel would: its PT_LOAD
 * segments, then its interpreter (ld.so), then a process stack with argv,
 * envp and the auxiliary vector. Relocation and the libraries are left to
 * ld.so, which runs as guest code.
 */
#include <string.h>
#include <lkl.h>
#include "gx.h"

#define PT_LOAD 1
#define PT_INTERP 3
#define PT_PHDR 6
#define ET_DYN 3
#define EM_AARCH64 183
#define STACK_SIZE (8UL << 20)

struct ehdr {
	unsigned char ident[16];
	uint16_t type, machine;
	uint32_t version;
	uint64_t entry, phoff, shoff;
	uint32_t flags;
	uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct phdr {
	uint32_t type, flags;
	uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

int gelf_load(const char *path, struct gx_image *img)
{
	struct ehdr eh;
	struct phdr ph[64];
	uint64_t lo = ~0UL, hi = 0, phdr_vaddr = 0;
	long base;
	int fd = lkl_sys_open(path, LKL_O_RDONLY, 0);

	memset(img, 0, sizeof(*img));
	if (fd < 0) {
		host_log("gx: cannot open %s (%d)\n", path, fd);
		return -1;
	}
	if (lkl_sys_pread64(fd, (char *)&eh, sizeof(eh), 0) != sizeof(eh) ||
	    memcmp(eh.ident, "\x7f" "ELF", 4) || eh.ident[4] != 2 ||
	    eh.machine != EM_AARCH64 || eh.type != ET_DYN || eh.phnum > 64 ||
	    lkl_sys_pread64(fd, (char *)ph, eh.phnum * sizeof(ph[0]), eh.phoff) !=
		    (long)(eh.phnum * sizeof(ph[0]))) {
		host_log("gx: %s is not a position-independent aarch64 ELF\n", path);
		lkl_sys_close(fd);
		return -1;
	}

	for (int i = 0; i < eh.phnum; i++) {
		if (ph[i].type == PT_INTERP && ph[i].filesz < sizeof(img->interp))
			lkl_sys_pread64(fd, img->interp, ph[i].filesz, ph[i].offset);
		if (ph[i].type == PT_PHDR)
			phdr_vaddr = ph[i].vaddr;
		if (ph[i].type != PT_LOAD)
			continue;
		if (GX_PAGE_DOWN(ph[i].vaddr) < lo)
			lo = GX_PAGE_DOWN(ph[i].vaddr);
		if (GX_PAGE_UP(ph[i].vaddr + ph[i].memsz) > hi)
			hi = GX_PAGE_UP(ph[i].vaddr + ph[i].memsz);
	}
	if (hi <= lo || (base = gmem_reserve_image(hi - lo)) < 0) {
		host_log("gx: no room for %s\n", path);
		lkl_sys_close(fd);
		return -1;
	}
	base -= lo;

	for (int i = 0; i < eh.phnum; i++) {
		uint64_t start, file_end, mem_end, skew;
		int prot = 0;

		if (ph[i].type != PT_LOAD)
			continue;
		if (ph[i].flags & 4) prot |= GX_PROT_READ;
		if (ph[i].flags & 2) prot |= GX_PROT_WRITE;
		if (ph[i].flags & 1) prot |= GX_PROT_EXEC;
		skew = ph[i].vaddr & (GX_PAGE - 1);
		start = base + ph[i].vaddr - skew;
		file_end = GX_PAGE_UP(base + ph[i].vaddr + ph[i].filesz);
		mem_end = GX_PAGE_UP(base + ph[i].vaddr + ph[i].memsz);
		/* The length is the file's, not the page's: what follows is .bss
		 * and gmem leaves it zero. */
		if (ph[i].filesz &&
		    gmem_mmap(start, ph[i].filesz + skew, prot, GX_MAP_FIXED, fd,
			      ph[i].offset - skew) < 0)
			goto fail;
		if (mem_end > file_end &&
		    gmem_mmap(file_end, mem_end - file_end, prot,
			      GX_MAP_FIXED | GX_MAP_ANONYMOUS, -1, 0) < 0)
			goto fail;
	}
	lkl_sys_close(fd);
	img->base = base;
	img->start = base + lo;
	img->entry = base + eh.entry;
	img->phdr = base + (phdr_vaddr ? phdr_vaddr : lo + eh.phoff);
	img->phnum = eh.phnum;
	img->phent = eh.phentsize;
	img->end = base + hi;
	return 0;
fail:
	host_log("gx: cannot map a segment of %s\n", path);
	lkl_sys_close(fd);
	return -1;
}

static uint64_t push_bytes(uint64_t *top, const void *data, size_t len)
{
	*top -= len;
	memcpy((void *)*top, data, len);
	return *top;
}

int gx_load(const char *path, int argc, char **argv, char **envp, struct gx_start *start)
{
	static const uint8_t random_bytes[16] = {
		0x5a, 0x13, 0xc4, 0x9e, 0x27, 0xb8, 0x61, 0xf0,
		0x8d, 0x36, 0xe9, 0x4b, 0x72, 0xa5, 0x1c, 0xd7,
	};
	struct gx_image exe, interp;
	struct gx_frame *frame;
	uint64_t top, *sp, execfn, platform, random;
	uint64_t strings[256];
	long stack;
	int envc = 0, words, n = 0;

	if (gelf_load(path, &exe))
		return -1;
	interp.base = interp.start = interp.end = 0;
	interp.entry = exe.entry;
	if (exe.interp[0] && gelf_load(exe.interp, &interp))
		return -1;

	stack = gmem_mmap(0, STACK_SIZE, GX_PROT_READ | GX_PROT_WRITE, GX_MAP_ANONYMOUS, -1, 0);
	if (stack < 0)
		return -1;
	top = stack + STACK_SIZE;

	while (envp && envp[envc])
		envc++;
	if (argc + envc + 2 > 256)
		return -1;
	execfn = push_bytes(&top, path, strlen(path) + 1);
	platform = push_bytes(&top, "aarch64", 8);
	random = push_bytes(&top, random_bytes, sizeof(random_bytes));
	for (int i = 0; i < argc; i++)
		strings[n++] = push_bytes(&top, argv[i], strlen(argv[i]) + 1);
	strings[n++] = 0;
	for (int i = 0; i < envc; i++)
		strings[n++] = push_bytes(&top, envp[i], strlen(envp[i]) + 1);
	strings[n++] = 0;

	words = 1 + n + 2 * 18;
	sp = (uint64_t *)((top - words * 8) & ~15UL);
	top = (uint64_t)sp;
	*sp++ = argc;
	for (int i = 0; i < n; i++)
		*sp++ = strings[i];
	start->aux_words = 0;
#define AUX(type, value) do { \
		*sp++ = start->aux[start->aux_words++] = (type); \
		*sp++ = start->aux[start->aux_words++] = (value); \
	} while (0)
	AUX(3, exe.phdr);          /* AT_PHDR */
	AUX(4, exe.phent);         /* AT_PHENT */
	AUX(5, exe.phnum);         /* AT_PHNUM */
	AUX(6, GX_PAGE);           /* AT_PAGESZ */
	AUX(7, interp.base);       /* AT_BASE */
	AUX(8, 0);                 /* AT_FLAGS */
	AUX(9, exe.entry);         /* AT_ENTRY */
	AUX(11, 0);                /* AT_UID */
	AUX(12, 0);                /* AT_EUID */
	AUX(13, 0);                /* AT_GID */
	AUX(14, 0);                /* AT_EGID */
	AUX(15, platform);         /* AT_PLATFORM */
	AUX(16, host_hwcap(0));    /* AT_HWCAP */
	AUX(17, 100);              /* AT_CLKTCK */
	AUX(23, 0);                /* AT_SECURE */
	AUX(25, random);           /* AT_RANDOM */
	AUX(31, execfn);           /* AT_EXECFN */
	AUX(0, 0);                 /* AT_NULL */
#undef AUX

	/* The first frame resumes at the entry point with every register zero,
	 * which is what a new process sees (x0 = 0: no rtld_fini). */
	frame = (struct gx_frame *)(top - GX_FRAME_SIZE);
	memset(frame, 0, GX_FRAME_SIZE);
	frame->exit = interp.entry;
	start->frame = frame;
	start->stack_lo = stack;
	start->stack_hi = stack + STACK_SIZE;
	start->image_end = exe.end;
	start->loaded[0][0] = exe.start;
	start->loaded[0][1] = exe.end;
	start->loaded[1][0] = interp.start;
	start->loaded[1][1] = interp.end;
	if (gx_trace)
		host_log("gx: %s at %#lx, interp at %#lx, entry %#lx, sp %#lx\n", path,
			 (unsigned long)exe.base, (unsigned long)interp.base,
			 (unsigned long)interp.entry, (unsigned long)top);
	return 0;
}

int gx_exec(const char *path, int argc, char **argv, char **envp)
{
	struct gx_start start;

	if (gx_load(path, argc, argv, envp, &start))
		return -1;
	gmem_set_brk(start.image_end);
	gx_start_first(path, &start);
}
