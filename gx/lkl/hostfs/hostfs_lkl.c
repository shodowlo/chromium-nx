// SPDX-License-Identifier: GPL-2.0
/*
 * gx: what UML's hostfs_user.c does, asked of the program LKL runs in: it
 * fills lkl_hostfs_ops before the kernel starts (gx/hostfs_nx.c), and the
 * kernel calls through it, holding its one processor. Errors come back as
 * negative Linux error numbers.
 */
#include <linux/errno.h>
#include <linux/types.h>
#include "hostfs.h"

struct lkl_hostfs_ops {
	int (*stat_file)(const char *path, struct hostfs_stat *p, int fd);
	int (*access_file)(char *path, int r, int w, int x);
	int (*open_file)(char *path, int r, int w, int append);
	void *(*open_dir)(char *path, int *err_out);
	void (*seek_dir)(void *stream, unsigned long long pos);
	char *(*read_dir)(void *stream, unsigned long long *pos_out, unsigned long long *ino_out,
			  int *len_out, unsigned int *type_out);
	void (*close_file)(void *stream);
	int (*replace_file)(int oldfd, int fd);
	void (*close_dir)(void *stream);
	int (*read_file)(int fd, unsigned long long *offset, char *buf, int len);
	int (*write_file)(int fd, unsigned long long *offset, const char *buf, int len);
	int (*lseek_file)(int fd, long long offset, int whence);
	int (*fsync_file)(int fd, int datasync);
	int (*file_create)(char *name, int mode);
	int (*set_attr)(const char *file, struct hostfs_iattr *attrs, int fd);
	int (*make_symlink)(const char *from, const char *to);
	int (*unlink_file)(const char *file);
	int (*do_mkdir)(const char *file, int mode);
	int (*do_rmdir)(const char *file);
	int (*do_readlink)(char *file, char *buf, int size);
	int (*rename_file)(char *from, char *to);
	int (*rename2_file)(char *from, char *to, unsigned int flags);
	int (*do_statfs)(char *root, long *bsize_out, long long *blocks_out, long long *bfree_out,
			 long long *bavail_out, long long *files_out, long long *ffree_out,
			 void *fsid_out, int fsid_size, long *namelen_out);
};

/* The program's: an undefined symbol of lkl.o, resolved when it is linked. */
extern struct lkl_hostfs_ops lkl_hostfs_ops;

#define HOST(f, ...) (lkl_hostfs_ops.f ? lkl_hostfs_ops.f(__VA_ARGS__) : -ENOSYS)

int stat_file(const char *path, struct hostfs_stat *p, int fd)
{
	return HOST(stat_file, path, p, fd);
}

int access_file(char *path, int r, int w, int x)
{
	return HOST(access_file, path, r, w, x);
}

int open_file(char *path, int r, int w, int append)
{
	return HOST(open_file, path, r, w, append);
}

void *open_dir(char *path, int *err_out)
{
	if (!lkl_hostfs_ops.open_dir) {
		*err_out = ENOSYS;
		return NULL;
	}
	return lkl_hostfs_ops.open_dir(path, err_out);
}

void seek_dir(void *stream, unsigned long long pos)
{
	lkl_hostfs_ops.seek_dir(stream, pos);
}

char *read_dir(void *stream, unsigned long long *pos_out, unsigned long long *ino_out,
	       int *len_out, unsigned int *type_out)
{
	return lkl_hostfs_ops.read_dir(stream, pos_out, ino_out, len_out, type_out);
}

void close_file(void *stream)
{
	lkl_hostfs_ops.close_file(stream);
}

int replace_file(int oldfd, int fd)
{
	return HOST(replace_file, oldfd, fd);
}

void close_dir(void *stream)
{
	lkl_hostfs_ops.close_dir(stream);
}

int read_file(int fd, unsigned long long *offset, char *buf, int len)
{
	return HOST(read_file, fd, offset, buf, len);
}

int write_file(int fd, unsigned long long *offset, const char *buf, int len)
{
	return HOST(write_file, fd, offset, buf, len);
}

int lseek_file(int fd, long long offset, int whence)
{
	return HOST(lseek_file, fd, offset, whence);
}

int fsync_file(int fd, int datasync)
{
	return HOST(fsync_file, fd, datasync);
}

int file_create(char *name, int mode)
{
	return HOST(file_create, name, mode);
}

int set_attr(const char *file, struct hostfs_iattr *attrs, int fd)
{
	return HOST(set_attr, file, attrs, fd);
}

int make_symlink(const char *from, const char *to)
{
	return HOST(make_symlink, from, to);
}

int unlink_file(const char *file)
{
	return HOST(unlink_file, file);
}

int do_mkdir(const char *file, int mode)
{
	return HOST(do_mkdir, file, mode);
}

int hostfs_do_rmdir(const char *file)
{
	return HOST(do_rmdir, file);
}

/* No devices nor second names on a memory card. */
int do_mknod(const char *file, int mode, unsigned int major, unsigned int minor)
{
	return -EPERM;
}

int link_file(const char *to, const char *from)
{
	return -EPERM;
}

int hostfs_do_readlink(char *file, char *buf, int size)
{
	return HOST(do_readlink, file, buf, size);
}

int rename_file(char *from, char *to)
{
	return HOST(rename_file, from, to);
}

int rename2_file(char *from, char *to, unsigned int flags)
{
	return HOST(rename2_file, from, to, flags);
}

int do_statfs(char *root, long *bsize_out, long long *blocks_out, long long *bfree_out,
	      long long *bavail_out, long long *files_out, long long *ffree_out,
	      void *fsid_out, int fsid_size, long *namelen_out)
{
	return HOST(do_statfs, root, bsize_out, blocks_out, bfree_out, bavail_out, files_out,
		    ffree_out, fsid_out, fsid_size, namelen_out);
}
