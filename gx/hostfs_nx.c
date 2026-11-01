/*
 * hostfs_nx.c - folders of the SD card as filesystems of the kernel: the
 * host's half of hostfs (gx/lkl/hostfs), on Horizon's own file calls.
 *
 * The browser's profile and its downloads live as plain files in
 * sdmc:/switch/chromium-nx, where they can be seen and copied. The card's
 * filesystem (FAT32 or exFAT, through Horizon) is not a Unix one, and what
 * Linux expects of it is made up here:
 *
 * - A file is open once, shared by all the kernel's openings of it, and
 *   opened again for writing in place when one wants to write: Horizon will
 *   not open a file twice for writing.
 * - A file removed while open stays until it is closed (Linux's way, which a
 *   database's temporary files count on): it is moved out of the way at once,
 *   into a folder emptied at the next start, so that its name is free.
 * - An open file that is renamed is closed, renamed and opened again.
 * - Symbolic links (a browser locks its profile with one) are empty files
 *   whose target is kept in a list next to the folders.
 * - Inode numbers, which the card has none of, are made from the path.
 * - No owners, permissions, devices nor second names.
 *
 * Every call comes with the kernel's one processor held, so they do not run
 * at the same time; a lock is taken all the same, for the start.
 */
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gx.h"

#define LINKS_FILE "/switch/chromium-nx/.gx-links"
#define TRASH_DIR "/switch/chromium-nx/.gx-trash"
#define PATH_LEN FS_MAX_PATH

/* As in the kernel's fs/hostfs/hostfs.h. */
struct hostfs_timespec {
	long long tv_sec;
	long long tv_nsec;
};

struct hostfs_iattr {
	unsigned int ia_valid;
	unsigned short ia_mode;
	unsigned int ia_uid;
	unsigned int ia_gid;
	long long ia_size;
	struct hostfs_timespec ia_atime, ia_mtime, ia_ctime;
};

struct hostfs_stat {
	unsigned long long ino;
	unsigned int mode;
	unsigned int nlink;
	unsigned int uid;
	unsigned int gid;
	unsigned long long size;
	struct hostfs_timespec atime, mtime, ctime;
	unsigned int blksize;
	unsigned long long blocks;
	struct {
		unsigned int maj;
		unsigned int min;
	} rdev, dev;
};

#define HOSTFS_ATTR_SIZE 8
#define HOSTFS_ATTR_ATIME 16
#define HOSTFS_ATTR_MTIME 32

/* Linux's numbers, not newlib's. */
enum { L_EPERM = 1, L_ENOENT = 2, L_EIO = 5, L_EBADF = 9, L_ENOMEM = 12, L_EBUSY = 16,
       L_EEXIST = 17, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22, L_EMFILE = 24,
       L_ENOSPC = 28, L_ENAMETOOLONG = 36, L_ENOTEMPTY = 39 };
#define L_S_IFDIR 0040000
#define L_S_IFREG 0100000
#define L_S_IFLNK 0120000
#define L_DT_DIR 4
#define L_DT_REG 8
#define L_DT_LNK 10

static FsFileSystem *sd;
static Mutex lock;
static unsigned long errors_said;

static int error_of(Result rc)
{
	if (R_MODULE(rc) == 2 /* fs */) {
		unsigned d = R_DESCRIPTION(rc);

		if (d == 1)
			return -L_ENOENT;
		if (d == 2)
			return -L_EEXIST;
		if (d == 7)
			return -L_EBUSY;
		if (d == 8)
			return -L_ENOTEMPTY;
		if (d >= 30 && d <= 45)
			return -L_ENOSPC;
		if (d == 6003)
			return -L_ENAMETOOLONG;
		if (d >= 6000 && d < 6100)
			return -L_EINVAL;
	}
	return -L_EIO;
}

/* The kernel's paths, with no doubled or trailing slash. */
static int clean(const char *in, char *out)
{
	size_t n = 0;

	for (const char *p = in; *p; p++) {
		if (*p == '/' && n && out[n - 1] == '/')
			continue;
		if (n + 1 >= PATH_LEN)
			return -L_ENAMETOOLONG;
		out[n++] = *p;
	}
	if (n > 1 && out[n - 1] == '/')
		n--;
	if (!n)
		out[n++] = '/';
	out[n] = 0;
	return 0;
}

static unsigned long long ino_of(const char *path)
{
	unsigned long long h = 1469598103934665603ULL;

	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		unsigned char c = *p >= 'A' && *p <= 'Z' ? *p + 32 : *p;   /* the card ignores case */

		h = (h ^ c) * 1099511628211ULL;
	}
	return h ? h : 1;
}

static int same_path(const char *a, const char *b)
{
	return !strcasecmp(a, b);
}

/* Whether path is prefix or inside it. */
static int under(const char *path, const char *prefix)
{
	size_t n = strlen(prefix);

	return !strncasecmp(path, prefix, n) && (path[n] == 0 || path[n] == '/');
}

/* ---------- symbolic links ---------- */

struct link {
	char *path, *target;
};
static struct link *links;
static int link_count, link_room;

static struct link *link_at(const char *path)
{
	for (int i = 0; i < link_count; i++)
		if (same_path(links[i].path, path))
			return &links[i];
	return NULL;
}

static void save_links(void)
{
	FsFile f;
	char *text;
	size_t len = 0, room = 1;

	for (int i = 0; i < link_count; i++)
		room += strlen(links[i].path) + strlen(links[i].target) + 2;
	text = malloc(room);
	if (!text)
		return;
	for (int i = 0; i < link_count; i++)
		len += sprintf(text + len, "%s\t%s\n", links[i].path, links[i].target);
	fsFsDeleteFile(sd, LINKS_FILE);
	if (R_SUCCEEDED(fsFsCreateFile(sd, LINKS_FILE, len, 0)) &&
	    R_SUCCEEDED(fsFsOpenFile(sd, LINKS_FILE, FsOpenMode_Write, &f))) {
		fsFileWrite(&f, 0, text, len, FsWriteOption_Flush);
		fsFileClose(&f);
	}
	free(text);
}

static void add_link(const char *path, const char *target)
{
	if (link_count == link_room) {
		struct link *more = realloc(links, (link_room * 2 + 16) * sizeof(*links));

		if (!more)
			return;
		links = more;
		link_room = link_room * 2 + 16;
	}
	links[link_count].path = strdup(path);
	links[link_count].target = strdup(target);
	link_count++;
}

static void drop_link(struct link *l)
{
	free(l->path);
	free(l->target);
	*l = links[--link_count];
}

static void load_links(void)
{
	FsFile f;
	s64 size = 0;
	u64 got = 0;
	char *text;

	if (R_FAILED(fsFsOpenFile(sd, LINKS_FILE, FsOpenMode_Read, &f)))
		return;
	fsFileGetSize(&f, &size);
	text = size > 0 && size < (1 << 20) ? malloc(size + 1) : NULL;
	if (text && R_SUCCEEDED(fsFileRead(&f, 0, text, size, FsReadOption_None, &got))) {
		text[got] = 0;
		for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
			char *tab = strchr(line, '\t');

			if (tab) {
				*tab = 0;
				add_link(line, tab + 1);
			}
		}
	}
	free(text);
	fsFileClose(&f);
}

/* ---------- open files ---------- */

struct hfile {
	char path[PATH_LEN];
	FsFile f;
	int open;          /* f is a file of Horizon's */
	int writable;
	int refs;          /* the kernel's descriptors on it */
	int deleted;       /* removed while open: goes when closed */
	/* The card is slow by small pieces, and the kernel reads and writes a
	 * page at a time: a piece read ahead, and writes gathered, each kept
	 * in one run of the file. */
	u8 *rbuf, *wbuf;
	u64 roff, woff;
	u32 rlen, wlen;
	struct hfile *next;
};
static struct hfile *hfiles;

#define RCAP (64 << 10)
#define WCAP (256 << 10)

/* What was gathered, written; with the lock held. */
static Result flush_write(struct hfile *h)
{
	Result rc = 0;

	if (h->wlen && h->open)
		rc = fsFileWrite(&h->f, h->woff, h->wbuf, h->wlen, FsWriteOption_None);
	if (R_FAILED(rc) && errors_said++ < 10)
		host_log("gx: hostfs: %u bytes of %s not written (%#x)\n", h->wlen, h->path, rc);
	h->wlen = 0;
	return rc;
}

static void forget_read(struct hfile *h)
{
	h->rlen = 0;
}

#define SLOTS 1024
static struct hfile *slots[SLOTS];

static struct hfile *hfile_at(const char *path)
{
	for (struct hfile *h = hfiles; h; h = h->next)
		if (!h->deleted && same_path(h->path, path))
			return h;
	return NULL;
}

static Result reopen(struct hfile *h)
{
	Result rc = fsFsOpenFile(sd, h->path, h->writable ?
				 FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append :
				 FsOpenMode_Read, &h->f);

	h->open = R_SUCCEEDED(rc);
	return rc;
}

/* Horizon's handles on what is under prefix let go, to rename or remove it. */
static void detach(const char *prefix)
{
	for (struct hfile *h = hfiles; h; h = h->next)
		if (h->open && under(h->path, prefix)) {
			flush_write(h);
			forget_read(h);
			fsFileClose(&h->f);
			h->open = 0;
		}
}

/* And taken again, at their new place: from prefix to its new name. */
static void reattach(const char *from, const char *to)
{
	for (struct hfile *h = hfiles; h; h = h->next) {
		if (!h->open && to && under(h->path, from)) {
			char moved[PATH_LEN];

			if ((size_t)snprintf(moved, sizeof(moved), "%s%s", to, h->path + strlen(from)) <
			    sizeof(moved))
				strcpy(h->path, moved);
		}
		if (!h->open && reopen(h) && errors_said++ < 10)
			host_log("gx: hostfs: %s cannot be opened again\n", h->path);
	}
}

static int new_slot(struct hfile *h)
{
	for (int i = 0; i < SLOTS; i++)
		if (!slots[i]) {
			slots[i] = h;
			h->refs++;
			return i;
		}
	return -L_EMFILE;
}

static void release(struct hfile *h)
{
	if (--h->refs > 0)
		return;
	flush_write(h);
	if (h->open)
		fsFileClose(&h->f);
	if (h->deleted)
		fsFsDeleteFile(sd, h->path);
	for (struct hfile **p = &hfiles; *p; p = &(*p)->next)
		if (*p == h) {
			*p = h->next;
			break;
		}
	free(h->rbuf);
	free(h->wbuf);
	free(h);
}

static struct hfile *slot(int fd)
{
	return fd >= 0 && fd < SLOTS ? slots[fd] : NULL;
}

/* A file removed while open is moved out of the way; it goes for good when
 * the last opening of it is closed, or at the next start. */
static void bury(struct hfile *h)
{
	static unsigned serial;
	char grave[PATH_LEN];

	snprintf(grave, sizeof(grave), TRASH_DIR "/%u-%u", (unsigned)armGetSystemTick(), serial++);
	flush_write(h);
	forget_read(h);
	if (h->open)
		fsFileClose(&h->f);
	h->open = 0;
	fsFsCreateDirectory(sd, TRASH_DIR);
	if (R_SUCCEEDED(fsFsRenameFile(sd, h->path, grave)))
		strcpy(h->path, grave);
	h->deleted = 1;
	reopen(h);
}

/* ---------- the operations ---------- */

static int hfs_stat(const char *in, struct hostfs_stat *p, int fd)
{
	char path[PATH_LEN];
	FsDirEntryType type;
	FsTimeStampRaw ts = { 0 };
	struct hfile *h = fd >= 0 ? slot(fd) : NULL;
	struct link *l;
	s64 size = 0;
	Result rc;
	int err;

	if (fd >= 0 && !h)
		return -L_EBADF;
	if (h)
		strcpy(path, h->path);
	else if ((err = clean(in, path)))
		return err;
	memset(p, 0, sizeof(*p));
	mutexLock(&lock);
	if (!h && (l = link_at(path))) {
		p->mode = L_S_IFLNK | 0777;
		p->size = strlen(l->target);
		type = FsDirEntryType_File;
	} else {
		rc = fsFsGetEntryType(sd, path, &type);
		if (R_FAILED(rc)) {
			mutexUnlock(&lock);
			return error_of(rc);
		}
		if (type == FsDirEntryType_Dir) {
			p->mode = L_S_IFDIR | 0755;
			p->size = 4096;
			p->nlink = 2;
		} else {
			struct hfile *o = h ? h : hfile_at(path);
			FsFile f;

			p->mode = L_S_IFREG | 0644;
			if (o && o->open) {
				flush_write(o);
				fsFileGetSize(&o->f, &size);
			} else if (R_SUCCEEDED(fsFsOpenFile(sd, path, FsOpenMode_Read, &f))) {
				fsFileGetSize(&f, &size);
				fsFileClose(&f);
			}
			p->size = size;
		}
	}
	if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(sd, path, &ts)) && ts.is_valid) {
		p->mtime.tv_sec = ts.modified;
		p->atime.tv_sec = ts.accessed;
		p->ctime.tv_sec = ts.created;
	}
	mutexUnlock(&lock);
	if (!p->nlink)
		p->nlink = 1;
	p->ino = ino_of(path);
	p->blksize = 4096;
	p->blocks = (p->size + 511) / 512;
	return 0;
}

/* The card has no permissions, and the kernel asks on every directory of
 * every path it walks: what exists may be used. */
static int hfs_access(char *path, int r, int w, int x)
{
	return 0;
}

static int hfs_open(char *in, int r, int w, int append)
{
	char path[PATH_LEN];
	struct hfile *h;
	Result rc;
	int err, fd;

	if ((err = clean(in, path)))
		return err;
	mutexLock(&lock);
	h = hfile_at(path);
	if (h && w && !h->writable) {
		/* Open again for writing, in place, for every opening of it. */
		flush_write(h);
		forget_read(h);
		if (h->open)
			fsFileClose(&h->f);
		h->writable = 1;
		rc = reopen(h);
		if (R_FAILED(rc)) {
			h->writable = 0;
			reopen(h);
			mutexUnlock(&lock);
			return error_of(rc);
		}
	} else if (!h) {
		h = calloc(1, sizeof(*h));
		if (!h) {
			mutexUnlock(&lock);
			return -L_ENOMEM;
		}
		strcpy(h->path, path);
		h->writable = w;
		rc = reopen(h);
		if (R_FAILED(rc)) {
			free(h);
			mutexUnlock(&lock);
			return error_of(rc);
		}
		h->next = hfiles;
		hfiles = h;
	}
	fd = new_slot(h);
	if (fd < 0 && !h->refs) {
		h->refs = 1;   /* no slot: let go of it as its last holder */
		release(h);
	}
	mutexUnlock(&lock);
	return fd;
}

struct dir {
	char path[PATH_LEN];
	int count, at;
	struct {
		char name[FS_MAX_PATH];
		unsigned type;
	} *entries;
};

static void *hfs_open_dir(char *in, int *err_out)
{
	struct dir *d = calloc(1, sizeof(*d));
	FsDir dir;
	FsDirectoryEntry batch[32];
	s64 got;
	Result rc;
	int err;

	*err_out = 0;
	if (!d) {
		*err_out = L_ENOMEM;
		return NULL;
	}
	if ((err = clean(in, d->path))) {
		free(d);
		*err_out = -err;
		return NULL;
	}
	mutexLock(&lock);
	rc = fsFsOpenDirectory(sd, d->path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir);
	if (R_FAILED(rc)) {
		mutexUnlock(&lock);
		free(d);
		*err_out = -error_of(rc);
		return NULL;
	}
	while (R_SUCCEEDED(fsDirRead(&dir, &got, 32, batch)) && got > 0) {
		void *more = realloc(d->entries, (d->count + got) * sizeof(*d->entries));

		if (!more)
			break;
		d->entries = more;
		for (s64 i = 0; i < got; i++) {
			char full[PATH_LEN];

			snprintf(full, sizeof(full), "%s/%s", strcmp(d->path, "/") ? d->path : "",
				 batch[i].name);
			strcpy(d->entries[d->count].name, batch[i].name);
			d->entries[d->count].type = batch[i].type == FsDirEntryType_Dir ? L_DT_DIR :
						    link_at(full) ? L_DT_LNK : L_DT_REG;
			d->count++;
		}
	}
	fsDirClose(&dir);
	mutexUnlock(&lock);
	return d;
}

static void hfs_seek_dir(void *stream, unsigned long long pos)
{
	((struct dir *)stream)->at = (int)pos;
}

/* "." and ".." first, which the card does not list. */
static char *hfs_read_dir(void *stream, unsigned long long *pos_out, unsigned long long *ino_out,
			  int *len_out, unsigned int *type_out)
{
	struct dir *d = stream;
	char *name;
	char full[PATH_LEN];

	if (d->at >= d->count + 2)
		return NULL;
	if (d->at < 2) {
		name = d->at ? ".." : ".";
		*type_out = L_DT_DIR;
		*ino_out = ino_of(d->path) + d->at;
	} else {
		name = d->entries[d->at - 2].name;
		*type_out = d->entries[d->at - 2].type;
		snprintf(full, sizeof(full), "%s/%s", strcmp(d->path, "/") ? d->path : "", name);
		*ino_out = ino_of(full);
	}
	d->at++;
	*pos_out = d->at;
	*len_out = strlen(name);
	return name;
}

static void hfs_close_dir(void *stream)
{
	struct dir *d = stream;

	free(d->entries);
	free(d);
}

static void hfs_close_file(void *stream)
{
	int fd = *(int *)stream;
	struct hfile *h;

	mutexLock(&lock);
	h = slot(fd);
	if (h) {
		slots[fd] = NULL;
		release(h);
	}
	mutexUnlock(&lock);
}

/* fd is to be what oldfd is (dup2()). */
static int hfs_replace_file(int oldfd, int fd)
{
	struct hfile *a, *b;

	mutexLock(&lock);
	a = slot(oldfd);
	b = slot(fd);
	if (!a || !b) {
		mutexUnlock(&lock);
		return -L_EBADF;
	}
	if (a != b) {
		slots[fd] = a;
		a->refs++;
		release(b);
	}
	mutexUnlock(&lock);
	return fd;
}

static int hfs_read(int fd, unsigned long long *offset, char *buf, int len)
{
	struct hfile *h;
	u64 off = *offset, got = 0;
	Result rc = 0;

	mutexLock(&lock);
	h = slot(fd);
	if (!h || !h->open) {
		mutexUnlock(&lock);
		return -L_EBADF;
	}
	/* What is gathered for writing is the file's latest. */
	if (h->wlen && off < h->woff + h->wlen && off + len > h->woff)
		flush_write(h);
	if (h->rlen && off >= h->roff && off + len <= h->roff + h->rlen) {
		memcpy(buf, h->rbuf + (off - h->roff), len);
		got = len;
	} else if (len >= RCAP || (!h->rbuf && !(h->rbuf = malloc(RCAP)))) {
		rc = fsFileRead(&h->f, off, buf, len, FsReadOption_None, &got);
	} else {
		u64 have = 0;

		rc = fsFileRead(&h->f, off, h->rbuf, RCAP, FsReadOption_None, &have);
		h->roff = off;
		h->rlen = R_SUCCEEDED(rc) ? (u32)have : 0;
		got = have < (u64)len ? have : (u64)len;
		memcpy(buf, h->rbuf, got);
	}
	mutexUnlock(&lock);
	if (R_FAILED(rc))
		return error_of(rc);
	*offset += got;
	return (int)got;
}

static int hfs_write(int fd, unsigned long long *offset, const char *buf, int len)
{
	struct hfile *h;
	u64 off = *offset;
	Result rc = 0;

	mutexLock(&lock);
	h = slot(fd);
	if (!h || !h->open || !h->writable) {
		mutexUnlock(&lock);
		return -L_EBADF;
	}
	forget_read(h);
	if (h->wlen && off >= h->woff && off <= h->woff + h->wlen && off + len - h->woff <= WCAP) {
		/* Next to, or over, what is gathered. */
		memcpy(h->wbuf + (off - h->woff), buf, len);
		if (off + len - h->woff > h->wlen)
			h->wlen = off + len - h->woff;
	} else {
		rc = flush_write(h);
		if (R_SUCCEEDED(rc)) {
			if (len >= WCAP || (!h->wbuf && !(h->wbuf = malloc(WCAP)))) {
				rc = fsFileWrite(&h->f, off, buf, len, FsWriteOption_None);
			} else {
				memcpy(h->wbuf, buf, len);
				h->woff = off;
				h->wlen = len;
			}
		}
	}
	mutexUnlock(&lock);
	if (R_FAILED(rc))
		return error_of(rc);
	*offset += len;
	return len;
}

static int hfs_lseek(int fd, long long offset, int whence)
{
	return slot(fd) ? 0 : -L_EBADF;   /* every read and write says where */
}

static int hfs_fsync(int fd, int datasync)
{
	struct hfile *h = slot(fd);

	Result rc;

	if (!h || !h->open)
		return -L_EBADF;
	if (!h->writable)
		return 0;
	mutexLock(&lock);
	rc = flush_write(h);
	if (R_SUCCEEDED(rc))
		rc = fsFileFlush(&h->f);
	mutexUnlock(&lock);
	return R_FAILED(rc) ? error_of(rc) : 0;
}

static int hfs_create(char *in, int mode)
{
	char path[PATH_LEN];
	struct link *l;
	Result rc;
	int err;

	if ((err = clean(in, path)))
		return err;
	mutexLock(&lock);
	if ((l = link_at(path))) {
		drop_link(l);
		save_links();
	}
	rc = fsFsCreateFile(sd, path, 0, 0);
	mutexUnlock(&lock);
	if (R_FAILED(rc) && error_of(rc) != -L_EEXIST)
		return error_of(rc);
	return hfs_open(path, 1, 1, 0);
}

static int hfs_set_attr(const char *in, struct hostfs_iattr *attrs, int fd)
{
	char path[PATH_LEN];
	int err = 0;

	if (fd < 0 && (err = clean(in, path)))
		return err;
	if (attrs->ia_valid & HOSTFS_ATTR_SIZE) {
		struct hfile *h = fd >= 0 ? slot(fd) : NULL;
		Result rc;

		mutexLock(&lock);
		if (!h)
			h = hfile_at(path);
		if (h) {
			flush_write(h);
			forget_read(h);
		}
		if (h && h->open && h->writable) {
			rc = fsFileSetSize(&h->f, attrs->ia_size);
		} else if (h) {
			/* Open for reading only: for writing a moment. */
			fsFileClose(&h->f);
			h->writable = 1;
			rc = reopen(h);
			if (R_SUCCEEDED(rc))
				rc = fsFileSetSize(&h->f, attrs->ia_size);
		} else {
			FsFile f;

			rc = fsFsOpenFile(sd, path, FsOpenMode_Write | FsOpenMode_Append, &f);
			if (R_SUCCEEDED(rc)) {
				rc = fsFileSetSize(&f, attrs->ia_size);
				fsFileClose(&f);
			}
		}
		mutexUnlock(&lock);
		if (R_FAILED(rc))
			return error_of(rc);
	}
	/* Owners, permissions and times: the card keeps none that can be set
	 * from here. What is asked back is what the card says. */
	if (attrs->ia_valid & (HOSTFS_ATTR_ATIME | HOSTFS_ATTR_MTIME)) {
		struct hostfs_stat st;

		if (!hfs_stat(fd >= 0 ? NULL : path, &st, fd)) {
			attrs->ia_atime = st.atime;
			attrs->ia_mtime = st.mtime;
		}
	}
	return 0;
}

static int hfs_symlink(const char *from, const char *to)
{
	char path[PATH_LEN];
	FsDirEntryType type;
	Result rc;
	int err;

	if ((err = clean(from, path)))
		return err;
	mutexLock(&lock);
	if (link_at(path) || R_SUCCEEDED(fsFsGetEntryType(sd, path, &type))) {
		mutexUnlock(&lock);
		return -L_EEXIST;
	}
	rc = fsFsCreateFile(sd, path, 0, 0);
	if (R_SUCCEEDED(rc)) {
		add_link(path, to);
		save_links();
	}
	mutexUnlock(&lock);
	return R_FAILED(rc) ? error_of(rc) : 0;
}

static int hfs_unlink(const char *in)
{
	char path[PATH_LEN];
	struct link *l;
	struct hfile *h;
	Result rc = 0;
	int err;

	if ((err = clean(in, path)))
		return err;
	mutexLock(&lock);
	if ((l = link_at(path))) {
		drop_link(l);
		save_links();
	}
	h = hfile_at(path);
	if (h)
		bury(h);
	else
		rc = fsFsDeleteFile(sd, path);
	mutexUnlock(&lock);
	return R_FAILED(rc) ? error_of(rc) : 0;
}

static int hfs_mkdir(const char *in, int mode)
{
	char path[PATH_LEN];
	int err;

	Result rc;

	if ((err = clean(in, path)))
		return err;
	rc = fsFsCreateDirectory(sd, path);
	return R_FAILED(rc) ? error_of(rc) : 0;
}

static int hfs_rmdir(const char *in)
{
	char path[PATH_LEN];
	Result rc;
	int err;

	if ((err = clean(in, path)))
		return err;
	rc = fsFsDeleteDirectory(sd, path);
	return R_FAILED(rc) ? error_of(rc) : 0;
}

static int hfs_readlink(char *in, char *buf, int size)
{
	char path[PATH_LEN];
	struct link *l;
	int n, err;

	if ((err = clean(in, path)))
		return err;
	mutexLock(&lock);
	l = link_at(path);
	n = l ? (int)strlen(l->target) : -L_EINVAL;
	if (l) {
		if (n > size)
			n = size;
		memcpy(buf, l->target, n);
		if (n < size)
			buf[n] = 0;
	}
	mutexUnlock(&lock);
	return n;
}

static int hfs_rename2(char *in_from, char *in_to, unsigned int flags)
{
	char from[PATH_LEN], to[PATH_LEN];
	FsDirEntryType type, to_type;
	struct link *l;
	struct hfile *h;
	Result rc;
	int err, to_exists;

	if ((err = clean(in_from, from)) || (err = clean(in_to, to)))
		return err;
	if (flags & ~1u)
		return -L_EINVAL;   /* RENAME_EXCHANGE and others */
	if (same_path(from, to))
		return 0;
	mutexLock(&lock);
	if (!(l = link_at(from)) && R_FAILED(rc = fsFsGetEntryType(sd, from, &type))) {
		mutexUnlock(&lock);
		return error_of(rc);
	}
	if (l)
		type = FsDirEntryType_File;
	to_exists = link_at(to) || R_SUCCEEDED(fsFsGetEntryType(sd, to, &to_type));
	if (to_exists && (flags & 1)) {
		mutexUnlock(&lock);
		return -L_EEXIST;
	}
	/* What is in the way goes first: Linux replaces it, Horizon will not. */
	if (to_exists) {
		struct link *tl = link_at(to);

		if (tl) {
			drop_link(tl);
			l = link_at(from);
		}
		if (R_SUCCEEDED(fsFsGetEntryType(sd, to, &to_type))) {
			if (to_type == FsDirEntryType_Dir) {
				if (type != FsDirEntryType_Dir) {
					mutexUnlock(&lock);
					return -L_EISDIR;
				}
				rc = fsFsDeleteDirectory(sd, to);
			} else if ((h = hfile_at(to))) {
				bury(h);
				rc = 0;
			} else {
				rc = fsFsDeleteFile(sd, to);
			}
			if (R_FAILED(rc)) {
				mutexUnlock(&lock);
				return error_of(rc);
			}
		}
	}
	detach(from);
	rc = type == FsDirEntryType_Dir ? fsFsRenameDirectory(sd, from, to) : fsFsRenameFile(sd, from, to);
	reattach(from, R_SUCCEEDED(rc) ? to : NULL);
	if (R_SUCCEEDED(rc)) {
		/* Links under what moved move with it. */
		int changed = 0;

		for (int i = 0; i < link_count; i++)
			if (under(links[i].path, from)) {
				char moved[PATH_LEN];

				snprintf(moved, sizeof(moved), "%s%s", to, links[i].path + strlen(from));
				free(links[i].path);
				links[i].path = strdup(moved);
				changed = 1;
			}
		if (changed)
			save_links();
	}
	mutexUnlock(&lock);
	return R_FAILED(rc) ? error_of(rc) : 0;
}

static int hfs_rename(char *from, char *to)
{
	return hfs_rename2(from, to, 0);
}

static int hfs_statfs(char *root, long *bsize_out, long long *blocks_out, long long *bfree_out,
		      long long *bavail_out, long long *files_out, long long *ffree_out,
		      void *fsid_out, int fsid_size, long *namelen_out)
{
	s64 total = 0, free_space = 0;

	fsFsGetTotalSpace(sd, "/", &total);
	fsFsGetFreeSpace(sd, "/", &free_space);
	*bsize_out = 4096;
	*blocks_out = total / 4096;
	*bfree_out = *bavail_out = free_space / 4096;
	*files_out = 1 << 20;
	*ffree_out = 1 << 19;
	memset(fsid_out, 0, fsid_size);
	*namelen_out = 255;
	return 0;
}

/* Named as the kernel looks for it (gx/lkl/hostfs/hostfs_lkl.c). */
struct lkl_hostfs_ops {
	int (*stat_file)(const char *, struct hostfs_stat *, int);
	int (*access_file)(char *, int, int, int);
	int (*open_file)(char *, int, int, int);
	void *(*open_dir)(char *, int *);
	void (*seek_dir)(void *, unsigned long long);
	char *(*read_dir)(void *, unsigned long long *, unsigned long long *, int *, unsigned int *);
	void (*close_file)(void *);
	int (*replace_file)(int, int);
	void (*close_dir)(void *);
	int (*read_file)(int, unsigned long long *, char *, int);
	int (*write_file)(int, unsigned long long *, const char *, int);
	int (*lseek_file)(int, long long, int);
	int (*fsync_file)(int, int);
	int (*file_create)(char *, int);
	int (*set_attr)(const char *, struct hostfs_iattr *, int);
	int (*make_symlink)(const char *, const char *);
	int (*unlink_file)(const char *);
	int (*do_mkdir)(const char *, int);
	int (*do_rmdir)(const char *);
	int (*do_readlink)(char *, char *, int);
	int (*rename_file)(char *, char *);
	int (*rename2_file)(char *, char *, unsigned int);
	int (*do_statfs)(char *, long *, long long *, long long *, long long *, long long *,
			 long long *, void *, int, long *);
} lkl_hostfs_ops;

/* What was removed while open, the last time: it is no longer anyone's. */
static void empty_trash(void)
{
	FsDir dir;
	FsDirectoryEntry batch[32];
	s64 got;
	char path[PATH_LEN];

	if (R_FAILED(fsFsOpenDirectory(sd, TRASH_DIR, FsDirOpenMode_ReadFiles, &dir)))
		return;
	while (R_SUCCEEDED(fsDirRead(&dir, &got, 32, batch)) && got > 0)
		for (s64 i = 0; i < got; i++) {
			snprintf(path, sizeof(path), TRASH_DIR "/%s", batch[i].name);
			fsFsDeleteFile(sd, path);
		}
	fsDirClose(&dir);
}

/* What each kind of operation costs: every one holds the kernel's processor
 * for as long as the card takes. */
enum { T_STAT, T_OPEN, T_CLOSE, T_READ, T_WRITE, T_SYNC, T_CREATE, T_ATTR, T_UNLINK,
       T_RENAME, T_DIR, T_KINDS };
static const char *const t_names[T_KINDS] = { "stat", "open", "close", "read", "write", "sync",
	"create", "truncate", "unlink", "rename", "opendir" };
static uint64_t t_calls[T_KINDS], t_ns[T_KINDS];

#define TIMED(kind, ret, name, params, args)                               \
	static ret t_##name params                                         \
	{                                                                  \
		uint64_t t0 = host_now_ns();                               \
		ret r = name args;                                         \
		t_calls[kind]++;                                           \
		t_ns[kind] += host_now_ns() - t0;                          \
		return r;                                                  \
	}
TIMED(T_STAT, int, hfs_stat, (const char *a, struct hostfs_stat *b, int c), (a, b, c))
TIMED(T_STAT, int, hfs_access, (char *a, int b, int c, int d), (a, b, c, d))
TIMED(T_OPEN, int, hfs_open, (char *a, int b, int c, int d), (a, b, c, d))
TIMED(T_READ, int, hfs_read, (int a, unsigned long long *b, char *c, int d), (a, b, c, d))
TIMED(T_WRITE, int, hfs_write, (int a, unsigned long long *b, const char *c, int d), (a, b, c, d))
TIMED(T_SYNC, int, hfs_fsync, (int a, int b), (a, b))
TIMED(T_CREATE, int, hfs_create, (char *a, int b), (a, b))
TIMED(T_ATTR, int, hfs_set_attr, (const char *a, struct hostfs_iattr *b, int c), (a, b, c))
TIMED(T_UNLINK, int, hfs_unlink, (const char *a), (a))
TIMED(T_RENAME, int, hfs_rename2, (char *a, char *b, unsigned int c), (a, b, c))
TIMED(T_DIR, void *, hfs_open_dir, (char *a, int *b), (a, b))

static void t_hfs_close_file(void *stream)
{
	uint64_t t0 = host_now_ns();

	hfs_close_file(stream);
	t_calls[T_CLOSE]++;
	t_ns[T_CLOSE] += host_now_ns() - t0;
}

static int t_hfs_rename(char *from, char *to)
{
	return t_hfs_rename2(from, to, 0);
}

void hostfs_nx_flush(void)
{
	if (!sd)
		return;
	mutexLock(&lock);
	for (struct hfile *h = hfiles; h; h = h->next)
		if (h->wlen) {
			flush_write(h);
			fsFileFlush(&h->f);
		}
	mutexUnlock(&lock);
}

void hostfs_nx_report(void)
{
	static uint64_t last_calls[T_KINDS], last_ns[T_KINDS];
	char line[400];
	int len = snprintf(line, sizeof(line), "gx: hostfs in 10 s (calls, ms):");

	for (int k = 0; k < T_KINDS; k++) {
		uint64_t c = t_calls[k] - last_calls[k], ns = t_ns[k] - last_ns[k];

		last_calls[k] = t_calls[k];
		last_ns[k] = t_ns[k];
		if (c && len < (int)sizeof(line) - 40)
			len += snprintf(line + len, sizeof(line) - len, " %s %lu, %lu;", t_names[k],
					(unsigned long)c, (unsigned long)(ns / 1000000));
	}
	if (len > 34)
		host_log("%s\n", line);
}

int hostfs_nx_init(void)
{
	sd = fsdevGetDeviceFileSystem("sdmc");
	if (!sd)
		return -1;
	mutexInit(&lock);
	load_links();
	empty_trash();
	lkl_hostfs_ops = (struct lkl_hostfs_ops){
		t_hfs_stat, t_hfs_access, t_hfs_open, t_hfs_open_dir, hfs_seek_dir, hfs_read_dir,
		t_hfs_close_file, hfs_replace_file, hfs_close_dir, t_hfs_read, t_hfs_write, hfs_lseek,
		t_hfs_fsync, t_hfs_create, t_hfs_set_attr, hfs_symlink, t_hfs_unlink, hfs_mkdir,
		hfs_rmdir, hfs_readlink, t_hfs_rename, t_hfs_rename2, hfs_statfs,
	};
	return 0;
}

/* Whether a folder of the card is there. */
int hostfs_nx_exists(const char *path)
{
	FsDirEntryType type;

	return sd && R_SUCCEEDED(fsFsGetEntryType(sd, path, &type)) && type == FsDirEntryType_Dir;
}

/* A folder of the card and all in it; links listed under it go too. */
int hostfs_nx_remove_tree(const char *path)
{
	int changed = 0;

	if (!sd)
		return -1;
	mutexLock(&lock);
	for (int i = 0; i < link_count;)
		if (under(links[i].path, path)) {
			drop_link(&links[i]);
			changed = 1;
		} else {
			i++;
		}
	if (changed)
		save_links();
	mutexUnlock(&lock);
	return R_FAILED(fsFsDeleteDirectoryRecursively(sd, path)) ? -1 : 0;
}

/* A folder of the card, made if it is not there. */
int hostfs_nx_mkdirs(const char *path)
{
	char part[PATH_LEN];

	for (const char *p = path + 1;; p++) {
		if (*p == '/' || !*p) {
			snprintf(part, sizeof(part), "%.*s", (int)(p - path), path);
			fsFsCreateDirectory(sd, part);
			if (!*p)
				break;
		}
	}
	return 0;
}
