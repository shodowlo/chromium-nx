#!/bin/bash
# Makes a libLLVM that has every name Mesa's software renderers are linked
# against and none of the compiler: 120 MB less in every program that draws
# with them, for as long as they are told not to compile anything
# (GALLIUM_DRIVER=softpipe, DRAW_USE_LLVM=0), which is the only way they are
# used under gx.
#
#   llvm-stub.sh <libgallium-*.so> <libLLVM.so.N to make>
set -eu
GALLIUM=$1
OUT=$2
TMP=$(mktemp -d)
CROSS=aarch64-linux-gnu-
VERSION=$(${CROSS}objdump -T "$GALLIUM" | awk '/\*UND\*/ && /LLVM_/ { gsub(/[()]/, "", $(NF-1)); print $(NF-1); exit }')
{
	echo '	.text'
	${CROSS}objdump -T "$GALLIUM" | awk '/\*UND\*/ && /LLVM_/ && $2 == "DF" { print $NF }' | while read -r name; do
		printf '	.globl %s\n	.type %s, %%function\n%s:\n	mov x0, #0\n	ret\n' "$name" "$name" "$name"
	done
	echo '	.bss'
	${CROSS}objdump -T "$GALLIUM" | awk '/\*UND\*/ && /LLVM_/ && $2 == "DO" { print $NF }' | while read -r name; do
		printf '	.globl %s\n	.type %s, %%object\n	.size %s, 4096\n	.balign 16\n%s:\n	.zero 4096\n' "$name" "$name" "$name" "$name"
	done
	echo '	.section .note.GNU-stack,"",%progbits'
} >"$TMP/stub.S"
printf '%s { global: *; };\n' "$VERSION" >"$TMP/stub.map"
${CROSS}gcc -shared -nostdlib -Wl,-soname,"$(basename "$OUT")" -Wl,--version-script,"$TMP/stub.map" \
	"$TMP/stub.S" -o "$OUT"
rm -rf "$TMP"
ls -la "$OUT"
