#!/usr/bin/env bash
# =============================================================================
# play.sh: play the game normally, with a fresh log for every session
# =============================================================================
#
# For playtesting. Each run writes its own pair of files, named by the start
# time, so a bug report can say "session 2026-09-25_1130, about 20 minutes
# in" and we find the matching lines:
#
#   logs/play-<date>_<time>.log         the game's log (--log_file). Every
#                                       line has a wall-clock timestamp.
#   logs/play-<date>_<time>-stdout.log  whatever went to the terminal
#                                       (crash messages can land only here)
#
# At the end it prints how the game exited: a normal quit, or the signal
# that killed it (a crash), so a silent crash doesn't go unnoticed.
#
# Usage:
#   tools/play.sh                        normal play
#   tools/play.sh --mnk_mode=true        extra game flags are passed through
#
# Notes:
# * logs/ is gitignored: logs mention file names from the game disc, which
#   is fine locally but they never go into the repo.
# * Save games live in ~/.local/share/crash_mom/B13EBABEBABEBABE/565507FA/
#   (the default profile's folder for title 565507FA; 00000001 = saves).
# * The "Resolve region is empty" spam is gone since SDK patch 0003, so an
#   [error] or [warning] line in these logs is worth a look.
# =============================================================================
set -uo pipefail  # no -e: we want to report the game's exit status ourselves

cd "$(dirname "$0")/.."
stamp=$(date +%Y-%m-%d_%H%M)
log="logs/play-${stamp}.log"
out="logs/play-${stamp}-stdout.log"
mkdir -p logs

echo "Session ${stamp}"
echo "  game log:   ${log}"
echo "  terminal:   ${out}"

out/build/linux-amd64-relwithdebinfo/crash_mom \
  --game_data_root="$PWD/game" --log_file="$log" "$@" 2>&1 | tee "$out"
status=${PIPESTATUS[0]}

echo
if [ "$status" -eq 0 ]; then
  echo "Game exited normally."
elif [ "$status" -gt 128 ]; then
  # Shell convention: 128 + N means "killed by signal N" (11 = SIGSEGV...).
  sig=$((status - 128))
  echo "Game was killed by signal ${sig} ($(kill -l "$sig" 2>/dev/null || echo '?')): probably a crash."
else
  echo "Game exited with status ${status}."
fi
errors=$(grep -c '\[error\]' "$log" 2>/dev/null || true)
warnings=$(grep -c '\[warning\]' "$log" 2>/dev/null || true)
echo "Log has ${errors:-0} [error] and ${warnings:-0} [warning] lines: ${log}"
