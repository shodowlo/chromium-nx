#!/bin/bash
# Rebuilds the LKL kernel gx links against, with the options gx needs on top
# of LKL's defconfig.
#
#   docker exec lkl-gx bash /project/gx/build-lkl.sh
set -eu

LINUX=${LINUX:-/work/linux}
CROSS=aarch64-linux-gnu-
# UNIX: AF_UNIX sockets and socketpair(), which curl's resolver thread needs.
# The others are what Chromium expects of any Linux: timerfd, signalfd, file
# locks, inotify, and fadvise/madvise.
OPTIONS="UNIX TIMERFD SIGNALFD FILE_LOCKING INOTIFY_USER ADVISE_SYSCALLS SYSVIPC"
# What LKL's defconfig has and gx has no use for. Each of these keeps threads
# of the kernel waiting, every one of them a thread of the host, and Horizon
# gives an application 96 threads in all: a browser needs most of them.
WITHOUT="XFS_FS BTRFS_FS BLK_DEV_NVME NVME_CORE IPV6"

cd "$LINUX"
# gx's own changes to the kernel (what they are for is said in the patch).
if ! git apply -R --check /project/gx/lkl-gx.patch 2>/dev/null; then
	git checkout arch/lkl/kernel/syscalls.c arch/lkl/kernel/threads.c
	git apply /project/gx/lkl-gx.patch
fi
build() { make -C tools/lkl CROSS_COMPILE=$CROSS -j"$(nproc)" >/tmp/lkl-build.log 2>&1 ||
	{ tail -n 40 /tmp/lkl-build.log; exit 1; }; }

# The first build is what creates .config from the defconfig.
[ -e .config ] || build
for opt in $OPTIONS; do
	scripts/config --enable "$opt"
done
for opt in $WITHOUT; do
	scripts/config --disable "$opt"
done
make ARCH=lkl olddefconfig >/dev/null
for opt in $OPTIONS; do
	grep -q "^CONFIG_$opt=y" .config || { echo "CONFIG_$opt did not stick"; exit 1; }
done
build
ls -la tools/lkl/liblkl.a
