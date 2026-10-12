#!/bin/bash
# Builds gx for an aarch64 Linux host, prepares a sysroot and runs the tests
# under qemu-user.
#
#   docker exec lkl-gx bash /project/gx/build-pc.sh [test...]
#
# With test names (hello, threads, dl, timers, gles, net, curl-http, curl-https), only
# those are run. GX_TRACE=1 traces the guest's syscalls. The curl tests need the arm64
# curl of the Docker image and Internet access. The gles test needs the arm64
# Mesa of the Docker image; the last frame it draws ends up in gles.png.
set -u

SRC=$(cd "$(dirname "$0")" && pwd)
LKL=${LKL:-/work/linux/tools/lkl}
OUT=${OUT:-/work/gx-out}
CC=aarch64-linux-gnu-gcc
CROSS_LIB=/usr/aarch64-linux-gnu/lib
QEMU=qemu-aarch64-static
SYSROOT=$OUT/sysroot
TESTS="hello threads dl timers signals spawn atomics lazy strain"
NET_TESTS="net curl-http curl-https"
ARM64_LD=/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1

mkdir -p "$OUT" "$SYSROOT/lib" "$SYSROOT/bin" || exit 1

echo "== gx"
# Dynamically linked: the host's EGL and GLES are loaded with dlopen(). qemu
# finds the loader and the libraries in the arm64 packages of the image.
$CC -O2 -g -Wall -Wextra -Wno-unused-parameter -DGX_ARENA_SIZE='(256UL << 30)' \
	-I"$LKL/include" -I"$SRC" \
	"$SRC"/tramp.S "$SRC"/gmem.c "$SRC"/gelf.c "$SRC"/gsys.c "$SRC"/nat.c "$SRC"/cdp.c "$SRC"/wl.c "$SRC"/glwin.c "$SRC"/video.c "$SRC"/host_posix.c \
	"$LKL/liblkl.a" -lavcodec -lavutil -lpthread -lrt -ldl \
	-o "$OUT/gx" || exit 1

echo "== sysroot"
for lib in ld-linux-aarch64.so.1 libc.so.6 libm.so.6; do
	cp -L "$CROSS_LIB/$lib" "$SYSROOT/lib/$lib" || exit 1
done
for t in $TESTS net; do
	# atomics is built for a processor newer than the one it is run on.
	[ $t = atomics ] && arch="-march=armv8.1-a -mno-outline-atomics" || arch=
	$CC -O2 -g -Wall -pthread $arch "$SRC/tests/$t.c" -ldl -o "$SYSROOT/bin/$t" || exit 1
done

# libgxvideo for the guest, loaded ahead of a browser: its H.264 decoder
# becomes the host's (guest/video.c, video.c).
printf 'GX_THUNK(gx_video_%s)\n' available open send receive copy flush close >"$OUT/video.list"
$CC -O2 -g -Wall -shared -fPIC -Wl,-soname,libgxvideo.so -DGX_LIST="\"$OUT/video.list\"" \
	"$SRC/guest/hostlib.c" "$SRC/guest/video.c" -o "$SYSROOT/lib/libgxvideo.so" || exit 1

# libgxfork for the guest, loaded ahead of programs that run others: a fork()
# that gx can carry out (guest/fork.c).
$CC -O2 -g -Wall -shared -fPIC -Wl,-soname,libgxfork.so "$SRC/guest/fork.c" -ldl \
	-o "$SYSROOT/lib/libgxfork.so" || exit 1

# xrun for the guest: an X display (Xwayland) for a program that wants one.
$CC -O2 -g -Wall "$SRC/guest/xrun.c" -o "$SYSROOT/bin/xrun" || exit 1

# libEGL and libGLESv2 for the guest: thunks to the host's, one per function
# the Khronos headers declare.
if [ -e /usr/include/GLES3/gl32.h ] && [ -e /usr/lib/aarch64-linux-gnu/libEGL.so.1 ]; then
	thunks() { grep -ohE "$1 [a-zA-Z0-9]+" "${@:2}" | awk '{ print $2 }' | sort -u |
		grep -vx eglGetProcAddress | sed 's/.*/GX_THUNK(&)/'; }
	thunks EGLAPIENTRY /usr/include/EGL/egl.h >"$OUT/egl.list"
	thunks GL_APIENTRY /usr/include/GLES2/gl2.h /usr/include/GLES3/gl32.h >"$OUT/gles.list"
	$CC -O2 -g -Wall -shared -fPIC -Wl,-soname,libEGL.so.1 \
		-DGX_LIST="\"$OUT/egl.list\"" -DGX_PROC_ADDRESS=eglGetProcAddress \
		"$SRC/guest/hostlib.c" -o "$SYSROOT/lib/libEGL.so.1" || exit 1
	$CC -O2 -g -Wall -shared -fPIC -Wl,-soname,libGLESv2.so.2 \
		-DGX_LIST="\"$OUT/gles.list\"" \
		"$SRC/guest/hostlib.c" -o "$SYSROOT/lib/libGLESv2.so.2" || exit 1
	# -idirafter: the Khronos headers are with the headers of the build machine.
	$CC -O2 -g -Wall -idirafter /usr/include "$SRC/tests/gles.c" \
		"$SYSROOT/lib/libEGL.so.1" "$SYSROOT/lib/libGLESv2.so.2" -lm \
		-o "$SYSROOT/bin/gles" || exit 1
	TESTS="$TESTS gles"
else
	echo "no arm64 Mesa in this container: skipping gles"
fi

# curl and everything it is linked against, from the arm64 packages.
if [ -x /usr/bin/curl ] && [ -e $ARM64_LD ]; then
	cp -L /usr/bin/curl "$SYSROOT/bin/curl" || exit 1
	for lib in $($QEMU $ARM64_LD --list /usr/bin/curl | awk '$2 == "=>" { print $3 }'); do
		[ -e "$SYSROOT/lib/$(basename "$lib")" ] || cp -L "$lib" "$SYSROOT/lib/" || exit 1
	done
	mkdir -p "$SYSROOT/etc/ssl/certs"
	cp -L /etc/ssl/certs/ca-certificates.crt "$SYSROOT/etc/ssl/certs/" || exit 1
	echo "127.0.0.1 localhost" >"$SYSROOT/etc/hosts"
	echo "hosts: files dns" >"$SYSROOT/etc/nsswitch.conf"
	# The address at which gx's NAT forwards DNS to the host's resolver.
	echo "nameserver 10.0.2.3" >"$SYSROOT/etc/resolv.conf"
	TESTS="$TESTS $NET_TESTS"
else
	echo "no arm64 curl in this container: skipping $NET_TESTS"
fi

# run <name> <pattern the output must contain> <gx arguments...>
run() {
	local name=$1 pattern=$2 log=$OUT/$1.log code
	shift 2
	timeout 120 $QEMU "$OUT/gx" ${GX_TRACE:+-t} "$@" >"$log" 2>&1
	code=$?
	if [ $code -eq 0 ] && grep -q -- "$pattern" "$log" && ! grep -q FAIL "$log"; then
		echo "PASS  $name"
	else
		echo "FAIL  $name (exit $code), see $log:"
		tail -n 40 "$log" | sed 's/^/    /'
		failed=1
	fi
}

echo "== tests"
[ $# -gt 0 ] && TESTS="$*"
failed=0
for t in $TESTS; do
	case $t in
	gles)
		rm -f "$OUT/gles.ppm"
		GX_FRAME=$OUT/gles.ppm run $t "^PASS$" "$SYSROOT" /bin/gles
		[ -e "$OUT/gles.ppm" ] && python3 "$SRC/ppm2png.py" "$OUT/gles.ppm" "$OUT/gles.png" ;;
	net)
		run $t "^PASS$" -n "$SYSROOT" /bin/net ;;
	curl-http)
		run $t "Example Domain" -n "$SYSROOT" /bin/curl -sS -m 60 http://example.com/ ;;
	curl-https)
		run $t "Example Domain" -n "$SYSROOT" /bin/curl -sS -m 60 https://example.com/ ;;
	spawn)
		run $t "^PASS$" -e LD_PRELOAD=/lib/libgxfork.so "$SYSROOT" /bin/spawn ;;
	atomics)
		# On the processor of a Switch, which has none of these instructions.
		QEMU_CPU=cortex-a57 run $t "^PASS$" "$SYSROOT" /bin/atomics ;;
	lazy)
		# With memory that is there when first used, as on the console.
		GX_LAZY=1 run $t "^PASS$" "$SYSROOT" /bin/lazy ;;
	strain)
		GX_LAZY=1 GX_POOL=16 run $t "^PASS$" "$SYSROOT" /bin/strain ;;
	*)
		run $t "^PASS$" "$SYSROOT" "/bin/$t" ;;
	esac
done
exit $failed
