#!/bin/bash
# Makes the root filesystem Chromium runs from (an ext4 image, for gx to mount)
# out of Debian's arm64 chromium-headless-shell, and runs it under qemu-user.
#
#   docker exec lkl-gx bash /project/gx/build-chromium.sh [chromium arguments]
#
# Without arguments, Chromium prints the DOM of a test page once its script
# has run. With "shot [url]", it renders the page (the test page by default)
# in a 1280x720 window and the picture ends up in chromium.png. With
# "browse <script> [url]", it shows the page continuously, as on the Switch:
# the pictures go to chromium-frames/ and the script plays the user (see
# host_input() in host_posix.c). GX_TRACE=1 traces the guest's syscalls. Run
# build-pc.sh first: it builds gx.
set -u

SRC=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/work/gx-out}
ROOT=$OUT/chromium-root
IMAGE=$OUT/chromium.ext4
QEMU=qemu-aarch64-static
ARM64_LIB=/usr/lib/aarch64-linux-gnu
ARM64_LD=$ARM64_LIB/ld-linux-aarch64.so.1
CHROMIUM=/usr/lib/chromium/chromium-headless-shell

[ -x $CHROMIUM ] || { echo "no arm64 chromium-headless-shell in this container"; exit 1; }

# What the whole browser is started with, here and on the console: one process,
# no helper of its own to report crashes (it would be a process), its profile
# in /root, and a cache that stays small since the files are in memory.
FULL_FLAGS="--no-sandbox --single-process --no-zygote --ozone-platform=wayland --user-data-dir=/root/profile \
--no-first-run --no-default-browser-check --disable-breakpad --disable-crashpad-for-testing --test-type \
--password-store=basic --disable-background-networking --disable-component-update \
--disk-cache-size=33554432 --media-cache-size=33554432 --disable-quic --hide-crash-restore-bubble --enable-spatial-navigation --remote-debugging-pipe"
# Where its address bar is, for gx (-A).
FULL_ADDRESS=225,46,1150,80

if [ ! -e "$IMAGE" ] || [ "$0" -nt "$IMAGE" ] || [ $CHROMIUM -nt "$IMAGE" ]; then
	echo "== root filesystem"
	rm -rf "$ROOT"
	# The image is mounted read-only: gx mounts memory filesystems on
	# these, and they have to be there.
	mkdir -p "$ROOT"/{proc,sys,dev,tmp,run,root,lib,etc/ssl/certs,usr/lib,usr/share/gx,usr/share/fonts}
	cp -r /usr/lib/chromium "$ROOT/usr/lib/" || exit 1

	# Every library the binary is linked against, and the ones NSS loads
	# by name at run time.
	# chromium-shell, if it is installed, is the same engine with a window
	# (the "wayland" mode below wants it).
	SHELL_BIN=/usr/lib/chromium/chromium-shell
	[ -x $SHELL_BIN ] || SHELL_BIN=
	# And chromium itself, the whole browser (the "full" mode below).
	FULL_BIN=/usr/lib/chromium/chromium
	[ -x $FULL_BIN ] || FULL_BIN=
	for lib in $($QEMU $ARM64_LD --list $CHROMIUM | awk '$2 == "=>" { print $3 }') \
		$([ -z "$SHELL_BIN" ] || $QEMU $ARM64_LD --list $SHELL_BIN | awk '$2 == "=>" { print $3 }') \
		$([ -z "$FULL_BIN" ] || $QEMU $ARM64_LD --list $FULL_BIN | awk '$2 == "=>" { print $3 }') \
		$ARM64_LIB/libsoftokn3.so $ARM64_LIB/libfreebl3.so $ARM64_LIB/libfreeblpriv3.so \
		$ARM64_LIB/libnssckbi.so $ARM64_LIB/libnssdbm3.so $ARM64_LIB/libsqlite3.so.0 \
		$ARM64_LIB/libnss_*.so.2 $ARM64_LIB/libgcc_s.so.1; do
		cp -L "$lib" "$ROOT/lib/" || exit 1
	done
	cp -L $ARM64_LD "$ROOT/lib/ld-linux-aarch64.so.1" || exit 1
	# For rendering with the GPU (GX_GPU=1 below): gx's EGL and GLES, which
	# are the host's (build-pc.sh makes them), and the small library a
	# Wayland client makes its GL window with.
	for lib in libEGL.so.1 libGLESv2.so.2; do
		[ ! -e "$OUT/sysroot/lib/$lib" ] || cp "$OUT/sysroot/lib/$lib" "$ROOT/lib/" || exit 1
	done
	[ ! -e $ARM64_LIB/libwayland-egl.so.1 ] || cp -L $ARM64_LIB/libwayland-egl.so.1 "$ROOT/lib/" || exit 1
	# For video decoded by the host: what takes over the browser's H.264
	# decoder (LD_PRELOAD), and a page with a video to try it on.
	[ ! -e "$OUT/sysroot/lib/libgxvideo.so" ] || cp "$OUT/sysroot/lib/libgxvideo.so" "$ROOT/lib/" || exit 1
	# For sound: gx's libasound (guest/asound.c) in place of ALSA's, with
	# every function the programs import. The ones it does not have say
	# that there is nothing: a size for what asks one, "no such thing" for
	# what opens something, "not there" for the rest.
	CC=aarch64-linux-gnu-gcc
	A=$OUT/asound
	mkdir -p "$A"
	for bin in $CHROMIUM $SHELL_BIN $FULL_BIN; do
		aarch64-linux-gnu-readelf --dyn-syms -W "$bin"
	done | awk '$7 == "UND" && $8 ~ /^snd_/ { sub(/ \([0-9]+\)$/, ""); print $8 }' | sort -u >"$A/imports"
	echo 'GX_THUNK(gx_audio_open) GX_THUNK(gx_audio_write) GX_THUNK(gx_audio_queued)
GX_THUNK(gx_audio_drop) GX_THUNK(gx_audio_close)' | tr ' ' '\n' >"$A/host.list"
	$CC -O2 -g -Wall -fPIC -c "$SRC/guest/asound.c" -o "$A/asound.o" || exit 1
	$CC -O2 -g -Wall -fPIC -DGX_LIST="\"$A/host.list\"" -c "$SRC/guest/hostlib.c" -o "$A/host.o" || exit 1
	aarch64-linux-gnu-nm --defined-only "$A/asound.o" | awk '{ print $3 }' >"$A/have"
	sed 's/@.*//' "$A/imports" | grep -vxFf "$A/have" | awk '
		/_sizeof$/ { print "unsigned long " $0 "(void) { return 64; }"; next }
		/_open$/   { print "long " $0 "(void) { return -2; }"; next }
		           { print "long " $0 "(void) { return -38; }" }' >"$A/rest.c"
	# The programs ask for the functions by version, as ALSA has them; one
	# asked for without a version is given the first.
	awk -F@ '{ v = $2 == "" ? "ALSA_0.9" : $2; names[v] = names[v] " " $1 ";" }
		END { for (v in names) print v " { global:" names[v] (v == "ALSA_0.9" ? " local: *;" : "") " };" }' \
		"$A/imports" >"$A/versions"
	grep -q "^ALSA_0.9 " "$A/versions" || { echo "no ALSA functions found in the programs"; exit 1; }
	$CC -shared -Wl,-soname,libasound.so.2 -Wl,--version-script="$A/versions" \
		"$A/asound.o" "$A/host.o" "$A/rest.c" -o "$ROOT/lib/libasound.so.2" || exit 1

	cp -rL /usr/share/fonts/truetype "$ROOT/usr/share/fonts/" || exit 1
	cp -rL /etc/fonts "$ROOT/etc/" || exit 1
	cp -L /etc/ssl/certs/ca-certificates.crt "$ROOT/etc/ssl/certs/" || exit 1
	echo "root:x:0:0:root:/root:/bin/sh" >"$ROOT/etc/passwd"
	echo "root:x:0:" >"$ROOT/etc/group"
	echo "127.0.0.1 localhost" >"$ROOT/etc/hosts"
	echo "hosts: files dns" >"$ROOT/etc/nsswitch.conf"
	echo "nameserver 10.0.2.3" >"$ROOT/etc/resolv.conf"
	echo "0123456789abcdef0123456789abcdef" >"$ROOT/etc/machine-id"
	# The key map gx's Wayland display hands out (wl.c), a standard one.
	if command -v xkbcli >/dev/null; then
		xkbcli compile-keymap --layout us >"$ROOT/usr/share/gx/keymap.xkb" || exit 1
	fi
	if [ -e "$OUT/media/test.mp4" ]; then
		cp "$OUT/media/test.mp4" "$ROOT/usr/share/gx/test.mp4"
		echo '<body style="margin:0;background:#000"><video src="test.mp4" autoplay muted loop style="width:100%"></video>' \
			>"$ROOT/usr/share/gx/video.html"
	fi
	cat >"$ROOT/usr/share/gx/page.html" <<'EOF'
<!doctype html><html><head><title>gx page</title>
<style>body{background:#204080;color:#fff;font:40px sans-serif}
div{width:300px;height:120px;background:linear-gradient(90deg,red,lime);border-radius:20px}</style></head>
<body><h1 id=t>Bonjour depuis Chromium</h1><div></div><p id=js></p>
<script>document.getElementById("js").textContent = "JS: " + [1, 2, 3].map(x => x * x).join(",")</script>
</body></html>
EOF
	size=$(( $(du -sm "$ROOT" | cut -f1) * 13 / 10 + 32 ))
	rm -f "$IMAGE"
	mke2fs -q -t ext4 -O ^has_journal -d "$ROOT" "$IMAGE" "${size}M" || exit 1
	echo "$IMAGE: ${size} MB"

	# The image for the console: the whole browser, without what only the
	# tests here use (the shells, and the graphics drivers that draw with the
	# processor: the console has its own). chromium-shell no longer has an
	# image of its own there.
	SLIM=$OUT/chromium-switch-root
	rm -rf "$SLIM" "$OUT/chromium-switch.ext4"
	if [ -n "$FULL_BIN" ]; then
		cp -al "$ROOT" "$SLIM" || exit 1
		rm -f "$SLIM"/usr/lib/chromium/{chromium-headless-shell,chromium-shell,chrome_crashpad_handler} \
			"$SLIM"/usr/lib/chromium/{libvk_swiftshader.so,vk_swiftshader_icd.json,libvulkan.so.1} \
			"$SLIM"/usr/lib/chromium/{libVkLayer_khronos_validation.so,libtest_trace_processor.so} \
			"$SLIM"/usr/lib/chromium/*.log
		size=$(( $(du -sm "$SLIM" | cut -f1) * 21 / 20 + 16 ))
		rm -f "$OUT/chromium-full-switch.ext4"
		mke2fs -q -t ext4 -O ^has_journal -d "$SLIM" "$OUT/chromium-full-switch.ext4" "${size}M" || exit 1
		rm -rf "$SLIM"
		echo "$OUT/chromium-full-switch.ext4: ${size} MB"
		# Where it keeps what is to last from one run to the next, its
		# profile: an empty filesystem that gx mounts on /root, read and
		# written. With a journal, since the console is switched off
		# without a word. Empty, it packs to nothing: the console unpacks
		# it the first time.
		rm -f "$OUT/profile.ext4" "$OUT/profile.ext4.gz"
		mke2fs -q -t ext4 -L profile "$OUT/profile.ext4" 256M || exit 1
		gzip -9 -k "$OUT/profile.ext4" || exit 1
		# How to start it, for build-nx.sh to put in the console's menu.
		echo "-A$FULL_ADDRESS /usr/lib/chromium/chromium $FULL_FLAGS" >"$OUT/chromium-full.cmd"
	fi
fi

# NO_TESTS=1: the images only, nothing run here.
[ -z "${NO_TESTS:-}" ] || exit 0

echo "== chromium"
export_arg=
if [ "${1:-}" = full ]; then
	shift
	# The whole browser, as on the console (the image made for it); the
	# pictures go to chromium-frames/ and GX_INPUT is the user's script.
	rm -rf "$OUT/chromium-frames"
	mkdir -p "$OUT/chromium-frames"
	export GX_FRAMES=$OUT/chromium-frames GX_INPUT=${GX_INPUT:-}
	# Its profile is kept from one run here to the next; GX_FRESH=1 starts
	# from an empty one.
	# GX_GPU=1: drawn with GLES, through ANGLE and gx's libEGL and libGLESv2.
	gpu="--disable-gpu"
	[ -z "${GX_GPU:-}" ] || gpu="--use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist --disable-gpu-sandbox --in-process-gpu"
	[ -z "${GX_FRESH:-}" ] || rm -f "$OUT/profile-pc.ext4"
	[ -e "$OUT/profile-pc.ext4" ] || cp "$OUT/profile.ext4" "$OUT/profile-pc.ext4" || exit 1
	exec timeout ${GX_TIMEOUT:-300} $QEMU "$OUT/gx" ${GX_TRACE:+-t} -n -w -b -A $FULL_ADDRESS \
		-p "$OUT/profile-pc.ext4" -e LD_PRELOAD=/lib/libgxvideo.so \
		-e XDG_RUNTIME_DIR=/run -e WAYLAND_DISPLAY=wayland-0 "$OUT/chromium-full-switch.ext4" \
		/usr/lib/chromium/chromium $FULL_FLAGS $gpu ${GX_JS---js-flags=--jitless} \
		"${@:-file:///usr/share/gx/page.html}"
fi
if [ "${1:-}" = wayland ]; then
	# The Chromium that has a window, on gx's Wayland display (wl.c). The
	# pictures go to chromium-frames/ and GX_INPUT is the user's script.
	shift
	rm -rf "$OUT/chromium-frames"
	mkdir -p "$OUT/chromium-frames"
	export GX_FRAMES=$OUT/chromium-frames GX_INPUT=${GX_INPUT:-}
	# GX_GPU=1: Chromium draws with GLES (through ANGLE's GLES backend and
	# gx's thunk libraries) instead of with the processor. An experiment.
	gpu="--disable-gpu"
	[ -z "${GX_GPU:-}" ] || gpu="--use-gl=angle --use-angle=gles-egl --ignore-gpu-blocklist --disable-gpu-sandbox --in-process-gpu ${GX_GPU_FLAGS:-}"
	exec timeout ${GX_TIMEOUT:-300} $QEMU "$OUT/gx" ${GX_TRACE:+-t} -n -w -b \
		-e XDG_RUNTIME_DIR=/run -e WAYLAND_DISPLAY=wayland-0 "$IMAGE" \
		/usr/lib/chromium/chromium-shell --no-sandbox --single-process --no-zygote \
		$gpu ${GX_JS---js-flags=--jitless} --ozone-platform=wayland --enable-spatial-navigation \
		--autoplay-policy=no-user-gesture-required --remote-debugging-pipe \
		"${@:-file:///usr/share/gx/page.html}"
fi
if [ "${1:-}" = browse ]; then
	rm -rf "$OUT/chromium-frames"
	mkdir -p "$OUT/chromium-frames"
	export GX_FRAMES=$OUT/chromium-frames GX_INPUT=$2
	export_arg=-b
	set -- --remote-debugging-pipe --window-size=1280,720 --hide-scrollbars \
		"${3:-file:///usr/share/gx/page.html}"
elif [ $# -eq 0 ]; then
	set -- --dump-dom file:///usr/share/gx/page.html
elif [ "$1" = shot ]; then
	rm -f "$OUT/chromium.png"
	export_arg="-o /tmp/shot.png=$OUT/chromium.png"
	set -- --screenshot=/tmp/shot.png --window-size=1280,720 --hide-scrollbars \
		"${2:-file:///usr/share/gx/page.html}"
fi
# One process: gx has no fork. No GPU process either, for now. V8 without its
# compilers (--jitless): they need memory that is writable and executable at
# once, which Horizon does not have and gx refuses everywhere.
exec timeout ${GX_TIMEOUT:-600} $QEMU "$OUT/gx" ${GX_TRACE:+-t} -n $export_arg "$IMAGE" $CHROMIUM \
	--no-sandbox --single-process --no-zygote --disable-gpu ${GX_JS---js-flags=--jitless} "$@"
