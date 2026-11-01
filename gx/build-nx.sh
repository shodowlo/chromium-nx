#!/bin/bash
# Builds gx for the Nintendo Switch (an NRO for the Homebrew Menu, "Chromium NX")
# and lays out what goes on the SD card in gx/dist-nx.
#
#   docker exec lkl-gx-nx bash /project/gx/build-nx.sh
#
# The container is devkitpro/devkita64 with the kernel's build tools, this
# repository on /project and the "lklwork" volume on /work:
#   docker run -d --name lkl-gx-nx -v <repository>:/project -v lklwork:/work \
#       devkitpro/devkita64:latest sleep infinity
#   docker exec lkl-gx-nx apt-get install -y flex bison bc libelf-dev cpio rsync build-essential
# The guest programs come from the sysroot build-pc.sh made (/work/gx-out).
set -eu

export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH

SRC=$(cd "$(dirname "$0")" && pwd)
LINUX=${LINUX:-/work/linux-nx}
LKL=$LINUX/tools/lkl
OUT=${OUT:-/work/gx-nx}
DIST=$SRC/dist-nx
PC_OUT=${PC_OUT:-/work/gx-out}
PC_SYSROOT=$PC_OUT/sysroot
CROSS=aarch64-none-elf-
# UNIX: AF_UNIX sockets and socketpair(), as in build-lkl.sh.
OPTIONS="UNIX TIMERFD SIGNALFD FILE_LOCKING INOTIFY_USER ADVISE_SYSCALLS HOSTFS"
# The pad as a game controller for pages (the overlay's Controller mode): a
# uinput device, which Chromium reads through joydev (host_nx.c).
OPTIONS="$OPTIONS INPUT_EVDEV INPUT_JOYDEV INPUT_MISC INPUT_UINPUT"
# And without these, as in build-lkl.sh: threads of the kernel nothing here
# uses, out of the 96 an application may have.
WITHOUT="XFS_FS BTRFS_FS BLK_DEV_NVME NVME_CORE IPV6"

mkdir -p "$OUT/obj" "$OUT/lkl"

echo "== kernel"
if [ ! -d "$LINUX" ]; then
	# A copy of the PC's kernel tree (build-lkl.sh), built apart.
	if [ -d /work/linux/.git ]; then
		git clone -q file:///work/linux "$LINUX"
	else
		cp -a /work/linux "$LINUX"
		make -C "$LINUX" mrproper >/dev/null 2>&1 || true
	fi
fi
# hostfs, UML's filesystem of the host's files, made to build in LKL: the
# browser's profile and downloads are folders of the SD card (gx/hostfs_nx.c
# is the other half).
if ! grep -q "^config HOSTFS" "$LINUX/arch/lkl/Kconfig"; then
	printf '\nconfig HOSTFS\n\tbool "Host filesystem (gx)"\n\tdefault y\n' >>"$LINUX/arch/lkl/Kconfig"
	rm -f "$LKL/lib/lkl.o" "$LINUX/.config"
fi
for f in Makefile os.h init.h kern.h hostfs_lkl.c; do
	if ! cmp -s "$SRC/lkl/hostfs/$f" "$LINUX/fs/hostfs/$f"; then
		cp "$SRC/lkl/hostfs/$f" "$LINUX/fs/hostfs/$f"
		rm -f "$LKL/lib/lkl.o"
	fi
done
# UML's hostfs forgets every name as soon as it is unused, so that what the
# host changes behind it is seen: each lookup went to the card (3000 a page
# load). Nothing else writes these folders here: names are kept.
if grep -q "sb->s_d_op = &simple_dentry_operations;" "$LINUX/fs/hostfs/hostfs_kern.c"; then
	sed -i 's|\tsb->s_d_op = &simple_dentry_operations;|\t/* gx: names kept (build-nx.sh) */|' \
		"$LINUX/fs/hostfs/hostfs_kern.c"
	rm -f "$LKL/lib/lkl.o"
fi
for opt in $OPTIONS; do
	grep -q "^CONFIG_$opt=y" "$LINUX/arch/lkl/configs/defconfig" ||
		echo "CONFIG_$opt=y" >>"$LINUX/arch/lkl/configs/defconfig"
done
for opt in $WITHOUT; do
	if ! grep -q "^# CONFIG_$opt is not set" "$LINUX/arch/lkl/configs/defconfig"; then
		sed -i "/^CONFIG_$opt=/d" "$LINUX/arch/lkl/configs/defconfig"
		echo "# CONFIG_$opt is not set" >>"$LINUX/arch/lkl/configs/defconfig"
		# The kernel is to be made again with it.
		rm -f "$LKL/lib/lkl.o" "$LINUX/.config"
	fi
done
if [ ! -e "$LKL/lib/lkl.o" ]; then
	make -C "$LKL" CROSS_COMPILE=$CROSS -j"$(nproc)" "$LKL/lib/lkl.o" >"$OUT/kernel.log" 2>&1 ||
		{ tail -n 40 "$OUT/kernel.log"; exit 1; }
fi
for opt in $OPTIONS; do
	grep -q "^CONFIG_$opt=y" "$LINUX/.config" ||
		{ echo "CONFIG_$opt is not in the kernel: remove $LKL/lib/lkl.o and $LINUX/.config"; exit 1; }
done
for opt in $WITHOUT; do
	! grep -q "^CONFIG_$opt=y" "$LINUX/.config" ||
		{ echo "CONFIG_$opt is still in the kernel: remove $LKL/lib/lkl.o and $LINUX/.config"; exit 1; }
done
if [ ! -e "$OUT/lkl-nro.o" ] || [ "$LKL/lib/lkl.o" -nt "$OUT/lkl-nro.o" ]; then
	# Without debug information, and with .rodata writable: the kernel's
	# holds pointers that are relocated when the NRO is loaded, and the
	# loader maps read-only data before it relocates (scripts/build-nro.sh).
	${CROSS}objcopy --strip-debug "$LKL/lib/lkl.o" "$OUT/lkl-stripped.o"
	${CROSS}objcopy --set-section-flags .rodata=alloc,load,contents,data \
		--rename-section .rodata=.kernel.rodata "$OUT/lkl-stripped.o" "$OUT/lkl-nro.o"
	rm "$OUT/lkl-stripped.o"
fi

# LKL's headers, and the parts of its host library that are not POSIX.
rm -rf "$OUT/include"
cp -r "$LKL/include" "$OUT/include"
printf '#define LKL_HOST_CONFIG_AARCH64 y\n#define LKL_HOST_CONFIG_VIRTIO_NET y\n#define LKL_HOST_CONFIG_VIRTIO_BLK y\n' \
	>"$OUT/include/lkl_autoconf.h"
cp "$LKL"/lib/{iomem.c,iomem.h,jmp_buf.c,jmp_buf.h,utils.c,virtio.c,virtio.h,virtio_net.c,virtio_blk.c,net.c,fs.c,endian.h} \
	"$OUT/lkl/"
# The disk's requests on threads of their own rather than under the kernel's
# processor (gx/lkl/virtio-defer.c says why): LKL's virtio_blk.c is replaced,
# and what it needs is added to virtio.c.
cp "$SRC/lkl/virtio_blk.c" "$OUT/lkl/virtio_blk.c"
cat "$SRC/lkl/virtio-defer.c" >>"$OUT/lkl/virtio.c"

# The EGL and GLES functions the guest can reach: those of its thunk libraries
# (lists made by build-pc.sh) that Mesa for the Switch has.
PORTLIBS=$DEVKITPRO/portlibs/switch
{
	echo "/* Made by build-nx.sh. */"
	echo "struct gx_gl_entry { const char *name; void (*fn)(void); };"
	names=$(comm -12 \
		<( (sed 's/GX_THUNK(\(.*\))/\1/' "$PC_OUT/egl.list" "$PC_OUT/gles.list"; echo eglGetProcAddress) | sort -u) \
		<(${CROSS}nm "$PORTLIBS/lib/libEGL.a" "$PORTLIBS/lib/libGLESv2.a" 2>/dev/null |
			awk '$2 == "T" { print $3 }' | sort -u))
	for n in $names; do echo "void $n(void);"; done
	echo "const struct gx_gl_entry gx_gl_table[] = {"
	for n in $names; do echo "	{ \"$n\", $n },"; done
	echo "	{ 0, 0 },"
	echo "};"
} >"$OUT/gl_table.c"
echo "$(grep -c '^	{ "' "$OUT/gl_table.c") EGL and GLES functions"

# The profile's empty image, packed (build-chromium.sh makes it): in the
# program, which unpacks it next to itself the first time (profile.ext4), so
# that nothing else has to be on the card.
[ -e "$PC_OUT/profile.ext4.gz" ] || { echo "run build-chromium.sh first: it makes profile.ext4.gz"; exit 1; }
python3 - "$PC_OUT/profile.ext4.gz" "$OUT/profile_gz.c" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('const unsigned char gx_profile_gz[] = {%s};\n' % ','.join(str(b) for b in data))
    f.write('const unsigned int gx_profile_gz_len = %d;\n' % len(data))
PY

echo "== gx"
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
# -iquote: LKL's host sources find their own headers, and nothing else does.
# The arena is as large as on the PC (Chromium fills 120 GB of addresses) if
# the console has that much to reserve.
CFLAGS="-g -O2 -Wall -ffunction-sections $ARCH -D__SWITCH__ -DGX_ARENA_SIZE=(256UL<<30) \
	-I$DEVKITPRO/libnx/include -I$PORTLIBS/include -I$PORTLIBS/include/freetype2 -I$OUT/include -I$SRC/nx -I$SRC \
	-iquote $OUT/lkl"
objs=
for f in "$SRC"/tramp.S "$SRC"/nx/exception.S "$SRC"/gmem.c "$SRC"/gelf.c "$SRC"/gsys.c "$SRC"/nat.c "$SRC"/cdp.c "$SRC"/wl.c "$SRC"/glwin.c "$SRC"/video.c "$SRC"/overlay.c "$SRC"/hostfs_nx.c "$SRC"/host_nx.c \
	"$OUT"/gl_table.c "$OUT"/profile_gz.c "$SRC"/../nro/source/nx_host.c "$OUT"/lkl/iomem.c "$OUT"/lkl/jmp_buf.c \
	"$OUT"/lkl/utils.c "$OUT"/lkl/virtio.c "$OUT"/lkl/virtio_net.c "$OUT"/lkl/virtio_blk.c \
	"$OUT"/lkl/net.c "$OUT"/lkl/fs.c; do
	o=$OUT/obj/$(basename "${f%.*}").o
	${CROSS}gcc $CFLAGS -c "$f" -o "$o"
	objs="$objs $o"
done
# The launcher (gx/launcher/): a home screen, the settings and the updates.
# Part of the same program, which starts as the launcher and is loaded again
# as the browser (--browser, host_nx.c): the browser has every byte it would
# have without it.
mkdir -p "$OUT/launcher"
python3 - "$SRC/nx/icon.jpg" "$OUT/launcher/icon.c" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
with open(sys.argv[2], 'w') as f:
    f.write('const unsigned char icon_jpg[] = {%s};\n' % ','.join(str(b) for b in data))
    f.write('const unsigned int icon_jpg_len = %d;\n' % len(data))
PY
LCFLAGS="-O2 -Wall $ARCH -D__SWITCH__ -I$DEVKITPRO/libnx/include -I$PORTLIBS/include -I$PORTLIBS/include/freetype2"
# For a test of the updates: UPDATE_RELEASES=http://<pc>:<port>/releases/ (a
# folder made by scripts/release.py --local) and GX_VERSION=<version>.
[ -z "${UPDATE_RELEASES:-}" ] || LCFLAGS="$LCFLAGS -DUPDATE_RELEASES=\"$UPDATE_RELEASES\""
[ -z "${GX_VERSION:-}" ] || LCFLAGS="$LCFLAGS -DGX_VERSION=\"$GX_VERSION\""
for f in "$SRC/launcher/main.c" "$SRC/launcher/update.c" "$OUT/launcher/icon.c"; do
	o=$OUT/launcher/$(basename "${f%.*}").o
	${CROSS}gcc $LCFLAGS -c "$f" -o "$o"
	objs="$objs $o"
done
VERSION=${GX_VERSION:-$(sed -n 's/^#define GX_VERSION "\(.*\)"$/\1/p' "$SRC/version.h")}
# Mesa is C++ inside.
${CROSS}g++ -specs=$DEVKITPRO/libnx/switch.specs -g $ARCH -Wl,-Map,"$OUT/gx.map" -Wl,--wrap=lkl_syscall \
	$objs "$OUT/lkl-nro.o" -L$PORTLIBS/lib -L$DEVKITPRO/libnx/lib \
	-lavcodec -ldav1d -lswresample -lavutil \
	-lEGL -lGLESv2 -lglapi -ldrm_nouveau -lcurl -lfreetype -lharfbuzz -lfreetype -lbz2 -lpng -lturbojpeg -lz \
	-lnx -lm -o "$OUT/gx.elf"
nacptool --create "Chromium NX" "shodowlo" "$VERSION" "$OUT/gx.nacp"
elf2nro "$OUT/gx.elf" "$OUT/chromium-nx.nro" --nacp="$OUT/gx.nacp" --icon="$SRC/nx/icon.jpg"

# What the browser is started with on the console, on top of
# chromium-full.cmd (build-chromium.sh):
# - AutomationControlled: --remote-debugging-pipe (cdp.c) makes
#   navigator.webdriver true, and YouTube then takes the browser for a robot
#   and stops its videos.
# - The features below work in the background for nothing here: models
#   downloaded and run on the device (the log showed it trying to load one),
#   hints fetched for pages, casting to other screens.
# - BackForwardCache: a page left stays in memory, in case Back comes; a
#   game of a page left for a video site in the same tab took its 1.5 GB
#   along, and the guest ran out.
# - GPU memory: textures come out of the host's heap (448 MB, also the GPU
#   driver's and the video decoder's), which Chromium does not know of; a page
#   of thousands of images filled it, and the GPU's context was lost. And no
#   texture over 4096 pixels a side (the screen is 1280): the driver wants
#   each in one piece of the heap, and a texture refused (while the heap had
#   229 MB free, in pieces) lost the context too.
# - No Sparkplug (V8's quick compiler): it writes code in small batches,
#   and a page of code here is writable or executable by turns, each turn a
#   fault and a remapping (gmem.c). YouTube went from 5000 turns to 1900,
#   from 2.2 s of them to 1.1, its video stuttering less, and no page or
#   benchmark was slower for it: the faster compilers do the hot code.
NX_FLAGS="--disable-blink-features=AutomationControlled \
--disable-features=OptimizationHints,OptimizationGuideModelDownloading,OptimizationGuideOnDeviceModel,OnDeviceModelBackgroundDownload,MediaRouter,DialMediaRouteProvider,BackForwardCache \
--force-gpu-mem-available-mb=128 --force-gpu-mem-discardable-limit-mb=32 --force-max-texture-size=4096 --disable-gl-extensions=GL_EXT_disjoint_timer_query,GL_EXT_disjoint_timer_query_webgl2,GL_ARB_timer_query,GL_EXT_timer_query \
--js-flags=--no-sparkplug"

echo "== SD card"
# Everything is in one folder of the card, sdmc:/switch/chromium-nx: the
# program, Chromium's root image, and the log it writes. Started as it is, it
# runs Chromium; with DEV=1 the folder also gets programs.txt, a menu of what
# to run, and the test programs build-pc.sh made.
APP=$DIST/switch/chromium-nx
# A log left there from the console is put aside first: all of this is made anew.
for log in "$APP"/*.log; do
	[ ! -e "$log" ] || mv "$log" "$SRC/../$(basename "$log")"
done
rm -rf "$DIST"
mkdir -p "$APP"
# The tile opens chromium-nx.nro: the launcher, then the browser.
cp "$OUT/chromium-nx.nro" "$APP/chromium-nx.nro"
if [ -n "${DEV:-}" ]; then
	[ -e "$PC_SYSROOT/bin/hello" ] ||
		{ echo "run build-pc.sh first: it makes the guest programs"; exit 1; }
	cp -r "$PC_SYSROOT" "$APP/sysroot"
	cat >"$APP/programs.txt" <<'EOF'
# One command per line, offered as a menu. -t traces the syscalls.
/bin/hello
/bin/threads
/bin/dl
/bin/timers
/bin/gles 600
/bin/net
/bin/curl -sS -m 60 https://example.com/
-t /bin/hello
EOF
fi
# Chromium: the whole browser, in its own image (chromium-full.ext4). The
# launcher starts it with the command line of host_nx.c (DEFAULT_COMMAND),
# which must say what this build of the image needs: the line below, which
# the build checks. With DEV=1, programs.txt offers it too, after the tests.
if [ -e "$PC_OUT/chromium-full-switch.ext4" ] && [ -e "$PC_OUT/chromium-full.cmd" ]; then
	cp "$PC_OUT/chromium-full-switch.ext4" "$APP/chromium-full.ext4"
	# Its profile is profile.ext4 next to it, and its downloads Downloads/
	# (host_nx.c, mount_home()). -c3: the browser is told of the three
	# processors it runs on.
	F="-w -b -c3 @chromium-full.ext4 =profile.ext4 $(cat "$PC_OUT/chromium-full.cmd") $NX_FLAGS"
	GPU="--use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist --disable-gpu-sandbox --in-process-gpu"
	LINE="$F $GPU https://www.google.com/"
	strings -n 200 "$OUT/gx.elf" | grep -qxF -- "$LINE" ||
		echo "warning: DEFAULT_COMMAND (host_nx.c) is not the image's command line: $LINE"
	if [ -n "${DEV:-}" ]; then
		echo "[Chromium NX] $LINE" >>"$APP/programs.txt"
		echo "[Chromium NX (no GPU)] $F --disable-gpu https://www.google.com/" >>"$APP/programs.txt"
	fi
fi
ls -la "$APP"
echo "copy the contents of gx/dist-nx to the root of the SD card"
