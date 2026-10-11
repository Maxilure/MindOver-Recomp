# gdb commands for `tools/play.sh --gdb`: the game under gdb, INTERACTIVE.
# Pause any time with Ctrl-C in the terminal; breakpoints on game functions,
# PowerPC registers, guest memory and the game's call stack come from
# tools/gdb/debug.py (type `ghelp`).
#
# The runtime uses these signals itself (fibers, the write watch, SIG35...):
# pass them silently, or gdb would stop hundreds of times a second.
set pagination off
set confirm off
set print thread-events off
set debuginfod enabled off
handle SIGSEGV nostop noprint pass
handle SIGBUS nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIGPIPE nostop noprint pass
handle SIG32 SIG33 SIG34 SIG35 SIG36 SIG37 SIG38 SIG39 SIG40 nostop noprint pass
source tools/gdb/debug.py
