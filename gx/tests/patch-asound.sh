#!/bin/bash
# Puts the libasound of this tree (guest/asound.c) in a root image made by
# build-chromium.sh, without making the image again (that wants the container
# with Debian's Chromium in it): the same recipe as there, on the image's own
# Chromium.
#
#   docker exec cnx-pc2 bash /project/gx/tests/patch-asound.sh IN.ext4 OUT.ext4
set -eu
SRC=$(cd "$(dirname "$0")/.." && pwd)
IN=$1
OUT=$2
A=$(mktemp -d)
CC=aarch64-linux-gnu-gcc

debugfs -R "dump /usr/lib/chromium/chromium $A/chromium" "$IN" 2>/dev/null
[ -s "$A/chromium" ] || { echo "no /usr/lib/chromium/chromium in $IN"; exit 1; }
aarch64-linux-gnu-readelf --dyn-syms -W "$A/chromium" |
	awk '$7 == "UND" && $8 ~ /^snd_/ { sub(/ \([0-9]+\)$/, ""); print $8 }' | sort -u >"$A/imports"
echo 'GX_THUNK(gx_audio_stream_open) GX_THUNK(gx_audio_stream_write) GX_THUNK(gx_audio_stream_queued)
GX_THUNK(gx_audio_stream_drop) GX_THUNK(gx_audio_stream_close)' | tr ' ' '\n' >"$A/host.list"
$CC -O2 -g -Wall -fPIC -c "$SRC/guest/asound.c" -o "$A/asound.o"
$CC -O2 -g -Wall -fPIC -DGX_LIST="\"$A/host.list\"" -c "$SRC/guest/hostlib.c" -o "$A/host.o"
aarch64-linux-gnu-nm --defined-only "$A/asound.o" | awk '{ print $3 }' >"$A/have"
sed 's/@.*//' "$A/imports" | grep -vxFf "$A/have" | awk '
	/_sizeof$/ { print "unsigned long " $0 "(void) { return 64; }"; next }
	/_open$/   { print "long " $0 "(void) { return -2; }"; next }
	           { print "long " $0 "(void) { return -38; }" }' >"$A/rest.c"
awk -F@ '{ v = $2 == "" ? "ALSA_0.9" : $2; names[v] = names[v] " " $1 ";" }
	END { for (v in names) print v " { global:" names[v] (v == "ALSA_0.9" ? " local: *;" : "") " };" }' \
	"$A/imports" >"$A/versions"
$CC -shared -Wl,-soname,libasound.so.2 -Wl,--version-script="$A/versions" \
	"$A/asound.o" "$A/host.o" "$A/rest.c" -o "$A/libasound.so.2"
chmod 755 "$A/libasound.so.2"

cp "$IN" "$OUT.tmp"
debugfs -w -R "rm /lib/libasound.so.2" "$OUT.tmp" >/dev/null 2>&1
debugfs -w -f - "$OUT.tmp" >/dev/null <<EOF
cd /lib
write $A/libasound.so.2 libasound.so.2
EOF
debugfs -R "stat /lib/libasound.so.2" "$OUT.tmp" 2>/dev/null | grep -q "Size: $(stat -c %s "$A/libasound.so.2")" ||
	{ echo "libasound.so.2 did not go in"; exit 1; }
e2fsck -fn "$OUT.tmp" >/dev/null 2>&1 || { echo "the image does not check"; exit 1; }
mv "$OUT.tmp" "$OUT"
echo "$OUT: libasound.so.2 of $(stat -c %s "$A/libasound.so.2") bytes, $(wc -l <"$A/imports") ALSA functions"
rm -rf "$A"
