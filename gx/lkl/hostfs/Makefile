# gx: hostfs (User-Mode Linux's filesystem of the host's files) built into
# LKL: build-nx.sh puts this file, the three headers UML has and LKL lacks,
# and hostfs_lkl.c in fs/hostfs of the kernel's tree. hostfs_lkl.c hands
# what UML's hostfs_user.c does to the host, through lkl_hostfs_ops.
obj-$(CONFIG_HOSTFS) += hostfs.o
hostfs-objs := hostfs_kern.o hostfs_lkl.o
ccflags-y += -I$(src)
