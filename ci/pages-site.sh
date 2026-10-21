#!/usr/bin/env bash
# Assembles the GitHub Pages site: the web flasher plus the firmware images it
# offers, next to it so the page can fetch them (release assets don't allow
# cross-origin downloads).
#   ci/pages-site.sh <dir with librecam-<board>-<version>.bin> <out dir>
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST=$1
OUT=$2
rm -rf "$OUT"
mkdir -p "$OUT/firmware"
cp "$ROOT/web-flasher/index.html" "$ROOT/web-flasher/bkflash.js" "$OUT/"
touch "$OUT/.nojekyll"

entries=""
for b in $("$ROOT/ci/list-boards.sh" | python3 -c 'import json,sys; print(" ".join(x["board"] for x in json.load(sys.stdin) if x["hal"] == "bk7252"))'); do
    bin=$(ls "$DIST"/librecam-"$b"-*.bin 2>/dev/null | head -n1 || true)
    [ -n "$bin" ] || { echo "no image for $b in $DIST" >&2; exit 1; }
    name=$(basename "$bin")
    version=${name#librecam-$b-}
    version=${version%.bin}
    cp "$bin" "$OUT/firmware/$name"
    title=$(sed -n 's/^[[:space:]]*set(LC_BOARD_TITLE[[:space:]]*"\(.*\)").*/\1/p' "$ROOT/firmware/boards/$b/board.cmake" | head -n1)
    sha=$(sha256sum "$bin" | cut -d' ' -f1)
    entries+="${entries:+,}{\"board\":\"$b\",\"title\":\"${title:-$b}\",\"version\":\"$version\",\"file\":\"firmware/$name\",\"sha256\":\"$sha\"}"
done
commit=$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo "")
printf '{"commit":"%s","date":"%s","boards":[%s]}\n' "$commit" "$(date -u +%Y-%m-%d)" "$entries" > "$OUT/firmware/manifest.json"
echo "site in $OUT:"; ls -R "$OUT"
