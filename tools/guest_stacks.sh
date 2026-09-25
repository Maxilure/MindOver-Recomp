#!/usr/bin/env bash
# =============================================================================
# guest_stacks.sh: "what is every game thread doing right now?"
# =============================================================================
#
# Runs the game under gdb, lets it play for N seconds, pauses it, and prints
# a condensed call stack for every thread that is running game code.
#
# Why this works so well for a static recompilation: every PowerPC function
# of the game became a real C++ function named sub_XXXXXXXX (its original
# address). So a plain host backtrace *is* the game's call stack, and each
# address can be fed straight to xexdis. Kernel calls show up as
# xboxkrnl::<Name>_entry frames, so you can see what a thread waits on.
#
# How we found the loading-screen hang with it (docs/findings/04): one thread
# was not waiting at all but running inside sub_8234C580 -> a busy-wait loop.
#
# Usage:
#   tools/guest_stacks.sh [seconds] [extra game args...]
#   tools/guest_stacks.sh 30
#   tools/guest_stacks.sh 20 --log_level=debug
#
# Output: condensed stacks on stdout; the full gdb output (complete
# backtraces with source lines) is kept in logs/guest_stacks_gdb.txt.
#
# Notes:
# * gdb must *launch* the game: this machine has ptrace_scope=1, which
#   forbids attaching to an already-running process that isn't our child.
# * The runtime uses SIGSEGV internally (guest memory watches), so the
#   script tells gdb to pass those signals silently.
# * That also hides a *fault loop*: a thread whose "crash" the SDK's handler
#   swallows, so the same instruction faults forever (100% CPU, identical
#   frame #0 on every run). If you see that, re-run under gdb and switch to
#   `handle SIGSEGV stop print` once the game is stuck: the stop shows the
#   faulting instruction and $_siginfo the address. docs/findings/05.
# * A game window opens for the duration, like a normal run.
# =============================================================================
set -euo pipefail

SECONDS_TO_RUN=${1:-25}
shift || true

ROOT=$(cd "$(dirname "$0")/.." && pwd)
EXE="$ROOT/out/build/linux-amd64-relwithdebinfo/crash_mom"
FULL_OUT="$ROOT/logs/guest_stacks_gdb.txt"
GDB_SCRIPT=$(mktemp --suffix=.gdb)
trap 'rm -f "$GDB_SCRIPT"' EXIT
mkdir -p "$ROOT/logs"

cat > "$GDB_SCRIPT" <<'EOF'
# Signals the runtime uses on purpose: let them through, don't stop.
handle SIGSEGV nostop noprint pass
handle SIGBUS  nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGPIPE nostop noprint pass
# Real-time signals (SIG32+) are used internally too (seen: SIG35 on a game
# thread); without this gdb halts on them and the script's commands misfire.
handle SIG32 SIG33 SIG34 SIG35 SIG36 SIG37 SIG38 SIG39 SIG40 nostop noprint pass
set pagination off
set print thread-events off
set confirm off
set debuginfod enabled off
run
# We land here when this script sends SIGINT below.
thread apply all bt 40
kill
quit
EOF

echo "Running the game for ${SECONDS_TO_RUN}s under gdb..." >&2
gdb -batch -x "$GDB_SCRIPT" --args "$EXE" --game_data_root="$ROOT/game" \
    --log_file="$ROOT/logs/guest_stacks_run.log" "$@" > "$FULL_OUT" 2>&1 &
GDB_PID=$!

sleep "$SECONDS_TO_RUN"
# -x: match the process *name* exactly, so we hit the game and not gdb
# (gdb's own command line also contains "crash_mom").
pkill -INT -x crash_mom || { echo "game is not running (crashed?), see $FULL_OUT" >&2; }
wait "$GDB_PID" || true

# Condense: for each thread keep only game functions (sub_XXXXXXXX),
# kernel exports (*_entry) and fiber switches; drop threads with none.
awk '
  /^Thread [0-9]+ /  { if (keep) printf "%s\n", block; block = $0 "\n"; keep = 0; last = ""; next }
  /^#/ {
    if (match($0, /(sub_[0-9A-F]{8}|[A-Za-z]+::[A-Za-z_]+_entry|swapcontext)/)) {
      name = substr($0, RSTART, RLENGTH)
      loc = ""
      if (match($0, /recomp\.[0-9]+\.cpp:[0-9]+/)) loc = "  (" substr($0, RSTART, RLENGTH) ")"
      if (name != last) block = block "    " name loc "\n"
      last = name
      if (name ~ /^sub_/) keep = 1
    }
  }
  END { if (keep) printf "%s", block }
' "$FULL_OUT"
