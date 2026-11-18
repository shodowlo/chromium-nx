#!/bin/bash
# Runs the whole browser on the PC under the Switch 2's constraints, to see
# there (where a fault is a signal, with its address) what ends it on the
# console (where a fault ends the process without a word): no shared memory
# (GX_NO_SHARED=1, host_posix.c), V8 without compiled code, no GPU.
#
#   docker exec cnx-pc2 bash /project/gx/tests/run-switch2-like.sh [url]
#
# OUT is a build-pc.sh output with chromium-full-switch.ext4, chromium-full.cmd
# and profile.ext4 (build-chromium.sh) in it. SHARED=1 keeps shared memory,
# to compare.
set -u
OUT=${OUT:-/work/gx-out-nx2}
QEMU=qemu-aarch64-static
read -r address chromium flags <"$OUT/chromium-full.cmd"
cp "$OUT/profile.ext4" "$OUT/profile-s2.ext4" || exit 1
rm -rf "$OUT/s2-frames"
mkdir -p "$OUT/s2-frames"
[ -n "${SHARED:-}" ] || export GX_NO_SHARED=1
export GX_FRAMES=$OUT/s2-frames
exec timeout ${GX_TIMEOUT:-240} $QEMU "$OUT/gx" -n -w -b -A "${address#-A}" \
	-p "$OUT/profile-s2.ext4" -e LD_PRELOAD=/lib/libgxvideo.so \
	-e XDG_RUNTIME_DIR=/run -e WAYLAND_DISPLAY=wayland-0 "$OUT/chromium-full-switch.ext4" \
	"$chromium" $flags --disable-gpu --js-flags=--jitless "${1:-https://www.google.com/}"
