# =============================================================================
# play_console.awk: the game's log, live and in colour (used by tools/play.sh)
# =============================================================================
#
# play.sh follows the session's log file (tail -F) through this filter while
# the game runs, so the terminal shows what the game is doing as it happens.
# The log file itself is untouched; this only changes how it LOOKS here.
#
# A log line looks like
#   [2026-09-29 19:22:16.387] [info] [core] [t89005] NativeRenderer: ...
# and is shown as
#   19:22:16 NativeRenderer: ...
# (date, milliseconds, category and thread id dropped: they're in the file).
#
# Colours (only when the terminal supports them; play.sh passes color=0
# otherwise):
#   *** SPOTTED ***   bold magenta, framed by blank lines: a material we're
#                     hunting is on screen (src/native/spotter.h): look at
#                     both pictures, press F10
#   Scan: / Photo     green: an automatic recording or a saved photo
#   [error]           red
#   [warning]         yellow
#   frame_rate / NativeRenderer statistics, Audio: lines   dim (they repeat)
#   anything else     normal
# =============================================================================

BEGIN {
  if (color) {
    RED = "\033[31m"; YELLOW = "\033[33m"; GREEN = "\033[32m"
    MAGENTA = "\033[1;35m"; DIM = "\033[2m"; RESET = "\033[0m"
  }
}

{
  line = $0
  level = ""
  # "[date time] [level] [category] [tNNN] message" -> time + message.
  if (match(line, /^\[[0-9-]+ ([0-9:]+)\.[0-9]+\] \[([a-z]+)\] \[[^]]*\] \[[^]]*\] (.*)$/, m)) {
    level = m[2]
    line = m[1] " " m[3]
  }
  if (line ~ /\*\*\* SPOTTED/) {
    printf "\n%s%s%s\n\n", MAGENTA, line, RESET
  } else if (level == "error" || level == "critical") {
    print RED line RESET
  } else if (level == "warning") {
    print YELLOW line RESET
  } else if (line ~ / (Scan: |Photo \(F10\): |Spotter: )/) {
    print GREEN line RESET
  } else if (line ~ / (frame_rate: |NativeRenderer: (last |texture checks|game frame )|Audio: )/) {
    print DIM line RESET
  } else {
    print line
  }
  fflush()
}
