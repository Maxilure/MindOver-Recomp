#!/bin/bash
# =============================================================================
# tools/ghidra_decompile.sh -- pseudo-C of game functions, from Ghidra
# =============================================================================
#
# Usage:  tools/ghidra_decompile.sh 0x82433510 [0x8217D0A0 ...]
#
# Needs (docs/01-building.md "Ghidra"): Ghidra 12.1.2 at /opt/ghidra (or
# GHIDRA_INSTALL_DIR), the XEXLoaderWV extension in Ghidra's user Extensions
# folder, and the analysed project out/ghidra/CrashMoM (made once by
# tools/ghidra_import.sh). out/ is gitignored: the project holds the game's
# code, which must never be committed.
#
# Each call starts Ghidra's headless analyzer (~10-20 s), opens the project
# READ-ONLY (nothing is saved) and runs tools/ghidra/DecompileAt.java. Only
# the functions' text is printed; Ghidra's own log goes to
# logs/ghidra-decompile.log. Ghidra names functions FUN_XXXXXXXX and the XEX
# loader Function_XXXXXXXX (from the XEX's function table); both are renamed
# sub_XXXXXXXX here, like our generated code.
#
# Pseudo-C is a READING AID: Ghidra guesses types and may get PowerPC
# details wrong (VMX128 vector instructions, 64-bit vs 32-bit values). The
# recompiled code (generated/default/*.cpp) and xexdis stay the reference.
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GHIDRA="${GHIDRA_INSTALL_DIR:-/opt/ghidra}"
PROJECT_DIR="$ROOT/out/ghidra"

if [ $# -eq 0 ]; then
  echo "usage: $0 <address> [address...]" >&2
  exit 1
fi
if [ ! -d "$PROJECT_DIR/CrashMoM.rep" ]; then
  echo "no Ghidra project yet: run tools/ghidra_import.sh first" >&2
  exit 1
fi
mkdir -p "$ROOT/logs"

GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-4G}" \
  "$GHIDRA/support/analyzeHeadless" "$PROJECT_DIR" CrashMoM \
  -process default.xex -noanalysis -readOnly \
  -scriptPath "$ROOT/tools/ghidra" -postScript DecompileAt.java "$@" \
  > "$ROOT/logs/ghidra-decompile.log" 2>&1 || {
    echo "Ghidra failed: see logs/ghidra-decompile.log" >&2
    exit 1
  }

# Keep only the script's output: from the first ===== marker to the end of
# the last function, minus Ghidra's INFO lines; FUN_ / Function_ -> sub_.
awk '/^===== /{on=1} on && !/^(INFO|WARN|ERROR) /' "$ROOT/logs/ghidra-decompile.log" |
  sed -E 's/\b(FUN|Function)_([0-9a-fA-F]{8})/sub_\U\2/g'
