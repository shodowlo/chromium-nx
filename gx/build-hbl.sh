#!/bin/bash
# Builds a Homebrew Loader whose programs get a 39-bit address space (512 GB)
# instead of the stock 36 bits (64 GB), which Chromium's allocators do not fit
# in, and 64 MB of their own for the kernel's records of their memory (see
# below): it replaces atmosphere/hbl.nsp on the SD card. Keep the original.
#
#   docker exec lkl-gx-nx bash /project/gx/build-hbl.sh
set -eu

export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH

SRC=$(cd "$(dirname "$0")" && pwd)
HBL=${HBL:-/work/nx-hbloader}
VERSION=v2.4.5

[ -d "$HBL" ] || git clone -q --depth 1 --branch $VERSION https://github.com/switchbrew/nx-hbloader.git "$HBL"
cd "$HBL"
git checkout -q hbl.json
# 3 is the 39-bit address space; the handle table is at its largest because
# every shared memory object and every thread of the guest takes a handle.
sed -i 's/"address_space_type": 1,/"address_space_type": 3,/' hbl.json
python3 - <<'PY'
import re
s = open("hbl.json").read()
t = re.sub(r'("type": "handle_table_size",\s*"value": )512', r'\g<1>1023', s)
assert t != s and '"address_space_type": 3,' in t
# Memory of the process's own for the kernel's records of it (its page tables,
# and one record per piece of memory mapped alike): without it they come out
# of a store all programs share, which a large guest empties at some 9000
# pieces, far fewer than a Linux program of a few gigabytes has.
u = t.replace('"address_space_type": 3,', '"address_space_type": 3,\n    "system_resource_size": "0x4000000",', 1)
assert u != t
open("hbl.json", "w").write(u)
PY
make clean >/dev/null
make >/dev/null 2>&1 || { make; exit 1; }
mkdir -p "$SRC/dist-hbl/atmosphere"
cp hbl.nsp "$SRC/dist-hbl/atmosphere/hbl.nsp"
ls -la "$SRC/dist-hbl/atmosphere/hbl.nsp"
