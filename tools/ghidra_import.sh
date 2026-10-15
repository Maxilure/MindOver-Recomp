#!/bin/bash
# =============================================================================
# tools/ghidra_import.sh -- make the Ghidra project of the game, once
# =============================================================================
#
# Imports game/default.xex into out/ghidra/CrashMoM through the XEXLoaderWV
# extension (it decrypts / decompresses the XEX and names the kernel imports)
# and runs Ghidra's auto-analysis: functions, cross references, data types.
# Takes a long time on the game's 6 MB of code; afterwards
# tools/ghidra_decompile.sh answers in seconds.
#
# out/ is gitignored: the project holds the game's code (rule 1, CLAUDE.md).
# Log: logs/ghidra-import.log (Ghidra's) + logs/ghidra-import.out (console).
# =============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GHIDRA="${GHIDRA_INSTALL_DIR:-/opt/ghidra}"
mkdir -p "$ROOT/out/ghidra" "$ROOT/logs"

if [ -d "$ROOT/out/ghidra/CrashMoM.rep" ]; then
  echo "out/ghidra/CrashMoM already exists (delete out/ghidra/CrashMoM.* to import again)" >&2
  exit 1
fi

GHIDRA_HEADLESS_MAXMEM="${GHIDRA_HEADLESS_MAXMEM:-4G}" \
  "$GHIDRA/support/analyzeHeadless" "$ROOT/out/ghidra" CrashMoM \
  -import "$ROOT/game/default.xex" -log "$ROOT/logs/ghidra-import.log" \
  > "$ROOT/logs/ghidra-import.out" 2>&1
echo "imported: $(grep -c . "$ROOT/logs/ghidra-import.out") log lines, see logs/ghidra-import.out"
