# gdb commands for `tools/play.sh --catch` (see catch_fault.py). The runtime uses
# these signals itself (fibers, the write watch, SIG35...): pass them silently.
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
source tools/gdb/catch_fault.py
run
