// =============================================================================
// app_restart.h -- close the game and start it again at once (Options ->
// Language -> "Restart Now": settings the game reads only at start-up)
// =============================================================================
//
// Linux: the process REPLACES itself (execv of /proc/self/exe with the same
// command line): same process id, so the launcher keeps watching it and its
// log (the new run appends a new "Session:" header to the same file).
// Windows: a new process with the same command line, then this one exits
// (the launcher sees the game end; the new one runs on its own).
// Either way --load_save is dropped: the restart lands on the title, not
// straight back into the save the session started with.
// Settings changed in Options must be saved first (the caller does).
// =============================================================================
#pragma once

namespace app_restart {

// Doesn't return when it works; logs and returns when it can't.
void RestartNow();

}  // namespace app_restart
