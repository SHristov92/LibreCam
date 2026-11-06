#!/usr/bin/env bash
# Builds the firmware for the TXW81x with Taixin's SDK as published in
# OpenTXW81X, which also carries the C-SKY toolchain (csky-elfabiv2, Linux).
#
#   TXW_SDK=/path/to/OpenTXW81X firmware/hal/txw81x/build.sh
#
# The SDK is copied to build/<board>/sdk (its Makefile builds objects next to
# the sources), this firmware tree is copied into it as ./lc, and the SDK
# Makefile's OpenBeken include is pointed at our sdk.mk. Outputs, in
# LC_BUILD_DIR: lc_app.elf and lc_app.bin (the raw linked image).
#
# lc_app.bin is NOT a flashable image yet: the vendor flow wraps it with the
# loader and the board's parameter block (project/BinScript.exe and
# makecode.exe, Windows only). See README.md.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
FW=$(cd "$HERE/../.." && pwd)
SDK=${TXW_SDK:?set TXW_SDK to an OpenTXW81X checkout}
BOARD=${LC_BOARD:-txw817-cam}
WORK=${LC_BUILD_DIR:-$FW/build/$BOARD}
VERSION=${LC_VERSION:-$(git -C "$FW" describe --tags --always --dirty 2>/dev/null || echo dev)}

[ -f "$SDK/project/Makefile" ] || { echo "TXW_SDK=$SDK is not an OpenTXW81X checkout" >&2; exit 1; }

mkdir -p "$WORK/sdk"
rsync -a --delete --exclude /.git --exclude /lc --exclude /lc_gen "$SDK"/ "$WORK/sdk"/
rsync -a --delete --exclude /build "$FW"/ "$WORK/sdk/lc"/

TC="$WORK/sdk/tools/gcc"
if [ ! -x "$TC/bin/csky-elfabiv2-gcc" ]; then
    tar -xf "$TC"/csky-elfabiv2-tools-*.tar.gz -C "$TC"
fi

mkdir -p "$WORK/sdk/lc_gen"
python3 "$FW/lib/tools/embed_web.py" "$FW/lib/web/index.html" "$WORK/sdk/lc_gen/web_ui.c"
printf '#define LC_FW_VERSION "%s"\n' "$VERSION" > "$WORK/sdk/lc_gen/lc_version.h.new"
cmp -s "$WORK/sdk/lc_gen/lc_version.h.new" "$WORK/sdk/lc_gen/lc_version.h" ||
    mv "$WORK/sdk/lc_gen/lc_version.h.new" "$WORK/sdk/lc_gen/lc_version.h"
rm -f "$WORK/sdk/lc_gen/lc_version.h.new"

cd "$WORK/sdk/project"
grep -q '^include ../../../platforms/TXW81X/OpenBeken.mk' Makefile ||
    { echo "OpenTXW81X Makefile has no OpenBeken include to replace" >&2; exit 1; }
sed -i 's|^include ../../../platforms/TXW81X/OpenBeken.mk|include ../lc/hal/txw81x/sdk.mk|' Makefile

# -k: report every compile error in one run.
make -k -j"$(nproc)" LC_BOARD="$BOARD"
cp project.elf "$WORK/lc_app.elf"
cp project.bin "$WORK/lc_app.bin"
"$TC/bin/csky-elfabiv2-size" project.elf || true
echo "$WORK/lc_app.bin: $(stat -c %s "$WORK/lc_app.bin") bytes, version $VERSION (raw image, not packaged)"
