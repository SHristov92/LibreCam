#!/usr/bin/env bash
# Builds the firmware image for one board, fetching that HAL's pinned SDK
# and toolchain into a cache directory first. Used by CI; works locally too.
#
#   ci/build-board.sh a13-mini-bk7252     -> dist/librecam-a13-mini-bk7252-<version>.bin
#
# LC_CACHE (default ~/.cache/librecam) holds the SDKs and toolchains.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BOARD=${1:?usage: ci/build-board.sh <board>}
CACHE=${LC_CACHE:-$HOME/.cache/librecam}
DIST=$ROOT/dist
CFG=$ROOT/firmware/boards/$BOARD/board.cmake

[ -f "$CFG" ] || { echo "unknown board '$BOARD'" >&2; exit 1; }
HAL=$(sed -n 's/^[[:space:]]*set(LC_HAL[[:space:]]*\([A-Za-z0-9_]*\)).*/\1/p' "$CFG" | head -n1)
VERSION=$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo unknown)
mkdir -p "$CACHE" "$DIST"

# Fetches one commit of a git repo into $2 (shallow), once.
fetch_rev() {
    local url=$1 dir=$2 rev=$3
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$rev" ]; then
        rm -rf "$dir"
        git init -q "$dir"
        git -C "$dir" fetch -q --depth 1 "$url" "$rev"
        git -C "$dir" checkout -q FETCH_HEAD
    fi
}

# Downloads and unpacks a tarball into $2 once, checking its SHA-256.
fetch_tar() {
    local url=$1 dir=$2 sha=$3 tmp
    [ -d "$dir" ] && return
    tmp=$(mktemp)
    curl -fsSL -o "$tmp" "$url"
    echo "$sha  $tmp" | sha256sum -c --quiet -
    mkdir -p "$dir.part"
    tar -xf "$tmp" -C "$dir.part" --strip-components=1
    mv "$dir.part" "$dir"
    rm -f "$tmp"
}

case "$HAL" in
bk7252)
    # Beken FreeRTOS SDK 3.0.76 (the only public SDK with BK7252N support).
    SDK_URL=https://github.com/YanMinge/beken_freertos_sdk_release.git
    SDK_REV=5139270bd0b27e258cfb46dee6e496f789a656e9
    TC_URL=https://developer.arm.com/-/media/Files/downloads/gnu-rm/10.3-2021.10/gcc-arm-none-eabi-10.3-2021.10-x86_64-linux.tar.bz2
    TC_SHA=97dbb4f019ad1650b732faffcc881689cedc14e2b7ee863d390e0a41ef16c9a3
    fetch_rev "$SDK_URL" "$CACHE/beken_freertos_sdk-$SDK_REV" "$SDK_REV"
    fetch_tar "$TC_URL" "$CACHE/gcc-arm-none-eabi-10.3-2021.10" "$TC_SHA"
    BK_SDK="$CACHE/beken_freertos_sdk-$SDK_REV" \
    FREERTOS_EXEC_PATH="$CACHE/gcc-arm-none-eabi-10.3-2021.10/bin/" \
    LC_VERSION="$VERSION" LC_BOARD="$BOARD" LC_BUILD_DIR="$ROOT/firmware/build/$BOARD" \
        "$ROOT/firmware/hal/bk7252/build.sh"
    OUT="$DIST/librecam-$BOARD-$VERSION.bin"
    cp "$ROOT/firmware/build/$BOARD/lc_app.bin" "$OUT"
    # OTA package for the web UI's firmware update.
    cp "$ROOT/firmware/build/$BOARD/lc_app.rbl" "${OUT%.bin}.rbl"
    (cd "$DIST" && sha256sum "$(basename "${OUT%.bin}.rbl")" > "$(basename "${OUT%.bin}.rbl").sha256")
    ;;
*)
    echo "no CI build for HAL '$HAL' (board $BOARD)" >&2
    exit 1
    ;;
esac

(cd "$DIST" && sha256sum "$(basename "$OUT")" > "$(basename "$OUT").sha256")
echo "built $OUT"
