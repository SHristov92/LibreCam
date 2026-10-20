#!/bin/bash
# Usage: decomp_at.sh ADDR... -> prints resolved decompilation of forced Thumb functions.
. ~/tools/rea-env.sh
R=$(cd "$(dirname "$0")/.." && pwd)
"$GHIDRA_INSTALL_DIR/support/analyzeHeadless" "$R/proj" A9 -process app_0x10000.bin -noanalysis \
  -scriptPath "$R/scripts" -postScript DecompAt.java "$R/at.c" "$@" > "$R/at.log" 2>&1
python3 -I "$R/scripts/resolve_dat.py" "$R/app_0x10000.bin" "$R/at.c" "$R/at_r.c" && cat "$R/at_r.c"
