#!/usr/bin/env bash
# Prints the firmware boards (every boards/<name> whose HAL isn't the Linux
# host build) as a JSON list for a GitHub Actions matrix:
#   [{"board":"a13-mini-bk7252","hal":"A13","experimental":false}, ...]
# A board with set(LC_BOARD_EXPERIMENTAL 1) in its board.cmake hasn't run on
# hardware yet: CI builds it without letting a failure block the rest.
set -euo pipefail
cd "$(dirname "$0")/../firmware/boards"
sep=""
printf '['
for f in */board.cmake; do
    board=${f%/board.cmake}
    hal=$(sed -n 's/^[[:space:]]*set(LC_HAL[[:space:]]*\([A-Za-z0-9_]*\)).*/\1/p' "$f" | head -n1)
    [ -n "$hal" ] || { echo "$f: no set(LC_HAL ...)" >&2; exit 1; }
    [ "$hal" = host ] && continue
    exp=false
    grep -Eq '^[[:space:]]*set\(LC_BOARD_EXPERIMENTAL[[:space:]]+(1|ON|TRUE)\)' "$f" && exp=true
    printf '%s{"board":"%s","hal":"%s","experimental":%s}' "$sep" "$board" "$hal" "$exp"
    sep=","
done
printf ']\n'
