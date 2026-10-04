#!/usr/bin/env bash
# =============================================================================
# play.sh: play the game normally, with a fresh log for every session
# =============================================================================
#
# For playtesting. Each run writes its own pair of files, named by the start
# time, so a bug report can say "session <date>_<time>, about 20 minutes
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
#   tools/play.sh --dual                 two windows: emulated picture in the
#                                        main one, our native renderer's in a
#                                        second one (= --native_window; F8 in
#                                        the game opens/closes it any time).
#                                        The emulated window then moves at half
#                                        rate (--emulated_draw_every=2, ~40%
#                                        less GPU work; =1 = every frame)
#   tools/play.sh --trace                for hunting a picture bug: each F10
#                                        photo also records every renderer call
#                                        of its frame (2 frames), and the game's
#                                        shaders are saved as the emulated GPU
#                                        meets them, all into logs/trace-<session>/.
#                                        AREA SCAN: the first time something the
#                                        native renderer can't draw shows up, the
#                                        game does the same by itself (a recording
#                                        + photo; src/native/scan.h)
#   tools/play.sh --native-only          EXPERIMENT: only our renderer draws.
#                                        The emulated GPU skips all its drawing
#                                        while our picture is shown (F9 back to
#                                        emulated turns it on again); shows what
#                                        fps the finished port could reach. F10
#                                        photos' emulated half is stale there.
#   tools/play.sh --emulated-only        the mirror image: only the emulated
#                                        GPU's picture, F9/F8 locked, our renderer
#                                        never draws (the baseline to compare with)
#   tools/play.sh --mangohud             the MangoHud overlay, with OUR layouts
#                                        (tools/mangohud/: Right Shift + F1 hide,
#                                        + F2 next layout, + F5 record a log)
#   tools/play.sh --fps_cap=144          above 60 fps (EXPERIMENTAL; 0 = no cap)
#   tools/play.sh --load_save=prison     start straight in a save: no movies,
#                                        title or menus (~20 s to gameplay). By
#                                        number (7), name or a part of it only
#                                        one save has, or "last" (src/saves/quick_load.h)
#   tools/play.sh --ground-trace         record when the physics thinks each
#                                        character is on the ground, and when
#                                        the game logic hears "not on ground",
#                                        into logs/ground-<date_time>.csv
#                                        (src/ground_physics.cpp; ~3 MB/min)
#   tools/play.sh --quiet                don't show the game's log live in
#                                        this terminal (it's still written)
#   tools/play.sh --mnk_mode=true        extra game flags are passed through
#
# Live console: while the game runs, this terminal shows the game's log as it
# is written, shortened and coloured by tools/play_console.awk (errors red,
# warnings yellow, repeating statistics dim, and a bold magenta
# "*** SPOTTED ***" banner when a material we're hunting shows up, e.g. the
# bump material: src/native/spotter.h). The first time in a session such a
# material shows up, an F10 photo is taken for you.
#
# Frame rate: --debug_log_fps is on, so the log gets the average fps, 1% and
# 0.1% lows every 5 s (docs/findings/07), and the whole session's numbers are
# printed when the game closes. --debug_log_fps=false turns it off. Next to
# it: how the main thread spent each frame (game / our renderer / waiting),
# docs/findings/19 section 6.
#
# Sound: --debug_audio_trace is on, so the log names every sound the game
# asks for ("Audio: load character\english\coco\...wav"), the moment it
# starts: a missing voice line can be looked up (docs/findings/19).
# --debug_audio_trace=false turns it off.
#
# Photos: press F10 in the game. They go into photos/ (gitignored), as PNG:
# with the native picture on screen, the same frame from both renderers
# (photo_<date>_<time>_native.png + _emulated.png), else just the screen.
# The number taken this session is printed when the game closes.
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

# Our own shorthands, turned into game flags; everything else passes through.
trace=""
ground=""
live=1
args=()
for arg in "$@"; do
  case "$arg" in
    --dual) args+=(--native_window=true) ;;  # src/native/native_window.h
    --trace)                                 # src/pddi/trace.cpp; --dump_shaders is the SDK's
      trace="logs/trace-${stamp}"
      args+=(--debug_pddi_trace_dir="$PWD/$trace" --debug_pddi_trace_frames=2
             --dump_shaders="$PWD/$trace/shaders") ;;
    --native-only) args+=(--native_only=true) ;;  # src/native/native_renderer.cpp, SDK 0009
    --ground-trace)                               # src/ground_physics.cpp
      ground="logs/ground-${stamp}.csv"
      args+=(--debug_ground_trace="$PWD/$ground") ;;
    --emulated-only) args+=(--emulated_only=true) ;;  # src/native/native_renderer.cpp
    --mangohud) export MANGOHUD=1 ;;  # MangoHud's Vulkan layer switches on by this
    --quiet) live=0 ;;
    *) args+=("$arg") ;;
  esac
done

# MangoHud (whether --mangohud or `mangohud tools/play.sh`): our layouts
# (tools/mangohud/). Its config wants an absolute log folder, so a copy with
# the real path is made in logs/mangohud/. Harmless without MangoHud.
mkdir -p logs/mangohud
sed "s|^output_folder=.*|output_folder=$PWD/logs/mangohud|" tools/mangohud/MangoHud.conf \
  > logs/mangohud/MangoHud.conf
export MANGOHUD_CONFIGFILE="$PWD/logs/mangohud/MangoHud.conf"
export MANGOHUD_PRESETSFILE="$PWD/tools/mangohud/presets.conf"

log="logs/play-${stamp}.log"
out="logs/play-${stamp}-stdout.log"
mkdir -p logs

echo "Session ${stamp}"
echo "  game log:   ${log}"
echo "  terminal:   ${out}"
echo "  photos:     photos/ (press F10 in the game)"
[ -n "$ground" ] && echo "  ground:     ${ground}"
[ -n "$trace" ] && mkdir -p "$trace" && echo "  traces:     ${trace}/ (F10, and automatically where something isn't drawn natively yet)"

# The live console (see the header): follow the log file from its first line
# while the game writes it. The log flushes every info line, so lines show
# up as they happen. Stopped after the game exits.
tail_pid=""
if [ "$live" -eq 1 ]; then
  : > "$log"  # the game appends (--log_file): start from an empty file
  color=0
  [ -t 1 ] && color=1  # colours only on a real terminal
  echo "  live log below (--quiet hides it)"
  echo
  tail -n +1 -F "$log" 2>/dev/null > >(awk -v color="$color" -f tools/play_console.awk) &
  tail_pid=$!
fi

out/build/linux-amd64-relwithdebinfo/crash_mom \
  --game_data_root="$PWD/game" --log_file="$log" --debug_log_fps --debug_audio_trace \
  --photo_dir="$PWD/photos" "${args[@]}" 2>&1 | tee "$out"
status=${PIPESTATUS[0]}
if [ -n "$tail_pid" ]; then
  sleep 0.5  # let the last lines through
  kill "$tail_pid" 2>/dev/null
  wait "$tail_pid" 2>/dev/null
fi

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
# The session's frame rate: the line logged at exit, or after a crash the
# last 5-second line's running total (frame_rate.cpp, section 4).
fps=$(grep -o 'frame_rate: at exit, .*' "$log" 2>/dev/null | tail -1)
[ -z "$fps" ] && fps=$(grep -o 'session (.*' "$log" 2>/dev/null | tail -1)
[ -n "$fps" ] && echo "Frame rate: ${fps#frame_rate: at exit, }"
photos=$(grep -c '\[info\].*Photo (F10): ' "$log" 2>/dev/null || true)  # saved ones only
[ "${photos:-0}" -gt 0 ] && echo "Photos taken: ${photos} (in photos/)"
if [ -n "$trace" ]; then
  traces=$(ls "$trace"/pddi_*.txt 2>/dev/null | wc -l)
  echo "Frame traces: ${traces} (in ${trace}/)"
  # The area scan's findings, in the order met (src/native/scan.h).
  grep -o 'Scan: first time: .*' "$log" 2>/dev/null | sed 's/ -> recording.*//; s/^Scan: first time: /  new gap: /'
fi
