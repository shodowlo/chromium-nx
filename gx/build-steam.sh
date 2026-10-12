#!/bin/bash
# Makes what the native arm64 Steam client runs from under gx: a root (a
# Debian with an X server for a Wayland display, Xwayland), and the home the
# client lives in, as two ext4 images; and a small image of gx's test
# programs, to try on a console what Steam needs before trying Steam.
#
# In two steps, in two containers that share the "lklwork" volume on /work:
#
#   docker exec -i steam-rootfs bash -s tree < gx/build-steam.sh
#       the root, as a tree of files (/work/steam/sroot), out of the container
#       itself: debian:trixie for arm64 with
#       apt-get install --no-install-recommends xwayland xkb-data x11-xkb-utils x11-apps
#       and what the client is linked against;
#   docker exec lkl-gx bash /project/gx/build-steam.sh
#       the images, in /work/gx-out, for build-nx.sh to put on the SD card.
#       Run build-pc.sh first: xrun and libgxfork come from it.
#
# The client itself is not made here: /work/steam/home.ext4 is a home in which
# it was installed and has updated itself (run under gx with the network,
# once; see run.sh there), and is taken as it is, but for one file: the script
# that starts its browser, which is told to be one process and to draw
# without a GPU (see below).
set -eu
S=/work/steam
OUT=/work/gx-out

if [ "${1:-}" = tree ]; then
	R=$S/sroot
	rm -rf $R
	mkdir -p $R
	cp -a /usr /etc /var $R/
	rm -rf $R/usr/share/doc $R/usr/share/man $R/usr/share/locale $R/usr/share/info \
		$R/var/cache/apt $R/var/lib/apt/lists $R/var/lib/dpkg
	mkdir -p $R/dev $R/proc $R/sys $R/run $R/root $R/tmp/.X11-unix
	chmod 1777 $R/tmp $R/tmp/.X11-unix
	ln -s usr/bin $R/bin
	ln -s usr/lib $R/lib
	ln -s usr/sbin $R/sbin
	# The address at which gx's NAT forwards DNS to the host's resolver.
	rm -f $R/etc/resolv.conf
	echo "nameserver 10.0.2.3" >$R/etc/resolv.conf
	du -sm $R
	exit 0
fi

SRC=$(cd "$(dirname "$0")" && pwd)
R=$S/sroot
[ -d $R/usr ] || { echo "make the root's tree first (see the top of this file)"; exit 1; }
[ -e $OUT/sysroot/bin/xrun ] || { echo "run build-pc.sh first"; exit 1; }

echo "== root"
cp $OUT/sysroot/bin/xrun $R/usr/bin/xrun
cp $OUT/sysroot/lib/libgxfork.so $R/usr/lib/libgxfork.so
# The software GL driver rebuilt with mesa-softpipe.patch (Mesa 25.0.7, softpipe
# only, the Debian version name kept in VERSION), where it has been built: as
# it comes, a context is 200 MB and a Steam client has a context a window.
M=/work/steam/mesa-gx; L=$R/usr/lib/aarch64-linux-gnu
if [ -e $M/libgallium.so ]; then
	cp $M/libgallium.so $L/libgallium-25.0.7-2+deb13u1.so
	cp $M/libGLX_mesa.so $L/libGLX_mesa.so.0.0.0
	cp $M/libEGL_mesa.so $L/libEGL_mesa.so.0.0.0
fi
# What the client asks who connects to it by: see guest/lsof.sh.
cp /project/gx/guest/lsof.sh $R/usr/bin/lsof
chmod 755 $R/usr/bin/lsof
# Mesa's software renderers are linked against LLVM, 120 MB that every
# program that draws would load and none uses the way they are run here.
GALLIUM=$(ls $R/usr/lib/aarch64-linux-gnu/libgallium-*.so | head -n 1)
LLVM=$(aarch64-linux-gnu-objdump -p "$GALLIUM" | awk '/NEEDED.*libLLVM/ { print $2 }')
[ -z "$LLVM" ] || bash "$SRC/llvm-stub.sh" "$GALLIUM" "$R/usr/lib/aarch64-linux-gnu/$LLVM"
rm -f $OUT/steam-root.ext4
mke2fs -q -t ext4 -O ^has_journal -L steam-root -d $R $OUT/steam-root.ext4 $(( $(du -sm $R | cut -f1) * 115 / 100 + 40 ))M
ls -la $OUT/steam-root.ext4

echo "== home"
[ -e $S/home.ext4 ] || { echo "no $S/home.ext4: the client is to be installed first"; exit 1; }
H=$S/home-tree
rm -rf $H
mkdir -p $H
e2fsck -fy $S/home.ext4 >/dev/null 2>&1 || true
debugfs -R "rdump / $H" $S/home.ext4 >/dev/null 2>&1
[ -e $H/Steam/steamrtarm64/steam ] || { echo "$S/home.ext4 has no client in it"; exit 1; }
# The client's browser (steamwebhelper, CEF): one process, since each one
# more is another copy of a 370 MB library in memory that is not shared here;
# no GPU, there is none behind the X server; and what it says on the console
# rather than in a file nobody would see written.
W=$H/Steam/steamrtarm64/steamwebhelper.sh
[ -e $S/steamwebhelper.sh.orig ] && cp $S/steamwebhelper.sh.orig $W
sed -i 's|steamwebhelper "\$@".*|steamwebhelper "$@" --no-zygote --single-process --in-process-gpu --disable-gpu --enable-logging=stderr|' $W
grep -q -- --single-process $W || { echo "steamwebhelper.sh is not as expected"; exit 1; }
chmod 755 $W
rm -rf $H/lost+found
# Under 4 GB: a card in FAT32 takes no larger file. And with room to spare:
# the client refuses to start with less than 250 MB free ("Steam needs 250MB
# of free disk space to update"), counted as an ordinary user counts it, so
# nothing is kept back for root (-m 0) and the image is as large as it may be.
used=$(du -sm $H | cut -f1)
size=4000
[ $(( used + 500 )) -lt $size ] || { echo "the client's home is too large for one file ($used MB used)"; exit 1; }
rm -f $OUT/steam-home.ext4
mke2fs -q -t ext4 -m 0 -L steam-home -d $H $OUT/steam-home.ext4 ${size}M
ls -la $OUT/steam-home.ext4
rm -rf $H

echo "== tests"
T=$S/tests-tree
rm -rf $T
mkdir -p $T/dev $T/proc $T/sys $T/run $T/root $T/tmp $T/etc
cp -a $OUT/sysroot/bin $OUT/sysroot/lib $T/
rm -f $OUT/tests.ext4
# (With room for the file strain reads from.)
mke2fs -q -t ext4 -O ^has_journal -L tests -d $T $OUT/tests.ext4 $(( $(du -sm $T | cut -f1) + 64 ))M
ls -la $OUT/tests.ext4
rm -rf $T

# How to start each, for build-nx.sh to put in the console's menu. -L, -P:
# memory committed when first used and guest threads sharing the host's,
# without which neither the memory nor the threads of the console are enough;
# -k, -m: less for the kernel and for the host than a browser with a GPU and a
# video decoder needs, the rest is the guest's.
X="-w -c3 -L -P52 -k416 -m176 -eLD_PRELOAD=/lib/libgxfork.so -eGALLIUM_DRIVER=softpipe -eDRAW_USE_LLVM=0 -eLANG=en_US.UTF-8"
{
	echo "[Steam test 1: processes] -L -P16 -eLD_PRELOAD=/lib/libgxfork.so @tests.ext4 /bin/spawn"
	echo "[Steam test 2: newer processors' instructions] -L -P16 @tests.ext4 /bin/atomics"
	echo "[Steam test 3: threads and signals] -L -P4 @tests.ext4 /bin/signals"
	echo "[Steam test 4: an X display (a clock)] $X @steam-root.ext4 /usr/bin/xrun /usr/bin/xclock -update 1"
	echo "[Steam test 5: memory under load] -L -P16 -c3 -eLD_PRELOAD=/lib/libgxfork.so @tests.ext4 /bin/strain"
	# Steam itself with less for the kernel still (it had 300 MB of its 416
	# free when a client that had just logged in ran out of memory).
	X=$(echo "$X" | sed 's/-k416 -m176/-k224 -m176/')
	echo "[Steam] -v $X -eLD_LIBRARY_PATH=/root/Steam/steamrtarm64:/root/Steam/steamrtarm64/libs @steam-root.ext4 =steam-home.ext4 /usr/bin/xrun /root/Steam/steamrtarm64/steam -no-child-update-ui -no-cef-sandbox -noverifyfiles"
	# For a console: the interface made for a controller and a television.
	echo "[Steam, Big Picture] -v $X -eLD_LIBRARY_PATH=/root/Steam/steamrtarm64:/root/Steam/steamrtarm64/libs @steam-root.ext4 =steam-home.ext4 /usr/bin/xrun /root/Steam/steamrtarm64/steam -no-child-update-ui -no-cef-sandbox -noverifyfiles -gamepadui"
	# The same told it has one processor and kept on one (-c1 -C1): slow, and
	# the one that got furthest while the others lost what they were copying.
	echo "[Steam, one processor] -v $(echo "$X" | sed 's/-c3/-c1 -C1/') -eLD_LIBRARY_PATH=/root/Steam/steamrtarm64:/root/Steam/steamrtarm64/libs @steam-root.ext4 =steam-home.ext4 /usr/bin/xrun /root/Steam/steamrtarm64/steam -no-child-update-ui -no-cef-sandbox -noverifyfiles"
} >$OUT/steam.cmd
cat $OUT/steam.cmd
