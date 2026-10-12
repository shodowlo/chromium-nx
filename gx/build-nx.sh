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
# DIST=<directory> puts the SD card's contents elsewhere (the Steam client's
# images are over 4 GB).
DIST=${DIST:-$SRC/dist-nx}
PC_OUT=${PC_OUT:-/work/gx-out}
PC_SYSROOT=$PC_OUT/sysroot
CROSS=aarch64-none-elf-
# UNIX: AF_UNIX sockets and socketpair(), as in build-lkl.sh.
OPTIONS="UNIX TIMERFD SIGNALFD FILE_LOCKING INOTIFY_USER ADVISE_SYSCALLS SYSVIPC"
# And without these, as in build-lkl.sh: threads of the kernel nothing here
# uses, out of the 96 an application may have.
WITHOUT="XFS_FS BTRFS_FS BLK_DEV_NVME NVME_CORE IPV6"

mkdir -p "$OUT/obj" "$OUT/lkl"

echo "== kernel"
if [ ! -d "$LINUX" ]; then
	git clone -q file:///work/linux "$LINUX"
fi
# gx's own changes to the kernel (what they are for is said in the patch).
if ! git -C "$LINUX" apply -R --check /project/gx/lkl-gx.patch 2>/dev/null; then
	git -C "$LINUX" checkout arch/lkl/kernel/syscalls.c arch/lkl/kernel/threads.c
	git -C "$LINUX" apply /project/gx/lkl-gx.patch
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

echo "== gx"
ARCH="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
# -iquote: LKL's host sources find their own headers, and nothing else does.
# The arena is as large as on the PC (Chromium fills 120 GB of addresses) if
# the console has that much to reserve.
CFLAGS="-g -O2 -Wall -ffunction-sections $ARCH -D__SWITCH__ -DGX_ARENA_SIZE=(256UL<<30) \
	-I$DEVKITPRO/libnx/include -I$PORTLIBS/include -I$OUT/include -I$SRC/nx -I$SRC \
	-iquote $OUT/lkl"
objs=
for f in "$SRC"/tramp.S "$SRC"/nx/exception.S "$SRC"/gmem.c "$SRC"/gelf.c "$SRC"/gsys.c "$SRC"/nat.c "$SRC"/cdp.c "$SRC"/wl.c "$SRC"/glwin.c "$SRC"/video.c "$SRC"/host_nx.c \
	"$OUT"/gl_table.c "$SRC"/../nro/source/nx_host.c "$OUT"/lkl/iomem.c "$OUT"/lkl/jmp_buf.c \
	"$OUT"/lkl/utils.c "$OUT"/lkl/virtio.c "$OUT"/lkl/virtio_net.c "$OUT"/lkl/virtio_blk.c \
	"$OUT"/lkl/net.c "$OUT"/lkl/fs.c; do
	o=$OUT/obj/$(basename "${f%.*}").o
	${CROSS}gcc $CFLAGS -c "$f" -o "$o"
	objs="$objs $o"
done
# What handles a fault does without the vector registers (nx/plain.c): the
# memory routines for the whole host, and the guest's memory.
PLAIN="-mgeneral-regs-only -fno-builtin -fno-tree-loop-distribute-patterns"
${CROSS}gcc $CFLAGS $PLAIN -c "$SRC/nx/plain.c" -o "$OUT/obj/plain.o"
${CROSS}gcc $CFLAGS -mgeneral-regs-only -fno-tree-loop-distribute-patterns -c "$SRC/gmem.c" -o "$OUT/obj/gmem.o"
objs="$OUT/obj/plain.o $objs"
# Mesa is C++ inside.
${CROSS}g++ -specs=$DEVKITPRO/libnx/switch.specs -g $ARCH -Wl,-Map,"$OUT/gx.map" \
	$objs "$OUT/lkl-nro.o" -L$PORTLIBS/lib -L$DEVKITPRO/libnx/lib \
	-lavcodec -ldav1d -lswresample -lavutil \
	-lEGL -lGLESv2 -lglapi -ldrm_nouveau -lpng -lturbojpeg -lz -lnx -lm -o "$OUT/gx.elf"
nacptool --create "Chromium NX" "shodowlo" "0.1" "$OUT/gx.nacp"
elf2nro "$OUT/gx.elf" "$OUT/gx.nro" --nacp="$OUT/gx.nacp" --icon="$SRC/nx/icon.jpg"

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
cp "$OUT/gx.nro" "$APP/chromium-nx.nro"
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
# Chromium, if build-chromium.sh made its root: the one with a window, in the
# image made for the console (without what only the PC's tests use).
if [ -e "$PC_OUT/chromium-switch.ext4" ]; then
	cp "$PC_OUT/chromium-switch.ext4" "$APP/chromium.ext4"
	[ -z "${DEV:-}" ] || {
		# A page that changes at every frame and says how many frames a
		# second the page itself gets (requestAnimationFrame).
		A='data:text/html,<body%20style="margin:0;background:%23204080;color:white;font:80px%20sans-serif"><div%20id=d%20style="position:absolute;top:300px;width:300px;height:300px;background:linear-gradient(red,lime)"></div><p%20id=o%20style="margin:100px%20200px">...</p><script>t=0;n=0;l=0;function%20f(now){t+=6;n++;if(now-l>=1000){o.textContent=n+"%20frames/s%20in%20the%20page";n=0;l=now}d.style.left=(t%251000)+"px";requestAnimationFrame(f)}requestAnimationFrame(f)</script>'
		# On gx's Wayland display (wl.c), with the debugging pipe for
		# what a window does not say (cdp.c).
		W="-w -b @chromium.ext4 /usr/lib/chromium/chromium-shell --no-sandbox --single-process --no-zygote --disable-gpu --js-flags=--jitless --ozone-platform=wayland --enable-spatial-navigation --remote-debugging-pipe"
		# The same drawing with the GPU: ANGLE inside Chromium, on the
		# console's own EGL and GLES through gx's libEGL and libGLESv2.
		G="${W/--disable-gpu/--use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist --disable-gpu-sandbox --in-process-gpu}"
		echo "[Chromium GPU: the test page] $G file:///usr/share/gx/page.html"
		echo "[Chromium GPU: speed test] $G $A"
		echo "[Chromium GPU: wikipedia.org] $G https://www.wikipedia.org/"
		echo "[Chromium GPU: google.com] $G https://www.google.com/"
		# And with V8's compilers: memory writable and executable by
		# turns (gmem.c) is what they needed.
		J="${G/ --js-flags=--jitless/}"
		echo "[Chromium GPU+JIT: speed test] $J $A"
		echo "[Chromium GPU+JIT: wikipedia.org] $J https://www.wikipedia.org/"
		echo "[Chromium GPU+JIT: google.com] $J https://www.google.com/"
		echo "[Chromium GPU+JIT: youtube.com] $J https://www.youtube.com/"
		echo "[Chromium WINDOW: google.com] $W https://www.google.com/"
		echo "[Chromium WINDOW: wikipedia.org] $W https://www.wikipedia.org/"
		echo "[Chromium WINDOW: speed test] $W $A"
		echo "[Chromium WINDOW: the test page] $W file:///usr/share/gx/page.html"
	} >>"$APP/programs.txt"
fi
# The whole browser, a trial next to chromium-shell: its own image, and a menu
# of two to choose from (the first is what runs without a menu).
if [ -e "$PC_OUT/chromium-full-switch.ext4" ] && [ -e "$PC_OUT/chromium-full.cmd" ]; then
	cp "$PC_OUT/chromium-full-switch.ext4" "$APP/chromium-full.ext4"
	if [ -z "${DEV:-}" ]; then
		S="-w -b @chromium.ext4 /usr/lib/chromium/chromium-shell --no-sandbox --single-process --no-zygote --use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist --disable-gpu-sandbox --in-process-gpu --ozone-platform=wayland --enable-spatial-navigation --remote-debugging-pipe https://www.google.com/"
		echo "[Chromium NX (chromium-shell, GPU)] $S" >"$APP/programs.txt"
	fi
	# Its profile: an empty filesystem, packed, that the program unpacks
	# next to itself the first time (profile.ext4, which is then the user's:
	# nothing here replaces it).
	[ ! -e "$PC_OUT/profile.ext4.gz" ] || cp "$PC_OUT/profile.ext4.gz" "$APP/profile.ext4.gz"
	# -c3: the browser is told of the three processors it runs on. Told of
	# one (what gx says by default) it decodes video on two threads and
	# keeps its other pools as small.
	F="-w -b -c3 @chromium-full.ext4 =profile.ext4 $(cat "$PC_OUT/chromium-full.cmd")"
	echo "[Chromium, the whole browser (GPU)] $F --use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist --disable-gpu-sandbox --in-process-gpu https://www.google.com/" >>"$APP/programs.txt"
	echo "[Chromium, the whole browser (no GPU)] $F --disable-gpu https://www.google.com/" >>"$APP/programs.txt"
fi
# The Steam client and what to try before it, if build-steam.sh made them: the
# root with an X server, the client's home (under 4 GB, for a card in FAT32;
# the console writes to it), gx's tests as an image, and their lines in the
# menu after the browser's.
if [ -e "$PC_OUT/steam-root.ext4" ] && [ -e "$PC_OUT/steam-home.ext4" ] && [ -e "$PC_OUT/steam.cmd" ]; then
	cp "$PC_OUT/steam-root.ext4" "$PC_OUT/steam-home.ext4" "$PC_OUT/tests.ext4" "$APP/"
	cat "$PC_OUT/steam.cmd" >>"$APP/programs.txt"
fi
ls -la "$APP"
echo "copy the contents of gx/dist-nx to the root of the SD card"
