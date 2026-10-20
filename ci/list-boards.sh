#!/usr/bin/env bash
# Prints the firmware boards (every boards/<name> whose HAL isn't the Linux
# host build) as a JSON list for a GitHub Actions matrix:
#   [{"board":"a13-mini-bk7252","hal":"bk7252"}, ...]
set -euo pipefail
cd "$(dirname "$0")/../firmware/boards"
sep=""
printf '['
for f in */board.cmake; do
    board=${f%/board.cmake}
    hal=$(sed -n 's/^[[:space:]]*set(LC_HAL[[:space:]]*\([A-Za-z0-9_]*\)).*/\1/p' "$f" | head -n1)
    [ -n "$hal" ] || { echo "$f: no set(LC_HAL ...)" >&2; exit 1; }
    [ "$hal" = host ] && continue
    printf '%s{"board":"%s","hal":"%s"}' "$sep" "$board" "$hal"
    sep=","
done
printf ']\n'
