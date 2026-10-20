#!/usr/bin/env bash
# Builds the firmware for the BK7252N with Beken's FreeRTOS SDK.
#
#   BK_SDK=/path/to/beken_freertos_sdk_release \
#   FREERTOS_EXEC_PATH=/path/to/gcc-arm-none-eabi-10.3-2021.10/bin/ \
#   firmware/hal/bk7252/build.sh
#
# The SDK is copied to build/bk7252/sdk (it builds in place), this tree is
# linked into it as ./lc, and the result is build/bk7252/lc_app.bin: the
# CRC-wrapped app to write at flash 0x11000.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
FW=$(cd "$HERE/../.." && pwd)
SDK=${BK_SDK:-$HOME/workspace/refs/beken_freertos_sdk_release}
TC=${FREERTOS_EXEC_PATH:-$HOME/tools/gcc-arm-none-eabi-10.3-2021.10/bin/}
BOARD=${LC_BOARD:-a13-mini-bk7252}
WORK=${LC_BUILD_DIR:-$FW/build/bk7252}

[ -f "$SDK/application.mk" ] || { echo "BK_SDK=$SDK is not the Beken FreeRTOS SDK" >&2; exit 1; }
[ -x "${TC}arm-none-eabi-gcc" ] || { echo "no arm-none-eabi-gcc in FREERTOS_EXEC_PATH=$TC" >&2; exit 1; }

mkdir -p "$WORK/sdk"
rsync -a --delete --exclude /.git --exclude /out --exclude /lc --exclude /lc_gen "$SDK"/ "$WORK/sdk"/
ln -sfn "$FW" "$WORK/sdk/lc"
mkdir -p "$WORK/sdk/lc_gen"
python3 "$FW/lib/tools/embed_web.py" "$FW/lib/web/index.html" "$WORK/sdk/lc_gen/web_ui.c"

cd "$WORK/sdk"
# Hook our sources in after LFLAGS is set and before OBJ_LIST is expanded.
sed -i '/^$(MISC_LIB): $(OBJ_LIST)/i -include ./lc/hal/bk7252/sdk.mk' application.mk
# The SDK's stock bk7252n config is used as is: its CFG_SUPPORT_BLE=0 path
# doesn't compile (low_voltage_ps.c), so BLE stays in even though we don't use it.

make bk7252n -j"$(nproc)" FREERTOS_EXEC_PATH="$TC" LC_BOARD="$BOARD"
python3 "$HERE/mkimage.py" out/bk7231_bsp.bin "$WORK/lc_app.bin"
